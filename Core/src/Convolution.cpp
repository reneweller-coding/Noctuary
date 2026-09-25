/**
 * @file Convolution.cpp
 * @brief The Room's convolver: the spectral products, impulse loading, the built-in hall, the schedule.
 *
 * Convolution.h explains the design -- three stages of growing partitions, the work spread over
 * 256-sample steps, band-limited partitions, two morphable impulses double-buffered. This file is
 * the machinery, in four parts. The products (sumOne, sumTwo, blendOne, blendTwo) are the one loop
 * acc += X * H in the four shapes the morph and a mono impulse call for, vectorised with AVX2 on the
 * desktop and NEON on the Quest; they are most of what the Room costs. The loading side, on the
 * message thread, resamples a file to the engine rate with a Kaiser-windowed sinc, shrinks one that
 * is longer than the cap instead of cutting it, normalises its energy and trims the silence at the
 * end, and analyse() cuts it into each stage's partitions and keeps of every partition only the bins
 * that carry more than its share of kDropBudget. makeDefaultImpulse() is the built-in hall, a
 * statistical late field synthesised in the short-time Fourier domain from the octave design tables.
 * The audio-thread side is process(), which moves samples between the input history and the output
 * ring and calls step() once every head_ samples; step() begins a stage's block when one is complete
 * and hands every stage in flight its share of the work left before its answer is due, which
 * advance() spends phase by phase -- the input transform, the push into the delay line, the sum over
 * the partitions, the inverse, the delivery -- through the resumable StepFft, so no step ever pays
 * for a 16384-sample partition at once.
 */
#include "ambient/Convolution.h"
#include "ambient/Dsp.h"
#include "ambient/Simd.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>

