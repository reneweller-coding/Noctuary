// Noctuary -- tuning: just-intonation tables, Scala parser, note -> frequency.
#pragma once
#include <cstddef>

namespace ambient {

// Fixed-size scale so it can be swapped into the audio thread without allocation.
struct FixedScale {
    static constexpr int kMax = 64;
    int    count  = 12;        // degrees per period (octave), excluding the period itself
    double period = 2.0;       // ratio of the repeating interval (2 = octave, 3 = tritave)
    double ratios[kMax] = {};  // ratios[0] == 1.0, ascending, all < period
    char   name[64] = "";
};

// Fill `out` with built-in scale `index` (0 .. kNumScaleChoices-2). Returns false if unknown.
bool makeBuiltinScale(int index, FixedScale& out);

// Parse the text of a Scala .scl file. Returns false on malformed input.
bool parseScala(const char* text, FixedScale& out);

// Frequency of a MIDI note. `rootNote` (MIDI number) sounds ratios[0] at the
// equal-tempered pitch of that key with reference A4 = refA4 Hz.
//   snapToKeys = true : the 12 keys of each octave snap to the nearest scale degree
//                       (keyboard stays intuitive; only for octave-periodic scales,
//                       otherwise falls back to consecutive mapping)
//   snapToKeys = false: consecutive keys map to consecutive scale degrees
//                       (the usual mapping for 19-EDO, 31-EDO, Bohlen-Pierce ...)
double scaleFrequency(const FixedScale& s, int midiNote, int rootNote, double refA4, bool snapToKeys = true);

// Interval consonance in [0,1]: 1 for a unison/octave, decreasing with the
// complexity of the nearest simple ratio (Tenney-height based).
double intervalConsonance(double ratio);

} // namespace ambient
