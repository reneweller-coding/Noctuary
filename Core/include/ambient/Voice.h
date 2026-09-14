// Noctuary -- one cluster voice: `unison` strands, each an additive bank of
// up to 32 harmonic partials with individually drifting amplitudes and a slowly
// drifting pitch, plus a filtered-noise "air" layer. Alias-free by construction.
//
// Spatial model (after Robert Rich): every voice sits on a plane between the
// listener's ear (distance 0: dry, bright, close) and the infinite background
// (distance 1: darker, softer, sent to the far reverb). The voice's centre pan
// is rendered with a true interaural time difference, not only with gain.
#pragma once
#include "Dsp.h"
#include "Modulation.h"
#include "Sources.h"
#include "Filter.h"
#include "ZPlane.h"
#include <cstdint>

namespace ambient {

constexpr int kMaxPartials  = 32;
constexpr int kMaxStrands   = 6;
constexpr int kControlBlock = 64;   // samples between control-rate updates (1.3 ms at 48 kHz)
constexpr int kItdBuffer    = 256;  // >= 0.7 ms at 192 kHz

struct VoiceParams {
    float level = 1.0f;         // level of the partial bank (Source 1)
    int   partials = 16;
    float tilt = 1.2f, brightness = 0.7f, oddEven = 0.0f, inharmonic = 0.0f;
    // Match: partials placed on the degrees of the current scale rather than on the harmonic
    // series (Sethares, the other direction of the timbre scale). The engine fills the table;
    // at 0 the voice never reads it and its frequencies are what they always were, to the bit.
    float match = 0.0f;
    float partialRatio[kMaxPartials] = {};   // f_h / f0 with Match blended in, valid when match > 0
    float shimmer = 0.4f, shimmerRate = 0.15f;
    int   unison = 3;
    float detune = 8.0f, drift = 4.0f, driftRate = 0.08f, spread = 0.7f;
    // Two ceilings on the beating the strands make (R4.6). Both do nothing at zero.
    float lowDetune = 0.0f;     // how much the detuning thins out towards the bottom of the range
    float beatCeiling = 0.0f;   // Hz: the fastest beat a strand may make, halved under 150 Hz
    float velAttack = 0.0f;     // velocity to attack time: + lets a quiet note enter slower
    float bloom = 0.0f, bloomTime = 60.0f;   // spectrum opens from brightness*(1-bloom) to brightness over bloomTime
    int   stack = 0;            // index into kStackRatios: strands at pure ratios (0 = classic detuned unison)
    float rateWander = 0.3f;    // drift/shimmer/breath rates wander by up to +-1 octave on a 100 s curve
    float air = 0.15f, airColor = 3.0f, airQ = 10.0f;
    float attack = 6.0f, decay = 4.0f, sustain = 0.8f, release = 12.0f;
    float cutoff = 2500.0f, resonance = 0.15f, filterEnv = 0.3f, filterDrift = 0.3f, keyTrack = 0.5f;
    int   filterModel = 1;      // FilterModel (Filter.h); 1 = the 12 dB state-variable low pass
    float filterDrive = 0.0f;
    float fold = 0.0f;          // wavefolder after the filters, 0 = off (see Voice.cpp)
    // Rich refinements: the binaural phase field, the breathing doppler, the pitch tide (from the
    // engine, already a multiplier), and the strike layer
    float phaseWidth = 0.0f, phaseRate = 0.03f, doppler = 0.0f;
    // Expression: how far this voice's own pressure, slide and bend reach
    float pressDistance = 0.0f, pressBright = 0.0f, pressLevel = 0.0f;
    float slideCutoff = 0.0f, slideZ = 0.0f;
    float externalise = 0.0f;   // pinna notch + shoulder reflection, for headphones
    // Height. The ear hears up and down at the pinna: a notch between about six and ten kHz
    // whose frequency rises with elevation (Hebrank and Wright 1974), and Blauert's directional
    // band near 8 kHz that says "above". Two numbers, one per plane, so the background can be
    // the sky while the foreground stays on the ground; a voice takes its height from where it
    // stands between them. Zero is exactly the old flat field.
    float elevNear = 0.0f, elevFar = 0.0f;   // -1 below .. 1 above
    // Depth Law: the plane warped so that the KNOB is linear in heard distance rather than in
    // the model's. Zahorik's pooled exponent for perceived against physical distance is about
    // 0.54, so at 1 the plane is d^1.85: the far half of the knob then sounds as far again as
    // the near half, instead of the near half doing most of the work.
    float depthLaw = 0.0f;
    float nearIld = 0.0f;
    bool  oneEuro = false;       // MPE Filter: the expression smoothing whose cutoff follows the distance left to travel
    float partialSpread = 0.0f;  // Partial Spread: the additive bank's partials spread across the stereo field one by one       // Near Field: the level difference a source within reach makes
    // Headphones binaural mode: the pan becomes an azimuth, the head's yaw turns the field the
    // other way, the interaural delay follows Woodworth's head and the shadow is at full
    // strength whatever Time Width says. Off, everything below is exactly as it was.
    bool  binaural = false;
    float headYawDeg = 0.0f;
    float sympathy = 0.0f;      // how much of the other voices this one hears, through its own filter
    float pitchMul = 1.0f;
    float strikeLevel = 0.0f, strikeDecay = 0.4f, strikeDamp = 0.5f;
    int   strikeType = 0;       // String, Wood, Metal
    bool  strikeBrain = false;  // the brain's notes strike too
    bool  filterOn = true;      // the voice filter can be switched out; z-plane Replace also bypasses it
    bool  filterParallel = false;   // both filters on: z-plane after the filter (false) or beside it (true)
    // Z-plane filter (ZPlane.h): 0 off, 1 in series after the SVF, 2 instead of it
    int   zMode = 0, zShape = 0;
    float zDecay = 2.5f, zDamp = 0.6f;   // Modal: T60 of the lowest mode, and how much shorter the high ones
    float zX = 0.5f, zY = 0.5f, zZ = 0.0f, zRate = 0.05f, zDepth = 0.5f, zRes = 0.5f, zKeyTrack = 0.0f, zMix = 0.7f;
    float panDrift = 0.4f, itd = 0.6f;
    // Rich's foreground/background carving inside the voice (see concept.md):
    float presence = 0.0f;      // dB bell at 2-5 kHz, full on the near plane, gone on the far plane
    float breath = 0.0f, breathRate = 0.03f;   // slow wandering of the distance itself (+-0.35 at 1)
    float lowCut = 0.0f;        // Hz; partials below fall 12 dB/oct, keeping the pads out of the sub's register
    float fmAmount = 0.0f;      // phase modulation of every partial (h times the deviation) by the `fm` signal given to render()
    bool  freeze = false;       // hold the spectrum still: shimmer, pitch drift, breath and bloom stop moving
    bool  airGhost = false;     // Air through six resonators on the note's harmonics 1 2 3 5 7 9 instead of one band
    double rootHz = 130.81;     // the brain's root, for the portamento's consonance gravity
    // Coherence offsets (from the engine's Kuramoto bank), added on top of the parameters
    float cohBrightness = 0.0f, cohPan = 0.0f, cohZ = 0.0f;
    // The four source slots (slot[0] = Source 1: Additive means the strand bank above, any other
    // type mutes the bank and renders in the slot) and the data they may need; pointers stay
    // valid for the block. Each slot has its own clip.
    SlotParams       slot[kSlots];
    const Wavetable* userTable = nullptr;
    const CycleTable* userCycles = nullptr;   // the same file as single cycles, for the Wavetable type
    const Texture*   texture[kSlots] = {};
    // The preset's six envelope shapes, for a slot that borrows one as its entrance. The same
    // shapes the modulation matrix reads, with the same Mode and Time; the difference is that here
    // they run from the note rather than from the phrase. Null for a shape that is not set.
    const ModEnv*     envShape[kNumModEnvs] = {};
    const ModEnvSpec* envSpec[kNumModEnvs] = {};
    // Each slot's own shape and its settings, for a slot whose Env is Own: read from the note in
    // the same way, but as a level from 0 to 1, and without using up one of the six above.
    const ModEnv*     srcEnvShape[kSlots] = {};
    const ModEnvSpec* srcEnvSpec[kSlots] = {};
};

class Voice {
public:
    void prepare(double sampleRate, uint64_t seed);
    // distance 0..1 = plane (see above); fixed for the life of the note.
    // allowStrike false: this note does not strike even where the section says it would. The
    // engine decides it, because whether a conductor's note strikes is a property of the piece
    // (its chance, its cascade), not of the voice that happens to be free.
    // `ageSeconds`: treat the note as having been sounding this long already. Only a cluster
    // inherited at a preset change passes anything but zero -- see Engine::startNote.
    void noteOn(int note, double freqHz, float velocity, int owner, float distance, const VoiceParams& p,
                bool allowStrike = true, float ageSeconds = 0.0f);
    void noteOff();
    void kill();

