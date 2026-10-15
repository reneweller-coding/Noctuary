/**
 * @file Gesture.h
 * @brief Gesture layer: a few continuous inputs (hands, head, or anything
 *        a controller sends) mapped onto parameters with range, smoothing, jitter
 *        dead-zone and a clutch.
 *
 * Framework-free: the same code runs behind an OpenXR
 * hand tracker on the Quest and behind the OSC receiver on the desktop.
 *
 * Inputs are normalised to 0..1 (tilts and head angles are mapped from -1..1).
 * The layer never writes parameters itself; update() hands (ParamId, value) to a
 * sink so each host can route it into its own parameter system.
 */
#pragma once
#include "Params.h"
#include <atomic>
#include <cstdint>
#include <cmath>

namespace ambient {

/**
 * @brief The inputs a host can feed, each held as a float in 0..1 (the metres and degrees they
 *        are mapped from are given per entry).
 *
 * The first twelve are what setHand() and setHead() derive from raw poses; the eight Custom
 * inputs are free for whatever a host has -- the Quest and the OSC receiver put Macro A..H on
 * them (GestureLayer::setDefaultMappings()). Count doubles as "no input": a mapping whose clutch
 * is Count is always engaged, and gestureInputFromName() reads "none" and "-" as Count.
 */
enum class GestureInputE : int {
    HAND_DISTANCE,     ///< distance between the hands, 0.1 .. 0.8 m
    LEFT_HEIGHT, RIGHT_HEIGHT,     ///< hand height, 0.9 .. 1.7 m
    LEFT_FORWARD, RIGHT_FORWARD,   ///< hand reach in front of the head, 0.2 .. 0.7 m
    LEFT_TILT, RIGHT_TILT,         ///< palm roll, -1 .. 1
    LEFT_PINCH, RIGHT_PINCH,       ///< pinch strength 0 .. 1
    HEAD_YAW, HEAD_PITCH, HEAD_ROLL,   ///< -1 .. 1 (= -90 .. 90 degrees)
    CUSTOM0, CUSTOM1, CUSTOM2, CUSTOM3, CUSTOM4, CUSTOM5, CUSTOM6, CUSTOM7,   ///< eight free inputs, 0 .. 1, for whatever the host sends (Macro A..H by default)
    COUNT   ///< how many inputs; as a mapping's clutch it means "always engaged"
};
constexpr int NumGestureInputs = static_cast<int>(GestureInputE::COUNT);   ///< size of the input table
/**
 * @brief The name of an input as the mapping text spells it ("HandDistance", "LeftPinch", ...).
 * @param in  the input; Count and anything out of range read as "none"
 * @return    a static string, never null
 */
const char* gestureInputName(GestureInputE);
/**
 * @brief The inverse of gestureInputName(): an input from its name.
 * @param name  the spelling, case-sensitive; "none" and "-" mean Count (no input, always engaged)
 * @param out   receives the input on success and is left alone otherwise
 * @return      false for a null pointer or a name that is not in the table
 */
bool gestureInputFromName(const char* name, GestureInputE& out);

/**
 * @brief One row of the vocabulary: an input, the parameter it drives and how.
 *
 * The parameter's value is min + (max - min) * input (or 1 - input when inverted), glided with a
 * one-pole of smoothSeconds, and only given a new target when the input has moved by at least
 * the deadzone since the last value that was taken -- and only while the clutch input, if there
 * is one, stands above 0.5. The text form is one line per mapping (GestureLayer::parseMappings()).
 */
struct GestureMapping {
    GestureInputE m_input = GestureInputE::HAND_DISTANCE;   ///< the input that drives the parameter
    ParamIdE      m_param = ParamIdE::MORPH_POS;            ///< the parameter driven, by id (the text form uses its key)
    /** @var float m_min
     *  @brief parameter value at input 0 (at input 1 when inverted) */
    float        m_min = 0.0f, m_max = 1.0f;      ///< parameter value at input 0 and 1
    float        m_smoothSeconds = 0.3f;        ///< one-pole glide of the parameter
    float        m_deadzone = 0.01f;            ///< ignore input changes smaller than this
    GestureInputE m_clutch = GestureInputE::COUNT; ///< Count = always engaged; else engaged while clutch > 0.5
    bool         m_invert = false;              ///< read the input as 1 - x, so a rising hand lowers the parameter
};

/**
 * @brief The layer itself: the input table, the mappings, the calibration of the hand ranges, the
 *        rest zone, and the per-block update that hands parameter values to the host.
 *
 * Threads: the inputs are atomics written from wherever the tracker or the OSC receiver runs and
 * read on the audio thread in update(); the mappings are written on the message thread and read
 * on the audio thread through a count that is published only after the row it counts is written.
 * Nothing here allocates after construction; the tables are fixed arrays.
 */
class GestureLayer {
public:
    static constexpr int MaxMappings = 32;   ///< rows the mapping table holds; addMapping() refuses the thirty-third

