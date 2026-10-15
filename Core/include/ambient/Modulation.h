/**
 * @file Modulation.h
 * @brief The modulation section: free LFOs, multi-segment envelopes and a matrix that
 *        connects any source to any number of targets.
 *
 * Until now every modulator in this instrument was soldered to one destination and carried its
 * own depth and rate parameters: the pitch drifter to pitch, the filter drifter to the cutoff,
 * the Kuramoto ring to brightness, distance, pan and the z-plane point. That is why adding a
 * source always meant adding another pair of knobs. This is the general form.
 *
 * What is a parameter and what is data
 * ------------------------------------
 * The LFOs and the envelope timings are parameters (Params.h), so a host can automate them and a
 * preset carries them like everything else. The *shapes* -- the envelope breakpoints and the
 * matrix rows -- are data, the way a Scala scale and the gesture mappings already are: a compact
 * text form that travels in the preset string and in the plugin state. Thirty-two matrix rows as
 * four parameters each would put a hundred and twenty entries into the automation list for very
 * little gain.
 *
 * Rates
 * -----
 * A drone instrument needs modulators far below the usual LFO range: the slowest here is one
 * cycle in twenty minutes. Everything runs at control rate (one step per 64 samples), which is
 * 750 Hz at 48 kHz -- ample for a modulator whose fastest setting is 20 Hz.
 *
 * The engine (Engine.h, EngineControl.cpp) owns eight Lfo, six ModEnv with their ModEnvSpec and
 * one ModMatrix; once per control block it steps the modulators, fills the source table and lets
 * the matrix sum into a per-parameter offset that readParams() adds to every parameter it reads.
 * Nothing here allocates: every table is a fixed array.
 * @author Rene Weller
 */
#pragma once
#include "Params.h"
#include "Dsp.h"
#include <cstdint>

