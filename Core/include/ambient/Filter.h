/**
 * @file Filter.h
 * @brief The voice filter: one of fifteen models behind the same five knobs.
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
 *
 * Five more since 26.09.2026, from Ephemeris: the classic analogue filters as circuit models
 * (CircuitFilter.h) -- the Moog ladder, the Oberheim SEM with its morph from low pass through notch
 * to high pass, the OTA cascades of the Prophet and the Juno, and the diode ladder of the EMS and the
 * TB-303. They run at twice the rate, both channels at once, and their resonance sits on Cutoff: a
 * ringing ladder with Key Track 1 plays the note.
 * @author Rene Weller
 */
#pragma once
#include "CircuitFilter.h"
#include "Dsp.h"
#include "Oversample.h"

namespace ambient {

/**
 * @brief The fifteen models a VoiceFilter can run; the value is the Model parameter's, FilterModelNames the display names.
 *
 * The first ten are built on the TPT state-variable filter of Dsp.h (Svf), except Lp6, Ladder and
 * Comb; the last five are the circuit models of CircuitFilter.h, appended so that every preset
 * written before them keeps its model. The coefficients each model makes of Cutoff and Resonance
 * are in VoiceFilter::set (Filter.cpp).
 */
enum class FilterModelE : int {
    LP6 = 0,   ///< a one-pole low pass, 6 dB an octave
    LP12,      ///< the state-variable low pass the voice always had
    LP24,      ///< two Lp12 stages in a row, the resonance shared
    HP12,      ///< the state-variable high pass
    BP12,      ///< the state-variable band pass, unity gain at the centre
    NOTCH,     ///< lp + hp: a hole at Cutoff
    PEAK,      ///< a bell at Cutoff, up to +14 dB at full Resonance
    LADDER,    ///< four one-poles with saturated feedback, self-oscillating near the top
    COMB,      ///< a feedback comb tuned to Cutoff, a low pass in its loop
    FORMANT,   ///< three band passes on the formants of a vowel Cutoff morphs through
    MOOG,      ///< the Moog transistor ladder, solved exactly: 24 dB, its resonance on Cutoff
    SEM,       ///< the Oberheim SEM's state-variable filter, 12 dB, Morph from low pass through notch to high pass
    PROPHET,   ///< the Prophet-5's OTA cascade (SSM2040 / CEM3320): 24 dB, its feedback saturating first
    JUNO,      ///< the Juno's OTA cascade (IR3109): 24 dB, cleaner stages, a gentler resonance
    DIODE,     ///< the diode ladder of the EMS and the TB-303: thinner and more nasal
    COUNT      ///< the number of models
};
constexpr int NumFilterModels = static_cast<int>(FilterModelE::COUNT);   ///< entries of FilterModelNames
extern const char* const FilterModelNames[NumFilterModels];   ///< display names in FilterModel order ("LP 6" .. "Diode"), defined in Filter.cpp
/**
 * @brief Whether a model is one of the circuit models (CircuitFilter.h), which run at twice the rate.
 * @param[in] m  the model
 * @return true for Moog, SEM, Prophet, Juno and Diode
 */
constexpr bool isCircuitModel(FilterModelE m) { return static_cast<int>(m) >= static_cast<int>(FilterModelE::MOOG) && m != FilterModelE::COUNT; }
extern const char* const FilterRouteNames[2];   ///< Series, Parallel: the names of the two ways a voice's two filters can be routed

constexpr int CombMax = 2048;   ///< longest comb delay in samples (40 Hz at 48 kHz needs 1200)

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
     * @param[in] sr  the engine's rate in Hz, kept as a float for the coefficient arithmetic
     */
    void prepare(double sr) { m_sr = static_cast<float>(sr); reset(); }
    /** @brief Clears every model's state -- the six state-variable pairs, the ladder, the one-pole, the comb -- to silence. */
    void reset();
    /**
     * @brief Coefficients for a block.
     *
     * Lp12 is exactly the old state-variable low pass.
     *
     * Called once a block from the voice's smoothed parameters. Switching the model resets the
     * state. Cutoff is clamped between 10 Hz and 0.45 of the rate, Resonance, Drive and Morph to 0 .. 1.
     * @param[in] model      which of the fifteen models to run from the next tick on
     * @param[in] cutoffHz   cutoff (or centre, or comb fundamental) in Hz, clamped inside the model
     * @param[in] resonance  0 .. 1, the meaning per model (Q, feedback, formant width)
     * @param[in] drive      0 .. 1, input gain into the soft clip -- or, for a circuit model, into the circuit itself
     *                       -- with the output compensated; off at 0
     * @param[in] morph      0 .. 1, the SEM's response: low pass, notch at 0.5, high pass; the other models ignore it
     */
    void set(FilterModelE model, float cutoffHz, float resonance, float drive, float morph = 0.0f);
    /**
     * @brief One sample of both channels through the drive and the model.
     *
     * Audio thread, once per sample. Each channel goes through the drive and then one(); the comb's
     * write pointer advances afterwards, once for both channels. The drive runs at four times the
     * rate (Oversample.h) since 25.09.2026, and is 29.5 samples late for it while it is on. A
     * circuit model runs at twice the rate as a whole, both channels in one register, its drive the
     * gain into the circuit, and is 25 samples late all the time.
     * @param[in]     inL   left input sample
     * @param[in]     inR   right input sample
     * @param[in,out] outL  left output
     * @param[in,out] outR  right output
     */
    inline void tick(float inL, float inR, float& outL, float& outR);
    /**
     * @brief How late the last tick's output is, in samples: the oversampler's round trip.
     * @return StereoOversampler2::Latency (25) for a circuit model, Oversampler4::Latency (29.5)
     *         while Drive is above 0 on any other, else 0
     */
    float latency() const { return m_circuit ? StereoOversampler2::Latency : (m_driving ? Oversampler4::Latency : 0.0f); }

