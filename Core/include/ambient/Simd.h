// Noctuary -- the loops worth vectorising by hand.
//
// Every voice is a bank of rotating phasors: per sample and per partial, one complex multiply to
// turn the phasor, one multiply-add into the sum, one add to ramp the amplitude. With up to six
// strands of thirty-two partials in sixteen voices, plus three source slots that run the same
// bank, this loop is most of the instrument's arithmetic. The compiler vectorises parts of it,
// but not the reduction, because floating-point addition is not associative and it may not
// reorder the sum on its own. Doing it here is the one place where saying so explicitly pays.
//
// The bank has THREE of these loops, and until 13.09.2026 only the first was written out:
//
//   phasorBankStep        the plain one: one sum, every partial in the middle.
//   phasorBankStepStereo  a left and a right weight per partial -- Partial Spread, which gives
//                         every partial its own place between the ears. Two sums.
//   phasorBankStepFm      the feedback loop phase-modulating every partial: one more rotation by
//                         a clamped angle, and two Newton steps to put the phasor back on the
//                         unit circle.
//
// Measured on one additive source at 32 partials and six strands, everything else shut: the plain
// loop 4.9 % of a core, the stereo one 10.3 % (+111 %), the FM one 16.7 % (+241 %). 2332 presets
// of the 2.0 library carry Partial Spread and 1038 carry feedback FM -- about a quarter of it --
// so two of the three hot loops were scalar for most of the presets that are dear to begin with.
// They are the same arithmetic as the first, with two accumulators or one more rotation.
//
// The scalar path stays, and is what any platform without AVX2 or NEON compiles: the same
// arithmetic in a different order, and the difference between them is the last bit or two of the
// sum. The Quest's arm64 now takes the NEON path here rather than the scalar one -- four lanes
// instead of eight, and a horizontal add that is written out because AArch64's vaddvq is not in
// the test shim.
#pragma once
#include "Dsp.h"

#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX__)) || (defined(_MSC_VER) && defined(_M_X64) && defined(AMBIENT_AVX))
  #define AMBIENT_HAS_AVX 1
  #include <immintrin.h>
#else
  #define AMBIENT_HAS_AVX 0
#endif

// The other half of the instrument's world: the Quest's arm64. Four lanes rather than eight, and
// no gather at all -- a grain reads eight different places in a clip, and on NEON those are eight
// loads whatever else happens. What vectorises there is the window and the two accumulations,
// which is most of the arithmetic but not the memory.
// AMBIENT_NEON_SHIM is an x86 test build that runs the NEON paths through Tests/neonshim/arm_neon.h,
// which spells the few intrinsics out lane by lane (see Tests/CMakeLists.txt).
#if defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(AMBIENT_NEON_SHIM)
  #define AMBIENT_HAS_NEON 1
  #include <arm_neon.h>
#else
  #define AMBIENT_HAS_NEON 0
#endif