    /** @brief Every input at 0 and the default vocabulary (setDefaultMappings()) in place. */
    GestureLayer();

    /**
     * @name Inputs, any thread.
     * @{ */
    /**
     * @brief Sets one input directly, already normalised (an OSC controller, a knob, a macro).
     * @param in          the input; anything out of range is ignored
     * @param normalised  its value, clamped to 0 .. 1
     */
    void setInput(GestureInputE in, float normalised);
    /**
     * @brief The current value of an input.
     * @param in  the input; a real one, not Count
     * @return    its last value, 0 .. 1
     */
    float input(GestureInputE in) const { return m_inputs[static_cast<int>(in)].load(std::memory_order_relaxed); }
    /** @} */
    /**
     * @brief Raw poses in metres (OpenXR convention: x right, y up, z back), relative to the stage.
     *
     * Derives distance, heights, reach; tilt and pinch are passed through. Height and reach are
     * normalised against the calibrated ranges (heightLow() .. heightHigh(), the reach and the
     * distance likewise), tilt is folded from -1..1 into 0..1. While a calibration is running the
     * raw extremes are collected here. It is also the sign that real hands exist: the rest zone
     * only ever engages after this has been called once.
     *
     * @param hand   0 left, 1 right (only the lowest bit counts)
     * @param x      metres to the right of the stage origin
     * @param y      metres up: the height
     * @param z      metres back, so -z is the reach in front of the head
     * @param pinch  pinch strength 0 .. 1
     * @param tilt   palm roll -1 .. 1
     */
    void setHand(int hand, float x, float y, float z, float pinch, float tilt);
    /**
     * @brief The head's orientation, from a headset or a head tracker.
     * @param yawDeg    -90 .. 90 degrees, mapped onto HeadYaw 0 .. 1 (clamped)
     * @param pitchDeg  the same for HeadPitch
     * @param rollDeg   the same for HeadRoll
     */
    void setHead(float yawDeg, float pitchDeg, float rollDeg);
    /**
     * @brief Sets the six calibrated bounds directly (from a saved calibration, or a host's own numbers).
     * @param heightLow   metres of hand height that read as 0
     * @param heightHigh  metres that read as 1
     * @param reachNear   metres of reach (-z) that read as 0
     * @param reachFar    metres that read as 1
     * @param distNear    hand distance in metres that reads as 0
     * @param distFar     hand distance that reads as 1
     */
    void setCalibration(float heightLow, float heightHigh, float reachNear, float reachFar, float distNear, float distFar);

