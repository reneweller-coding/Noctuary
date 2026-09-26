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
 */
#pragma once
#include "Simd.h"
#include <algorithm>
#include <cmath>
#if !AMBIENT_HAS_AVX && !AMBIENT_HAS_NEON && (defined(_M_X64) || defined(__SSE2__))
  #include <emmintrin.h>
#endif

namespace ambient {
namespace circuit {

#ifndef AMBIENT_CIRCUIT_NEWTON
#define AMBIENT_CIRCUIT_NEWTON 3
#endif
constexpr int kNewton = AMBIENT_CIRCUIT_NEWTON;   ///< Newton steps per sample: fixed, so the sound does not depend on convergence tests

/**
 * @brief Four lanes of floats for the circuits: the left channel in lane 0, the right in lane 1.
 *
 * SSE on x86-64 (AVX2 builds use its FMA), NEON on arm64, four floats anywhere else. Only what the
 * circuits need: the four operations, min, max, a fused multiply-add, and moving two samples in
 * and out.
 */
#if AMBIENT_HAS_AVX || defined(_M_X64) || defined(__SSE2__)
struct F4 { __m128 v; };
inline F4 operator+(F4 a, F4 b) { return { _mm_add_ps(a.v, b.v) }; }   ///< lane by lane
inline F4 operator-(F4 a, F4 b) { return { _mm_sub_ps(a.v, b.v) }; }   ///< lane by lane
inline F4 operator*(F4 a, F4 b) { return { _mm_mul_ps(a.v, b.v) }; }   ///< lane by lane
inline F4 operator/(F4 a, F4 b) { return { _mm_div_ps(a.v, b.v) }; }   ///< lane by lane
inline F4 operator-(F4 a) { return { _mm_sub_ps(_mm_setzero_ps(), a.v) }; }   ///< negation
inline F4 vmin(F4 a, F4 b) { return { _mm_min_ps(a.v, b.v) }; }   ///< lane by lane
inline F4 vmax(F4 a, F4 b) { return { _mm_max_ps(a.v, b.v) }; }   ///< lane by lane
#if AMBIENT_HAS_AVX
inline F4 mad(F4 a, F4 b, F4 c) { return { _mm_fmadd_ps(a.v, b.v, c.v) }; }   ///< a b + c, fused
#else
inline F4 mad(F4 a, F4 b, F4 c) { return { _mm_add_ps(_mm_mul_ps(a.v, b.v), c.v) }; }   ///< a b + c
#endif
inline F4 splat(F4*, float x) { return { _mm_set1_ps(x) }; }   ///< one value in every lane (tagged by the pointer's type)
inline F4 pack(float l, float r) { return { _mm_setr_ps(l, r, 0.0f, 0.0f) }; }   ///< two samples into lanes 0 and 1
/** @brief Lanes 0 and 1 out again. @param x the lanes @param l receives lane 0 @param r receives lane 1 */
inline void unpack(F4 x, float& l, float& r) { l = _mm_cvtss_f32(x.v); r = _mm_cvtss_f32(_mm_shuffle_ps(x.v, x.v, 1)); }
#elif AMBIENT_HAS_NEON
struct F4 { float32x4_t v; };
inline F4 operator+(F4 a, F4 b) { return { vaddq_f32(a.v, b.v) }; }
inline F4 operator-(F4 a, F4 b) { return { vsubq_f32(a.v, b.v) }; }
inline F4 operator*(F4 a, F4 b) { return { vmulq_f32(a.v, b.v) }; }
inline F4 operator/(F4 a, F4 b) { return { vdivq_f32(a.v, b.v) }; }
inline F4 operator-(F4 a) { return { vnegq_f32(a.v) }; }
inline F4 vmin(F4 a, F4 b) { return { vminq_f32(a.v, b.v) }; }
inline F4 vmax(F4 a, F4 b) { return { vmaxq_f32(a.v, b.v) }; }
inline F4 mad(F4 a, F4 b, F4 c) { return { vmlaq_f32(c.v, a.v, b.v) }; }
inline F4 splat(F4*, float x) { return { vdupq_n_f32(x) }; }
inline F4 pack(float l, float r) { const float t[4] = { l, r, 0.0f, 0.0f }; return { vld1q_f32(t) }; }
inline void unpack(F4 x, float& l, float& r) { float t[4]; vst1q_f32(t, x.v); l = t[0]; r = t[1]; }
#else
struct F4 { float v[4]; };
inline F4 operator+(F4 a, F4 b) { F4 o; for (int i = 0; i < 4; ++i) o.v[i] = a.v[i] + b.v[i]; return o; }
inline F4 operator-(F4 a, F4 b) { F4 o; for (int i = 0; i < 4; ++i) o.v[i] = a.v[i] - b.v[i]; return o; }
inline F4 operator*(F4 a, F4 b) { F4 o; for (int i = 0; i < 4; ++i) o.v[i] = a.v[i] * b.v[i]; return o; }
inline F4 operator/(F4 a, F4 b) { F4 o; for (int i = 0; i < 4; ++i) o.v[i] = a.v[i] / b.v[i]; return o; }
inline F4 operator-(F4 a) { F4 o; for (int i = 0; i < 4; ++i) o.v[i] = -a.v[i]; return o; }
inline F4 vmin(F4 a, F4 b) { F4 o; for (int i = 0; i < 4; ++i) o.v[i] = std::min(a.v[i], b.v[i]); return o; }
inline F4 vmax(F4 a, F4 b) { F4 o; for (int i = 0; i < 4; ++i) o.v[i] = std::max(a.v[i], b.v[i]); return o; }
inline F4 mad(F4 a, F4 b, F4 c) { F4 o; for (int i = 0; i < 4; ++i) o.v[i] = a.v[i] * b.v[i] + c.v[i]; return o; }
inline F4 splat(F4*, float x) { return { { x, x, x, x } }; }
inline F4 pack(float l, float r) { return { { l, r, 0.0f, 0.0f } }; }
inline void unpack(F4 x, float& l, float& r) { l = x.v[0]; r = x.v[1]; }
#endif

// The same few operations on a plain float, so every model also compiles for one lane.
inline float vmin(float a, float b) { return std::min(a, b); }         ///< min
inline float vmax(float a, float b) { return std::max(a, b); }         ///< max
inline float mad(float a, float b, float c) { return a * b + c; }      ///< a b + c
inline float splat(float*, float x) { return x; }                      ///< the value itself
/** @brief A constant in every lane of V. @tparam V float or F4 @param x the constant @return it, broadcast */
template <class V> inline V lanes(float x) { return splat(static_cast<V*>(nullptr), x); }

/**
 * @brief tanh by Lambert's continued fraction (7th order), exact to 1e-7 inside +-4.97 and held there.
 * @param x  the argument
 * @return   tanh(x)
 */
template <class V>
inline V ftanh(V x)
{
    x = vmax(vmin(x, lanes<V>(4.97f)), lanes<V>(-4.97f));
    const V x2 = x * x;
    const V num = x * mad(x2, mad(x2, x2 + lanes<V>(378.0f), lanes<V>(17325.0f)), lanes<V>(135135.0f));
    const V den = mad(x2, mad(x2, mad(x2, lanes<V>(28.0f), lanes<V>(3150.0f)), lanes<V>(62370.0f)), lanes<V>(135135.0f));
    return num / den;
}

/**
 * @brief The Moog transistor ladder (Huovilainen 2004; solved implicitly as D'Angelo and Välimäki 2014):
 *        v0' = w (tanh(x - k v3) - tanh v0), vi' = w (tanh v(i-1) - tanh vi).
 * @param v  the four stage voltages (in: the last sample's, the Newton start; out: this sample's)
 * @param s  the four trapezoidal states
 * @param x  the input
 * @param g  tan(pi fc / fs)
 * @param k  the resonance feedback (4: self-oscillation)
 * @return   the fourth stage
 */
template <class V>
inline V ladderMoog(V* v, V* s, V x, V g, V k)
{
    const V one = lanes<V>(1.0f);
    for (int it = 0; it < kNewton; ++it) {
        const V tx = ftanh(x - k * v[3]);
        const V t0 = ftanh(v[0]), t1 = ftanh(v[1]), t2 = ftanh(v[2]), t3 = ftanh(v[3]);
        const V F0 = v[0] - s[0] - g * (tx - t0), F1 = v[1] - s[1] - g * (t0 - t1);
        const V F2 = v[2] - s[2] - g * (t1 - t2), F3 = v[3] - s[3] - g * (t2 - t3);
        const V d0 = one - t0 * t0, d1 = one - t1 * t1, d2 = one - t2 * t2, d3 = one - t3 * t3, dx = one - tx * tx;
        const V i1 = one / mad(g, d1, one), i2 = one / mad(g, d2, one), i3 = one / mad(g, d3, one);
        const V a1 = -F1 * i1, b1 = g * d0 * i1;
        const V a2 = mad(g * d1, a1, -F2) * i2, b2 = g * d1 * b1 * i2;
        const V a3 = mad(g * d2, a2, -F3) * i3, b3 = g * d2 * b2 * i3;
        const V J00 = mad(g, d0, one), J03 = g * dx * k;
        const V D0 = (-F0 - J03 * a3) / mad(J03, b3, J00);
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
 * @param v  the four stage voltages (the Newton start in, this sample's out)
 * @param s  the four trapezoidal states
 * @param x  the input
 * @param g  tan(pi fc / fs)
 * @param k  the resonance feedback (4: self-oscillation)
 * @param d  the stages' drive, their saturation
 * @param r  the resonance VCA's saturation
 * @return   the fourth stage
 */
template <class V>
inline V otaCascade(V* v, V* s, V x, V g, V k, float d, float r)
{
    const V one = lanes<V>(1.0f), D = lanes<V>(d), Di = lanes<V>(1.0f / d), R = lanes<V>(r), Ri = lanes<V>(1.0f / r);
    for (int it = 0; it < kNewton; ++it) {
        const V tr = ftanh(R * v[3]);
        const V a0 = x - k * tr * Ri;
        const V e0 = ftanh(D * (a0 - v[0])), e1 = ftanh(D * (v[0] - v[1])), e2 = ftanh(D * (v[1] - v[2])), e3 = ftanh(D * (v[2] - v[3]));
        const V F0 = v[0] - s[0] - g * e0 * Di, F1 = v[1] - s[1] - g * e1 * Di;
        const V F2 = v[2] - s[2] - g * e2 * Di, F3 = v[3] - s[3] - g * e3 * Di;
        const V q0 = g * (one - e0 * e0), q1 = g * (one - e1 * e1), q2 = g * (one - e2 * e2), q3 = g * (one - e3 * e3);
        const V i1 = one / (one + q1), i2 = one / (one + q2), i3 = one / (one + q3);
        const V a1 = -F1 * i1, b1 = q1 * i1;
        const V a2 = mad(q2, a1, -F2) * i2, b2 = q2 * b1 * i2;
        const V a3 = mad(q3, a2, -F3) * i3, b3 = q3 * b2 * i3;
        const V J00 = one + q0, J03 = q0 * k * (one - tr * tr);
        const V D0 = (-F0 - J03 * a3) / mad(J03, b3, J00);
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
 * @param v      band pass and low pass (the Newton start in, this sample's out)
 * @param s      their trapezoidal states
 * @param x      the input
 * @param g      tan(pi fc / fs)
 * @param R      the damping, Q = 1 / 2R
 * @param morph  0 low pass, 0.5 notch (low pass plus high pass), 1 high pass; the same in every lane
 * @return       the morphed output
 */
template <class V>
inline V svfSem(V* v, V* s, V x, V g, V R, float morph)
{
    const V one = lanes<V>(1.0f), two = lanes<V>(2.0f), L = lanes<V>(1.5f), Li = lanes<V>(1.0f / 1.5f);
    const V R2 = two * R;
    auto damp = [&](V b) { return mad(R2, b, b) - L * ftanh(b * Li); };
    for (int it = 0; it < kNewton; ++it) {
        const V tq = ftanh(v[0] * Li);
        const V h = x - (mad(R2, v[0], v[0]) - L * tq) - v[1];
        const V th = ftanh(h * Li);
        const V tb = tq;                                   // N(v0) and the damping's saturation are the same curve
        const V F0 = v[0] - s[0] - g * L * th, F1 = v[1] - s[1] - g * L * tb;
        const V nh = g * (one - th * th), nb = g * (one - tb * tb);
        const V dh = mad(tq, tq, R2);                      // -dh/dv0: 2R + tanh^2(v0 / 1.5)
        const V det = mad(nh, dh + nb, one);               // 1 + g N'(h) (2R + ...) + g^2 N'(h) N'(v0)
        const V D0 = mad(F1, nh, -F0) / det;
        v[0] = v[0] + D0;
        v[1] = v[1] + mad(nb, D0, -F1);
    }
    s[0] = v[0] + v[0] - s[0];
    s[1] = v[1] + v[1] - s[1];
    const V hp = x - damp(v[0]) - v[1];
    const float m2 = morph + morph;
    return morph < 0.5f ? mad(lanes<V>(m2), hp, v[1]) : mad(lanes<V>(2.0f - m2), v[1], hp);
}

/**
 * @brief The diode ladder (TB-303, EMS; Stinchcombe): four capacitor nodes coupled through diode pairs,
 *        the last one of half the capacitance, u = x - k v3:
 *        v0' = w (T(u - v0) - T(v0 - v1)), v1' = w (T(v0 - v1) - T(v1 - v2)),
 *        v2' = w (T(v1 - v2) - T(v2 - v3)), v3' = 2 w T(v2 - v3).
 *        Linear, it oscillates at k = 17 and sqrt 2 times w: the caller passes g / sqrt 2.
 * @param v  the four node voltages (the Newton start in, this sample's out)
 * @param s  the four trapezoidal states
 * @param x  the input
 * @param g  tan(pi fc / fs) / sqrt 2
 * @param k  the feedback (17: self-oscillation)
 * @return   the last node
 */
template <class V>
inline V diodeLadder(V* v, V* s, V x, V g, V k)
{
    const V one = lanes<V>(1.0f), g2 = g + g;
    for (int it = 0; it < kNewton; ++it) {
        const V u = x - k * v[3];
        const V ti = ftanh(u - v[0]), t01 = ftanh(v[0] - v[1]), t12 = ftanh(v[1] - v[2]), t23 = ftanh(v[2] - v[3]);
        const V r0 = -(v[0] - s[0] - g * (ti - t01)), r1 = -(v[1] - s[1] - g * (t01 - t12));
        const V r2 = -(v[2] - s[2] - g * (t12 - t23)), r3 = -(v[3] - s[3] - g2 * t23);
        const V di = one - ti * ti, d01 = one - t01 * t01, d12 = one - t12 * t12, d23 = one - t23 * t23;
        // The tridiagonal part.
        const V T00 = mad(g, di + d01, one), T01 = -g * d01;
        const V T10 = T01, T11 = mad(g, d01 + d12, one), T12 = -g * d12;
        const V T21 = T12, T22 = mad(g, d12 + d23, one), T23 = -g * d23;
        const V T32 = -g2 * d23, T33 = mad(g2, d23, one);
        const V J03 = g * di * k;   // the corner: the feedback into the first node
        // Thomas, for the residual and for the unit vector e0 at once.
        const V z0 = one / T00;
        const V c0 = T01 * z0, x0 = r0 * z0;
        const V n1 = one / (T11 - T10 * c0);
        const V c1 = T12 * n1, x1 = (r1 - T10 * x0) * n1, z1 = -(T10 * z0) * n1;
        const V n2 = one / (T22 - T21 * c1);
        const V c2 = T23 * n2, x2 = (r2 - T21 * x1) * n2, z2 = -(T21 * z1) * n2;
        const V n3 = one / (T33 - T32 * c2);
        const V X3 = (r3 - T32 * x2) * n3, Z3 = -(T32 * z2) * n3;
        const V X2 = x2 - c2 * X3, Z2 = z2 - c2 * Z3;
        const V X1 = x1 - c1 * X2, Z1 = z1 - c1 * Z2;
        const V X0 = x0 - c0 * X1, Z0 = z0 - c0 * Z1;
        // Sherman-Morrison: J = T + J03 e0 e3^T.
        const V f = J03 * X3 / mad(J03, Z3, one);
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
