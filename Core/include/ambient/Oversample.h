/**
 * @file Oversample.h
 * @brief Oversampling by linear-phase half-band stages: four times around a memoryless nonlinearity,
 *        two times around a stereo circuit.
 *
 * A saturation, a clipper or a wavefolder makes partials the sample rate cannot hold, and they fold
 * back below Nyquist as tones in no harmonic relation to anything -- the "digital grey" the
 * production guide warns about (25.09.2026, section 3: 4x on everything nonlinear). The cure is to
 * run the curve at four times the rate, where the new partials have room, and to filter them away
 * before coming back down.
 *
 * Two half-band stages of two times each, rather than one of four: the first carries the steep
 * transition around the original Nyquist frequency (51 taps, 80 dB, pass band to 0.8 of Nyquist),
 * the second only has to separate the images a whole octave apart (19 taps). A half-band filter
 * has every second tap at zero and its centre at one half, so each stage costs one multiply per
 * non-zero tap on one polyphase branch and a plain delay on the other. Both are Kaiser-windowed
 * sincs normalised to unity gain at DC, designed once in the constructor -- not on the audio
 * thread, and not as a static a first call would have to build under a lock.
 *
 * The price is latency: 29.5 samples at the base rate, about 0.6 ms at 48 kHz. Every place that
 * uses it is either the whole signal (the master, the Patina, the feedback loop, the air ahead of
 * the far hall) or the whole of one voice after its filters, so nothing is summed against an
 * undelayed copy of itself -- with one exception the voice documents (the filter's parallel mode).
 *
 * One Oversampler4 is one mono channel; a stereo stage owns two. StereoOversampler2 (26.09.2026) is
 * the first stage alone for both channels at once, for the voice filter's circuit models: they are
 * filters with memory rather than a curve, their own stages low-pass what their saturation makes,
 * and they are dear enough that twice the rate is what Ephemeris runs them at too. Its latency is
 * 25 samples.
 *
 * **Cost** (26.09.2026). The taps' sums are the stages' whole work. They are vector sums now (AVX2,
 * NEON, or four scalar accumulators), which adds the same products in a different order: the result
 * moves in the last bit or two. A vector sum over a history that has just been written stalls,
 * though -- a wide load cannot be served from the narrow store still on its way to the cache, and
 * the first attempt, over the doubled rings the stages used to keep, came out twice as slow as the
 * scalar sums it replaced. So each history is now a Line: the newest sample is multiplied on its
 * own, the vector sum reads only samples written a call or more ago, and the line runs down a
 * buffer and jumps back to its top every 64 samples instead of wrapping.
 */
#pragma once
#include "Simd.h"
#include <cmath>
#include <cstring>