namespace ambient {

namespace {

constexpr double kPiD = 3.14159265358979323846;   ///< pi in double: the twiddles, the windows, the sinc
/**
 * @brief All that the band limits and the trimmed end may leave out of an impulse, together: -100 dB of
 *        its energy.
 *
 * A fifth goes to the silence at the end, the rest is shared by the partitions.
 */
constexpr double kDropBudget = 1.0e-10;
constexpr long long kUnlimited = std::numeric_limits<long long>::max() / 4;   ///< a budget no transform can spend: the offline callers, and the last step before a block is due
/**
 * @brief Where a stage's block in progress stands (Stage::phase), in the order advance() runs them.
 *
 * kIdle: nothing in flight; the stage waits for its next block to complete. kFftIn: the forward
 * transform of the input block, one channel after the other. kPush: the transform goes into the
 * frequency-domain delay line and the accumulators are cleared. kSum: the products over the
 * partitions, resumable at any bin. kInvert: the inverse transform of the sum, channel by channel.
 * kDeliver: the result, scaled by 1/N, is added into the output ring where the block is due.
 */
enum Phase { kIdle = 0, kFftIn, kPush, kSum, kInvert, kDeliver };

/**
 * @brief @p v rounded up to a multiple of eight: the bin counts, so the vector paths cover every bin.
 * @param v  a bin count
 * @return   the smallest multiple of eight >= v
 */
inline int roundUp8(int v) { return (v + 7) & ~7; }

// ---------------------------------------------------------------- the products
/**
 * @name The products
 * One loop in four shapes: acc += X * H over the bins [k0, k1). k1 - k0 is always a multiple of
 * eight and every array is padded to match, so the vector paths -- eight lanes with AVX2 on the
 * desktop, four with NEON on the Quest -- cover all of it, and the scalar lines are only what a
 * build with neither runs. This is most of the Room's arithmetic.
 * @{ */

#if AMBIENT_HAS_NEON
/**
 * @name NEON multiply-add
 * a + b c and a - b c. Fused wherever the target has FMA: every AArch64 compiler says so with
 * \__ARM_FEATURE_FMA (the NDK header declares vfmaq_f32 only then), and MSVC's arm64 header has it
 * anyway -- one instruction each, rounded like the fmadd of the AVX path. The plain multiply-add is
 * for GCC or clang on a 32-bit ARM without VFPv4; the x86 test variant AMBIENT_NEON_SHIM_NOFMA runs it.
 * @{ */
#if (defined(__ARM_FEATURE_FMA) || defined(_M_ARM64) || defined(AMBIENT_NEON_SHIM)) && !defined(AMBIENT_NEON_SHIM_NOFMA)
/**
 * @brief a + b c in one fused instruction.
 * @param a  the addend, four lanes
 * @param b  first factor
 * @param c  second factor
 * @return   a + b * c, rounded once
 */
inline float32x4_t neonAddMul(float32x4_t a, float32x4_t b, float32x4_t c) { return vfmaq_f32(a, b, c); }
/**
 * @brief a - b c in one fused instruction.
 * @param a  the minuend, four lanes
 * @param b  first factor
 * @param c  second factor
 * @return   a - b * c, rounded once
 */
inline float32x4_t neonSubMul(float32x4_t a, float32x4_t b, float32x4_t c) { return vfmsq_f32(a, b, c); }
#else
/**
 * @brief a + b c as a multiply and an add, for a target without FMA.
 * @param a  the addend, four lanes
 * @param b  first factor
 * @param c  second factor
 * @return   a + b * c, rounded twice
 */
inline float32x4_t neonAddMul(float32x4_t a, float32x4_t b, float32x4_t c) { return vmlaq_f32(a, b, c); }
/**
 * @brief a - b c as a multiply and a subtract, for a target without FMA.
 * @param a  the minuend, four lanes
 * @param b  first factor
 * @param c  second factor
 * @return   a - b * c, rounded twice
 */
inline float32x4_t neonSubMul(float32x4_t a, float32x4_t b, float32x4_t c) { return vmlsq_f32(a, b, c); }
#endif
/** @} */
#endif

/**
 * @brief One channel against one impulse, weighted.
 * @param ar  the accumulator's real parts; += Re(w H X) over the bins
 * @param ai  the accumulator's imaginary parts; += Im(w H X)
 * @param xr  the input block's spectrum, real parts
 * @param xi  the input block's spectrum, imaginary parts
 * @param hr  the partition's spectrum, real parts
 * @param hi  the partition's spectrum, imaginary parts
 * @param w   the impulse's morph weight; at exactly 1 the vector paths skip the multiply
 * @param k0  the first bin
 * @param k1  one past the last bin; k1 - k0 is a multiple of eight
 */
void sumOne(float* ar, float* ai, const float* xr, const float* xi, const float* hr, const float* hi, float w, int k0, int k1)
{
    int k = k0;
#if AMBIENT_HAS_AVX
    const __m256 W = _mm256_set1_ps(w);
    const bool unit = w == 1.0f;
    for (; k + 8 <= k1; k += 8) {
        __m256 HR = _mm256_loadu_ps(hr + k), HI = _mm256_loadu_ps(hi + k);
        if (!unit) { HR = _mm256_mul_ps(W, HR); HI = _mm256_mul_ps(W, HI); }
        const __m256 XR = _mm256_loadu_ps(xr + k), XI = _mm256_loadu_ps(xi + k);
        _mm256_storeu_ps(ar + k, _mm256_fnmadd_ps(XI, HI, _mm256_fmadd_ps(XR, HR, _mm256_loadu_ps(ar + k))));
        _mm256_storeu_ps(ai + k, _mm256_fmadd_ps(XI, HR, _mm256_fmadd_ps(XR, HI, _mm256_loadu_ps(ai + k))));
    }
#elif AMBIENT_HAS_NEON
    const float32x4_t W = vdupq_n_f32(w);
    const bool unit = w == 1.0f;
    for (; k + 4 <= k1; k += 4) {
        float32x4_t HR = vld1q_f32(hr + k), HI = vld1q_f32(hi + k);
        if (!unit) { HR = vmulq_f32(W, HR); HI = vmulq_f32(W, HI); }
        const float32x4_t XR = vld1q_f32(xr + k), XI = vld1q_f32(xi + k);
        vst1q_f32(ar + k, neonSubMul(neonAddMul(vld1q_f32(ar + k), XR, HR), XI, HI));   // + xr hr - xi hi
        vst1q_f32(ai + k, neonAddMul(neonAddMul(vld1q_f32(ai + k), XR, HI), XI, HR));   // + xr hi + xi hr
    }
#endif
    for (; k < k1; ++k) {
        const float h_r = w * hr[k], h_i = w * hi[k];
        ar[k] += xr[k] * h_r - xi[k] * h_i;
        ai[k] += xr[k] * h_i + xi[k] * h_r;
    }
}

/**
 * @brief Both channels against one mono impulse: the impulse is loaded once for the two of them.
 * @param a0r  left accumulator, real parts
 * @param a0i  left accumulator, imaginary parts
 * @param a1r  right accumulator, real parts
 * @param a1i  right accumulator, imaginary parts
 * @param x0r  the left input block's spectrum, real parts
 * @param x0i  the left input block's spectrum, imaginary parts
 * @param x1r  the right input block's spectrum, real parts
 * @param x1i  the right input block's spectrum, imaginary parts
 * @param hr   the mono partition's spectrum, real parts
 * @param hi   the mono partition's spectrum, imaginary parts
 * @param w    the impulse's morph weight; at exactly 1 the vector paths skip the multiply
 * @param k0   the first bin
 * @param k1   one past the last bin; k1 - k0 is a multiple of eight
 */
void sumTwo(float* a0r, float* a0i, float* a1r, float* a1i,
            const float* x0r, const float* x0i, const float* x1r, const float* x1i,
            const float* hr, const float* hi, float w, int k0, int k1)
{
    int k = k0;
#if AMBIENT_HAS_AVX
    const __m256 W = _mm256_set1_ps(w);
    const bool unit = w == 1.0f;
    for (; k + 8 <= k1; k += 8) {
        __m256 HR = _mm256_loadu_ps(hr + k), HI = _mm256_loadu_ps(hi + k);
        if (!unit) { HR = _mm256_mul_ps(W, HR); HI = _mm256_mul_ps(W, HI); }
        __m256 XR = _mm256_loadu_ps(x0r + k), XI = _mm256_loadu_ps(x0i + k);
        _mm256_storeu_ps(a0r + k, _mm256_fnmadd_ps(XI, HI, _mm256_fmadd_ps(XR, HR, _mm256_loadu_ps(a0r + k))));
        _mm256_storeu_ps(a0i + k, _mm256_fmadd_ps(XI, HR, _mm256_fmadd_ps(XR, HI, _mm256_loadu_ps(a0i + k))));
        XR = _mm256_loadu_ps(x1r + k); XI = _mm256_loadu_ps(x1i + k);
        _mm256_storeu_ps(a1r + k, _mm256_fnmadd_ps(XI, HI, _mm256_fmadd_ps(XR, HR, _mm256_loadu_ps(a1r + k))));
        _mm256_storeu_ps(a1i + k, _mm256_fmadd_ps(XI, HR, _mm256_fmadd_ps(XR, HI, _mm256_loadu_ps(a1i + k))));
    }
#elif AMBIENT_HAS_NEON
    const float32x4_t W = vdupq_n_f32(w);
    const bool unit = w == 1.0f;
    for (; k + 4 <= k1; k += 4) {
        float32x4_t HR = vld1q_f32(hr + k), HI = vld1q_f32(hi + k);
        if (!unit) { HR = vmulq_f32(W, HR); HI = vmulq_f32(W, HI); }
        float32x4_t XR = vld1q_f32(x0r + k), XI = vld1q_f32(x0i + k);
        vst1q_f32(a0r + k, neonSubMul(neonAddMul(vld1q_f32(a0r + k), XR, HR), XI, HI));
        vst1q_f32(a0i + k, neonAddMul(neonAddMul(vld1q_f32(a0i + k), XR, HI), XI, HR));
        XR = vld1q_f32(x1r + k); XI = vld1q_f32(x1i + k);
        vst1q_f32(a1r + k, neonSubMul(neonAddMul(vld1q_f32(a1r + k), XR, HR), XI, HI));
        vst1q_f32(a1i + k, neonAddMul(neonAddMul(vld1q_f32(a1i + k), XR, HI), XI, HR));
    }
#endif
    for (; k < k1; ++k) {
        const float h_r = w * hr[k], h_i = w * hi[k];
        a0r[k] += x0r[k] * h_r - x0i[k] * h_i;
        a0i[k] += x0r[k] * h_i + x0i[k] * h_r;
        a1r[k] += x1r[k] * h_r - x1i[k] * h_i;
        a1i[k] += x1r[k] * h_i + x1i[k] * h_r;
    }
}

/**
 * @brief One channel against the blend of two impulses.
 * @param ar   the accumulator's real parts
 * @param ai   the accumulator's imaginary parts
 * @param xr   the input block's spectrum, real parts
 * @param xi   the input block's spectrum, imaginary parts
 * @param har  impulse A's partition, real parts
 * @param hai  impulse A's partition, imaginary parts
 * @param wa   impulse A's weight, 1 - morph
 * @param hbr  impulse B's partition, real parts
 * @param hbi  impulse B's partition, imaginary parts
 * @param wb   impulse B's weight, the morph
 * @param k0   the first bin
 * @param k1   one past the last bin; k1 - k0 is a multiple of eight
 */
void blendOne(float* ar, float* ai, const float* xr, const float* xi,
              const float* har, const float* hai, float wa, const float* hbr, const float* hbi, float wb, int k0, int k1)
{
    int k = k0;
#if AMBIENT_HAS_AVX
    const __m256 WA = _mm256_set1_ps(wa), WB = _mm256_set1_ps(wb);
    for (; k + 8 <= k1; k += 8) {
        const __m256 HR = _mm256_fmadd_ps(WA, _mm256_loadu_ps(har + k), _mm256_mul_ps(WB, _mm256_loadu_ps(hbr + k)));
        const __m256 HI = _mm256_fmadd_ps(WA, _mm256_loadu_ps(hai + k), _mm256_mul_ps(WB, _mm256_loadu_ps(hbi + k)));
        const __m256 XR = _mm256_loadu_ps(xr + k), XI = _mm256_loadu_ps(xi + k);
        _mm256_storeu_ps(ar + k, _mm256_fnmadd_ps(XI, HI, _mm256_fmadd_ps(XR, HR, _mm256_loadu_ps(ar + k))));
        _mm256_storeu_ps(ai + k, _mm256_fmadd_ps(XI, HR, _mm256_fmadd_ps(XR, HI, _mm256_loadu_ps(ai + k))));
    }
#elif AMBIENT_HAS_NEON
    const float32x4_t WA = vdupq_n_f32(wa), WB = vdupq_n_f32(wb);
    for (; k + 4 <= k1; k += 4) {
        const float32x4_t HR = neonAddMul(vmulq_f32(WB, vld1q_f32(hbr + k)), WA, vld1q_f32(har + k));   // wa ha + wb hb
        const float32x4_t HI = neonAddMul(vmulq_f32(WB, vld1q_f32(hbi + k)), WA, vld1q_f32(hai + k));
        const float32x4_t XR = vld1q_f32(xr + k), XI = vld1q_f32(xi + k);
        vst1q_f32(ar + k, neonSubMul(neonAddMul(vld1q_f32(ar + k), XR, HR), XI, HI));
        vst1q_f32(ai + k, neonAddMul(neonAddMul(vld1q_f32(ai + k), XR, HI), XI, HR));
    }
#endif
    for (; k < k1; ++k) {
        const float h_r = wa * har[k] + wb * hbr[k], h_i = wa * hai[k] + wb * hbi[k];
        ar[k] += xr[k] * h_r - xi[k] * h_i;
        ai[k] += xr[k] * h_i + xi[k] * h_r;
    }
}

/**
 * @brief Both channels against the blend of two mono impulses.
 * @param a0r  left accumulator, real parts
 * @param a0i  left accumulator, imaginary parts
 * @param a1r  right accumulator, real parts
 * @param a1i  right accumulator, imaginary parts
 * @param x0r  the left input block's spectrum, real parts
 * @param x0i  the left input block's spectrum, imaginary parts
 * @param x1r  the right input block's spectrum, real parts
 * @param x1i  the right input block's spectrum, imaginary parts
 * @param har  impulse A's mono partition, real parts
 * @param hai  impulse A's mono partition, imaginary parts
 * @param wa   impulse A's weight, 1 - morph
 * @param hbr  impulse B's mono partition, real parts
 * @param hbi  impulse B's mono partition, imaginary parts
 * @param wb   impulse B's weight, the morph
 * @param k0   the first bin
 * @param k1   one past the last bin; k1 - k0 is a multiple of eight
 */
void blendTwo(float* a0r, float* a0i, float* a1r, float* a1i,
              const float* x0r, const float* x0i, const float* x1r, const float* x1i,
              const float* har, const float* hai, float wa, const float* hbr, const float* hbi, float wb, int k0, int k1)
{
    int k = k0;
#if AMBIENT_HAS_AVX
    const __m256 WA = _mm256_set1_ps(wa), WB = _mm256_set1_ps(wb);
    for (; k + 8 <= k1; k += 8) {
        const __m256 HR = _mm256_fmadd_ps(WA, _mm256_loadu_ps(har + k), _mm256_mul_ps(WB, _mm256_loadu_ps(hbr + k)));
        const __m256 HI = _mm256_fmadd_ps(WA, _mm256_loadu_ps(hai + k), _mm256_mul_ps(WB, _mm256_loadu_ps(hbi + k)));
        __m256 XR = _mm256_loadu_ps(x0r + k), XI = _mm256_loadu_ps(x0i + k);
        _mm256_storeu_ps(a0r + k, _mm256_fnmadd_ps(XI, HI, _mm256_fmadd_ps(XR, HR, _mm256_loadu_ps(a0r + k))));
        _mm256_storeu_ps(a0i + k, _mm256_fmadd_ps(XI, HR, _mm256_fmadd_ps(XR, HI, _mm256_loadu_ps(a0i + k))));
        XR = _mm256_loadu_ps(x1r + k); XI = _mm256_loadu_ps(x1i + k);
        _mm256_storeu_ps(a1r + k, _mm256_fnmadd_ps(XI, HI, _mm256_fmadd_ps(XR, HR, _mm256_loadu_ps(a1r + k))));
        _mm256_storeu_ps(a1i + k, _mm256_fmadd_ps(XI, HR, _mm256_fmadd_ps(XR, HI, _mm256_loadu_ps(a1i + k))));
    }
#elif AMBIENT_HAS_NEON
    const float32x4_t WA = vdupq_n_f32(wa), WB = vdupq_n_f32(wb);
    for (; k + 4 <= k1; k += 4) {
        const float32x4_t HR = neonAddMul(vmulq_f32(WB, vld1q_f32(hbr + k)), WA, vld1q_f32(har + k));
        const float32x4_t HI = neonAddMul(vmulq_f32(WB, vld1q_f32(hbi + k)), WA, vld1q_f32(hai + k));
        float32x4_t XR = vld1q_f32(x0r + k), XI = vld1q_f32(x0i + k);
        vst1q_f32(a0r + k, neonSubMul(neonAddMul(vld1q_f32(a0r + k), XR, HR), XI, HI));
        vst1q_f32(a0i + k, neonAddMul(neonAddMul(vld1q_f32(a0i + k), XR, HI), XI, HR));
        XR = vld1q_f32(x1r + k); XI = vld1q_f32(x1i + k);
        vst1q_f32(a1r + k, neonSubMul(neonAddMul(vld1q_f32(a1r + k), XR, HR), XI, HI));
        vst1q_f32(a1i + k, neonAddMul(neonAddMul(vld1q_f32(a1i + k), XR, HI), XI, HR));
    }
#endif
    for (; k < k1; ++k) {
        const float h_r = wa * har[k] + wb * hbr[k], h_i = wa * hai[k] + wb * hbi[k];
        a0r[k] += x0r[k] * h_r - x0i[k] * h_i;
        a0i[k] += x0r[k] * h_i + x0i[k] * h_r;
        a1r[k] += x1r[k] * h_r - x1i[k] * h_i;
        a1i[k] += x1r[k] * h_i + x1i[k] * h_r;
    }
}
/** @} */

// ---------------------------------------------------------------- resampling

/**
 * @brief The modified Bessel function of the first kind and order zero, by its power series.
 *
 * The Kaiser window is I0(beta sqrt(1 - z^2)) / I0(beta); the series converges in a few dozen terms
 * for the beta of 7 used here.
 *
 * @param x  the argument, non-negative
 * @return   I0(x), 1 at zero and growing like e^x / sqrt(2 pi x)
 */
double besselI0(double x)
{
    double sum = 1.0, term = 1.0;
    const double q = 0.25 * x * x;
    for (int k = 1; k < 200; ++k) {
        term *= q / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < 1.0e-14 * sum) break;
    }
    return sum;
}

/**
 * @brief An impulse recorded at another rate, band-limited onto the engine's: a Kaiser-windowed sinc with
 *        16 zero crossings each side at the lower of the two rates, about -70 dB outside the band.
 *
 * The
 * linear interpolation that stood here dulled the top octave of every 44.1 kHz impulse and folded
 * what lay above a lower rate's Nyquist back down. At most `limit` samples come out.
 *
 * @param x      the impulse's samples at its own rate
 * @param n      how many of them
 * @param ratio  engine rate over the impulse's rate: the output has n * ratio samples
 * @param limit  the most samples to produce (the Room's cap); the rest of the impulse is not computed
 * @return       the impulse at the engine rate; a copy of the first `limit` samples when the rates match
 */
std::vector<float> resample(const float* x, int n, double ratio, long limit)
{
    if (std::fabs(ratio - 1.0) < 1.0e-9) return std::vector<float>(x, x + std::min<long>(n, limit));
    constexpr int kHalf = 16, kTable = 512;
    constexpr double kBeta = 7.0;
    const double a = std::min(1.0, ratio) * 0.95;   // kernel a * sinc(a u): cutoff 0.475 of the lower rate
    const double i0b = besselI0(kBeta);
    std::vector<float> table(static_cast<size_t>(kHalf * kTable + 2), 0.0f);
    for (int j = 0; j <= kHalf * kTable; ++j) {
        const double v = static_cast<double>(j) / kTable;   // in zero crossings
        const double z = v / kHalf;
        const double sinc = j == 0 ? 1.0 : std::sin(kPiD * v) / (kPiD * v);
        table[static_cast<size_t>(j)] = static_cast<float>(sinc * besselI0(kBeta * std::sqrt(std::max(0.0, 1.0 - z * z))) / i0b);
    }
    const long outN = std::min(limit, static_cast<long>(std::floor(static_cast<double>(n) * ratio)));
    std::vector<float> y(static_cast<size_t>(std::max(0L, outN)), 0.0f);
    const double reach = kHalf / a;      // in input samples
    const double toTable = a * kTable;
    for (long i = 0; i < outN; ++i) {
        const double t = static_cast<double>(i) / ratio;
        const int j0 = std::max(0, static_cast<int>(std::ceil(t - reach)));
        const int j1 = std::min(n - 1, static_cast<int>(std::floor(t + reach)));
        double acc = 0.0;
        for (int j = j0; j <= j1; ++j) {
            const double v = std::fabs(t - j) * toTable;
            const int iv = static_cast<int>(v);
            if (iv >= kHalf * kTable) continue;
            const double f = v - iv;
            acc += x[j] * (table[static_cast<size_t>(iv)] + f * (table[static_cast<size_t>(iv) + 1] - table[static_cast<size_t>(iv)]));
        }
        y[static_cast<size_t>(i)] = static_cast<float>(a * acc);
    }
    return y;
}

/**
 * @brief An impulse longer than the Room keeps, shortened rather than cut.
 *
 * A cut stops the tail wherever
 * it happens to be: a 56-second space cut at twelve seconds stopped at -34 dB, and every note
 * through it ended in a gate. Instead the kept part gets an exponential window that brings it to
 * -60 dB against its loudest 50 ms at the cap -- every band's decay shortened by the same extra
 * rate, so the room keeps its colour and only gets smaller (Canfield-Dafilou and Abel 2018) -- and
 * then a 50 ms fade. Measured on that space: a straight 13.8-second room that ends 76 dB down and
 * gates nowhere; on the Quest's four seconds, a 4.3-second room.
 *
 * @param ch        the channels, already resampled and cut at the cap, all of one length; windowed in place
 * @param channels  1 or 2, how many of @p ch are in use
 * @param sr        the engine's sample rate, for the 50 ms windows
 */
void shrinkToCap(std::vector<float>* ch, int channels, double sr)
{
    const size_t len = ch[0].size();
    const size_t k = std::max<size_t>(1, static_cast<size_t>(0.05 * sr));
    if (len < 2 * k) return;
    auto energy = [&](size_t i) {
        double e = 0.0;
        for (int c = 0; c < channels; ++c) e += static_cast<double>(ch[c][i]) * ch[c][i];
        return e;
    };
    double run = 0.0, loud = 0.0;
    for (size_t i = 0; i < len; ++i) {
        run += energy(i);
        if (i >= k) run -= energy(i - k);
        if (i + 1 >= k) loud = std::max(loud, run);
    }
    if (loud <= 0.0) return;
    double last = 0.0;
    for (size_t i = len - k; i < len; ++i) last += energy(i);
    const double levelDb = 10.0 * std::log10(std::max(last, 1.0e-300) / loud);
    const double extraDb = std::min(0.0, -60.0 - levelDb);         // what the window adds by the cap
    const double step = std::pow(10.0, extraDb / 20.0 / static_cast<double>(len));
    double w = 1.0;
    for (size_t i = 0; i < len; ++i) {
        double g = w;
        if (i >= len - k) {
            const double c = std::cos(0.5 * kPiD * static_cast<double>(i - (len - k)) / static_cast<double>(k));
            g *= c * c;
        }
        for (int c = 0; c < channels; ++c) ch[c][i] = static_cast<float>(ch[c][i] * g);
        w *= step;
    }
}

/**
 * @name The built-in hall's design
 * The built-in hall's design, solved offline by the generator that makes the library's rooms
 * (Tools/ImpulseGen/roomgen.py): the initial spectrum in dB and T60 in seconds at the octaves.
 * @{ */
constexpr int kOct = 9;   ///< the octave bands, 63 Hz .. 16 kHz
constexpr double kOctHz[kOct]  = { 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0 };   ///< the bands' centres in Hz
constexpr double kOctB[kOct]   = { -3.7, -3.5, -2.5, -1.1, -1.4, -3.9, -7.5, -11.9, -17.9 };   ///< the initial spectrum at each band, in dB
constexpr double kOctT60[kOct] = { 2.52, 2.66, 2.80, 2.80, 2.80, 2.06, 1.43, 0.85, 0.37 };   ///< the decay to -60 dB at each band, in seconds
/** @} */

/**
 * @brief The design at any frequency: linear in log frequency between the octaves (the T60 in its logarithm); outside them the ends
 *        are held, with the design's guards: 12 dB an octave below 40 Hz, 6 dB an octave above 14 kHz.
 *
 * @param f    the frequency in Hz (a bin's centre); anything below 1 Hz is taken as 1
 * @param bDb  receives the initial level in dB
 * @param t60  receives the decay time to -60 dB in seconds
 */
void octaveDesign(double f, double& bDb, double& t60)
{
    f = std::max(f, 1.0);
    const double lf = std::log2(f);
    if (f <= kOctHz[0]) {
        bDb = kOctB[0] - 10.0 * std::log10(1.0 + std::pow(40.0 / f, 4.0)) + 10.0 * std::log10(1.0 + std::pow(40.0 / kOctHz[0], 4.0));
        t60 = kOctT60[0];
        return;
    }
    if (f >= kOctHz[kOct - 1]) {
        const double top = kOctHz[kOct - 1] / 14000.0, here = f / 14000.0;
        bDb = kOctB[kOct - 1] - 10.0 * std::log10(1.0 + here * here) + 10.0 * std::log10(1.0 + top * top);
        t60 = kOctT60[kOct - 1];
        return;
    }
    int i = 0;
    while (i + 2 < kOct && f >= kOctHz[i + 1]) ++i;
    const double a = std::log2(kOctHz[i]), b = std::log2(kOctHz[i + 1]);
    const double u = (lf - a) / (b - a);
    bDb = kOctB[i] + u * (kOctB[i + 1] - kOctB[i]);
    t60 = std::exp(std::log(kOctT60[i]) + u * (std::log(kOctT60[i + 1]) - std::log(kOctT60[i])));
}

// ---------------------------------------------------------------- rings

/**
 * @brief Copies a run of samples into a ring, wrapping at its end.
 * @param ring  the ring, a power of two long
 * @param at    the index of the first sample written, already masked into the ring
 * @param src   the samples
 * @param m     how many; never more than the ring holds
 */
void ringWrite(std::vector<float>& ring, int at, const float* src, int m)
{
    const int size = static_cast<int>(ring.size());
    const int first = std::min(m, size - at);
    std::memcpy(ring.data() + at, src, sizeof(float) * static_cast<size_t>(first));
    if (first < m) std::memcpy(ring.data(), src + first, sizeof(float) * static_cast<size_t>(m - first));
}

/**
 * @brief Read and empty: what the stages add lands on zeros.
 * @param ring  the output ring, a power of two long
 * @param at    the index of the first sample read, already masked into the ring
 * @param dst   receives the samples
 * @param m     how many; never more than the ring holds
 */
void ringTake(std::vector<float>& ring, int at, float* dst, int m)
{
    const int size = static_cast<int>(ring.size());
    const int first = std::min(m, size - at);
    std::memcpy(dst, ring.data() + at, sizeof(float) * static_cast<size_t>(first));
    std::memset(ring.data() + at, 0, sizeof(float) * static_cast<size_t>(first));
    if (first < m) {
        std::memcpy(dst + first, ring.data(), sizeof(float) * static_cast<size_t>(m - first));
        std::memset(ring.data(), 0, sizeof(float) * static_cast<size_t>(m - first));
    }
}

} // namespace

