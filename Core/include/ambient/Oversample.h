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

constexpr int Pad = 8;   ///< the vector width the taps and the sums are padded to

/// The path dot() was compiled with, for Tests/filtertest.cpp.
#if AMBIENT_HAS_AVX
constexpr const char* DotPath = "avx2";
#elif AMBIENT_HAS_NEON
constexpr const char* DotPath = "neon";
#else
constexpr const char* DotPath = "scalar";
#endif

/**
 * @brief A length rounded up to whole vectors.
 * @param n  the length
 * @return   n rounded up to a multiple of Pad
 */
constexpr int padded(int n) { return (n + Pad - 1) / Pad * Pad; }

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
    constexpr double PiD = 3.14159265358979323846;
    const double beta = 7.857;   // Kaiser, 80 dB of stop-band attenuation
    const double c = 0.5 * (n - 1);
    double sum = 0.0;
    double tmp[26];
    int j = 0;
    for (int i = 0; i < n; i += 2) {
        const double t = (i - c) * 0.5;   // odd-half values: the sinc's non-zero samples
        const double sinc = std::sin(PiD * t) / (PiD * t);
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
 * @param n  a multiple of Pad
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
 *        samples run down a buffer, and every Cap samples the last P of them are copied back to
 *        its top.
 * @tparam P  how many samples before the newest a sum reads (a multiple of Pad)
 */
template <int P>
struct Line {
    static constexpr int Cap = 64;       ///< samples between two jumps back to the top
    float m_buf[Cap + 1 + P] = {};         ///< the history, newest at buf[pos]
    int   m_pos = Cap;                     ///< where the newest sample is

    /** @brief Silence. */
    void reset() { std::memset(m_buf, 0, sizeof(m_buf)); m_pos = Cap; }
    /** @brief Takes the newest sample. @param x the sample */
    void push(float x)
    {
        if (m_pos == 0) { std::memcpy(m_buf + Cap + 1, m_buf, P * sizeof(float)); m_pos = Cap; }
        else --m_pos;
        m_buf[m_pos] = x;
    }
    /** @brief A sample of the history. @param k how many samples ago, 0 .. P @return it */
    float at(int k) const { return m_buf[m_pos + k]; }
    /** @brief The P samples before the newest, oldest last: what a sum reads. @return their start */
    const float* older() const { return m_buf + m_pos + 1; }
};

/**
 * @brief One half-band stage of Taps taps: its design, and the two directions through it.
 * @tparam Taps  the length, 4m + 3
 */
template <int Taps>
struct Stage {
    static constexpr int Side = (Taps + 1) / 2;   ///< non-zero taps of the filtering branch (the even-indexed ones)
    static constexpr int M = (Taps - 3) / 4;      ///< the pure-delay branch's delay in input samples
    static constexpr int P = padded(Side - 1);   ///< the taps after the first, padded: what the vector sum reads
    using HistoryT = Line<P>;                      ///< the history one direction of the stage keeps

    float m_h0 = 0.0f;       ///< the first even-indexed tap, the newest sample's
    float m_hv[P] = {};     ///< the rest, zero past Side - 1

    /** @brief Designs the stage. Allocation-free. */
    Stage()
    {
        float t[26];
        design(t, Taps);
        m_h0 = t[0];
        for (int k = 1; k < Side; ++k) m_hv[k - 1] = t[k];
    }
    /**
     * @brief One sample in, two out at twice the rate: y[2n] from the filtering branch, y[2n+1]
     *        the input delayed by M samples (the centre tap, one half, times the gain of two).
     * @param line  the input's history
     * @param x     the input sample
     * @param y     receives the two output samples, in time order
     */
    void up(HistoryT& line, float x, float* y) const
    {
        line.push(x);
        y[0] = 2.0f * (m_h0 * x + dot(m_hv, line.older(), P));
        y[1] = line.at(M);
    }
    /**
     * @brief Two samples in at twice the rate, one out: the even one through the filtering branch,
     *        the odd one through the centre tap, M + 1 samples late.
     * @param e     the even samples' history
     * @param o     the odd samples' history
     * @param even  the first sample of the pair, v[2n]
     * @param odd   the second, v[2n + 1]
     * @return      w[n] = sum h[2j] v[2n - 2j] + 0.5 v[2(n - M - 1) + 1]
     */
    float down(HistoryT& e, HistoryT& o, float even, float odd) const
    {
        e.push(even);
        o.push(odd);
        return m_h0 * even + dot(m_hv, e.older(), P) + 0.5f * o.at(M + 1);
    }
};

using Stage1T = Stage<51>;   ///< 4m + 3 with m = 12: 80 dB, transition 0.2 .. 0.3 of its rate
using Stage2T = Stage<19>;   ///< 4m + 3 with m = 4: the images are an octave away

} // namespace halfband

