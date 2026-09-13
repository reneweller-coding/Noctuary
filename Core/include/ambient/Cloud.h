// Noctuary -- the granular cloud on the far plane.
//
// Grains of the recent foreground, scattered back in time, transposed and dropped into the far
// reverb. Since 12.09.2026 it also does what Absynth's Aetherizer is known for -- grains that hear
// their own echoes, tuned resonance, grains in groups -- built from the parts the literature does
// better where it has them:
//
//   Feedback   The cloud's output goes back into its own history: grains of grains, and the
//              transpositions stack on every pass. The loop is low-passed at Tone, saturated with
//              first-order antiderivative antialiasing (Parker, Zavalishin and Le Bihan, DAFx 2016;
//              Bilbao, Esqueda, Parker and Valimaki, IEEE SPL 2017), so the partials a saturation
//              makes are not folded back below Nyquist again on every pass, and throttled by its
//              own level like the shimmer loop, so at 1 it holds instead of running away.
//   Shift      The loop transposed on every pass -- an octave, a fifth or a fourth, up or down --
//              in the spectrum (Shifter.h), so a spiral of grains climbs or sinks cleanly instead of
//              growing grainier on every turn.
//   Scatter    Transpositions drawn from the intervals of the scale that is playing, measured from
//              the conductor's root and opened from the most consonant outwards -- octaves, then
//              fifths and fourths, then the rest -- until at a half every interval of the scale
//              within an octave is in; beyond a half the grains begin to leave the scale, and at 1
//              they are free. The Aetherizer rasters to thirteen fixed twelve-tone menus; this
//              follows the instrument's own tuning, just intonation and Scala files included.
//   Resonance  Resonators on the scale's notes (the whole scale, the chord on the root, fifths or
//              octaves). Each grain excites one of them, and the resonator rings on after its grain
//              has ended -- the Aetherizer's filter lives and dies with its grain, which is why its
//              manual has a low comb speak only with long grains. Band is the phasor filter of
//              Mathews and Smith (SMAC 2003): a rotation, so its level does not move when its
//              frequency does, and a new root glides in instead of clicking. Comb is a tuned
//              feedback comb, every harmonic of the note ringing.
//   Swarm      The onsets as a self-exciting (Hawkes) process: every grain raises the chance of the
//              next for a quarter of a second, so the grains come in flocks at the same mean rate.
//              At 0 they are the Poisson stream the cloud always had.
//
// The grain loop is the vector kernel of the texture sources (GrainRing.h). Buffers are allocated
// in prepare() only.
#pragma once
#include "Dsp.h"
#include "Adaa.h"
#include "Shifter.h"
#include "Tuning.h"
#include "GrainRing.h"
#include <vector>
#include <cstdint>

namespace ambient {

class GrainCloud {
public:
    static constexpr int kMaxGrains = 64;
    static constexpr int kMaxRes = 12;
    static constexpr int kSub = 64;          // the loop's delay and the resonators' block, in samples

    void prepare(double sampleRate, uint64_t seed);
    void set(float densityPerSec, float sizeMs, float pitch, float spraySec, float level);
    // Feedback 0..1 (1 holds at the loop's ceiling) and the loop's low-pass in hertz.
    void setLoop(float feedback, float toneHz);
    // The loop's transposition on every pass, in semitones; 0 is off.
    void setShift(float semitones);
    // Transposition of every grain in semitones, and the scatter over the scale's intervals, 0..1.
    void setScatter(float transposeSemitones, float scatter);
    // 0: Poisson onsets, as always; towards 1 the onsets excite each other and come in flocks.
    void setSwarm(float swarm);
    // Mix 0..1; mode 0 Band, 1 Comb; notes 0 Scale, 1 Chord, 2 Fifths, 3 Octaves; ring time to -60 dB.
    void setResonators(float amount, int mode, int notes, float decaySeconds);
    // The music the scatter and the resonators follow: the scale, the frequency of its tonic and of
    // the conductor's root. Cheap to call on every block -- the tables are rebuilt only when one of
    // the three has moved.
    void setHarmony(const FixedScale& scale, double tonicHz, double rootHz);
    // Feeds the history with both channels and ADDS the cloud to outL/outR. A grain reads the left
    // history into its left gain and the right into its right, so a wide field keeps its sides in
    // the cloud; identical channels give exactly the cloud the mono sum would.
    void process(const float* inL, const float* inR, float* outL, float* outR, int n);
    // No grain sounding and nothing left ringing: the engine may stop calling process().
    bool quiet() const { return live_ == 0 && tail_ <= 0; }