    /**
     * @brief Calibration gesture: for `seconds` the layer watches the raw hand data (hands
     *        together and apart, low and high, near and far) and then sets the ranges from
     *        the extremes seen, with a small margin.
     *
     * Progress 0..1 for a display. While it runs update() moves nothing; the extremes are
     * gathered in setHand() and finishCalibration() takes them over at the end -- only where a
     * range wide enough to be believed was explored (a quarter metre of height, twenty
     * centimetres of distance, fifteen of reach), with five per cent of it cut from each end.
     *
     * @param seconds  how long to watch, at least half a second
     */
    void  startCalibration(float seconds);
    /** @brief Whether a calibration is running. @return true until its time is up */
    bool  calibrating() const { return m_calibRemaining > 0.0f; }
    /**
     * @brief Whether real hand data (setHand: the Quest app, or its bridge over OSC) arrived in the last @p seconds
     *        (01.10.2026: the panel shows the headset's controls only while a headset sends, or when asked to). Any thread.
     */
    bool handsRecently(double seconds) const;
    /** @brief How far the running calibration has come. @return 0..1; 1 when none is running */
    float calibrationProgress() const { return m_calibTotal > 0.0f ? 1.0f - m_calibRemaining / m_calibTotal : 1.0f; }
    /**
     * @brief "hLow hHigh rNear rFar dNear dFar" for persistence.
     *
     * The six calibrated bounds in metres, space-separated, each in printf's shortest form.
     * @param out       the buffer
     * @param capacity  its size in bytes
     * @return          what snprintf returns: the length written, or the length that would have
     *                  been needed when it did not fit
     */
    int   writeCalibration(char* out, int capacity) const;
    /**
     * @brief Reads the six numbers writeCalibration() wrote and sets the calibration from them.
     * @param text  "hLow hHigh rNear rFar dNear dFar"
     * @return      false for a null pointer, fewer than six numbers, or a range whose high end is
     *              not above its low end; nothing is changed then
     */
    bool  parseCalibration(const char* text);
    /**
     * @fn float GestureLayer::heightLow() const
     * @brief The calibrated hand height that reads as 0.
     * @return metres
     */
    /**
     * @fn float GestureLayer::heightHigh() const
     * @brief The calibrated hand height that reads as 1.
     * @return metres
     */
    float heightLow() const { return m_hLow; }  float heightHigh() const { return m_hHigh; }
    /**
     * @fn float GestureLayer::distNear() const
     * @brief The hand distance that reads as 0.
     * @return metres
     */
    /**
     * @fn float GestureLayer::distFar() const
     * @brief The hand distance that reads as 1.
     * @return metres
     */
    float distNear() const { return m_dNear; }  float distFar() const { return m_dFar; }

    /**
     * @brief While suspended (a menu is open), clutched mappings hold their values.
     *
     * Unclutched mappings (the macros, the head) keep moving. Any thread.
     * @param s  true to suspend
     */
    void setSuspended(bool s) { m_suspended.store(s, std::memory_order_relaxed); }
    /** @brief Whether the clutched mappings are held. @return the flag setSuspended() set */
    bool suspended() const { return m_suspended.load(std::memory_order_relaxed); }
    /**
     * @brief Rest zone: share of the calibrated height below which both hands count as resting (0 = off).
     * @param share  0 .. 1 of the height range; 0 switches the rest zone off
     */
    void  setRestZone(float share) { m_restZone = share; }
    /** @brief The rest-zone share. @return what setRestZone() set; 0.08 by default */
    float restZone() const { return m_restZone; }
    /** @brief Whether both hands hung below the rest zone at the last update(); nothing is written while they do. @return true while resting */
    bool  resting() const { return m_resting; }

    /**
     * @name Mappings, message thread (the audio thread reads them; keep changes rare).
     * @{ */
    /** @brief How many rows are in force. @return 0 .. MaxMappings */
    int  numMappings() const { return m_numMappings.load(std::memory_order_acquire); }
    /**
     * @brief One row.
     * @param i  0 .. numMappings() - 1
     * @return   the row
     */
    const GestureMapping& mapping(int i) const { return m_maps[i]; }
    /** @brief Zero FIRST, so the audio thread stops reading before the table underneath it is rewritten. */
    void clearMappings() { m_numMappings.store(0, std::memory_order_release); }
    /**
     * @brief Appends a row with a fresh running state and publishes the new count once it is written.
     * @param m  the row
     * @return   false when the table is full
     */
    bool addMapping(const GestureMapping& m);
    /**
     * @brief The built-in vocabulary: seven hand mappings under the right pinch as clutch, the head's
     *        yaw on Width, and Macro A..H on Custom0..7, each moving several parameters at once.
     *
     * Replaces whatever was there. The macros are what the Quest's and the OSC desktop's knobs
     * reach by default -- "Space", "Alien", "Motion", "Bloom", "Density", "Distance", "Evolution"
     * and "Air" (Gesture.cpp names them row by row).
     */
    void setDefaultMappings();
    /**
     * @brief Text form, one mapping per line:
     * @code{.unparsed}
     *   input param min max smooth deadzone clutch invert
     * @endcode
     * e.g. "HandDistance morph 0 1 0.5 0.01 RightPinch 0"
     *
     * The first four fields are required, the rest take the defaults of GestureMapping; a line
     * may end in a comment introduced by the hash sign, and empty lines are skipped. The whole
     * text is parsed before the table is touched, so a bad line leaves the mappings as they were.
     *
     * @param text  the lines
     * @return      false for a null pointer, an unknown input or parameter key, a line with fewer
     *              than four fields, or more rows than MaxMappings
     */
    bool parseMappings(const char* text);
    /**
     * @brief The rows as text parseMappings() reads: one line each, "none" for no clutch, 1 or 0 for
     *        invert, the parameter by its key.
     * @param out       the buffer
     * @param capacity  its size in bytes; writing stops once it is full
     * @return          characters written (the sum of what snprintf returned)
     */
    int  writeMappings(char* out, int capacity) const;
    /** @} */