    // How many times this voice has struck since it was prepared: the engine sums them, the self
    // test counts them, and it is the only honest answer to "does it ever strike?".
    unsigned strikes() const { return strikes_; }
    bool isActive() const    { return env_.isActive(); }
    // A struck string rings on its own physics, not on the amplitude envelope: a short release
    // (or a sustain of nought and a short decay) used to end the block loop mid-swing and cut the
    // strike to zero, which is a click. The engine renders a voice while EITHER is still going;
    // the note itself is given back as soon as the envelope is done, so nothing waits for a tail.
    bool isStriking() const  { return ksOn_; }
    bool isReleasing() const { return env_.isReleasing(); }
    float level() const      { return env_.level(); }
    int  note() const        { return note_; }
    int  owner() const       { return owner_; }
    float distance() const   { return distEff_; }   // where the voice is right now (breath included)
    float layer() const      { return layer_; }     // the role it took, for the matrix source Layer
    // R4.7: what the root moved under a voice that was already sounding, so it can go on sounding
    // where it was. 1 for every note that began after the move.
    float rootComp() const      { return rootComp_; }
    void  setRootComp(float m)  { rootComp_ = m; }
    double frequency() const { return freq_; }
    // Retune a sounding voice (tuning purity/drift): glides in the log domain, ~1 s time constant.
    void setTargetFrequency(double hz) { freqTarget_ = hz; }
    // Per-note expression. Pressure and slide are 0..1, bend is in semitones; all three are
    // smoothed inside the voice, so a controller sending steps never steps the sound.
    void setPressure(float v) { pressTarget_ = clampv(v, 0.0f, 1.0f); }
    void setSlide(float v)    { slideTarget_ = clampv(v, 0.0f, 1.0f); }
    void setBend(float semitones) { bendTarget_ = semitones; }
    float pressure() const { return press_; }
    float slide() const    { return slide_; }
    // Portamento: start at fromHz and slide to the note's frequency over `seconds`, slowing near
    // consonant ratios to the root by `gravity` (0 = an even log-domain glide).
    void glideFrom(double fromHz, float seconds, float gravity);
    bool gliding() const { return portaLeft_ > 0.0f; }
    // A sounding note slides to another (a near Phrase's second degree): the note and its
    // frequency change, and the slide runs on the portamento with its gravity.
    void glideTo(int note, double hz, float seconds, float gravity);
    // The plane moves: to `distance` over `seconds` -- a near event arriving out of the far plane
    // or leaving into it. Everything that hangs on the distance follows, as it does for Breath.
    void moveTo(float distance, float seconds) { distTarget_ = clampv(distance, 0.0f, 1.0f); distSeconds_ = std::max(seconds, 0.01f); }
    // Where this note stands in what its owner is sounding, for the slot roles (SlotRole): set by
    // the engine before noteOn, and again whenever the cluster changes.
    void setPlace(bool lowest, bool highest) { placeLowest_ = lowest; placeHighest_ = highest; }
    // A near event's shape, set before its noteOn: the gate as a share of the release, the bloom
    // as a factor on the cutoff, the near field's low lift in decibels, and its place in the field.
    void setNearShape(float releaseMul, float cutoffMul, float proximityDb, float pan, float gain = 1.0f)
    { releaseMul_ = std::max(releaseMul, 0.05f); cutoffMul_ = clampv(cutoffMul, 0.05f, 8.0f); proxDb_ = proximityDb; panOffset_ = clampv(pan, -1.0f, 1.0f); nearGain_ = std::max(gain, 0.0f); }
    uint64_t order = 0;      // allocation order for voice stealing