namespace ambient {

struct Wavetable;   // Sources.h -- a table frame can be an LFO shape

constexpr int NumLfos      = 8;    ///< free LFOs; each owns seven parameters from Lfo1Shape on (Params.h)
constexpr int NumModEnvs   = 6;    ///< modulation envelopes; each owns four parameters from Env1Mode on
constexpr int MaxEnvPoints = 16;   ///< breakpoints a ModEnv holds at most
constexpr int MaxModRoutes = 32;   ///< rows a ModMatrix holds at most

// ---------------------------------------------------------------- LFO

/**
 * @brief The waveforms an LFO can run, read at a phase in 0..1 and returning -1..1 (Lfo::shapeAt()).
 *
 * Sine; Triangle (0 at phase 0, 1 at a quarter, -1 at three quarters); RampUp from -1 to 1;
 * RampDown from 1 to -1; Square with its edges ramped over 7 percent of the cycle, because a
 * modulator here may not step; Random, a smoothstep from one random value to the next, one pair
 * a cycle; StepRandom, four random holds a cycle, each reached over a short ramp; Table, a frame
 * of the user wavetable resynthesised from its spectrum. Count is the size of the choice.
 */
enum class LfoShapeE : int {
    SINE, TRIANGLE, RAMP_UP, RAMP_DOWN, SQUARE, RANDOM, STEP_RANDOM, TABLE, COUNT   ///< in the order LfoShapeNames spells them, Count last
};
constexpr int NumLfoShapes = static_cast<int>(LfoShapeE::COUNT);   ///< shapes in the choice
extern const char* const LfoShapeNames[NumLfoShapes];   ///< "Sine", "Triangle", "Ramp Up", "Ramp Down", "Square", "Random", "Steps", "Table" (Modulation.cpp)

/**
 * @brief How an LFO's phase relates to the voices.
 *
 * Global: one phase for the whole instrument, so every voice breathes together.
 * Voice: each voice runs its own copy from wherever the shared phase was when it started.
 * Retrigger: each voice restarts its copy from Phase.
 *
 * As built, the engine steps every LFO once per control block for the whole instrument
 * (Engine::stepModulation): Retrigger puts the phase back to Phase when a note arrives into
 * silence, and Voice cannot be honoured there and behaves as Global -- the help says so.
 */
enum class LfoModeE : int { GLOBAL, VOICE, RETRIGGER, COUNT };   ///< Global, Voice, Retrigger as above; Count the size of the choice
constexpr int NumLfoModes = static_cast<int>(LfoModeE::COUNT);   ///< modes in the choice
extern const char* const LfoModeNames[NumLfoModes];   ///< "Global", "Per Voice", "Retrigger" (Modulation.cpp)

/**
 * @brief One LFO's settings for a block: shape, rate, start phase, depth, mode and table frame.
 *
 * Filled by the engine from the LFO's seven parameters once per control block, with the matrix's
 * modulation of rate, phase and depth already applied, and handed to Lfo::step().
 */
struct LfoSpec {
    LfoShapeE m_shape = LfoShapeE::SINE;   ///< the waveform
    float    m_rateHz = 0.05f;    ///< cycles per second, from one in twenty minutes up to 20 Hz (or a synced division's rate)
    float    m_phase = 0.0f;      ///< 0..1, where the shape starts
    float    m_depth = 1.0f;      ///< scales the output; the matrix scales again per route
    LfoModeE  m_mode = LfoModeE::GLOBAL;   ///< how the phase relates to the voices (LfoMode)
    int      m_table = 0;         ///< frame of the user wavetable when shape == Table
};

/**
 * @brief A shape read at a phase in 0..1, returning -1..1.
 *
 * `Table` reads a frame of a wavetable, which
 * is what makes any of the 608 generated tables -- and any curve drawn into one -- a usable LFO
 * shape without a second mechanism for "drawable" modulators.
 *
 * One instance is one running LFO: a phase in double, the last value, and the random material
 * the Random and StepRandom shapes draw a cycle at a time from the LFO's own generator, so a
 * seed makes the run repeatable.
 */
class Lfo {
public:
    /**
     * @brief Seeds the generator, puts the phase where the shape should start, and draws the first
     *        random pair and the first holds.
     * @param[in] seed     the generator's seed
     * @param[in] phase01  the starting phase, wrapped into 0..1
     */
    void reset(uint64_t seed, float phase01);
    /**
     * @brief Advances by dt seconds and returns the new value in -1..1.
     *
     * `table` may be null. At the wrap of a cycle the Random shape's pair moves on by one and the
     * StepRandom holds are redrawn with the last hold carried over, so neither steps at the wrap;
     * a phase that rounds to exactly one is pulled just under it, because wrapping that gave a
     * jump one control block before the cycle ended. Control rate, once per block.
     * @param[in] dt     seconds since the last step
     * @param[in] spec   the settings for this block; spec.phase is added to the running phase and spec.depth scales the
     *                   result
     * @param[in] table  the user wavetable for the Table shape, or null (a sine stands in)
     * @return the value, -1..1 times spec.depth
     */
    float step(float dt, const LfoSpec& spec, const Wavetable* table);
    /** @brief The last value step() returned. @return -1..1 times the depth */
    float value() const { return m_value; }
    /** @brief The running phase. @return 0..1 */
    float phase() const { return static_cast<float>(m_phase); }
    /**
     * @brief Puts the phase somewhere: a synced LFO is set onto the clock's position every block, a
     *        Retrigger one back to its start at a phrase.
     * @param[in] p  the phase; wrapped into 0..1
     */
    void  setPhase(float p) { m_phase = p - std::floor(p); }
    /**
     * @brief The shape at a phase, without stepping: the editor draws a whole cycle with this.
     * @param[in] spec     which shape (and which table frame)
     * @param[in] phase01  the phase, wrapped into 0..1
     * @param[in] table    the user wavetable for the Table shape, or null
     * @param[in] live     a running LFO whose random pair and holds the Random and StepRandom shapes read, or null for
     *                     a fixed stand-in pattern
     * @return -1..1, before any depth
     */
    static float shapeAt(const LfoSpec& spec, float phase01, const Wavetable* table,
                         const Lfo* live = nullptr);

private:
    /** @brief Grants a test hook access to m_steps; declared here only, nothing in the tree defines it at present. */
    friend float lfoStepsAccess(const Lfo&);
    double  m_phase = 0.0;                 ///< the running phase, 0..1, in double so a twenty-minute cycle does not drift
    float   m_value = 0.0f;                ///< the last value step() returned
    /** @var float m_prev
     *  @brief Random: the value this cycle started from */
    float   m_prev = 0.0f, m_next = 0.0f;   ///< Random: the two ends of this cycle
    float   m_steps[5] = {};               ///< Steps: four holds a cycle, the fifth carries over the wrap
    Rng     m_rng;                         ///< the LFO's own stream for the random shapes, seeded by reset()
};

// ---------------------------------------------------------------- envelope

/** @brief One breakpoint of a ModEnv: when, what value, and how the segment that leaves it curves. */
struct EnvPoint {
    float m_time = 0.0f;    ///< seconds from the start of the envelope, non-decreasing
    float m_value = 0.0f;   ///< -1..1
    float m_curve = 0.0f;   ///< -1 (fast then slow) .. 0 (linear) .. 1 (slow then fast)
};

/**
 * @name Shapes to start an envelope from
 * Shapes to start an envelope from, in the text form below. They live here rather than in the
 * editor so that the self test can check that every one of them parses -- a shape string with a
 * typo in it does not fail loudly, it simply does nothing when the menu item is picked.
 * @{ */
constexpr int NumEnvShapePresets = 10;   ///< entries in the two tables
extern const char* const EnvShapePresetNames[NumEnvShapePresets];   ///< the menu's captions, "ADSR" first because it is what anybody looks for first
extern const char* const EnvShapePresetTexts[NumEnvShapePresets];   ///< the shapes in ModEnv::parse() form, written over about four time units (Modulation.cpp)
/** @} */

/**
 * @brief How an envelope runs: OneShot plays the shape once and holds its last value; Loop
 *        repeats it (between its loop points when it has them); SustainLoop holds at the sustain
 *        point -- or loops -- while the note is down and plays the rest from there on release.
 */
enum class EnvModeE : int { ONE_SHOT, LOOP, SUSTAIN_LOOP, COUNT };   ///< OneShot, Loop, SustainLoop as above; Count the size of the choice
constexpr int NumEnvModes = static_cast<int>(EnvModeE::COUNT);   ///< modes in the choice
extern const char* const EnvModeNames[NumEnvModes];   ///< "One Shot", "Loop", "Sustain Loop" (Modulation.cpp)

/**
 * @brief A multi-segment envelope: up to sixteen breakpoints, a curve per segment, an optional sustain
 *        point the envelope holds at while the note is down, and an optional loop between two points so
 *        it can run as a slow shape generator rather than a one-shot.
 *
 * The shape is data (see the file comment): it travels as text in the preset's envelope field and
 * in the plugin state, and the engine keeps a pending copy the editor writes and a live copy the
 * audio thread reads. Times are the shape's own seconds; the Time parameter (ModEnvSpec)
 * stretches them from outside. Fewer than two points is a constant.
 */
class ModEnv {
public:
    /**
     * @brief Points must be in non-decreasing time order; returns false otherwise.
     *
     * Fewer than two points
     * is a constant. A sustain or loop index the new count no longer holds is dropped.
     * @param[in] points  the breakpoints, copied
     * @param[in] count   how many, 0 .. MaxEnvPoints
     * @return false for a null pointer, a count out of range or times that go backwards; the shape is then unchanged
     */
    bool set(const EnvPoint* points, int count);
    /** @brief How many breakpoints the shape has. @return 0 .. MaxEnvPoints */
    int  count() const { return m_count; }
    /**
     * @brief One breakpoint.
     * @param[in] i  0 .. count() - 1
     * @return the point
     */
    const EnvPoint& point(int i) const { return m_points[i]; }
    /**
     * @brief Chooses the point SustainLoop holds at while the note is down.
     * @param[in] index  -1 = none
     */
    void setSustain(int index) { m_sustain = index; }
    /** @brief The sustain point. @return its index, -1 = none */
    int  sustain() const { return m_sustain; }
    /**
     * @brief Chooses the two points Loop and SustainLoop cycle between.
     * @param[in] from  the loop's first point
     * @param[in] to    its last; the loop is honoured only when to > from and both exist
     */
    void setLoop(int from, int to) { m_loopFrom = from; m_loopTo = to; }
    /** @brief The loop's first point. @return its index, -1 = none */
    int  loopFrom() const { return m_loopFrom; }
    /** @brief The loop's last point. @return its index, -1 = none */
    int  loopTo() const { return m_loopTo; }
    /** @brief The shape's length in its own seconds. @return the last point's time; 0 for an empty shape */
    float length() const { return m_count > 0 ? m_points[m_count - 1].m_time : 0.0f; }

