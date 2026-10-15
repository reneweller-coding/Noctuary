/**
 * @file CircuitFilter.h
 * @brief The circuit models of the voice filter (26.09.2026): the Moog ladder, the OTA cascades of the
 *        Prophet and the Juno, the Oberheim SEM's state-variable filter and the diode ladder, solved
 *        sample by sample.
 *
 * Ported from Ephemeris (the Berlin School generator, eph/synth/Filters.h, the same day), where they
 * were written and tested: small-signal response within 0.15 dB of the analytic one, self-
 * oscillation within four per cent of the cutoff. Rene asked for the four that suit a drone
 * instrument; the Polivoks, the Wasp and the MS-20 were left behind, being built to scream.
 *
 * **Method.** Every model is the circuit's differential equation with its nonlinearities where the
 * circuit has them -- the transistor differential pairs of a ladder (tanh), the OTAs of a cascade
 * (tanh of their differential input), the diode pairs of a diode ladder. The capacitors are
 * integrated by the trapezoidal rule in the form of the topology-preserving transform (Zavalishin,
 * "The Art of VA Filter Design"): with g = tan(pi fc / fs) each state obeys v = s + g f(v), and after
 * the step s <- 2 v - s. The implicit equation is solved every sample with Newton-Raphson on the
 * analytic Jacobian, warm-started from the last sample -- the loop through the resonance resolved
 * exactly rather than broken by a sample of delay, which is what the voice's old Ladder does and
 * why its resonance drifts off its cutoff (D'Angelo and Välimäki 2014 for the Moog ladder). The
 * Jacobians are solved by their structure: a cascade is bidiagonal with the feedback in one corner
 * (forward substitution and one division), the diode ladder tridiagonal with that corner (Thomas
 * and Sherman-Morrison). A fixed number of iterations keeps the sound independent of the host's
 * blocks.
 *
 * **Cost** (measured 26.09.2026). As first ported -- scalar, one channel after the other, at four
 * times the rate through two Oversampler4 -- a circuit model cost 2500 to 3000 cycles a stereo sample
 * against 32 for LP 12, and a preset of the library went from eleven times realtime to two and a
 * half. Now 470 (Moog), 520 (SEM), 600 (Prophet, Juno) and 730 (Diode):
 *   - twice the rate instead of four (StereoOversampler2), as Ephemeris runs them. A circuit is not a
 *     bare curve: the stages after each saturation low-pass what it makes. A sine at full scale
 *     into a fully driven Moog (Cutoff 5 kHz) aliases at -75 dB at the base rate and at -101 dB,
 *     the measurement's floor, at twice the rate -- and four times measured no better anywhere;
 *   - both channels in one register (F4 below): the Newton steps are chains of dependent
 *     divisions, so the second lane rides along for free;
 *   - the Jacobian's diagonal inverted as soon as it is known, off the chain of the substitution;
 *   - and the oversampler's sums vectorised (Oversample.h).
 * Three Newton steps stay, as in Ephemeris: warm-started at twice the rate, two leave an error under
 * -128 dB of the signal for a quiet or a pushed input, but only -33 to -52 dB with the cutoff swept
 * up to 21 kHz at a resonance of 0.8, where three leave -72 to -103.
 * The models are templates, as in Ephemeris, on `float` and on F4.
 * @author Rene Weller
 *
 * @par Compile Flags:
 *   @arg @c AMBIENT_SCALAR_LANES - the four lanes as plain floats even where SSE is there (the scalar reference of the
 *                                  filter tests)
 *   @arg @c AMBIENT_CIRCUIT_NEWTON - Newton steps per sample of the circuit filters (3 unless the build sets another
 *                                    number)
 */
#pragma once
#include "Simd.h"
#include <algorithm>
#include <cmath>
#if !AMBIENT_HAS_NEON && !defined(AMBIENT_SCALAR_LANES) && (defined(_M_X64) || defined(__SSE2__))
  #include <immintrin.h>
#endif