    // For pictures only: the current amplitude of each partial of the first strand, exactly the
    // numbers the oscillator is summing right now -- so a display drawn from them breathes with
    // the shimmer and the drift instead of being a static illustration. Read without
    // synchronisation from the message thread; a torn float costs one wrong pixel.
    int displayPartials(float* out, int maxCount) const
    {
        const int n = maxCount < strands_[0].active ? maxCount : strands_[0].active;
        for (int i = 0; i < n; ++i) out[i] = strands_[0].amp[i];
        return n < 0 ? 0 : n;
    }
    // The same for a slot's own bank (Additive / Wavetable in Source 1..3), and its grains.
    int displaySlotPartials(int slot, float* out, int maxCount) const { return slots_[slot < 0 ? 0 : (slot >= kSlots ? kSlots - 1 : slot)].displayAmps(out, maxCount); }
    int displayGrains(int slot, SourceSlot::GrainInfo* out, int maxCount, int clipLen) const { return slots_[slot < 0 ? 0 : (slot >= kSlots ? kSlots - 1 : slot)].displayGrains(out, maxCount, clipLen); }
    float displaySlotPosition(int slot) const { return slots_[slot < 0 ? 0 : (slot >= kSlots ? kSlots - 1 : slot)].displayPosition(); }
    float pan() const { return centre_; }   // where the voice's centre sits right now, -1..1
    // For the envelope editor's playhead: the time each slot's entrance shape was read at in the
    // last control block (seconds of the shape, -1 while the slot follows none) and the gain it
    // gave. Read without synchronisation, as above.
    float slotEnvTime(int k) const { return slotEnvT_[k < 0 ? 0 : (k >= kSlots ? kSlots - 1 : k)]; }
    float slotGain(int k) const    { return slotGain_[k < 0 ? 0 : (k >= kSlots ? kSlots - 1 : k)]; }

