/**
 * @file Voice.h
 * @brief One cluster voice: `unison` strands, each an additive bank of
 *        up to 32 harmonic partials with individually drifting amplitudes and a slowly
 *        drifting pitch, plus a filtered-noise "air" layer.
 *
 * Alias-free by construction.
 *
 * Spatial model (after Robert Rich): every voice sits on a plane between the
 * listener's ear (distance 0: dry, bright, close) and the infinite background
 * (distance 1: darker, softer, sent to the far reverb). The voice's centre pan
 * is rendered with a true interaural time difference, not only with gain.
 *
 * The engine owns a pool of these (Engine.h), fills a VoiceParams from the parameter table once
 * per block and hands it to every sounding voice's render(), which adds into the near and far
 * buses. Inside, the work splits in two: control() runs once per kControlBlock samples and turns
 * the parameters into coefficients (the partials' rotations and amplitude targets, the filters,
 * the head model, the plane), and the per-sample loop in render() only turns phasors, sums, filters
 * and delays -- the hot loops of that sum are in Simd.h. Everything a voice allocates is inside
 * the object, so the pool is one allocation and the audio thread never asks for memory.
 */
#pragma once
#include "Dsp.h"
#include "Modulation.h"
#include "Sources.h"
#include "Filter.h"
#include "ZPlane.h"
#include <cstdint>
#include <vector>