namespace ambient {

#if AMBIENT_HAS_AVX
// The eight lanes added up. Written once: three loops end with it.
inline float horizontalSum(__m256 acc)
{
    __m128 lo = _mm256_castps256_ps128(acc), hi = _mm256_extractf128_ps(acc, 1);
    lo = _mm_add_ps(lo, hi);
    lo = _mm_add_ps(lo, _mm_movehl_ps(lo, lo));
    lo = _mm_add_ss(lo, _mm_shuffle_ps(lo, lo, 1));
    return _mm_cvtss_f32(lo);
}
#elif AMBIENT_HAS_NEON
// The same for four lanes, through memory rather than through vaddvq: the test shim spells out
// only the intrinsics the instrument uses, and one store is not worth another entry in it.
inline float horizontalSum(float32x4_t acc)
{
    float v[4];
    vst1q_f32(v, acc);
    return (v[0] + v[1]) + (v[2] + v[3]);
}
#endif

// The same step with a left and a right weight per partial: two sums instead of one, for the
// bank whose partials are spread across the field one by one (Partial Spread).
inline void phasorBankStepStereo(float* pc, float* ps, const float* rc, const float* rs,
                                 float* amp, const float* step, int n,
                                 const float* wL, const float* wR, float& outL, float& outR)
{
    int h = 0;
    float sumL = 0.0f, sumR = 0.0f;
#if AMBIENT_HAS_AVX
    __m256 accL = _mm256_setzero_ps(), accR = _mm256_setzero_ps();
    for (; h + 8 <= n; h += 8) {
        const __m256 c = _mm256_loadu_ps(pc + h), s = _mm256_loadu_ps(ps + h);
        const __m256 kc = _mm256_loadu_ps(rc + h), ks = _mm256_loadu_ps(rs + h);
        const __m256 a = _mm256_loadu_ps(amp + h);
        const __m256 v = _mm256_mul_ps(a, s);
        accL = _mm256_add_ps(accL, _mm256_mul_ps(v, _mm256_loadu_ps(wL + h)));
        accR = _mm256_add_ps(accR, _mm256_mul_ps(v, _mm256_loadu_ps(wR + h)));
        _mm256_storeu_ps(pc + h, _mm256_sub_ps(_mm256_mul_ps(c, kc), _mm256_mul_ps(s, ks)));
        _mm256_storeu_ps(ps + h, _mm256_add_ps(_mm256_mul_ps(s, kc), _mm256_mul_ps(c, ks)));
        _mm256_storeu_ps(amp + h, _mm256_add_ps(a, _mm256_loadu_ps(step + h)));
    }
    sumL = horizontalSum(accL); sumR = horizontalSum(accR);
#elif AMBIENT_HAS_NEON
    float32x4_t accL = vdupq_n_f32(0.0f), accR = vdupq_n_f32(0.0f);
    for (; h + 4 <= n; h += 4) {
        const float32x4_t c = vld1q_f32(pc + h), s = vld1q_f32(ps + h);
        const float32x4_t kc = vld1q_f32(rc + h), ks = vld1q_f32(rs + h);
        const float32x4_t a = vld1q_f32(amp + h);
        const float32x4_t v = vmulq_f32(a, s);
        accL = vmlaq_f32(accL, v, vld1q_f32(wL + h));
        accR = vmlaq_f32(accR, v, vld1q_f32(wR + h));
        vst1q_f32(pc + h, vmlsq_f32(vmulq_f32(c, kc), s, ks));
        vst1q_f32(ps + h, vmlaq_f32(vmulq_f32(s, kc), c, ks));
        vst1q_f32(amp + h, vaddq_f32(a, vld1q_f32(step + h)));
    }
    sumL = horizontalSum(accL); sumR = horizontalSum(accR);
#endif
    for (; h < n; ++h) {
        const float v = amp[h] * ps[h];
        sumL += v * wL[h];
        sumR += v * wR[h];
        const float nc = pc[h] * rc[h] - ps[h] * rs[h];
        ps[h] = ps[h] * rc[h] + pc[h] * rs[h];
        pc[h] = nc;
        amp[h] += step[h];
    }
    outL = sumL; outR = sumR;
}

// One sample of a phasor bank: turns every phasor by its own rotation, sums amp*sin, and steps
// the amplitudes. Returns the sum. `n` may be anything from 0 to the array length.
inline float phasorBankStep(float* pc, float* ps, const float* rc, const float* rs,
                            float* amp, const float* step, int n)
{
    int h = 0;
    float sum = 0.0f;
#if AMBIENT_HAS_AVX
    __m256 acc = _mm256_setzero_ps();
    for (; h + 8 <= n; h += 8) {
        const __m256 c = _mm256_loadu_ps(pc + h), s = _mm256_loadu_ps(ps + h);
        const __m256 kc = _mm256_loadu_ps(rc + h), ks = _mm256_loadu_ps(rs + h);
        const __m256 a = _mm256_loadu_ps(amp + h);
        acc = _mm256_add_ps(acc, _mm256_mul_ps(a, s));
        // (c, s) turned by (kc, ks): the complex product, which is two multiplies and a subtract
        // for the real part and the same for the imaginary one.
        _mm256_storeu_ps(pc + h, _mm256_sub_ps(_mm256_mul_ps(c, kc), _mm256_mul_ps(s, ks)));
        _mm256_storeu_ps(ps + h, _mm256_add_ps(_mm256_mul_ps(s, kc), _mm256_mul_ps(c, ks)));
        _mm256_storeu_ps(amp + h, _mm256_add_ps(a, _mm256_loadu_ps(step + h)));
    }
    sum = horizontalSum(acc);
#elif AMBIENT_HAS_NEON
    float32x4_t acc = vdupq_n_f32(0.0f);
    for (; h + 4 <= n; h += 4) {
        const float32x4_t c = vld1q_f32(pc + h), s = vld1q_f32(ps + h);
        const float32x4_t kc = vld1q_f32(rc + h), ks = vld1q_f32(rs + h);
        const float32x4_t a = vld1q_f32(amp + h);
        acc = vmlaq_f32(acc, a, s);
        vst1q_f32(pc + h, vmlsq_f32(vmulq_f32(c, kc), s, ks));
        vst1q_f32(ps + h, vmlaq_f32(vmulq_f32(s, kc), c, ks));
        vst1q_f32(amp + h, vaddq_f32(a, vld1q_f32(step + h)));
    }
    sum = horizontalSum(acc);
#endif
    for (; h < n; ++h) {
        sum += amp[h] * ps[h];
        const float nc = pc[h] * rc[h] - ps[h] * rs[h];
        ps[h] = ps[h] * rc[h] + pc[h] * rs[h];
        pc[h] = nc;
        amp[h] += step[h];
    }
    return sum;
}

// The bank with the feedback loop phase-modulating every partial: after its own rotation, each
// phasor is turned again by the modulation angle h*theta -- a small-angle rotation by tan = t,
// clamped, so deep modulation saturates softly on the high partials instead of tearing -- and
// then put back on the unit circle by two Newton steps of the inverse square root. `hf` is the
// partial's harmonic number, `theta` the modulation for this sample, `maxStep` the clamp.
inline float phasorBankStepFm(float* pc, float* ps, const float* rc, const float* rs,
                              float* amp, const float* step, int n,
                              const float* hf, float theta, float maxStep)
{
    int h = 0;
    float sum = 0.0f;
#if AMBIENT_HAS_AVX
    __m256 acc = _mm256_setzero_ps();
    const __m256 vth = _mm256_set1_ps(theta);
    const __m256 vhi = _mm256_set1_ps(maxStep), vlo = _mm256_set1_ps(-maxStep);
    const __m256 v15 = _mm256_set1_ps(1.5f), v05 = _mm256_set1_ps(0.5f);
    for (; h + 8 <= n; h += 8) {
        const __m256 c = _mm256_loadu_ps(pc + h), s = _mm256_loadu_ps(ps + h);
        const __m256 kc = _mm256_loadu_ps(rc + h), ks = _mm256_loadu_ps(rs + h);
        const __m256 a = _mm256_loadu_ps(amp + h);
        acc = _mm256_add_ps(acc, _mm256_mul_ps(a, s));
        const __m256 nc = _mm256_sub_ps(_mm256_mul_ps(c, kc), _mm256_mul_ps(s, ks));
        const __m256 ns = _mm256_add_ps(_mm256_mul_ps(s, kc), _mm256_mul_ps(c, ks));
        const __m256 t = _mm256_min_ps(_mm256_max_ps(_mm256_mul_ps(vth, _mm256_loadu_ps(hf + h)), vlo), vhi);
        const __m256 c2 = _mm256_sub_ps(nc, _mm256_mul_ps(ns, t));
        const __m256 s2 = _mm256_add_ps(ns, _mm256_mul_ps(nc, t));
        const __m256 r2 = _mm256_add_ps(_mm256_mul_ps(c2, c2), _mm256_mul_ps(s2, s2));
        __m256 fix = _mm256_sub_ps(v15, _mm256_mul_ps(v05, r2));
        // fix * (1.5 - 0.5 r2 fix fix), multiplied left to right as the scalar path has it.
        fix = _mm256_mul_ps(fix, _mm256_sub_ps(v15, _mm256_mul_ps(_mm256_mul_ps(_mm256_mul_ps(v05, r2), fix), fix)));
        _mm256_storeu_ps(pc + h, _mm256_mul_ps(c2, fix));
        _mm256_storeu_ps(ps + h, _mm256_mul_ps(s2, fix));
        _mm256_storeu_ps(amp + h, _mm256_add_ps(a, _mm256_loadu_ps(step + h)));
    }
    sum = horizontalSum(acc);
#elif AMBIENT_HAS_NEON
    float32x4_t acc = vdupq_n_f32(0.0f);
    const float32x4_t vth = vdupq_n_f32(theta);
    const float32x4_t vhi = vdupq_n_f32(maxStep), vlo = vdupq_n_f32(-maxStep);
    const float32x4_t v15 = vdupq_n_f32(1.5f), v05 = vdupq_n_f32(0.5f);
    for (; h + 4 <= n; h += 4) {
        const float32x4_t c = vld1q_f32(pc + h), s = vld1q_f32(ps + h);
        const float32x4_t kc = vld1q_f32(rc + h), ks = vld1q_f32(rs + h);
        const float32x4_t a = vld1q_f32(amp + h);
        acc = vmlaq_f32(acc, a, s);
        const float32x4_t nc = vmlsq_f32(vmulq_f32(c, kc), s, ks);
        const float32x4_t ns = vmlaq_f32(vmulq_f32(s, kc), c, ks);
        const float32x4_t t = vminq_f32(vmaxq_f32(vmulq_f32(vth, vld1q_f32(hf + h)), vlo), vhi);
        const float32x4_t c2 = vmlsq_f32(nc, ns, t);
        const float32x4_t s2 = vmlaq_f32(ns, nc, t);
        const float32x4_t r2 = vmlaq_f32(vmulq_f32(c2, c2), s2, s2);
        float32x4_t fix = vmlsq_f32(v15, v05, r2);
        fix = vmulq_f32(fix, vmlsq_f32(v15, vmulq_f32(vmulq_f32(v05, r2), fix), fix));
        vst1q_f32(pc + h, vmulq_f32(c2, fix));
        vst1q_f32(ps + h, vmulq_f32(s2, fix));
        vst1q_f32(amp + h, vaddq_f32(a, vld1q_f32(step + h)));
    }
    sum = horizontalSum(acc);
#endif
    for (; h < n; ++h) {
        sum += amp[h] * ps[h];
        const float nc = pc[h] * rc[h] - ps[h] * rs[h];
        const float ns = ps[h] * rc[h] + pc[h] * rs[h];
        const float t = clampv(theta * hf[h], -maxStep, maxStep);
        const float c2 = nc - ns * t, s2 = ns + nc * t;
        const float r2 = c2 * c2 + s2 * s2;
        float fix = 1.5f - 0.5f * r2;
        fix *= 1.5f - 0.5f * r2 * fix * fix;
        pc[h] = c2 * fix; ps[h] = s2 * fix;
        amp[h] += step[h];
    }
    return sum;
}

} // namespace ambient
