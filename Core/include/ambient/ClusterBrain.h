/**
 * @file ClusterBrain.h
 * @brief Cluster Brain: the generative "sleep concert" conductor.
 *
 * Slowly starts and stops notes of the current scale around a wandering root,
 * weighted by interval consonance. Emits note events; the engine turns them into
 * voices with their long envelopes. Header-only (templated callbacks), no allocation.
 *
 * Where it sits. The engine owns two conductors (Engine::brain_ for the foreground cluster and
 * brain2_ for the background one, on the first's root plus an interval) and ticks each of them
 * once per control block from Engine::process, on the audio thread, through ClusterBrain::update;
 * the knobs arrive in a BrainParams the engine fills from the parameters (EngineControl.cpp), and
 * every decision leaves as a BrainEvent that the engine's lambda turns into startNote or stopNote.
 * The crossfade between two presets is the one place the conductor is touched from the message
 * thread: soundingNotes() reads the chord out of the engine that leaves and adopt() hands it to
 * the one that arrives (Engine::adoptCluster).
 *
 * What the file holds, in order: the mode and the partial template the conductor judges timbre
 * with; the psychoacoustic measures it weighs a candidate note by -- Sethares' spectral roughness
 * and the consonance made of it, the scale a timbre asks for (makeTimbreScale, which is also how
 * the engine builds its Timbre scale), Terhardt's harmonicity of a whole chord, the
 * Krumhansl-Kessler key profiles and the key found with them, Tymoczko's evenness; the knobs
 * (BrainParams); the event; and the conductor itself. Every measure is written so that a knob at
 * its default changes nothing, down to the random stream, because the library's presets were
 * rendered and measured before most of the knobs existed. The comments cite the user's rule book
 * of 12.09.2026 by section (R6.1, Anti 2, G3, table 2): those are the rules the conductor was made
 * to keep, and the measurements beside them are what it did before and after.
 */
#pragma once
#include "Dsp.h"
#include "Tuning.h"
#include <cmath>
#include <cstdio>    // the timbre scale names itself

namespace ambient {

/**
 * @brief How the conductor works.
 *
 * Free is what it has always done: notes start and stop on their own
 * timers, so the cluster breathes but never really moves. Chords keeps the cluster full and
 * exchanges exactly one voice at a time, choosing the new note for how it sits against the ones
 * that stay and for how far that voice has to travel -- which is voice leading, and it is what
 * makes a drone shift into a new chord instead of merely churning.
 *
 * Free is what every preset in the library selects; the same names serve the Autoplay parameter
 * (Params.cpp, kBrainModeNames).
 */
enum class BrainMode : int { Free = 0, Chords, Count };
/** @var BrainMode::Free
 *  @brief Notes start and stop on their own timers; the cluster breathes around its root. */
/** @var BrainMode::Chords
 *  @brief The cluster is kept full and one voice at a time is exchanged for the best-fitting note within its voice-leading allowance. */
/** @var BrainMode::Count
 *  @brief Number of modes, for the tables. */
constexpr int kNumBrainModes = static_cast<int>(BrainMode::Count);   ///< entries of kBrainModeNames
extern const char* const kBrainModeNames[kNumBrainModes];             ///< "Free", "Chords" -- the choice names of the parameter (Params.cpp)

/**
 * @brief The spectrum the conductor judges intervals with when Timbre is up: the partial template of
 * the voice as it stands -- the same tilt, odd/even weight, brightness window and inharmonic
 * stretch the bank renders with -- as frequency ratios to the fundamental and amplitudes.
 *
 * The engine fills one from the voice parameters whenever they change (EngineControl.cpp,
 * brainSpec_) and points both conductors' BrainParams::spectrum at it; makeTimbreScale builds the
 * Timbre scale from the same template, and the editor draws it.
 */
struct BrainSpectrum {
    static constexpr int kMax = 12;   ///< the most partials a template holds
    int    count = 0;                 ///< partials in use, 0 .. kMax; at 0 the conductor falls back to the ratio score
    double ratio[kMax] = {};    ///< f_h / f0, including the stiff-string stretch
    double amp[kMax] = {};      ///< amplitude of each partial, linear and relative to one another; the scale does not matter, spectralConsonance normalises
};

/**
 * @brief Sethares' sensory roughness of two tones that share this partial template.
 *
 * Roughness of two tones with this template at f1 and f2, after Sethares (1993, 2005), which is
 * Plomp and Levelt's curve for a pair of pure tones summed over every pair of partials:
 *     d(x) = a1 a2 (exp(-b1 s x) - exp(-b2 s x)),  s = d* / (s1 min(f) + s2),
 *     b1 = 3.5, b2 = 5.75, d* = 0.24, s1 = 0.021, s2 = 19
 * so that the peak of the roughness sits at about a quarter of a critical bandwidth whatever the
 * register. Pairs more than a few critical bandwidths apart contribute nothing and are skipped.
 *
 * @param f1  fundamental of the first tone, Hz
 * @param f2  fundamental of the second tone, Hz
 * @param sp  the partial template both tones are sounded with
 * @return    the summed roughness, non-negative, in the square of the template's amplitude units;
 *            not normalised, so compare only values made with the same template
 */
inline double spectralRoughness(double f1, double f2, const BrainSpectrum& sp)
{
    double d = 0.0;
    for (int i = 0; i < sp.count; ++i) {
        const double fa = f1 * sp.ratio[i];
        for (int j = 0; j < sp.count; ++j) {
            const double fb = f2 * sp.ratio[j];
            const double lo = fa < fb ? fa : fb;
            const double x = std::fabs(fb - fa);
            const double s = 0.24 / (0.021 * lo + 19.0);
            const double u = s * x;
            if (u > 2.0) continue;                       // exp(-3.5 * 2) is under a thousandth
            d += sp.amp[i] * sp.amp[j] * (std::exp(-3.5 * u) - std::exp(-5.75 * u));
        }
    }
    return d;
}

/**
 * @brief The Plomp-Levelt roughness of a spectrum that was measured: n peaks, normalised to describe
 *        the shape of the sound rather than its level.
 *
 * The same curve over a spectrum that was measured rather than one that was assumed: n peaks at
 * f[i] with amplitude a[i]. Normalised by the square of the total amplitude, so it describes the
 * shape of the sound and not how loud it is -- 0 for a single tone or a pure octave, and up where
 * partials sit a few tens of hertz apart and beat. The library's map reads a preset's roughness
 * with it; the conductor uses the template form above, and both are the same Plomp-Levelt curve.
 *
 * @param f  frequencies of the peaks, Hz, in any order
 * @param a  their amplitudes, linear, same length as @p f
 * @param n  how many peaks
 * @return   the normalised roughness, 0 for no beating at all; 0 as well when the amplitudes sum to nothing
 */
inline double peakRoughness(const double* f, const double* a, int n)
{
    double d = 0.0, sum = 0.0;
    for (int i = 0; i < n; ++i) sum += a[i];
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            const double lo = f[i] < f[j] ? f[i] : f[j];
            const double x = std::fabs(f[j] - f[i]);
            const double u = 0.24 / (0.021 * lo + 19.0) * x;
            if (u > 2.0) continue;
            d += a[i] * a[j] * (std::exp(-3.5 * u) - std::exp(-5.75 * u));
        }
    return sum > 1.0e-12 ? 2.0 * d / (sum * sum) : 0.0;
}

/**
 * @brief The scale a timbre asks for.
 *
 * This is the central claim of Sethares (Tuning, Timbre, Spectrum, Scale, 2005), and it runs the
 * other way round from how an instrument is normally built. A scale is not a thing you choose and
 * then find a sound for; to a given spectrum there BELONGS a set of intervals at which that
 * spectrum, sounded against a transposed copy of itself, is least rough. Play those intervals and
 * the partials line up; play others and they beat. For a harmonic spectrum those intervals are
 * just intonation -- which is why just intonation exists at all, rather than because the numbers
 * are small. For an inharmonic one they are somewhere else entirely, and the ordinary scales are
 * as arbitrary there as a gamelan's would be on a piano.
 *
 * The curve is the roughness of the timbre against itself transposed, swept across the octave;
 * the scale is where it dips. Nothing here is a table: change the Tilt or the Inharmonic knob and
 * the tuning follows the sound.
 *
 * Measured on this instrument's own spectrum: for the plain harmonic setting the minima land on
 * 5/4, 4/3, 3/2, 8/5, 5/3 and 7/4 to within a cent, and on 7/5 and 10/7 in the tritone. Turn
 * Inharmonic up and only 4/3 survives.
 *
 * Called by the engine on the message thread whenever the template changes (EngineControl.cpp,
 * the Timbre scale slot); four hundred roughness evaluations, no allocation.
 *
 * @param sp      the partial template to find the scale of
 * @param out     receives the scale: period 2, ratios ascending from 1.0, named "Timbre (n)"; left as
 *                an empty FixedScale when the function returns false after reaching it
 * @param want    how many dips to keep at most (the least rough ones), capped at FixedScale::kMax - 1
 * @param baseHz  the fundamental the sweep is made at; the curve depends on the register through the
 *                critical bandwidth, so middle C is the reference
 * @return        true when a scale of at least two degrees (the unison and one dip) was made
 */