namespace ambient {

constexpr int kMaxPartials  = 32;   ///< partials per strand: the bank's width, and the Partials knob's ceiling
constexpr int kMaxStrands   = 6;    ///< strands per voice: the Strands knob's ceiling
constexpr int kControlBlock = 64;   ///< samples between control-rate updates (1.3 ms at 48 kHz)
constexpr int kItdBuffer    = 256;  ///< >= 0.7 ms at 192 kHz

/**
 * @brief Everything a voice reads from the instrument: the parameter table translated into the
 *        voice's own terms, filled by the engine once per block and shared by every voice.
 *
 * Plain data, no methods. The defaults are the table's defaults; a field the engine does not
 * write keeps them. Pointers (the slots' tables, clips and envelope shapes) stay valid for the
 * block.
 */
struct VoiceParams {
    float level = 1.0f;         ///< level of the partial bank (Source 1)
    int   partials = 16;        ///< how many harmonics each strand renders, 1 .. kMaxPartials
    float tilt = 1.2f,         ///< @brief spectral tilt: partial h at h^-tilt
          brightness = 0.7f,   ///< @brief 0 .. 1, where the spectrum's cosine window closes (1 + 31 b^2 harmonics)
          oddEven = 0.0f,      ///< @brief -1 .. 1: negative thins the odd partials, positive the even
          inharmonic = 0.0f;   ///< 0 .. 1, stretches the partials as a stiff string does (B = 0.02 i^2)
    /// Match: partials placed on the degrees of the current scale rather than on the harmonic
    /// series (Sethares, the other direction of the timbre scale). The engine fills the table;
    /// at 0 the voice never reads it and its frequencies are what they always were, to the bit.
    float match = 0.0f;         ///< 0 .. 1, how far the partials are pulled onto the scale
    float partialRatio[kMaxPartials] = {};   ///< f_h / f0 with Match blended in, valid when match > 0
    float shimmer = 0.4f,        ///< @brief 0 .. 1, how far each partial's amplitude wanders on its own
          shimmerRate = 0.15f;   ///< Hz, how fast it wanders
    int   unison = 3;           ///< strands, 1 .. kMaxStrands
    float detune = 8.0f,       ///< @brief cents between the outermost strands
          drift = 4.0f,        ///< @brief cents of slow pitch drift per strand
          driftRate = 0.08f,   ///< @brief Hz, the drift's rate (also the base rate of the filter, air and pan drifts)
          spread = 0.7f;       ///< 0 .. 1, how far the strands fan out around the centre pan
    /// Two ceilings on the beating the strands make (R4.6). Both do nothing at zero.
    float lowDetune = 0.0f;     ///< how much the detuning thins out towards the bottom of the range
    float beatCeiling = 0.0f;   ///< Hz: the fastest beat a strand may make, halved under 150 Hz
    float velAttack = 0.0f;     ///< velocity to attack time: + lets a quiet note enter slower
    float bloom = 0.0f,        ///< @brief 0 .. 1, how closed the spectrum starts
          bloomTime = 60.0f;   ///< spectrum opens from brightness*(1-bloom) to brightness over bloomTime
    int   stack = 0;            ///< index into kStackRatios: strands at pure ratios (0 = classic detuned unison)
    float rateWander = 0.3f;    ///< drift/shimmer/breath rates wander by up to +-1 octave on a 100 s curve
    float air = 0.15f,       ///< @brief level of the filtered-noise layer, 0 = off
          airColor = 3.0f,   ///< @brief the band's centre as a multiple of the fundamental
          airQ = 10.0f;      ///< the band's Q, 1 .. 40 (x8 in Ghost mode)
    float attack = 6.0f,     ///< @brief seconds
          decay = 4.0f,      ///< @brief seconds
          sustain = 0.8f,    ///< @brief 0 .. 1
          release = 12.0f;   ///< seconds; the amplitude envelope's four stages
    float cutoff = 2500.0f,     ///< @brief Hz at note 60 with nothing else moving it
          resonance = 0.15f,    ///< @brief 0 .. 1, the meaning per model (Filter.h)
          filterEnv = 0.3f,     ///< @brief octaves the amplitude envelope opens the cutoff by, times four
          filterDrift = 0.3f,   ///< @brief octaves of slow cutoff wander, times two
          keyTrack = 0.5f;      ///< octaves of cutoff per octave of pitch
    int   filterModel = 1;      ///< FilterModel (Filter.h); 1 = the 12 dB state-variable low pass
    float filterDrive = 0.0f;   ///< 0 .. 1, saturation ahead of the filter
    float fold = 0.0f;          ///< wavefolder after the filters, 0 = off (see Voice.cpp)
    /// Rich refinements: the binaural phase field, the breathing doppler, the pitch tide (from the
    /// engine, already a multiplier), and the strike layer
    float phaseWidth = 0.0f,   ///< @brief 0 .. 1, how far the two ears' all-pass corners drift apart (0 = off)
          phaseRate = 0.03f,   ///< @brief Hz, the phase field's drift rate
          doppler = 0.0f;      ///< 0 .. 1, how much the breathing distance bends the pitch
    /// Expression: how far this voice's own pressure, slide and bend reach
    float pressDistance = 0.0f,   ///< @brief how far full pressure pulls the note towards the ear
          pressBright = 0.0f,     ///< @brief how much full pressure adds to brightness
          pressLevel = 0.0f;      ///< how much full pressure adds to the level
    float slideCutoff = 0.0f,   ///< @brief octaves a full slide opens the cutoff
          slideZ = 0.0f;        ///< how far a full slide moves the z-plane point's X
    float externalise = 0.0f;   ///< pinna notch + shoulder reflection, for headphones
    /// Height. The ear hears up and down at the pinna: a notch between about six and ten kHz
    /// whose frequency rises with elevation (Hebrank and Wright 1974), and Blauert's directional
    /// band near 8 kHz that says "above". Two numbers, one per plane, so the background can be
    /// the sky while the foreground stays on the ground; a voice takes its height from where it
    /// stands between them. Zero is exactly the old flat field.
    float elevNear = 0.0f,   ///< @brief height of the near plane
          elevFar = 0.0f;    ///< -1 below .. 1 above
    /// Depth Law: the plane warped so that the KNOB is linear in heard distance rather than in
    /// the model's. Zahorik's pooled exponent for perceived against physical distance is about
    /// 0.54, so at 1 the plane is d^1.85: the far half of the knob then sounds as far again as
    /// the near half, instead of the near half doing most of the work.
    float depthLaw = 0.0f;      ///< 0 .. 1, how much of that warp is applied
    float nearIld = 0.0f;       ///< Near Field: the level difference a source within reach makes, 0 .. 1 (see partialSpread's line)
    bool  oneEuro = false;       ///< MPE Filter: the expression smoothing whose cutoff follows the distance left to travel
    float partialSpread = 0.0f;  ///< Partial Spread: the additive bank's partials spread across the stereo field one by one       // Near Field: the level difference a source within reach makes
    /// Headphones binaural mode: the pan becomes an azimuth, the head's yaw turns the field the
    /// other way, the interaural delay follows Woodworth's head and the shadow is at full
    /// strength whatever Time Width says. Off, everything below is exactly as it was.
    bool  binaural = false;     ///< the binaural mode's switch
    float headYawDeg = 0.0f;    ///< the head's yaw in degrees, from the tracker or OSC (binaural mode)
    float sympathy = 0.0f;      ///< how much of the other voices this one hears, through its own filter
    float pitchMul = 1.0f;      ///< the tide's multiplier on every frequency, 1 exactly when off
    float strikeLevel = 0.0f,   ///< @brief level of the Karplus-Strong strike at note-on, 0 = off
          strikeDecay = 0.4f,   ///< @brief seconds the strike rings
          strikeDamp = 0.5f;    ///< 0 .. 1, how fast the strike's highs die
    int   strikeType = 0;       ///< String, Wood, Metal
    bool  strikeBrain = false;  ///< the brain's notes strike too
    bool  filterOn = true;      ///< the voice filter can be switched out; z-plane Replace also bypasses it
    bool  filterParallel = false;   ///< both filters on: z-plane after the filter (false) or beside it (true)
    /// Z-plane filter (ZPlane.h): 0 off, 1 in series after the SVF, 2 instead of it
    int   zMode = 0,    ///< @brief 0 off, 1 Series, 2 Replace, 3 Modal
          zShape = 0;   ///< 0 .. kZShapes-1, which corner set
    float zDecay = 2.5f,   ///< @brief Modal: T60 of the lowest mode in seconds
          zDamp = 0.6f;    ///< Modal: T60 of the lowest mode, and how much shorter the high ones
    float zX = 0.5f,          ///< @brief the point's X, 0 .. 1
          zY = 0.5f,          ///< @brief the point's Y, 0 .. 1
          zZ = 0.0f,          ///< @brief the cube's third axis (Transform), 0 .. 1
          zRate = 0.05f,      ///< @brief Hz, how fast the point wanders
          zDepth = 0.5f,      ///< @brief how far it wanders, +-0.5 zDepth
          zRes = 0.5f,        ///< @brief 0 .. 1: bandwidths doubled at 0, quartered at 1
          zKeyTrack = 0.0f,   ///< @brief how much the frame follows the note (exponent on f / C4)
          zMix = 0.7f;        ///< wet share of the z-plane's output
    float panDrift = 0.4f,   ///< @brief how far the centre pan wanders on its own
          itd = 0.6f;        ///< Time Width: 0 .. 1 of the full 0.65 ms interaural delay and of the head shadow
    /// Rich's foreground/background carving inside the voice (see concept.md):
    float presence = 0.0f;      ///< dB bell at 2-5 kHz, full on the near plane, gone on the far plane
    /// The production guide's depth model (25.09.2026), three more cues hung on the one distance:
    /// Range is the level a source loses between the ear and the horizon, in dB, so that the
    /// background really is 20 to 36 dB under the foreground instead of 6; Gap is the pre-delay a
    /// source at the ear gets before the far reverb, shrinking to nothing on the horizon, which is
    /// what tells the ear whether a sound stands in front of a room or inside it; Near Width is
    /// how wide the strands fan out at the ear as a share of their Spread, full on the horizon,
    /// so that the near plane is a place and the far plane a surround.
    float depthRange = 20.0f,     ///< @brief dB lost between the ear and the horizon
          depthPreDelay = 40.0f,  ///< @brief ms of far-send gap at the ear, none on the horizon
          depthWidth = 0.35f;     ///< the strands' width at the ear, 0 .. 1 of Spread
    float breath = 0.0f,        ///< @brief 0 .. 1, how far the plane breathes
          breathRate = 0.03f;   ///< slow wandering of the distance itself (+-0.35 at 1)
    float lowCut = 0.0f;        ///< Hz; partials below fall 12 dB/oct, keeping the pads out of the sub's register
    float fmAmount = 0.0f;      ///< phase modulation of every partial (h times the deviation) by the `fm` signal given to render()
    bool  freeze = false;       ///< hold the spectrum still: shimmer, pitch drift, breath and bloom stop moving
    bool  airGhost = false;     ///< Air through six resonators on the note's harmonics 1 2 3 5 7 9 instead of one band
    double rootHz = 130.81;     ///< the brain's root, for the portamento's consonance gravity
    /// Coherence offsets (from the engine's Kuramoto bank), added on top of the parameters
    float cohBrightness = 0.0f,   ///< @brief added to brightness
          cohPan = 0.0f,          ///< @brief added to the centre pan
          cohZ = 0.0f;            ///< added to the z-plane point's X and taken from its Y
    /// The four source slots (slot[0] = Source 1: Additive means the strand bank above, any other
    /// type mutes the bank and renders in the slot) and the data they may need; pointers stay
    /// valid for the block. Each slot has its own clip.
    SlotParams       slot[kSlots];              ///< the four slots' settings
    const Wavetable* userTable = nullptr;       ///< the loaded user wavetable as spectra (Harmonic type), or nullptr
    const CycleTable* userCycles = nullptr;   ///< the same file as single cycles, for the Wavetable type
    const Texture*   texture[kSlots] = {};      ///< each slot's clip, or nullptr
    /// The preset's six envelope shapes, for a slot that borrows one as its entrance. The same
    /// shapes the modulation matrix reads, with the same Mode and Time; the difference is that here
    /// they run from the note rather than from the phrase. Null for a shape that is not set.
    const ModEnv*     envShape[kNumModEnvs] = {};   ///< the six shapes
    const ModEnvSpec* envSpec[kNumModEnvs] = {};    ///< their Mode, Time and Depth
    /// Each slot's own shape and its settings, for a slot whose Env is Own: read from the note in
    /// the same way, but as a level from 0 to 1, and without using up one of the six above.
    const ModEnv*     srcEnvShape[kSlots] = {};   ///< each slot's own shape
    const ModEnvSpec* srcEnvSpec[kSlots] = {};    ///< its Mode, Time and Depth
};

/**
 * @brief One voice of the pool: the strands, the source slots, the filters and the whole spatial
 *        chain from the plane to the two ears.
 *
 * prepare() once from the message thread; everything else on the audio thread. The engine calls
 * noteOn/noteOff, the expression setters and render(); the display getters are read from the
 * message thread without synchronisation and are written so a torn read costs a pixel, not a
 * crash.
 */
class Voice {
public:
    /**
     * @brief Seeds the voice's dice, sets every sample-rate-dependent constant and puts the strands, drifters
     *        and filters in their starting state.
     *
     * Message thread, once per instance and again at a sample-rate change.
     *
     * @param sampleRate  in Hz
     * @param seed        the voice's own random stream (each voice of the pool gets a different one)
     */
    void prepare(double sampleRate, uint64_t seed);
    /**
     * @brief Starts a note, or retriggers a sounding one.
     *
     * distance 0..1 = plane (see above); fixed for the life of the note.
     * allowStrike false: this note does not strike even where the section says it would. The
     * engine decides it, because whether a conductor's note strikes is a property of the piece
     * (its chance, its cascade), not of the voice that happens to be free.
     * `ageSeconds`: treat the note as having been sounding this long already. Only a cluster
     * inherited at a preset change passes anything but zero -- see Engine::startNote.
     *
     * A fresh start (the envelope was idle) draws new random phases for every partial and resets
     * the filters and delays; a retrigger keeps them. The role (layer()) is read off the register
     * unless the owner is the second conductor or a near event.
     *
     * @param note         MIDI note number, for key tracking and the register roles
     * @param freqHz       the note's frequency in Hz (already tuned by the engine)
     * @param velocity     0 .. 1; the level is 0.3 + 0.7 velocity, and Vel to Attack reads it too
     * @param owner        who plays it: 0 keys, 1 the brain, 2 the second conductor, 3 a near event
     * @param distance     the plane, 0 at the ear .. 1 on the horizon
     * @param p            the parameters of this block
     * @param allowStrike  whether the strike may fire (see above)
     * @param ageSeconds   how long the note has notionally been sounding (see above)
     */
    void noteOn(int note, double freqHz, float velocity, int owner, float distance, const VoiceParams& p,
                bool allowStrike = true, float ageSeconds = 0.0f);
    /** @brief Lets the note go: the amplitude envelope enters its release; a strike rings on. */
    void noteOff();
    /** @brief Silence at once, the strike included; the note is given back. */
    void kill();

