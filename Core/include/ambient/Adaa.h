// Noctuary -- saturation with antiderivative antialiasing.
//
// A saturation makes partials the sample rate cannot hold, and they fold back below Nyquist as tones
// in no harmonic relation to anything; in a feedback loop they fold back again on every pass.
// First-order antiderivative antialiasing (Parker, Zavalishin and Le Bihan, DAFx 2016; Bilbao,
// Esqueda, Parker and Valimaki, IEEE SPL 2017) evaluates the curve's antiderivative instead and
// differentiates it across the sample interval -- the curve convolved with a one-sample rectangle --
// which takes the aliases down by the order of twenty decibels for half a sample of delay.
#pragma once
#include <cmath>

namespace ambient {

// log(cosh(x)), the antiderivative of tanh, written so that it neither overflows nor cancels.
inline double logCosh(double x)
{
    const double a = std::fabs(x);
    return a + std::log1p(std::exp(-2.0 * a)) - 0.6931471805599453;
}

// tanh through first-order ADAA. The state is the previous input and its antiderivative; where the
// two inputs are too close for the difference quotient to be accurate, tanh of their midpoint, which
// is its limit. Half a sample late either way.
struct TanhAdaa {
    float  x1 = 0.0f;
    double f1 = 0.0;
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
    void reset() { x1 = 0.0f; f1 = 0.0; }
};

} // namespace ambient