// ---------------------------------------------------------------- the transform

void Convolver::StepFft::init(int size)
{
    n = size;
    int bits = 0;
    while ((1 << bits) < n) ++bits;
    rev.assign(static_cast<size_t>(n), 0);
    for (int i = 0; i < n; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        rev[static_cast<size_t>(i)] = r;
    }
    cs.assign(static_cast<size_t>(n), 0.0f);
    snF.assign(static_cast<size_t>(n), 0.0f);
    snI.assign(static_cast<size_t>(n), 0.0f);
    for (int h = 1; h < n; h <<= 1)
        for (int k = 0; k < h; ++k) {
            const double ang = kPiD * k / h;
            cs[static_cast<size_t>(h + k)]  = static_cast<float>(std::cos(ang));
            snI[static_cast<size_t>(h + k)] = static_cast<float>(std::sin(ang));
            snF[static_cast<size_t>(h + k)] = -snI[static_cast<size_t>(h + k)];
        }
    work = static_cast<long long>(n) / 4 + static_cast<long long>(n) * bits / 2;
}

/**
 * @brief The same radix-2 transform as Fft (Cosmos.h): forward turns by e^-i, inverse by e^+i and is left
 *        unscaled -- the one caller divides by n when it delivers.
 *
 * Stops when the budget is spent, at
 * the end of a run of butterflies, and returns true once the whole transform is done.
 */