    /**
     * @brief How many times this voice has struck since it was prepared: the engine sums them, the self
     *        test counts them, and it is the only honest answer to "does it ever strike?".
     * @return the count
     */
    unsigned strikes() const { return strikes_; }
    /** @return whether the amplitude envelope is running (attack to release) */
    bool isActive() const    { return env_.isActive(); }
    /**
     * @brief Whether the strike is still ringing on its own physics.
     *
     * A struck string rings on its own physics, not on the amplitude envelope: a short release
     * (or a sustain of nought and a short decay) used to end the block loop mid-swing and cut the
     * strike to zero, which is a click. The engine renders a voice while EITHER is still going;
     * the note itself is given back as soon as the envelope is done, so nothing waits for a tail.
     *
     * @return true while the Karplus-Strong loop has not run its course
     */
    bool isStriking() const  { return ksOn_; }
    /** @return whether the envelope is in its release stage */
    bool isReleasing() const { return env_.isReleasing(); }
    /** @return the amplitude envelope's current level, 0 .. 1 */
    float level() const      { return env_.level(); }
    /** @return the MIDI note sounding, or -1 once the envelope is done */
    int  note() const        { return note_; }
    /** @return who plays this note (see noteOn) */
    int  owner() const       { return owner_; }
    /** @return where the voice is right now (breath included) */
    float distance() const   { return distEff_; }
    /** @return the role it took, for the matrix source Layer */
    float layer() const      { return layer_; }
    /**
     * @brief R4.7: what the root moved under a voice that was already sounding, so it can go on sounding
     *        where it was. 1 for every note that began after the move.
     * @return the frequency multiplier the engine set with setRootComp
     */
    float rootComp() const      { return rootComp_; }
    /**
     * @brief Sets the root compensation (see rootComp()).
     * @param m  the multiplier that keeps this voice's frequency where it was across a root change
     */
    void  setRootComp(float m)  { rootComp_ = m; }
    /** @return the frequency the bank is at right now in Hz (glides included) */
    double frequency() const { return freq_; }
    /**
     * @brief Retune a sounding voice (tuning purity/drift): glides in the log domain, ~1 s time constant.
     * @param hz  the frequency to arrive at
     */
    void setTargetFrequency(double hz) { freqTarget_ = hz; }
    /**
     * @brief Per-note expression. Pressure and slide are 0..1, bend is in semitones; all three are
     *        smoothed inside the voice, so a controller sending steps never steps the sound.
     * @param v  pressure 0 .. 1 (clamped)
     */
    void setPressure(float v) { pressTarget_ = clampv(v, 0.0f, 1.0f); }
    /**
     * @brief The key's sideways slide (see setPressure).
     * @param v  0 .. 1 (clamped)
     */
    void setSlide(float v)    { slideTarget_ = clampv(v, 0.0f, 1.0f); }
    /**
     * @brief The note's own pitch bend (see setPressure).
     * @param semitones  signed, unclamped; the engine has applied the bend range
     */
    void setBend(float semitones) { bendTarget_ = semitones; }
    /** @return the smoothed pressure, 0 .. 1 */
    float pressure() const { return press_; }
    /** @return the smoothed slide, 0 .. 1 */
    float slide() const    { return slide_; }
    /**
     * @brief Portamento: start at fromHz and slide to the note's frequency over `seconds`, slowing near
     *        consonant ratios to the root by `gravity` (0 = an even log-domain glide).
     *
     * Call right after noteOn. Does nothing for a non-positive start or duration.
     *
     * @param fromHz   where the slide starts (the previous key's frequency)
     * @param seconds  the nominal duration of an even slide
     * @param gravity  0 .. 1, the pull towards the just ratios (see Voice.cpp for the magnet)
     */
    void glideFrom(double fromHz, float seconds, float gravity);
    /** @return whether a portamento is still under way */
    bool gliding() const { return portaLeft_ > 0.0f; }
    /**
     * @brief A sounding note slides to another (a near Phrase's second degree): the note and its
     *        frequency change, and the slide runs on the portamento with its gravity.
     *
     * Ignored when the voice is not sounding.
     *
     * @param note     the new MIDI note
     * @param hz       its frequency
     * @param seconds  the slide's duration, at least 50 ms
     * @param gravity  0 .. 1, as glideFrom
     */
    void glideTo(int note, double hz, float seconds, float gravity);
    /**
     * @brief The plane moves: to `distance` over `seconds` -- a near event arriving out of the far plane
     *        or leaving into it. Everything that hangs on the distance follows, as it does for Breath.
     * @param distance  where to go, 0 .. 1 (clamped)
     * @param seconds   how long the (exponential) approach takes, at least 10 ms
     */
    void moveTo(float distance, float seconds) { distTarget_ = clampv(distance, 0.0f, 1.0f); distSeconds_ = std::max(seconds, 0.01f); }
    /**
     * @brief Where this note stands in what its owner is sounding, for the slot roles (SlotRole): set by
     *        the engine before noteOn, and again whenever the cluster changes.
     * @param lowest   this note is the lowest of its owner's cluster
     * @param highest  this note is the highest
     */
    void setPlace(bool lowest, bool highest) { placeLowest_ = lowest; placeHighest_ = highest; }
    /**
     * @brief A near event's shape, set before its noteOn: the gate as a share of the release, the bloom
     *        as a factor on the cutoff, the near field's low lift in decibels, and its place in the field.
     * @param releaseMul   factor on the release time, at least 0.05
     * @param cutoffMul    factor on the filter cutoff, 0.05 .. 8
     * @param proximityDb  the lift between 120 and 300 Hz on the near plane, in dB
     * @param pan          -1 .. 1, the event's place, added to the centre pan
     * @param gain         the event's Gain (fore_gain) as a linear factor, folded into the pan pair
     */
    void setNearShape(float releaseMul, float cutoffMul, float proximityDb, float pan, float gain = 1.0f)
    { releaseMul_ = std::max(releaseMul, 0.05f); cutoffMul_ = clampv(cutoffMul, 0.05f, 8.0f); proxDb_ = proximityDb; panOffset_ = clampv(pan, -1.0f, 1.0f); nearGain_ = std::max(gain, 0.0f); }
    uint64_t order = 0;      ///< allocation order for voice stealing

