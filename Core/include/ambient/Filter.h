/**
 * @file Filter.h
 * @brief The voice filter: one of ten models behind the same five knobs.
 *
 * For a long time the voice had a single 12 dB state-variable low pass. That is the right default
 * for a drone instrument (it takes the edge off a spectrum without imposing a character), but a
 * pad that has to live for twenty minutes wants more than one way of being carved: a steeper
 * slope, the opposite slope, a band, a notch that sweeps a hole through the harmonics, a bell
 * that lifts one register, the saturating four-pole ladder everyone recognises, and a tuned comb
 * -- which on a sustained cluster is less a filter than a second resonating body.
 *
 * Every model reads the same Cutoff / Resonance / Drive, so a preset can switch models and stay
 * in a sensible place, and every model reports its own frequency response for the display.
 */
#pragma once
#include "Dsp.h"

namespace ambient {

/**
 * @brief The ten models a VoiceFilter can run; the value is the Model parameter's, kFilterModelNames the display names.
 *
 * All but Lp6, Ladder and Comb are built on the TPT state-variable filter of Dsp.h (Svf); the
 * coefficients each model makes of Cutoff and Resonance are in VoiceFilter::set (Filter.cpp).
 */
enum class FilterModel : int {
    Lp6 = 0,   ///< a one-pole low pass, 6 dB an octave
    Lp12,      ///< the state-variable low pass the voice always had
    Lp24,      ///< two Lp12 stages in a row, the resonance shared
    Hp12,      ///< the state-variable high pass
    Bp12,      ///< the state-variable band pass, unity gain at the centre
    Notch,     ///< lp + hp: a hole at Cutoff
    Peak,      ///< a bell at Cutoff, up to +14 dB at full Resonance
    Ladder,    ///< four one-poles with saturated feedback, self-oscillating near the top
    Comb,      ///< a feedback comb tuned to Cutoff, a low pass in its loop
    Formant,   ///< three band passes on the formants of a vowel Cutoff morphs through
    Count      ///< the number of models
};
constexpr int kNumFilterModels = static_cast<int>(FilterModel::Count);   ///< entries of kFilterModelNames
extern const char* const kFilterModelNames[kNumFilterModels];   ///< display names in FilterModel order ("LP 6" .. "Formant"), defined in Filter.cpp
extern const char* const kFilterRouteNames[2];   ///< Series, Parallel: the names of the two ways a voice's two filters can be routed

constexpr int kCombMax = 2048;   ///< longest comb delay in samples (40 Hz at 48 kHz needs 1200)

/**
 * @brief One stereo filter of a voice: the model, its coefficients and its state for both channels.
 *
 * A voice owns one (or two, routed in series or parallel). prepare() takes the rate, set() runs
 * once a block with the model and the three knobs, tick() once a sample with both channels; a
 * model change starts from rest because the states of one model mean nothing to another. The
 * drive is a soft clip ahead of the filter with its level compensated, so the knob adds harmonics
 * rather than loudness, and it costs nothing at zero. magnitude() is the display's static picture
 * of the same coefficients.
 */
class VoiceFilter {
public:
    /**
     * @brief Takes the sample rate and silences the filter.
     * @param sr  the engine's rate in Hz, kept as a float for the coefficient arithmetic
     */
    void prepare(double sr) { sr_ = static_cast<float>(sr); reset(); }
    /** @brief Clears every model's state -- the six state-variable pairs, the ladder, the one-pole, the comb -- to silence. */
    void reset();
    /**
     * @brief Coefficients for a block.
     *
     * Lp12 is exactly the old state-variable low pass.
     *
     * Called once a block from the voice's smoothed parameters. Switching the model resets the
     * state. Cutoff is clamped between 10 Hz and 0.45 of the rate, Resonance and Drive to 0 .. 1.
     * @param model      which of the ten models to run from the next tick on
     * @param cutoffHz   cutoff (or centre, or comb fundamental) in Hz, clamped inside the model
     * @param resonance  0 .. 1, the meaning per model (Q, feedback, formant width)
     * @param drive      0 .. 1, input gain into the soft clip with the output compensated; off at 0
     */
    void set(FilterModel model, float cutoffHz, float resonance, float drive);
    /**
     * @brief One sample of both channels through the drive and the model.
     *
     * Audio thread, once per sample. Each channel goes through sat() and then one(); the comb's
     * write pointer advances afterwards, once for both channels.
     * @param inL   left input sample
     * @param inR   right input sample
     * @param outL  left output
     * @param outR  right output
     */
    inline void tick(float inL, float inR, float& outL, float& outR);

