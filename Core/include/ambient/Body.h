// Noctuary -- the resonating body: a bank of modes under everything.
//
// The Cosmos comb is a tuned comb: one delay, one pitch, a metallic ring. A real instrument's
// body is not that. It is a small number of modes with their own frequencies, their own decay
// times and their own places in the stereo field, excited by whatever passes through it -- a
// soundboard, a plate, a bell, the air in a wooden room. Rich's drones sit on such a thing: the
// pad is the string, the body is what makes it an instrument rather than an oscillator.
//
// Twelve modes, four materials (the ratio sets are what makes wood sound like wood and a plate
// like a plate), tuned to a frequency the engine gives it (normally the brain's root), fed from
// a send of the finished mix and returned to it. Nothing here is a reverb: a reverb is a
// statistical tail, this is a handful of pitched resonances, which is exactly the difference the
// ear hears between a room and a body.
#pragma once
#include "Dsp.h"
#include "ZPlane.h"   // Resonator

namespace ambient {

enum class BodyMaterial : int { Wood = 0, Plate, Bell, String, Count };
constexpr int kNumBodyMaterials = static_cast<int>(BodyMaterial::Count);
extern const char* const kBodyMaterialNames[kNumBodyMaterials];

constexpr int kBodyModes = 12;

class Body {
public:
    void prepare(double sampleRate, uint64_t seed);
    // baseHz: what the body is tuned to (the brain's root). decaySeconds: the longest mode's T60;
    // tone tilts the gains towards the high modes; spread fans the modes across the stereo field.
    void set(BodyMaterial material, float baseHz, float decaySeconds, float tone, float spread);
    // Adds the body's answer to `in` into outL/outR at `level`. The input is mono (the sum), the
    // output is stereo because the modes sit in different places.
    void process(const float* in, float* outL, float* outR, int n, float level);
    void reset();

private:
    Resonator res_[kBodyModes];
    float     gainL_[kBodyModes] = {}, gainR_[kBodyModes] = {};
    float     modePan_[kBodyModes] = {};   // how far out each mode sits: drawn once, in prepare()
    float     sr_ = 48000.0f;
    float     norm_ = 1.0f;
    Rng       rng_;
    // what set() last saw, so coefficients are only rebuilt when they change
    BodyMaterial lastMat_ = BodyMaterial::Count;
    float     lastBase_ = -1.0f, lastDecay_ = -1.0f, lastTone_ = -1.0f, lastSpread_ = -1.0f;
};

} // namespace ambient
