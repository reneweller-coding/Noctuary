/**
 * @file Filter.cpp
 * @brief The voice filter's coefficients and its frequency response, model by model.
 *
 * The per-sample work of VoiceFilter is inline in Filter.h (one() and tick()); this file holds what
 * runs once a block or once a display frame. set() turns Cutoff, Resonance and Drive into the
 * coefficients of whichever model is chosen and resets the state when the model changes, reset()
 * clears every model's state at once, and magnitude() evaluates |H(f)| of the analogue prototypes
 * -- the state-variable stages, the one-pole, the four-pole ladder, the comb with its damping, the
 * three formant band passes, the circuit models linearised -- for the display, without touching a
 * voice. The formant table the Formant model morphs through lives here as well, since both set()
 * and magnitude() read it: the one to tune three band passes, the other to draw them.
 * @author Rene Weller
 */
#include "ambient/Filter.h"
#include <complex>
#include <cstring>

namespace ambient {

const char* const FilterModelNames[NumFilterModels] = {
    "LP 6", "LP 12", "LP 24", "HP 12", "BP 12", "Notch", "Peak", "Ladder", "Comb", "Formant",
    "Moog", "SEM", "Prophet", "Juno", "Diode",
};

namespace {
/** @brief Formant frequencies of u o a e i (a rising sweep as Cutoff turns), interpolated in log frequency. */
const float Formants[5][3] = { { 325.0f, 700.0f, 2530.0f }, { 450.0f, 800.0f, 2830.0f }, { 800.0f, 1150.0f, 2900.0f },
                                { 400.0f, 1600.0f, 2700.0f }, { 270.0f, 2300.0f, 3000.0f } };
/**
 * @brief The three formant frequencies the Formant model sits on at a Cutoff.
 *
 * Cutoff is mapped onto the vowel sweep in log frequency -- 40 Hz is u, 18 kHz is i -- and between
 * two neighbouring vowels each formant is interpolated in log frequency as well, so the sweep
 * moves evenly in pitch. Called by set() when the Formant model is chosen and by magnitude() for
 * the display.
 * @param[in]  cutoffHz  the filter's Cutoff in Hz; clamped to 40 Hz .. 18 kHz for the sweep
 * @param[out] out       receives the three formant frequencies in Hz, lowest first
 */
void formantsAt(float cutoffHz, float* out)
{
    const float t = clampv(std::log(cutoffHz / 40.0f) / std::log(18000.0f / 40.0f), 0.0f, 1.0f) * 4.0f;
    const int i = std::min(static_cast<int>(t), 3);
    const float f = t - static_cast<float>(i);
    for (int k = 0; k < 3; ++k) out[k] = std::exp(std::log(Formants[i][k]) + f * (std::log(Formants[i + 1][k]) - std::log(Formants[i][k])));
}

/**
 * @brief The highest one-pole coefficient the old Ladder may run at with a feedback k.
 *
 * Its loop takes the fourth stage from the last sample, and at Nyquist that sample of delay turns
 * the negative feedback positive: the loop gain there is k (g / (2 - g))^4, and past one the ladder
 * oscillates at 24 kHz at full scale. Nobody hears it; the loudness measurement of the library did,
 * and turned presets down by up to sixteen decibels for it (26.09.2026, "Envelope Hollow" among
 * them, found moving the Ladder's presets to the Moog). Held at a loop gain of 0.8 there: at the
 * highest Resonance the corner stops near 14 kHz, 9 kHz on the knob.
 * @param[in] k  the ladder's feedback
 * @return the ceiling for g1, 1 where k cannot reach the loop gain at all
 */
float ladderCeiling(float k)
{
    if (k <= 0.8f) return 1.0f;
    const float q = std::pow(0.8f / k, 0.25f);   // g / (2 - g) at the ceiling
    return 2.0f * q / (1.0f + q);
}
} // namespace
const char* const FilterRouteNames[2] = { "Series", "Parallel" };

void VoiceFilter::reset()
{
    m_svfL.reset(); m_svfR.reset(); m_svf2L.reset(); m_svf2R.reset(); m_svf3L.reset(); m_svf3R.reset();
    std::memset(m_lad, 0, sizeof(m_lad));
    m_onePole[0] = m_onePole[1] = 0.0f;
    std::memset(m_combBuf, 0, sizeof(m_combBuf));
    m_combW = 0;
    m_combDamp[0] = m_combDamp[1] = 0.0f;
    std::memset(m_cv, 0, sizeof(m_cv));
    std::memset(m_cs, 0, sizeof(m_cs));
    m_os2.reset();
    m_osL.reset(); m_osR.reset();
    m_driving = false;
}

void VoiceFilter::set(FilterModelE model, float cutoffHz, float resonance, float drive, float morph)
{
    if (model != m_model) {   // a model change starts from rest; the states mean different things
        m_model = model;
        m_circuit = isCircuitModel(model);
        reset();
    }
    const float fc = clampv(cutoffHz, 10.0f, m_sr * 0.45f);
    const float res = clampv(resonance, 0.0f, 1.0f);
    // Drive: a soft saturation ahead of the filter, level-compensated so the knob adds harmonics
    // rather than loudness. Off at zero (and then not even computed).
    const float d = clampv(drive, 0.0f, 1.0f);
    m_driveIn  = d > 0.0f ? 1.0f + 6.0f * d : 1.0f;
    m_driveOut = d > 0.0f ? (1.0f + 0.5f * d) / m_driveIn : 1.0f;
    switch (model) {
    case FilterModelE::LP12:
        m_svfL.set(fc, res, m_sr); m_svfR.copyCoefficients(m_svfL);
        break;
    case FilterModelE::LP24:
        // two identical stages; the resonance is shared so the peak is not squared into a spike
        m_svfL.set(fc, res * 0.75f, m_sr); m_svfR.copyCoefficients(m_svfL);
        m_svf2L.copyCoefficients(m_svfL); m_svf2R.copyCoefficients(m_svfL);
        break;
    case FilterModelE::HP12:
    case FilterModelE::BP12:
    case FilterModelE::NOTCH:
        m_svfL.set(fc, res, m_sr); m_svfR.copyCoefficients(m_svfL);
        break;
    case FilterModelE::PEAK:
        m_svfL.set(fc, 0.5f + 0.5f * res, m_sr); m_svfR.copyCoefficients(m_svfL);   // narrower bell with more resonance
        m_peakGain = 4.0f * res;                                                  // up to +14 dB
        break;
    case FilterModelE::LP6:
        m_g1 = 1.0f - std::exp(-TwoPi * fc / m_sr);
        break;
    case FilterModelE::LADDER: {
        // The four-pole cascade is 12 dB down at its one-pole corner, so the corner is placed
        // above the knob's value to bring the -3 dB point back near Cutoff.
        m_g1 = 1.0f - std::exp(-TwoPi * std::min(fc * 1.55f, m_sr * 0.45f) / m_sr);
        m_kLad = 4.0f * res * res * 0.98f;   // squared: the interesting range is near the top
        m_ladComp = 1.0f + m_kLad * 0.5f;     // passband loss under feedback, partly restored
        m_g1 = std::min(m_g1, ladderCeiling(m_kLad));
        break;
    }
    case FilterModelE::COMB:
        m_combDelay = clampv(m_sr / fc, 2.0f, static_cast<float>(CombMax - 4));
        m_combFb = 0.98f * res;
        break;
    case FilterModelE::FORMANT: {
        float f[3]; formantsAt(fc, f);
        const float q = 3.0f + 12.0f * res;
        m_svfL.setQ(f[0], q, m_sr); m_svfR.copyCoefficients(m_svfL);
        m_svf2L.setQ(f[1], q, m_sr); m_svf2R.copyCoefficients(m_svf2L);
        m_svf3L.setQ(f[2], q, m_sr); m_svf3R.copyCoefficients(m_svf3L);
        break;
    }
    case FilterModelE::MOOG: case FilterModelE::SEM: case FilterModelE::PROPHET: case FilterModelE::JUNO: case FilterModelE::DIODE: {
        // The circuit models at twice the rate, their corner on Cutoff; Resonance as Ephemeris
        // voices it (FilterVoicing there): the ladders and cascades self-oscillate just at 1, the
        // diode ladder at a loop gain of 17, and the SEM's Q runs from 0.7 to 40. Their pass band
        // loses 1 / (1 + k) to the feedback, and a part of it is made good.
        const float g = std::tan(Pi * fc / (2.0f * m_sr));
        float makeup = 1.0f;
        m_cg = g;
        m_cMorph = clampv(morph, 0.0f, 1.0f);
        switch (model) {
        case FilterModelE::MOOG:    m_ck = 4.0f * res * 0.985f; makeup = 1.0f + 0.5f * m_ck; break;
        case FilterModelE::SEM:     m_ck = 0.707f * (1.0f - res) + 0.012f; break;
        case FilterModelE::PROPHET: m_ck = 4.1f * res; makeup = 1.0f + 0.5f * m_ck; break;
        case FilterModelE::JUNO:    m_ck = 4.1f * res; makeup = 1.0f + 0.25f * m_ck; break;
        default:                   m_ck = 17.5f * res; makeup = 1.0f + 0.3f * m_ck; m_cg = g * 0.70710678f; break;   // Diode: it rings at sqrt 2 times its w
        }
        m_cOut = makeup * m_driveOut;
        break;
    }
    default: break;
    }
}

float VoiceFilter::magnitude(FilterModelE model, float cutoffHz, float resonance, float hz, float sr, float morph)
{
    using CT = std::complex<float>;
    const float fc = clampv(cutoffHz, 10.0f, sr * 0.45f);
    const float res = clampv(resonance, 0.0f, 1.0f);
    const float w = TwoPi * hz / sr;
    // The state-variable stages in the analogue prototype (s normalised to the warped cutoff).
    auto svf = [&](float r) {
        const float gc = std::tan(Pi * fc / sr);
        const float k = 2.0f - 1.9f * r;
        const CT s(0.0f, std::tan(0.5f * w) / gc);
        const CT den = s * s + k * s + 1.0f;
        struct R { CT m_lp, m_bp, m_hp; float m_k; } out{ 1.0f / den, s / den, (s * s) / den, k };
        return out;
    };
    switch (model) {
    case FilterModelE::LP12: return std::abs(svf(res).m_lp);
    case FilterModelE::LP24: { const CT h = svf(res * 0.75f).m_lp; return std::abs(h * h); }
    case FilterModelE::HP12: return std::abs(svf(res).m_hp);
    case FilterModelE::BP12: { auto r = svf(res); return std::abs(r.m_bp * r.m_k); }
    case FilterModelE::NOTCH: { auto r = svf(res); return std::abs(r.m_lp + r.m_hp); }
    case FilterModelE::PEAK: { auto r = svf(0.5f + 0.5f * res); return std::abs(1.0f + 4.0f * res * r.m_k * r.m_bp); }
    case FilterModelE::LP6: {
        const float g = 1.0f - std::exp(-TwoPi * fc / sr);
        const CT z = std::polar(1.0f, -w);
        return std::abs(g / (1.0f - (1.0f - g) * z));
    }
    case FilterModelE::LADDER: {
        const float k = 4.0f * res * res * 0.98f;
        const float g = std::min(1.0f - std::exp(-TwoPi * std::min(fc * 1.55f, sr * 0.45f) / sr), ladderCeiling(k));
        const CT z = std::polar(1.0f, -w);
        const CT G = g / (1.0f - (1.0f - g) * z);
        const CT G4 = G * G * G * G;
        return std::abs(G4 / (1.0f + k * G4)) * (1.0f + k * 0.5f);
    }
    case FilterModelE::FORMANT: {
        float f[3]; formantsAt(fc, f);
        const float q = 3.0f + 12.0f * res;
        static const float gain[3] = { 1.0f, 0.5f, 0.3f };
        float sum = 0.0f;
        for (int i = 0; i < 3; ++i) {
            const float gc = std::tan(Pi * clampv(f[i], 10.0f, sr * 0.45f) / sr);
            const CT s(0.0f, std::tan(0.5f * w) / gc);
            const float k = 1.0f / q;
            sum += std::abs(k * s / (s * s + k * s + 1.0f)) * gain[i];   // magnitudes summed: the picture without the phases
        }
        return sum;
    }
    case FilterModelE::COMB: {
        const float D = clampv(sr / fc, 2.0f, static_cast<float>(CombMax - 4));
        const float fb = 0.98f * res;
        const CT z = std::polar(1.0f, -w);
        const CT zD = std::polar(1.0f, -w * D);
        const CT damp = 0.35f / (1.0f - 0.65f * z);
        return std::abs((1.0f - fb) / (1.0f - fb * damp * zD));
    }
    case FilterModelE::MOOG: case FilterModelE::PROPHET: case FilterModelE::JUNO: {
        // Small-signal, the ladder and the cascades are the same four one-poles with the fourth
        // fed back, at twice the rate; they differ in how they saturate and in their makeup.
        const float k = model == FilterModelE::MOOG ? 4.0f * res * 0.985f : 4.1f * res;
        const float makeup = 1.0f + (model == FilterModelE::JUNO ? 0.25f : 0.5f) * k;
        const CT s(0.0f, std::tan(Pi * hz / (2.0f * sr)) / std::tan(Pi * fc / (2.0f * sr)));
        const CT h = 1.0f / (1.0f + s);
        const CT h4 = h * h * h * h;
        return std::abs(h4 / (1.0f + k * h4)) * makeup;
    }
    case FilterModelE::SEM: {
        const float R = 0.707f * (1.0f - res) + 0.012f;
        const float m = clampv(morph, 0.0f, 1.0f);
        const CT s(0.0f, std::tan(Pi * hz / (2.0f * sr)) / std::tan(Pi * fc / (2.0f * sr)));
        const CT den = s * s + 2.0f * R * s + 1.0f;
        const CT lp = 1.0f / den, hp = (s * s) / den;
        return std::abs(m < 0.5f ? lp + 2.0f * m * hp : (2.0f - 2.0f * m) * lp + hp);
    }
    case FilterModelE::DIODE: {
        // The four nodes solved from the last one up (v3 = 1): the half capacitor at the end, the
        // three coupled through their diode pairs, u the input after the feedback.
        const float k = 17.5f * res;
        const CT s(0.0f, std::tan(Pi * hz / (2.0f * sr)) / (std::tan(Pi * fc / (2.0f * sr)) * 0.70710678f));
        const CT a = s + 2.0f;
        const CT v2 = 0.5f * a, v1 = a * v2 - 1.0f, v0 = a * v1 - v2, u = a * v0 - v1;
        return std::abs(1.0f / (u + k)) * (1.0f + 0.3f * k);
    }
    default: return 1.0f;
    }
}

} // namespace ambient