    /**
     * @brief For pictures only: the current amplitude of each partial of the first strand, exactly the
     *        numbers the oscillator is summing right now -- so a display drawn from them breathes with
     *        the shimmer and the drift instead of being a static illustration.
     *
     * Read without
     * synchronisation from the message thread; a torn float costs one wrong pixel.
     *
     * @param out       receives the amplitudes
     * @param maxCount  room in @p out
     * @return          how many were written (the strand's active partials, at most @p maxCount)
     */
    int displayPartials(float* out, int maxCount) const
    {
        const int n = maxCount < strands_[0].active ? maxCount : strands_[0].active;
        for (int i = 0; i < n; ++i) out[i] = strands_[0].amp[i];
        return n < 0 ? 0 : n;
    }
    /**
     * @brief The same for a slot's own bank (Additive / Wavetable in Source 1..3), and its grains.
     * @param slot      0 .. kSlots-1 (clamped)
     * @param out       receives the amplitudes
     * @param maxCount  room in @p out
     * @return          how many were written
     */
    int displaySlotPartials(int slot, float* out, int maxCount) const { return slots_[slot < 0 ? 0 : (slot >= kSlots ? kSlots - 1 : slot)].displayAmps(out, maxCount); }
    /**
     * @brief The grains a slot is sounding, for the picture (see SourceSlot::displayGrains).
     * @param slot      0 .. kSlots-1 (clamped)
     * @param out       receives one entry per live grain
     * @param maxCount  room in @p out
     * @param clipLen   the clip's length in samples, to give positions as 0 .. 1
     * @return          how many were written
     */
    int displayGrains(int slot, SourceSlot::GrainInfo* out, int maxCount, int clipLen) const { return slots_[slot < 0 ? 0 : (slot >= kSlots ? kSlots - 1 : slot)].displayGrains(out, maxCount, clipLen); }
    /**
     * @brief Where a slot's table is actually being read, Pos Drift included (see SourceSlot::displayPosition).
     * @param slot  0 .. kSlots-1 (clamped)
     * @return      the position 0 .. 1, or -1 until the slot has rendered a table
     */
    float displaySlotPosition(int slot) const { return slots_[slot < 0 ? 0 : (slot >= kSlots ? kSlots - 1 : slot)].displayPosition(); }
    /** @return where the voice's centre sits right now, -1..1 */
    float pan() const { return centre_; }
    /**
     * @brief For the envelope editor's playhead: the time each slot's entrance shape was read at in the
     *        last control block (seconds of the shape, -1 while the slot follows none) and the gain it
     *        gave. Read without synchronisation, as above.
     * @param k  the slot, 0 .. kSlots-1 (clamped)
     * @return   seconds into the shape, or -1
     */
    float slotEnvTime(int k) const { return slotEnvT_[k < 0 ? 0 : (k >= kSlots ? kSlots - 1 : k)]; }
    /**
     * @brief The gain a slot's entrance gave in the last control block (see slotEnvTime).
     * @param k  the slot, 0 .. kSlots-1 (clamped)
     * @return   0 .. 1
     */
    float slotGain(int k) const    { return slotGain_[k < 0 ? 0 : (k >= kSlots ? kSlots - 1 : k)]; }

