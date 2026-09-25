/**
 * @file Tuning.cpp
 * @brief The tuning tables and the arithmetic over them: built-in scales, the Scala reader,
 *        note-to-frequency and interval consonance.
 *
 * The built-in scales are just-intonation tables written as ratios over the root, one row per
 * scale, in the order of kScaleNames (Params.h) -- a static_assert keeps the two in step. 12-TET is
 * the one exception and is computed rather than tabulated. makeBuiltinScale() copies a row into a
 * FixedScale, the fixed-size form that can be swapped into the audio thread without allocating;
 * parseScala() fills the same form from the text of a .scl file, deciding cents against ratio by
 * the value alone and tolerating the empty description line the format allows.
 *
 * scaleFrequency() is the one function the voices call: MIDI note in, hertz out, with the root key
 * sounding ratios[0] at its equal-tempered pitch and the keyboard either snapped to the nearest
 * degree (twelve keys an octave, for octave-periodic scales) or walked degree by degree.
 * intervalConsonance() ranks a ratio by the Tenney height of the nearest simple fraction within
 * ten cents; the cluster brain uses it to prefer the intervals a just scale makes consonant.
 */
#include "ambient/Tuning.h"
#include "ambient/Params.h"
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>

namespace ambient {

namespace {

/** @brief One built-in scale as the table describes it: a name, the repeating interval and the ratios of its degrees. */
struct BuiltinDef {
    const char* name;       ///< the display name, the same text as in kScaleNames
    double period;          ///< ratio of the repeating interval: 2 for an octave, 3 for the Bohlen-Pierce tritave
    int n;                  ///< degrees per period, the period itself excluded
    const double* ratios;   ///< the n ratios over the root, ascending from 1; nullptr for 12-TET, which is computed
};

/**
 * @name The ratio tables of the built-in scales
 * Ratios exclude the period; ratios[0] must be 1.
 * @{ */
const double kMajor[]      = { 1, 9.0/8, 5.0/4, 4.0/3, 3.0/2, 5.0/3, 15.0/8 };                    ///< JI Major (Ptolemy's intense diatonic), 5-limit
const double kMinor[]      = { 1, 9.0/8, 6.0/5, 4.0/3, 3.0/2, 8.0/5, 9.0/5 };                     ///< JI Minor, the natural minor in 5-limit ratios
const double kSevenLimit[] = { 1, 9.0/8, 7.0/6, 5.0/4, 4.0/3, 7.0/5, 3.0/2, 8.0/5, 5.0/3, 7.0/4, 15.0/8 };   ///< JI 7-limit: eleven degrees with the septimal third, tritone and seventh
const double kPythag[]     = { 1, 9.0/8, 81.0/64, 4.0/3, 3.0/2, 27.0/16, 243.0/128 };             ///< Pythagorean: every degree a stack of pure fifths
const double kPenta[]      = { 1, 9.0/8, 5.0/4, 3.0/2, 5.0/3 };                                   ///< JI Pentatonic, the major pentatonic in 5-limit ratios
const double kHarm[]       = { 1, 9.0/8, 5.0/4, 11.0/8, 3.0/2, 13.0/8, 7.0/4, 15.0/8 };           ///< Harmonic 8-16: harmonics 8 to 15 over the eighth
const double kSubharm[]    = { 1, 16.0/15, 8.0/7, 16.0/13, 4.0/3, 16.0/11, 8.0/5, 16.0/9 };       ///< Subharmonic 16-8: the harmonic series turned upside down
const double kSlendro[]    = { 1, 8.0/7, 21.0/16, 3.0/2, 7.0/4 };                                 ///< Slendro (JI): a just reading of the five-tone gamelan scale
const double kBP[]         = { 1, 27.0/25, 25.0/21, 9.0/7, 7.0/5, 75.0/49, 5.0/3, 9.0/5, 49.0/25, 15.0/7, 7.0/3, 63.0/25, 25.0/9 };   ///< Bohlen-Pierce (JI): thirteen degrees repeating at the tritave (period 3)
const double kOtonal[]     = { 1, 9.0/8, 5.0/4, 11.0/8, 3.0/2, 7.0/4 };                           ///< Otonality 1-11: harmonics 8, 9, 10, 11, 12 and 14 over the eighth
/** @} */

/**
 * @brief Builds a BuiltinDef from a ratio table, taking the degree count from the array's size so
 *        that it cannot be miscounted.
 * @tparam N      the number of ratios in the table, deduced
 * @param name    the display name, the same text as in kScaleNames
 * @param period  ratio of the repeating interval (2 = octave, 3 = tritave)
 * @param r       the ratio table, ratios[0] == 1, ascending, all below the period
 * @return        the definition kBuiltins holds
 */
template <size_t N> constexpr BuiltinDef def(const char* name, double period, const double (&r)[N])
{ return { name, period, static_cast<int>(N), r }; }

/**
 * @brief The built-in scales in the order of kScaleNames (Params.h): 12-TET first, computed, then
 *        the ten just tables above.
 */
const BuiltinDef kBuiltins[] = {
    { "12-TET", 2.0, 12, nullptr },
    def("JI Major (Ptolemy)", 2.0, kMajor),
    def("JI Minor", 2.0, kMinor),
    def("JI 7-limit", 2.0, kSevenLimit),
    def("Pythagorean", 2.0, kPythag),
    def("JI Pentatonic", 2.0, kPenta),
    def("Harmonic 8-16", 2.0, kHarm),
    def("Subharmonic 16-8", 2.0, kSubharm),
    def("Slendro (JI)", 2.0, kSlendro),
    def("Bohlen-Pierce (JI)", 3.0, kBP),
    def("Otonality 1-11", 2.0, kOtonal),
};
constexpr int kNumBuiltins = static_cast<int>(sizeof(kBuiltins) / sizeof(kBuiltins[0]));   ///< how many scales kBuiltins holds; must equal kUserScaleIndex, the first choice that is not a table
static_assert(kNumBuiltins == kUserScaleIndex, "scale name table and built-in table out of sync");

/**
 * @brief Copies a name into the scale's fixed buffer, truncated to fit and always NUL-terminated.
 * @param s     the scale whose name is set
 * @param name  the text to copy; longer than FixedScale::name holds, it is cut
 */
void copyName(FixedScale& s, const char* name)
{
    std::strncpy(s.name, name, sizeof(s.name) - 1);
    s.name[sizeof(s.name) - 1] = 0;
}

} // namespace

bool makeBuiltinScale(int index, FixedScale& out)
{
    if (index < 0 || index >= kNumBuiltins) return false;
    const BuiltinDef& d = kBuiltins[index];
    out.period = d.period;
    out.count  = d.n;
    if (d.ratios == nullptr) {
        for (int i = 0; i < 12; ++i) out.ratios[i] = std::pow(2.0, i / 12.0);
    } else {
        for (int i = 0; i < d.n; ++i) out.ratios[i] = d.ratios[i];
    }
    copyName(out, d.name);
    return true;
}

bool parseScala(const char* text, FixedScale& out)
{
    if (text == nullptr) return false;
    FixedScale s;
    int lineState = 0;   // 0 = description, 1 = count, 2+ = pitches
    int expected = 0, got = 0;
    double values[FixedScale::kMax + 1];

    const char* p = text;
    while (*p) {
        const char* end = p;
        while (*end && *end != '\n') ++end;
        // trim
        const char* a = p;
        while (a < end && (*a == ' ' || *a == '\t' || *a == '\r')) ++a;
        // The description may be an empty line -- the format allows it and many files have one.
        // Skipping blank lines in every state turned the count line into the name and the first
        // pitch into the count, and a valid file was refused.
        if (a >= end && lineState == 0) { copyName(s, ""); lineState = 1; }
        if (a < end && *a != '!') {
            char line[256];
            size_t len = static_cast<size_t>(end - a);
            if (len >= sizeof(line)) len = sizeof(line) - 1;
            std::memcpy(line, a, len);
            line[len] = 0;
            if (lineState == 0) {
                copyName(s, line);
                lineState = 1;
            } else if (lineState == 1) {
                expected = std::atoi(line);
                if (expected < 1 || expected > FixedScale::kMax) return false;
                lineState = 2;
            } else if (got < expected) {
                double v = 0.0;
                char* endp = nullptr;
                // Cents or ratio is decided by the value alone, not by the comment that may
                // follow it on the same line: "3/2 pure fifth (approx. 702c)" is a ratio.
                size_t vend = 0;
                while (line[vend] != 0 && line[vend] != ' ' && line[vend] != '\t') ++vend;
                if (std::memchr(line, '.', vend) != nullptr) {
                    double cents = std::strtod(line, &endp);
                    v = std::pow(2.0, cents / 1200.0);
                } else {
                    long num = std::strtol(line, &endp, 10);
                    long den = 1;
                    if (endp && *endp == '/') den = std::strtol(endp + 1, nullptr, 10);
                    if (num <= 0 || den <= 0) return false;
                    v = static_cast<double>(num) / static_cast<double>(den);
                }
                if (!(v > 0.0)) return false;
                values[got++] = v;
            }
        }
        p = (*end) ? end + 1 : end;
    }
    if (lineState < 2 || got != expected) return false;

    // Scala lists pitches above 1/1 and ends with the period.
    s.period = values[expected - 1];
    s.count  = expected;
    s.ratios[0] = 1.0;
    for (int i = 1; i < expected; ++i) s.ratios[i] = values[i - 1];
    if (s.period <= 1.0) return false;
    if (s.name[0] == 0) copyName(s, "Scala");
    out = s;
    return true;
}

double scaleFrequency(const FixedScale& s, int midiNote, int rootNote, double refA4, bool snapToKeys)
{
    const double rootFreq = refA4 * std::pow(2.0, (rootNote - 69) / 12.0);
    const int n = s.count > 0 ? s.count : 1;
    const int k = midiNote - rootNote;
    const bool octavePeriodic = std::fabs(s.period - 2.0) < 1e-9;
    if (snapToKeys && octavePeriodic) {
        const int oct = (k >= 0) ? k / 12 : -((-k + 11) / 12);
        const int semis = k - oct * 12;
        const double target = std::pow(2.0, semis / 12.0);
        double best = s.ratios[0], bestErr = 1e9;
        for (int d = 0; d <= n; ++d) {
            const double r = (d < n) ? s.ratios[d] : s.period;   // degree 0 of the next octave is a candidate too
            const double err = std::fabs(std::log2(r / target));
            if (err < bestErr) { bestErr = err; best = r; }
        }
        return rootFreq * std::pow(2.0, oct) * best;
    }
    const int oct = (k >= 0) ? k / n : -((-k + n - 1) / n);
    const int deg = k - oct * n;
    return rootFreq * std::pow(s.period, oct) * s.ratios[deg];
}

double intervalConsonance(double ratio)
{
    if (!(ratio > 0.0)) return 0.0;
    while (ratio >= 2.0) ratio *= 0.5;
    while (ratio < 1.0) ratio *= 2.0;
    // Nearest simple ratio p/q with q <= 32 within 10 cents; rank by Tenney height.
    double best = 1e9; long bp = 0;
    for (long q = 1; q <= 32; ++q) {
        const long p = static_cast<long>(std::lround(ratio * static_cast<double>(q)));
        if (p < q) continue;
        const double err = std::fabs(std::log2((static_cast<double>(p) / static_cast<double>(q)) / ratio)) * 1200.0;
        if (err > 10.0) continue;
        const double height = std::log2(static_cast<double>(p * q));
        if (height < best) { best = height; bp = p; }
    }
    if (bp == 0) return 0.05;
    return 1.0 / (1.0 + best);
}

} // namespace ambient
