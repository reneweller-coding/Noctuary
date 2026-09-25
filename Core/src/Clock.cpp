/**
 * @file Clock.cpp
 * @brief The clock's tables: the names of its sources and divisions, and each division in beats.
 *
 * Everything the clock computes is inline in Clock.h; this file holds only the three tables the
 * header declares. kClockSourceNames and kSyncDivNames are what the Source and Sync choices show on
 * the panel, and kSyncBeats is the division list again in quarter-note beats of 4/4 -- 0 for Free,
 * 256 for 64 bars, a dotted division at one and a half times its plain one, a triplet at two thirds
 * -- which is what syncSeconds(), syncHz() and syncPhase() read. The three tables follow the order
 * of the ClockSource and SyncDiv enums and must stay in step with them.
 */
#include "ambient/Clock.h"

namespace ambient {

const char* const kClockSourceNames[kNumClockSources] = { "Internal", "Host", "MIDI" };

const char* const kSyncDivNames[kNumSyncDivs] = {
    "Free", "64 bars", "32 bars", "16 bars", "8 bars", "4 bars", "2 bars", "1 bar",
    "1/2", "1/2 T", "1/4", "1/4 D", "1/4 T", "1/8", "1/8 D", "1/8 T", "1/16", "1/16 T", "1/32",
};

const double kSyncBeats[kNumSyncDivs] = {
    0.0, 256.0, 128.0, 64.0, 32.0, 16.0, 8.0, 4.0,
    2.0, 4.0 / 3.0, 1.0, 1.5, 2.0 / 3.0, 0.5, 0.75, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125,
};

} // namespace ambient
