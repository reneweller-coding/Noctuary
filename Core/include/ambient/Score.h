/**
 * @file Score.h
 * @brief A score: what should happen, written down.
 *
 * The set timeline (Timeline.h) records what you did and plays it back. The map route walks a
 * path between presets. Neither lets you write a piece: "start dry, bring the Cosmos in over four
 * minutes from the twelfth, open the far reverb at the twentieth, and let it fall away for the
 * last ten". That is what this is -- a plain text file of timed ramps, which is the smallest
 * thing that makes a composed forty-minute piece repeatable.
 *
 *   # a comment
 *   0:00    brain_density 3
 *   2:00    cosmos_send 0.55 over 4:00
 *   12:00   far_decay 70 over 6:00
 *   30:00   master_gain -18 over 8:00
 *
 * A line is \<time\> \<parameter key\> \<value\> [over \<duration\>]. Times are m:ss or h:mm:ss or plain
 * seconds. Without "over" the value is set at that moment; with it, the parameter travels there
 * from wherever it was when the ramp started -- in the parameter's own skewed domain, so a
 * logarithmic knob moves the way the hand would move it.
 */
#pragma once
#include "Params.h"
#include "Dsp.h"   // clampv
#include <cstddef>

namespace ambient {

constexpr int kMaxScoreEvents = 512;   ///< the most ramps a score may hold; fixed so a Score has no allocation in it

/** @brief One line of a score: a parameter travelling to a value, starting at a time, over a duration. */
struct ScoreEvent {
    double  at = 0.0;        ///< seconds from the start of the piece
    double  over = 0.0;      ///< seconds the ramp takes; 0 = immediately
    ParamId target = ParamId::MasterGain;   ///< the parameter that moves
    float   value = 0.0f;    ///< where it ends up, in the parameter's real range (clamped by parse)
};

/**
 * @brief The score: up to kMaxScoreEvents ramps and the clock that plays them.
 *
 * No allocation inside the object, so step() may run on the audio thread; load() and save() go
 * through files and belong to the message thread.
 */
class Score {
public:
    /**
     * @brief Parses the text form (see the file comment): replaces the score; false on the first bad line.
     *
     * A choice may be written by its name and a switch as on/off as well as by number; a value
     * that is not a number becomes the parameter's default, and every value is clamped to the
     * parameter's range. Comments ('#' to the end of the line) and blank lines are skipped. The
     * clock is rewound afterwards.
     *
     * @param text  the whole file, NUL-terminated; nullptr is refused
     * @return      true when every line parsed
     */
    bool parse(const char* text);
    /**
     * @brief The text form again.
     *
     * One line per event, choices by name and switches as on/off, times as m:ss or h:mm:ss.
     *
     * @param buf  receives the NUL-terminated text (always terminated when @p cap > 0, cut short if it does not fit)
     * @param cap  size of @p buf
     * @return     the length the whole text has (which may exceed @p cap), or -1 on a formatting error
     */
    int  write(char* buf, size_t cap) const;
    /**
     * @brief Reads a file and parses it (see parse()). Message thread: allocates.
     * @param path  the file to read
     * @return      false when the file is missing or a line is malformed
     */
    bool load(const char* path);
    /**
     * @brief Writes the text form to a file. Message thread: allocates.
     * @param path  the file to create
     * @return      false when the file could not be opened or written in full
     */
    bool save(const char* path) const;

    /** @return how many events the score holds */
    int  count() const { return count_; }
    /**
     * @brief One event, in the order of the text.
     * @param i  0 .. count()-1, unchecked
     * @return   the event
     */
    const ScoreEvent& event(int i) const { return events_[i]; }
    /** @return when the last ramp finishes, in seconds (the latest at + over) */
    double length() const;
    /** @brief Forgets every event; the clock is left where it was. */
    void clear() { count_ = 0; }
    /**
     * @brief Appends an event, not yet started.
     * @param e  the event
     * @return   false when the score is full (kMaxScoreEvents)
     */
    bool add(const ScoreEvent& e);