    /**
     * @brief Adds `n` samples into the near (dry plane) and far (reverb send) buses.
     *
     * `fm` (n samples,
     * may be null) phase-modulates the partials when p.fmAmount > 0 (the feedback loop).
     * `couple` (n samples, may be null) is the previous block's foreground, mixed into this
     * voice's own filter input when p.sympathy > 0 -- strings on a shared soundboard.
     *
     * Runs control() every kControlBlock samples and stops early once neither the envelope nor
     * the strike is going; guards the z-plane state at the end of the block.
     *
     * @param nearL   left of the near bus, added to
     * @param nearR   right of the near bus, added to
     * @param farL    left of the far bus, added to
     * @param farR    right of the far bus, added to
     * @param n       samples
     * @param p       the parameters of this block
     * @param fm      the feedback signal, n samples or nullptr
     * @param couple  the previous block's foreground, n samples or nullptr
     */
    void render(float* nearL, float* nearR, float* farL, float* farR, int n, const VoiceParams& p,
                const float* fm = nullptr, const float* couple = nullptr);

private:
    /**
     * @brief One detuned copy of the spectrum: a bank of kMaxPartials rotating phasors and its place in the field.
     */
    struct Strand {
        /// Every partial is a rotating phasor (cos, sin) turned by its own per-sample rotation;
        /// independent across partials, so the loop pipelines and vectorises. Phasors are
        /// renormalised once per control block.
        float  pc[kMaxPartials] = {},   ///< @brief cos of each partial's phasor
               ps[kMaxPartials] = {};   ///< sin of each partial's phasor (the sample is amp * ps)
        float  rc[kMaxPartials] = {},   ///< @brief cos of each partial's per-sample rotation
               rs[kMaxPartials] = {};   ///< sin of each partial's per-sample rotation
        float  amp[kMaxPartials] = {};       ///< each partial's amplitude now, ramped towards the block's target
        float  ampStep[kMaxPartials] = {};   ///< the per-sample increment that reaches the target at the block's end
        Drifter shimmer[kMaxPartials];       ///< each partial's own amplitude wander
        Drifter pitch;                       ///< the strand's slow pitch drift
        float  gainL = 0.7f,   ///< @brief the strand's left gain (equal power from its pan) times the norm
               gainR = 0.7f;   ///< the strand's right gain
        /// Partial Spread renders this strand as a stereo pair instead of one sum, and a pair
        /// cannot be panned by multiplying each side with its own gain: that does not move the
        /// field, it deletes whatever leans the wrong way. The strand's place is folded into the
        /// partials' own weights instead (see control()), so what is left here is one gain.
        float  wL[kMaxPartials] = {},   ///< @brief each partial's left weight with the strand's pan composed in
               wR[kMaxPartials] = {};   ///< each partial's right weight
        float  spreadGain = 0.7f;            ///< the one gain of the spread pair: norm / sqrt 2
        int    active = 0;                   ///< partials with any amplitude left, the loop's length
    };
    /**
     * @brief The control-rate half of render(): parameters into coefficients, once per kControlBlock samples.
     *
     * Envelope times, the plane's approach and breath, the portamento and retune glides, the
     * rate wander, Partial Spread's weights, the expression smoothing, the doppler, the presence
     * bell, Bloom, each slot's entrance and role, the strands' rotations and amplitude targets,
     * the interaural delay and head shadow, the near field, externalisation and height, the
     * voice filter, the phase field, the z-plane frame and the air. Everything that costs a
     * transcendental lives here, so the per-sample loop has none.
     *
     * @param blockLen  samples this block covers (at most kControlBlock)
     * @param p         the parameters
     */
    void control(int blockLen, const VoiceParams& p);