bool Convolver::runFft(const StepFft& f, FftRun& r, float* re, float* im, bool inverse, long long& budget)
{
    const int n = f.n;
    if (r.half == 0) {
        while (r.pos < n) {
            if (budget <= 0) return false;
            const int end = std::min(n, r.pos + 4096);
            for (int i = r.pos; i < end; ++i) {
                const int j = f.rev[static_cast<size_t>(i)];
                if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
            }
            budget -= (end - r.pos) / 4 + 1;
            r.pos = end;
        }
        r.half = 1; r.pos = 0;
    }
    const float* sn = inverse ? f.snI.data() : f.snF.data();
    while (r.half < n) {
        const int h = r.half;
        const float* c = f.cs.data() + h;
        const float* s = sn + h;
        while (r.pos < n) {
            if (budget <= 0) return false;
            const int end = std::min(n, r.pos + std::max(2 * h, 1024));
            for (int start = r.pos; start < end; start += 2 * h) {
                float* ra = re + start; float* ia = im + start;
                float* rb = ra + h;     float* ib = ia + h;
                int k = 0;
#if AMBIENT_HAS_AVX
                for (; k + 8 <= h; k += 8) {
                    const __m256 C = _mm256_loadu_ps(c + k), S = _mm256_loadu_ps(s + k);
                    const __m256 RB = _mm256_loadu_ps(rb + k), IB = _mm256_loadu_ps(ib + k);
                    const __m256 RA = _mm256_loadu_ps(ra + k), IA = _mm256_loadu_ps(ia + k);
                    const __m256 tr = _mm256_fnmadd_ps(IB, S, _mm256_mul_ps(RB, C));   // rb c - ib s
                    const __m256 ti = _mm256_fmadd_ps(IB, C, _mm256_mul_ps(RB, S));    // rb s + ib c
                    _mm256_storeu_ps(rb + k, _mm256_sub_ps(RA, tr));
                    _mm256_storeu_ps(ib + k, _mm256_sub_ps(IA, ti));
                    _mm256_storeu_ps(ra + k, _mm256_add_ps(RA, tr));
                    _mm256_storeu_ps(ia + k, _mm256_add_ps(IA, ti));
                }
#elif AMBIENT_HAS_NEON
                for (; k + 4 <= h; k += 4) {
                    const float32x4_t C = vld1q_f32(c + k), S = vld1q_f32(s + k);
                    const float32x4_t RB = vld1q_f32(rb + k), IB = vld1q_f32(ib + k);
                    const float32x4_t RA = vld1q_f32(ra + k), IA = vld1q_f32(ia + k);
                    const float32x4_t tr = neonSubMul(vmulq_f32(RB, C), IB, S);   // rb c - ib s
                    const float32x4_t ti = neonAddMul(vmulq_f32(RB, S), IB, C);   // rb s + ib c
                    vst1q_f32(rb + k, vsubq_f32(RA, tr));
                    vst1q_f32(ib + k, vsubq_f32(IA, ti));
                    vst1q_f32(ra + k, vaddq_f32(RA, tr));
                    vst1q_f32(ia + k, vaddq_f32(IA, ti));
                }
#endif
                for (; k < h; ++k) {
                    const float tr = rb[k] * c[k] - ib[k] * s[k];
                    const float ti = rb[k] * s[k] + ib[k] * c[k];
                    rb[k] = ra[k] - tr; ib[k] = ia[k] - ti;
                    ra[k] += tr;        ia[k] += ti;
                }
            }
            budget -= (end - r.pos) / 2;
            r.pos = end;
        }
        r.half <<= 1; r.pos = 0;
    }
    return true;
}

