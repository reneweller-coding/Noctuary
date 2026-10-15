/**
 * @file Clock.h
 * @brief The clock: a tempo, a beat position, and the note divisions that hang on it.
 *
 * A drone instrument does not need a clock to make sound, but the moment it plays with other
 * machines every rate in it wants to sit on the grid: an LFO that takes exactly two bars, a
 * delay on a dotted eighth, a grain every sixteenth, a new brain note every four bars. Each of
 * those parameters keeps its free knob and gains a Sync choice; when the choice is not Free the
 * knob is ignored and the time comes from the tempo. Where the clock comes from is one setting:
 * ```{.unparsed}
 *   Internal -- the Tempo parameter, running whenever Run is on (the standalone's own clock)
 *   Host     -- the DAW's play head (tempo and position); falls back to Internal without a host
 *   MIDI     -- MIDI clock at the input (24 ticks a quarter, start/stop/continue)
 * ```
 * The clock's parameters are performance state, like the morph: no preset touches the tempo.
 *
 * This header holds only what is shared: the two enumerations, their name tables (Clock.cpp) and
 * the arithmetic that turns a division and a tempo into seconds, hertz or a phase. The clock
 * itself -- who advances the beat position and from where -- lives in the engine.
 * @author Rene Weller
 */
#pragma once
#include <cmath>

namespace ambient {

/** @brief Where the tempo and the beat position come from (the Clock Source setting, see the file comment). */
enum class ClockSourceE : int {
    INTERNAL = 0,   ///< the Tempo parameter, running whenever Run is on
    HOST,           ///< the DAW's play head; Internal without a host
    MIDI,           ///< MIDI clock at the input
    COUNT           ///< the number of sources
};
constexpr int NumClockSources = static_cast<int>(ClockSourceE::COUNT);   ///< entries of ClockSourceNames
extern const char* const ClockSourceNames[NumClockSources];   ///< display names in ClockSource order ("Internal", "Host", "MIDI"), defined in Clock.cpp

/**
 * @brief Note divisions, in quarter-note beats (4/4).
 *
 * The long ones exist for the slow parameters: an
 * arc over 64 bars is a quarter of an hour at 90 bpm.
 *
 * The value is a parameter's Sync choice. SyncBeats gives each division its length in beats (0
 * for Free), SyncDivNames its display name; D is dotted (one and a half times), T triplet (two
 * thirds). Free is 0 so that "any nonzero value" means "synced" (syncOn()).
 */
enum class SyncDivE : int {
    FREE = 0,   ///< the knob's own time; the tempo is ignored
    BARS64,     ///< 256 beats
    BARS32,     ///< 128 beats
    BARS16,     ///< 64 beats
    BARS8,      ///< 32 beats
    BARS4,      ///< 16 beats
    BARS2,      ///< 8 beats
    BAR1,       ///< 4 beats
    HALF,           ///< 2 beats
    HALF_T,          ///< 4/3 beats, a half-note triplet
    QUARTER,        ///< 1 beat
    QUARTER_D,       ///< 1.5 beats, a dotted quarter
    QUARTER_T,       ///< 2/3 beat, a quarter triplet
    EIGHTH,         ///< 1/2 beat
    EIGHTH_D,        ///< 3/4 beat, a dotted eighth
    EIGHTH_T,        ///< 1/3 beat, an eighth triplet
    SIXTEENTH,      ///< 1/4 beat
    SIXTEENTH_T,     ///< 1/6 beat, a sixteenth triplet
    THIRTY_SECOND,   ///< 1/8 beat
    COUNT   ///< the number of divisions
};
constexpr int NumSyncDivs = static_cast<int>(SyncDivE::COUNT);   ///< entries of SyncDivNames and SyncBeats
extern const char* const SyncDivNames[NumSyncDivs];   ///< display names in SyncDiv order ("Free", "64 bars", ... "1/32"), defined in Clock.cpp
extern const double      SyncBeats[NumSyncDivs];   ///< length of each division in quarter-note beats; 0 for Free

/**
 * @brief Whether a Sync choice means "take the time from the tempo".
 * @param[in] div  a parameter's Sync value, as an int (SyncDiv)
 * @return true for every division but Free; false for Free and for anything out of range
 */
inline bool   syncOn(int div) { return div > 0 && div < NumSyncDivs; }
/**
 * @brief Length of a division in quarter-note beats.
 * @param[in] div  a SyncDiv as an int
 * @return SyncBeats of the division; 0 for Free or out of range
 */
inline double syncBeats(int div) { return syncOn(div) ? SyncBeats[div] : 0.0; }
/**
 * @brief Seconds of one division at a tempo; 0 for Free.
 * @param[in] div  a SyncDiv as an int
 * @param[in] bpm  the tempo in quarter notes per minute; anything under 1 counts as 1
 * @return beats x 60 / bpm, or 0 when the division is Free
 */
inline double syncSeconds(int div, double bpm) { return syncOn(div) ? SyncBeats[div] * 60.0 / (bpm > 1.0 ? bpm : 1.0) : 0.0; }
/**
 * @brief A rate in Hz for one cycle per division; 0 for Free.
 * @param[in] div  a SyncDiv as an int
 * @param[in] bpm  the tempo in quarter notes per minute
 * @return 1 / syncSeconds(), or 0 when the division is Free -- what an LFO or a grain clock runs at when synced
 */
inline double syncHz(int div, double bpm) { const double s = syncSeconds(div, bpm); return s > 0.0 ? 1.0 / s : 0.0; }
/**
 * @brief Where in a cycle of `div` the beat position sits, 0..1.
 * @param[in] div      a SyncDiv as an int
 * @param[in] beatPos  the clock's position in quarter-note beats since it started
 * @return the fractional part of beatPos over the division's length; 0 for Free, so a synced modulator can be
 *         phase-locked to the grid
 */
inline double syncPhase(int div, double beatPos) { const double b = syncBeats(div); if (b <= 0.0) return 0.0; const double x = beatPos / b; return x - std::floor(x); }

} // namespace ambient