    Strand   strands_[kMaxStrands];   ///< the strands; the first `unison` are rendered
    SourceSlot slots_[kSlots];        ///< the four source slots
    float    rateMul_ = 1.0f;         ///< Rate Wander's factor on every movement rate this block
    Envelope env_;                    ///< the amplitude envelope
    VoiceFilter filt_;                ///< the voice filter (one of the models in Filter.h)
    bool     lastFilterOn_ = true;    ///< filterOn as of the last block, to reset the filter when it is switched back in
    /// Binaural phase field: two first-order all-passes per ear, corners drifting apart
    Drifter  phaseDrift_;             ///< the one slow curve both ears' corners follow, in opposite directions
    bool     phaseOn_ = false;        ///< phaseWidth > 0 this block
    float    apC_[2][2] = {},   ///< @brief the all-pass coefficients, [ear][stage]
             apX_[2][2] = {},   ///< @brief each all-pass's previous input
             apY_[2][2] = {};   ///< each all-pass's previous output
    /// Doppler from the breathing distance
    float    prevDist_ = 0.0f;        ///< the effective distance of the previous block, for its velocity
    double   dopplerMul_ = 1.0;       ///< the pitch factor the velocity gives, 0.97 .. 1.03
    /// Strike: a Karplus-Strong loop excited at note-on, on the near plane
    static constexpr int kStrikeMax = 4096;   ///< the loop's longest period in samples
    float    ks_[kStrikeMax] = {};    ///< the delay loop
    int      ksLen_ = 0,     ///< @brief the loop's length: the period in samples
             ksPos_ = 0,     ///< @brief the read/write position in the loop
             ksLeft_ = 0,    ///< @brief noise samples still to inject
             ksT_ = 0,       ///< @brief samples since the strike began
             ksTotal_ = 0;   ///< samples the strike runs for before it is declared over
    float    ksG_ = 0.0f,     ///< @brief loop gain per pass, from the decay time
             ksLpC_ = 0.5f,   ///< @brief the damping low pass's coefficient
             ksLp_ = 0.0f,    ///< @brief the low pass's state
             ksApK_ = 0.0f,   ///< @brief the all-pass coefficient (Metal), 0 for none
             ksApX_ = 0.0f,   ///< @brief the all-pass's previous input
             ksApY_ = 0.0f,   ///< @brief the all-pass's previous output
             ksAmp_ = 0.0f;   ///< the excitation's amplitude
    float    ksGainL_ = 0.7f,   ///< @brief the strike's left gain, from the centre pan at note-on
             ksGainR_ = 0.7f;   ///< the strike's right gain
    bool     ksOn_ = false;           ///< the strike is running
    /**
     * @brief Starts the strike: fills the loop's parameters for the type and arms the noise burst.
     *
     * String rings at the note; Wood is two octaves up (clamped to 200 .. 2500 Hz), short and dull;
     * Metal puts an all-pass in the loop. The strike is placed at the voice's centre pan.
     *
     * @param hz  the note's frequency
     * @param p   the parameters, for level, type, decay and damp
     */
    void     strikeStart(double hz, const VoiceParams& p);
    /**
     * @brief One sample of the strike loop: inject noise while any is left, filter, feed back, count down.
     * @return the sample read from the loop
     */
    inline float strikeTick();
    Svf      airL_,   ///< @brief the Air band pass, left
             airR_;   ///< the Air band pass, right
    Drifter  filterDrift_,   ///< @brief the cutoff's slow wander
             airDrift_,      ///< @brief the Air band's wander
             panCenter_,     ///< @brief the centre pan's wander
             breath_,        ///< @brief the plane's breathing
             rateWander_;    ///< the very slow curve that scales every other rate
    Drifter  zDriftX_,   ///< @brief the z-plane point's wander in X
             zDriftY_;   ///< and in Y
    ZBiquad  zbL_[kZSections],   ///< @brief the z-plane cascade, left
             zbR_[kZSections];   ///< the z-plane cascade, right (same coefficients)
    ZModal   zModal_;                  ///< the same frame read as a bank of ringing modes
    /**
     * @brief Series of biquads, or a parallel bank of resonators: one call so the two paths cannot
     *        drift apart at the two places the filter is applied.
     * @param l  left sample, filtered in place
     * @param r  right sample, filtered in place
     */
    inline void zRun(float& l, float& r)
    {
        if (zModeCur_ == 3) { l = zModal_.tick(0, l); r = zModal_.tick(1, r); return; }
        for (int k = 0; k < zUsed_; ++k) { l = zbL_[k].tick(l); r = zbR_[k].tick(r); }
    }
    int      zUsed_ = 0;              ///< sections (or modes) in use this block
    float    zNorm_ = 1.0f;           ///< the cascade's make-up gain from zBuildCascade (1 for the modal bank)
    float    fmHpXL_ = 0.0f,        ///< @brief DC blocker, left: previous input
             fmHpXR_ = 0.0f,        ///< @brief DC blocker, right: previous input
             fmHpYL_ = 0.0f,        ///< @brief DC blocker, left: previous output
             fmHpYR_ = 0.0f,        ///< @brief DC blocker, right: previous output
             fmHpCoef_ = 0.9987f;   ///< DC blocker for feedback FM
    float    zWet_ = 0.0f,   ///< @brief the z-plane's share of the output (zMix)
             zDry_ = 1.0f;   ///< the unfiltered share
    int      zModeCur_ = 0;           ///< the z-plane mode this block: 0 off, 1 Series, 2 Replace, 3 Modal
    Rng      rng_;                    ///< the voice's own random stream (phases, noise, strike)
    double   sr_ = 48000.0;           ///< sample rate in Hz
    double   freq_ = 220.0,         ///< @brief the bank's frequency now, glides included
             freqTarget_ = 220.0;   ///< where a retune or portamento is heading
    double   portaFrom_ = 0.0;          ///< @brief where the portamento started
             float portaLeft_ = 0.0f,   ///< @brief seconds of the slide still to go, 0 = none
             portaSeconds_ = 0.0f,      ///< @brief the slide's nominal duration
             portaGravity_ = 0.0f;      ///< the pull towards the just ratios, 0 .. 1
    Resonator ghostL_[6],   ///< @brief Ghost mode's six resonators, left
              ghostR_[6];   ///< and right
    float    ghostGain_ = 0.0f;       ///< the Ghost noise's gain, 0 when the mode is off
    float    velocity_ = 1.0f;        ///< the note's level from its velocity, 0.3 .. 1
    float    press_ = 0.0f,         ///< @brief the smoothed pressure
             pressTarget_ = 0.0f;   ///< per-note expression, smoothed at control rate
    float    slide_ = 0.0f,         ///< @brief the smoothed slide
             slideTarget_ = 0.0f;   ///< what the controller last said
    float    bend_ = 0.0f,         ///< @brief the smoothed bend in semitones
             bendTarget_ = 0.0f;   ///< what the controller last said
    float    centre_ = 0.0f;     ///< pan centre after drift and Source 1's Pan, for the stage picture
    float    distance_ = 0.0f;   ///< the plane the note was placed on
    float    layer_ = 0.5f;      ///< the role this note took: foundation 0, body .25, colour .5, air .75, shadow 1
    float    attackMul_ = 1.0f;  ///< what Vel to Attack made of the written attack, fixed at note-on
    float    rootComp_ = 1.0f;   ///< the frequency this voice keeps across a root change
    float    distEff_ = 0.0f;    ///< distance after breathing, refreshed at control rate
    float    gNear_ = 1.0f,    ///< @brief the plane's share into the near bus, cos
             gFar_ = 0.0f,     ///< @brief the plane's share into the far bus, sin
             gLevel_ = 1.0f;   ///< the level the plane leaves, 1 at the ear, 0.5 on the horizon
    float    airGain_ = 0.0f;         ///< the Air band's gain, normalised so `air` reads as a level
    float    bloomT_ = 0.0f;     ///< seconds since note start on the movement clock, for Bloom
    float    slotT_  = 0.0f;     ///< the same on real time, for each slot's entrance (Freeze stops the first, not the second)
    float    slotGain_[kSlots] = { 1.0f, 1.0f, 1.0f, 1.0f };   ///< each slot's own envelope, at control rate
    /// Sustain Loop entrances: how far each slot's clock was set back when the note was let go, so
    /// the shape carries on from where it was held; where each shape was read last; and whether the
    /// note was down in the last control block.
    float    slotShift_[kSlots] = {};                          ///< the set-back per slot, in seconds
    float    slotEnvT_[kSlots] = { -1.0f, -1.0f, -1.0f, -1.0f };   ///< where each shape was read last, -1 for none
    bool     slotHeld_ = false;                                ///< the note was down in the last control block
    /// The slot roles: where the note stands, and each slot's gain from its role, faded at control
    /// rate. All ones for a preset whose slots have no role, which is every preset before 13.09.2026.
    bool     placeLowest_ = true,    ///< @brief this note is the lowest of its owner's cluster
             placeHighest_ = true;   ///< this note is the highest
    float    roleGain_[kSlots] = { 1.0f, 1.0f, 1.0f, 1.0f };   ///< each slot's gain from its role, faded over 1.5 s
    /**
     * @brief The gain a role asks for at a given place: 1 when the note is where the role says, else 0.
     * @param r        the slot's role
     * @param lowest   the note is the lowest of the cluster
     * @param highest  the note is the highest
     * @return         1 or 0 (always 1 for SlotRole::All)
     */
    static float roleTarget(SlotRole r, bool lowest, bool highest)
    {
        switch (r) {
        case SlotRole::Lowest:  return lowest ? 1.0f : 0.0f;
        case SlotRole::Highest: return highest ? 1.0f : 0.0f;
        case SlotRole::Inner:   return (!lowest && !highest) ? 1.0f : 0.0f;
        default: return 1.0f;
        }
    }
    /// A near event's shape (see setNearShape), the low lift's one-pole per ear, its place in the
    /// field as an equal-power pair, and the plane it is travelling to.
    float    releaseMul_ = 1.0f,   ///< @brief factor on the release time
             cutoffMul_ = 1.0f,    ///< @brief factor on the cutoff
             proxDb_ = 0.0f,       ///< @brief the near field's lift in dB
             panOffset_ = 0.0f;    ///< the event's pan, added to the centre
    float    nearGain_ = 1.0f;   ///< the event's Gain (fore_gain), folded into the pan pair at note-on
    float    proxAmt_ = 0.0f,      ///< @brief the lift as a linear gain minus one
             proxLoCoef_ = 0.0f,   ///< @brief one-pole coefficient at 120 Hz
             proxHiCoef_ = 0.0f;   ///< one-pole coefficient at 300 Hz
    float    proxLoL_ = 0.0f,   ///< @brief the 120 Hz one-pole's state, left
             proxLoR_ = 0.0f,   ///< @brief and right
             proxHiL_ = 0.0f,   ///< @brief the 300 Hz one-pole's state, left
             proxHiR_ = 0.0f;   ///< and right
    float    panL_ = 1.0f,   ///< @brief the event's left gain (sqrt 2 cos, times its Gain); 1 for any other note
             panR_ = 1.0f;   ///< the event's right gain
    float    distTarget_ = 0.0f,    ///< @brief the plane moveTo() is heading for
             distSeconds_ = 0.0f;   ///< how long the approach takes, 0 when none is under way
    /// Control-rate caches: the spectral shape only changes when its parameters do.
    float    tiltCache_[kMaxPartials + 1] = {};   ///< tilt and odd/even per harmonic, indexed 1 .. partials
    float    cachedTilt_ = -1.0f,      ///< @brief the tilt the cache was built for
             cachedOddEven_ = -9.0f;   ///< the odd/even the cache was built for
    int      cachedPartials_ = -1;                ///< the partial count the cache was built for
    double   stretchCache_[kMaxPartials] = {};    ///< the inharmonic stretch factor per harmonic
    float    cachedB_ = -1.0f;                    ///< the stiffness the stretch cache was built for
    float    itdBufL_[kItdBuffer] = {},   ///< @brief the interaural delay ring, left
             itdBufR_[kItdBuffer] = {};   ///< the interaural delay ring, right
    int      itdW_ = 0;                           ///< the rings' write position
    /// The far send's gap (Depth Gap): a source at the ear reaches the far reverb this much later
    /// than its direct sound, a source on the horizon at once -- the pre-delay as a cue of distance,
    /// per voice, ahead of the far bus. Heap rings sized in prepare() (80 ms at the rate), so a bank
    /// of engines on a test's stack stays the size it was.
    std::vector<float> farDlyL_,   ///< @brief the far send's delay ring, left
                       farDlyR_;   ///< the far send's delay ring, right
    int      farDlyMask_ = 0,      ///< @brief ring size - 1 (a power of two; 0 until prepare())
             farDlyW_ = 0;         ///< the rings' write position
    float    farDly_ = 0.0f,         ///< @brief the far send's delay now, in samples (glided per sample)
             farDlyTarget_ = 0.0f;   ///< where it is heading: Gap x (1 - distance) at the rate
    float    itdL_ = 0.0f,         ///< @brief the left delay now, in samples (smoothed)
             itdR_ = 0.0f,         ///< @brief the right delay now
             itdLTarget_ = 0.0f,   ///< @brief where the left delay is heading
             itdRTarget_ = 0.0f;   ///< where the right delay is heading
    float    shadowL_ = 0.0f,       ///< @brief the far ear's one-pole state, left
             shadowR_ = 0.0f,       ///< @brief and right
             shadowCoefL_ = 1.0f,   ///< @brief its coefficient, left (1 = no shadow)
             shadowCoefR_ = 1.0f;   ///< head shadow on the far ear
    /// The binaural mode's head shadow is a one-pole/one-zero rather than a one-pole: the
    /// spherical-head model (Brown and Duda 1998), whose zero moves with the angle.
    bool     sphereShadow_ = false;   ///< the spherical-head filter is in use (binaural mode)
    float    sphB0_[2] = { 1.0f, 1.0f },   ///< @brief the filter's input gain per ear
             sphB1_[2] = {},               ///< @brief its zero's coefficient per ear
             sphA1_[2] = {};               ///< its pole's coefficient per ear
    float    sphX1_[2] = {},   ///< @brief its previous input per ear
             sphY1_[2] = {};   ///< its previous output per ear
    /// Externalisation (Brown and Duda's structural model, the parts that need no measured data):
    /// the pinna's notch, whose frequency moves with the source's angle, and the shoulder echo.
    Svf      pinnaL_,   ///< @brief the pinna notch, left
             pinnaR_;   ///< the pinna notch, right
    float    extAmt_ = 0.0f;          ///< externalise, 0 .. 1
    int      shoulder_ = 0;           ///< the shoulder echo's delay in samples (about 0.26 ms)
    /// Height: the notch above is shared with externalisation; the 8 kHz band is its own.
    Svf      skyL_,   ///< @brief Blauert's 8 kHz band, left
             skyR_;   ///< and right
    /// Both of these depend on the sample rate and nothing else, and were being recomputed for
    /// every voice every 64 samples -- an exp and a tan each. Once, in prepare().
    Svf      skyProto_;               ///< the 8 kHz band's coefficients, copied into skyL_/skyR_ when needed
    float    ildCoefConst_ = 0.0f;    ///< the near field's 1 kHz one-pole coefficient
    float    pinnaAmt_ = 0.0f,   ///< @brief how much of the notch path is mixed in: the larger of externalise and |elevation|
             skyGain_ = 0.0f;    ///< the 8 kHz band's gain, signed by elevation (0 = flat)
    /// Near field: a low shelf on each ear, cut on the far one and lifted on the near one.
    float    ildAmt_ = 0.0f,    ///< @brief nearIld, 0 .. 1
             ildCoef_ = 0.0f,   ///< @brief the shelf's one-pole coefficient
             ildL_ = 0.0f,      ///< @brief the left ear's shelf amount (positive cuts, negative lifts)
             ildR_ = 0.0f,      ///< @brief the right ear's
             ildLpL_ = 0.0f,    ///< @brief the one-pole's state, left
             ildLpR_ = 0.0f;    ///< and right
    /// Partial Spread: per-partial left/right weights (equal power, so the sum of each pair is
    /// exactly what the mono path gives), and the slow turn of their pattern.
    float    spreadAmt_ = 0.0f;       ///< partialSpread, 0 .. 1
    float    spreadSum_[kMaxPartials] = {},   ///< @brief each partial's left plus right weight
             spreadDif_[kMaxPartials] = {};   ///< each partial's left minus right weight
    double   spreadPhase_ = 0.0;      ///< the pattern's turn, 0 .. 1, once every fifty seconds
    int      note_ = -1;              ///< the MIDI note, -1 when none
    int      owner_ = 0;              ///< who plays it (see noteOn)
    unsigned strikes_ = 0;   ///< how often this voice has struck
    int      lastUnison_ = 0;         ///< the strand count of the previous block, to silence strands that were switched off
};

} // namespace ambient