inline bool makeTimbreScale(const BrainSpectrum& sp, FixedScale& out, int want = 12,
                            double baseHz = 261.6255653005986)
{
    if (sp.count <= 0) return false;
    // Three cents is close enough for finding a dip and cheap enough to do while the sound runs;
    // where a dip is found, three points around it fit a parabola and give the bottom to well
    // under a cent.
    constexpr int kStep = 3, kPoints = 1200 / kStep;
    double d[kPoints + 2];
    for (int k = 0; k <= kPoints; ++k)
        d[k] = spectralRoughness(baseHz, baseHz * std::pow(2.0, (k * kStep) / 1200.0), sp);

    struct Dip { double cents, depth; };
    Dip dips[kPoints];
    int n = 0;
    for (int k = 1; k < kPoints; ++k) {
        if (!(d[k] < d[k - 1] && d[k] <= d[k + 1])) continue;
        const double a = d[k - 1], b = d[k], c = d[k + 1];
        const double den = a - 2.0 * b + c;
        const double off = std::fabs(den) > 1.0e-18 ? clampv(0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
        dips[n].cents = (k + off) * kStep;
        dips[n].depth = b;
        ++n;
    }
    if (n < 2) return false;                     // a spectrum with almost no minima has no scale
    // The least rough dips are the ones worth having. Insertion sort: n is at most a few dozen.
    for (int i = 1; i < n; ++i) {
        const Dip v = dips[i];
        int j = i - 1;
        while (j >= 0 && dips[j].depth > v.depth) { dips[j + 1] = dips[j]; --j; }
        dips[j + 1] = v;
    }
    int keep = n < want ? n : want;
    if (keep > FixedScale::kMax - 1) keep = FixedScale::kMax - 1;
    double cents[FixedScale::kMax];
    for (int i = 0; i < keep; ++i) cents[i] = dips[i].cents;
    for (int i = 1; i < keep; ++i) {             // back into pitch order
        const double v = cents[i];
        int j = i - 1;
        while (j >= 0 && cents[j] > v) { cents[j + 1] = cents[j]; --j; }
        cents[j + 1] = v;
    }
    out = FixedScale{};
    out.period = 2.0;
    out.ratios[0] = 1.0;
    int m = 1;
    for (int i = 0; i < keep && m < FixedScale::kMax; ++i) {
        if (cents[i] < 8.0 || cents[i] > 1192.0) continue;                 // the unison and the octave are already there
        if (m > 0 && cents[i] - (1200.0 * std::log2(out.ratios[m - 1])) < 8.0) continue;   // and no two degrees a hair apart
        out.ratios[m++] = std::pow(2.0, cents[i] / 1200.0);
    }
    out.count = m;
    std::snprintf(out.name, sizeof(out.name), "Timbre (%d)", m);
    return m >= 2;
}

/**
 * @brief The spectral roughness turned into a consonance in 0 .. 1, on the scale of intervalConsonance().
 *
 * The roughness turned into a consonance in 0..1 that sits on the same scale as
 * intervalConsonance(): 1 for a tone against itself, about a tenth for a semitone. The semitone
 * is the yardstick because it is the roughest interval a scale contains, and measuring against
 * it rather than against a fixed number keeps the score meaningful for a soft timbre with few
 * partials and for a bright one alike.
 *
 * @param f1  the reference tone, Hz; the semitone yardstick is measured above it
 * @param f2  the tone judged against it, Hz
 * @param sp  the partial template; an empty one answers 1
 * @return    consonance 0.05 .. 1 -- floored at 0.05 so that no interval is ever quite impossible
 */
inline double spectralConsonance(double f1, double f2, const BrainSpectrum& sp)
{
    if (sp.count <= 0) return 1.0;
    const double d = spectralRoughness(f1, f2, sp) - spectralRoughness(f1, f1, sp);   // less the tone's own roughness
    const double ref = spectralRoughness(f1, f1 * 1.0594630943592953, sp) - spectralRoughness(f1, f1, sp);
    if (ref <= 1.0e-12) return 1.0;
    const double c = std::exp(-2.3 * (d > 0.0 ? d : 0.0) / ref);
    return c < 0.05 ? 0.05 : c;
}

/**
 * @brief How strongly a set of tones implies ONE virtual root: harmonicity, in the sense of Terhardt's
 *        virtual pitch (1974, 1979) and Parncutt's root support.
 *
 * This is a different question from
 * whether the tones are pairwise consonant, and the difference is not academic.
 *
 * The conductor scores a chord as the MEAN CONSONANCE OVER ALL PAIRS, and measured on the
 * instrument's own function that rule prefers a stack of fifths (4:6:9, mean 0.233) to a just
 * major triad (4:5:6, 0.212) -- and puts a plain segment of the harmonic series (8:9:10:11:12,
 * 0.165) last of all. Adjacent members of one series make complicated ratios pairwise (9/8,
 * 11/10) however perfectly the set as a whole fits together. For a dyad the two measures agree,
 * which is why this went unnoticed; for five voices they invert.
 *
 * The measure: try every fundamental that could hold the set -- the lowest tone divided by one
 * to sixteen -- assign each tone to its nearest harmonic, and score how cleanly it sits there,
 * weighted so that a tone on a low harmonic supports the root far more than one high up.
 *
 * Two rules keep the trivial answers out, and both were put there because the first version gave
 * them. A tone may not share a harmonic number with another: several tones crammed onto one
 * harmonic is a cluster, not a fit. And a candidate root supported by fewer than two tones does
 * not count at all, because every tone is the first harmonic of itself -- without that rule a
 * semitone cluster scored as high as a just major triad, and a bare tritone scored higher than
 * both.
 *
 * Read by both draws of the conductor when Harmonic is up, once per candidate note with the
 * chord it would make, so it is written to be cheap: sixteen roots by n tones, no allocation.
 *
 * @param freqs  the tones of the chord, Hz, in any order
 * @param n      how many; fewer than two have no root and score 0
 * @return       0 .. 1, the best root's mean fit per tone (about 1 for a low, clean harmonic
 *               segment, near 0 for a set no series can hold); 0 for a non-positive frequency
 */
inline double chordHarmonicity(const double* freqs, int n)
{
    if (n < 2) return 0.0;
    constexpr double kCents = 25.0;      // how far off a harmonic a tone may sit and still support the root
    constexpr int kMaxHarmonic = 32, kMaxSub = 16;
    double fmin = freqs[0];
    for (int i = 1; i < n; ++i) if (freqs[i] < fmin) fmin = freqs[i];
    if (!(fmin > 0.0)) return 0.0;
    double best = 0.0;
    for (int k = 1; k <= kMaxSub; ++k) {
        const double f0 = fmin / k;
        double s = 0.0;
        int landed = 0;
        unsigned int used = 0;           // a bit per harmonic number, 1..32
        for (int i = 0; i < n; ++i) {
            const int h = static_cast<int>(std::lround(freqs[i] / f0));
            if (h < 1 || h > kMaxHarmonic) continue;
            const unsigned int bit = 1u << (h - 1);
            if (used & bit) continue;
            const double cents = std::fabs(1200.0 * std::log2(freqs[i] / (h * f0)));
            const double fit = std::exp(-(cents / kCents) * (cents / kCents));
            if (fit < 0.05) continue;
            used |= bit;
            ++landed;
            s += fit / std::log2(1.0 + h);
        }
        if (landed < 2) continue;
        s /= n;
        if (s > best) best = s;
    }
    return best;
}

/**
 * @name The tonal hierarchy: how stable each degree of a key feels.
 *
 * Krumhansl and Kessler (1982) measured it. A listener hears a context that establishes a key,
 * then a single probe tone, and rates how well it fits; averaged over listeners the twelve
 * ratings are these two profiles. The tonic stands highest, then the fifth, then the third, then
 * the rest of the scale, then the notes outside it. It is not a rule anybody wrote down -- it is
 * what a set of listeners reported, and it has held up across four decades of replication.
 *
 * The conductor had no notion of a degree at all. It asked how a candidate sounded against what
 * was already sounding, which is a question about intervals, and never how it sat in a key.
 * @{ */
/**
 * @brief The Krumhansl-Kessler profile of a major key.
 * @return twelve ratings on a 1 .. 7 scale, index 0 the tonic and then ascending semitones; a
 *         static table, valid for ever
 */
inline const float* keyProfileMajor()
{
    static const float p[12] = { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
    return p;
}
/**
 * @brief The Krumhansl-Kessler profile of a minor key.
 * @return twelve ratings on a 1 .. 7 scale, index 0 the tonic and then ascending semitones (the
 *         minor third at 5.38 is what tells it from the major profile); a static table
 */
inline const float* keyProfileMinor()
{
    static const float p[12] = { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };
    return p;
}
/** @} */

/**
 * @brief The pitch class of a frequency, in twelve bins of a hundred cents from an arbitrary anchor.
 *
 * Arbitrary is fine: the key search tries all twelve rotations, so only the intervals matter.
 * Taken from the frequency rather than from the MIDI number because a scale in this instrument
 * need not have twelve degrees -- with consecutive degrees on a nine-tone scale, note % 12 means
 * nothing at all, while cents always mean cents.
 *
 * @param f  frequency in Hz; anything not positive lands in bin 0
 * @return   0 .. 11, bin 0 at middle C (261.63 Hz) and its octaves, rounded to the nearest semitone
 */
inline int pitchClassOf(double f)
{
    if (!(f > 0.0)) return 0;
    const int semis = static_cast<int>(std::lround(12.0 * std::log2(f / 261.6255653005986)));
    return ((semis % 12) + 12) % 12;
}

/**
 * @brief How evenly a chord's pitch classes are spread round the octave.
 *
 * Tymoczko (Science, 2006) showed that the chords which can be joined to their transpositions by
 * small voice movements are the nearly even ones -- and that those are, not by coincidence, the
 * chords Western music actually uses. Evenness is therefore not a taste: it is the property that
 * makes a chord able to MOVE. A cluster can only leap.
 *
 * One for a chord whose notes divide the octave equally, zero for one whose notes are all in the
 * same place. Duplicated pitch classes leave a gap of zero, which is exactly right: an octave
 * doubling adds nothing to how the chord is spread.
 *
 * @param freqs  the tones, Hz; non-positive ones are skipped, at most sixteen are counted
 * @param n      how many; fewer than two score 0
 * @return       0 .. 1 as described, with the pitch classes measured in fractional semitones
 */
inline double chordEvenness(const double* freqs, int n)
{
    if (n < 2) return 0.0;
    double pc[16];
    int m = 0;
    for (int i = 0; i < n && m < 16; ++i) {
        if (!(freqs[i] > 0.0)) continue;
        double x = std::fmod(12.0 * std::log2(freqs[i] / 261.6255653005986), 12.0);
        if (x < 0.0) x += 12.0;
        pc[m++] = x;
    }
    if (m < 2) return 0.0;
    for (int i = 1; i < m; ++i) {                 // insertion sort: m is at most sixteen
        const double v = pc[i];
        int j = i - 1;
        while (j >= 0 && pc[j] > v) { pc[j + 1] = pc[j]; --j; }
        pc[j + 1] = v;
    }
    const double ideal = 12.0 / m;
    double err = 0.0;
    for (int i = 0; i < m; ++i) {
        const double gap = (i + 1 < m) ? pc[i + 1] - pc[i] : 12.0 - pc[m - 1] + pc[0];
        err += std::fabs(gap - ideal);
    }
    const double worst = 24.0 * (1.0 - 1.0 / m);  // every note in one place
    return clampv(1.0 - err / worst, 0.0, 1.0);
}

/**
 * @brief The key the conductor hears, and how clearly: the answer of findKey().
 *
 * Returned by ClusterBrain::estimatedKey (and Engine::brainKey for the panel) and computed again
 * inside the draws whenever Key is up; the confidence is the correlation itself, so a passage
 * with no key in it reports as much and pulls at nothing (ClusterBrain::keyWeightOf).
 */
struct KeyEstimate {
    int   key = -1;            ///< 0..11 major, 12..23 minor, -1 for nothing heard yet
    float confidence = 0.0f;   ///< the correlation, -1..1
    /**
     * @brief Whether the key found is a minor one.
     * @return true for keys 12 .. 23; false for a major key and for none at all
     */
    bool  minor() const { return key >= 12; }
    /**
     * @brief The tonic of the key found, as a pitch class in pitchClassOf()'s bins.
     * @return 0 .. 11, or -1 while nothing has been heard
     */
    int   tonic() const { return key < 0 ? -1 : key % 12; }
};

/**
 * @brief Which key a distribution of sounding pitch classes is in: the Krumhansl-Schmuckler method.
 *
 * Correlate the distribution against all twenty-four rotated profiles and take the best. The
 * correlation itself is the confidence, and it is worth having: an honest "this is barely a key
 * at all" is exactly what a cluster should report.
 *
 * @param weights  twelve weights, one per pitch class in pitchClassOf()'s bins -- the conductor's
 *                 faded histogram of how long each class has been sounding
 * @return         the best of the twenty-four keys with its correlation; key -1 and confidence 0
 *                 for silence, or for twelve classes in perfect balance
 */
inline KeyEstimate findKey(const float* weights)
{
    double mean = 0.0;
    for (int i = 0; i < 12; ++i) mean += weights[i];
    mean /= 12.0;
    double var = 0.0;
    for (int i = 0; i < 12; ++i) var += (weights[i] - mean) * (weights[i] - mean);
    KeyEstimate out;
    if (var < 1.0e-9) return out;                 // silence, or twelve notes in perfect balance
    const double sd = std::sqrt(var);
    for (int k = 0; k < 24; ++k) {
        const float* prof = k < 12 ? keyProfileMajor() : keyProfileMinor();
        const int rot = k % 12;
        double pm = 0.0;
        for (int i = 0; i < 12; ++i) pm += prof[i];
        pm /= 12.0;
        double pv = 0.0, cov = 0.0;
        for (int i = 0; i < 12; ++i) {
            const double a = weights[(i + rot) % 12] - mean;
            const double b = prof[i] - pm;
            cov += a * b;
            pv += b * b;
        }
        const double r = cov / (sd * std::sqrt(pv) + 1.0e-12);
        if (out.key < 0 || r > out.confidence) { out.key = k; out.confidence = static_cast<float>(r); }
    }
    return out;
}

/**
 * @brief Everything the conductor is told: the knobs of the Cluster Brain and Brain 2 panels, as one
 *        struct handed to ClusterBrain::update on every tick.
 *
 * The engine keeps one per conductor (bp_ and bp2_), fills it from the parameters on the audio
 * thread (EngineControl.cpp) and passes it by reference; the conductor keeps nothing of it
 * between ticks, so a knob turned is a knob heard at the next decision. Every knob added after the
 * first version is an identity at its default -- the comments say so knob by knob -- because the
 * library's presets were rendered and measured before those knobs existed and must render as they
 * did, down to the random stream. The three small helpers at the end (erbAt, crowding,
 * consonanceOf) are the parts of the scoring that depend on the knobs alone, kept here so that
 * both draws of the conductor ask the same question.
 */
struct BrainParams {
    bool  on = true;              ///< the conductor runs; switched off, update() releases every voice once and then does nothing
    BrainMode mode = BrainMode::Free;   ///< Free or Chords, see BrainMode
    float voiceLead = 7.0f;       ///< Chords: how far the exchanged voice may move, in semitones
    float chordTension = 0.0f;    ///< 0 = the new note must fit the ones that stay; 1 = anything goes
    float rootMove = 0.0f;        ///< Chords: how often the exchange moves the root as well
    int   density = 5;            ///< target number of simultaneous notes
    float rateSeconds = 25.0f;    ///< mean time between events
    /** @brief Hold Min and Hold Max: the shortest and the longest hold a note may draw, in seconds. */
    float holdMin = 30.0f, holdMax = 120.0f;   ///< seconds a note is held in Free mode, drawn between the two (the draw shaped by Spread and Bias, the result scaled by the register role)
    /** @brief Lowest and Highest: the register the conductor may choose from, either order. */
    int   low = 36, high = 79;    ///< MIDI range for chosen notes
    float consonance = 0.7f;      ///< 0 = anything goes (clusters), 1 = strictly consonant
    float wander = 0.3f;          ///< probability weight for root movement
    /**
     * @brief Timbre: how much of the consonance is judged from the actual spectrum (Sethares) rather
     * than from the ratio alone.
     *
     * 0 is the ratio score the conductor always had.
     */
    float timbre = 0.0f;
    /**
     * @brief Even: how strongly the conductor prefers chords whose notes are spread evenly round the
     * octave.
     *
     * Those are the chords that can be joined to their neighbours by small movements
     * rather than leaps (Tymoczko 2006), which is what lets a harmony go somewhere at all.
     * Against Harmonic, which pulls towards low harmonics and octaves, this pulls apart; the two
     * are meant to be set against each other.
     */
    float even = 0.0f;
    /**
     * @brief Smooth: which voice moves.
     *
     * The conductor retires the one that has been sounding longest and
     * then looks for its replacement; with this up it tries every voice and keeps the exchange
     * that moves the chord the shortest distance -- the voice-leading distance of Tymoczko's
     * geometry, which for an exchange of one note is exactly the leap that voice makes. In Free
     * mode it reads as the step size of a wandering voice instead: a candidate is weighted by how
     * far it stands from the note chosen before it, so the conductor steps oftener than it leaps.
     */
    float smooth = 0.0f;
    /**
     * @brief Blend: how a chord arrives.
     *
     * Rasch (1979) measured the onset asynchrony of ensembles at
     * thirty to fifty milliseconds, and Bregman's rule is that tones starting together are heard
     * as one object while tones starting apart are heard as separate voices. The conductor
     * brings its voices in one at a time, minutes apart, so every voice is its own object. With
     * this up, the notes that fill an empty chord arrive TOGETHER -- within thirty milliseconds
     * at the top, fused into one sound -- rather than one per tick.
     */
    float blend = 0.0f;
    /**
     * @brief Cascade: events that cause events.
     *
     * The conductor's clock is a Poisson process -- every
     * gap drawn afresh, nothing remembered -- which is the most even a random clock can be, and
     * nothing in nature is that even: a gust brings gusts, a crack in cooling wood brings more.
     * A Hawkes process (Hawkes 1971) is the model of that: each event lifts the rate by a jump
     * that decays away, so events come in clusters that are caused, not scheduled. The jump is
     * sized so that at full an event breeds 0.65 further events on average (the branching
     * ratio; above 1 the process explodes), which makes the clock fire about three times as
     * often as its base rate. 0 is the clock as it always was.
     */
    float cascade = 0.0f;
    /**
     * @name Surprise and Homeostat
     *
     * Surprise and Homeostat: how predictable the music is allowed to become. Predictive-coding
     * accounts of music (Vuust, Koelsch) put listening between two failures: when nothing is
     * ever surprising the ear stops attending, and when everything is it gives up. The
     * conductor keeps a fading histogram of the intervals it has chosen and measures its
     * entropy in bits; Surprise is the entropy it aims for (0 a machine repeating itself, 1 as
     * unpredictable as twelve pitch classes can be), and Homeostat is how hard it leans towards
     * that aim -- when the music has become too predictable the draw is flattened so unlikely
     * notes get their turn, when it has become too random the draw is sharpened towards the
     * best-fitting notes. At Homeostat 0 nothing leans and the conductor is as it always was.
     * @{ */
    float surprise = 0.5f;    ///< the interval entropy aimed for, 0 .. 1 of the log2(12) bits twelve classes allow
    float homeostat = 0.0f;   ///< how hard the draw leans towards that aim, 0 .. 1; 0 is off
    /** @} */
    /**
     * @name Deja Vu and Loop
     *
     * Deja Vu and Loop: Mutable Instruments' Marbles, in the conductor. A ring of Loop places
     * holds the last notes chosen; each new choice moves one place round it, and with
     * probability Deja Vu the note already there is played again instead of the fresh one --
     * which then does not overwrite it. At 1 the loop plays round and round; below it, new
     * notes seep into the loop at a rate the knob sets, so a figure comes back and slowly
     * mutates. A note the ring offers that is still sounding is passed over for the fresh one.
     * 0 draws nothing from the random stream: the conductor is as it always was.
     * @{ */
    float dejavu = 0.0f;   ///< probability, 0 .. 1, that the ring's note is played instead of the fresh one
    int   loop = 8;        ///< places in the ring, 1 .. ClusterBrain::kRing (16): the length of the figure that can come back
    /** @} */
    /**
     * @name Spread and Bias
     *
     * Spread and Bias: the shape of the conductor's random draws (velocity, how long a note
     * holds), after Marbles again. Spread at 0.5 leaves the draw uniform; towards 0 it gathers
     * round the middle, towards 1 it is pushed to the extremes -- a bimodal draw, so notes are
     * soft or loud and short or long, seldom in between. Bias moves the centre: positive draws
     * higher, negative lower. Both are identities at their defaults.
     * @{ */
    float spread = 0.5f;   ///< 0 .. 1, 0.5 uniform, below gathers round the middle, above pushes to the extremes
    float bias = 0.0f;     ///< -1 .. 1, moves the centre of the draw up (positive) or down; 0 leaves it
    /** @} */
    /**
     * @brief The draw, shaped.
     *
     * Written so that the default returns u untouched, bit for bit.
     *
     * @param u  a uniform draw in [0, 1) from the conductor's stream
     * @return   the draw pushed by Spread towards the middle or the extremes and by Bias up or
     *           down, still in 0 .. 1
     */
    float shaped(float u) const
    {
        if (spread == 0.5f && bias == 0.0f) return u;
        float v = u - 0.5f;
        const float a = std::fabs(2.0f * v);
        const float k = spread < 0.5f ? 1.0f + (0.5f - spread) * 6.0f : 1.0f / (1.0f + (spread - 0.5f) * 6.0f);
        v = (v < 0.0f ? -0.5f : 0.5f) * std::pow(a, k);
        float w = clampv(v + 0.5f, 0.0f, 1.0f);
        if (bias != 0.0f) w = std::pow(w, std::pow(2.0f, -2.0f * bias));
        return clampv(w, 0.0f, 1.0f);
    }
    /**
     * @brief Key: how strongly the conductor prefers the stable degrees of the key it finds itself in.
     *
     * Nothing sets that key -- it is measured from what has actually been sounding, weighted by
     * how long, which for an instrument whose notes last minutes is the only weighting that means
     * anything. Turn it up and the music acquires a home it keeps returning to; leave it at zero
     * and the conductor hears intervals and no key at all, as it always did.
     */
    float key = 0.0f;
    /**
     * @brief Harmonic: how much the choice is judged by how well the WHOLE resulting chord fits one
     * harmonic series, rather than only by how its pairs sound.
     *
     * Listeners' preferences track
     * harmonicity at least as strongly as they track the absence of beating (McDermott, Lehr and
     * Oxenham 2010), and the two models are combined in the current accounts (Harrison and Pearce
     * 2020). 0 is the pairwise judgement the conductor always had.
     */
    float harmonic = 0.0f;
    /**
     * @brief Spacing: how strongly the conductor avoids putting a note within a critical band of one
     * that is already sounding.
     *
     * Two tones closer than about an equivalent rectangular bandwidth
     * excite overlapping regions of the cochlea, and the ear fuses them into one rough sound
     * instead of hearing two (Glasberg and Moore 1990; Bregman 1990). Negative values seek that
     * crowding out instead, which is what a cluster is. 0 leaves the choice as it was.
     */
    float spacing = 0.0f;
    // ---- the register roles, and the intervals that belong to them (12.09.2026)
    /**
     * @name The register roles, and the intervals that belong to them
     *
     * A conductor that draws a note from one weighted list treats the bottom of its register like
     * the top, and the ear does not: two tones a third apart are a chord at C4 and mud at C2,
     * because the critical band is a fixed fraction of the frequency and therefore an enormous
     * interval down there. These say so. All of them do nothing at their defaults.
     * @{ */
    float layers = 0.0f;      ///< 0: the register weight as it was. 1: a voice at the bottom, most
                              ///< in the body, few on top -- and at most two notes to an octave, one
                              ///< below MIDI 36.
    float bassHold = 1.0f;    ///< multiplies the hold of the lowest sounding voice: the foundation
                              ///< lies while what stands on it moves.
    float topSoft = 0.0f;     ///< velocity falls with height: at 1 the top of the register arrives
                              ///< at half the velocity of the bottom.
    float lowSpacing = 0.0f;  ///< the minimum interval grows as the register falls: an octave below
                              ///< MIDI 36, a fifth to 47, a minor third to 59, a second to 71.
    int   thirdFloor = 0;     ///< no third is chosen below this note (0: off). A third in the bass
                              ///< is the single most reliable way to make a drone muddy.
    float leading = 0.0f;     ///< penalises the semitone under a sounding root: it pulls somewhere,
                              ///< and this music has nowhere to go.
    float thirds = 0.0f;      ///< -1 avoids thirds, +1 seeks them. Judged against every sounding
                              ///< note, not against the root alone -- a tone that is a fifth to the
                              ///< bass and a second to a middle voice IS a second.
    float seconds = 0.0f;     ///< the same knob for seconds and ninths, judged against every sounding note as
                              ///< the thirds are: -1 avoids them (the minor second and the major seventh at full
                              ///< weight, the major second at a third of it), +1 seeks them, which counts only
                              ///< above the third floor and MIDI 60 -- a second is a colour up there and mud below.
    float seventh = 0.0f;     ///< lifts the minor seventh, which in a just scale is 7/4: the one
                              ///< interval that fuses with the root instead of pressing against it.
    /** @} */
    // ---- what the clock may and may not do (12.09.2026)
    /**
     * @name What the clock may and may not do
     *
     * The guards on the conductor's timing (rule book sections 5 and 7): what may begin or end
     * when, and how a change is spread over time. All of them do nothing at their defaults.
     * @{ */
    float rateBreath = 0.0f;    ///< the mean gap breathes between half and double on its own curve
    float breathPeriod = 10.0f; ///< minutes for one breath
    float overlap = 0.0f;       ///< seconds a voice keeps sounding after the one replacing it began
    bool  onsetGuard = false;   ///< two onsets are either inside 30 ms or at least 3 s apart
    float releaseGap = 0.0f;    ///< seconds between two note-offs
    float retrigger = 0.0f;     ///< seconds a pitch must rest before it may be chosen again
    float densitySlew = 0.0f;   ///< minutes a change of density is spread over, one voice at a time
    float silence = 0.0f;       ///< chance of an empty pause at a root change
    float silenceLen = 20.0f;   ///< its length in seconds, give or take a third
    /** @} */
    // ---- root and long form (12.09.2026)
    /**
     * @name Root and long form
     *
     * Where the root may go, how a change is announced, and the pull back home over the hours
     * (rule book section 6). All of them do nothing at their defaults.
     * @{ */
    int   rootSteps = 0;        ///< 0 Any, 1 Fifths, 2 Diatonic (R6.2 priorities), 3 Falling
    float rootDown = 0.0f;      ///< the lean of the root's step: + leans the move downwards, - upwards
    float pivot = 0.0f;         ///< seconds of the changeover window: common tone, new root, old goes
    float home = 0.0f;          ///< pull back towards the root the night began on
    float homeTime = 60.0f;     ///< minutes after which that pull is at its strongest
    float memory = 0.0f;        ///< minutes in which a chord already heard may not return
    float degreeSwap = 0.0f;    ///< chance a root change also exchanges one degree of the supply
    /** @} */
    const BrainSpectrum* spectrum = nullptr;   ///< the voice's partial template for Timbre, owned by the engine (brainSpec_); null or empty means the ratio score alone
    /**
     * @brief The equivalent rectangular bandwidth of the auditory filter at f, in hertz
     * (Glasberg and Moore 1990): ERB = 24.7 (0.00437 f + 1).
     * @param f  centre frequency, Hz
     * @return   the bandwidth in Hz (about 25 Hz at the bottom, 130 Hz at 1 kHz)
     */
    static double erbAt(double f) { return 24.7 * (0.00437 * f + 1.0); }
    /**
     * @brief How much a candidate at fa is discouraged by a sounding tone at fb. 1 leaves it alone.
     * @param fa  the candidate's frequency, Hz
     * @param fb  the sounding tone's frequency, Hz
     * @return    a weight to multiply the candidate's by: below 1 (down to 0.05 at the same pitch
     *            and Spacing 1) when Spacing is positive, above 1 when it is negative, exactly 1
     *            at Spacing 0 or more than about one ERB apart
     */
    float crowding(double fa, double fb) const
    {
        if (spacing == 0.0f) return 1.0f;
        const double d = std::fabs(fa - fb) / erbAt(0.5 * (fa + fb));   // in critical bandwidths
        const double closeness = std::exp(-d * d * 2.0);                // 1 at the same pitch, gone past one ERB
        const double s = static_cast<double>(spacing);
        return static_cast<float>(s > 0.0 ? (1.0 - 0.95 * s * closeness) : (1.0 - s * closeness));
    }

    /**
     * @brief The consonance of two frequencies, as this conductor currently hears it.
     *
     * The ratio score of intervalConsonance() blended with spectralConsonance() over the voice's
     * template by Timbre; with Timbre at 0 or no template the ratio score alone, as it always was.
     *
     * @param fa  the candidate, Hz
     * @param fb  the tone it is judged against (the root, or a sounding voice), Hz
     * @return    0 .. 1, 1 for a unison, about a tenth for a semitone on either scale
     */
    double consonanceOf(double fa, double fb) const
    {
        const double byRatio = intervalConsonance(fa / fb);
        if (timbre <= 0.0f || spectrum == nullptr || spectrum->count <= 0) return byRatio;
        const double bySpectrum = spectralConsonance(fb, fa, *spectrum);
        return byRatio + (bySpectrum - byRatio) * static_cast<double>(timbre);
    }
};

/**
 * @brief One decision of the conductor, handed to the engine's emit callback: a note begins or ends.
 *
 * The engine's lambdas (Engine::process for the ticks, Engine::adoptCluster for a crossfade) turn
 * a NoteOn into startNote with the conductor's velocity and the engine's own choice of depth, and
 * a NoteOff into stopNote, which begins the voice's long release. The note is a MIDI number the
 * engine's freqOf maps into the current scale, so a scale of nine degrees is addressed with
 * consecutive numbers like any other.
 */
struct BrainEvent {
    /** @brief What happened: NoteOn -- the note begins with the event's velocity; NoteOff -- it is let go into its release. */
    enum class Type { NoteOn, NoteOff };
    Type  type;       ///< which of the two
    int   note;       ///< MIDI note number, 0 .. 127, within the conductor's range
    float velocity;   ///< 0 .. 1 for a NoteOn -- nearness rather than loudness (see BrainParams::topSoft); 0 for a NoteOff
};
/** @var BrainEvent::Type::NoteOn
 *  @brief The note begins, with the event's velocity. */
/** @var BrainEvent::Type::NoteOff
 *  @brief The note is let go; velocity is 0. */

/**
 * @brief The conductor itself: one cluster of up to kSlots notes, its clock, its root and its memory.
 *
 * The engine owns two (Engine::brain_ for the foreground cluster, brain2_ for the background one
 * on the first's root plus an interval) and drives each from Engine::process once per control
 * block through update(), on the audio thread; everything else here is either a read-out for the
 * panel and the tests or a request that update() honours at its next tick (requestStep,
 * requestFill, holdOnsets, holdRoot, setRoot). The one exception is the crossfade between two
 * engines, where soundingNotes() and adopt() run on the message thread while the incoming engine
 * is being prepared and has not rendered a block yet.
 *
 * Nothing is allocated: the slots, the rings and the memory are fixed arrays, and the random
 * stream is the conductor's own Rng, seeded by reset(), so a set rendered twice from the same
 * seed is the same set. That is why so many of the comments below insist that a knob at nought
 * "draws nothing from the stream": every feature added after the first version is
 * short-circuited so that the presets which never asked for it render bit for bit as they did.
 *
 * Two modes (BrainMode). In Free mode every note has a hold drawn from Hold Min to Hold Max and
 * leaves when it expires; the clock's decisions add a note while the cluster is under its
 * density and retire one half of the time when it is full. In Chords mode (updateChords) the
 * cluster is kept full and every decision exchanges one voice for the best note chooseNote()
 * finds. Both modes share the clock (tickSeconds, gapDraw, advanceTimer), the root's wandering
 * (wanderRoot), the register and interval rules (ruleWeight), the constellation memory
 * (chordKey, rememberChord, constellationHeard) and the guards of the rule book of 12.09.2026,
 * whose sections the comments cite as R6.1, Anti 2, G3 and table 2.
 */
class ClusterBrain {
public:
    static constexpr int kSlots = 12;   ///< the most notes a cluster holds, and the length the arrays of soundingNotes() and adopt() must have

    /**
     * @brief Puts the conductor back to silence with a fresh random stream and a root.
     *
     * Called by the engine at prepare, when the seed changes and when the tuning's root pitch
     * class moves (Engine.cpp, EngineControl.cpp); the second conductor is reset with a stream of
     * its own so that switching it on never moves the first. Emits nothing -- the engine has
     * already silenced or never started the voices. The guard clocks are set so that the first
     * note may begin at once, every pitch counts as long rested, and the exposition (settled_)
     * starts over.
     *
     * @param seed      seed of the conductor's own Rng, usually forked from the engine's
     * @param rootNote  MIDI note the root and the home root begin on (the engine passes its key
     *                  root an octave down, or 48 plus the pitch class)
     */
    void reset(uint64_t seed, int rootNote)
    {
        rng_.seed(seed);
        stepRequested_ = false;
        for (int& r : recent_) r = -1;
        recentHead_ = 0;
        for (auto& s : slots_) { s.note = -1; s.remaining = 0.0; s.startIn = 0.0; }
        for (float& w : pcWeight_) w = 0.0f;
        root_ = rootNote;
        timer_ = 1.0;
        wasOn_ = false;
        excite_ = 0.0;
        lastNote_ = -1;
        for (int& r : ring_) r = -1;
        ringPos_ = 0;
        lastLean_ = 0.0f;
        for (float& w : ic_) w = 0.0f;
        homeRoot_ = rootNote;
        age_ = 0.0;
        rootAge_ = 0.0;
        pivotLeft_ = 0.0;
        pivotTo_ = -1;
        pivotOld_ = -1;
        silenceLeft_ = 0.0;
        densityNow_ = -1.0;
        densityTarget_ = -1.0;
        sinceOn_ = sinceOff_ = 1.0e9;
        now_ = 1.0e6;
        for (double& t : pitchOffAt_) t = 0.0;
        for (float& b : degreeBias_) b = 1.0f;
        for (auto& m : memo_) { m.key = 0; m.at = -1.0e9; }
        memoHead_ = 0;
        breathPhase_ = 0.0;
        breathValue_ = 0.0f;
        for (auto& l : leaving_) l = Leaving{};
        pivotTones_ = 0;
        rootMoves_ = 0;
        settled_ = false;
    }

    /**
     * @brief Minutes since the last root change, for the matrix source Root Age.
     * @return the age in seconds, as the name says -- the matrix source divides it by Home Time in
     *         minutes (EngineControl.cpp), so an hour-long night and a three-hour one read the same
     */
    double rootAgeSeconds() const { return rootAge_; }
    /**
     * @brief A planned silence (R5.6) is running: the foreground keeps out of it too.
     * @return true while the pause begun at a root change has seconds left
     */
    bool inSilence() const { return silenceLeft_ > 0.0; }
    /**
     * @brief Seconds since the conductor last began a note.
     * @return the onset clock the guards read; huge after reset(), before anything has sounded
     */
    double sinceOnset() const { return sinceOn_; }
    /**
     * @name The foreground's asks
     *
     * The foreground's two asks of the background (13.09.2026). Hold Onsets: no new note begins,
     * but the holds run on, the releases happen, the last voice waits for its replacement as it
     * always does -- a soloist asks the room to stop moving, not to stop breathing. (Stopping the
     * clock outright was tried first: a conductor that stands still for six seconds is heard as a
     * pause button, and it broke the overlap rule.) Hold Root: the root does not move, so a line
     * that transposes with it is not thrown across a changeover.
     *
     * Both are set every control block by the engine's near layer (Engine.h, from stepNear) for
     * both conductors, and read by update() at its next tick.
     * @{ */
    /**
     * @brief Hold Onsets: while set, update() begins no new note (a Chords-mode step asked for by hand still goes through).
     * @param h  true while a near Note or a Phrase is speaking
     */
    void holdOnsets(bool h) { onsetHold_ = h; }
    /**
     * @brief Hold Root: while set, the root does not wander (an anchor note from the keyboard still sets it).
     * @param h  true while a sequence is running
     */
    void holdRoot(bool h)   { rootHold_ = h; }
    /** @} */

    /**
     * @brief What key the conductor finds itself in, and how sure it is.
     *
     * Measured, never set: the
     * histogram below is what has actually been sounding, weighted by how long.
     *
     * @return findKey() over pitchClassWeights(); key -1 while nothing has sounded yet
     */
    KeyEstimate estimatedKey() const { return findKey(pcWeight_); }
    /**
     * @brief The histogram estimatedKey() reads: how long each pitch class has been sounding, faded
     *        over about three minutes (pcWeight_).
     * @return twelve weights in faded seconds, in pitchClassOf()'s bins; the pointer stays valid
     *         for the conductor's lifetime and the values change at every update()
     */
    const float* pitchClassWeights() const { return pcWeight_; }

    /**
     * @brief The root the cluster is built around.
     * @return its MIDI note, 0 .. 127; the engine reads it for the second conductor's root, the
     *         body's pitch and the panel
     */
    int  root() const { return root_; }
    /**
     * @brief Moves the root without a changeover: the engine's tuning root changed, or the second
     *        conductor follows the first plus its interval.
     *
     * Unlike the private setRoot(int, const BrainParams&) it neither resets the root's age nor
     * swaps a degree; it is bookkeeping, not a decision.
     * @param note  the new root, clamped to 0 .. 127
     */
    void setRoot(int note) { root_ = clampv(note, 0, 127); }
    /**
     * @brief How many slots hold a note, sounding or about to (Blend), not counting voices sounding out their overlap.
     * @return 0 .. kSlots
     */
    int  activeCount() const { int c = 0; for (auto& s : slots_) if (s.note >= 0) ++c; return c; }
    /**
     * @brief Whether a note is in the cluster: in a slot, or exchanged and still sounding out its overlap.
     * @param note  MIDI note
     * @return true while the engine has a voice on it that the conductor started
     */
    bool sounding(int note) const
    {
        for (auto& s : slots_) if (s.note == note) return true;
        for (auto& l : leaving_) if (l.note == note) return true;   // exchanged, still sounding out its overlap
        return false;
    }
    /**
     * @brief For the tests: how often the root moved, and how many of those changeovers were announced
     * by a tone belonging to both roots.
     *
     * Counting is the only honest answer to "does it happen?".
     * @return root changes made by wanderRoot() since reset(), announced or not
     */
    int rootMoves() const  { return rootMoves_; }
    /**
     * @brief How many of those root changes found a pivot tone -- one belonging to both roots -- to
     *        announce themselves with (for the tests; the self test wants four in five).
     * @return the count since reset(), at most rootMoves()
     */
    int pivotTones() const { return pivotTones_; }

    /**
     * @brief Ask for one exchange at the next opportunity, whatever the timer says: the button on the
     * panel, a mapped controller, a footswitch.
     *
     * Read and cleared inside update().
     *
     * It is Chords mode that acts on it (updateChords): the exchange happens at the next tick,
     * through Hold Onsets as well. The Free-mode draw does not read the flag. The engine sets it
     * from the Autoplay step (EngineControl.cpp).
     */
    void requestStep() { stepRequested_ = true; }
    /**
     * @brief Whether a step was asked for and not yet taken.
     * @return true between requestStep() and the tick that acts on it
     */
    bool stepPending() const { return stepRequested_; }

    /**
     * @brief What the conductor is holding, so another one can take it over.
     *
     * Writes at most
     * ClusterBrain::kSlots notes -- twelve, NOT the four of ambient::kSlots, which is the number of
     * source slots and the trap that stands next to this one: both arrays must be twelve long, or
     * the conductor writes past their end. Returns how many; a slot that has been chosen but has
     * not started yet counts, because it is about to sound.
     *
     * Message thread, from Engine::soundingCluster at the start of a crossfade; adopt() on the
     * arriving engine is its counterpart.
     *
     * @param notes  receives the MIDI notes, at least kSlots long
     * @param vels   receives their velocities, at least kSlots long
     * @return       how many were written, 0 .. kSlots
     */
    int soundingNotes(int* notes, float* vels) const
    {
        int n = 0;
        for (const auto& s : slots_)
            if (s.note >= 0 && n < kSlots) { notes[n] = s.note; vels[n] = s.vel; ++n; }
        return n;
    }

    /**
     * @brief Take a cluster over from another conductor and carry on from there.
     *
     * A crossfade is meant to change the instrument, not the music. Left to itself the arriving
     * conductor picks its own notes, so what the listener heard was one chord fading out under a
     * different chord fading in -- two pieces of music at once for the length of the fade. Here it
     * inherits the chord instead and goes on with it: what is out of its range or too dense it
     * lets go of over the next few events, the way it would treat any cluster it was given, and
     * what is missing it adds at its own pace. Nothing is forced.
     *
     * Whatever this conductor held is released first (a NoteOff for each), then every inherited
     * note is placed in a slot with a hold of nought -- due at the next tick, so Free mode lets it
     * go through the ordinary path when it does not fit -- and emitted as a NoteOn. Message thread,
     * from Engine::adoptCluster, before the engine has rendered a block; the engine asks for a
     * fill afterwards (requestFill), because this clears the flag.
     *
     * @tparam EmitFn  callable `void(const BrainEvent&)`
     * @param notes  the inherited MIDI notes, as soundingNotes() wrote them
     * @param vels   their velocities
     * @param count  how many; at most kSlots are taken
     * @param emit   receives the NoteOffs of what was here and the NoteOns of what arrives
     */
    template <class EmitFn>
    void adopt(const int* notes, const float* vels, int count, EmitFn&& emit)
    {
        for (auto& s : slots_) { if (s.note >= 0) emit(BrainEvent{ BrainEvent::Type::NoteOff, s.note, 0.0f }); s = Slot{}; }
        for (auto& l : leaving_) { if (l.note >= 0) emit(BrainEvent{ BrainEvent::Type::NoteOff, l.note, 0.0f }); l = Leaving{}; }
        for (int i = 0; i < count && i < kSlots; ++i) {
            slots_[i].note = notes[i];
            slots_[i].vel = vels[i];
            slots_[i].remaining = 0.0;
            slots_[i].startIn = 0.0;
            remember(notes[i]);
            emit(BrainEvent{ BrainEvent::Type::NoteOn, notes[i], vels[i] });
        }
        if (count > 0) sinceOn_ = 0.0;
        filling_ = false;
    }

    /**
     * @brief Fill the cluster now rather than at the event rate.
     *
     * An empty chord grows by one note per tick, which is an entrance when the instrument starts
     * cold and is the right thing there. After a crossfade it is not: the preset that is leaving
     * was sounding a full cluster, and the one arriving is heard to fail rather than to enter.
     * With the library's slower conductors the wait is not a few bars. Measured on Interior Bloom,
     * which asks for six voices at an event every 98.8 seconds: two voices after one minute, three
     * after three, four after seven.
     *
     * While this is set the tick is a third of a second instead, so the chord walks in over a
     * second or two rather than landing as a block; it clears itself once the cluster is full.
     * Nothing else changes -- the notes are chosen the way they always are, by the same draw.
     *
     * Also what the conductor asks of itself when a planned silence ends. The standing wait is
     * cancelled so the first note comes at the next tick.
     */
    void requestFill() { filling_ = true; timer_ = 0.0; }
    /**
     * @brief Whether the cluster is being filled at speed.
     * @return true from requestFill() until the cluster once reaches its density
     */
    bool filling() const { return filling_; }
    /**
     * @name Read-outs for the panel and the tests
     *
     * How much the homeostat is leaning right now (+ towards more surprise, - towards less), the
     * entropy of the recent interval choices in bits, and the cascade's excitation in multiples
     * of the base rate. For the panel and the tests.
     * @{ */
    /**
     * @brief The homeostat's lean as of the last decision.
     * @return -1 .. 1 times Homeostat: positive flattens the draw towards surprise, negative
     *         sharpens it; 0 with Homeostat off or too little history
     */
    float lean() const { return lastLean_; }
    /**
     * @brief The entropy of the faded interval histogram the homeostat reads.
     * @return bits, 0 for a conductor repeating one interval up to log2(12) = 3.58 for twelve
     *         classes in balance; 0 before any interval was made
     */
    float entropyBits() const
    {
        float total = 0.0f;
        for (float w : ic_) total += w;
        if (total <= 0.0f) return 0.0f;
        double h = 0.0;
        for (float w : ic_) if (w > 0.0f) { const double pr = w / total; h -= pr * std::log2(pr); }
        return static_cast<float>(h);
    }
    /**
     * @brief The Hawkes clock's excitation.
     * @return how much faster than its base rate the timer runs, 0 at rest; the engine turns it
     *         into a matrix source and the sources' activity read it
     */
    double excitation() const { return excite_; }
    /**
     * @brief One place of the deja-vu ring, for the panel's picture of the loop.
     * @param i  place, 0 .. kRing - 1
     * @return   the MIDI note held there, or -1 for an empty place or an index out of range
     */
    int ringNote(int i) const { return (i >= 0 && i < kRing) ? ring_[i] : -1; }
    /**
     * @brief Where the deja-vu ring stands.
     * @return the place the last choice was written to, 0 .. Loop - 1
     */
    int ringPos() const { return ringPos_; }
    /** @} */

    /**
     * @brief Advance `dt` seconds.
     *
     * `freqOf(int note) -> double`, `emit(const BrainEvent&)`.
     * `anchorNote` (>= 0) pins the root to a note the player holds on the keyboard.
     *
     * One tick of the conductor, from Engine::process once per control block on the audio thread
     * (under Quantize once per note value, with the accumulated time). In order: a conductor
     * switched off releases everything it holds, once; an anchor that moves the root may begin a
     * planned silence; the density glide, a changeover in progress and the guard clocks advance;
     * notes held back by Blend are released as their few milliseconds run out; the key histogram
     * fades and grows; then the mode's own decision -- updateChords() in Chords mode, else, below,
     * the hold expiries with their two guards, the memoryless clock with its floor and its guards,
     * the retirement of a voice when the room is full, the weighted draw with the root's wander
     * inside it, the constellation memory, the deja-vu ring, and Blend's extra notes.
     *
     * @tparam FreqFn  callable `double(int note)`: the frequency of a MIDI note in the engine's current scale
     * @tparam EmitFn  callable `void(const BrainEvent&)`: receives every NoteOn and NoteOff
     * @param dt          seconds since the last tick: a control block, or the wait Quantize accumulated
     * @param p           the knobs as the engine filled them for this block
     * @param anchorNote  the lowest MIDI note held on the keyboard, or -1; while held it is the root
     *                    and the root does not wander
     * @param freqOf      see FreqFn
     * @param emit        see EmitFn
     */
    template <class FreqFn, class EmitFn>
    void update(double dt, const BrainParams& p, int anchorNote, FreqFn&& freqOf, EmitFn&& emit)
    {
        if (!p.on) {
            if (wasOn_) {
                for (auto& s : slots_) if (s.note >= 0) { emit(BrainEvent{ BrainEvent::Type::NoteOff, s.note, 0.0f }); s.note = -1; }
                wasOn_ = false;
            }
            return;
        }
        if (!wasOn_) { wasOn_ = true; timer_ = 0.5; }

        // A planned silence (R5.6). At a root change -- and only there -- the conductor may let
        // everything go and leave the room empty for a few seconds. This is the one exception to
        // "nothing ends in the void": at a section boundary an empty stretch is a breath, anywhere
        // else it is a dropout. The voices are not cut together, which would be Anti 1; each is
        // given a short rest of its own and the ordinary note-off path lets it go, Release Gap and
        // all, so the room empties from one end rather than at a stroke.
        if (anchorNote >= 0 && anchorNote != root_ && p.silence > 0.0f && silenceLeft_ <= 0.0
            && rng_.uniform() < p.silence) {
            silenceLeft_ = static_cast<double>(p.silenceLen) * (0.667 + 0.667 * static_cast<double>(rng_.uniform()));
            int n = 0;
            for (auto& s : slots_)
                if (s.note >= 0) s.remaining = std::min(s.remaining, 0.5 + 1.5 * static_cast<double>(rng_.uniform()) + 0.7 * n++);
        }
        if (anchorNote >= 0) root_ = anchorNote;

        // Density glides (Anti 9). A change of density -- by hand, by a macro, by the Arc -- is
        // carried out one voice at a time over Density Slew minutes instead of at once: five voices
        // appearing together is an edit, not a piece of music. The first tick takes the value as it
        // stands, so nothing ramps in from nowhere when the instrument starts.
        {
            const double tgt = static_cast<double>(clampv(p.density, 1, kSlots));
            if (densityNow_ < 0.0 || p.densitySlew <= 0.0f) { densityNow_ = tgt; densityTarget_ = tgt; }
            else {
                if (tgt != densityTarget_) {
                    densityTarget_ = tgt;
                    densityStep_ = std::fabs(tgt - densityNow_) / std::max(1.0, static_cast<double>(p.densitySlew) * 60.0);
                }
                const double d = densityTarget_ - densityNow_, s = densityStep_ * dt;
                densityNow_ += (std::fabs(d) <= s) ? d : (d > 0.0 ? s : -s);
            }
        }

        // A changeover in progress (R6.3). Three things in order: a tone that belongs to both roots
        // begins now, the root itself moves at the middle of the window, and only at its end is
        // whatever still sounds on the old root asked to go. A listener hears the harmony turn
        // rather than switch, which is the whole of the difference.
        if (pivotLeft_ > 0.0) {
            if (!pivotAnnounced_ && pivotOld_ >= 0 && pivotTo_ >= 0) {
                pivotAnnounced_ = true;
                const int lo = std::min(p.low, p.high), hi = std::max(p.low, p.high);
                const int span = std::max(1, hi - lo + 1);
                const int from = lo + rng_.below(span);
                // A tone that belongs to both roots: a perfect consonance to each if there is one,
                // else an imperfect one, a third or a sixth. Root, fourth and fifth alone share a
                // tone only when the two roots stand a second, a fourth or a fifth apart, and four
                // of the six steps the root takes are thirds and sixths -- so two changeovers in
                // three found nothing and went unannounced. The tone also has to pass what every
                // other note passes: a pivot a third above the foundation is still a third there.
                int found = -1;
                for (int tier = 0; tier < 2 && found < 0; ++tier) {
                    for (int i = 0; i < span; ++i) {
                        const int c = lo + ((from - lo + i) % span);
                        if (c < 0 || c > 127 || sounding(c)) continue;
                        const int a = ((c - pivotOld_) % 12 + 12) % 12, b = ((c - pivotTo_) % 12 + 12) % 12;
                        const bool fits = tier == 0 ? (perfectTo(a) && perfectTo(b)) : (consonantTo(a) && consonantTo(b));
                        // Admissible like any other note -- in range, rested, not a constellation
                        // heard lately, and by the rules -- except that a pivot tone may be the
                        // leading note, which is the one place R4.4 allows it.
                        if (fits && admissible(c, lo, hi, p, freqOf, true)) { found = c; break; }
                    }
                }
                if (found >= 0) {
                    ++pivotTones_;
                    if (p.memory > 0.0f) rememberChord(chordKey(-1, freqOf));   // the set that ends here
                    startIn(found, p, emit);
                    if (p.memory > 0.0f) rememberChord(chordKey(-1, freqOf));
                }
            }
            const double was = pivotLeft_;
            pivotLeft_ -= dt;
            if (was > 0.5 * static_cast<double>(p.pivot) && pivotLeft_ <= 0.5 * static_cast<double>(p.pivot) && pivotTo_ >= 0)
                setRoot(pivotTo_, p);
            if (pivotLeft_ <= 0.0) {
                pivotLeft_ = 0.0;
                if (pivotOld_ >= 0)
                    for (auto& s : slots_)
                        if (s.note >= 0 && ((s.note - pivotOld_) % 12 + 12) % 12 == 0) s.remaining = std::min(s.remaining, 1.0);
                pivotTo_ = pivotOld_ = -1;
            }
        }

        // The clocks the guards below read: how long since anything began, how long since anything
        // ended, and the running time against which a pitch's own rest is measured. Counted at the
        // control rate, which is where every decision here is made.
        sinceOn_ += dt;
        sinceOff_ += dt;
        now_ += dt;
        age_ += dt;
        rootAge_ += dt;
        if (p.rateBreath > 0.0f) {
            breathPhase_ += dt / std::max(10.0, static_cast<double>(p.breathPeriod) * 60.0);
            if (breathPhase_ > 1.0e6) breathPhase_ -= 1.0e6;
            const double tau = 6.283185307179586;
            breathValue_ = static_cast<float>((std::sin(tau * breathPhase_)
                                             + 0.5 * std::sin(tau * breathPhase_ * 1.6180339887)) / 1.5);
        }

        // Notes chosen a moment ago and held back so that they arrive together (Blend).
        for (auto& s : slots_) {
            if (s.note < 0 || s.startIn <= 0.0) continue;
            s.startIn -= dt;
            if (s.startIn <= 0.0) { s.startIn = 0.0; sinceOn_ = 0.0; emit(BrainEvent{ BrainEvent::Type::NoteOn, s.note, s.vel }); }
        }

        // What has been sounding, and for how long. A note held for four minutes tells more about
        // where the music is than one that passed through in twenty seconds, and on this
        // instrument that is the difference between almost every pair of notes. The memory fades
        // over about three minutes, so the key can drift when the music does instead of being
        // anchored for ever to whatever it opened with.
        {
            const double fade = std::exp(-dt / 180.0);
            for (float& w : pcWeight_) w = static_cast<float>(w * fade);
            for (const auto& s : slots_)
                if (s.note >= 0) pcWeight_[pitchClassOf(freqOf(s.note))] += static_cast<float>(dt);
        }
        if (p.mode == BrainMode::Chords) { updateChords(dt, p, freqOf, emit); return; }

        bool lastVoiceDue = false;
        for (auto& s : slots_) {
            if (s.note < 0) continue;
            s.remaining -= dt;
            if (s.remaining > 0.0) continue;
            // Two guards on letting go. Overlap keeps a voice sounding until the one that replaces
            // it has been in the air for its seconds, so no change ever lands on an empty chord;
            // Release Gap keeps two note-offs apart, so a cluster never collapses at once. Both
            // only postpone: the note goes as soon as the condition is met. And nothing ends in
            // the void (G3): the LAST voice does not go at all until its replacement is in -- it
            // used to, when it was alone, and an hour under the rules at full had 115 seconds of
            // silence in it and sixteen changes that landed on nothing.
            if (p.overlap > 0.0f) {
                if (activeCount() <= 1) { lastVoiceDue = true; continue; }
                if (sinceOn_ < static_cast<double>(p.overlap)) continue;
            }
            if (p.releaseGap > 0.0f && sinceOff_ < static_cast<double>(p.releaseGap)) continue;
            if (p.memory > 0.0f) rememberChord(chordKey(-1, freqOf));   // the constellation that ends here
            emit(BrainEvent{ BrainEvent::Type::NoteOff, s.note, 0.0f });
            if (s.note >= 0 && s.note < 128) pitchOffAt_[static_cast<size_t>(s.note)] = now_;
            sinceOff_ = 0.0;
            s.note = -1;
        }
        // The last voice, due, simply lies until the clock's next decision brings its replacement,
        // and goes once the newcomer has sounded its Overlap. Asking for the replacement at once
        // was tried: where the holds are shorter than the rate, the decisions then followed the
        // holds -- a pulse, with a coefficient of variation of 0.09 in the gaps, which is the one
        // thing G2 forbids. The clock is the clock; the voice waits for it.
        (void)lastVoiceDue;

        // While the pause runs nothing new begins. When it ends the cluster walks back in rather
        // than waiting out a draw made before it: a minute of nothing after the silence would read
        // as a fault, not as a rest.
        if (silenceLeft_ > 0.0) {
            silenceLeft_ -= dt;
            if (silenceLeft_ <= 0.0) { silenceLeft_ = 0.0; requestFill(); }
            return;
        }

        const double mean = tickSeconds(p);
        lastLean_ = leanOf(p);
        // A wait already begun was drawn against the rate that began it. Turn Event Rate down
        // from a hundred seconds to five and nothing happened for the rest of the old interval:
        // minutes of a knob that looked broken. The same thing kept the cluster thin -- when a
        // note leaves and the conductor switches to its filling pace, the wait still standing
        // was drawn at the slow one. Capping it was not enough; it is RESCALED, so the wait is
        // always measured in units of the rate that applies now and the shape of the draw is
        // kept. No preset modulates brain_rate, so nothing rendered before renders differently.
        if (timerMean_ > 0.0 && mean != timerMean_) timer_ *= mean / timerMean_;
        timerMean_ = mean;
        advanceTimer(dt, p, mean);
        if (onsetHold_) { timer_ = std::max(timer_, 0.2); return; }   // the foreground is speaking: no new onset
        if (timer_ > 0.0) return;
        // A memoryless draw, with a floor: no change faster than every twenty seconds (Anti 2),
        // which an exponential alone would give a quarter of the time at a mean of a minute. At
        // fast rates the floor is half the mean, so a conductor asked for two seconds still gets
        // them; the rule book's own rates begin at twenty.
        timer_ = clampv(gapDraw(mean), std::min(20.0, 0.5 * mean), mean * 4.0);
        timerMean_ = mean;

        // Two onsets belong either inside the window in which the ear fuses them into one sound,
        // or far enough apart to be two voices. In between they are heard as one chord played
        // inaccurately, which is the sound of a machine and not of an instrument. An event that
        // falls there waits for the far side rather than being dropped. The entrance is exempt: a
        // cluster walking in at a third of a second would otherwise never assemble at all.
        if (p.onsetGuard && !filling_ && sinceOn_ > 0.03 && sinceOn_ < 3.0) { timer_ = 3.0 - sinceOn_; return; }
        // And no decision inside twenty seconds of the last one (Anti 2) -- the floor the timer's
        // draw has, made a guard, so that neither the breath, a rescaled wait nor a last voice
        // asking for its replacement can get under it. Half the rate where the rate is faster.
        // A gust (Cascade, R5.5) is the one thing allowed under it: while the excitation is up
        // the floor comes down with it, so events can breed events as the rule intends.
        {
            const double least = std::min(20.0, 0.5 * static_cast<double>(p.rateSeconds)) / (1.0 + excite_);
            if (!filling_ && sinceOn_ > 0.03 && sinceOn_ < least) { timer_ = least - sinceOn_; return; }
        }

        const int low = std::min(p.low, p.high), high = std::max(p.low, p.high);
        const int density = clampv(static_cast<int>(std::lround(densityNow_)), 1, kSlots);
        if (filling_ && activeCount() >= density) filling_ = false;
        const KeyEstimate key = p.key > 0.0f ? findKey(pcWeight_) : KeyEstimate{};

        if (activeCount() >= density) {
            // One over the target already: an exchange is under way, its retiring voice sounding
            // out the overlap. Nothing else changes until it has gone.
            if (p.overlap > 0.0f && activeCount() > density) return;
            // Room is full: half of the time retire a note, else wait.
            if (rng_.uniform() >= 0.5f) return;
            int best = -1;
            // Which one leaves is half the question. By the clock alone -- the note that would end
            // soonest -- whatever the additions gained in harmonicity is given back one voice at a
            // time, and the chord never settles anywhere. With Harmonic up, the voice that goes is
            // the one whose leaving does the rest of the chord the most good. The draw is
            // short-circuited at zero, so a conductor that has not been asked for this behaves
            // exactly as it always did, down to the random stream.
            if (p.harmonic > 0.0f && rng_.uniform() < p.harmonic) {
                double bestH = -1.0;
                for (int i = 0; i < kSlots; ++i) {
                    if (slots_[i].note < 0) continue;
                    double rest[kSlots];
                    int m = 0;
                    for (int j = 0; j < kSlots; ++j) if (j != i && slots_[j].note >= 0 && m < kSlots) rest[m++] = freqOf(slots_[j].note);
                    const double h = m >= 2 ? chordHarmonicity(rest, m) : 0.0;
                    if (h > bestH) { bestH = h; best = i; }
                }
            } else {
                double rem = 1e12;
                for (int i = 0; i < kSlots; ++i) if (slots_[i].note >= 0 && slots_[i].remaining < rem) { rem = slots_[i].remaining; best = i; }
            }
            if (best < 0) return;
            if (p.overlap > 0.0f && activeCount() == density) {
                // The exchange in the rule's order (R5.4, G3): the newcomer first, below, and
                // the voice it replaces goes once the newcomer has sounded for Overlap seconds --
                // through the expiry above, which is where that wait is kept. It used to be the
                // other way round, note-off and note-on on one tick, so every change landed on a
                // chord one voice thinner than the one the listener had been hearing.
                slots_[best].remaining = -1.0e-9;   // due, and already "on its way out" to chordKey()
            } else {
                // A note retired because the room is full is a note-off like any other, and the two
                // clocks that guard note-offs apply to it: it waits its turn rather than slipping past.
                if (p.releaseGap > 0.0f && sinceOff_ < static_cast<double>(p.releaseGap)) return;
                if (p.memory > 0.0f) rememberChord(chordKey(-1, freqOf));
                emit(BrainEvent{ BrainEvent::Type::NoteOff, slots_[best].note, 0.0f });
                if (slots_[best].note >= 0 && slots_[best].note < 128) pitchOffAt_[static_cast<size_t>(slots_[best].note)] = now_;
                sinceOff_ = 0.0;
                slots_[best].note = -1;
                // At the target this is an exchange: one note leaves, one arrives below, and the
                // cluster stays the size it was. ABOVE the target it must not be -- Density had been
                // turned down, and refilling here is what made the cluster ignore that. Measured over
                // ninety-second stretches before this line existed: asked for two voices while ten
                // were sounding, it held 9.99; asked for one, 7.79. Now it sheds one per tick until
                // it is where it was asked to be.
                if (activeCount() >= density) return;
            }
        }

        // Weighted choice of the next note. Weighed BEFORE the root is allowed to wander (below):
        // a draw that found no candidate used to have moved the root all the same, and under the
        // rules at full, where the vetoes empty the field now and then, that was more root
        // changes than decisions -- sixteen an hour for sixty. If the root does move, the field
        // is weighed again against the root that now stands.
        const double mid = 0.5 * (low + high), half = std::max(1.0, 0.5 * (high - low));
        const float lean = lastLean_;
        float weights[128] = {};
        float total = 0.0f;
        bool wandered = false;
      weigh:
        std::fill(std::begin(weights), std::end(weights), 0.0f);
        total = 0.0f;
        const double rootFreq = freqOf(root_);
        bool rootSounding = false;
        for (auto& s : slots_) if (s.note >= 0 && pitchClassEqual(freqOf(s.note), rootFreq)) rootSounding = true;
        for (int c = low; c <= high && c < 128; ++c) {
            if (sounding(c)) continue;
            // A pitch that has only just been let go is not a new note, it is the same note again --
            // which is the one repetition this music notices. It rests for its seconds first.
            if (p.retrigger > 0.0f && now_ - pitchOffAt_[static_cast<size_t>(c)] < static_cast<double>(p.retrigger)) continue;
            const double fc = freqOf(c);
            bool duplicate = false;   // with snapped keys two keys can share one pitch
            for (auto& s : slots_) if (s.note >= 0 && std::fabs(std::log2(freqOf(s.note) / fc)) * 1200.0 < 1.0) duplicate = true;
            if (duplicate) continue;
            const double cons = p.consonanceOf(fc, rootFreq);
            float w = static_cast<float>(std::pow(cons, p.consonance * 3.0f));
            w *= 0.6f + 0.4f * static_cast<float>(1.0 - std::fabs(c - mid) / half);
            // Octave doubling is rare -- but the two rules disagree here, and the disagreement is
            // real rather than a wrinkle to be smoothed over. "A doubling is not a new colour" is a
            // matter of taste; harmonicity says an octave is the strongest relation two tones can
            // have, harmonics one and two of the same series. Harmonic is what settles it: at zero
            // the old taste rule stands untouched, and as it rises the veto softens to a
            // preference.
            {
                float doubling = 0.15f + 0.6f * clampv(p.harmonic, 0.0f, 1.0f);
                // Under Layers the taste rule yields to the rule book, whose table has the octave
                // and the unison at 0.18, "always allowed, low as well"; the ceiling per octave
                // and the spacing keep them in check. Without it the octave came out at 0.06.
                doubling += (1.0f - doubling) * clampv(p.layers, 0.0f, 1.0f);
                for (auto& s : slots_) if (s.note >= 0 && pitchClassEqual(freqOf(s.note), fc)) w *= doubling;
            }
            if (p.spacing != 0.0f)
                for (auto& s : slots_) if (s.note >= 0) w *= p.crowding(fc, freqOf(s.note));
            // How far this note may stand from the one chosen before it. In Chords mode Smooth is
            // the voice-leading distance of an exchange; in Free mode -- the mode nearly every
            // preset uses -- it did nothing at all, although the same idea is what a line is made
            // of: a voice that wanders steps far more often than it leaps. A weight rather than a
            // rule, so a leap stays possible, and short-circuited at zero so a conductor that was
            // never asked for it draws exactly what it always drew. At 0.5 a neighbour is about
            // ten times as likely as a note an octave away.
            if (p.smooth > 0.0f && lastNote_ >= 0) {
                const double steps = std::fabs(static_cast<double>(c - lastNote_));
                w *= static_cast<float>(std::pow(1.0 / (1.0 + steps / 3.0), 3.0 * static_cast<double>(p.smooth)));
            }
            // The register and interval rules -- roles, spacing, the third floor, the leading note,
            // the interval colours, a swapped degree -- as one weight, shared with chooseNote().
            w *= ruleWeight(c, low, high, rootSounding, p);
            if (w <= 0.0f) continue;   // a veto: the candidate is out, and weights[c] stays at nought
            // Free mode weighs a candidate against the ROOT alone, which is a weaker test than
            // the chord mode's: a note can sit well on the root and still pull the chord away
            // from having one. Harmonic is where that is caught, and it belongs here at least as
            // much as it belongs there -- this is the mode nearly every preset uses.
            if (p.harmonic > 0.0f) {
                double set[kSlots + 1];
                int m = 0;
                for (const auto& s : slots_) if (s.note >= 0 && m < kSlots) set[m++] = freqOf(s.note);
                set[m++] = fc;
                if (m >= 2)
                    w *= static_cast<float>(std::pow(std::max(chordHarmonicity(set, m), 1.0e-4), 5.0 * static_cast<double>(p.harmonic)));
            }
            if (p.key > 0.0f) w *= static_cast<float>(keyWeightOf(fc, key, p.key));
            if (p.even > 0.0f) {
                double set[kSlots + 1];
                int m = 0;
                for (const auto& s : slots_) if (s.note >= 0 && m < kSlots) set[m++] = freqOf(s.note);
                set[m++] = fc;
                w *= static_cast<float>(std::pow(std::max(chordEvenness(set, m), 1.0e-3), 5.0 * static_cast<double>(p.even)));
            }
            if (pitchClassEqual(fc, rootFreq)) w *= rootSounding ? 0.25f : 3.0f;                       // keep a foundation
            // Sharpening needs far more than flattening: the untouched draw is already within
            // two per cent of the maximum entropy twelve interval classes allow (measured:
            // 3.52 of 3.58 bits), so there is almost nowhere to go upwards and a long way down.
            if (lean != 0.0f) w = std::pow(w, lean > 0.0f ? 1.0f / (1.0f + 2.0f * lean) : 1.0f - 6.0f * lean);
            weights[c] = w;
            total += w;
        }
        if (total <= 0.0f) return;
        // The root wanders on Wander, and on Root Move, which until 12.09.2026 did nothing in this
        // mode: it was read only in Chords, and Free is what every preset in the library selects.
        // One draw from the stream, once a decision, and only once the field has proved to hold a
        // candidate -- see above.
        // Not during the exposition, though: the root stays where the night began until the
        // cluster has once stood at its density, or for four minutes at the least (R6.5's first
        // section has one root; R6.1 has the first change at four minutes at the earliest). A
        // changeover announced five milliseconds after the first note was the alternative.
        if (!settled_ && (activeCount() >= density || age_ > 240.0)) settled_ = true;
        if (!wandered && settled_) {
            wandered = true;
            // The way home (R6.4) also asks for the moves that get there: away from home and ripe,
            // the root moves oftener, so a piece two steps out at the fiftieth minute is not left
            // waiting for a chance that comes once in ten minutes.
            // R6.1's upper bound, as a certainty rather than a chance: past twelve minutes the
            // next decision ASKS whether the root should move, however the dice fall. Left to the
            // dice alone, two hours in eight had two root changes where section 11 wants three to
            // ten -- and a conductor whose root does not move keeps one pitch class sounding for
            // a third of the hour, which is the last line of that table. The draw is made either
            // way, so a conductor with Wander and Root Move at nought is untouched, down to the
            // last bit of its random stream.
            const float chance = moveChanceOf(p);
            const float u = rng_.uniform();
            if (anchorNote < 0 && chance > 0.0f && (u < chance || rootAge_ > 720.0)) {
                const int was = root_;
                if (!rootHold_) wanderRoot(low, high, freqOf, p);
                if (root_ != was) goto weigh;
            }
        }
        // The draw, and then the one thing the draw cannot see: whether this exact chord has been
        // heard before. A pitch may return, a constellation may not (R6.6) -- so a candidate that
        // would rebuild a chord from the last few minutes is struck out and the draw repeated, at
        // most a few times, because a conductor that refuses everything plays nothing.
        int chosen = -1;
        for (int attempt = 0; attempt < 4 && total > 0.0f; ++attempt) {
            float r = rng_.uniform() * total;
            chosen = -1;
            for (int c = low; c <= high && c < 128; ++c) { r -= weights[c]; if (r <= 0.0f && weights[c] > 0.0f) { chosen = c; break; } }
            if (chosen < 0) for (int c = high; c >= low; --c) if (weights[c] > 0.0f) { chosen = c; break; }
            if (chosen < 0) break;
            if (!constellationHeard(chosen, p, freqOf)) break;
            total -= weights[chosen];
            weights[chosen] = 0.0f;
            chosen = -1;
        }
        if (chosen < 0) return;
        chosen = viaDejaVu(chosen, p, -1, low, high, freqOf);
        // Three constellations are stamped here, and the first of them is the one that was missing:
        // the set that is ENDING. A constellation ends either because a voice goes -- stamped on
        // the note-off paths -- or because one ARRIVES, and that second way was stamped only at the
        // set's birth. Measured: a chord that had sounded until minute 48 was remembered as of
        // minute 42, and came back at minute 57, nine minutes after it was last heard rather than
        // fifteen. Then the set the new note makes, and the set it leaves behind once whatever is
        // sounding out its overlap has gone.
        if (p.memory > 0.0f) {
            rememberChord(chordKey(-1, freqOf));
            rememberChord(chordKey(chosen, freqOf));
            rememberChord(chordKey(chosen, freqOf, true));
        }

        for (auto& s : slots_) {
            if (s.note >= 0) continue;
            s.note = chosen;
            s.remaining = holdFor(chosen, low, high, p, isLowest(chosen));
            const float vel = velocityFor(chosen, low, high, p);   // nearness, not loudness (Top Soft)
            s.vel = vel;
            sinceOn_ = 0.0;
            emit(BrainEvent{ BrainEvent::Type::NoteOn, chosen, vel });
            kick(p);
            remember(chosen);
            break;
        }

        // Blend: the rest of the chord comes with it. Each further note is chosen against the
        // ones already committed -- the slot is taken at once, so the next choice sees it -- and
        // released a few milliseconds later, all of them inside the window in which the ear
        // fuses onsets into one event. Short-circuited at zero, so a conductor that has not been
        // asked for this draws nothing extra from its random stream. Not from silence, though:
        // the sound builds from below, one voice first (R2.3), and a chord that lands whole on an
        // empty room is an edit -- so the entrance, and the return after a rest, is a single note,
        // and Blend fills what is added to a chord already there.
        if (p.blend > 0.0f && activeCount() > 1) {
            const int missing = density - activeCount();
            const int extra = static_cast<int>(std::lround(static_cast<double>(p.blend) * missing));
            // Never past thirty milliseconds. It used to widen to fifty as Blend came down, and
            // between thirty milliseconds and three seconds is exactly the zone R5.3 forbids: a
            // second note forty milliseconds late is a chord played inaccurately, not a chord.
            const double window = 0.029;
            for (int k = 0; k < extra; ++k) {
                const int next = chooseNote(-1, low, high, p, freqOf);
                if (next < 0) break;
                remember(next);
                if (p.memory > 0.0f) { rememberChord(chordKey(-1, freqOf)); rememberChord(chordKey(next, freqOf)); }
                bool placed = false;
                for (auto& s : slots_) {
                    if (s.note >= 0) continue;
                    s.note = next;
                    s.remaining = holdFor(next, low, high, p, isLowest(next));
                    s.startIn = 0.001 + rng_.uniform() * window;
                    s.vel = velocityFor(next, low, high, p);
                    placed = true;
                    break;
                }
                if (!placed) break;
            }
        }
    }

private:
    /**
     * @brief One voice of the cluster: the note it holds, how it stands in time, and the velocity it was
     *        started with.
     *
     * startIn > 0: chosen, not yet sounding
     */
    struct Slot { int note = -1; double remaining = 0.0; double startIn = 0.0; float vel = 0.7f; };
    /** @var int ClusterBrain::Slot::note
     *  @brief MIDI note held, or -1 for an empty slot */
    /** @var double ClusterBrain::Slot::remaining
     *  @brief Free mode: seconds of hold left, negative once due and "on its way out"; Chords mode: seconds the voice has been sounding (its age) */
    /** @var double ClusterBrain::Slot::startIn
     *  @brief seconds until the NoteOn is emitted, for a Blend note held back so the chord arrives together; 0 once it sounds */
    /** @var float ClusterBrain::Slot::vel
     *  @brief the velocity it was, or will be, started with */

    // ---------------------------------------------------------------- chords
    /**
     * @brief Chords mode's tick: the cluster is kept full and one voice at a time is exchanged.
     *
     * The cluster is kept at `density` notes. Every `rateSeconds` -- or the moment someone asks --
     * one voice is exchanged: the one that has been sounding longest goes, and the note that takes
     * its place is scored for three things at once. How well it sits against the voices that stay
     * (the mean consonance with each of them, which is what makes it a chord rather than a heap),
     * how far it has to travel from the note it replaces (near is better: that is voice leading,
     * and it is why the change is heard as a shift and not as a cut), and a little dislike of
     * doubling a pitch class that is already there. Tension loosens the first of the three, so a
     * progression can be made to lean without becoming random.
     *
     * Called from update() after the bookkeeping the modes share, in its place. In order: the
     * voices sounding out their overlap are let go when they may; an empty or thin chord is filled
     * one note per tick (with Blend's extras); then, when the clock or a request says so, the
     * oldest voice -- or with Smooth the one whose exchange travels least for what it gains -- is
     * exchanged for chooseNote()'s pick, the allowance doubling until a note is found, and the root
     * may travel with the chord. Chords mode has no holds: `remaining` counts age here.
     *
     * @tparam FreqFn  callable `double(int note)`
     * @tparam EmitFn  callable `void(const BrainEvent&)`
     * @param dt      seconds since the last tick
     * @param p       the knobs
     * @param freqOf  frequency of a MIDI note in the engine's current scale
     * @param emit    receives the NoteOns and NoteOffs
     */
    template <class FreqFn, class EmitFn>
    void updateChords(double dt, const BrainParams& p, FreqFn&& freqOf, EmitFn&& emit)
    {
        const int low = std::min(p.low, p.high), high = std::max(p.low, p.high);
        const int density = clampv(p.density, 1, kSlots);
        for (auto& s : slots_) if (s.note >= 0) s.remaining += dt;   // in this mode it counts age, not time left

        // Voices already exchanged, sounding out their overlap before they are let go (R5.4), and
        // the release gap between note-offs applies to them as to any other.
        for (auto& l : leaving_) {
            if (l.note < 0) continue;
            l.in -= dt;
            if (l.in > 0.0) continue;
            if (p.releaseGap > 0.0f && sinceOff_ < static_cast<double>(p.releaseGap)) continue;
            if (p.memory > 0.0f) rememberChord(chordKey(-1, freqOf));   // the constellation that ends here
            emit(BrainEvent{ BrainEvent::Type::NoteOff, l.note, 0.0f });
            if (l.note < 128) pitchOffAt_[static_cast<size_t>(l.note)] = now_;
            sinceOff_ = 0.0;
            l.note = -1;
        }

        // Fill an empty chord one note per tick, so the first bars are an entrance and not a chord.
        int sounding = activeCount();
        lastLean_ = leanOf(p);
        if (filling_ && sounding >= density) filling_ = false;
        advanceTimer(dt, p, tickSeconds(p));
        const bool asked = stepRequested_;
        if (onsetHold_ && !asked) { timer_ = std::max(timer_, 0.2); return; }   // the foreground is speaking
        if (sounding < density) {
            if (timer_ > 0.0 && !asked && sounding > 0) return;
            stepRequested_ = false;
            timer_ = tickSeconds(p);
            const int add = chooseNote(-1, low, high, p, freqOf);
            if (add >= 0) {
            kick(p);
            remember(add);
            if (p.memory > 0.0f) rememberChord(chordKey(-1, freqOf));   // the set that ends here
            startIn(add, p, emit);
            if (p.memory > 0.0f) rememberChord(chordKey(-1, freqOf));
        }
            // As in Free mode: a Blend fills what is added to a chord already there; the entrance
            // from silence is one voice (R2.3). Four at once was what "Hush Field" opened with.
            if (p.blend > 0.0f && activeCount() > 1) {
                const int missing = density - activeCount();
                const int extra = static_cast<int>(std::lround(static_cast<double>(p.blend) * missing));
                const double window = 0.029;   // inside the thirty milliseconds the ear fuses (R5.3)
                for (int k = 0; k < extra; ++k) {
                    const int next = chooseNote(-1, low, high, p, freqOf);
                    if (next < 0) break;
                    remember(next);
                    if (p.memory > 0.0f) { rememberChord(chordKey(-1, freqOf)); rememberChord(chordKey(next, freqOf)); }
                    bool placed = false;
                    for (auto& s : slots_) {
                        if (s.note >= 0) continue;
                        s.note = next; s.remaining = 0.0;
                        s.startIn = 0.001 + rng_.uniform() * window;
                        s.vel = velocityFor(next, low, high, p);
                        placed = true;
                        break;
                    }
                    if (!placed) break;
                }
            }
            return;
        }
        if (timer_ > 0.0 && !asked) return;
        stepRequested_ = false;
        // Memoryless, as Free mode's clock and as G2 asks -- it was a fixed interval, which
        // measured as a pulse: a coefficient of variation of 0.05 in the gaps, and the density
        // periodic at the rate. The same floor, and the same breath in the mean.
        {
            const double mean = tickSeconds(p);
            timer_ = clampv(gapDraw(mean), std::min(20.0, 0.5 * mean), mean * 4.0);
        }

        {   // Anti 2, as in Free mode: no decision inside twenty seconds of the last one.
            const double least = std::min(20.0, 0.5 * static_cast<double>(p.rateSeconds)) / (1.0 + excite_);
            if (!filling_ && sinceOn_ > 0.03 && sinceOn_ < least) { timer_ = least - sinceOn_; return; }
        }
        // Which voice goes. By default the one that has been sounding longest, which is a rule
        // about time and knows nothing about where the chord would land.
        int oldest = -1; double age = -1.0;
        for (int i = 0; i < kSlots; ++i) if (slots_[i].note >= 0 && slots_[i].remaining > age) { age = slots_[i].remaining; oldest = i; }
        if (oldest < 0) return;
        // Above the target -- a changeover's tone took a slot, or Density came down -- the exchange
        // sheds a voice without bringing one in, through the overlap like any other, until the
        // chord is the size it was asked to be. Chords mode has no holds, so left alone every
        // pivot tone stayed for good: ten to thirteen voices after an hour at a density of five.
        if (activeCount() > density) {
            const int leaving = slots_[oldest].note;
            bool parked = false;
            if (p.overlap > 0.0f)
                for (auto& l : leaving_) if (l.note < 0) { l.note = leaving; l.in = static_cast<double>(p.overlap); parked = true; break; }
            if (!parked) {
                if (p.memory > 0.0f) rememberChord(chordKey(-1, freqOf));
                emit(BrainEvent{ BrainEvent::Type::NoteOff, leaving, 0.0f });
                if (leaving >= 0 && leaving < 128) pitchOffAt_[static_cast<size_t>(leaving)] = now_;
                sinceOff_ = 0.0;
            }
            slots_[oldest].note = -1;
            return;
        }
        // The root may travel with the chord, which is what turns a voicing change into a
        // progression; without it the harmony circles one centre for ever. The same chance as
        // Free mode's, Wander included -- Chords read Root Move alone, and at the rule book's
        // values that was two changes an hour -- and the same exposition and the same way home.
        if (!settled_ && (activeCount() >= density || age_ > 240.0)) settled_ = true;
        {   // as in Free mode: the dice, or twelve minutes (R6.1), and the draw made either way
            const float chance = moveChanceOf(p);
            const float u = rng_.uniform();
            if (settled_ && !rootHold_ && chance > 0.0f && (u < chance || rootAge_ > 720.0)) wanderRoot(low, high, freqOf, p);
        }

        int moving = oldest, arriving = -1;
        if (p.smooth > 0.0f) {
            // Try them all and keep the exchange that moves the chord least for what it gains.
            // The distance a chord travels is the sum of what its voices move under the cheapest
            // pairing of the two chords; when one note is exchanged that sum is exactly the leap
            // that one voice makes, which is worth stating because it means the number already in
            // this function is the right one and needs no correcting -- checked over four thousand
            // random exchanges, the two agree exactly.
            double bestQ = -1.0;
            for (int i = 0; i < kSlots; ++i) {
                if (slots_[i].note < 0) continue;
                const int was = slots_[i].note;
                // Out of its slot so that it does not vote on its successor -- but still heard by
                // the rules, since it sounds for the overlap beside whatever replaces it. Unheard,
                // the replacement could stand a third under it for ten seconds (measured: five an
                // hour at the rule book's values, and thirty pairs closer than the register allows).
                slots_[i].note = -1;
                extraHeard_ = was;
                double sc = 0.0;
                const int cand = chooseNote(was, low, high, p, freqOf, &sc);
                extraHeard_ = -1;
                slots_[i].note = was;
                if (cand < 0 || cand == was) continue;
                const double travel = std::fabs(static_cast<double>(cand - was)) / 12.0;
                const double q = sc / (1.0 + static_cast<double>(p.smooth) * travel);
                if (q > bestQ) { bestQ = q; moving = i; arriving = cand; }
            }
            if (arriving < 0) return;
        } else {
            const int was = slots_[oldest].note;
            slots_[oldest].note = -1;                               // it must not vote on its own replacement
            extraHeard_ = was;                                      // but the rules still hear it (see above)
            arriving = chooseNote(was, low, high, p, freqOf);
            extraHeard_ = -1;
            slots_[oldest].note = was;
        }
        const int leaving = slots_[moving].note;
        // No candidate inside the voice-leading allowance -- the rules at full leave few notes a
        // fifth from the one that goes -- used to mean no exchange at all, and another draw of the
        // clock before the next try: fifteen minutes without a decision, measured, then a run of
        // them. A leap is better than a silence of the harmony: the allowance doubles until a
        // note is found, up to the whole range.
        for (float lead = p.voiceLead * 2.0f; arriving < 0 && lead < 128.0f; lead *= 2.0f) {
            BrainParams wider = p;
            wider.voiceLead = lead;
            slots_[moving].note = -1;
            extraHeard_ = leaving;
            arriving = chooseNote(leaving, low, high, wider, freqOf);
            extraHeard_ = -1;
            slots_[moving].note = leaving;
        }
        arriving = viaDejaVu(arriving, p, leaving, low, high, freqOf);
        const int oldestKeep = oldest;
        (void)oldestKeep;
        if (arriving < 0 || arriving == leaving) return;
        // The set that is ending, stamped before anything moves: a constellation ends either
        // because a voice goes or because one arrives, and the second way used to be remembered
        // only from its birth.
        if (p.memory > 0.0f) rememberChord(chordKey(-1, freqOf));
        // The voice that goes is let go only once the one replacing it has been sounding for
        // Overlap seconds (R5.4), so the change is heard inside a chord that is still there. Its
        // note-off used to fall on the same tick as the note-on. Without Overlap it still does.
        bool parked = false;
        if (p.overlap > 0.0f)
            for (auto& l : leaving_) if (l.note < 0) { l.note = leaving; l.in = static_cast<double>(p.overlap); parked = true; break; }
        if (!parked) {
            emit(BrainEvent{ BrainEvent::Type::NoteOff, leaving, 0.0f });
            if (leaving >= 0 && leaving < 128) pitchOffAt_[static_cast<size_t>(leaving)] = now_;
            sinceOff_ = 0.0;
        }
        recent_[recentHead_] = leaving;                             // it has had its turn
        recentHead_ = (recentHead_ + 1) % kRecent;
        slots_[moving].note = arriving;
        slots_[moving].remaining = 0.0;
        kick(p);
        remember(arriving);
        // The set with the leaving voice still in it, and the one that remains once it has gone.
        if (p.memory > 0.0f) { rememberChord(chordKey(-1, freqOf)); rememberChord(chordKey(-1, freqOf, true)); }
        sinceOn_ = 0.0;
        emit(BrainEvent{ BrainEvent::Type::NoteOn, arriving, velocityFor(arriving, low, high, p) });
    }

    /**
     * @brief Starts a note in the first free slot at once: the pivot tone of a changeover, or the note
     *        Chords mode adds to a thin chord.
     *
     * Draws the hold (Free mode) and the velocity from the stream, resets the onset clock and emits
     * the NoteOn itself; the callers stamp the constellation memory around it. Nothing happens
     * when every slot is taken.
     *
     * @tparam EmitFn  callable `void(const BrainEvent&)`
     * @param note  MIDI note to start, already passed admissible() or chooseNote()
     * @param p     the knobs: the mode decides what `remaining` means, the register and roles the hold and velocity
     * @param emit  receives the NoteOn
     */
    template <class EmitFn>
    void startIn(int note, const BrainParams& p, EmitFn&& emit)
    {
        const int lo = std::min(p.low, p.high), hi = std::max(p.low, p.high);
        for (auto& s : slots_) if (s.note < 0) {
            s.note = note;
            // In Chords mode `remaining` counts age; in Free mode it is the hold, and a tone started
            // here -- the pivot tone of a changeover -- is a voice like any other. At nought it
            // expired at once and went ten seconds later: every changeover ended with a note-off.
            s.remaining = p.mode == BrainMode::Chords ? 0.0 : holdFor(note, lo, hi, p, isLowest(note));
            sinceOn_ = 0.0;
            s.vel = velocityFor(note, lo, hi, p);
            emit(BrainEvent{ BrainEvent::Type::NoteOn, note, s.vel });
            return;
        }
    }

    /**
     * @brief The wait until the next decision: memoryless (G2), and never under the floor (Anti 2) -- but
     * SHIFTED past the floor rather than clipped to it.
     *
     * Clipped, every draw that fell under twenty
     * seconds landed on twenty exactly, and in Chords mode, where every expiry of the clock is an
     * exchange, that was a third of all gaps: a pulse at the floor, and the density periodic at
     * twenty seconds (measured, 0.5 at lag 20). The mean is what the rate says either way.
     *
     * One draw from the stream.
     *
     * @param mean  the mean gap in seconds, as tickSeconds() gives it
     * @return      seconds to wait: the floor (twenty seconds, or half the mean where that is less)
     *              plus an exponential draw whose mean makes the whole average out to @p mean
     */
    double gapDraw(double mean)
    {
        const double floor = std::min(20.0, 0.5 * mean);
        const double u = -std::log(1.0 - static_cast<double>(rng_.uniform()) + 1e-9);
        return floor + u * std::max(mean - floor, 0.25 * mean);
    }

    /**
     * @brief How likely a decision is to move the root as well: Wander, Root Move, and -- away from home
     * and ripe -- the way home (R6.4), which asks for the moves that get there, so a piece two
     * steps out at the fiftieth minute is not left waiting for a chance that comes once in ten
     * minutes.
     *
     * One formula for both modes.
     *
     * @param p  the knobs: Wander, Root Move, Home and Home Time
     * @return   a probability 0 .. 1 that the decision at hand asks wanderRoot() to move
     */
    float moveChanceOf(const BrainParams& p) const
    {
        const float homing = p.home > 0.0f && root_ != homeRoot_
            ? p.home * static_cast<float>(clampv(age_ / std::max(60.0, static_cast<double>(p.homeTime) * 60.0), 0.0, 1.0)) : 0.0f;
        return clampv(p.wander * 0.35f + p.rootMove * 0.5f + homing * homing * 0.4f, 0.0f, 1.0f);
    }

    /**
     * @brief Whether `note`, already in its slot, is the lowest voice sounding.
     * @param note  the MIDI note asked about
     * @return      true when no slot holds a lower note -- the voice Bass Hold applies to
     */
    bool isLowest(int note) const
    {
        for (const auto& o : slots_) if (o.note >= 0 && o.note < note) return false;
        return true;
    }

    /**
     * @brief The register and interval rules of the rule book, as one weight on a candidate; nought is a veto.
     *
     * The register and interval rules of the rule book (sections 2 to 4, 12.09.2026), as one weight
     * on a candidate: the register roles and their ceiling per octave, the minimum interval by
     * register, the floor under which no third may stand, the leading note against a sounding
     * root, the interval colours, and the one degree a root change may have swapped. Nought is a
     * veto. Both draws consult it -- the weighted draw of Free mode, and chooseNote(), which scores
     * the exchanges of Chords mode and the extra notes of a Blend. Until it was shared, chooseNote()
     * had none of them: every note a Blend added to a chord, and every note Chords mode chose, was
     * placed without roles, without the spacing floor, with thirds in the bass and the leading note
     * under a sounding root. All of it on MIDI numbers, which is what the rules are written in.
     *
     * @param c             the candidate MIDI note
     * @param low           bottom of the register, MIDI
     * @param high          top of the register, MIDI (the roles are placed between the two)
     * @param rootSounding  whether the root's pitch class is in the cluster, for the leading-note rule
     * @param p             the knobs of the register roles section
     * @param pivotal       true for the pivot tone of a changeover, which may be the leading note
     * @return              a weight to multiply the candidate's by, 0 for a veto, 1 with every knob at its default
     */
    float ruleWeight(int c, int low, int high, bool rootSounding, const BrainParams& p, bool pivotal = false) const
    {
        float w = 1.0f;
        // Everything that sounds: the slots, and in Chords mode the voices sounding out their
        // overlap on the leaving list -- which the rules did not see, so a new note could stand a
        // third under one of them for ten seconds (five an hour at the rule book's values).
        int heard[2 * kSlots + 1];
        int n = 0;
        for (const auto& s : slots_) if (s.note >= 0) heard[n++] = s.note;
        for (const auto& l : leaving_) if (l.note >= 0) heard[n++] = l.note;
        if (extraHeard_ >= 0) heard[n++] = extraHeard_;   // Chords: the voice being exchanged, out of its slot but still sounding
        // The register roles. The share each part of the register wants, and the ceiling on how
        // many notes an octave may hold -- two, and one below MIDI 36, where the critical band
        // is wider than a fifth and a second note only makes the first one rough.
        if (p.layers > 0.0f) {
            const double t = high > low ? static_cast<double>(c - low) / static_cast<double>(high - low) : 0.5;
            const double want = t < 0.15 ? 0.5 : (t < 0.55 ? 1.6 : (t < 0.85 ? 1.0 : 0.35));
            w *= static_cast<float>(1.0 + static_cast<double>(p.layers) * (want - 1.0));
            // The foundation is the root (table 2: one voice, its octave at most): a fifth lying
            // there for twenty minutes was the one pitch class over a quarter of the hour that the
            // last check of section 11 forbids.
            if (t < 0.15 && ((c - root_) % 12 + 12) % 12 != 0) w *= 1.0f - 0.7f * p.layers;
            int inOctave = 0;
            for (int i = 0; i < n; ++i) if (heard[i] / 12 == c / 12) ++inOctave;
            // At most two to an octave and one below 36 (R2.2): a veto at 1, where it left a
            // tenth -- which was a third note in some octave for 195 seconds of an hour.
            if (inOctave >= (c < 36 ? 1 : 2)) w *= std::max(0.0f, 1.0f - p.layers);
        }
        // The minimum interval, by register -- the register of the LOWER note of the pair, since
        // the critical band is a matter of the lower frequency; judged on the candidate alone, a
        // C3 over a sounding B2 was a second in the bass that passed (measured: 8 of 356 pairs
        // under the rules at full). Below 36 an octave, to 47 a fifth, to 59 a minor third, to 71
        // a second; above that a semitone is a colour and this says nothing.
        if (p.lowSpacing > 0.0f) {
            for (int i = 0; i < n; ++i) {
                const int d = std::abs(c - heard[i]);
                const int lower = std::min(c, heard[i]);
                const int least = lower < 36 ? 12 : (lower < 48 ? 7 : (lower < 60 ? 3 : (lower < 72 ? 2 : 1)));
                // At 1 this is a veto, not a penalty: the rule says only octaves below 36 and
                // only fifths and fourths below 48, and a five-per-cent survivor is still a
                // second in the bass every tenth note.
                if (d > 0 && d < least) w *= std::max(0.0f, 1.0f - p.lowSpacing);
                // The tritone (R3.2): out below 55 and rare above it, the one interval the rule
                // book's table weights at a hundredth. It had no rule of its own -- consonance
                // made it rare, and Harmonic and Key let it back in: nine in an hour, all low.
                if (d % 12 == 6) w *= lower < 55 ? std::max(0.0f, 1.0f - p.lowSpacing) : 1.0f - 0.75f * p.lowSpacing;
            }
        }
        // No third under the floor (R3.1), and a floor is a floor: a veto, not the two per cent
        // that stood here. It is the LOWER note of the pair that has to clear it -- judged on the
        // candidate alone, a C3 over a sounding A2 was a minor third whose bass note stood under
        // the floor, and it passed. And it is the CLOSE third that is mud, three or four semitones:
        // a tenth is the open voicing that avoids it, and counting pitch classes forbade that too.
        if (p.thirdFloor > 0)
            for (int i = 0; i < n; ++i) {
                const int d = std::abs(c - heard[i]);
                if ((d == 3 || d == 4) && std::min(c, heard[i]) < p.thirdFloor) return 0.0f;
            }
        // The leading note (R4.4, Anti 6): penalised while the root sounds, and at 1 excluded
        // whether it sounds or not -- a root entering over a sounding leading note is the same
        // simultaneity from the other side, and it happened twice an hour under the rules at
        // full. The pivot tone of a changeover is the one exception the rule makes.
        if (p.leading > 0.0f && !pivotal && ((c - root_) % 12 + 12) % 12 == 11)
            w *= rootSounding ? 1.0f - p.leading : 1.0f - p.leading * p.leading;
        // Interval colour, against everything that sounds.
        if (p.thirds != 0.0f || p.seconds != 0.0f || p.seventh > 0.0f) {
            for (int i = 0; i < n; ++i) {
                const int ic = std::abs(c - heard[i]) % 12;
                if ((ic == 3 || ic == 4) && p.thirds != 0.0f)
                    w *= p.thirds >= 0.0f ? (1.0f + 1.5f * p.thirds) : (1.0f + 0.95f * p.thirds);
                // Seconds are a colour up top and mud down below, so seeking them only counts
                // above the floor; avoiding them counts everywhere. The major seventh belongs
                // here: it is a minor second turned upside down, the one interval class the rule
                // book's table of weights has no line for at all, and the conductor made it a
                // tenth of what it played -- because harmonicity likes it (the fifteenth harmonic
                // is a major seventh), which is a disagreement between two of the document's own
                // instructions rather than a fault. Avoiding seconds now avoids it too.
                // ...and not all seconds alike: the table has the minor second at 0.03 and the
                // major at 0.11, nearly four times as much, so one knob pulling both down equally
                // put the major second under its share while it took the minor one out. A third of
                // the amount for the major second, the whole of it for the minor and for the major
                // seventh, which the table does not have at all.
                if ((ic == 1 || ic == 2 || ic == 11) && p.seconds != 0.0f
                    && (p.seconds < 0.0f || (ic != 11 && c >= std::max(p.thirdFloor, 60)))) {
                    const float amount = p.seconds * (ic == 2 ? 0.33f : 1.0f);
                    w *= amount >= 0.0f ? (1.0f + 1.5f * amount) : (1.0f + 0.95f * amount);
                }
                if (ic == 10 && p.seventh > 0.0f) w *= 1.0f + 1.5f * p.seventh;
            }
        }
        // The one degree a root change may have exchanged (Degree Swap). Nothing at all until a
        // change has actually swapped one, and then only that pair.
        w *= degreeBias_[static_cast<size_t>(((c - root_) % 12 + 12) % 12)];
        return w;
    }

    /**
     * @brief Velocity as nearness rather than as loudness: the air is far and quiet, the foundation near
     * and steady (Rene's table of roles).
     *
     * One draw from the stream, the same one every path made.
     *
     * @param note  the MIDI note being started
     * @param low   bottom of the register, MIDI
     * @param high  top of the register, MIDI; the fall of velocity with height runs between the two
     * @param p     the knobs: Spread and Bias shape the draw, Top Soft narrows it and tilts it
     * @return      velocity 0 .. 1; 0.5 .. 0.9 uniform with Top Soft at nought
     */
    float velocityFor(int note, int low, int high, const BrainParams& p)
    {
        const float u = p.shaped(rng_.uniform());
        // Within a role velocity varies by twelve at most (R7.2), so the spread narrows with Top
        // Soft: from the 0.5..0.9 the conductor always drew to 0.67 +- 0.095 at 1, which is 85 +-
        // 12 for the foundation and, at forty per cent at the top, 34 +- 5 for the air -- table
        // 2's numbers (70..100 down to 20..45). At nought the draw is exactly what it was.
        const float soft = clampv(p.topSoft, 0.0f, 1.0f);
        float vel = soft > 0.0f ? 0.67f + (0.2f - 0.105f * soft) * (2.0f * u - 1.0f) : 0.5f + 0.4f * u;
        if (soft > 0.0f && high > low)
            vel *= 1.0f - 0.6f * soft * static_cast<float>(clampv(note, low, high) - low) / static_cast<float>(high - low);
        return vel;
    }

    /**
     * @brief The hold a new note is given: Hold Min to Hold Max, shaped, and under Layers scaled to its
     * role (table 2) -- the colour holds half as long as the body, the air a quarter.
     *
     * The
     * foundation's multiple is Bass Hold's, applied by the caller, which knows which voice is
     * lowest. One draw from the stream, the same one as before.
     *
     * @param note    the MIDI note being started
     * @param low     bottom of the register, MIDI
     * @param high    top of the register, MIDI; the roles are placed between the two
     * @param p       the knobs: Hold Min, Hold Max, Spread, Bias, Layers, Bass Hold
     * @param lowest  whether this note is the lowest voice (isLowest), which Bass Hold multiplies
     * @return        seconds the note holds before it is due
     */
    double holdFor(int note, int low, int high, const BrainParams& p, bool lowest)
    {
        const float hmin = std::min(p.holdMin, p.holdMax), hmax = std::max(p.holdMin, p.holdMax);
        double hold = hmin + p.shaped(rng_.uniform()) * (hmax - hmin);
        // The foundation lies while what stands on it moves: the lowest voice keeps its note for
        // Bass Hold times everyone else's, which at 6 is table 2's three to twenty minutes. The
        // other roles have their own times under Layers -- a second foundation voice four times
        // the draw, the body once, the colour half, the air a quarter.
        if (lowest && p.bassHold > 1.0f) hold *= static_cast<double>(p.bassHold);
        else if (p.layers > 0.0f && high > low) {
            const double t = static_cast<double>(clampv(note, low, high) - low) / static_cast<double>(high - low);
            const double role = t < 0.15 ? 4.0 : (t < 0.55 ? 1.0 : (t < 0.85 ? 0.5 : 0.25));
            hold *= 1.0 + static_cast<double>(p.layers) * (role - 1.0);
        }
        return hold;
    }

    /**
     * @brief The best note to bring in, given the ones that stay.
     *
     * `from` is the note being replaced, or
     * -1 when the chord is still filling up.
     *
     * The scoring draw of Chords mode and of Blend's extra notes (Free mode's own draw is the
     * weighted one in update(); the two share ruleWeight, the retrigger rest, the constellation
     * memory, Key, Even and Harmonic). Every candidate in the register that is free, rested and
     * not vetoed is scored -- mean consonance with the root and the voices that stay, raised by
     * Tension; the travel from `from` within the voice-leading allowance; a fading penalty on
     * notes that left recently; Key, Even, Harmonic; a dislike of doublings -- and a little noise
     * is added so the best is not always the same; the highest score wins. Draws from the stream
     * once per candidate.
     *
     * @tparam FreqFn  callable `double(int note)`
     * @param from      the note being replaced, or -1 when adding to the chord
     * @param low       bottom of the register, MIDI
     * @param high      top of the register, MIDI
     * @param p         the knobs
     * @param freqOf    frequency of a MIDI note in the engine's current scale
     * @param outScore  receives the winner's score (0 when none was found) when not null; Smooth
     *                  compares exchanges by it
     * @return          the MIDI note to bring in, or -1 when no candidate survived
     */
    template <class FreqFn>
    int chooseNote(int from, int low, int high, const BrainParams& p, FreqFn&& freqOf,
                   double* outScore = nullptr) const
    {
        const double rootFreq = freqOf(root_);
        const KeyEstimate key = p.key > 0.0f ? findKey(pcWeight_) : KeyEstimate{};
        const float lead = std::max(p.voiceLead, 0.5f);
        const float lean = leanOf(p);
        bool rootSounding = false;
        for (const auto& s : slots_) if (s.note >= 0 && pitchClassEqual(freqOf(s.note), rootFreq)) rootSounding = true;
        int best = -1; double bestScore = -1e9;
        for (int c = low; c <= high && c < 128; ++c) {
            if (sounding(c)) continue;
            // The note being replaced cannot replace itself. Its slot is emptied before this runs
            // so that it does not vote on its own successor -- which also left it in the running,
            // and being at zero distance it always won: the chord never moved once until this
            // line existed. The self test found it, the descriptors never would have.
            if (from >= 0 && c == from) continue;
            // What Free mode asks of every candidate, asked here too: a pitch is resting after its
            // release (R7.4), a constellation was heard lately (R6.6), and the register and interval
            // rules as one weight -- a veto ends the candidate.
            if (p.retrigger > 0.0f && now_ - pitchOffAt_[static_cast<size_t>(c)] < static_cast<double>(p.retrigger)) continue;
            if (constellationHeard(c, p, freqOf)) continue;
            const float rule = ruleWeight(c, low, high, rootSounding, p);
            if (rule <= 0.0f) continue;
            const double fc = freqOf(c);
            bool duplicate = false;
            for (const auto& s : slots_) if (s.note >= 0 && std::fabs(std::log2(freqOf(s.note) / fc)) * 1200.0 < 1.0) duplicate = true;
            if (duplicate) continue;
            // How it sits against the voices that stay, and against the root.
            double fit = p.consonanceOf(fc, rootFreq);
            int n = 1;
            for (const auto& s : slots_) if (s.note >= 0) { fit += p.consonanceOf(fc, freqOf(s.note)); ++n; }
            fit /= n;
            double score = std::pow(fit, 1.0 + 2.5 * (1.0 - clampv(p.chordTension, 0.0f, 1.0f)));
            // Voice leading: the further this voice has to travel, the worse, and beyond the
            // allowance it is not considered at all.
            if (from >= 0) {
                const double steps = std::fabs(static_cast<double>(c - from));
                if (steps > lead) continue;
                score *= 1.0 - 0.75 * (steps / lead);
            }
            // What left recently is worth less. Without this the harmony keeps picking up the note
            // it has just put down -- it is the nearest candidate and it fitted a moment ago, so
            // it wins again -- and a run with little room to move (a pinned root and a narrow
            // allowance) circles a handful of chords for ever instead of going somewhere. The
            // penalty fades with age, so nothing is banned, only postponed.
            for (int i = 0; i < kRecent; ++i) if (recent_[i] == c) {
                const int age = (recentHead_ - 1 - i + 2 * kRecent) % kRecent;   // 0 = just left
                score *= 0.12 + 0.11 * static_cast<double>(age);
            }
            if (p.key > 0.0f) score *= keyWeightOf(fc, key, p.key);
            if (p.even > 0.0f) {
                double set[kSlots + 1];
                int m = 0;
                for (const auto& s : slots_) if (s.note >= 0 && m < kSlots) set[m++] = freqOf(s.note);
                set[m++] = fc;
                score *= std::pow(std::max(chordEvenness(set, m), 1.0e-3), 5.0 * static_cast<double>(p.even));
            }
            // And how the WHOLE chord would sit, if the conductor has been told to listen for it.
            // The pairwise score above cannot hear this: it asks how each pair sounds, never
            // whether the set has one root.
            if (p.harmonic > 0.0f) {
                double set[kSlots + 1];
                int m = 0;
                for (const auto& s : slots_) if (s.note >= 0 && m < kSlots) set[m++] = freqOf(s.note);
                set[m++] = fc;
                const double h = chordHarmonicity(set, m);
                score *= std::pow(std::max(h, 1.0e-4), 5.0 * static_cast<double>(p.harmonic));
            }
            // An octave of something already sounding is a doubling, not a new colour.
            for (const auto& s : slots_) if (s.note >= 0 && pitchClassEqual(freqOf(s.note), fc)) score *= 0.2;
            if (pitchClassEqual(fc, rootFreq)) score *= 0.5;
            score *= rule;
            if (lean < 0.0f) score = std::pow(score, 1.0 - 6.0 * static_cast<double>(lean));
            score *= lean > 0.0f ? (0.85 - 0.7 * static_cast<double>(lean)) + (0.3 + 1.4 * static_cast<double>(lean)) * rng_.uniform()
                                 : 0.85 + 0.3 * rng_.uniform();          // a little life, so it is not a machine
            if (score > bestScore) { bestScore = score; best = c; }
        }
        if (outScore != nullptr) *outScore = best >= 0 ? bestScore : 0.0;
        return best;
    }

    /**
     * @brief How stable a frequency is as a degree of the key that was found, on the Krumhansl-Kessler
     * profile: 1 for the tonic, about a third for a note outside the scale.
     *
     * Weighted by the
     * confidence of the key estimate, so an uncertain key pulls gently and a clear one pulls
     * hard -- and a passage with no key in it at all is left alone.
     *
     * @param f       the candidate's frequency, Hz
     * @param k       the key as findKey() found it; no key, or no confidence, answers 1
     * @param amount  the Key knob, 0 .. 1; 0 answers 1
     * @return        a weight to multiply the candidate's by, the profile's stability raised by
     *                amount and confidence; 1 for the tonic whatever the amount
     */
    static double keyWeightOf(double f, const KeyEstimate& k, float amount)
    {
        if (amount <= 0.0f || k.key < 0 || k.confidence <= 0.0f) return 1.0;
        const float* prof = k.minor() ? keyProfileMinor() : keyProfileMajor();
        const int degree = ((pitchClassOf(f) - k.tonic()) % 12 + 12) % 12;
        const double stability = static_cast<double>(prof[degree]) / 6.35;
        return std::pow(stability, 2.5 * static_cast<double>(amount) * static_cast<double>(k.confidence));
    }

    /**
     * @brief Interval classes a pivot tone may stand at to a root: the perfect consonances, and the
     * imperfect ones the second tier falls back to.
     *
     * This is the first tier: unison, fourth and fifth (the octave is the unison's class).
     * @param ic  interval class in semitones, 0 .. 11, from the root up to the tone
     * @return    true for 0, 5 and 7
     */
    static bool perfectTo(int ic)   { return ic == 0 || ic == 5 || ic == 7; }
    /**
     * @brief The second tier of the pivot tone's search: the perfect consonances and the imperfect
     *        ones, thirds and sixths, major and minor.
     * @param ic  interval class in semitones, 0 .. 11, from the root up to the tone
     * @return    true for 0, 3, 4, 5, 7, 8 and 9
     */
    static bool consonantTo(int ic) { return ic == 0 || ic == 3 || ic == 4 || ic == 5 || ic == 7 || ic == 8 || ic == 9; }

    /**
     * @brief Whether two frequencies are the same pitch class: within ten cents of a unison or an
     *        octave, once folded into one octave.
     *
     * Asked of frequencies rather than MIDI numbers because the scale need not have twelve degrees,
     * and used for the doubling rule, the "root is sounding" test and the foundation weight.
     *
     * @param fa  one frequency, Hz
     * @param fb  the other, Hz
     * @return    true when they are octaves of one another; false, not a hang, for a zero or
     *            infinite ratio
     */
    static bool pitchClassEqual(double fa, double fb)
    {
        double r = fa / fb;
        // The same guard intervalConsonance has, and for the same reason: these two loops
        // halve and double until the ratio is inside an octave, and neither of them ends for
        // a ratio of zero or infinity. A Scala file with a degree of zero -- which the tuning
        // code answers with a frequency of zero, by design -- would have hung the audio thread
        // here for ever.
        if (!(r > 0.0) || !(r < 1.0e30)) return false;
        while (r >= 2.0) r *= 0.5;
        while (r < 1.0) r *= 2.0;
        return std::fabs(std::log2(r)) * 1200.0 < 10.0 || std::fabs(std::log2(r) - 1.0) * 1200.0 < 10.0;
    }

    /**
     * @brief Chooses the root's next home and moves there -- at once, or through a changeover when Pivot is up.
     *
     * Called by both modes when a decision's dice (moveChanceOf) or the twelve-minute bound say the
     * root should move. Keeps R6.1's floor of four minutes between moves unless the way home
     * overrides it; then scores every note of the register as a candidate root -- the interval the
     * dice drew from the six just steps, the size and the direction of the step, the key that was
     * found, the leading-note simultaneity it would make, and the pull home -- and takes the best
     * when it is worth having. Counts the move in rootMoves_. Draws from the stream once for the
     * target interval.
     *
     * @tparam FreqFn  callable `double(int note)`
     * @param low     bottom of the register, MIDI: the roots considered
     * @param high    top of the register, MIDI
     * @param freqOf  frequency of a MIDI note in the engine's current scale
     * @param p       the knobs of the root and long form section, and Key and Leading
     */
    template <class FreqFn>
    void wanderRoot(int low, int high, FreqFn&& freqOf, const BrainParams& p)
    {
        // R6.1: every four to twelve minutes. The lower bound is a rule as much as the upper one,
        // and without it the way home -- which asks for more moves the later it gets -- ran to
        // fifteen an hour where section 11 wants three to ten. R6.4 overrides it: late and away
        // from where the night began, the music may take the step home whenever it finds it, and
        // with the floor in force it could not, four hours in eight.
        {
            const double ripeNow = p.home > 0.0f
                ? clampv(age_ / std::max(60.0, static_cast<double>(p.homeTime) * 60.0), 0.0, 1.0) : 0.0;
            const bool goingHome = p.home > 0.0f && root_ != homeRoot_ && ripeNow > 0.55;
            if (rootAge_ < 240.0 && !goingHome) return;
        }
        const float keyAmount = p.key;
        static const double kTargets[] = { 1.5, 4.0 / 3.0, 1.25, 1.2, 5.0 / 3.0, 1.6 };
        const double target = kTargets[rng_.below(6)];
        const double rootFreq = freqOf(root_);
        // The root and the key are two different things, and this is where they are told about
        // each other: a root that lands on a stable degree of the key the music is already in is
        // what a modulation is, while a root that ignores it is a second conductor disagreeing
        // with the first. It should be said that this is an argument, not a measurement. Measured
        // over three minutes it makes no difference to how clearly the music sits in a key -- 0.90
        // against 0.91 with the root left key-blind -- and what it does buy is one more pitch
        // class in play, nine against eight. It is kept because it is right, not because the
        // number moved.
        const KeyEstimate key = keyAmount > 0.0f ? findKey(pcWeight_) : KeyEstimate{};
        int best = root_; double bestScore = 1e9;
        for (int c = low; c <= high; ++c) {
            if (c == root_) continue;
            // Which steps the root may take at all (R6.2). Any is what it always did; the others
            // judge the step itself -- its size and its direction -- before anything else is
            // weighed. The ascending semitone goes out under every one of them, Any included
            // (Anti 8): it is heard as a lift, and this music has nothing to lift towards. It
            // stood inside the gate below until 12.09.2026, so Any still allowed it.
            const int step = c - root_, a = std::abs(step) % 12;
            if (step > 0 && a == 1) continue;
            if (p.rootSteps != 0) {
                if (p.rootSteps == 1 && a != 0 && a != 5 && a != 7) continue;                      // fifths and fourths
                if (p.rootSteps == 2 && a != 5 && a != 7 && a != 3 && a != 2 && !(step < 0 && a == 1)) continue;
                if (p.rootSteps == 3 && step > 0) continue;                                        // downwards only
            }
            double r = freqOf(c) / rootFreq;
            while (r >= 2.0) r *= 0.5;
            while (r < 1.0) r *= 2.0;
            // The interval the dice drew, and the size of the step. As the night ripens (Home)
            // the interval matters less and less: the way home is whatever step leads there,
            // not whatever the dice had in mind -- with the target in force to the end, the
            // music came within a step of home and stayed there, six hours in eight.
            const double homing = p.home > 0.0f
                ? static_cast<double>(p.home) * clampv(age_ / std::max(60.0, static_cast<double>(p.homeTime) * 60.0), 0.0, 1.0) : 0.0;
            double score = std::fabs(std::log2(r / target)) * 12.0 * (1.0 - homing) + std::fabs(c - root_) / 12.0;
            // A penalty, not a veto: the wander is what keeps the harmony moving at all.
            if (keyAmount > 0.0f) score += 2.0 * static_cast<double>(keyAmount) * (1.0 - keyWeightOf(freqOf(c), key, 1.0f));
            // A fifth down and a fifth up are one interval to the scoring above, and they are not
            // one move: downwards the music settles, upwards it climbs.
            if (p.rootDown != 0.0f) score += (step > 0 ? 1.5 : -1.5) * static_cast<double>(p.rootDown);
            // A root that turns a sounding note into its own leading note breaks Anti 6 the moment
            // it arrives, and nothing can be done about it afterwards: R4.7 forbids retuning what
            // already sounds. So it is decided here, where there is still a choice. Measured: two
            // or three such simultaneities an hour, all of them just after a root change.
            if (p.leading > 0.0f)
                for (const auto& s : slots_)
                    if (s.note >= 0 && ((s.note - c) % 12 + 12) % 12 == 11) { score += 2.0 * static_cast<double>(p.leading); break; }
            // The way home (R6.4). It grows with the hours since the night began and with how far
            // the root has travelled, so a piece left running returns to where it started without
            // ever being told to -- and a piece switched off after ten minutes never notices.
            if (p.home > 0.0f) {
                const double ripe = clampv(age_ / std::max(60.0, static_cast<double>(p.homeTime) * 60.0), 0.0, 1.0);
                const double away = std::fabs(static_cast<double>(root_ - homeRoot_)) / 12.0;
                const double closer = (std::fabs(static_cast<double>(c - homeRoot_))
                                     - std::fabs(static_cast<double>(root_ - homeRoot_))) / 12.0;
                score += static_cast<double>(p.home) * ripe * (1.0 + away) * closer * 3.0;
                // And the home root itself, ripe, outranks any interval the dice have in mind: the
                // pull alone brought the music CLOSER and left it a step off home at the hour's end
                // six times in eight, because home was seldom the interval that had been drawn.
                // Once home and ripe, it stays: leaving again at the fifty-fifth minute is what
                // happened next, four times in eight.
                if (c == homeRoot_) score -= static_cast<double>(p.home) * ripe * 6.0;
                // Once home and ripe, it stays -- against any step, not only a wide one: a penalty
                // that grew with the distance let a semitone slip out at the fifty-eighth minute.
                // Cubed, so that the first half of the night is not nailed to its root.
                if (away <= 0.0) score += static_cast<double>(p.home) * ripe * ripe * 12.0;
            }
            if (score < bestScore) { bestScore = score; best = c; }
        }
        // A move has to be worth making -- but the threshold is on a score that Home, Key and Root
        // Down all add penalties to, and with the three of them up every candidate can stand above
        // it: measured, an hour at the rule book's own values in which the root never moved once.
        // R6.1 wants it every four to twelve minutes, so past twelve the best candidate goes
        // through whatever it scores. A conductor whose Wander and Root Move are nought never asks
        // this question at all, and the profiles that want a fixed root keep it.
        // A move has to be worth making, and past twelve minutes the best candidate goes through
        // whatever it scores (R6.1's upper bound) -- except at home and late, where that release
        // was what let the root wander off again after it had come back: one hour in eight ended a
        // step away from where the night began, having been home at minute 33.
        {
            const double ripeNow = p.home > 0.0f
                ? clampv(age_ / std::max(60.0, static_cast<double>(p.homeTime) * 60.0), 0.0, 1.0) : 0.0;
            const bool restingAtHome = p.home > 0.0f && root_ == homeRoot_ && ripeNow > 0.55;
            if ((bestScore >= 3.5 && (rootAge_ < 720.0 || restingAtHome)) || best == root_) return;
        }
        ++rootMoves_;
        // The changeover (R6.3). Without Pivot the root simply moves, as it always did. With it the
        // new root is announced rather than declared: a tone belonging to both is started at once,
        // the root itself follows halfway through the window, and the voice on the old root is let
        // go only at the end of it.
        if (p.pivot > 0.0f) { pivotTo_ = best; pivotOld_ = root_; pivotLeft_ = static_cast<double>(p.pivot); pivotAnnounced_ = false; return; }
        setRoot(best, p);
    }

    /**
     * @brief Take the new root, and with it what the change is allowed to carry: a single exchanged degree
     * of the supply, so the mode wanders instead of being swapped (R4.1).
     *
     * The root's age starts over. With Degree Swap the dice may turn a major third or sixth into
     * the minor one or back (degreeBias_), which ruleWeight() applies from then on. One draw from
     * the stream when Degree Swap is up, two when it strikes.
     *
     * @param note  the new root, MIDI (a candidate wanderRoot() chose, or the destination of a changeover)
     * @param p     the knobs: Degree Swap
     */
    void setRoot(int note, const BrainParams& p)
    {
        root_ = note;
        rootAge_ = 0.0;
        if (p.degreeSwap > 0.0f && rng_.uniform() < p.degreeSwap) {
            const bool sixth = rng_.uniform() < 0.5f;
            const int lo = sixth ? 8 : 3, hi = sixth ? 9 : 4;   // minor/major sixth, minor/major third
            const bool toMajor = degreeBias_[lo] >= degreeBias_[hi];
            degreeBias_[lo] = toMajor ? 0.25f : 1.6f;
            degreeBias_[hi] = toMajor ? 1.6f : 0.25f;
        }
    }

    /**
     * @brief What a chord is, for the purpose of not hearing it twice: the pitch classes that sound,
     * together with the octave the bottom of it sits in.
     *
     * Two voicings of the same set in the same
     * register are the same chord; the same set an octave apart is not. `extra` is added if given. For a candidate (`forNew`) a voice that is on its way out -- exchanged, and
     * sounding out its overlap, which in Free mode is a negative `remaining` -- is left out: it
     * will not be in the constellation the candidate makes. Counted in, the memory judged a
     * four-note chord that lasted ten seconds and never the three-note one that followed, and a
     * voice could go A, B, A, B between two notes for an hour (Glacier Bloom, measured).
     *
     * @tparam FreqFn  callable `double(int note)`
     * @param extra   a MIDI note to count in as if it sounded (the candidate), or -1
     * @param freqOf  frequency of a MIDI note in the engine's current scale
     * @param forNew  true for the constellation a candidate would make, which leaves out the voices
     *                on their way out; false for the one sounding now, which counts them in
     * @return        bits 0 .. 11 the pitch classes, bits 12 and up the octave (MIDI / 12) of the lowest note
     */
    template <class FreqFn>
    uint32_t chordKey(int extra, FreqFn&& freqOf, bool forNew = false) const
    {
        uint32_t mask = 0;
        int lowest = 127;
        for (const auto& s : slots_) {
            if (s.note < 0 || (forNew && s.remaining < 0.0)) continue;
            mask |= 1u << pitchClassOf(freqOf(s.note)); lowest = std::min(lowest, s.note);
        }
        // Chords mode's exchanged voices, sounding out their overlap: in the constellation heard
        // now, not in the one a candidate will leave behind.
        if (!forNew) {
            for (const auto& l : leaving_) if (l.note >= 0) { mask |= 1u << pitchClassOf(freqOf(l.note)); lowest = std::min(lowest, l.note); }
            if (extraHeard_ >= 0) { mask |= 1u << pitchClassOf(freqOf(extraHeard_)); lowest = std::min(lowest, extraHeard_); }
        }
        if (extra >= 0) { mask |= 1u << pitchClassOf(freqOf(extra)); lowest = std::min(lowest, extra); }
        return mask | (static_cast<uint32_t>(clampv(lowest, 0, 127) / 12) << 12);
    }
    /**
     * @brief Whether a constellation was stamped in the memory within the last Memory minutes.
     * @param k  a chordKey()
     * @param p  the knobs: Memory, in minutes
     * @return   true when an entry with that key is younger than the memory
     */
    bool heardLately(uint32_t k, const BrainParams& p) const
    {
        const double within = static_cast<double>(p.memory) * 60.0;
        for (const auto& m : memo_) if (m.key == k && now_ - m.at < within) return true;
        return false;
    }
    /**
     * @brief Whether adding `c` would rebuild a constellation heard within the memory.
     *
     * A note that adds no
     * pitch class and lowers nothing -- an octave or a unison of what sounds -- makes no new
     * constellation, it thickens the one there is, and R6.6 is about constellations; asked of it,
     * the memory forbade every doubling, and the octave the rule book's table has at 0.18 came out
     * at 0.06.
     *
     * Reports its verdict to the trace hook when one is set.
     *
     * @tparam FreqFn  callable `double(int note)`
     * @param c       the candidate MIDI note
     * @param p       the knobs: Memory; at 0 nothing is ever heard lately
     * @param freqOf  frequency of a MIDI note in the engine's current scale
     * @return        true to strike the candidate out
     */
    template <class FreqFn>
    bool constellationHeard(int c, const BrainParams& p, FreqFn&& freqOf) const
    {
        if (p.memory <= 0.0f) return false;
        const uint32_t k = chordKey(c, freqOf, true);
        if (k == chordKey(-1, freqOf, true)) return false;
        // Both the constellation the candidate makes and the one it makes for the ten seconds
        // an exchanged voice is still sounding: the listener hears that one too.
        const uint32_t during = chordKey(c, freqOf, false);
        const bool heard = heardLately(k, p) || (during != k && heardLately(during, p));
        if (trace) trace(heard ? "veto" : "pass", c, k);
        return heard;
    }
    /**
     * @brief Stamps a constellation in the memory as sounding now.
     *
     * A constellation is remembered from the last moment it sounded, not from the moment it was
     * made: stamped when it is created and again whenever a note leaves it, so the ten minutes of
     * R6.6 run from its end. Stamped at creation only, a chord that had lasted eleven minutes
     * could come straight back.
     *
     * An entry already there is re-stamped in place; a new one takes the oldest place of the ring.
     * @param k  a chordKey()
     */
    void rememberChord(uint32_t k)
    {
        if (trace) trace("stamp", -1, k);
        for (auto& m : memo_) if (m.key == k) { m.at = now_; return; }
        memo_[memoHead_] = Memo{ k, now_ };
        memoHead_ = (memoHead_ + 1) % kMemo;
    }

public:
    /**
     * @brief A hook for the audit tool: every stamp of the memory and every verdict it gives, so that a
     * constellation that comes back can be traced to the check that let it.
     *
     * Off unless set.
     *
     * Tools/render/brain_audit.cpp sets it. `what` is "stamp" (rememberChord), "veto" or "pass"
     * (constellationHeard); `note` is the candidate, or -1 for a stamp; `key` is the chordKey().
     * One pointer for every conductor in the process.
     */
    static inline void (*trace)(const char* what, int note, uint32_t key) = nullptr;
private:

    /**
     * @brief How long until the next event.
     *
     * The conductor's own rate, unless the cluster is being
     * filled on request (see requestFill), in which case it is short enough to arrive as music.
     *
     * @param p  the knobs: Event Rate, Rate Breath
     * @return   the mean gap in seconds the next wait is drawn against: 0.35 while filling, else
     *           the rate (at least half a second) swung by the breath
     */
    double tickSeconds(const BrainParams& p) const
    {
        // Density is read as a CEILING here, not as a target: the cluster grows by one note an
        // event and loses notes to their own Hold, so the number that sounds settles at
        // Hold / Event Rate. Measured over the 7514 presets with a conductor that equilibrium
        // is a median of 2.8 voices against a median Density of 4, and 4529 of them -- sixty
        // per cent -- can never reach the number they ask for. Filling faster while short was
        // tried and does work, but it costs Even most of its effect (0.664 -> 0.768 became
        // 0.685 -> 0.694): with the chord always full, notes are only ever chosen at an
        // exchange, and that is where Even has least to decide. Left as it is until the two
        // can be had together.
        if (filling_) return 0.35;
        // The mean gap breathes. A constant mean is the one thing a Poisson clock cannot hide: the
        // density is then the same at minute three and at minute fifty, and the piece has no
        // shape. At 1 the mean swings between half and double over its period, which for the
        // library's rates is a slow tide rather than a change of tempo.
        double mean = std::max(0.5, static_cast<double>(p.rateSeconds));
        if (p.rateBreath > 0.0f) mean *= std::pow(2.0, static_cast<double>(p.rateBreath * breathValue_));
        return mean;
    }

    /**
     * @name The Hawkes clock
     *
     * The Hawkes clock. Time runs faster for the timer while the excitation is up: an
     * inhomogeneous Poisson process is a homogeneous one in rescaled time, so the gap is drawn
     * exactly as before and only consumed faster. The excitation decays with a time constant of
     * half the mean gap, and each event adds enough that at full Cascade one event breeds 0.65
     * further ones on average (kick = branching * mean / tau = 0.65 * 2). Short-circuited at
     * zero and at rest, so a conductor without Cascade subtracts dt as it always did.
     * @{ */
    /**
     * @brief Runs the timer down by `dt`, faster while the excitation is up, and lets the excitation decay.
     * @param dt    seconds since the last tick
     * @param p     the knobs: Cascade
     * @param mean  the mean gap in seconds, whose half is the excitation's time constant
     */
    void advanceTimer(double dt, const BrainParams& p, double mean)
    {
        if (p.cascade > 0.0f || excite_ > 0.0) {
            excite_ *= std::exp(-dt / (0.5 * mean));
            if (excite_ < 1.0e-4) excite_ = 0.0;
            timer_ -= dt * (1.0 + excite_);
        } else timer_ -= dt;
    }
    /**
     * @brief An event happened: lifts the excitation by 1.3 times Cascade, the jump that gives the branching ratio of 0.65 at full.
     * @param p  the knobs: Cascade; nothing happens at 0
     */
    void kick(const BrainParams& p) { if (p.cascade > 0.0f) excite_ += 1.3 * static_cast<double>(p.cascade); }
    /** @} */

    /**
     * @brief The interval histogram the homeostat reads: which of the twelve interval classes the
     * last choice made against the one before it.
     *
     * It fades by 0.92 per event, so it remembers
     * the last dozen or so.
     *
     * Called with every note the conductor chooses or inherits; the first note after reset()
     * only sets the reference.
     * @param note  the MIDI note just chosen
     */
    void remember(int note)
    {
        if (lastNote_ >= 0) {
            for (float& w : ic_) w *= 0.92f;
            ic_[((note - lastNote_) % 12 + 12) % 12] += 1.0f;
        }
        lastNote_ = note;
    }
    /**
     * @brief Where the homeostat leans: + when the music is more predictable than Surprise asks for, - when it is less, scaled by Homeostat.
     *
     * Needs a few events of history to say anything.
     *
     * @param p  the knobs: Surprise, Homeostat
     * @return   -1 .. 1 times Homeostat; 0 with Homeostat off or fewer than three events of history
     */
    float leanOf(const BrainParams& p) const
    {
        if (p.homeostat <= 0.0f) return 0.0f;
        float total = 0.0f;
        for (float w : ic_) total += w;
        if (total < 3.0f) return 0.0f;
        const double h = entropyBits() / std::log2(12.0);
        return clampv(static_cast<float>((p.surprise - h) * 2.0), -1.0f, 1.0f) * p.homeostat;
    }

    /**
     * @brief Whether a note may sound at all under the rules that every draw applies: in range, not
     * resting after its release (R7.4), not rebuilding a constellation heard lately (R6.6), and
     * not vetoed by the register and interval rules.
     *
     * For the paths that do not draw -- the ring
     * of Deja Vu offers a PAST note, and until this was asked of it the offer went past every
     * rule: a second in the bass, a leading note under a sounding root, a pitch inside its rest.
     *
     * @tparam FreqFn  callable `double(int note)`
     * @param c        the note offered, MIDI
     * @param low      one end of the register, MIDI (either order)
     * @param high     the other end
     * @param p        the knobs
     * @param freqOf   frequency of a MIDI note in the engine's current scale
     * @param pivotal  true for the pivot tone of a changeover, which may be the leading note
     * @return         true when the note may be started
     */
    template <class FreqFn>
    bool admissible(int c, int low, int high, const BrainParams& p, FreqFn&& freqOf, bool pivotal = false) const
    {
        if (c < std::min(low, high) || c > std::max(low, high) || c < 0 || c > 127) return false;
        if (p.retrigger > 0.0f && now_ - pitchOffAt_[static_cast<size_t>(c)] < static_cast<double>(p.retrigger)) return false;
        if (constellationHeard(c, p, freqOf)) return false;
        bool rootSounding = false;
        for (const auto& s : slots_) if (s.note >= 0 && ((s.note - root_) % 12 + 12) % 12 == 0) rootSounding = true;
        return ruleWeight(c, std::min(low, high), std::max(low, high), rootSounding, p, pivotal) > 0.0f;
    }

    /**
     * @brief The deja-vu ring.
     *
     * Moves one place per choice; offers what it holds with probability
     * Deja Vu, and keeps it when it was taken. Nothing is drawn at zero.
     *
     * Every path that chooses a note passes its choice through here last. The ring is Loop places
     * long (at most kRing); the offered note must be free, admissible and not the one being
     * replaced, else the fresh note plays and is written into the ring in its stead. One draw from
     * the stream when Deja Vu is up.
     *
     * @tparam FreqFn  callable `double(int note)`
     * @param fresh   the note the draw chose, MIDI, or -1 (returned unchanged)
     * @param p       the knobs: Deja Vu and Loop
     * @param avoid   Chords: the note being replaced, which the ring may not offer back; -1 in Free mode
     * @param low     one end of the register, for admissible()
     * @param high    the other end
     * @param freqOf  frequency of a MIDI note in the engine's current scale
     * @return        the note to play: the ring's when it was offered and allowed, else `fresh`
     */
    template <class FreqFn>
    int viaDejaVu(int fresh, const BrainParams& p, int avoid, int low, int high, FreqFn&& freqOf)
    {
        if (p.dejavu <= 0.0f || fresh < 0) return fresh;
        const int len = clampv(p.loop, 1, kRing);
        ringPos_ = (ringPos_ + 1) % len;
        int out = fresh;
        if (ring_[ringPos_] >= 0 && rng_.uniform() < p.dejavu) {
            const int old = ring_[ringPos_];
            if (old != avoid && !sounding(old) && admissible(old, low, high, p, freqOf)) out = old;
        }
        ring_[ringPos_] = out;
        return out;
    }

    Slot   slots_[kSlots];        ///< the cluster: one voice per slot, note -1 when empty
    mutable Rng rng_;             ///< the conductor's own random stream, seeded by reset(); mutable so that chooseNote() can add its little life while const
    static constexpr int kRing = 16;   ///< places in the deja-vu ring; Loop may use fewer
    int    ring_[kRing] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };   ///< the deja-vu ring: the last notes chosen, -1 for an empty place
    int    ringPos_ = 0;          ///< the place the last choice was written to
    double excite_ = 0.0;         ///< Cascade: the rate's excitation, in multiples of the base rate
    float  ic_[12] = {};          ///< Homeostat: fading histogram of chosen interval classes
    int    lastNote_ = -1;        ///< the note chosen before this one, for the interval histogram and Smooth; -1 after reset()
    float  lastLean_ = 0.0f;      ///< the homeostat's lean as of the last decision, for lean() and the Free-mode draw
    /**
     * @name The clocks the guards read (12.09.2026)
     *
     * The clocks the guards read (12.09.2026). Seconds since the last onset and the last release,
     * and the moment each of the 128 pitches was last let go. The running clock starts at a
     * million seconds rather than at zero so that a zeroed stamp reads as "long ago": every pitch
     * is free before it has ever sounded, without filling 128 entries by hand.
     * @{ */
    /** @brief Seconds since the last onset, and seconds since the last release. */
    double sinceOn_ = 1.0e9, sinceOff_ = 1.0e9;   ///< seconds since the last onset and since the last release; huge until something has happened
    double now_ = 1.0e6;          ///< the running clock in seconds, from a million at reset()
    double pitchOffAt_[128] = {}; ///< now_ at the moment each MIDI pitch was last let go, 0 for never
    /** @} */
    /**
     * @name The breath behind the event rate
     *
     * The breath behind the event rate: two sines whose periods stand in the golden ratio, so the
     * sum never repeats within a piece and the tide does not become a pulse.
     * @{ */
    double breathPhase_ = 0.0;    ///< phase of the slower sine in cycles of Breath Period; wraps at a million
    float  breathValue_ = 0.0f;   ///< the breath's current value, -1 .. 1, applied as an exponent of two to the mean gap
    /** @} */
    double densityNow_ = -1.0;    ///< the density actually in force; negative until the first tick
    double densityTarget_ = -1.0; ///< the density the glide is heading for, the knob's value
    double densityStep_ = 0.0;    ///< voices per second while a change is being carried out
    double silenceLeft_ = 0.0;    ///< seconds of planned pause still to run
    /** @brief Hold Onsets and Hold Root, as the near layer last set them. */
    bool   onsetHold_ = false, rootHold_ = false;   ///< the foreground's asks (holdOnsets, holdRoot)
    // ---- root and long form
    int    homeRoot_ = 48;        ///< the root the night began on
    double age_ = 0.0;            ///< seconds since the conductor was reset
    double rootAge_ = 0.0;        ///< seconds since the root last moved
    /** @brief The changeover's destination root, and the root it leaves. */
    int    pivotTo_ = -1, pivotOld_ = -1;   ///< the changeover's destination root and the root it leaves; -1 when none is running
    double pivotLeft_ = 0.0;      ///< seconds left of the changeover window
    bool   pivotAnnounced_ = false;   ///< the changeover's pivot tone has been looked for (found or not)
    int    pivotTones_ = 0;       ///< changeovers that found a tone belonging to both roots
    int    rootMoves_ = 0;        ///< times the root was sent somewhere else
    bool   settled_ = false;      ///< the cluster has once stood at its density: the exposition is over
    int    extraHeard_ = -1;      ///< Chords: the voice being exchanged, out of its slot for the vote, still heard by the rules
    /**
     * @brief Chords: a voice that has been exchanged keeps sounding until the one replacing it has been
     * in the air for Overlap seconds (R5.4), and only then is let go.
     *
     * Its slot has gone to the
     * arriving note, so it waits here -- and while it waits it still counts as sounding.
     */
    struct Leaving { int note = -1; double in = 0.0; };
    /** @var int ClusterBrain::Leaving::note
     *  @brief the MIDI note sounding out its overlap, -1 for an empty place */
    /** @var double ClusterBrain::Leaving::in
     *  @brief seconds until it may be let go */
    Leaving leaving_[kSlots];     ///< the voices on their way out, one place per slot at most
    float  degreeBias_[12] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };   ///< Degree Swap: a weight per interval class over the root, 1 until a root change swaps a third or a sixth (0.25 against 1.6)
    /** @brief One entry of the constellation memory. */
    struct Memo { uint32_t key; double at; };
    /** @var uint32_t ClusterBrain::Memo::key
     *  @brief the chordKey() of the constellation */
    /** @var double ClusterBrain::Memo::at
     *  @brief now_ when it last sounded */
    /**
     * @brief One entry per constellation, kept from the last moment it sounded.
     *
     * An hour has a hundred to
     * two hundred of them, and at 48 the ring overwrote entries younger than the memory: a chord
     * three minutes gone came back, twice an hour under the rules at full. Thirty minutes of
     * memory at the rule book's rates want a few hundred.
     */
    static constexpr int kMemo = 256;
    Memo   memo_[kMemo] = {};     ///< the constellation memory, a ring of kMemo entries
    int    memoHead_ = 0;         ///< the next place of the ring a new constellation takes
    double timer_ = 1.0;          ///< seconds until the next decision, run down by advanceTimer()
    double timerMean_ = 0.0;   ///< the rate the standing wait was drawn against, so it can be rescaled
    bool   filling_ = false;   ///< fill the cluster at speed, then go back to the event rate
    int    root_ = 48;            ///< the root the cluster is built around, MIDI
    bool   wasOn_ = false;        ///< the conductor was on at the last tick, so a switch-off releases the voices once
    bool   stepRequested_ = false;   ///< requestStep() has asked for an exchange that has not happened yet
    static constexpr int kRecent = 8;   ///< Chords: the notes that left, most recent first-ish
    float  pcWeight_[12] = {};    ///< how long each pitch class has been sounding, faded
    int    recent_[kRecent] = { -1, -1, -1, -1, -1, -1, -1, -1 };   ///< Chords: the notes that left recently, a ring of kRecent, -1 for empty
    int    recentHead_ = 0;       ///< the next place of that ring
};

} // namespace ambient