namespace ambient {

/**
 * @brief The half-band stages' shared parts: the design, the vector sum, the history and the stage.
 */
namespace halfband {

constexpr int kPad = 8;   ///< the vector width the taps and the sums are padded to

/**
 * @brief A length rounded up to whole vectors.
 * @param n  the length
 * @return   n rounded up to a multiple of kPad
 */
constexpr int padded(int n) { return (n + kPad - 1) / kPad * kPad; }

/**
 * @brief The modified Bessel function of the first kind, order zero, by its power series.
 * @param x  the argument, 0 .. 20 in practice
 * @return   I0(x)
 */
inline double besselI0(double x)
{
    double sum = 1.0, term = 1.0;
    const double q = 0.25 * x * x;
    for (int k = 1; k < 64; ++k) {
        term *= q / (static_cast<double>(k) * k);
        sum += term;
        if (term < 1.0e-12 * sum) break;
    }
    return sum;
}

/**
 * @brief A Kaiser-windowed half-band low-pass, keeping only its even-indexed taps.
 *
 * h[n] = 0.5 sinc((n - c) / 2) w(n), c = (N - 1) / 2 odd. The odd-indexed taps are zero except
 * the centre, which is exactly one half; the even-indexed ones are stored, scaled so that they
 * sum to one half, which puts the DC gain of the whole filter at one.
 *
 * @param out  receives the (N + 1) / 2 even-indexed taps, h[0], h[2], ..., h[N - 1]
 * @param n    the length, 4m + 3, at most 51
 */
inline void design(float* out, int n)
{
    constexpr double kPiD = 3.14159265358979323846;
    const double beta = 7.857;   // Kaiser, 80 dB of stop-band attenuation
    const double c = 0.5 * (n - 1);
    double sum = 0.0;
    double tmp[26];
    int j = 0;
    for (int i = 0; i < n; i += 2) {
        const double t = (i - c) * 0.5;   // odd-half values: the sinc's non-zero samples
        const double sinc = std::sin(kPiD * t) / (kPiD * t);
        const double r = (i - c) / c;
        const double w = besselI0(beta * std::sqrt(std::fmax(0.0, 1.0 - r * r))) / besselI0(beta);
        tmp[j] = 0.5 * sinc * w;
        sum += tmp[j];
        ++j;
    }
    for (int k = 0; k < j; ++k) out[k] = static_cast<float>(tmp[k] * 0.5 / sum);
}

/**
 * @brief The sum of h[k] v[k] over a padded length, as a vector sum.
 * @param h  the taps, n of them (zero past the real ones)
 * @param v  the history, n readable floats
 * @param n  a multiple of kPad
 * @return   the sum
 */
inline float dot(const float* h, const float* v, int n)
{
#if AMBIENT_HAS_AVX
    __m256 acc = _mm256_mul_ps(_mm256_loadu_ps(h), _mm256_loadu_ps(v));
    for (int k = 8; k < n; k += 8) acc = _mm256_fmadd_ps(_mm256_loadu_ps(h + k), _mm256_loadu_ps(v + k), acc);
    return horizontalSum(acc);
#elif AMBIENT_HAS_NEON
    float32x4_t a = vmulq_f32(vld1q_f32(h), vld1q_f32(v)), b = vmulq_f32(vld1q_f32(h + 4), vld1q_f32(v + 4));
    for (int k = 8; k < n; k += 8) {
        a = vmlaq_f32(a, vld1q_f32(h + k), vld1q_f32(v + k));
        b = vmlaq_f32(b, vld1q_f32(h + k + 4), vld1q_f32(v + k + 4));
    }
    return horizontalSum(vaddq_f32(a, b));
#else
    float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
    for (int k = 0; k < n; k += 4) {
        a0 += h[k] * v[k]; a1 += h[k + 1] * v[k + 1]; a2 += h[k + 2] * v[k + 2]; a3 += h[k + 3] * v[k + 3];
    }
    return (a0 + a1) + (a2 + a3);
#endif
}

/**
 * @brief A sample history that a vector sum can read without waiting for the newest store: the
 *        samples run down a buffer, and every kCap samples the last P of them are copied back to
 *        its top.
 * @tparam P  how many samples before the newest a sum reads (a multiple of kPad)
 */
template <int P>
struct Line {
    static constexpr int kCap = 64;       ///< samples between two jumps back to the top
    float buf[kCap + 1 + P] = {};         ///< the history, newest at buf[pos]
    int   pos = kCap;                     ///< where the newest sample is

    /** @brief Silence. */
    void reset() { std::memset(buf, 0, sizeof(buf)); pos = kCap; }
    /** @brief Takes the newest sample. @param x the sample */
    void push(float x)
    {
        if (pos == 0) { std::memcpy(buf + kCap + 1, buf, P * sizeof(float)); pos = kCap; }
        else --pos;
        buf[pos] = x;
    }
    /** @brief A sample of the history. @param k how many samples ago, 0 .. P @return it */
    float at(int k) const { return buf[pos + k]; }
    /** @brief The P samples before the newest, oldest last: what a sum reads. @return their start */
    const float* older() const { return buf + pos + 1; }
};

/**
 * @brief One half-band stage of Taps taps: its design, and the two directions through it.
 * @tparam Taps  the length, 4m + 3
 */
template <int Taps>
struct Stage {
    static constexpr int kSide = (Taps + 1) / 2;   ///< non-zero taps of the filtering branch (the even-indexed ones)
    static constexpr int kM = (Taps - 3) / 4;      ///< the pure-delay branch's delay in input samples
    static constexpr int kP = padded(kSide - 1);   ///< the taps after the first, padded: what the vector sum reads
    using History = Line<kP>;                      ///< the history one direction of the stage keeps

    float h0 = 0.0f;       ///< the first even-indexed tap, the newest sample's
    float hv[kP] = {};     ///< the rest, zero past kSide - 1