namespace ambient {
namespace circuit {

#ifndef AMBIENT_CIRCUIT_NEWTON
/** @brief Newton steps per sample, unless the build sets another number (see Newton). */
#define AMBIENT_CIRCUIT_NEWTON 3
#endif
constexpr int Newton = AMBIENT_CIRCUIT_NEWTON;   ///< Newton steps per sample: fixed, so the sound does not depend on convergence tests

/**
 * @brief Four lanes of floats for the circuits: the left channel in lane 0, the right in lane 1.
 *
 * NEON on arm64 (and on x86 through Tests/neonshim, which is how the Quest's path is tested here),
 * SSE on x86-64 (AVX2 builds use its FMA), four floats anywhere else. Only what the circuits need:
 * the four operations, min, max, a multiply-add, and moving two samples in and out. LanePath
 * names the one that was compiled, for Tests/filtertest.cpp, whose scalar variant defines
 * AMBIENT_SCALAR_LANES to reach the last one on an x86 machine.
 */
#if AMBIENT_HAS_NEON
constexpr const char* LanePath = "neon";   ///< the path F4 was compiled with
struct F4 { float32x4_t m_v; };
inline F4 operator+(F4 a, F4 b) { return { vaddq_f32(a.m_v, b.m_v) }; }
inline F4 operator-(F4 a, F4 b) { return { vsubq_f32(a.m_v, b.m_v) }; }
inline F4 operator*(F4 a, F4 b) { return { vmulq_f32(a.m_v, b.m_v) }; }
inline F4 operator/(F4 a, F4 b) { return { vdivq_f32(a.m_v, b.m_v) }; }
inline F4 operator-(F4 a) { return { vnegq_f32(a.m_v) }; }
inline F4 vmin(F4 a, F4 b) { return { vminq_f32(a.m_v, b.m_v) }; }
inline F4 vmax(F4 a, F4 b) { return { vmaxq_f32(a.m_v, b.m_v) }; }
inline F4 mad(F4 a, F4 b, F4 c) { return { vmlaq_f32(c.m_v, a.m_v, b.m_v) }; }
inline F4 splat(F4*, float x) { return { vdupq_n_f32(x) }; }
inline F4 pack(float l, float r) { const float t[4] = { l, r, 0.0f, 0.0f }; return { vld1q_f32(t) }; }
inline void unpack(F4 x, float& l, float& r) { float t[4]; vst1q_f32(t, x.m_v); l = t[0]; r = t[1]; }
#elif (defined(_M_X64) || defined(__SSE2__)) && !defined(AMBIENT_SCALAR_LANES)
constexpr const char* LanePath = "sse";    ///< the path F4 was compiled with
struct F4 { __m128 m_v; };
inline F4 operator+(F4 a, F4 b) { return { _mm_add_ps(a.m_v, b.m_v) }; }   ///< lane by lane
inline F4 operator-(F4 a, F4 b) { return { _mm_sub_ps(a.m_v, b.m_v) }; }   ///< lane by lane
inline F4 operator*(F4 a, F4 b) { return { _mm_mul_ps(a.m_v, b.m_v) }; }   ///< lane by lane
inline F4 operator/(F4 a, F4 b) { return { _mm_div_ps(a.m_v, b.m_v) }; }   ///< lane by lane
inline F4 operator-(F4 a) { return { _mm_sub_ps(_mm_setzero_ps(), a.m_v) }; }   ///< negation
inline F4 vmin(F4 a, F4 b) { return { _mm_min_ps(a.m_v, b.m_v) }; }   ///< lane by lane
inline F4 vmax(F4 a, F4 b) { return { _mm_max_ps(a.m_v, b.m_v) }; }   ///< lane by lane
#if AMBIENT_HAS_AVX
inline F4 mad(F4 a, F4 b, F4 c) { return { _mm_fmadd_ps(a.m_v, b.m_v, c.m_v) }; }   ///< a b + c, fused
#else
inline F4 mad(F4 a, F4 b, F4 c) { return { _mm_add_ps(_mm_mul_ps(a.v, b.v), c.v) }; }   ///< a b + c
#endif
inline F4 splat(F4*, float x) { return { _mm_set1_ps(x) }; }   ///< one value in every lane (tagged by the pointer's type)
inline F4 pack(float l, float r) { return { _mm_setr_ps(l, r, 0.0f, 0.0f) }; }   ///< two samples into lanes 0 and 1
/** @brief Lanes 0 and 1 out again. @param[in] x the lanes @param[out] l receives lane 0 @param[out] r receives lane 1 */
inline void unpack(F4 x, float& l, float& r) { l = _mm_cvtss_f32(x.m_v); r = _mm_cvtss_f32(_mm_shuffle_ps(x.m_v, x.m_v, 1)); }
#else
constexpr const char* LanePath = "scalar"; ///< the path F4 was compiled with
/** @brief Four lanes as four plain floats (no vector unit, or AMBIENT_SCALAR_LANES). */
struct F4 {
    float m_v[4];   ///< the lanes: left, right, two unused
};
/**
 * lane by lane
 * @param[in] a  the first lanes
 * @param[in] b  the second lanes
 * @return the sums
 */
inline F4 operator+(F4 a, F4 b) { F4 o; for (int i = 0; i < 4; ++i) o.m_v[i] = a.m_v[i] + b.m_v[i]; return o; }
/**
 * lane by lane
 * @param[in] a  the first lanes
 * @param[in] b  the second lanes
 * @return the differences
 */
inline F4 operator-(F4 a, F4 b) { F4 o; for (int i = 0; i < 4; ++i) o.m_v[i] = a.m_v[i] - b.m_v[i]; return o; }
/**
 * lane by lane
 * @param[in] a  the first lanes
 * @param[in] b  the second lanes
 * @return the products
 */
inline F4 operator*(F4 a, F4 b) { F4 o; for (int i = 0; i < 4; ++i) o.m_v[i] = a.m_v[i] * b.m_v[i]; return o; }
/**
 * lane by lane
 * @param[in] a  the dividends
 * @param[in] b  the divisors
 * @return the quotients
 */
inline F4 operator/(F4 a, F4 b) { F4 o; for (int i = 0; i < 4; ++i) o.m_v[i] = a.m_v[i] / b.m_v[i]; return o; }
/**
 * negation
 * @param[in] a  the lanes
 * @return their negation
 */
inline F4 operator-(F4 a) { F4 o; for (int i = 0; i < 4; ++i) o.m_v[i] = -a.m_v[i]; return o; }
/**
 * lane by lane
 * @param[in] a  the first lanes
 * @param[in] b  the second lanes
 * @return the smaller of each pair
 */
inline F4 vmin(F4 a, F4 b) { F4 o; for (int i = 0; i < 4; ++i) o.m_v[i] = std::min(a.m_v[i], b.m_v[i]); return o; }
/**
 * lane by lane
 * @param[in] a  the first lanes
 * @param[in] b  the second lanes
 * @return the larger of each pair
 */
inline F4 vmax(F4 a, F4 b) { F4 o; for (int i = 0; i < 4; ++i) o.m_v[i] = std::max(a.m_v[i], b.m_v[i]); return o; }
/**
 * a b + c
 * @param[in] a  the first factors
 * @param[in] b  the second factors
 * @param[in] c  the addends
 * @return a b + c per lane
 */
inline F4 mad(F4 a, F4 b, F4 c) { F4 o; for (int i = 0; i < 4; ++i) o.m_v[i] = a.m_v[i] * b.m_v[i] + c.m_v[i]; return o; }
/**
 * one value in every lane (tagged by the pointer's type)
 * @param[in] x  the value
 * @return the value in every lane
 */
inline F4 splat(F4*, float x) { return { { x, x, x, x } }; }
/**
 * two samples into lanes 0 and 1
 * @param[in] l  the left sample
 * @param[in] r  the right sample
 * @return the lanes
 */
inline F4 pack(float l, float r) { return { { l, r, 0.0f, 0.0f } }; }
/**
 * @brief Lanes 0 and 1 out again.
 * @param[in]  x  the lanes
 * @param[out] l  receives lane 0
 * @param[out] r  receives lane 1
 */
inline void unpack(F4 x, float& l, float& r) { l = x.m_v[0]; r = x.m_v[1]; }
#endif

// The same few operations on a plain float, so every model also compiles for one lane.
/**
 * min
 * @param[in] a  the first value
 * @param[in] b  the second value
 * @return the smaller
 */
inline float vmin(float a, float b) { return std::min(a, b); }
/**
 * max
 * @param[in] a  the first value
 * @param[in] b  the second value
 * @return the larger
 */
inline float vmax(float a, float b) { return std::max(a, b); }
/**
 * a b + c
 * @param[in] a  the first factor
 * @param[in] b  the second factor
 * @param[in] c  the addend
 * @return a b + c
 */
inline float mad(float a, float b, float c) { return a * b + c; }
/**
 * @brief The value itself: one lane (tagged by the pointer's type).
 * @param[in] x  the value
 * @return the value
 */
inline float splat(float*, float x) { return x; }
/**
 * @brief A constant in every lane of V.
 * @param[in] x  the constant
 * @return it, broadcast
 * @tparam V float or F4
 */
template <class VT> inline VT lanes(float x) { return splat(static_cast<VT*>(nullptr), x); }

/**
 * @brief tanh by Lambert's continued fraction (7th order), exact to 1e-7 inside +-4.97 and held there.
 * @param[in] x  the argument
 * @return tanh(x)
 */
template <class VT>
inline VT ftanh(VT x)
{
    x = vmax(vmin(x, lanes<VT>(4.97f)), lanes<VT>(-4.97f));
    const VT x2 = x * x;
    const VT num = x * mad(x2, mad(x2, x2 + lanes<VT>(378.0f), lanes<VT>(17325.0f)), lanes<VT>(135135.0f));
    const VT den = mad(x2, mad(x2, mad(x2, lanes<VT>(28.0f), lanes<VT>(3150.0f)), lanes<VT>(62370.0f)), lanes<VT>(135135.0f));
    return num / den;
}

/**
 * @brief The Moog transistor ladder (Huovilainen 2004; solved implicitly as D'Angelo and Välimäki 2014):
 *        v0' = w (tanh(x - k v3) - tanh v0), vi' = w (tanh v(i-1) - tanh vi).
 * @param[in,out] v  the four stage voltages (in: the last sample's, the Newton start; out: this sample's)
 * @param[in,out] s  the four trapezoidal states
 * @param[in]     x  the input
 * @param[in]     g  tan(pi fc / fs)
 * @param[in]     k  the resonance feedback (4: self-oscillation)
 * @return the fourth stage
 */
template <class VT>
inline VT ladderMoog(VT* v, VT* s, VT x, VT g, VT k)
{
    const VT one = lanes<VT>(1.0f);
    for (int it = 0; it < Newton; ++it) {
        const VT tx = ftanh(x - k * v[3]);
        const VT t0 = ftanh(v[0]), t1 = ftanh(v[1]), t2 = ftanh(v[2]), t3 = ftanh(v[3]);
        const VT F0 = v[0] - s[0] - g * (tx - t0), F1 = v[1] - s[1] - g * (t0 - t1);
        const VT F2 = v[2] - s[2] - g * (t1 - t2), F3 = v[3] - s[3] - g * (t2 - t3);
        const VT d0 = one - t0 * t0, d1 = one - t1 * t1, d2 = one - t2 * t2, d3 = one - t3 * t3, dx = one - tx * tx;
        const VT i1 = one / mad(g, d1, one), i2 = one / mad(g, d2, one), i3 = one / mad(g, d3, one);
        const VT a1 = -F1 * i1, b1 = g * d0 * i1;
        const VT a2 = mad(g * d1, a1, -F2) * i2, b2 = g * d1 * b1 * i2;
        const VT a3 = mad(g * d2, a2, -F3) * i3, b3 = g * d2 * b2 * i3;
        const VT J00 = mad(g, d0, one), J03 = g * dx * k;
        const VT D0 = (-F0 - J03 * a3) / mad(J03, b3, J00);
        v[0] = v[0] + D0;
        v[1] = v[1] + mad(b1, D0, a1);
        v[2] = v[2] + mad(b2, D0, a2);
        v[3] = v[3] + mad(b3, D0, a3);
    }
    for (int i = 0; i < 4; ++i) s[i] = v[i] + v[i] - s[i];
    return v[3];
}

/**
 * @brief An OTA cascade (the Prophet's SSM2040 / CEM3320, the Juno's IR3109): four buffered OTA stages,
 *        vi' = w tanh(d (ai - vi)) / d with a0 = x - k tanh(r v3) / r and ai = v(i-1).
 * @param[in,out] v  the four stage voltages (the Newton start in, this sample's out)
 * @param[in,out] s  the four trapezoidal states
 * @param[in]     x  the input
 * @param[in]     g  tan(pi fc / fs)
 * @param[in]     k  the resonance feedback (4: self-oscillation)
 * @param[in]     d  the stages' drive, their saturation
 * @param[in]     r  the resonance VCA's saturation
 * @return the fourth stage
 */
template <class VT>
inline VT otaCascade(VT* v, VT* s, VT x, VT g, VT k, float d, float r)
{
    const VT one = lanes<VT>(1.0f), D = lanes<VT>(d), Di = lanes<VT>(1.0f / d), R = lanes<VT>(r), Ri = lanes<VT>(1.0f / r);
    for (int it = 0; it < Newton; ++it) {
        const VT tr = ftanh(R * v[3]);
        const VT a0 = x - k * tr * Ri;
        const VT e0 = ftanh(D * (a0 - v[0])), e1 = ftanh(D * (v[0] - v[1])), e2 = ftanh(D * (v[1] - v[2])), e3 = ftanh(D * (v[2] - v[3]));
        const VT F0 = v[0] - s[0] - g * e0 * Di, F1 = v[1] - s[1] - g * e1 * Di;
        const VT F2 = v[2] - s[2] - g * e2 * Di, F3 = v[3] - s[3] - g * e3 * Di;
        const VT q0 = g * (one - e0 * e0), q1 = g * (one - e1 * e1), q2 = g * (one - e2 * e2), q3 = g * (one - e3 * e3);
        const VT i1 = one / (one + q1), i2 = one / (one + q2), i3 = one / (one + q3);
        const VT a1 = -F1 * i1, b1 = q1 * i1;
        const VT a2 = mad(q2, a1, -F2) * i2, b2 = q2 * b1 * i2;
        const VT a3 = mad(q3, a2, -F3) * i3, b3 = q3 * b2 * i3;
        const VT J00 = one + q0, J03 = q0 * k * (one - tr * tr);
        const VT D0 = (-F0 - J03 * a3) / mad(J03, b3, J00);
        v[0] = v[0] + D0;
        v[1] = v[1] + mad(b1, D0, a1);
        v[2] = v[2] + mad(b2, D0, a2);
        v[3] = v[3] + mad(b3, D0, a3);
    }
    for (int i = 0; i < 4; ++i) s[i] = v[i] + v[i] - s[i];
    return v[3];
}

/**
 * @brief The Oberheim SEM: a state-variable filter whose OTA integrators saturate, morphing from low
 *        pass through notch to high pass.
 *
 * v0 = band pass, v1 = low pass, h = x - 2 R v0 - v1, v0' = w N(h), v1' = w N(v0) with
 * N(e) = L tanh(e / L), L = 1.5. The damping grows with the band pass's swing, as the circuit's
 * resonance path saturates (h takes - (v0 - 1.5 tanh(v0 / 1.5)) more): nothing changes in the small-
 * signal range, and a loud input at full resonance stays within a few times itself.
 * @param[in,out] v      band pass and low pass (the Newton start in, this sample's out)
 * @param[in,out] s      their trapezoidal states
 * @param[in]     x      the input
 * @param[in]     g      tan(pi fc / fs)
 * @param[in]     R      the damping, Q = 1 / 2R
 * @param[in]     morph  0 low pass, 0.5 notch (low pass plus high pass), 1 high pass; the same in every lane
 * @return the morphed output
 */
template <class VT>
inline VT svfSem(VT* v, VT* s, VT x, VT g, VT R, float morph)
{
    const VT one = lanes<VT>(1.0f), two = lanes<VT>(2.0f), L = lanes<VT>(1.5f), Li = lanes<VT>(1.0f / 1.5f);
    const VT R2 = two * R;
    auto damp = [&](VT b) { return mad(R2, b, b) - L * ftanh(b * Li); };
    for (int it = 0; it < Newton; ++it) {
        const VT tq = ftanh(v[0] * Li);
        const VT h = x - (mad(R2, v[0], v[0]) - L * tq) - v[1];
        const VT th = ftanh(h * Li);
        const VT tb = tq;                                   // N(v0) and the damping's saturation are the same curve
        const VT F0 = v[0] - s[0] - g * L * th, F1 = v[1] - s[1] - g * L * tb;
        const VT nh = g * (one - th * th), nb = g * (one - tb * tb);
        const VT dh = mad(tq, tq, R2);                      // -dh/dv0: 2R + tanh^2(v0 / 1.5)
        const VT det = mad(nh, dh + nb, one);               // 1 + g N'(h) (2R + ...) + g^2 N'(h) N'(v0)
        const VT D0 = mad(F1, nh, -F0) / det;
        v[0] = v[0] + D0;
        v[1] = v[1] + mad(nb, D0, -F1);
    }
    s[0] = v[0] + v[0] - s[0];
    s[1] = v[1] + v[1] - s[1];
    const VT hp = x - damp(v[0]) - v[1];
    const float m2 = morph + morph;
    return morph < 0.5f ? mad(lanes<VT>(m2), hp, v[1]) : mad(lanes<VT>(2.0f - m2), v[1], hp);
}

/**
 * @brief The diode ladder (TB-303, EMS; Stinchcombe): four capacitor nodes coupled through diode pairs,
 *        the last one of half the capacitance, u = x - k v3:
 *        v0' = w (T(u - v0) - T(v0 - v1)), v1' = w (T(v0 - v1) - T(v1 - v2)),
 *        v2' = w (T(v1 - v2) - T(v2 - v3)), v3' = 2 w T(v2 - v3).
 *        Linear, it oscillates at k = 17 and sqrt 2 times w: the caller passes g / sqrt 2.
 * @param[in,out] v  the four node voltages (the Newton start in, this sample's out)
 * @param[in,out] s  the four trapezoidal states
 * @param[in]     x  the input
 * @param[in]     g  tan(pi fc / fs) / sqrt 2
 * @param[in]     k  the feedback (17: self-oscillation)
 * @return the last node
 */
template <class VT>
inline VT diodeLadder(VT* v, VT* s, VT x, VT g, VT k)
{
    const VT one = lanes<VT>(1.0f), g2 = g + g;
    for (int it = 0; it < Newton; ++it) {
        const VT u = x - k * v[3];
        const VT ti = ftanh(u - v[0]), t01 = ftanh(v[0] - v[1]), t12 = ftanh(v[1] - v[2]), t23 = ftanh(v[2] - v[3]);
        const VT r0 = -(v[0] - s[0] - g * (ti - t01)), r1 = -(v[1] - s[1] - g * (t01 - t12));
        const VT r2 = -(v[2] - s[2] - g * (t12 - t23)), r3 = -(v[3] - s[3] - g2 * t23);
        const VT di = one - ti * ti, d01 = one - t01 * t01, d12 = one - t12 * t12, d23 = one - t23 * t23;
        // The tridiagonal part.
        const VT T00 = mad(g, di + d01, one), T01 = -g * d01;
        const VT T10 = T01, T11 = mad(g, d01 + d12, one), T12 = -g * d12;
        const VT T21 = T12, T22 = mad(g, d12 + d23, one), T23 = -g * d23;
        const VT T32 = -g2 * d23, T33 = mad(g2, d23, one);
        const VT J03 = g * di * k;   // the corner: the feedback into the first node
        // Thomas, for the residual and for the unit vector e0 at once.
        const VT z0 = one / T00;
        const VT c0 = T01 * z0, x0 = r0 * z0;
        const VT n1 = one / (T11 - T10 * c0);
        const VT c1 = T12 * n1, x1 = (r1 - T10 * x0) * n1, z1 = -(T10 * z0) * n1;
        const VT n2 = one / (T22 - T21 * c1);
        const VT c2 = T23 * n2, x2 = (r2 - T21 * x1) * n2, z2 = -(T21 * z1) * n2;
        const VT n3 = one / (T33 - T32 * c2);
        const VT X3 = (r3 - T32 * x2) * n3, Z3 = -(T32 * z2) * n3;
        const VT X2 = x2 - c2 * X3, Z2 = z2 - c2 * Z3;
        const VT X1 = x1 - c1 * X2, Z1 = z1 - c1 * Z2;
        const VT X0 = x0 - c0 * X1, Z0 = z0 - c0 * Z1;
        // Sherman-Morrison: J = T + J03 e0 e3^T.
        const VT f = J03 * X3 / mad(J03, Z3, one);
        v[0] = v[0] + (X0 - f * Z0);
        v[1] = v[1] + (X1 - f * Z1);
        v[2] = v[2] + (X2 - f * Z2);
        v[3] = v[3] + (X3 - f * Z3);
    }
    for (int i = 0; i < 4; ++i) s[i] = v[i] + v[i] - s[i];
    return v[3];
}

} // namespace circuit
} // namespace ambient
