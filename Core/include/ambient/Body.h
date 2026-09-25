/**
 * @file Body.h
 * @brief The resonating body: a bank of modes under everything.
 *
 * The Cosmos comb is a tuned comb: one delay, one pitch, a metallic ring. A real instrument's
 * body is not that. It is a small number of modes with their own frequencies, their own decay
 * times and their own places in the stereo field, excited by whatever passes through it -- a
 * soundboard, a plate, a bell, the air in a wooden room. Rich's drones sit on such a thing: the
 * pad is the string, the body is what makes it an instrument rather than an oscillator.
 *
 * Twelve modes, four materials (the ratio sets are what makes wood sound like wood and a plate
 * like a plate), tuned to a frequency the engine gives it (normally the brain's root), fed from
 * a send of the finished mix and returned to it. Nothing here is a reverb: a reverb is a
 * statistical tail, this is a handful of pitched resonances, which is exactly the difference the
 * ear hears between a room and a body.
 */
#pragma once
#include "Dsp.h"
#include "ZPlane.h"   // Resonator

namespace ambient {

/**
 * @brief The four materials: which set of mode ratios and which damping the body is built from.
 *
 * The value is the Material parameter's; kBodyMaterialNames gives the display names in the same
 * order, and Body.cpp holds the twelve ratios of each material and how fast its high modes die
 * relative to its first.
 */
enum class BodyMaterial : int {
    Wood = 0,   ///< a soundboard's low, irregular modes
    Plate,      ///< the stretched series of a flat metal plate
    Bell,       ///< hum, prime, tierce, quint and nominal of a tuned bell, continued
    String,     ///< harmonic with a little stiffness, as an actual string is
    Count       ///< the number of materials
};
constexpr int kNumBodyMaterials = static_cast<int>(BodyMaterial::Count);   ///< entries of kBodyMaterialNames and of the ratio tables in Body.cpp
extern const char* const kBodyMaterialNames[kNumBodyMaterials];   ///< display names in BodyMaterial order ("Wood", "Plate", "Bell", "String"), defined in Body.cpp

constexpr int kBodyModes = 12;   ///< resonators per body: one for each mode ratio of the material

/**
 * @brief Twelve pitched resonances on a send of the mix, returned in stereo.
 *
 * One instance sits after the effects on a mono send of the finished mix. prepare() is called
 * once with the rate and a seed, set() every block from the parameters and the brain's root
 * (it rebuilds nothing unless something moved), process() on the audio thread with the block.
 * Each mode is one two-pole Resonator (ZPlane.h) with its own frequency, bandwidth and gain,
 * placed on its own side of the field; the sum is normalised on the modes' expected power so the
 * Body knob means the same thing at long and at short decays.
 */
class Body {
public:
    /**
     * @brief Takes the sample rate, draws each mode's place in the field once, and silences the modes.
     *
     * The pan magnitudes come from the seed, so a seed always puts the same mode in the same
     * place; the next set() is forced to rebuild the coefficients. Not for the audio thread.
     * @param sampleRate  the engine's rate in Hz; every coefficient is computed against it
     * @param seed        seeds the body's own generator, which draws the twelve pan magnitudes
     */
    void prepare(double sampleRate, uint64_t seed);
    /**
     * @brief Coefficients for the twelve modes from the material, the root and the three knobs.
     *
     * baseHz: what the body is tuned to (the brain's root). decaySeconds: the longest mode's T60;
     * tone tilts the gains towards the high modes; spread fans the modes across the stereo field.
     *
     * Every block from the parameters. The five arguments are clamped and compared with what the
     * last call saw, and nothing is rebuilt unless one of them moved -- the brain's root changes
     * only every few minutes. A rebuild is twelve cos and twelve exp plus the normalisation.
     * @param material      which ratio set and damping (clamped into BodyMaterial)
     * @param baseHz        frequency of the first mode in Hz, clamped 20 .. 2000
     * @param decaySeconds  T60 of the first mode, 0.05 .. 30 s; the higher modes lose it at the material's rate
     * @param tone          0 .. 1: at 0 the body is dark and only the low modes speak, at 1 the high modes are as loud as the low ones
     * @param spread        0 .. 1: how far the modes fan out from the centre, neighbouring modes on opposite sides
     */
    void set(BodyMaterial material, float baseHz, float decaySeconds, float tone, float spread);
    /**
     * @brief Runs one block of the send through the modes and adds the answer to the outputs.
     *
     * Adds the body's answer to `in` into outL/outR at `level`. The input is mono (the sum), the
     * output is stereo because the modes sit in different places.
     *
     * Audio thread. Returns at once when `level` is zero; after the block every resonator's guard
     * runs (Resonator::guard). The gain is `level` times the normalisation set() computed.
     * @param in     n samples of the mono send (the sum of the mix)
     * @param outL   left output, the body ADDED to what is already there
     * @param outR   right output, likewise
     * @param n      samples in the block
     * @param level  the Body knob; 0 does nothing at all, 1 puts the body about at the level of the dry mix it answers
     */
    void process(const float* in, float* outL, float* outR, int n, float level);
    /** @brief Silences the twelve resonators; the coefficients stay. */
    void reset();

private:
    Resonator res_[kBodyModes];   ///< the modes, one two-pole resonator each (ZPlane.h)
    float     gainL_[kBodyModes] = {},   ///< each mode's gain into the left channel, cos of its pan angle
              gainR_[kBodyModes] = {};   ///< each mode's gain into the right channel, sin of its pan angle
    float     modePan_[kBodyModes] = {};   ///< how far out each mode sits: drawn once, in prepare()
    float     sr_ = 48000.0f;   ///< sample rate the coefficients were computed for
    float     norm_ = 1.0f;     ///< output gain from the modes' expected power (0.05 over its root, capped at 60), rebuilt by set()
    Rng       rng_;             ///< the body's own generator, seeded in prepare(); it draws the pan magnitudes
    /**
     * @name what set() last saw, so coefficients are only rebuilt when they change
     * @{ */
    BodyMaterial lastMat_ = BodyMaterial::Count;   ///< the material; Count means "nothing yet", which prepare() uses to force a rebuild
    float     lastBase_ = -1.0f,     ///< the clamped baseHz
              lastDecay_ = -1.0f,    ///< the clamped decaySeconds
              lastTone_ = -1.0f,     ///< the clamped tone
              lastSpread_ = -1.0f;   ///< the clamped spread
    /** @} */
};

} // namespace ambient