    /**
     * @brief Value at a time, following the mode.
     *
     * `held` is whether the note is still down. The time is folded into the loop, held at the
     * sustain point or clamped to the end as the mode says (readTime() gives that time), then the
     * segment it falls in is interpolated with the curve of the point that begins it.
     * @param[in] seconds  time since the envelope started, in the shape's own seconds (the caller has divided by Time)
     * @param[in] mode     how it runs
     * @param[in] held     whether the note is still down (SustainLoop listens to it)
     * @return the value, -1..1 as the points are
     */
    float at(float seconds, EnvModeE mode, bool held) const;
    /**
     * @brief The time `at` reads the shape at: folded into the loop or held at the sustain point, as the
     *        mode says, and no later than the end.
     *
     * A release continues from here, so the value does not
     * jump from where the shape was being held to where the clock has run on to meanwhile.
     * @param[in] seconds  the clock's time, in the shape's own seconds
     * @param[in] mode     how it runs
     * @param[in] held     whether the note is still down
     * @return the time in the shape, 0 .. length()
     */
    float readTime(float seconds, EnvModeE mode, bool held) const;

    /**
     * @brief Text form: "t:v:c/t:v:c/...", optionally followed by "!s\<index\>" for the sustain point and
     *        "!l\<from\>-\<to\>" for the loop.
     *
     * Times in seconds. The marker is '!' and not '|' because a
     * pack line splits its fields on '|' -- an envelope with a loop used to tear the line apart.
     * A point's curve may be left out and reads as 0. The whole string is parsed before the shape
     * is touched, so a bad one leaves the shape as it was.
     * @param[in] text  the shape
     * @return false for a null pointer, a point that does not parse, too many points, or times that go backwards
     */
    bool parse(const char* text);
    /**
     * @brief The shape as parse() text, with the "!s" and "!l" markers where they are set.
     * @param[in,out] buf  the buffer
     * @param[in]     cap  its size in bytes
     * @return characters written, or 0 when the text did not fit
     */
    int  write(char* buf, size_t cap) const;

private:
    EnvPoint m_points[MaxEnvPoints];   ///< the breakpoints, in time order
    int m_count = 0;                    ///< how many of them are in use
    /** @var int m_sustain
     *  @brief the sustain point's index, -1 = none */
    /** @var int m_loopFrom
     *  @brief the loop's first point, -1 = none */
    int m_sustain = -1, m_loopFrom = -1, m_loopTo = -1;   ///< m_loopTo: the loop's last point, -1 = none
};

/** @brief One envelope's per-preset settings that are parameters rather than shape. */
struct ModEnvSpec {
    EnvModeE m_mode = EnvModeE::ONE_SHOT;   ///< how the shape runs
    float   m_timeScale = 1.0f;   ///< stretches the whole shape, 0.05 .. 20
    float   m_depth = 1.0f;       ///< scales the value; the matrix scales again per route
};

// ---------------------------------------------------------------- matrix

/**
 * @brief Everything that can drive a target.
 *
 * The order is the text form's order, so it must only ever
 * grow at the end.
 *
 * The values a route reads are bipolar, -1..1; sources that are naturally 0..1 (the macros, the
 * amplitude, the hands, the Lenia readings, the cascade, the three from the piece's shape) are
 * mapped to -1..1 by the engine, and a route's `unipolar` flag maps them back (ModMatrix::apply()).
 * modSourceName() spells each one for the text form.
 */
enum class ModSourceE : int {
    NONE,   ///< no source: the row does nothing
    LFO1, LFO2, LFO3, LFO4, LFO5, LFO6, LFO7, LFO8,   ///< the eight free LFOs, after their own depth
    ENV1, ENV2, ENV3, ENV4, ENV5, ENV6,   ///< the six modulation envelopes, after their own depth
    AMP,                       ///< the voice's own ADSR
    MACRO_A, MACRO_B, MACRO_C, MACRO_D, MACRO_E, MACRO_F, MACRO_G, MACRO_H,   ///< the eight macro knobs, 0..1 read as -1..1
    KURA1, KURA2, KURA3, KURA4,   ///< the coherence ring, now addressable
    NOTE, VELOCITY, DISTANCE,     ///< per voice: pitch 0..1 over the keyboard, velocity, plane
    RANDOM_PER_NOTE,   ///< one draw in -1..1 per note, held until the next
    /**
     * @brief The instrument listening to its own harmonic friction.
     *
     * The two lowest sounding voices are
     * compared with the simplest just ratio near the interval they make, and the rate of this
     * oscillator is the beat between them: silent when the chord is in tune, and quicker the
     * further the tuning has drifted from it. Route it at anything and the sound breathes in
     * time with how far out of tune it currently is.
     */
    BEAT,
    /**
     * @brief What the hands are doing, as three ordinary sources.
     *
     * Pressure and Slide already reach the
     * sound through fixed routes (Expression), but only there; a player who wants aftertouch on
     * the filter's resonance, the wavetable position and the reverb at once needs them here. All
     * three rest at 0, so route them with the 0..1 flag and a patch at rest sounds untouched.
     */
    PRESSURE, WHEEL, SLIDE,
    /**
     * @brief A Lenia field (Chan 2019): a continuous cellular automaton -- Conway's Life with real-valued
     *        cells, a ring-shaped neighbourhood and a smooth growth rule -- run on a small torus in the
     *        background, read at four fixed points.
     *
     * Its blobs drift, pulse, split and die; the four
     * readings are what they do near each corner. Off the clock and off the random stream:
     * motion that is caused by its own neighbours, which is what makes it read as alive.
     */
    LENIA1, LENIA2, LENIA3, LENIA4,
    /**
     * @brief Two strange attractors, integrated in the background on a time scale of minutes: the
     *        Lorenz system (two lobes, switched between at irregular moments) and the Roessler system
     *        (a slow spiral with a sudden climb).
     *
     * Deterministic chaos: never the same path twice,
     * never a cycle, and never a step -- what an LFO cannot be and filtered noise cannot be
     * either, which is the reason for having them. Three coordinates each.
     */
    LORENZ_X, LORENZ_Y, LORENZ_Z, ROSSLER_X, ROSSLER_Y, ROSSLER_Z,
    /**
     * @brief The conductor's own excitement.
     *
     * With Cascade up its clock is a Hawkes process -- an event
     * breeds events -- and this is that excitation, as exc / (1 + exc), so it rests at 0 between
     * the clusters and climbs towards 1 inside one. It is the only source that comes from what
     * the piece is doing rather than from a clock, a shape or a field of its own, so a route
     * from it makes the instrument swell where it is busy and rest where it is not.
     */
    CASCADE,
    /**
     * @brief Three that come from the piece's own shape rather than from a clock (12.09.2026).
     *
     * Root Age: how long the root has stood, over Home Time -- 0 the moment it moves, climbing
     * towards 1 the longer it holds, so a sound can open after a change instead of being there
     * already. Layer: the role of the voice being read (foundation 0, body .25, colour .5, air
     * .75, shadow 1), which gives every register its own brightness, its own air, its own plane
     * without the conductor needing to know. Section: the hour's arc as five steps rather than a
     * glide, for the things that should switch rather than slide.
     */
    ROOT_AGE, LAYER, SECTION,
    COUNT   ///< how many sources; the size of the engine's source table
};
constexpr int NumModSources = static_cast<int>(ModSourceE::COUNT);   ///< sources in the table
/**
 * @brief The name of a source as the matrix text spells it ("lfo1", "env2", "macro_a", "lorenz_x", ...).
 * @param[in] s  the source; anything out of range reads as "none"
 * @return a static string, never null
 */
const char* modSourceName(ModSourceE s);
/**
 * @brief The inverse of modSourceName(): a source from its name.
 * @param[in]  name  the spelling, case-sensitive ("none" is ModSource::None)
 * @param[out] out   receives the source on success and is left alone otherwise
 * @return false for a null pointer or a name that is not in the table
 */
bool modSourceFromName(const char* name, ModSourceE& out);

/**
 * @brief One row of the matrix: a source, a target, a depth, an optional second source that
 *        scales the depth, and whether the source is read as 0..1.
 */
struct ModRoute {
    ModSourceE m_source = ModSourceE::NONE;   ///< what drives the row; None makes it inert
    ParamIdE   m_target = ParamIdE::CUTOFF;   ///< the parameter driven, in its own units
    float     m_depth = 0.0f;       ///< -1 .. 1 of the target's own range
    ModSourceE m_via = ModSourceE::NONE;   ///< scales the depth (a second source as an amount)
    bool      m_unipolar = false;   ///< treat the source as 0..1 instead of -1..1
};

/**
 * @brief Up to thirty-two rows.
 *
 * One source may appear in as many rows as it likes -- that is the whole
 * point of a matrix, and what the soldered drifters could never do.
 *
 * A plain value the engine keeps two of (Engine.h): the pending one the message thread edits and
 * the live one the audio thread sums with apply() once per control block.
 */
class ModMatrix {
public:
    /** @brief How many rows are in use. @return 0 .. MaxModRoutes */
    int  count() const { return m_count; }
    /**
     * @brief One row.
     * @param[in] i  0 .. count() - 1
     * @return the row
     */
    const ModRoute& route(int i) const { return m_routes[i]; }
    /** @brief Forgets every row. */
    void clear() { m_count = 0; }
    /**
     * @brief Appends a row.
     * @param[in] r  the row
     * @return false when the matrix is full
     */
    bool add(const ModRoute& r) { if (m_count >= MaxModRoutes) return false; m_routes[m_count++] = r; return true; }
    /**
     * @brief Removes one row and closes the gap, keeping the order of the rest.
     * @param[in] i  0 .. count() - 1
     * @return false for an index outside the rows
     */
    bool remove(int i);