// ---------------------------------------------------------------- setup

Convolver::Convolver()
{
    active_[0].store(-1, std::memory_order_relaxed);
    active_[1].store(-1, std::memory_order_relaxed);
}

void Convolver::prepare(double sampleRate, float maxSeconds)
{
    sr_ = sampleRate;
    const int scale = sampleRate < 64000.0 ? 1 : (sampleRate < 128000.0 ? 2 : 4);
    head_ = 256 * scale;
    capSamples_ = std::max<long>(head_, static_cast<long>(std::ceil(maxSeconds * sr_)));
    // Partition sizes and where their stretches begin. Every stage keeps start >= 2 block - 2 head,
    // so its answer is never due before its next block: see the header.
    const int  blocks[kStages] = { 256 * scale, 2048 * scale, 16384 * scale };
    const long starts[kStages] = { 0L, 4096L * scale, 32768L * scale };
    stages_ = 0;
    long ringNeed = 0;
    int histNeed = 0;
    for (int g = 0; g < kStages; ++g) {
        if (capSamples_ <= starts[g]) break;
        const long end = g + 1 < kStages ? std::min(starts[g + 1], capSamples_) : capSamples_;
        Stage& s = stage_[g];
        s = Stage{};
        s.block = blocks[g];
        s.start = static_cast<int>(starts[g]);
        s.capParts = static_cast<int>((end - starts[g] + s.block - 1) / s.block);
        s.slot = s.block + 8;
        s.fft.init(2 * s.block);
        for (int c = 0; c < 2; ++c) {
            s.fdlRe[c].assign(static_cast<size_t>(s.capParts) * static_cast<size_t>(s.slot), 0.0f);
            s.fdlIm[c].assign(static_cast<size_t>(s.capParts) * static_cast<size_t>(s.slot), 0.0f);
            s.re[c].assign(static_cast<size_t>(2 * s.block), 0.0f);
            s.im[c].assign(static_cast<size_t>(2 * s.block), 0.0f);
        }
        ringNeed = std::max(ringNeed, starts[g] + 2L * s.block + 2L * head_);
        histNeed = std::max(histNeed, s.block);
        stages_ = g + 1;
    }
    int ring = 1; while (ring < ringNeed) ring <<= 1;
    int hist = 1; while (hist < histNeed + head_) hist <<= 1;
    for (int c = 0; c < 2; ++c) {
        ring_[c].assign(static_cast<size_t>(ring), 0.0f);
        hist_[c].assign(static_cast<size_t>(hist), 0.0f);
    }
    ringMask_ = ring - 1;
    histMask_ = hist - 1;
    for (auto& pair : imp_) for (auto& im : pair) im = Impulse{};
    active_[0].store(-1, std::memory_order_release);
    active_[1].store(-1, std::memory_order_release);
    reset();
}

/**
 * @brief Nothing old is read after this: a delay line counts as empty until it is written again, and
 *        the rings are cleared.
 *
 * Cheap enough for the audio thread whatever the impulse length.
 */
void Convolver::reset()
{
    for (int g = 0; g < stages_; ++g) {
        Stage& s = stage_[g];
        s.fdlHead = 0; s.fdlFilled = 0;
        s.phase = kIdle; s.ch = 0; s.part = 0; s.bin = 0; s.parts = 0;
        s.run = FftRun{};
    }
    for (int c = 0; c < 2; ++c) {
        std::fill(ring_[c].begin(), ring_[c].end(), 0.0f);
        std::fill(hist_[c].begin(), hist_[c].end(), 0.0f);
    }
    t_ = 0;
}

float Convolver::impulseSeconds() const
{
    const int a = active_[0].load(std::memory_order_acquire);
    return a < 0 ? 0.0f : static_cast<float>(imp_[0][a].seconds);
}

long Convolver::tailSamples() const
{
    long longest = 0;
    for (int w = 0; w < 2; ++w) {
        const int a = active_[w].load(std::memory_order_acquire);
        if (a >= 0) longest = std::max(longest, imp_[w][a].samples);
    }
    return longest + head_ + (stages_ > 0 ? 2L * stage_[stages_ - 1].block : 0L);
}

long long Convolver::keptBins() const
{
    const int a = active_[0].load(std::memory_order_acquire);
    return a < 0 ? 0 : imp_[0][a].kept;
}

long long Convolver::fullBins() const
{
    const int a = active_[0].load(std::memory_order_acquire);
    return a < 0 ? 0 : imp_[0][a].full;
}

// ---------------------------------------------------------------- loading

