/**
 * @file Tuning.h
 * @brief Tuning: just-intonation tables, Scala parser, note -> frequency.
 *
 * The instrument does not assume twelve equal steps to the octave. A scale is a fixed-size table
 * of ratios over a repeating period (FixedScale), filled either from one of the built-in
 * just-intonation tables (makeBuiltinScale; their names are kScaleNames in Params.h, and the two
 * are kept in step by a static_assert in Tuning.cpp) or from the text of a Scala .scl file
 * (parseScala, the user slot kUserScaleIndex). scaleFrequency turns a MIDI note into hertz against
 * such a scale, and intervalConsonance ranks a ratio by the simplicity of the nearest just interval
 * -- the yardstick the conductor, the portamento gravity and the near events judge intervals by.
 * Everything here is framework-free and allocation-free once a FixedScale exists, so a scale can be
 * built on the message thread and handed to the audio thread by value.
 */
#pragma once
#include <cstddef>

namespace ambient {

/** @brief Fixed-size scale so it can be swapped into the audio thread without allocation. */
struct FixedScale {
    static constexpr int kMax = 64;   ///< the most degrees a scale may hold (Scala files with more are refused)
    int    count  = 12;        ///< degrees per period (octave), excluding the period itself
    double period = 2.0;       ///< ratio of the repeating interval (2 = octave, 3 = tritave)
    double ratios[kMax] = {};  ///< ratios[0] == 1.0, ascending, all < period
    char   name[64] = "";      ///< display name, NUL-terminated: the built-in table's, or the Scala file's description line
};

/**
 * @brief Fill `out` with built-in scale `index` (0 .. kNumScaleChoices-2). Returns false if unknown.
 *
 * Index 0 is 12-TET, computed as 2^(i/12); the others copy their just-ratio table. Message thread
 * (it only writes into @p out, never allocates).
 *
 * @param index  which built-in table, in the order of kScaleNames (Params.h); the user and timbre
 *               slots (kUserScaleIndex and above) are not tables and give false
 * @param out    receives count, period, ratios and name; untouched when the index is unknown
 * @return       true when @p out was filled
 */
bool makeBuiltinScale(int index, FixedScale& out);

/**
 * @brief Parse the text of a Scala .scl file. Returns false on malformed input.
 *
 * The format: a description line (which may be empty), the number of pitches, then one pitch per
 * line above 1/1, either as cents (a value containing a '.') or as a ratio "p/q", the last of them
 * the period. Lines starting with '!' are comments. A value's kind is decided by the value alone,
 * not by any comment following it on the same line. The result is only written into @p out when
 * the whole file parsed, so a failed parse leaves the previous scale standing.
 *
 * @param text  the whole file as a NUL-terminated string; nullptr is refused
 * @param out   receives the scale (name "Scala" when the description line is empty)
 * @return      true when @p out holds a valid scale with 1 .. kMax degrees and a period above 1
 */
bool parseScala(const char* text, FixedScale& out);

/**
 * @brief Frequency of a MIDI note. `rootNote` (MIDI number) sounds ratios[0] at the
 *        equal-tempered pitch of that key with reference A4 = refA4 Hz.
 *
 *   snapToKeys = true : the 12 keys of each octave snap to the nearest scale degree
 *                       (keyboard stays intuitive; only for octave-periodic scales,
 *                       otherwise falls back to consecutive mapping)
 *   snapToKeys = false: consecutive keys map to consecutive scale degrees
 *                       (the usual mapping for 19-EDO, 31-EDO, Bohlen-Pierce ...)
 *
 * Pure arithmetic, safe on the audio thread. When snapping, the period itself (degree 0 of the
 * next octave) is a candidate too, so the top key of an octave can land on the octave.
 *
 * @param s           the scale to read the ratios from
 * @param midiNote    the key, any integer (keys below the root go down by periods)
 * @param rootNote    the key that sounds ratios[0], as a MIDI note number
 * @param refA4       the reference pitch of MIDI note 69 in Hz (the Ref Pitch parameter)
 * @param snapToKeys  the key mapping, see above
 * @return            the frequency in Hz
 */
double scaleFrequency(const FixedScale& s, int midiNote, int rootNote, double refA4, bool snapToKeys = true);

/**
 * @brief Interval consonance in [0,1]: 1 for a unison/octave, decreasing with the
 *        complexity of the nearest simple ratio (Tenney-height based).
 *
 * The ratio is folded into one octave, then the nearest p/q with q <= 32 within ten cents is
 * taken and scored 1 / (1 + log2(p q)). An interval that is not within ten cents of any such
 * ratio gets a floor of 0.05, so a wandering pitch is never rated as exactly nothing.
 *
 * @param ratio  frequency ratio of the two tones, any positive value (0 or less gives 0)
 * @return       the consonance, 0.05 .. 1
 */
double intervalConsonance(double ratio);

} // namespace ambient
