/**
 * @file Adaa.h
 * @brief Saturation with antiderivative antialiasing.
 *
 * A saturation makes partials the sample rate cannot hold, and they fold back below Nyquist as tones
 * in no harmonic relation to anything; in a feedback loop they fold back again on every pass.
 * First-order antiderivative antialiasing (Parker, Zavalishin and Le Bihan, DAFx 2016; Bilbao,
 * Esqueda, Parker and Valimaki, IEEE SPL 2017) evaluates the curve's antiderivative instead and
 * differentiates it across the sample interval -- the curve convolved with a one-sample rectangle --
 * which takes the aliases down by the order of twenty decibels for half a sample of delay.
 */
#pragma once
#include <cmath>

namespace ambient {

/**
 * @brief log(cosh(x)), the antiderivative of tanh, written so that it neither overflows nor cancels.
 *
 * cosh(x) overflows a double past |x| of about 710, and for large |x| the direct log(cosh(x)) would
 * also cancel to |x| - log 2 with a loss of digits. The form used here, |x| + log1p(exp(-2|x|)) - log 2,
 * is exact for every magnitude: the exponential only ever sees a non-positive argument, and log1p
 * keeps the small correction term accurate near zero.
 *
 * @param x  the input sample (any magnitude)
 * @return   log(cosh(x)), non-negative, with log(cosh(0)) = 0
 */
inline double logCosh(double x)
{
    const double a = std::fabs(x);
    return a + std::log1p(std::exp(-2.0 * a)) - 0.6931471805599453;
}

/**
 * @brief tanh through first-order ADAA.
 *
 * The state is the previous input and its antiderivative; where the
 * two inputs are too close for the difference quotient to be accurate, tanh of their midpoint, which
 * is its limit. Half a sample late either way.
 *
 * One instance is one mono channel of saturation: it carries a single sample of history, so a stereo
 * stage owns two of them. The antiderivative is kept in double because the difference quotient
 * divides two nearly equal numbers by a small step, and float would lose the result in the noise.
 */
struct TanhAdaa {
    float  x1 = 0.0f;   ///< the previous input sample
    double f1 = 0.0;    ///< logCosh(x1), the antiderivative at the previous input

    /**
     * @brief Saturates one sample.
     * @param x  the input sample
     * @return   tanh of the input, antialiased to first order and delayed by half a sample
     */
    float operator()(float x)
    {
        const double fx = logCosh(static_cast<double>(x));
        const double dx = static_cast<double>(x) - static_cast<double>(x1);
        const float y = std::fabs(dx) > 1.0e-5 ? static_cast<float>((fx - f1) / dx)
                                               : std::tanh(0.5f * (x + x1));
        x1 = x;
        f1 = fx;
        return y;
    }

    /** @brief Clears the one sample of history, as at a voice start or a sample-rate change. */
    void reset() { x1 = 0.0f; f1 = 0.0; }
};

} // namespace ambient
