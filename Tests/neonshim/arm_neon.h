/**
 * @file arm_neon.h
 * @brief NOT the real \<arm_neon.h\>.
 *
 * The few AArch64 NEON intrinsics the Room's convolver and the grain
 * loop use, written out lane by lane after their ACLE definitions, so that an x86 test build
 * (AMBIENT_NEON_SHIM, see Tests/CMakeLists.txt) runs the NEON paths instead of skipping them.
 *
 * The arithmetic is the same per lane as on the hardware: add, subtract and multiply are single
 * IEEE operations, and the fused forms use std::fma, which is what vfmaq_f32 and vfmsq_f32 compute
 * on AArch64 (a + b c and a - b c as one fused multiply-add). What this cannot check is whether
 * the real header spells something differently -- that is what the NDK build is for.
 */
#pragma once
#include <cmath>

typedef float float32_t;   ///< the ACLE's name for one 32-bit float lane

/** @brief A NEON Q register of four float lanes, as a plain array: lane 0 is the one a load reads first. */
struct float32x4_t { float32_t lane[4]; };
/** @var float32x4_t::lane
 *  the four lanes in memory order, lane[0] at the lowest address
 */

/**
 * @brief Loads four consecutive floats into the four lanes.
 * @param p  the first of four floats to read; no alignment is required, as on the hardware
 * @return   lane i = p[i]
 */
inline float32x4_t vld1q_f32(const float32_t* p)
{ float32x4_t r; for (int i = 0; i < 4; ++i) r.lane[i] = p[i]; return r; }

/**
 * @brief Stores the four lanes to four consecutive floats.
 * @param p  where the first lane goes; p[0 .. 3] are written
 * @param a  the register to store
 */
inline void vst1q_f32(float32_t* p, float32x4_t a)
{ for (int i = 0; i < 4; ++i) p[i] = a.lane[i]; }

/**
 * @brief Broadcasts one value into every lane.
 * @param x  the value
 * @return   a register with x in all four lanes
 */
inline float32x4_t vdupq_n_f32(float32_t x)
{ float32x4_t r; for (int i = 0; i < 4; ++i) r.lane[i] = x; return r; }

/**
 * @brief Lane-wise a + b, one IEEE addition per lane.
 * @param a  the first operand
 * @param b  the second operand
 * @return   a + b per lane
 */
inline float32x4_t vaddq_f32(float32x4_t a, float32x4_t b)
{ for (int i = 0; i < 4; ++i) a.lane[i] += b.lane[i]; return a; }

/**
 * @brief Lane-wise a - b, one IEEE subtraction per lane.
 * @param a  the minuend
 * @param b  the subtrahend
 * @return   a - b per lane
 */
inline float32x4_t vsubq_f32(float32x4_t a, float32x4_t b)
{ for (int i = 0; i < 4; ++i) a.lane[i] -= b.lane[i]; return a; }

/**
 * @brief Lane-wise a * b, one IEEE multiplication per lane.
 * @param a  the first factor
 * @param b  the second factor
 * @return   a * b per lane
 */
inline float32x4_t vmulq_f32(float32x4_t a, float32x4_t b)
{ for (int i = 0; i < 4; ++i) a.lane[i] *= b.lane[i]; return a; }

/**
 * @brief a + b c, fused
 *
 * One rounding per lane, through std::fma, as the AArch64 instruction rounds once.
 * @param a  the addend
 * @param b  the first factor
 * @param c  the second factor
 * @return   fma(b, c, a) per lane
 */
inline float32x4_t vfmaq_f32(float32x4_t a, float32x4_t b, float32x4_t c)
{ for (int i = 0; i < 4; ++i) a.lane[i] = std::fma(b.lane[i], c.lane[i], a.lane[i]); return a; }

/**
 * @brief a - b c, fused
 *
 * One rounding per lane, through std::fma with the first factor negated, as the AArch64 instruction does.
 * @param a  the minuend
 * @param b  the first factor
 * @param c  the second factor
 * @return   fma(-b, c, a) per lane
 */
inline float32x4_t vfmsq_f32(float32x4_t a, float32x4_t b, float32x4_t c)
{ for (int i = 0; i < 4; ++i) a.lane[i] = std::fma(-b.lane[i], c.lane[i], a.lane[i]); return a; }

/**
 * @brief a + b c and a - b c, not fused (32-bit ARM without FMA)
 *
 * This is the a + b c half, vmlsq_f32 the other: the product is rounded, then the sum -- two
 * roundings per lane, which is what the convtest's neon-shim-nofma variant runs against.
 * @param a  the addend
 * @param b  the first factor
 * @param c  the second factor
 * @return   a + (b * c) per lane, rounded twice
 */
inline float32x4_t vmlaq_f32(float32x4_t a, float32x4_t b, float32x4_t c)
{ for (int i = 0; i < 4; ++i) a.lane[i] += b.lane[i] * c.lane[i]; return a; }

/**
 * @brief a - b c, not fused: the other half of vmlaq_f32 (32-bit ARM without FMA).
 * @param a  the minuend
 * @param b  the first factor
 * @param c  the second factor
 * @return   a - (b * c) per lane, rounded twice
 */
inline float32x4_t vmlsq_f32(float32x4_t a, float32x4_t b, float32x4_t c)
{ for (int i = 0; i < 4; ++i) a.lane[i] -= b.lane[i] * c.lane[i]; return a; }

/**
 * @brief Lane-wise minimum and maximum, for the clamp in the FM bank (Simd.h).
 *
 * AArch64's vminq/vmaxq
 * return the second operand when either is a NaN; nothing here feeds them one, and std::fmin
 * would differ exactly there, so they are written as the plain comparison the hardware does.
 * This is the minimum; vmaxq_f32 is the maximum.
 * @param a  the first operand
 * @param b  the second operand
 * @return   per lane a if a < b, else b
 */
inline float32x4_t vminq_f32(float32x4_t a, float32x4_t b)
{ for (int i = 0; i < 4; ++i) a.lane[i] = a.lane[i] < b.lane[i] ? a.lane[i] : b.lane[i]; return a; }

/**
 * @brief Lane-wise maximum, the other half of vminq_f32: the plain comparison the hardware does.
 * @param a  the first operand
 * @param b  the second operand
 * @return   per lane a if a > b, else b
 */
inline float32x4_t vmaxq_f32(float32x4_t a, float32x4_t b)
{ for (int i = 0; i < 4; ++i) a.lane[i] = a.lane[i] > b.lane[i] ? a.lane[i] : b.lane[i]; return a; }