    /**
     * @brief Text form, one row per ';': "\<source\>>\<target\>:\<depth\>[:\<via\>][:u]"
     * @code{.unparsed}
     *   "lfo1>cutoff:0.4;env2>z_x:-0.25:macro_a;lfo3>shimmer:0.6:none:u"
     * @endcode
     * The source is a name modSourceName() knows, the target a parameter key (Params.h), the depth
     * a number (a NaN is refused, since it would poison every target); the optional fields after
     * it are a via source and the flag "u", in either order. The whole text is parsed before the
     * rows are touched, so a bad one leaves the matrix as it was.
     * @param[in] text  the rows
     * @return false for a null pointer, an unknown source, target or via, a bad depth, or more rows than MaxModRoutes
     */
    bool parse(const char* text);
    /**
     * @brief The rows as parse() text.
     * @param[in,out] buf  the buffer
     * @param[in]     cap  its size in bytes
     * @return characters written, or 0 when they did not fit
     */
    int  write(char* buf, size_t cap) const;

    /**
     * @brief Sums every route into `out` (one entry per parameter, in the target's own units), given the
     *        current value of each source.
     *
     * Targets outside the sound scope are ignored. A via source always
     * acts as an amount, 0..1, whatever the flag says; depth is a fraction of the target's range,
     * so one number means the same on a cutoff in hertz and on a mix in 0..1. Adds into `out`;
     * the caller clears it first (the engine also zeroes the performance parameters afterwards,
     * Engine::stepModulation).
     * @param[in]     sourceValues  one value per ModSource, -1..1, indexed by the enum
     * @param[in,out] out           NumParams entries, added into
     */
    void apply(const float* sourceValues, float* out) const;

private:
    ModRoute m_routes[MaxModRoutes];   ///< the rows, in text order
    int m_count = 0;                    ///< how many of them are in use
};

} // namespace ambient