void Convolver::load(int which, const float* L, const float* R, int n, double rate)
{
    if (L == nullptr || n <= 0 || stages_ == 0) return;
    const double ratio = rate > 0.0 ? sr_ / rate : 1.0;
    const bool stereo = R != nullptr;
    std::vector<float> ch[2];
    ch[0] = resample(L, n, ratio, capSamples_);
    if (stereo) ch[1] = resample(R, n, ratio, capSamples_);
    int len = static_cast<int>(ch[0].size());
    if (len <= 0) return;
    if (static_cast<double>(n) * ratio >= static_cast<double>(capSamples_) + 1.0)   // longer than the Room keeps
        shrinkToCap(ch, stereo ? 2 : 1, sr_);
    // Energy normalised to 1, so a room never changes the level of what it reverberates.
    auto energyAt = [&](int i) {
        const double l = ch[0][static_cast<size_t>(i)];
        if (!stereo) return l * l;
        const double r = ch[1][static_cast<size_t>(i)];
        return 0.5 * (l * l + r * r);
    };
    double e = 0.0;
    for (int i = 0; i < len; ++i) e += energyAt(i);
    if (e > 1.0e-12) {
        const float g = static_cast<float>(1.0 / std::sqrt(e));
        for (int c = 0; c < (stereo ? 2 : 1); ++c) for (int i = 0; i < len; ++i) ch[c][static_cast<size_t>(i)] *= g;
        // The silence at the end costs as much as the sound; it goes, as far as a fifth of the budget reaches.
        double tail = 0.0;
        while (len > 1 && tail + energyAt(len - 1) <= 0.2 * kDropBudget) { tail += energyAt(len - 1); --len; }
    }
    const int activeNow = active_[which].load(std::memory_order_acquire);
    const int target = activeNow < 0 ? 0 : 1 - activeNow;
    // The other copy is written only once every step that had begun has ended: the same reasoning
    // as Engine::setTexture. A step that began before this call chose its copy back then.
    waitForQuiet();
    analyse(imp_[which][target], ch, stereo, len);
    active_[which].store(target, std::memory_order_release);
    // The copy that was playing is read by nothing once the steps that began before the swap have
    // ended; its memory goes back now instead of when the next impulse overwrites it.
    if (activeNow >= 0) {
        waitForQuiet();
        imp_[which][activeNow] = Impulse{};
    }
}

void Convolver::clearImpulseB()
{
    if (active_[1].load(std::memory_order_acquire) < 0) return;
    active_[1].store(-1, std::memory_order_release);
    // A step that began while B was playing chose it back then; its memory goes once those have ended.
    waitForQuiet();
    imp_[1][0] = Impulse{};
    imp_[1][1] = Impulse{};
}

void Convolver::analyse(Impulse& imp, const std::vector<float>* ch, bool stereo, int len)
{
    imp = Impulse{};
    imp.stereo = stereo;
    imp.samples = len;
    imp.seconds = static_cast<double>(len) / sr_;
    const int channels = stereo ? 2 : 1;
    int totalParts = 0;
    for (int g = 0; g < stages_; ++g) {
        const Stage& s = stage_[g];
        const long end = g + 1 < stages_ ? static_cast<long>(stage_[g + 1].start) : capSamples_;
        const long stop = std::min<long>(end, len);
        imp.st[g].parts = stop > s.start ? std::min(s.capParts, static_cast<int>((stop - s.start + s.block - 1) / s.block)) : 0;
        totalParts += imp.st[g].parts;
    }
    const double partBudget = 0.8 * kDropBudget / std::max(1, totalParts);
    for (int g = 0; g < stages_; ++g) {
        const Stage& s = stage_[g];
        Spectra& sp = imp.st[g];
        const int b = s.block, N = 2 * b;
        std::vector<float> re[2], im[2];
        std::vector<double> power(static_cast<size_t>(b + 1));
        sp.off.assign(static_cast<size_t>(sp.parts), 0);
        sp.cnt.assign(static_cast<size_t>(sp.parts), 0);
        for (int c = 0; c < channels; ++c) {
            sp.re[c].reserve(static_cast<size_t>(sp.parts) * static_cast<size_t>(s.slot));
            sp.im[c].reserve(static_cast<size_t>(sp.parts) * static_cast<size_t>(s.slot));
        }
        for (int p = 0; p < sp.parts; ++p) {
            const long from = s.start + static_cast<long>(p) * b;
            for (int c = 0; c < channels; ++c) {
                re[c].assign(static_cast<size_t>(N), 0.0f);
                im[c].assign(static_cast<size_t>(N), 0.0f);
                for (int i = 0; i < b && from + i < len; ++i) re[c][static_cast<size_t>(i)] = ch[c][static_cast<size_t>(from + i)];
                FftRun run;
                long long budget = kUnlimited;
                runFft(s.fft, run, re[c].data(), im[c].data(), false, budget);
            }
            for (int k = 0; k <= b; ++k) {
                double pk = 0.0;
                for (int c = 0; c < channels; ++c) {
                    const double v = static_cast<double>(re[c][static_cast<size_t>(k)]) * re[c][static_cast<size_t>(k)]
                                   + static_cast<double>(im[c][static_cast<size_t>(k)]) * im[c][static_cast<size_t>(k)];
                    pk = std::max(pk, v);
                }
                power[static_cast<size_t>(k)] = pk;
            }
            // The highest bin that matters: everything above it, mirror half included, stays
            // inside this partition's share of the budget (Parseval: bin power / N is energy).
            double dropped = 0.0;
            int top = -1;
            for (int k = b; k >= 0; --k) {
                const double v = (k == 0 || k == b ? 1.0 : 2.0) * power[static_cast<size_t>(k)] / N;
                if (dropped + v > partBudget) { top = k; break; }
                dropped += v;
            }
            const int count = top < 0 ? 0 : std::min(roundUp8(top + 1), s.slot);
            sp.off[static_cast<size_t>(p)] = static_cast<int>(sp.re[0].size());
            sp.cnt[static_cast<size_t>(p)] = count;
            const int real = std::min(count, b + 1);   // bins past b are the mirror half: padding is zeros
            for (int c = 0; c < channels; ++c) {
                sp.re[c].insert(sp.re[c].end(), re[c].begin(), re[c].begin() + real);
                sp.im[c].insert(sp.im[c].end(), im[c].begin(), im[c].begin() + real);
                sp.re[c].insert(sp.re[c].end(), static_cast<size_t>(count - real), 0.0f);
                sp.im[c].insert(sp.im[c].end(), static_cast<size_t>(count - real), 0.0f);
            }
            imp.kept += count;
            imp.full += b + 1;
        }
        for (int c = 0; c < channels; ++c) { sp.re[c].shrink_to_fit(); sp.im[c].shrink_to_fit(); }
    }
}

void Convolver::generateDefault(uint64_t seed, float seconds)
{
    std::vector<float> L, R;
    makeDefaultImpulse(sr_, seed, seconds, L, R);
    setImpulse(L.data(), R.data(), static_cast<int>(L.size()), sr_);
}

/**
 * @brief The built-in hall: the statistical late field (Polack 1993; Jot 1992) the generated library's
 *        rooms are made of, synthesised in the short-time Fourier domain so it needs nothing but this
 *        file's own transform.
 *
 * Two streams of Gaussian noise, a Hann frame of 2048 samples every 512; in
 * each frame every bin gets the design's initial spectrum and its own decay at the frame's time,
 * and the two ears are rotated into each other so that they are half-coherent in the bass and
 * independent above 300 Hz. Then a 15 ms onset and a 100 ms fade.
 *
 * The design (the tables above): T60 2.8 s through the mids, the bass held just under it, the
 * treble shortened by walls and air down to 0.37 s at 16 kHz; the colour solved for what measured
 * rooms have -- 15.8 % of the energy between 150 and 500 Hz, -15.6 dB below 150 Hz, a power
 * centroid of 2.0 kHz. The hall that stood here before was three bands of noise whose top band
 * leaked into the middle: a centroid of 8.7 kHz and a T30 of 3.3 s at 8 kHz, the brightest room
 * the instrument had, and the one every fresh instance played.
 */
