/**
 * @file Body.cpp
 * @brief The resonating body: the four materials' mode tables and the bank they tune.
 *
 * What lives here and not in Body.h is the data that makes a material a material -- the twelve
 * mode ratios of wood, plate, bell and string, and how fast each material loses its high modes --
 * and the two things the class does with it. set() turns the five arguments into twelve resonator
 * coefficients and the pan of every mode, rebuilt only when something has moved: the brain's root
 * changes every few minutes, but an LFO on Tone calls it every block. process() runs the bank on
 * the mono send and adds each mode to its own side of the stereo field, then guards the resonators
 * once a block. The engine calls prepare() once, set() per block from the parameters and the
 * brain's root, and process() on the audio thread after the mix it answers is finished.
 */
#include "ambient/Body.h"
#include <cmath>

namespace ambient {

const char* const kBodyMaterialNames[kNumBodyMaterials] = { "Wood", "Plate", "Bell", "String" };

namespace {

/**
 * @brief Mode ratios.
 *
 * Wood: a soundboard's low, irregular modes (measured spruce plates cluster like
 * this). Plate: the stretched series of a flat metal plate. Bell: the classic hum-prime-tierce-
 * quint-nominal of a tuned bell, continued. String: harmonic with a little stiffness, which is
 * what an actual string does.
 */
const float kRatios[kNumBodyMaterials][kBodyModes] = {
    { 1.00f, 1.41f, 1.93f, 2.42f, 2.87f, 3.61f, 4.19f, 4.97f, 5.83f, 6.72f, 7.91f, 9.14f },   // Wood
    { 1.00f, 2.01f, 2.98f, 4.02f, 5.44f, 6.79f, 8.11f, 9.98f, 11.7f, 13.9f, 16.2f, 19.1f },   // Plate
    { 0.50f, 1.00f, 1.19f, 1.50f, 2.00f, 2.51f, 2.66f, 3.01f, 4.07f, 5.12f, 6.31f, 8.02f },   // Bell
    { 1.00f, 2.00f, 3.01f, 4.02f, 5.04f, 6.07f, 7.11f, 8.16f, 9.23f, 10.3f, 11.4f, 12.6f },   // String
};

/**
 * @brief How fast each material's higher modes die away relative to the first (a real body loses its
 *        high modes first; a bell holds them, which is why a bell rings and a plate clangs).
 *
 * The exponent on a mode's ratio: mode i rings for decay * ratio^-kDamping of the first mode's time.
 */
const float kDamping[kNumBodyMaterials] = { 0.55f, 0.40f, 0.20f, 0.35f };

} // namespace

void Body::prepare(double sampleRate, uint64_t seed)
{
    sr_ = static_cast<float>(sampleRate);
    rng_.seed(seed);
    for (float& m : modePan_) m = 0.45f + 0.55f * rng_.uniform();
    reset();
    lastMat_ = BodyMaterial::Count;   // force a rebuild
}

void Body::reset()
{
    for (auto& r : res_) r.reset();
}

void Body::set(BodyMaterial material, float baseHz, float decaySeconds, float tone, float spread)
{
    const int m = clampv(static_cast<int>(material), 0, kNumBodyMaterials - 1);
    const float base = clampv(baseHz, 20.0f, 2000.0f);
    const float decay = clampv(decaySeconds, 0.05f, 30.0f);
    const float t = clampv(tone, 0.0f, 1.0f);
    const float sp = clampv(spread, 0.0f, 1.0f);
    // Rebuilding twelve resonators is twelve cos and twelve exp: cheap per block, but pointless
    // when nothing moved, and the brain's root only changes every few minutes.
    if (static_cast<BodyMaterial>(m) == lastMat_ && base == lastBase_ && decay == lastDecay_
        && t == lastTone_ && sp == lastSpread_) return;
    lastMat_ = static_cast<BodyMaterial>(m); lastBase_ = base; lastDecay_ = decay; lastTone_ = t; lastSpread_ = sp;

    float sumSq = 0.0f;
    for (int i = 0; i < kBodyModes; ++i) {
        const float ratio = kRatios[m][i];
        const float hz = clampv(base * ratio, 20.0f, sr_ * 0.45f);
        // Decay: the first mode gets the full time, the higher ones lose it at the material's rate.
        // A resonator's T60 is 2.2 / bandwidth, so the bandwidth follows from the time we want.
        const float t60 = decay * std::pow(ratio, -kDamping[m]);
        // Q is capped at 300: a mode narrower than that is never excited by a drone that drifts
        // a couple of cents, and a body whose modes cannot be excited is not a body.
        const float bw = clampv(2.2f / std::max(t60, 0.02f), hz / 300.0f, 400.0f);
        // Tone tilts the gains: at 0 the body is dark (only the low modes speak), at 1 the high
        // modes are as loud as the low ones.
        const float g = std::pow(ratio, -1.4f + 1.4f * t);
        res_[i].set(hz, bw, g, sr_);
        // Each mode has its own place, and neighbouring modes sit on opposite sides. Scattering
        // them randomly around the centre put most modes in both channels, and since they are all
        // driven by the same mono signal the two channels came out correlated: the body collapsed
        // the stereo image (measured: width 0.69 -> 0.11). Alternating sides gives each ear a
        // different set of modes, which is both what a real body does and what keeps the width.
        // How far out each mode sits is drawn ONCE, in prepare(): this function rebuilds whenever
        // any of its five arguments moves, and Tone, Decay and the root all move -- an LFO on Tone
        // called it every block, so a fresh draw here meant all twelve modes jumping to new places
        // a hundred times a second. The body is a body; where its modes are does not flutter.
        const float pan = sp * ((i & 1) ? 1.0f : -1.0f) * modePan_[i];
        const float angle = (pan + 1.0f) * 0.25f * kPi;
        gainL_[i] = std::cos(angle);
        gainR_[i] = std::sin(angle);
        // A resonator normalised to unity at its peak passes almost nothing of a broadband
        // signal when it is narrow: the power it collects is proportional to its bandwidth. So
        // the level is normalised on the expected power instead, the way the Air band is --
        // otherwise the Body knob would do nothing at long decays and far too much at short
        // ones. (Measured before this: a body at 0.8 changed the mix by 0.02 dB.)
        sumSq += g * g * kPi * bw / (2.0f * sr_);
    }
    // The cap keeps a very long, very narrow body from turning a partial that happens to sit on
    // a mode into a howl: past 60 the resonance is a feedback loop, not a body.
    // 0.05 puts the body at Level 1 at about the level of the dry mix it is answering (measured
    // on the default patch); 0.32 made it 11 dB louder than the music and drove the clipper.
    norm_ = std::min(0.05f / std::sqrt(std::max(sumSq, 1e-9f)), 60.0f);
}

void Body::process(const float* in, float* outL, float* outR, int n, float level)
{
    if (level <= 0.0f) return;
    const float g = level * norm_;
    for (int i = 0; i < n; ++i) {
        const float x = in[i];
        float l = 0.0f, r = 0.0f;
        for (int m = 0; m < kBodyModes; ++m) {
            const float y = res_[m].tick(x);
            l += y * gainL_[m];
            r += y * gainR_[m];
        }
        outL[i] += l * g;
        outR[i] += r * g;
    }
    // Once a block, after the modes have run: see Resonator::guard.
    for (auto& r : res_) r.guard();
}

} // namespace ambient
