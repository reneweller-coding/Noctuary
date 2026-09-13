// Noctuary -- the clock: a tempo, a beat position, and the note divisions that hang on it.
//
// A drone instrument does not need a clock to make sound, but the moment it plays with other
// machines every rate in it wants to sit on the grid: an LFO that takes exactly two bars, a
// delay on a dotted eighth, a grain every sixteenth, a new brain note every four bars. Each of
// those parameters keeps its free knob and gains a Sync choice; when the choice is not Free the
// knob is ignored and the time comes from the tempo. Where the clock comes from is one setting:
//   Internal -- the Tempo parameter, running whenever Run is on (the standalone's own clock)
//   Host     -- the DAW's play head (tempo and position); falls back to Internal without a host
//   MIDI     -- MIDI clock at the input (24 ticks a quarter, start/stop/continue)
// The clock's parameters are performance state, like the morph: no preset touches the tempo.
#pragma once
#include <cmath>

namespace ambient {

enum class ClockSource : int { Internal = 0, Host, Midi, Count };
constexpr int kNumClockSources = static_cast<int>(ClockSource::Count);
extern const char* const kClockSourceNames[kNumClockSources];

// Note divisions, in quarter-note beats (4/4). The long ones exist for the slow parameters: an
// arc over 64 bars is a quarter of an hour at 90 bpm.
enum class SyncDiv : int {
    Free = 0, Bars64, Bars32, Bars16, Bars8, Bars4, Bars2, Bar1,
    Half, HalfT, Quarter, QuarterD, QuarterT, Eighth, EighthD, EighthT, Sixteenth, SixteenthT, ThirtySecond,
    Count
};
constexpr int kNumSyncDivs = static_cast<int>(SyncDiv::Count);
extern const char* const kSyncDivNames[kNumSyncDivs];
extern const double      kSyncBeats[kNumSyncDivs];   // 0 for Free

inline bool   syncOn(int div) { return div > 0 && div < kNumSyncDivs; }
inline double syncBeats(int div) { return syncOn(div) ? kSyncBeats[div] : 0.0; }
// Seconds of one division at a tempo; 0 for Free.
inline double syncSeconds(int div, double bpm) { return syncOn(div) ? kSyncBeats[div] * 60.0 / (bpm > 1.0 ? bpm : 1.0) : 0.0; }
// A rate in Hz for one cycle per division; 0 for Free.
inline double syncHz(int div, double bpm) { const double s = syncSeconds(div, bpm); return s > 0.0 ? 1.0 / s : 0.0; }
// Where in a cycle of `div` the beat position sits, 0..1.
inline double syncPhase(int div, double beatPos) { const double b = syncBeats(div); if (b <= 0.0) return 0.0; const double x = beatPos / b; return x - std::floor(x); }

} // namespace ambient