    /** @brief Designs the stage. Allocation-free. */
    Stage()
    {
        float t[26];
        design(t, Taps);
        h0 = t[0];
        for (int k = 1; k < kSide; ++k) hv[k - 1] = t[k];
    }
    /**
     * @brief One sample in, two out at twice the rate: y[2n] from the filtering branch, y[2n+1]
     *        the input delayed by kM samples (the centre tap, one half, times the gain of two).
     * @param line  the input's history
     * @param x     the input sample
     * @param y     receives the two output samples, in time order
     */
    void up(History& line, float x, float* y) const
    {
        line.push(x);
        y[0] = 2.0f * (h0 * x + dot(hv, line.older(), kP));
        y[1] = line.at(kM);
    }
    /**
     * @brief Two samples in at twice the rate, one out: the even one through the filtering branch,
     *        the odd one through the centre tap, kM + 1 samples late.
     * @param e     the even samples' history
     * @param o     the odd samples' history
     * @param even  the first sample of the pair, v[2n]
     * @param odd   the second, v[2n + 1]
     * @return      w[n] = sum h[2j] v[2n - 2j] + 0.5 v[2(n - kM - 1) + 1]
     */
    float down(History& e, History& o, float even, float odd) const
    {
        e.push(even);
        o.push(odd);
        return h0 * even + dot(hv, e.older(), kP) + 0.5f * o.at(kM + 1);
    }
};

using Stage1 = Stage<51>;   ///< 4m + 3 with m = 12: 80 dB, transition 0.2 .. 0.3 of its rate
using Stage2 = Stage<19>;   ///< 4m + 3 with m = 4: the images are an octave away

} // namespace halfband

/**
 * @brief Four-times oversampler around a memoryless curve, for one channel.
 */
class Oversampler4 {
public:
    /** @brief The delay the round trip adds, in samples at the base rate: (51 - 1) / 2 + (19 - 1) / 4. */
    static constexpr float kLatency = 29.5f;

    /** @brief Clears the four filters' histories: the next sample starts from silence. */
    void reset()
    {
        up1_.reset(); up2_.reset(); dn2e_.reset(); dn2o_.reset(); dn1e_.reset(); dn1o_.reset();
        evenHeld_ = 0.0f;
    }

    /**
     * @brief One sample through the curve at four times the rate.
     *
     * @tparam F  callable `float(float)`: the memoryless curve, evaluated four times per call
     * @param x   the input sample at the base rate
     * @param f   the curve
     * @return    f(x), band-limited and kLatency samples late
     */
    template <class F>
    float process(float x, F&& f)
    {
        float a[2];
        s1_.up(up1_, x, a);
        float out = 0.0f;
        for (int k = 0; k < 2; ++k) {
            float b[2];
            s2_.up(up2_, a[k], b);
            const float c = s2_.down(dn2e_, dn2o_, f(b[0]), f(b[1]));
            // Stage one comes down from the pair of stage-two outputs, the first even, the second odd.
            if (k == 0) evenHeld_ = c;
            else out = s1_.down(dn1e_, dn1o_, evenHeld_, c);
        }
        return out;
    }

private:
    halfband::Stage1 s1_;   ///< stage one's taps (designed in the constructor)
    halfband::Stage2 s2_;   ///< stage two's
    halfband::Stage1::History up1_,    ///< stage one's upsampler history
                              dn1e_,   ///< stage one's downsampler history, even samples
                              dn1o_;   ///< and odd ones
    halfband::Stage2::History up2_,    ///< stage two's upsampler history
                              dn2e_,   ///< stage two's downsampler history, even samples
                              dn2o_;   ///< and odd ones
    float evenHeld_ = 0.0f;   ///< the first stage-two output of a pair, waiting for its partner
};

/**
 * @brief Two-times oversampler around a stereo process with memory: stage one of Oversampler4 alone,
 *        for both channels, the process called once per sample at twice the rate with both.
 */
class StereoOversampler2 {
public:
    /** @brief The delay the round trip adds, in samples at the base rate: (51 - 1) / 2. */
    static constexpr float kLatency = 25.0f;

    /** @brief Clears both channels' histories. */
    void reset()
    {
        for (int c = 0; c < 2; ++c) { up_[c].reset(); dnE_[c].reset(); dnO_[c].reset(); }
    }

    /**
     * @brief One stereo sample through the process at twice the rate.
     *
     * @tparam F  callable `void(float l, float r, float& yl, float& yr)`, called twice per call, in
     *            time order: the process, which keeps its own state
     * @param l   the left sample at the base rate, replaced by the process's output, kLatency late
     * @param r   the right sample, the same
     * @param f   the process
     */
    template <class F>
    void process(float& l, float& r, F&& f)
    {
        float a[2], b[2], y0[2], y1[2];
        s_.up(up_[0], l, a);
        s_.up(up_[1], r, b);
        f(a[0], b[0], y0[0], y0[1]);
        f(a[1], b[1], y1[0], y1[1]);
        l = s_.down(dnE_[0], dnO_[0], y0[0], y1[0]);
        r = s_.down(dnE_[1], dnO_[1], y0[1], y1[1]);
    }

private:
    halfband::Stage1 s_;   ///< the stage's taps
    halfband::Stage1::History up_[2],    ///< the upsamplers' histories, per channel
                              dnE_[2],   ///< the downsamplers' histories, even samples
                              dnO_[2];   ///< and odd ones
};

} // namespace ambient
