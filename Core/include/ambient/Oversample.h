/**
 * @file Oversample.h
 * @brief Four-times oversampling for a memoryless nonlinearity: two linear-phase half-band stages.
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
 * One instance is one mono channel; a stereo stage owns two.
 */
#pragma once
#include <cmath>
#include <cstring>

namespace ambient {

/**
 * @brief Four-times oversampler around a memoryless curve, for one channel.
 */
class Oversampler4 {
public:
    static constexpr int kTaps1 = 51;   ///< the first stage's length, 4m + 3 with m = 12: 80 dB, transition 0.2 .. 0.3 of its rate
    static constexpr int kTaps2 = 19;   ///< the second stage's length, 4m + 3 with m = 4: the images are an octave away
    static constexpr int kSide1 = (kTaps1 + 1) / 2;   ///< non-zero taps of stage one's filtering branch (the even-indexed ones)
    static constexpr int kSide2 = (kTaps2 + 1) / 2;   ///< the same for stage two
    /** @brief The delay the round trip adds, in samples at the base rate: (51 - 1) / 2 + (19 - 1) / 4. */
    static constexpr float kLatency = 29.5f;

    /** @brief Designs both stages and clears the history. Allocation-free; call on any thread before use. */
    Oversampler4()
    {
        design(h1_, kTaps1);
        design(h2_, kTaps2);
        reset();
    }

    /** @brief Clears the four filters' histories: the next sample starts from silence. */
    void reset()
    {
        std::memset(up1_, 0, sizeof(up1_));
        std::memset(up2_, 0, sizeof(up2_));
        std::memset(dn2e_, 0, sizeof(dn2e_));
        std::memset(dn2o_, 0, sizeof(dn2o_));
        std::memset(dn1e_, 0, sizeof(dn1e_));
        std::memset(dn1o_, 0, sizeof(dn1o_));
        p1_ = p2_ = q2_ = q1_ = 0;
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
        upsample(up1_, p1_, h1_, kSide1, (kTaps1 - 3) / 4, x, a);
        float out = 0.0f;
        for (int k = 0; k < 2; ++k) {
            float b[2];
            upsample(up2_, p2_, h2_, kSide2, (kTaps2 - 3) / 4, a[k], b);
            const float c = downsample(dn2e_, dn2o_, q2_, h2_, kSide2, (kTaps2 - 3) / 4, f(b[0]), f(b[1]));
            // Stage one comes down from the pair of stage-two outputs, the first even, the second odd.
            if (k == 0) evenHeld_ = c;
            else out = downsample(dn1e_, dn1o_, q1_, h1_, kSide1, (kTaps1 - 3) / 4, evenHeld_, c);
        }
        return out;
    }

private:
    /**
     * @brief A Kaiser-windowed half-band low-pass, keeping only its even-indexed taps.
     *
     * h[n] = 0.5 sinc((n - c) / 2) w(n), c = (N - 1) / 2 odd. The odd-indexed taps are zero except
     * the centre, which is exactly one half; the even-indexed ones are stored, scaled so that they
     * sum to one half, which puts the DC gain of the whole filter at one.
     *
     * @param out  receives the (N + 1) / 2 even-indexed taps, h[0], h[2], ..., h[N - 1]
     * @param n    the length, 4m + 3
     */
    static void design(float* out, int n)
    {
        constexpr double kPi = 3.14159265358979323846;
        const double beta = 7.857;   // Kaiser, 80 dB of stop-band attenuation
        const double c = 0.5 * (n - 1);
        double sum = 0.0;
        double tmp[kSide1];
        int j = 0;
        for (int i = 0; i < n; i += 2) {
            const double t = (i - c) * 0.5;   // odd-half values: the sinc's non-zero samples
            const double sinc = std::sin(kPi * t) / (kPi * t);
            const double r = (i - c) / c;
            const double w = besselI0(beta * std::sqrt(std::fmax(0.0, 1.0 - r * r))) / besselI0(beta);
            tmp[j] = 0.5 * sinc * w;
            sum += tmp[j];
            ++j;
        }
        for (int k = 0; k < j; ++k) out[k] = static_cast<float>(tmp[k] * 0.5 / sum);
    }
    /**
     * @brief The modified Bessel function of the first kind, order zero, by its power series.
     * @param x  the argument, 0 .. 20 in practice
     * @return   I0(x)
     */
    static double besselI0(double x)
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
     * @brief One sample in, two out at twice the rate: y[2n] from the filtering branch, y[2n+1]
     *        the input delayed by m samples (the centre tap, which is one half, times the gain of two).
     * @param hist  the input history, a doubled ring of 2 * side floats
     * @param pos   the ring's write position, advanced here
     * @param h     the even-indexed taps
     * @param side  how many of them
     * @param m     (N - 3) / 4: the pure-delay branch's delay in input samples
     * @param x     the input sample
     * @param y     receives the two output samples, in time order
     */
    static void upsample(float* hist, int& pos, const float* h, int side, int m, float x, float* y)
    {
        pos = (pos == 0 ? side : pos) - 1;
        hist[pos] = x;
        hist[pos + side] = x;
        const float* v = hist + pos;   // v[k] = x[n - k]
        float s = 0.0f;
        for (int k = 0; k < side; ++k) s += h[k] * v[k];
        y[0] = 2.0f * s;
        y[1] = v[m];
    }
    /**
     * @brief Two samples in at twice the rate, one out: the even one through the filtering branch,
     *        the odd one through the centre tap, m + 1 samples late.
     * @param he    the even samples' history, a doubled ring of 2 * side floats
     * @param ho    the odd samples' history, the same size
     * @param pos   the rings' write position, advanced here
     * @param h     the even-indexed taps
     * @param side  how many of them
     * @param m     (N - 3) / 4
     * @param even  the first sample of the pair, v[2n]
     * @param odd   the second, v[2n + 1]
     * @return      w[n] = sum h[2j] v[2n - 2j] + 0.5 v[2(n - m - 1) + 1]
     */
    static float downsample(float* he, float* ho, int& pos, const float* h, int side, int m, float even, float odd)
    {
        pos = (pos == 0 ? side : pos) - 1;
        he[pos] = even; he[pos + side] = even;
        ho[pos] = odd;  ho[pos + side] = odd;
        const float* ve = he + pos;
        const float* vo = ho + pos;
        float s = 0.0f;
        for (int k = 0; k < side; ++k) s += h[k] * ve[k];
        return s + 0.5f * vo[m + 1];
    }

    float h1_[kSide1] = {};   ///< stage one's even-indexed taps
    float h2_[kSide2] = {};   ///< stage two's even-indexed taps
    float up1_[2 * kSide1] = {};   ///< stage one's upsampler history, a doubled ring
    float up2_[2 * kSide2] = {};   ///< stage two's upsampler history
    float dn2e_[2 * kSide2] = {};  ///< stage two's downsampler history, even samples
    float dn2o_[2 * kSide2] = {};  ///< and odd ones
    float dn1e_[2 * kSide1] = {};  ///< stage one's downsampler history, even samples
    float dn1o_[2 * kSide1] = {};  ///< and odd ones
    int   p1_ = 0,   ///< stage one upsampler's ring position
          p2_ = 0,   ///< stage two upsampler's ring position
          q2_ = 0,   ///< stage two downsampler's ring position
          q1_ = 0;   ///< stage one downsampler's ring position
    float evenHeld_ = 0.0f;   ///< the first stage-two output of a pair, waiting for its partner
};

} // namespace ambient