    /**
     * @brief |H(f)| of a model at these settings, for the display (Drive is not a linear quantity and is
     *        left out of the picture). Static: the picture never touches a voice.
     *
     * The analogue prototype of each model is evaluated at the warped frequency, so the curve is
     * the one the digital filter actually has; the formant model sums magnitudes, not phases. The
     * circuit models are drawn by their small-signal response: what they do to a quiet input.
     * @param[in] model      the model whose response is drawn
     * @param[in] cutoffHz   the Cutoff knob in Hz (clamped like set() does)
     * @param[in] resonance  the Resonance knob, 0 .. 1
     * @param[in] hz         the frequency to evaluate at, in Hz
     * @param[in] sr         the sample rate the voice runs at
     * @param[in] morph      the Morph knob, 0 .. 1 (the SEM's response)
     * @return the linear magnitude at hz; 1 for a model that is not drawn
     */
    static float magnitude(FilterModelE model, float cutoffHz, float resonance, float hz, float sr, float morph = 0.0f);

private:
    FilterModelE m_model = FilterModelE::LP12;   ///< the model tick() runs; set() resets the state when it changes
    float m_sr = 48000.0f;   ///< sample rate the coefficients were computed for
    Svf   m_svfL,    ///< first state-variable pair, left: every SVF model's (only) stage, the first formant
          m_svfR,    ///< first pair, right
          m_svf2L,   ///< second pair, left: Lp24's second stage, the second formant
          m_svf2R,   ///< second pair, right
          m_svf3L,   ///< third pair, left: the third formant
          m_svf3R;   ///< third pair, right -- the formant model uses all three pairs
    /**
     * @name one-pole (Lp6) and ladder
     * @{ */
    float m_g1 = 0.1f,        ///< the one-pole coefficient, 1 - exp(-2 pi fc / sr); the ladder's corner sits 1.55 x Cutoff up
          m_kLad = 0.0f,      ///< the ladder's feedback, 4 x Resonance squared x 0.98
          m_ladComp = 1.0f;   ///< makeup for the passband loss under feedback, 1 + m_kLad / 2
    float m_lad[2][4] = {};   ///< the four ladder stages per channel
    float m_onePole[2] = {};   ///< the Lp6 state per channel
    /** @} */
    /**
     * @name peak
     * @{ */
    float m_peakGain = 0.0f;   ///< gain of the band pass added to the input, 4 x Resonance (up to +14 dB)
    /** @} */
    /**
     * @name comb
     * @{ */
    float m_combBuf[2][CombMax] = {};   ///< the comb's delay line per channel, a power-of-two ring
    int   m_combW = 0;   ///< the comb's write index, shared by both channels; advanced once per tick
    float m_combDelay = 100.0f,   ///< the loop's delay in samples, sr / Cutoff, 2 .. CombMax - 4
          m_combFb = 0.0f,        ///< the loop gain, 0.98 x Resonance
          m_combDamp[2] = {};     ///< the one-pole low pass in the loop, per channel
    /** @} */
    /**
     * @name drive
     * @{ */
    float m_driveIn = 1.0f,    ///< gain into the soft clip, 1 + 6 x Drive; exactly 1 means the clip is skipped
          m_driveOut = 1.0f;   ///< gain out of it, (1 + Drive / 2) / m_driveIn
    /// The drive at four times the rate (25.09.2026, the production guide's "4x on everything
    /// nonlinear"): a soft clip at Drive 1 turns every partial above a third of Nyquist into an
    /// alias. One oversampler per channel, cleared whenever the drive comes back on so it does not
    /// start with a fragment of whatever it heard the last time.
    Oversampler4 m_osL,   ///< @brief the left channel's drive, oversampled
                 m_osR;   ///< the right channel's
    bool  m_driving = false;   ///< the drive was on at the last tick
    /** @} */
    /**
     * @name circuit models (CircuitFilter.h), at twice the rate, left in lane 0 and right in lane 1
     * @{ */
    bool  m_circuit = false;   ///< m_model is one of the circuit models
    circuit::F4 m_cv[4] = {},  ///< their node voltages, the next sample's Newton start
                m_cs[4] = {};  ///< their trapezoidal states
    StereoOversampler2 m_os2;  ///< the round trip to twice the rate and back, both channels
    float m_cg = 0.01f,        ///< tan(pi fc / 2 sr); for the diode ladder divided by sqrt 2
          m_ck = 0.0f,         ///< the feedback (Moog, cascades, diode) or the SEM's damping R
          m_cMorph = 0.0f,     ///< the SEM's morph
          m_cOut = 1.0f;       ///< the output gain: the pass band's makeup times the drive's compensation
    /** @} */

