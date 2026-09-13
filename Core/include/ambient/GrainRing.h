// Noctuary -- the grain loop the granular effects share.
//
// The texture sources read their grains out of a finished clip; the Cloud and the Memory read
// theirs out of a ring that is being written while they play. The loop is the one Sources.cpp
// worked out with VTune for the clips (10.09.): the lifetime is decided once per block and not
// once per sample, the vector runs over eight SAMPLES of one grain and never over eight grains
// (they all add into the same output sample), and the Hann window is eight phasors seeded at
// r^0..r^7 and turned by r^8. A ring needs one change to that: both places the interpolation
// reads are folded back into the ring before the gather -- by a compare and a subtract rather
// than a mask, so a ring does not have to be a power of two long.
#pragma once
#include "Simd.h"

namespace ambient {

struct RingGrain {
    double pos = 0.0;                                     // read position in frames, in [0, cap)
    double rate = 1.0;                                    // frames read per sample, > 0
    float  wc = 1.0f, ws = 0.0f, rc = 1.0f, rs = 0.0f;    // the window's phasor and its step
    float  gl = 0.0f, gr = 0.0f;                          // gains into the left and the right output
    int    len = 0, age = 0;                              // window length and samples played so far
    int    start = 0;                                     // first sample of the current block it plays in
    int    bus = 0;                                       // which output it plays into (the cloud's resonators)
    int    ring = 0;                                      // which ring it reads (the memory's lines)
};

namespace detail {

template <int CH, bool VEC>
inline void ringGrainBody(const float* ring, int cap, RingGrain& g, float* oL, float* oR, int count)
{
    const double base = g.pos, rate = g.rate;
    const float gl = g.gl, gr = g.gr, rc = g.rc, rs = g.rs;
    float wc = g.wc, ws = g.ws;
    int i = 0;
#if AMBIENT_HAS_AVX && defined(__AVX2__)
    if (VEC && count >= 8) {
        alignas(32) float wcL[8], wsL[8];
        {
            float c = wc, sn = ws;
            for (int k = 0; k < 8; ++k) {
                wcL[k] = c; wsL[k] = sn;
                const float nc = c * rc - sn * rs;
                sn = sn * rc + c * rs;
                c = nc;
            }
        }
        const float c2 = rc * rc - rs * rs, s2 = 2.0f * rc * rs;   // r^2
        const float c4 = c2 * c2 - s2 * s2, s4 = 2.0f * c2 * s2;   // r^4
        const float c8 = c4 * c4 - s4 * s4, s8 = 2.0f * c4 * s4;   // r^8
        __m256 vwc = _mm256_load_ps(wcL), vws = _mm256_load_ps(wsL);
        const __m256 vc8 = _mm256_set1_ps(c8), vs8 = _mm256_set1_ps(s8);
        const __m256 vgl = _mm256_set1_ps(gl), vgr = _mm256_set1_ps(gr);
        const __m256 vhalf = _mm256_set1_ps(0.5f);
        const __m256d vrate = _mm256_set1_pd(rate);
        const __m256d kLo = _mm256_setr_pd(0.0, 1.0, 2.0, 3.0);
        const __m256d kHi = _mm256_setr_pd(4.0, 5.0, 6.0, 7.0);
        const __m256i vcap = _mm256_set1_epi32(cap);
        const __m256i vlast = _mm256_set1_epi32(cap - 1);
        const __m256i vone = _mm256_set1_epi32(1);
        for (; i + 8 <= count; i += 8) {
            const __m256d b  = _mm256_set1_pd(base + rate * i);
            const __m256d pa = _mm256_add_pd(b, _mm256_mul_pd(vrate, kLo));
            const __m256d pb = _mm256_add_pd(b, _mm256_mul_pd(vrate, kHi));
            const __m128i ia = _mm256_cvttpd_epi32(pa);
            const __m128i ib = _mm256_cvttpd_epi32(pb);
            __m256i i0 = _mm256_insertf128_si256(_mm256_castsi128_si256(ia), ib, 1);
            const __m128 fa = _mm256_cvtpd_ps(_mm256_sub_pd(pa, _mm256_cvtepi32_pd(ia)));
            const __m128 fb = _mm256_cvtpd_ps(_mm256_sub_pd(pb, _mm256_cvtepi32_pd(ib)));
            const __m256 frac = _mm256_insertf128_ps(_mm256_castps128_ps256(fa), fb, 1);
            // Into the ring: a position past its end is a position at its start.
            i0 = _mm256_sub_epi32(i0, _mm256_and_si256(_mm256_cmpgt_epi32(i0, vlast), vcap));
            __m256i i1 = _mm256_add_epi32(i0, vone);
            i1 = _mm256_sub_epi32(i1, _mm256_and_si256(_mm256_cmpgt_epi32(i1, vlast), vcap));
            __m256i a0 = i0, a1 = i1;
            if constexpr (CH == 2) { a0 = _mm256_add_epi32(i0, i0); a1 = _mm256_add_epi32(i1, i1); }
            const __m256 s0 = _mm256_i32gather_ps(ring, a0, 4);
            const __m256 s1 = _mm256_i32gather_ps(ring, a1, 4);
            const __m256 vL = _mm256_fmadd_ps(frac, _mm256_sub_ps(s1, s0), s0);
            __m256 vR = vL;
            if constexpr (CH == 2) {
                // The pair is interleaved, so the right channel's gather lands in the cache lines
                // the left one has just pulled in.
                const __m256 r0 = _mm256_i32gather_ps(ring + 1, a0, 4);
                const __m256 r1 = _mm256_i32gather_ps(ring + 1, a1, 4);
                vR = _mm256_fmadd_ps(frac, _mm256_sub_ps(r1, r0), r0);
            }
            const __m256 w = _mm256_sub_ps(vhalf, _mm256_mul_ps(vhalf, vwc));
            _mm256_storeu_ps(oL + i, _mm256_add_ps(_mm256_loadu_ps(oL + i), _mm256_mul_ps(_mm256_mul_ps(vL, w), vgl)));
            _mm256_storeu_ps(oR + i, _mm256_add_ps(_mm256_loadu_ps(oR + i), _mm256_mul_ps(_mm256_mul_ps(vR, w), vgr)));
            const __m256 nc = _mm256_sub_ps(_mm256_mul_ps(vwc, vc8), _mm256_mul_ps(vws, vs8));
            vws = _mm256_add_ps(_mm256_mul_ps(vws, vc8), _mm256_mul_ps(vwc, vs8));
            vwc = nc;
        }
        // Lane 0 is the phasor for sample i, which is where the tail picks it up.
        alignas(32) float lastC[8], lastS[8];
        _mm256_store_ps(lastC, vwc);
        _mm256_store_ps(lastS, vws);
        wc = lastC[0]; ws = lastS[0];
    }
#elif AMBIENT_HAS_NEON
    // The Quest (13.09.2026: it had none, and the Cloud's and the Memory's grains were the only
    // thing left running scalar there). Four lanes, and the interpolation stays scalar because
    // NEON has no gather -- four places in the ring are four loads however they are written --
    // exactly as the texture source's own grain loop has it in Sources.cpp. What vectorises is
    // the window, the level and the two accumulations: most of the arithmetic, none of the memory.
    if (VEC && count >= 4) {
        float wcL[4], wsL[4];
        {
            float c = wc, sn = ws;
            for (int k = 0; k < 4; ++k) {
                wcL[k] = c; wsL[k] = sn;
                const float nc = c * rc - sn * rs;
                sn = sn * rc + c * rs;
                c = nc;
            }
        }
        const float c2 = rc * rc - rs * rs, s2 = 2.0f * rc * rs;   // r^2
        const float c4 = c2 * c2 - s2 * s2, s4 = 2.0f * c2 * s2;   // r^4, which turns all four lanes
        float32x4_t vwc = vld1q_f32(wcL), vws = vld1q_f32(wsL);
        const float32x4_t vc4 = vdupq_n_f32(c4), vs4 = vdupq_n_f32(s4);
        const float32x4_t vgl = vdupq_n_f32(gl), vgr = vdupq_n_f32(gr);
        const float32x4_t vhalf = vdupq_n_f32(0.5f);
        for (; i + 4 <= count; i += 4) {
            float laneL[4], laneR[4];
            for (int k = 0; k < 4; ++k) {
                const double p = base + rate * (i + k);
                int i0 = static_cast<int>(p);
                const float f = static_cast<float>(p - i0);
                if (i0 >= cap) i0 -= cap;      // into the ring, by a subtract: a ring need not be a power of two
                int i1 = i0 + 1;
                if (i1 >= cap) i1 -= cap;
                const float l0 = ring[CH * i0], l1 = ring[CH * i1];
                laneL[k] = l0 + f * (l1 - l0);
                if constexpr (CH == 2) {
                    const float r0 = ring[2 * i0 + 1], r1 = ring[2 * i1 + 1];
                    laneR[k] = r0 + f * (r1 - r0);
                } else {
                    laneR[k] = laneL[k];
                }
            }
            const float32x4_t vL = vld1q_f32(laneL), vR = vld1q_f32(laneR);
            const float32x4_t w = vmlsq_f32(vhalf, vhalf, vwc);   // the Hann window: 0.5 - 0.5 cos
            vst1q_f32(oL + i, vmlaq_f32(vld1q_f32(oL + i), vmulq_f32(vL, w), vgl));
            vst1q_f32(oR + i, vmlaq_f32(vld1q_f32(oR + i), vmulq_f32(vR, w), vgr));
            const float32x4_t nc = vmlsq_f32(vmulq_f32(vwc, vc4), vws, vs4);
            vws = vmlaq_f32(vmulq_f32(vws, vc4), vwc, vs4);
            vwc = nc;
        }
        float lastC[4], lastS[4];
        vst1q_f32(lastC, vwc);
        vst1q_f32(lastS, vws);
        wc = lastC[0]; ws = lastS[0];
    }
#endif
    (void) sizeof(VEC);
    for (; i < count; ++i) {
        const double p = base + rate * i;
        int i0 = static_cast<int>(p);
        const float f = static_cast<float>(p - i0);
        if (i0 >= cap) i0 -= cap;
        int i1 = i0 + 1;
        if (i1 >= cap) i1 -= cap;
        const float w = 0.5f - 0.5f * wc;
        const float l0 = ring[CH * i0], l1 = ring[CH * i1];
        const float vL = l0 + f * (l1 - l0);
        float vR = vL;
        if constexpr (CH == 2) {
            const float r0 = ring[2 * i0 + 1], r1 = ring[2 * i1 + 1];
            vR = r0 + f * (r1 - r0);
        }
        oL[i] += vL * w * gl;
        oR[i] += vR * w * gr;
        const float nc = wc * rc - ws * rs;
        ws = ws * rc + wc * rs;
        wc = nc;
    }
    double np = base + rate * count;
    if (np >= cap) np -= cap;
    g.pos = np;
    g.wc = wc; g.ws = ws;
}

} // namespace detail

// Adds `count` samples of grain `g` into oL and oR. The ring holds `cap` frames of `channels`
// channels (1, or 2 interleaved). Requires g.pos in [0, cap), a positive rate, and
// g.pos + g.rate * count < 2 * cap - 1. Advances pos (folded back into the ring) and the window;
// the caller keeps the age and keeps the phasor on the unit circle between blocks.
inline void renderRingGrain(const float* ring, int cap, int channels, RingGrain& g, float* oL, float* oR, int count)
{
    if (channels == 2) detail::ringGrainBody<2, true>(ring, cap, g, oL, oR, count);
    else               detail::ringGrainBody<1, true>(ring, cap, g, oL, oR, count);
}

// Which path renderRingGrain was compiled to. A test that holds the vector path against the
// scalar one passes for nothing at all when there is no vector path to hold: this is how it says
// so. The Quest had none here until 13.09.2026 and the check had been green throughout.
inline const char* ringGrainPath()
{
#if AMBIENT_HAS_AVX && defined(__AVX2__)
    return "avx2";
#elif AMBIENT_HAS_NEON
    return "neon";
#else
    return "scalar";
#endif
}

// The same arithmetic without the vector path, so the self test can hold the two against each other.
inline void renderRingGrainScalar(const float* ring, int cap, int channels, RingGrain& g, float* oL, float* oR, int count)
{
    if (channels == 2) detail::ringGrainBody<2, false>(ring, cap, g, oL, oR, count);
    else               detail::ringGrainBody<1, false>(ring, cap, g, oL, oR, count);
}

} // namespace ambient