void Convolver::makeDefaultImpulse(double sampleRate, uint64_t seed, float seconds, std::vector<float>& L, std::vector<float>& R)
{
    const int scale = sampleRate < 64000.0 ? 1 : (sampleRate < 128000.0 ? 2 : 4);
    const int N = 2048 * scale, hop = N / 4, half = N / 2;
    const int n = std::max(1, static_cast<int>(std::lround(static_cast<double>(seconds) * sampleRate)));
    constexpr double kLn1e6 = 13.815510557964274;                   // energy down 60 dB at this exponent
    std::vector<double> amp(static_cast<size_t>(half + 1)), rate(static_cast<size_t>(half + 1));
    std::vector<float> rotC(static_cast<size_t>(half + 1)), rotS(static_cast<size_t>(half + 1));
    for (int k = 0; k <= half; ++k) {
        const double f = k * sampleRate / N;
        double bDb = 0.0, t60 = 1.0;
        octaveDesign(f, bDb, t60);
        amp[static_cast<size_t>(k)] = std::pow(10.0, bDb / 20.0);
        rate[static_cast<size_t>(k)] = 0.5 * kLn1e6 / t60;        // of the amplitude, per second
        const double rho = 0.05 + (0.5 - 0.05) / (1.0 + (f / 300.0) * (f / 300.0));
        const double th = 0.5 * std::asin(rho);
        rotC[static_cast<size_t>(k)] = static_cast<float>(std::cos(th));
        rotS[static_cast<size_t>(k)] = static_cast<float>(std::sin(th));
    }
    Rng rng; rng.seed(seed + 77);
    const int pad = N;
    const int frames = (n + 2 * pad + hop - 1) / hop;
    const int total = (frames - 1) * hop + N;
    std::vector<float> noise[2], out[2];
    for (int c = 0; c < 2; ++c) {
        noise[c].resize(static_cast<size_t>(total));
        for (int i = 0; i < total; ++i) {                          // Box-Muller: the late field is Gaussian
            const double u1 = std::max(1.0e-12, static_cast<double>(rng.uniform()));
            const double u2 = rng.uniform();
            noise[c][static_cast<size_t>(i)] = static_cast<float>(std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPiD * u2));
        }
        out[c].assign(static_cast<size_t>(total), 0.0f);
    }
    std::vector<float> win(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) win[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPiD * i / N));
    StepFft fft;
    fft.init(N);
    std::vector<float> re0(static_cast<size_t>(N)), im0(static_cast<size_t>(N)), re1(static_cast<size_t>(N)), im1(static_cast<size_t>(N));
    std::vector<float> lr(static_cast<size_t>(N)), li(static_cast<size_t>(N)), rr(static_cast<size_t>(N)), ri(static_cast<size_t>(N));
    std::vector<float> g(static_cast<size_t>(half + 1));
    const float inv = 1.0f / (1.5f * static_cast<float>(N));   // the inverse is unscaled; Hann in and out at a quarter hop sums to 1.5
    for (int j = 0; j < frames; ++j) {
        const size_t a = static_cast<size_t>(j) * static_cast<size_t>(hop);
        for (int i = 0; i < N; ++i) {
            re0[static_cast<size_t>(i)] = win[static_cast<size_t>(i)] * noise[0][a + static_cast<size_t>(i)];
            re1[static_cast<size_t>(i)] = win[static_cast<size_t>(i)] * noise[1][a + static_cast<size_t>(i)];
            im0[static_cast<size_t>(i)] = im1[static_cast<size_t>(i)] = 0.0f;
        }
        FftRun r0, r1, r2, r3;
        long long b0 = kUnlimited, b1 = kUnlimited, b2 = kUnlimited, b3 = kUnlimited;
        runFft(fft, r0, re0.data(), im0.data(), false, b0);
        runFft(fft, r1, re1.data(), im1.data(), false, b1);
        const double t = std::min(static_cast<double>(seconds), std::max(0.0, (static_cast<double>(a) + 0.5 * N - pad) / sampleRate));
        for (int k = 0; k <= half; ++k)
            g[static_cast<size_t>(k)] = static_cast<float>(amp[static_cast<size_t>(k)] * std::exp(-rate[static_cast<size_t>(k)] * t));
        for (int i = 0; i < N; ++i) {
            const size_t k = static_cast<size_t>(i <= half ? i : N - i);
            const float c = rotC[k] * g[k], s = rotS[k] * g[k];
            const size_t u = static_cast<size_t>(i);
            lr[u] = c * re0[u] + s * re1[u];  li[u] = c * im0[u] + s * im1[u];
            rr[u] = s * re0[u] + c * re1[u];  ri[u] = s * im0[u] + c * im1[u];
        }
        runFft(fft, r2, lr.data(), li.data(), true, b2);
        runFft(fft, r3, rr.data(), ri.data(), true, b3);
        for (int i = 0; i < N; ++i) {
            out[0][a + static_cast<size_t>(i)] += inv * win[static_cast<size_t>(i)] * lr[static_cast<size_t>(i)];
            out[1][a + static_cast<size_t>(i)] += inv * win[static_cast<size_t>(i)] * rr[static_cast<size_t>(i)];
        }
    }
    L.assign(out[0].begin() + pad, out[0].begin() + pad + n);
    R.assign(out[1].begin() + pad, out[1].begin() + pad + n);
    const int onset = std::max(1, static_cast<int>(0.015 * sampleRate));
    const int fadeN = std::max(1, std::min(n / 10, static_cast<int>(0.1 * sampleRate)));
    for (int i = 0; i < n; ++i) {
        double w = 1.0;
        if (i < onset) { const double s = std::sin(0.5 * kPiD * i / onset); w = s * s; }
        if (i >= n - fadeN) { const double c = std::cos(0.5 * kPiD * (i - (n - fadeN)) / fadeN); w *= c * c; }
        L[static_cast<size_t>(i)] *= static_cast<float>(w);
        R[static_cast<size_t>(i)] *= static_cast<float>(w);
    }
}