    /**
     * @brief The drive stage: a soft clip with its level compensated, at four times the rate;
     *        skipped entirely at Drive 0.
     * @param[in,out] inL  the left input sample, driven in place
     * @param[in,out] inR  the right input sample, driven in place
     */
    inline void drive(float& inL, float& inR)
    {
        if (m_driveIn == 1.0f) { m_driving = false; return; }
        if (!m_driving) { m_osL.reset(); m_osR.reset(); m_driving = true; }
        const float gi = m_driveIn, go = m_driveOut;
        auto curve = [gi, go](float v) { return softClip(v * gi) * go; };
        inL = m_osL.process(inL, curve);
        inR = m_osR.process(inR, curve);
    }
    /**
     * @brief One sample of one channel through the current model.
     * @param[in] ch  0 left, 1 right: which channel's state to use
     * @param[in] x   the (already driven) input sample
     * @return the model's output; the input unchanged for an unknown model
     */
    inline float one(int ch, float x);
    /**
     * @brief One stereo sample through the current circuit model, at twice the rate.
     * @param[in]  l   the left input at twice the rate, the drive's gain already applied
     * @param[in]  r   the right input, the same
     * @param[out] yl  receives the left output, before m_cOut
     * @param[out] yr  receives the right output
     */
    inline void circuitTick(float l, float r, float& yl, float& yr);
};

inline void VoiceFilter::circuitTick(float l, float r, float& yl, float& yr)
{
    using circuit::F4;
    using circuit::lanes;
    const F4 x = circuit::pack(l, r), g = lanes<F4>(m_cg), k = lanes<F4>(m_ck);
    F4 y;
    switch (m_model) {
    case FilterModelE::MOOG:    y = circuit::ladderMoog(m_cv, m_cs, x, g, k); break;
    case FilterModelE::SEM:     y = circuit::svfSem(m_cv, m_cs, x, g, k, m_cMorph); break;
    case FilterModelE::PROPHET: y = circuit::otaCascade(m_cv, m_cs, x, g, k, 0.7f, 1.6f); break;    // its feedback saturates before its stages
    case FilterModelE::JUNO:    y = circuit::otaCascade(m_cv, m_cs, x, g, k, 0.45f, 1.2f); break;   // cleaner stages, a softer resonance VCA
    case FilterModelE::DIODE:   y = circuit::diodeLadder(m_cv, m_cs, x, g, k); break;
    default: y = x; break;
    }
    circuit::unpack(y, yl, yr);
}

