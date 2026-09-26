/**
 * @file Filter.cpp
 * @brief The voice filter's coefficients and its frequency response, model by model.
 *
 * The per-sample work of VoiceFilter is inline in Filter.h (one() and tick()); this file holds what
 * runs once a block or once a display frame. set() turns Cutoff, Resonance and Drive into the
 * coefficients of whichever model is chosen and resets the state when the model changes, reset()
 * clears every model's state at once, and magnitude() evaluates |H(f)| of the analogue prototypes
 * -- the state-variable stages, the one-pole, the four-pole ladder, the comb with its damping, the
 * three formant band passes -- for the display, without touching a voice. The formant table the
 * Formant model morphs through lives here as well, since both set() and magnitude() read it: the
 * one to tune three band passes, the other to draw them.
 */
#include "ambient/Filter.h"
#include <complex>
#include <cstring>

namespace ambient {

const char* const kFilterModelNames[kNumFilterModels] = {
    "LP 6", "LP 12", "LP 24", "HP 12", "BP 12", "Notch", "Peak", "Ladder", "Comb", "Formant",
};

namespace {
/** @brief Formant frequencies of u o a e i (a rising sweep as Cutoff turns), interpolated in log frequency. */
const float kFormants[5][3] = { { 325.0f, 700.0f, 2530.0f }, { 450.0f, 800.0f, 2830.0f }, { 800.0f, 1150.0f, 2900.0f },
                                { 400.0f, 1600.0f, 2700.0f }, { 270.0f, 2300.0f, 3000.0f } };
/**
 * @brief The three formant frequencies the Formant model sits on at a Cutoff.
 *
 * Cutoff is mapped onto the vowel sweep in log frequency -- 40 Hz is u, 18 kHz is i -- and between
 * two neighbouring vowels each formant is interpolated in log frequency as well, so the sweep
 * moves evenly in pitch. Called by set() when the Formant model is chosen and by magnitude() for
 * the display.
 *
 * @param cutoffHz  the filter's Cutoff in Hz; clamped to 40 Hz .. 18 kHz for the sweep
 * @param out       receives the three formant frequencies in Hz, lowest first
 */
void formantsAt(float cutoffHz, float* out)
{
    const float t = clampv(std::log(cutoffHz / 40.0f) / std::log(18000.0f / 40.0f), 0.0f, 1.0f) * 4.0f;
    const int i = std::min(static_cast<int>(t), 3);
    const float f = t - static_cast<float>(i);
    for (int k = 0; k < 3; ++k) out[k] = std::exp(std::log(kFormants[i][k]) + f * (std::log(kFormants[i + 1][k]) - std::log(kFormants[i][k])));
}
} // namespace
const char* const kFilterRouteNames[2] = { "Series", "Parallel" };

void VoiceFilter::reset()
{
    svfL_.reset(); svfR_.reset(); svf2L_.reset(); svf2R_.reset(); svf3L_.reset(); svf3R_.reset();
    std::memset(lad_, 0, sizeof(lad_));
    onePole_[0] = onePole_[1] = 0.0f;
    std::memset(combBuf_, 0, sizeof(combBuf_));
    combW_ = 0;
    combDamp_[0] = combDamp_[1] = 0.0f;
    osL_.reset(); osR_.reset();
    driving_ = false;
}

void VoiceFilter::set(FilterModel model, float cutoffHz, float resonance, float drive)
{
    if (model != model_) {   // a model change starts from rest; the states mean different things
        model_ = model;
        reset();
    }
    const float fc = clampv(cutoffHz, 10.0f, sr_ * 0.45f);
    const float res = clampv(resonance, 0.0f, 1.0f);
    // Drive: a soft saturation ahead of the filter, level-compensated so the knob adds harmonics
    // rather than loudness. Off at zero (and then not even computed).
    const float d = clampv(drive, 0.0f, 1.0f);
    driveIn_  = d > 0.0f ? 1.0f + 6.0f * d : 1.0f;
    driveOut_ = d > 0.0f ? (1.0f + 0.5f * d) / driveIn_ : 1.0f;
    switch (model) {
    case FilterModel::Lp12:
        svfL_.set(fc, res, sr_); svfR_.copyCoefficients(svfL_);
        break;
    case FilterModel::Lp24:
        // two identical stages; the resonance is shared so the peak is not squared into a spike
        svfL_.set(fc, res * 0.75f, sr_); svfR_.copyCoefficients(svfL_);
        svf2L_.copyCoefficients(svfL_); svf2R_.copyCoefficients(svfL_);
        break;
    case FilterModel::Hp12:
    case FilterModel::Bp12:
    case FilterModel::Notch:
        svfL_.set(fc, res, sr_); svfR_.copyCoefficients(svfL_);
        break;
    case FilterModel::Peak:
        svfL_.set(fc, 0.5f + 0.5f * res, sr_); svfR_.copyCoefficients(svfL_);   // narrower bell with more resonance
        peakGain_ = 4.0f * res;                                                  // up to +14 dB
        break;
    case FilterModel::Lp6:
        g1_ = 1.0f - std::exp(-kTwoPi * fc / sr_);
        break;
    case FilterModel::Ladder: {
        // The four-pole cascade is 12 dB down at its one-pole corner, so the corner is placed
        // above the knob's value to bring the -3 dB point back near Cutoff.
        g1_ = 1.0f - std::exp(-kTwoPi * std::min(fc * 1.55f, sr_ * 0.45f) / sr_);
        kLad_ = 4.0f * res * res * 0.98f;   // squared: the interesting range is near the top
        ladComp_ = 1.0f + kLad_ * 0.5f;     // passband loss under feedback, partly restored
        break;
    }
    case FilterModel::Comb:
        combDelay_ = clampv(sr_ / fc, 2.0f, static_cast<float>(kCombMax - 4));
        combFb_ = 0.98f * res;
        break;
    case FilterModel::Formant: {
        float f[3]; formantsAt(fc, f);
        const float q = 3.0f + 12.0f * res;
        svfL_.setQ(f[0], q, sr_); svfR_.copyCoefficients(svfL_);
        svf2L_.setQ(f[1], q, sr_); svf2R_.copyCoefficients(svf2L_);
        svf3L_.setQ(f[2], q, sr_); svf3R_.copyCoefficients(svf3L_);
        break;
    }
    default: break;
    }
}

float VoiceFilter::magnitude(FilterModel model, float cutoffHz, float resonance, float hz, float sr)
{
    using C = std::complex<float>;
    const float fc = clampv(cutoffHz, 10.0f, sr * 0.45f);
    const float res = clampv(resonance, 0.0f, 1.0f);
    const float w = kTwoPi * hz / sr;
    // The state-variable stages in the analogue prototype (s normalised to the warped cutoff).
    auto svf = [&](float r) {
        const float gc = std::tan(kPi * fc / sr);
        const float k = 2.0f - 1.9f * r;
        const C s(0.0f, std::tan(0.5f * w) / gc);
        const C den = s * s + k * s + 1.0f;
        struct R { C lp, bp, hp; float k; } out{ 1.0f / den, s / den, (s * s) / den, k };
        return out;
    };
    switch (model) {
    case FilterModel::Lp12: return std::abs(svf(res).lp);
    case FilterModel::Lp24: { const C h = svf(res * 0.75f).lp; return std::abs(h * h); }
    case FilterModel::Hp12: return std::abs(svf(res).hp);
    case FilterModel::Bp12: { auto r = svf(res); return std::abs(r.bp * r.k); }
    case FilterModel::Notch: { auto r = svf(res); return std::abs(r.lp + r.hp); }
    case FilterModel::Peak: { auto r = svf(0.5f + 0.5f * res); return std::abs(1.0f + 4.0f * res * r.k * r.bp); }
    case FilterModel::Lp6: {
        const float g = 1.0f - std::exp(-kTwoPi * fc / sr);
        const C z = std::polar(1.0f, -w);
        return std::abs(g / (1.0f - (1.0f - g) * z));
    }
    case FilterModel::Ladder: {
        const float g = 1.0f - std::exp(-kTwoPi * std::min(fc * 1.55f, sr * 0.45f) / sr);
        const float k = 4.0f * res * res * 0.98f;
        const C z = std::polar(1.0f, -w);
        const C G = g / (1.0f - (1.0f - g) * z);
        const C G4 = G * G * G * G;
        return std::abs(G4 / (1.0f + k * G4)) * (1.0f + k * 0.5f);
    }
    case FilterModel::Formant: {
        float f[3]; formantsAt(fc, f);
        const float q = 3.0f + 12.0f * res;
        static const float gain[3] = { 1.0f, 0.5f, 0.3f };
        float sum = 0.0f;
        for (int i = 0; i < 3; ++i) {
            const float gc = std::tan(kPi * clampv(f[i], 10.0f, sr * 0.45f) / sr);
            const C s(0.0f, std::tan(0.5f * w) / gc);
            const float k = 1.0f / q;
            sum += std::abs(k * s / (s * s + k * s + 1.0f)) * gain[i];   // magnitudes summed: the picture without the phases
        }
        return sum;
    }
    case FilterModel::Comb: {
        const float D = clampv(sr / fc, 2.0f, static_cast<float>(kCombMax - 4));
        const float fb = 0.98f * res;
        const C z = std::polar(1.0f, -w);
        const C zD = std::polar(1.0f, -w * D);
        const C damp = 0.35f / (1.0f - 0.65f * z);
        return std::abs((1.0f - fb) / (1.0f - fb * damp * zD));
    }
    default: return 1.0f;
    }
}

} // namespace ambient