    /**
     * @brief |H(f)| of a model at these settings, for the display (Drive is not a linear quantity and is
     *        left out of the picture). Static: the picture never touches a voice.
     *
     * The analogue prototype of each model is evaluated at the warped frequency, so the curve is
     * the one the digital filter actually has; the formant model sums magnitudes, not phases.
     * @param model      the model whose response is drawn
     * @param cutoffHz   the Cutoff knob in Hz (clamped like set() does)
     * @param resonance  the Resonance knob, 0 .. 1
     * @param hz         the frequency to evaluate at, in Hz
     * @param sr         the sample rate the voice runs at
     * @return           the linear magnitude at hz; 1 for a model that is not drawn
     */
    static float magnitude(FilterModel model, float cutoffHz, float resonance, float hz, float sr);

private:
    FilterModel model_ = FilterModel::Lp12;   ///< the model tick() runs; set() resets the state when it changes
    float sr_ = 48000.0f;   ///< sample rate the coefficients were computed for
    Svf   svfL_,    ///< first state-variable pair, left: every SVF model's (only) stage, the first formant
          svfR_,    ///< first pair, right
          svf2L_,   ///< second pair, left: Lp24's second stage, the second formant
          svf2R_,   ///< second pair, right
          svf3L_,   ///< third pair, left: the third formant
          svf3R_;   ///< third pair, right -- the formant model uses all three pairs
    /**
     * @name one-pole (Lp6) and ladder
     * @{ */
    float g1_ = 0.1f,        ///< the one-pole coefficient, 1 - exp(-2 pi fc / sr); the ladder's corner sits 1.55 x Cutoff up
          kLad_ = 0.0f,      ///< the ladder's feedback, 4 x Resonance squared x 0.98
          ladComp_ = 1.0f;   ///< makeup for the passband loss under feedback, 1 + kLad_ / 2
    float lad_[2][4] = {};   ///< the four ladder stages per channel
    float onePole_[2] = {};   ///< the Lp6 state per channel
    /** @} */
    /**
     * @name peak
     * @{ */
    float peakGain_ = 0.0f;   ///< gain of the band pass added to the input, 4 x Resonance (up to +14 dB)
    /** @} */
    /**
     * @name comb
     * @{ */
    float combBuf_[2][kCombMax] = {};   ///< the comb's delay line per channel, a power-of-two ring
    int   combW_ = 0;   ///< the comb's write index, shared by both channels; advanced once per tick
    float combDelay_ = 100.0f,   ///< the loop's delay in samples, sr / Cutoff, 2 .. kCombMax - 4
          combFb_ = 0.0f,        ///< the loop gain, 0.98 x Resonance
          combDamp_[2] = {};     ///< the one-pole low pass in the loop, per channel
    /** @} */
    /**
     * @name drive
     * @{ */
    float driveIn_ = 1.0f,    ///< gain into the soft clip, 1 + 6 x Drive; exactly 1 means the clip is skipped
          driveOut_ = 1.0f;   ///< gain out of it, (1 + Drive / 2) / driveIn_
    /** @} */

    /**
     * @brief The drive stage: a soft clip with its level compensated, skipped entirely at Drive 0.
     * @param x  the input sample
     * @return   softClip(x x driveIn_) x driveOut_, or x itself when the drive is off
     */
    inline float sat(float x) const { return driveIn_ == 1.0f ? x : softClip(x * driveIn_) * driveOut_; }
    /**
     * @brief One sample of one channel through the current model.
     * @param ch  0 left, 1 right: which channel's state to use
     * @param x   the (already driven) input sample
     * @return    the model's output; the input unchanged for an unknown model
     */
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