    // For the self test.
    int       intervalCount() const { return numIv_; }
    float     interval(int k) const { return iv_[k]; }
    int       resonatorCount() const { return numRes_; }
    float     resonatorHz(int k) const { return resHz_[k]; }
    long long grainsStarted() const { return started_; }
private:
    static constexpr int kMaxIv = 3 * FixedScale::kMax;
    static constexpr int kFbLen = 256;       // the loop's ring: a power of two, at least two sub-blocks
    static constexpr int kCombLen = 1024;    // a comb resonator's ring (down to 47 Hz)
    void   spawnSub(int w0, int m);
    void   spawn(int w0, int offset);
    double drawRatio();
    void   renderGrains(int m);
    void   mixSub(int m, float* outL, float* outR);
    void   runResonators(int m, float* wetL, float* wetR);
    void   buildIntervals();
    void   buildResonators();

    std::vector<float> hist_;                // the history, left and right interleaved, cap_ frames
    int       cap_ = 0, w_ = 0;
    long long t_ = 0;                        // samples processed: the loop's ring is indexed by it
    RingGrain grains_[kMaxGrains];
    int       live_ = 0;
    long long started_ = 0;
    Rng       rng_;
    double    sr_ = 48000.0;
    float     density_ = 12.0f, size_ = 250.0f, pitch_ = 0.3f, spray_ = 0.8f, level_ = 0.7f;
    float     levelCur_ = 0.7f;
    int       tail_ = 0;
    bool      routing_ = false;              // grains go to their resonators' buses this sub-block
    // onsets
    float     swarm_ = 0.0f;
    double    hazard_ = 0.0, nextHazard_ = 0.0, excite_ = 0.0, exciteDecay_ = 1.0;
    // the loop
    float     feedback_ = 0.0f, toneHz_ = 5000.0f, toneC_ = 0.5f, fbCur_ = 0.0f;
    float     fbRingL_[kFbLen] = {}, fbRingR_[kFbLen] = {};
    float     toneL_ = 0.0f, toneR_ = 0.0f;
    float     dcxL_ = 0.0f, dcxR_ = 0.0f, dcyL_ = 0.0f, dcyR_ = 0.0f, dcR_ = 0.997f;
    TanhAdaa  satL_, satR_;
    float     loopEnv_ = 0.0f, envC_ = 0.0002f;
    SpectralShifter shift_;
    float     shiftSemis_ = 0.0f;
    // the scatter
    double    transposeRatio_ = 1.0;
    float     scatter_ = 0.0f;
    float     iv_[kMaxIv] = {};
    int       numIv_ = 0;
    // the resonators
    float     resAmount_ = 0.0f, resCur_ = 0.0f, resDecay_ = 2.5f;
    int       resMode_ = 0, resNotes_ = 1, numRes_ = 0;
    float     resHz_[kMaxRes] = {}, resTheta_[kMaxRes] = {}, resThetaTarget_[kMaxRes] = {};
    float     bandU_[kMaxRes][2] = {}, bandV_[kMaxRes][2] = {};
    float     combLp_[kMaxRes][2] = {};
    std::vector<float> comb_;                // kMaxRes x 2 x kCombLen
    int       combW_ = 0;
    float     combA_ = 0.5f, combLag_ = 1.0f;
    float     busL_[kMaxRes][kSub] = {}, busR_[kMaxRes][kSub] = {};
    // the harmony
    bool      haveHarmony_ = false;
    double    scaleSig_ = -1.0, tonicHz_ = 0.0, rootHz_ = 0.0, period_ = 2.0;
    int       count_ = 12;
    double    ratios_[FixedScale::kMax] = {};
    double    rot_[FixedScale::kMax] = {};   // the scale's intervals above the conductor's root
};

} // namespace ambient
