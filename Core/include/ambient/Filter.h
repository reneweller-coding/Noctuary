// Noctuary -- the voice filter: one of ten models behind the same five knobs.
//
// For a long time the voice had a single 12 dB state-variable low pass. That is the right default
// for a drone instrument (it takes the edge off a spectrum without imposing a character), but a
// pad that has to live for twenty minutes wants more than one way of being carved: a steeper
// slope, the opposite slope, a band, a notch that sweeps a hole through the harmonics, a bell
// that lifts one register, the saturating four-pole ladder everyone recognises, and a tuned comb
// -- which on a sustained cluster is less a filter than a second resonating body.
//
// Every model reads the same Cutoff / Resonance / Drive, so a preset can switch models and stay
// in a sensible place, and every model reports its own frequency response for the display.
#pragma once
#include "Dsp.h"

namespace ambient {

enum class FilterModel : int { Lp6 = 0, Lp12, Lp24, Hp12, Bp12, Notch, Peak, Ladder, Comb, Formant, Count };
constexpr int kNumFilterModels = static_cast<int>(FilterModel::Count);
extern const char* const kFilterModelNames[kNumFilterModels];
extern const char* const kFilterRouteNames[2];   // Series, Parallel

constexpr int kCombMax = 2048;   // longest comb delay in samples (40 Hz at 48 kHz needs 1200)

class VoiceFilter {
public:
    void prepare(double sr) { sr_ = static_cast<float>(sr); reset(); }
    void reset();
    // Coefficients for a block. Lp12 is exactly the old state-variable low pass.
    void set(FilterModel model, float cutoffHz, float resonance, float drive);
    inline void tick(float inL, float inR, float& outL, float& outR);

    // |H(f)| of a model at these settings, for the display (Drive is not a linear quantity and is
    // left out of the picture). Static: the picture never touches a voice.
    static float magnitude(FilterModel model, float cutoffHz, float resonance, float hz, float sr);

private:
    FilterModel model_ = FilterModel::Lp12;
    float sr_ = 48000.0f;
    Svf   svfL_, svfR_, svf2L_, svf2R_, svf3L_, svf3R_;   // the formant model uses all three pairs
    // one-pole (Lp6) and ladder
    float g1_ = 0.1f, kLad_ = 0.0f, ladComp_ = 1.0f;
    float lad_[2][4] = {};
    float onePole_[2] = {};
    // peak
    float peakGain_ = 0.0f;
    // comb
    float combBuf_[2][kCombMax] = {};
    int   combW_ = 0;
    float combDelay_ = 100.0f, combFb_ = 0.0f, combDamp_[2] = {};
    // drive
    float driveIn_ = 1.0f, driveOut_ = 1.0f;

    inline float sat(float x) const { return driveIn_ == 1.0f ? x : softClip(x * driveIn_) * driveOut_; }
    inline float one(int ch, float x);
};

inline float VoiceFilter::one(int ch, float x)
{
    switch (model_) {
    case FilterModel::Lp6: {
        onePole_[ch] += g1_ * (x - onePole_[ch]);
        return onePole_[ch];
    }
    case FilterModel::Lp12: return (ch == 0 ? svfL_ : svfR_).lp(x);
    case FilterModel::Lp24: {
        Svf& a = ch == 0 ? svfL_ : svfR_; Svf& b = ch == 0 ? svf2L_ : svf2R_;
        return b.lp(a.lp(x));
    }
    case FilterModel::Hp12: { float lp, bp, hp; (ch == 0 ? svfL_ : svfR_).tick(x, lp, bp, hp); return hp; }
    case FilterModel::Bp12: { float lp, bp, hp; Svf& s = ch == 0 ? svfL_ : svfR_; s.tick(x, lp, bp, hp); return bp * s.k; }
    case FilterModel::Notch: { float lp, bp, hp; (ch == 0 ? svfL_ : svfR_).tick(x, lp, bp, hp); return lp + hp; }
    case FilterModel::Peak: { float lp, bp, hp; Svf& s = ch == 0 ? svfL_ : svfR_; s.tick(x, lp, bp, hp); return x + peakGain_ * s.k * bp; }
    case FilterModel::Ladder: {
        // Four one-poles in a row with the last stage fed back to the input through a soft
        // saturation: the classic ladder shape, self-oscillating near the top of Resonance.
        float* s = lad_[ch];
        const float in = softClip(x - kLad_ * s[3]);
        s[0] += g1_ * (in - s[0]);
        s[1] += g1_ * (s[0] - s[1]);
        s[2] += g1_ * (s[1] - s[2]);
        s[3] += g1_ * (s[2] - s[3]);
        return s[3] * ladComp_;
    }
    case FilterModel::Formant: {
        // Three band passes on the formants of a vowel that Cutoff morphs through (u-o-a-e-i);
        // Resonance narrows them. A cold wave comes out with a throat.
        float lp, bp, hp, sum = 0.0f;
        Svf* f[3] = { ch == 0 ? &svfL_ : &svfR_, ch == 0 ? &svf2L_ : &svf2R_, ch == 0 ? &svf3L_ : &svf3R_ };
        static const float gain[3] = { 1.0f, 0.5f, 0.3f };
        for (int i = 0; i < 3; ++i) { f[i]->tick(x, lp, bp, hp); sum += bp * f[i]->k * gain[i]; }
        return sum;
    }
    case FilterModel::Comb: {
        // Feedback comb tuned to Cutoff, a low pass in the loop; output scaled so the peaks sit at
        // unity and Resonance deepens the dips between them instead of raising the level.
        float* buf = combBuf_[ch];
        // In double: as a float the write pointer lost its integer part after 2^24 samples, and a
        // note held for six minutes (the brain holds for up to ten) heard its comb drift.
        const double rp = static_cast<double>(combW_) - static_cast<double>(combDelay_);
        const int i0 = static_cast<int>(std::floor(rp));
        const float f = static_cast<float>(rp - static_cast<double>(i0));
        const float a = buf[(i0) & (kCombMax - 1)], b = buf[(i0 + 1) & (kCombMax - 1)];
        const float d = a + f * (b - a);
        combDamp_[ch] += 0.35f * (d - combDamp_[ch]);
        const float y = x + combFb_ * combDamp_[ch];
        buf[combW_ & (kCombMax - 1)] = y;
        return y * (1.0f - combFb_);
    }
    default: return x;
    }
}

inline void VoiceFilter::tick(float inL, float inR, float& outL, float& outR)
{
    outL = one(0, sat(inL));
    outR = one(1, sat(inR));
    if (model_ == FilterModel::Comb) combW_ = (combW_ + 1) & (kCombMax - 1);
}

} // namespace ambient
