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
 */
#pragma once
#include <cmath>

namespace ambient {

/** @brief Where the tempo and the beat position come from (the Clock Source setting, see the file comment). */
enum class ClockSource : int {
    Internal = 0,   ///< the Tempo parameter, running whenever Run is on
    Host,           ///< the DAW's play head; Internal without a host
    Midi,           ///< MIDI clock at the input
    Count           ///< the number of sources
};
constexpr int kNumClockSources = static_cast<int>(ClockSource::Count);   ///< entries of kClockSourceNames
extern const char* const kClockSourceNames[kNumClockSources];   ///< display names in ClockSource order ("Internal", "Host", "MIDI"), defined in Clock.cpp

/**
 * @brief Note divisions, in quarter-note beats (4/4).
 *
 * The long ones exist for the slow parameters: an
 * arc over 64 bars is a quarter of an hour at 90 bpm.
 *
 * The value is a parameter's Sync choice. kSyncBeats gives each division its length in beats (0
 * for Free), kSyncDivNames its display name; D is dotted (one and a half times), T triplet (two
 * thirds). Free is 0 so that "any nonzero value" means "synced" (syncOn()).
 */
enum class SyncDiv : int {
    Free = 0,   ///< the knob's own time; the tempo is ignored
    Bars64,     ///< 256 beats
    Bars32,     ///< 128 beats
    Bars16,     ///< 64 beats
    Bars8,      ///< 32 beats
    Bars4,      ///< 16 beats
    Bars2,      ///< 8 beats
    Bar1,       ///< 4 beats
    Half,           ///< 2 beats
    HalfT,          ///< 4/3 beats, a half-note triplet
    Quarter,        ///< 1 beat
    QuarterD,       ///< 1.5 beats, a dotted quarter
    QuarterT,       ///< 2/3 beat, a quarter triplet
    Eighth,         ///< 1/2 beat
    EighthD,        ///< 3/4 beat, a dotted eighth
    EighthT,        ///< 1/3 beat, an eighth triplet
    Sixteenth,      ///< 1/4 beat
    SixteenthT,     ///< 1/6 beat, a sixteenth triplet
    ThirtySecond,   ///< 1/8 beat
    Count   ///< the number of divisions
};
constexpr int kNumSyncDivs = static_cast<int>(SyncDiv::Count);   ///< entries of kSyncDivNames and kSyncBeats
extern const char* const kSyncDivNames[kNumSyncDivs];   ///< display names in SyncDiv order ("Free", "64 bars", ... "1/32"), defined in Clock.cpp
extern const double      kSyncBeats[kNumSyncDivs];   ///< length of each division in quarter-note beats; 0 for Free

/**
 * @brief Whether a Sync choice means "take the time from the tempo".
 * @param div  a parameter's Sync value, as an int (SyncDiv)
 * @return     true for every division but Free; false for Free and for anything out of range
 */
inline bool   syncOn(int div) { return div > 0 && div < kNumSyncDivs; }
/**
 * @brief Length of a division in quarter-note beats.
 * @param div  a SyncDiv as an int
 * @return     kSyncBeats of the division; 0 for Free or out of range
 */
inline double syncBeats(int div) { return syncOn(div) ? kSyncBeats[div] : 0.0; }
/**
 * @brief Seconds of one division at a tempo; 0 for Free.
 * @param div  a SyncDiv as an int
 * @param bpm  the tempo in quarter notes per minute; anything under 1 counts as 1
 * @return     beats x 60 / bpm, or 0 when the division is Free
 */
inline double syncSeconds(int div, double bpm) { return syncOn(div) ? kSyncBeats[div] * 60.0 / (bpm > 1.0 ? bpm : 1.0) : 0.0; }
/**
 * @brief A rate in Hz for one cycle per division; 0 for Free.
 * @param div  a SyncDiv as an int
 * @param bpm  the tempo in quarter notes per minute
 * @return     1 / syncSeconds(), or 0 when the division is Free -- what an LFO or a grain clock runs at when synced
 */
inline double syncHz(int div, double bpm) { const double s = syncSeconds(div, bpm); return s > 0.0 ? 1.0 / s : 0.0; }
/**
 * @brief Where in a cycle of `div` the beat position sits, 0..1.
 * @param div      a SyncDiv as an int
 * @param beatPos  the clock's position in quarter-note beats since it started
 * @return         the fractional part of beatPos over the division's length; 0 for Free, so a synced modulator can be phase-locked to the grid
 */
inline double syncPhase(int div, double beatPos) { const double b = syncBeats(div); if (b <= 0.0) return 0.0; const double x = beatPos / b; return x - std::floor(x); }

} // namespace ambient