inline float VoiceFilter::one(int ch, float x)
{
    switch (m_model) {
    case FilterModelE::LP6: {
        m_onePole[ch] += m_g1 * (x - m_onePole[ch]);
        return m_onePole[ch];
    }
    case FilterModelE::LP12: return (ch == 0 ? m_svfL : m_svfR).lp(x);
    case FilterModelE::LP24: {
        Svf& a = ch == 0 ? m_svfL : m_svfR; Svf& b = ch == 0 ? m_svf2L : m_svf2R;
        return b.lp(a.lp(x));
    }
    case FilterModelE::HP12: { float lp, bp, hp; (ch == 0 ? m_svfL : m_svfR).tick(x, lp, bp, hp); return hp; }
    case FilterModelE::BP12: { float lp, bp, hp; Svf& s = ch == 0 ? m_svfL : m_svfR; s.tick(x, lp, bp, hp); return bp * s.m_k; }
    case FilterModelE::NOTCH: { float lp, bp, hp; (ch == 0 ? m_svfL : m_svfR).tick(x, lp, bp, hp); return lp + hp; }
    case FilterModelE::PEAK: { float lp, bp, hp; Svf& s = ch == 0 ? m_svfL : m_svfR; s.tick(x, lp, bp, hp); return x + m_peakGain * s.m_k * bp; }
    case FilterModelE::LADDER: {
        // Four one-poles in a row with the last stage fed back to the input through a soft
        // saturation: the classic ladder shape, self-oscillating near the top of Resonance.
        float* s = m_lad[ch];
        const float in = softClip(x - m_kLad * s[3]);
        s[0] += m_g1 * (in - s[0]);
        s[1] += m_g1 * (s[0] - s[1]);
        s[2] += m_g1 * (s[1] - s[2]);
        s[3] += m_g1 * (s[2] - s[3]);
        return s[3] * m_ladComp;
    }
    case FilterModelE::FORMANT: {
        // Three band passes on the formants of a vowel that Cutoff morphs through (u-o-a-e-i);
        // Resonance narrows them. A cold wave comes out with a throat.
        float lp, bp, hp, sum = 0.0f;
        Svf* f[3] = { ch == 0 ? &m_svfL : &m_svfR, ch == 0 ? &m_svf2L : &m_svf2R, ch == 0 ? &m_svf3L : &m_svf3R };
        static const float gain[3] = { 1.0f, 0.5f, 0.3f };
        for (int i = 0; i < 3; ++i) { f[i]->tick(x, lp, bp, hp); sum += bp * f[i]->m_k * gain[i]; }
        return sum;
    }
    case FilterModelE::COMB: {
        // Feedback comb tuned to Cutoff, a low pass in the loop; output scaled so the peaks sit at
        // unity and Resonance deepens the dips between them instead of raising the level.
        float* buf = m_combBuf[ch];
        // In double: as a float the write pointer lost its integer part after 2^24 samples, and a
        // note held for six minutes (the brain holds for up to ten) heard its comb drift.
        const double rp = static_cast<double>(m_combW) - static_cast<double>(m_combDelay);
        const int i0 = static_cast<int>(std::floor(rp));
        const float f = static_cast<float>(rp - static_cast<double>(i0));
        const float a = buf[(i0) & (CombMax - 1)], b = buf[(i0 + 1) & (CombMax - 1)];
        const float d = a + f * (b - a);
        m_combDamp[ch] += 0.35f * (d - m_combDamp[ch]);
        const float y = x + m_combFb * m_combDamp[ch];
        buf[m_combW & (CombMax - 1)] = y;
        return y * (1.0f - m_combFb);
    }
    default: return x;
    }
}

inline void VoiceFilter::tick(float inL, float inR, float& outL, float& outR)
{
    if (m_circuit) {
        // The whole circuit at twice the rate, both channels at once: the drive is the gain into
        // it, and its own stages are the saturation.
        const float gi = m_driveIn;
        m_os2.process(inL, inR, [this, gi](float l, float r, float& yl, float& yr) { circuitTick(l * gi, r * gi, yl, yr); });
        outL = inL * m_cOut;
        outR = inR * m_cOut;
        return;
    }
    drive(inL, inR);
    outL = one(0, inL);
    outR = one(1, inR);
    if (m_model == FilterModelE::COMB) m_combW = (m_combW + 1) & (CombMax - 1);
}

} // namespace ambient