    /**
     * @brief Audio thread, once per block.
     *
     * Calls sink(ParamId, float) for every mapping
     * whose target changed. Returns the number of parameters written.
     *
     * In order: a running calibration counts down and nothing moves; the rest zone (both hands
     * low, and real hands have been seen) writes nothing; then every row reads its input, takes it
     * as a new target when the clutch is engaged and the change clears the deadzone, glides its
     * value towards that target with the row's one-pole, and calls the sink when the value moved
     * (or has never been sent). A row's first sight of its input primes the value at the input's
     * position, so the glide starts from where the hand is rather than from zero.
     *
     * @tparam Sink  anything callable as sink(ParamId, float)
     * @param dt     seconds in this block
     * @param sink   receives (parameter, new value) for every row that moved
     * @return       how many rows called the sink
     */
    template <class SinkT>
    int update(double dt, SinkT&& sink)
    {
        if (m_calibRemaining > 0.0f) {
            m_calibRemaining -= static_cast<float>(dt);
            if (m_calibRemaining <= 0.0f) { m_calibRemaining = 0.0f; finishCalibration(); }
            return 0;   // nothing moves while calibrating
        }
        // Rest zone: both hands hanging low (below m_restZone of the calibrated height) means
        // "I am not playing" -- nothing is written, so the arms can drop without touching the sound.
        m_resting = m_restZone > 0.0f && m_handsSeen && input(GestureInputE::LEFT_HEIGHT) < m_restZone && input(GestureInputE::RIGHT_HEIGHT) < m_restZone;
        if (m_resting) return 0;
        const bool susp = suspended();
        int written = 0;
        const int count = m_numMappings.load(std::memory_order_acquire);
        for (int i = 0; i < count; ++i) {
            const GestureMapping& m = m_maps[i];
            State& s = m_state[i];
            float x = input(m.m_input);
            const bool engaged = !(susp && m.m_clutch != GestureInputE::COUNT) && ((m.m_clutch == GestureInputE::COUNT) || input(m.m_clutch) > 0.5f);
            if (!s.m_seen) {   // a mapping only acts once its input has moved; the glide starts from the rest position
                s.m_seen = true; s.m_lastInput = x;
                s.m_value = m.m_min + (m.m_max - m.m_min) * (m.m_invert ? 1.0f - x : x); s.m_primed = true;
            }
            if (engaged && std::fabs(x - s.m_lastInput) >= m.m_deadzone) { s.m_lastInput = x; s.m_hasTarget = true; }
            if (!s.m_hasTarget) continue;
            float u = s.m_lastInput;
            if (m.m_invert) u = 1.0f - u;
            const float target = m.m_min + (m.m_max - m.m_min) * u;
            const float coef = m.m_smoothSeconds <= 1e-4 ? 1.0f : 1.0f - static_cast<float>(std::exp(-dt / m.m_smoothSeconds));
            const float next = s.m_value + (target - s.m_value) * coef;
            if (std::fabs(next - s.m_value) > 1e-6f || !s.m_sent) {
                s.m_value = next; s.m_sent = true;
                sink(m.m_param, next);
                ++written;
            }
        }
        return written;
    }