    // Adds `n` samples into the near (dry plane) and far (reverb send) buses. `fm` (n samples,
    // may be null) phase-modulates the partials when p.fmAmount > 0 (the feedback loop).
    // `couple` (n samples, may be null) is the previous block's foreground, mixed into this
    // voice's own filter input when p.sympathy > 0 -- strings on a shared soundboard.
    void render(float* nearL, float* nearR, float* farL, float* farR, int n, const VoiceParams& p,
                const float* fm = nullptr, const float* couple = nullptr);

private:
    struct Strand {
        // Every partial is a rotating phasor (cos, sin) turned by its own per-sample rotation;
        // independent across partials, so the loop pipelines and vectorises. Phasors are
        // renormalised once per control block.
        float  pc[kMaxPartials] = {}, ps[kMaxPartials] = {};
        float  rc[kMaxPartials] = {}, rs[kMaxPartials] = {};
        float  amp[kMaxPartials] = {};
        float  ampStep[kMaxPartials] = {};
        Drifter shimmer[kMaxPartials];
        Drifter pitch;
        float  gainL = 0.7f, gainR = 0.7f;
        // Partial Spread renders this strand as a stereo pair instead of one sum, and a pair
        // cannot be panned by multiplying each side with its own gain: that does not move the
        // field, it deletes whatever leans the wrong way. The strand's place is folded into the
        // partials' own weights instead (see control()), so what is left here is one gain.
        float  wL[kMaxPartials] = {}, wR[kMaxPartials] = {};
        float  spreadGain = 0.7f;
        int    active = 0;
    };
    void control(int blockLen, const VoiceParams& p);