/** @brief Message thread: see Engine::waitForQuiet. */
void Convolver::waitForQuiet()
{
    const unsigned long long begun = stepsBegun_.load(std::memory_order_acquire);
    for (int spin = 0; spin < 8000; ++spin) {
        if (stepsDone_.load(std::memory_order_acquire) >= begun) return;
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

// ---------------------------------------------------------------- the audio thread

void Convolver::begin(Stage& s)
{
    const int b = s.block;
    const int size = histMask_ + 1;
    const int from = static_cast<int>((t_ - b) & histMask_);
    const int first = std::min(b, size - from);
    for (int c = 0; c < 2; ++c) {
        float* re = s.re[c].data();
        std::memcpy(re, hist_[c].data() + from, sizeof(float) * static_cast<size_t>(first));
        if (first < b) std::memcpy(re + first, hist_[c].data(), sizeof(float) * static_cast<size_t>(b - first));
        std::memset(re + b, 0, sizeof(float) * static_cast<size_t>(b));
        std::memset(s.im[c].data(), 0, sizeof(float) * static_cast<size_t>(2 * b));
    }
    s.phase = kFftIn; s.ch = 0; s.run = FftRun{};
    // The answer begins where this block meets the stretch's first partition, one latency on. It
    // is due then -- and by the step before this stage's next block at the latest, so a stage
    // never holds two blocks at once.
    s.write = t_ - b + s.start + head_;
    s.due = std::min(s.write, t_ + b - head_) / head_;
    const float m = std::min(1.0f, std::max(0.0f, morph_));
    s.wa = 1.0f - m;
    s.wb = m;
    const int parts = std::min(s.fdlFilled + 1, s.capParts);
    s.work = 4 * s.fft.work + 4LL * b + static_cast<long long>(parts) * 2LL * (b + 1) + 2LL * b;
}

long long Convolver::advance(Stage& s, int g, long long quota, const Impulse* A, const Impulse* B)
{
    const long long given = quota;
    const int b = s.block, N = 2 * b;
    while (s.phase != kIdle && quota > 0) {
        switch (s.phase) {
        case kFftIn:
            if (runFft(s.fft, s.run, s.re[s.ch].data(), s.im[s.ch].data(), false, quota)) {
                s.run = FftRun{};
                if (++s.ch == 2) s.phase = kPush;
            }
            break;
        case kPush:
            for (int c = 0; c < 2; ++c) {
                const size_t at = static_cast<size_t>(s.fdlHead) * static_cast<size_t>(s.slot);
                float* dr = s.fdlRe[c].data() + at;
                float* di = s.fdlIm[c].data() + at;
                std::memcpy(dr, s.re[c].data(), sizeof(float) * static_cast<size_t>(b + 1));
                std::memcpy(di, s.im[c].data(), sizeof(float) * static_cast<size_t>(b + 1));
                std::memset(dr + b + 1, 0, sizeof(float) * static_cast<size_t>(s.slot - b - 1));
                std::memset(di + b + 1, 0, sizeof(float) * static_cast<size_t>(s.slot - b - 1));
                // the sum starts from nothing; the mirror half is written whole before the inverse
                std::memset(s.re[c].data(), 0, sizeof(float) * static_cast<size_t>(s.slot));
                std::memset(s.im[c].data(), 0, sizeof(float) * static_cast<size_t>(s.slot));
            }
            s.fdlHead = (s.fdlHead + 1) % s.capParts;
            s.fdlFilled = std::min(s.fdlFilled + 1, s.capParts);
            s.parts = s.fdlFilled; s.part = 0; s.bin = 0;
            s.phase = kSum;
            quota -= 2LL * b;
            break;
        case kSum:
            sum(s, g, A, B, quota);
            break;
        case kInvert:
            if (runFft(s.fft, s.run, s.re[s.ch].data(), s.im[s.ch].data(), true, quota)) {
                s.run = FftRun{};
                if (++s.ch == 2) s.phase = kDeliver;
            }
            break;
        case kDeliver: {
            const float inv = 1.0f / static_cast<float>(N);
            const int size = ringMask_ + 1;
            for (int c = 0; c < 2; ++c) {
                const float* src = s.re[c].data();
                float* dst = ring_[c].data();
                int at = static_cast<int>(s.write & ringMask_);
                for (int i = 0; i < N - 1;) {
                    const int m = std::min(N - 1 - i, size - at);
                    for (int j = 0; j < m; ++j) dst[at + j] += inv * src[i + j];
                    i += m;
                    at = (at + m) & ringMask_;
                }
            }
            s.phase = kIdle;
            quota -= 2LL * b;
            break;
        }
        default:
            s.phase = kIdle;
            break;
        }
    }
    return given - quota;
}

void Convolver::sum(Stage& s, int g, const Impulse* A, const Impulse* B, long long& quota)
{
    const Spectra* sa = (A != nullptr && s.wa > 0.0f) ? &A->st[g] : nullptr;
    const Spectra* sb = (B != nullptr && s.wb > 0.0f) ? &B->st[g] : nullptr;
    float* a0r = s.re[0].data(); float* a0i = s.im[0].data();
    float* a1r = s.re[1].data(); float* a1i = s.im[1].data();
    while (s.part < s.parts) {
        if (quota <= 0) return;
        const int p = s.part;
        const int ca = (sa != nullptr && p < sa->parts) ? sa->cnt[static_cast<size_t>(p)] : 0;
        const int cb = (sb != nullptr && p < sb->parts) ? sb->cnt[static_cast<size_t>(p)] : 0;
        const int top = std::max(ca, cb);
        if (s.bin >= top) { ++s.part; s.bin = 0; continue; }
        // Partition p meets the input block p blocks older than the newest.
        int slotIndex = s.fdlHead - 1 - p;
        if (slotIndex < 0) slotIndex += s.capParts;
        const size_t xo = static_cast<size_t>(slotIndex) * static_cast<size_t>(s.slot);
        const float* x0r = s.fdlRe[0].data() + xo; const float* x0i = s.fdlIm[0].data() + xo;
        const float* x1r = s.fdlRe[1].data() + xo; const float* x1i = s.fdlIm[1].data() + xo;
        const int k0 = s.bin;
        const int chunk = roundUp8(static_cast<int>(std::min<long long>(std::max<long long>(64, quota / 2), 1 << 24)));
        const int k1 = std::min(top, k0 + chunk);
        const int both = std::min(ca, cb);
        if (k0 < both) {   // the two impulses blended
            const int e = std::min(k1, both);
            const size_t oa = static_cast<size_t>(sa->off[static_cast<size_t>(p)]), ob = static_cast<size_t>(sb->off[static_cast<size_t>(p)]);
            if (!A->stereo && !B->stereo) {
                blendTwo(a0r, a0i, a1r, a1i, x0r, x0i, x1r, x1i,
                         sa->re[0].data() + oa, sa->im[0].data() + oa, s.wa, sb->re[0].data() + ob, sb->im[0].data() + ob, s.wb, k0, e);
            } else {
                const int a0 = 0, a1 = A->stereo ? 1 : 0, b0 = 0, b1 = B->stereo ? 1 : 0;
                blendOne(a0r, a0i, x0r, x0i, sa->re[a0].data() + oa, sa->im[a0].data() + oa, s.wa, sb->re[b0].data() + ob, sb->im[b0].data() + ob, s.wb, k0, e);
                blendOne(a1r, a1i, x1r, x1i, sa->re[a1].data() + oa, sa->im[a1].data() + oa, s.wa, sb->re[b1].data() + ob, sb->im[b1].data() + ob, s.wb, k0, e);
            }
        }
        const int f = std::max(k0, both);
        if (f < k1) {      // only the one that reaches further up
            const bool fromA = ca > cb;
            const Impulse* I = fromA ? A : B;
            const Spectra* sp = fromA ? sa : sb;
            const float w = fromA ? s.wa : s.wb;
            const size_t o = static_cast<size_t>(sp->off[static_cast<size_t>(p)]);
            if (!I->stereo) {
                sumTwo(a0r, a0i, a1r, a1i, x0r, x0i, x1r, x1i, sp->re[0].data() + o, sp->im[0].data() + o, w, f, k1);
            } else {
                sumOne(a0r, a0i, x0r, x0i, sp->re[0].data() + o, sp->im[0].data() + o, w, f, k1);
                sumOne(a1r, a1i, x1r, x1i, sp->re[1].data() + o, sp->im[1].data() + o, w, f, k1);
            }
        }
        quota -= 2LL * (k1 - k0);
        s.bin = k1;
    }
    // Real input, so the upper half of the spectrum is the conjugate of the lower.
    const int b = s.block, N = 2 * b;
    for (int c = 0; c < 2; ++c) {
        float* re = s.re[c].data(); float* im = s.im[c].data();
        for (int k = 1; k < b; ++k) { re[N - k] = re[k]; im[N - k] = -im[k]; }
    }
    quota -= 2LL * b;
    s.phase = kInvert; s.ch = 0; s.run = FftRun{};
}

void Convolver::step()
{
    stepsBegun_.fetch_add(1, std::memory_order_acq_rel);
    // Every way out of this function has to say so, including this one. A step that is counted
    // as begun and never as done leaves the two counters apart for good, and every loader after
    // it waits out its whole timeout for a step that ended long ago.
    struct Done {
        std::atomic<unsigned long long>& c;
        ~Done() { c.fetch_add(1, std::memory_order_release); }
    } done { stepsDone_ };
    const int ia = active_[0].load(std::memory_order_acquire), ib = active_[1].load(std::memory_order_acquire);
    const Impulse* A = ia >= 0 ? &imp_[0][ia] : nullptr;
    const Impulse* B = ib >= 0 ? &imp_[1][ib] : nullptr;
    const long long now = t_ / head_;
    for (int g = 0; g < stages_; ++g) {
        Stage& s = stage_[g];
        if (t_ % s.block == 0) {
            // The schedule finishes every block a step before the next; should one ever be left,
            // it is finished here rather than dropped.
            if (s.phase != kIdle) advance(s, g, kUnlimited, A, B);
            begin(s);
        }
        if (s.phase == kIdle) continue;
        // An equal share of what is left for every step until it is due; the last takes the rest.
        const long long left = s.due - now + 1;
        const long long quota = left <= 1 ? kUnlimited : std::max<long long>(1, (s.work + left - 1) / left);
        const long long used = advance(s, g, quota, A, B);
        s.work = std::max<long long>(1, s.work - used);
    }
}

void Convolver::process(const float* inL, const float* inR, float* outL, float* outR, int n)
{
    int i = 0;
    while (i < n) {
        const int toStep = head_ - static_cast<int>(t_ & (head_ - 1));
        const int m = std::min(n - i, toStep);
        // In before out, so the outputs may be the inputs.
        ringWrite(hist_[0], static_cast<int>(t_ & histMask_), inL + i, m);
        ringWrite(hist_[1], static_cast<int>(t_ & histMask_), inR + i, m);
        ringTake(ring_[0], static_cast<int>(t_ & ringMask_), outL + i, m);
        ringTake(ring_[1], static_cast<int>(t_ & ringMask_), outR + i, m);
        t_ += m;
        i += m;
        if ((t_ & (head_ - 1)) == 0) step();
    }
}

} // namespace ambient