    /**
     * @brief Playing. `now` is the seconds since the piece started; call it once per block with the
     *        interval that has passed.
     *
     * `valueOf(ParamId) -> float` reads the current value (a ramp needs
     * to know where it starts), `set(ParamId, float)` receives every parameter that moved.
     *
     * rewind() puts the clock back to zero and marks every ramp as not yet started, so a piece
     * can be played again from the top.
     */
    void  rewind();
    /** @return seconds since the piece started, as step() has counted them */
    double position() const { return t_; }
    /**
     * @brief Advances the clock by @p dt and drives every ramp that is due.
     *
     * A ramp reads its starting value through @p valueOf the first time it is reached and then
     * writes rampValue() of its progress through @p set on every call until it has finished; an
     * event with no duration is set once its time has come and stays set. Nothing happens for a
     * non-positive @p dt or an empty score.
     *
     * @tparam ValueFn  callable as float(ParamId)
     * @tparam SetFn    callable as void(ParamId, float)
     * @param dt        seconds since the last call
     * @param valueOf   where a parameter is now, read when a ramp starts
     * @param set       receives every parameter that moved
     */
    template <class ValueFn, class SetFn>
    void step(double dt, ValueFn&& valueOf, SetFn&& set)
    {
        if (count_ == 0 || dt <= 0.0) return;
        const double t0 = t_, t1 = t_ + dt;
        t_ = t1;
        for (int i = 0; i < count_; ++i) {
            const ScoreEvent& e = events_[i];
            if (t1 < e.at) continue;
            if (t0 >= e.at + e.over && started_[i]) continue;   // finished, and it did run
            if (!started_[i]) { from_[i] = valueOf(e.target); started_[i] = true; }
            if (e.over <= 0.0) { set(e.target, e.value); continue; }
            const double x = clampv((t1 - e.at) / e.over, 0.0, 1.0);
            set(e.target, rampValue(e.target, from_[i], e.value, static_cast<float>(x)));
        }
    }

    /**
     * @brief Where a ramp is at fraction x, in the parameter's own perceptual domain (the same one the
     *        morph and the map blend use, so a score moves a knob the way a hand would).
     *
     * A float travels in the skew domain (the normalised value raised to the parameter's skew);
     * a choice, a switch or an integer does not travel at all and changes when the ramp is over.
     *
     * @param id    the parameter, for its range and skew
     * @param from  the value the ramp started at
     * @param to    the value it ends at
     * @param x     progress 0 .. 1 (clamped)
     * @return      the value to set now, in the parameter's real range
     */
    static float rampValue(ParamId id, float from, float to, float x);

private:
    ScoreEvent events_[kMaxScoreEvents];   ///< the events, in the order added
    int    count_ = 0;                     ///< how many of events_ are in use
    double t_ = 0.0;                       ///< the clock: seconds since the piece started
    bool   started_[kMaxScoreEvents] = {}; ///< per event: whether its starting value has been read
    float  from_[kMaxScoreEvents] = {};    ///< per event: the value it started from, valid once started_
};

/**
 * @brief "3:20" / "1:02:30" / "45" -> seconds; negative if the text is not a time.
 *
 * Up to three ':'-separated numbers (h:mm:ss, m:ss or plain seconds), fractions allowed; trailing
 * blanks are ignored, anything else after the number refuses the text.
 *
 * @param text  the time as written; nullptr or empty is not a time
 * @return      seconds, or -1 when @p text is not a time
 */
double parseScoreTime(const char* text);
/**
 * @brief Writes seconds as m:ss, or h:mm:ss once there is an hour, whole seconds only.
 * @param seconds  the time; negative counts as 0
 * @param buf      receives the NUL-terminated text
 * @param cap      size of @p buf
 * @return         what snprintf returns: the length written, or the length needed if @p cap was too small
 */
int    writeScoreTime(double seconds, char* buf, size_t cap);

} // namespace ambient