    Strand   strands_[kMaxStrands];
    SourceSlot slots_[kSlots];
    float    rateMul_ = 1.0f;
    Envelope env_;
    VoiceFilter filt_;
    bool     lastFilterOn_ = true;
    // Binaural phase field: two first-order all-passes per ear, corners drifting apart
    Drifter  phaseDrift_;
    bool     phaseOn_ = false;
    float    apC_[2][2] = {}, apX_[2][2] = {}, apY_[2][2] = {};
    // Doppler from the breathing distance
    float    prevDist_ = 0.0f;
    double   dopplerMul_ = 1.0;
    // Strike: a Karplus-Strong loop excited at note-on, on the near plane
    static constexpr int kStrikeMax = 4096;
    float    ks_[kStrikeMax] = {};
    int      ksLen_ = 0, ksPos_ = 0, ksLeft_ = 0, ksT_ = 0, ksTotal_ = 0;
    float    ksG_ = 0.0f, ksLpC_ = 0.5f, ksLp_ = 0.0f, ksApK_ = 0.0f, ksApX_ = 0.0f, ksApY_ = 0.0f, ksAmp_ = 0.0f;
    float    ksGainL_ = 0.7f, ksGainR_ = 0.7f;
    bool     ksOn_ = false;
    void     strikeStart(double hz, const VoiceParams& p);
    inline float strikeTick();
    Svf      airL_, airR_;
    Drifter  filterDrift_, airDrift_, panCenter_, breath_, rateWander_;
    Drifter  zDriftX_, zDriftY_;
    ZBiquad  zbL_[kZSections], zbR_[kZSections];
    ZModal   zModal_;                  // the same frame read as a bank of ringing modes
    // Series of biquads, or a parallel bank of resonators: one call so the two paths cannot
    // drift apart at the two places the filter is applied.
    inline void zRun(float& l, float& r)
    {
        if (zModeCur_ == 3) { l = zModal_.tick(0, l); r = zModal_.tick(1, r); return; }
        for (int k = 0; k < zUsed_; ++k) { l = zbL_[k].tick(l); r = zbR_[k].tick(r); }
    }
    int      zUsed_ = 0;
    float    zNorm_ = 1.0f;
    float    fmHpXL_ = 0.0f, fmHpXR_ = 0.0f, fmHpYL_ = 0.0f, fmHpYR_ = 0.0f, fmHpCoef_ = 0.9987f;   // DC blocker for feedback FM
    float    zWet_ = 0.0f, zDry_ = 1.0f;
    int      zModeCur_ = 0;
    Rng      rng_;
    double   sr_ = 48000.0;
    double   freq_ = 220.0, freqTarget_ = 220.0;
    double   portaFrom_ = 0.0; float portaLeft_ = 0.0f, portaSeconds_ = 0.0f, portaGravity_ = 0.0f;
    Resonator ghostL_[6], ghostR_[6];
    float    ghostGain_ = 0.0f;
    float    velocity_ = 1.0f;
    float    press_ = 0.0f, pressTarget_ = 0.0f;     // per-note expression, smoothed at control rate
    float    slide_ = 0.0f, slideTarget_ = 0.0f;
    float    bend_ = 0.0f, bendTarget_ = 0.0f;
    float    centre_ = 0.0f;     // pan centre after drift and Source 1's Pan, for the stage picture
    float    distance_ = 0.0f;   // the plane the note was placed on
    float    layer_ = 0.5f;      // the role this note took: foundation 0, body .25, colour .5, air .75, shadow 1
    float    attackMul_ = 1.0f;  // what Vel to Attack made of the written attack, fixed at note-on
    float    rootComp_ = 1.0f;   // the frequency this voice keeps across a root change
    float    distEff_ = 0.0f;    // distance after breathing, refreshed at control rate
    float    gNear_ = 1.0f, gFar_ = 0.0f, gLevel_ = 1.0f;
    float    airGain_ = 0.0f;
    float    bloomT_ = 0.0f;     // seconds since note start on the movement clock, for Bloom
    float    slotT_  = 0.0f;     // the same on real time, for each slot's entrance (Freeze stops the first, not the second)
    float    slotGain_[kSlots] = { 1.0f, 1.0f, 1.0f, 1.0f };   // each slot's own envelope, at control rate
    // Sustain Loop entrances: how far each slot's clock was set back when the note was let go, so
    // the shape carries on from where it was held; where each shape was read last; and whether the
    // note was down in the last control block.
    float    slotShift_[kSlots] = {};
    float    slotEnvT_[kSlots] = { -1.0f, -1.0f, -1.0f, -1.0f };
    bool     slotHeld_ = false;
    // The slot roles: where the note stands, and each slot's gain from its role, faded at control
    // rate. All ones for a preset whose slots have no role, which is every preset before 13.09.2026.
    bool     placeLowest_ = true, placeHighest_ = true;
    float    roleGain_[kSlots] = { 1.0f, 1.0f, 1.0f, 1.0f };
    static float roleTarget(SlotRole r, bool lowest, bool highest)
    {
        switch (r) {
        case SlotRole::Lowest:  return lowest ? 1.0f : 0.0f;
        case SlotRole::Highest: return highest ? 1.0f : 0.0f;
        case SlotRole::Inner:   return (!lowest && !highest) ? 1.0f : 0.0f;
        default: return 1.0f;
        }
    }
    // A near event's shape (see setNearShape), the low lift's one-pole per ear, its place in the
    // field as an equal-power pair, and the plane it is travelling to.
    float    releaseMul_ = 1.0f, cutoffMul_ = 1.0f, proxDb_ = 0.0f, panOffset_ = 0.0f;
    float    nearGain_ = 1.0f;   // the event's Gain (fore_gain), folded into the pan pair at note-on
    float    proxAmt_ = 0.0f, proxLoCoef_ = 0.0f, proxHiCoef_ = 0.0f;
    float    proxLoL_ = 0.0f, proxLoR_ = 0.0f, proxHiL_ = 0.0f, proxHiR_ = 0.0f;
    float    panL_ = 1.0f, panR_ = 1.0f;
    float    distTarget_ = 0.0f, distSeconds_ = 0.0f;
    // Control-rate caches: the spectral shape only changes when its parameters do.
    float    tiltCache_[kMaxPartials + 1] = {};
    float    cachedTilt_ = -1.0f, cachedOddEven_ = -9.0f;
    int      cachedPartials_ = -1;
    double   stretchCache_[kMaxPartials] = {};
    float    cachedB_ = -1.0f;
    float    itdBufL_[kItdBuffer] = {}, itdBufR_[kItdBuffer] = {};
    int      itdW_ = 0;
    float    itdL_ = 0.0f, itdR_ = 0.0f, itdLTarget_ = 0.0f, itdRTarget_ = 0.0f;
    float    shadowL_ = 0.0f, shadowR_ = 0.0f, shadowCoefL_ = 1.0f, shadowCoefR_ = 1.0f;   // head shadow on the far ear
    // The binaural mode's head shadow is a one-pole/one-zero rather than a one-pole: the
    // spherical-head model (Brown and Duda 1998), whose zero moves with the angle.
    bool     sphereShadow_ = false;
    float    sphB0_[2] = { 1.0f, 1.0f }, sphB1_[2] = {}, sphA1_[2] = {};
    float    sphX1_[2] = {}, sphY1_[2] = {};
    // Externalisation (Brown and Duda's structural model, the parts that need no measured data):
    // the pinna's notch, whose frequency moves with the source's angle, and the shoulder echo.
    Svf      pinnaL_, pinnaR_;
    float    extAmt_ = 0.0f;
    int      shoulder_ = 0;
    // Height: the notch above is shared with externalisation; the 8 kHz band is its own.
    Svf      skyL_, skyR_;
    // Both of these depend on the sample rate and nothing else, and were being recomputed for
    // every voice every 64 samples -- an exp and a tan each. Once, in prepare().
    Svf      skyProto_;
    float    ildCoefConst_ = 0.0f;
    float    pinnaAmt_ = 0.0f, skyGain_ = 0.0f;
    // Near field: a low shelf on each ear, cut on the far one and lifted on the near one.
    float    ildAmt_ = 0.0f, ildCoef_ = 0.0f, ildL_ = 0.0f, ildR_ = 0.0f, ildLpL_ = 0.0f, ildLpR_ = 0.0f;
    // Partial Spread: per-partial left/right weights (equal power, so the sum of each pair is
    // exactly what the mono path gives), and the slow turn of their pattern.
    float    spreadAmt_ = 0.0f;
    float    spreadSum_[kMaxPartials] = {}, spreadDif_[kMaxPartials] = {};
    double   spreadPhase_ = 0.0;
    int      note_ = -1;
    int      owner_ = 0;
    unsigned strikes_ = 0;   // how often this voice has struck
    int      lastUnison_ = 0;
};

} // namespace ambient