/**
 * @brief Four-times oversampler around a memoryless curve, for one channel.
 */
class Oversampler4 {
public:
    /** @brief The delay the round trip adds, in samples at the base rate: (51 - 1) / 2 + (19 - 1) / 4. */
    static constexpr float Latency = 29.5f;

    /** @brief Clears the four filters' histories: the next sample starts from silence. */
    void reset()
    {
        m_up1.reset(); m_up2.reset(); m_dn2e.reset(); m_dn2o.reset(); m_dn1e.reset(); m_dn1o.reset();
        m_evenHeld = 0.0f;
    }

    /**
     * @brief One sample through the curve at four times the rate.
     *
     * @tparam F  callable `float(float)`: the memoryless curve, evaluated four times per call
     * @param x   the input sample at the base rate
     * @param f   the curve
     * @return    f(x), band-limited and Latency samples late
     */
    template <class FT>
    float process(float x, FT&& f)
    {
        float a[2];
        m_s1.up(m_up1, x, a);
        float out = 0.0f;
        for (int k = 0; k < 2; ++k) {
            float b[2];
            m_s2.up(m_up2, a[k], b);
            const float c = m_s2.down(m_dn2e, m_dn2o, f(b[0]), f(b[1]));
            // Stage one comes down from the pair of stage-two outputs, the first even, the second odd.
            if (k == 0) m_evenHeld = c;
            else out = m_s1.down(m_dn1e, m_dn1o, m_evenHeld, c);
        }
        return out;
    }

private:
    halfband::Stage1T m_s1;   ///< stage one's taps (designed in the constructor)
    halfband::Stage2T m_s2;   ///< stage two's
    halfband::Stage1T::HistoryT m_up1,    ///< stage one's upsampler history
                              m_dn1e,   ///< stage one's downsampler history, even samples
                              m_dn1o;   ///< and odd ones
    halfband::Stage2T::HistoryT m_up2,    ///< stage two's upsampler history
                              m_dn2e,   ///< stage two's downsampler history, even samples
                              m_dn2o;   ///< and odd ones
    float m_evenHeld = 0.0f;   ///< the first stage-two output of a pair, waiting for its partner
};

/**
 * @brief Two-times oversampler around a stereo process with memory: stage one of Oversampler4 alone,
 *        for both channels, the process called once per sample at twice the rate with both.
 */
class StereoOversampler2 {
public:
    /** @brief The delay the round trip adds, in samples at the base rate: (51 - 1) / 2. */
    static constexpr float Latency = 25.0f;

    /** @brief Clears both channels' histories. */
    void reset()
    {
        for (int c = 0; c < 2; ++c) { m_up[c].reset(); m_dnE[c].reset(); m_dnO[c].reset(); }
    }

    /**
     * @brief One stereo sample through the process at twice the rate.
     *
     * @tparam F  callable `void(float l, float r, float& yl, float& yr)`, called twice per call, in
     *            time order: the process, which keeps its own state
     * @param l   the left sample at the base rate, replaced by the process's output, Latency late
     * @param r   the right sample, the same
     * @param f   the process
     */
    template <class FT>
    void process(float& l, float& r, FT&& f)
    {
        float a[2], b[2], y0[2], y1[2];
        m_s.up(m_up[0], l, a);
        m_s.up(m_up[1], r, b);
        f(a[0], b[0], y0[0], y0[1]);
        f(a[1], b[1], y1[0], y1[1]);
        l = m_s.down(m_dnE[0], m_dnO[0], y0[0], y1[0]);
        r = m_s.down(m_dnE[1], m_dnO[1], y0[1], y1[1]);
    }

private:
    halfband::Stage1T m_s;   ///< the stage's taps
    halfband::Stage1T::HistoryT m_up[2],    ///< the upsamplers' histories, per channel
                              m_dnE[2],   ///< the downsamplers' histories, even samples
                              m_dnO[2];   ///< and odd ones
};

} // namespace ambient