    /** @brief How many times setInput() has been called, for a display that shows the controller is alive. @return a running count */
    uint64_t inputUpdates() const { return m_updates.load(std::memory_order_relaxed); }

private:
    /** @var float State::m_lastInput
     *  @brief the input value last taken as a target; the deadzone is measured from it */
    /** @var float State::m_value
     *  @brief the parameter value as glided so far, what the sink last received */
    /** @var bool State::m_seen
     *  @brief the row has read its input once and primed value from it */
    /** @var bool State::m_hasTarget
     *  @brief an input change has been taken since the row was made; nothing is sent before */
    /** @var bool State::m_primed
     *  @brief set with seen when value was placed at the input's position; update() does not read it back */
    /** @var bool State::m_sent
     *  @brief the sink has received this row at least once (the first send happens even without a move) */
    /** @brief Per-row running state of update(): what was last taken, where the glide stands, and the flags of its life. */
    struct State { float m_lastInput = 0.0f, m_value = 0.0f; bool m_seen = false, m_hasTarget = false, m_primed = false, m_sent = false; };
    std::atomic<float> m_inputs[NumGestureInputs];   ///< the input table, 0 .. 1 each, written from any thread
    std::atomic<uint64_t> m_updates{ 0 };             ///< setInput() calls so far
    GestureMapping m_maps[MaxMappings];              ///< the rows; only the first m_numMappings are in force
    State m_state[MaxMappings];                      ///< one running state per row
    /**
     * @brief Written by the message thread when the mapping text is edited, read by the audio thread
     *        every block.
     *
     * Published after the entry it counts has been written, so a half-written
     * mapping is never acted on.
     */
    std::atomic<int> m_numMappings { 0 };
    /** @brief Takes the extremes a calibration gathered over into the six bounds, where a range wide enough was explored (see startCalibration()). */
    void finishCalibration();
    /** @var float m_handX[2]
     *  @brief the last raw x of each hand in metres (0 left, 1 right) */
    /** @var float m_handY[2]
     *  @brief the last raw y (height) of each hand */
    float m_handX[2] = {}, m_handY[2] = {}, m_handZ[2] = {};   ///< m_handZ: the last raw z of each hand; the hand distance is taken across all three
    /** @var float m_hLow
     *  @brief hand height that reads as 0, metres (the calibration; see setCalibration()) */
    /** @var float m_hHigh
     *  @brief hand height that reads as 1, metres */
    /** @var float m_rNear
     *  @brief reach (-z) that reads as 0, metres */
    /** @var float m_rFar
     *  @brief reach that reads as 1, metres */
    /** @var float m_dNear
     *  @brief hand distance that reads as 0, metres */
    float m_hLow = 0.9f, m_hHigh = 1.7f, m_rNear = 0.2f, m_rFar = 0.7f, m_dNear = 0.1f, m_dFar = 0.8f;   ///< m_dFar: hand distance that reads as 1, metres
    std::atomic<bool> m_suspended{ false };   ///< a menu is open: clutched rows hold
    float m_restZone = 0.08f;                 ///< share of the height range under which both hands count as resting; 0 = off
    bool  m_resting = false;                  ///< both hands were in the rest zone at the last update()
    bool  m_handsSeen = false;   ///< set once real hand data arrived (setHand); knobs and OSC gestures alone never "rest"
    std::atomic<int64_t> m_handStampMs{ 0 };   ///< when setHand was last called (steady clock, ms; 0 never)
    /** @var float m_calibRemaining
     *  @brief seconds of the running calibration left; 0 when none runs */
    float m_calibRemaining = 0.0f, m_calibTotal = 0.0f;   ///< m_calibTotal: its full length, for the progress
    /** @var float m_calMinY
     *  @brief lowest hand height seen during the calibration (the six extremes are reset by startCalibration()) */
    /** @var float m_calMaxY
     *  @brief highest hand height seen */
    /** @var float m_calMinD
     *  @brief smallest hand distance seen */
    /** @var float m_calMaxD
     *  @brief largest hand distance seen */
    /** @var float m_calMinR
     *  @brief shortest reach seen */
    float m_calMinY = 1e9f, m_calMaxY = -1e9f, m_calMinD = 1e9f, m_calMaxD = -1e9f, m_calMinR = 1e9f, m_calMaxR = -1e9f;   ///< m_calMaxR: the longest reach seen
};

} // namespace ambient
