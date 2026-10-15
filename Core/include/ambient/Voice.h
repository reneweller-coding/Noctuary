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
 * buses. Inside, the work splits in two: control() runs once per ControlBlock samples and turns
 * the parameters into coefficients (the partials' rotations and amplitude targets, the filters,
 * the head model, the plane), and the per-sample loop in render() only turns phasors, sums, filters
 * and delays -- the hot loops of that sum are in Simd.h. Everything a voice allocates is inside
 * the object, so the pool is one allocation and the audio thread never asks for memory.
 * @author Rene Weller
 */
#pragma once
#include "Dsp.h"
#include "Modulation.h"
#include "Sources.h"
#include "Filter.h"
#include "ZPlane.h"
#include <cstdint>
#include <vector>

namespace ambient
{

constexpr int MaxPartials = 32;    ///< partials per strand: the bank's width, and the Partials knob's ceiling
constexpr int MaxStrands = 6;      ///< strands per voice: the Strands knob's ceiling
constexpr int ControlBlock = 64;   ///< samples between control-rate updates (1.3 ms at 48 kHz)
constexpr int ItdBuffer = 256;     ///< >= 0.7 ms at 192 kHz

/**
 * @brief Everything a voice reads from the instrument: the parameter table translated into the
 *        voice's own terms, filled by the engine once per block and shared by every voice.
 *
 * Plain data, no methods. The defaults are the table's defaults; a field the engine does not
 * write keeps them. Pointers (the slots' tables, clips and envelope shapes) stay valid for the
 * block.
 */
struct VoiceParams
{
    float m_level = 1.0f;      ///< level of the partial bank (Source 1)
    int   m_partials = 16;     ///< how many harmonics each strand renders, 1 .. MaxPartials
    float m_tilt = 1.2f,       ///< @brief spectral tilt: partial h at h^-tilt
        m_brightness = 0.7f,   ///< @brief 0 .. 1, where the spectrum's cosine window closes (1 + 31 b^2 harmonics)
        m_oddEven = 0.0f,      ///< @brief -1 .. 1: negative thins the odd partials, positive the even
        m_inharmonic = 0.0f;   ///< 0 .. 1, stretches the partials as a stiff string does (B = 0.02 i^2)
    /// Match: partials placed on the degrees of the current scale rather than on the harmonic
    /// series (Sethares, the other direction of the timbre scale). The engine fills the table;
    /// at 0 the voice never reads it and its frequencies are what they always were, to the bit.
    float m_match = 0.0f;                     ///< 0 .. 1, how far the partials are pulled onto the scale
    float m_partialRatio[MaxPartials] = {};   ///< f_h / f0 with Match blended in, valid when match > 0
    float m_shimmer = 0.4f,                   ///< @brief 0 .. 1, how far each partial's amplitude wanders on its own
        m_shimmerRate = 0.15f;                ///< Hz, how fast it wanders
    int   m_unison = 3;                       ///< strands, 1 .. MaxStrands
    float m_detune = 8.0f,                    ///< @brief cents between the outermost strands
        m_drift = 4.0f,                       ///< @brief cents of slow pitch drift per strand
        m_driftRate = 0.08f,   ///< @brief Hz, the drift's rate (also the base rate of the filter, air and pan drifts)
        m_spread = 0.7f;       ///< 0 .. 1, how far the strands fan out around the centre pan
    /// Two ceilings on the beating the strands make (R4.6). Both do nothing at zero.
    float m_lowDetune = 0.0f;     ///< how much the detuning thins out towards the bottom of the range
    float m_beatCeiling = 0.0f;   ///< Hz: the fastest beat a strand may make, halved under 150 Hz
    float m_velAttack = 0.0f;     ///< velocity to attack time: + lets a quiet note enter slower
    float m_bloom = 0.0f,         ///< @brief 0 .. 1, how closed the spectrum starts
        m_bloomTime = 60.0f;      ///< spectrum opens from brightness*(1-bloom) to brightness over bloomTime
    int   m_stack = 0;            ///< index into StackRatios: strands at pure ratios (0 = classic detuned unison)
    float m_rateWander = 0.3f;    ///< drift/shimmer/breath rates wander by up to +-1 octave on a 100 s curve
    float m_air = 0.15f,          ///< @brief level of the filtered-noise layer, 0 = off
        m_airColor = 3.0f,        ///< @brief the band's centre as a multiple of the fundamental
        m_airQ = 10.0f;           ///< the band's Q, 1 .. 40 (x8 in Ghost mode)
    float m_attack = 6.0f,        ///< @brief seconds
        m_decay = 4.0f,           ///< @brief seconds
        m_sustain = 0.8f,         ///< @brief 0 .. 1
        m_release = 12.0f;        ///< seconds; the amplitude envelope's four stages
    float m_cutoff = 2500.0f,     ///< @brief Hz at note 60 with nothing else moving it
        m_resonance = 0.15f,      ///< @brief 0 .. 1, the meaning per model (Filter.h)
        m_filterEnv = 0.3f,       ///< @brief octaves the amplitude envelope opens the cutoff by, times four
        m_filterDrift = 0.3f,     ///< @brief octaves of slow cutoff wander, times two
        m_keyTrack = 0.5f;        ///< octaves of cutoff per octave of pitch
    int   m_filterModel = 1;      ///< FilterModel (Filter.h); 1 = the 12 dB state-variable low pass
    float m_filterDrive = 0.0f;   ///< 0 .. 1, saturation ahead of the filter
    float m_filterMorph = 0.0f;   ///< 0 .. 1, the SEM's response: low pass, notch, high pass
    float m_fold = 0.0f;          ///< wavefolder after the filters, 0 = off (see Voice.cpp)
    /// Rich refinements: the binaural phase field, the breathing doppler, the pitch tide (from the
    /// engine, already a multiplier), and the strike layer
    float m_phaseWidth = 0.0f,   ///< @brief 0 .. 1, how far the two ears' all-pass corners drift apart (0 = off)
        m_phaseRate = 0.03f,     ///< @brief Hz, the phase field's drift rate
        m_doppler = 0.0f;        ///< 0 .. 1, how much the breathing distance bends the pitch
    /// Expression: how far this voice's own pressure, slide and bend reach
    float m_pressDistance = 0.0f,   ///< @brief how far full pressure pulls the note towards the ear
        m_pressBright = 0.0f,       ///< @brief how much full pressure adds to brightness
        m_pressLevel = 0.0f;        ///< how much full pressure adds to the level
    float m_slideCutoff = 0.0f,     ///< @brief octaves a full slide opens the cutoff
        m_slideZ = 0.0f;            ///< how far a full slide moves the z-plane point's X
    float m_externalise = 0.0f;     ///< pinna notch + shoulder reflection, for headphones
    /// Height. The ear hears up and down at the pinna: a notch between about six and ten kHz
    /// whose frequency rises with elevation (Hebrank and Wright 1974), and Blauert's directional
    /// band near 8 kHz that says "above". Two numbers, one per plane, so the background can be
    /// the sky while the foreground stays on the ground; a voice takes its height from where it
    /// stands between them. Zero is exactly the old flat field.
    float m_elevNear = 0.0f,   ///< @brief height of the near plane
        m_elevFar = 0.0f;      ///< -1 below .. 1 above
    /// Depth Law: the plane warped so that the KNOB is linear in heard distance rather than in
    /// the model's. Zahorik's pooled exponent for perceived against physical distance is about
    /// 0.54, so at 1 the plane is d^1.85: the far half of the knob then sounds as far again as
    /// the near half, instead of the near half doing most of the work.
    float m_depthLaw = 0.0f;   ///< 0 .. 1, how much of that warp is applied
    float m_nearIld =
        0.0f;   ///< Near Field: the level difference a source within reach makes, 0 .. 1 (see partialSpread's line)
    bool m_oneEuro = false;   ///< MPE Filter: the expression smoothing whose cutoff follows the distance left to travel
    float m_partialSpread =
        0.0f;   ///< Partial Spread: the additive bank's partials spread across the stereo field one by one       // Near Field: the level difference a source within reach makes
    /// Headphones binaural mode: the pan becomes an azimuth, the head's yaw turns the field the
    /// other way, the interaural delay follows Woodworth's head and the shadow is at full
    /// strength whatever Time Width says. Off, everything below is exactly as it was.
    bool  m_binaural = false;        ///< the binaural mode's switch
    float m_headYawDeg = 0.0f;       ///< the head's yaw in degrees, from the tracker or OSC (binaural mode)
    float m_sympathy = 0.0f;         ///< how much of the other voices this one hears, through its own filter
    float m_pitchMul = 1.0f;         ///< the tide's multiplier on every frequency, 1 exactly when off
    float m_strikeLevel = 0.0f,      ///< @brief level of the Karplus-Strong strike at note-on, 0 = off
        m_strikeDecay = 0.4f,        ///< @brief seconds the strike rings
        m_strikeDamp = 0.5f;         ///< 0 .. 1, how fast the strike's highs die
    int  m_strikeType = 0;           ///< String, Wood, Metal
    bool m_strikeBrain = false;      ///< the brain's notes strike too
    bool m_filterOn = true;          ///< the voice filter can be switched out; z-plane Replace also bypasses it
    bool m_filterParallel = false;   ///< both filters on: z-plane after the filter (false) or beside it (true)
    /// Z-plane filter (ZPlane.h): 0 off, 1 in series after the SVF, 2 instead of it
    int m_zMode = 0,           ///< @brief 0 off, 1 Series, 2 Replace, 3 Modal
        m_zShape = 0;          ///< 0 .. ZShapes-1, which corner set
    float m_zDecay = 2.5f,     ///< @brief Modal: T60 of the lowest mode in seconds
        m_zDamp = 0.6f;        ///< Modal: T60 of the lowest mode, and how much shorter the high ones
    float m_zX = 0.5f,         ///< @brief the point's X, 0 .. 1
        m_zY = 0.5f,           ///< @brief the point's Y, 0 .. 1
        m_zZ = 0.0f,           ///< @brief the cube's third axis (Transform), 0 .. 1
        m_zRate = 0.05f,       ///< @brief Hz, how fast the point wanders
        m_zDepth = 0.5f,       ///< @brief how far it wanders, +-0.5 zDepth
        m_zRes = 0.5f,         ///< @brief 0 .. 1: bandwidths doubled at 0, quartered at 1
        m_zKeyTrack = 0.0f,    ///< @brief how much the frame follows the note (exponent on f / C4)
        m_zMix = 0.7f;         ///< wet share of the z-plane's output
    float m_panDrift = 0.4f,   ///< @brief how far the centre pan wanders on its own
        m_itd = 0.6f;          ///< Time Width: 0 .. 1 of the full 0.65 ms interaural delay and of the head shadow
    /// Rich's foreground/background carving inside the voice (see concept.md):
    float m_presence = 0.0f;   ///< dB bell at 2-5 kHz, full on the near plane, gone on the far plane
    /// The production guide's depth model (25.09.2026), three more cues hung on the one distance:
    /// Range is the level a source loses between the ear and the horizon, in dB, so that the
    /// background really is 20 to 36 dB under the foreground instead of 6; Gap is the pre-delay a
    /// source at the ear gets before the far reverb, shrinking to nothing on the horizon, which is
    /// what tells the ear whether a sound stands in front of a room or inside it; Near Width is
    /// how wide the strands fan out at the ear as a share of their Spread, full on the horizon,
    /// so that the near plane is a place and the far plane a surround.
    float m_depthRange = 20.0f,    ///< @brief dB lost between the ear and the horizon
        m_depthPreDelay = 40.0f,   ///< @brief ms of far-send gap at the ear, none on the horizon
        m_depthWidth = 0.35f;      ///< the strands' width at the ear, 0 .. 1 of Spread
    float m_breath = 0.0f,         ///< @brief 0 .. 1, how far the plane breathes
        m_breathRate = 0.03f;      ///< slow wandering of the distance itself (+-0.35 at 1)
    float m_lowCut = 0.0f;         ///< Hz; partials below fall 12 dB/oct, keeping the pads out of the sub's register
    float m_fmAmount =
        0.0f;   ///< phase modulation of every partial (h times the deviation) by the `fm` signal given to render()
    bool   m_freeze = false;     ///< hold the spectrum still: shimmer, pitch drift, breath and bloom stop moving
    bool   m_airGhost = false;   ///< Air through six resonators on the note's harmonics 1 2 3 5 7 9 instead of one band
    double m_rootHz = 130.81;    ///< the brain's root, for the portamento's consonance gravity
    /// Coherence offsets (from the engine's Kuramoto bank), added on top of the parameters
    float m_cohBrightness = 0.0f,   ///< @brief added to brightness
        m_cohPan = 0.0f,            ///< @brief added to the centre pan
        m_cohZ = 0.0f;              ///< added to the z-plane point's X and taken from its Y
    /// The four source slots (slot[0] = Source 1: Additive means the strand bank above, any other
    /// type mutes the bank and renders in the slot) and the data they may need; pointers stay
    /// valid for the block. Each slot has its own clip.
    SlotParams        m_slot[Slots];            ///< the four slots' settings
    const Wavetable  *m_userTable = nullptr;    ///< the loaded user wavetable as spectra (Harmonic type), or nullptr
    const CycleTable *m_userCycles = nullptr;   ///< the same file as single cycles, for the Wavetable type
    const Texture    *m_texture[Slots] = {};    ///< each slot's clip, or nullptr
    /// The preset's six envelope shapes, for a slot that borrows one as its entrance. The same
    /// shapes the modulation matrix reads, with the same Mode and Time; the difference is that here
    /// they run from the note rather than from the phrase. Null for a shape that is not set.
    const ModEnv     *m_envShape[NumModEnvs] = {};   ///< the six shapes
    const ModEnvSpec *m_envSpec[NumModEnvs] = {};    ///< their Mode, Time and Depth
    /// Each slot's own shape and its settings, for a slot whose Env is Own: read from the note in
    /// the same way, but as a level from 0 to 1, and without using up one of the six above.
    const ModEnv     *m_srcEnvShape[Slots] = {};   ///< each slot's own shape
    const ModEnvSpec *m_srcEnvSpec[Slots] = {};    ///< its Mode, Time and Depth
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
class Voice
{
public:
    /**
     * @brief Seeds the voice's dice, sets every sample-rate-dependent constant and puts the strands, drifters
     *        and filters in their starting state.
     *
     * Message thread, once per instance and again at a sample-rate change.
     * @param[in] sampleRate  in Hz
     * @param[in] seed        the voice's own random stream (each voice of the pool gets a different one)
     */
    void prepare( double sampleRate, uint64_t seed );
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
     * @param[in] note         MIDI note number, for key tracking and the register roles
     * @param[in] freqHz       the note's frequency in Hz (already tuned by the engine)
     * @param[in] velocity     0 .. 1; the level is 0.3 + 0.7 velocity, and Vel to Attack reads it too
     * @param[in] owner        who plays it: 0 keys, 1 the brain, 2 the second conductor, 3 a near event
     * @param[in] distance     the plane, 0 at the ear .. 1 on the horizon
     * @param[in] p            the parameters of this block
     * @param[in] allowStrike  whether the strike may fire (see above)
     * @param[in] ageSeconds   how long the note has notionally been sounding (see above)
     */
    void noteOn( int note, double freqHz, float velocity, int owner, float distance, const VoiceParams &p,
                 bool allowStrike = true, float ageSeconds = 0.0f );
    /** @brief Lets the note go: the amplitude envelope enters its release; a strike rings on. */
    void noteOff();
    /** @brief Silence at once, the strike included; the note is given back. */
    void kill();

    /**
     * @brief How many times this voice has struck since it was prepared: the engine sums them, the self
     *        test counts them, and it is the only honest answer to "does it ever strike?".
     * @return the count
     */
    unsigned strikes() const { return m_strikes; }
    /** @return whether the amplitude envelope is running (attack to release) */
    bool isActive() const { return m_env.isActive(); }
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
    bool isStriking() const { return m_ksOn; }
    /** @return whether the envelope is in its release stage */
    bool isReleasing() const { return m_env.isReleasing(); }
    /** @return the amplitude envelope's current level, 0 .. 1 */
    float level() const { return m_env.level(); }
    /** @return the MIDI note sounding, or -1 once the envelope is done */
    int note() const { return m_note; }
    /** @return who plays this note (see noteOn) */
    int owner() const { return m_owner; }
    /** @return where the voice is right now (breath included) */
    float distance() const { return m_distEff; }
    /** @return the role it took, for the matrix source Layer */
    float layer() const { return m_layer; }
    /**
     * @brief R4.7: what the root moved under a voice that was already sounding, so it can go on sounding
     *        where it was. 1 for every note that began after the move.
     * @return the frequency multiplier the engine set with setRootComp
     */
    float rootComp() const { return m_rootComp; }
    /**
     * @brief Sets the root compensation (see rootComp()).
     * @param[in] m  the multiplier that keeps this voice's frequency where it was across a root change
     */
    void setRootComp( float m ) { m_rootComp = m; }
    /** @return the frequency the bank is at right now in Hz (glides included) */
    double frequency() const { return m_freq; }
    /**
     * @brief Retune a sounding voice (tuning purity/drift): glides in the log domain, ~1 s time constant.
     * @param[in] hz  the frequency to arrive at
     */
    void setTargetFrequency( double hz ) { m_freqTarget = hz; }
    /**
     * @brief Per-note expression. Pressure and slide are 0..1, bend is in semitones; all three are
     *        smoothed inside the voice, so a controller sending steps never steps the sound.
     * @param[in] v  pressure 0 .. 1 (clamped)
     */
    void setPressure( float v ) { m_pressTarget = clampv( v, 0.0f, 1.0f ); }
    /**
     * @brief The key's sideways slide (see setPressure).
     * @param[in] v  0 .. 1 (clamped)
     */
    void setSlide( float v ) { m_slideTarget = clampv( v, 0.0f, 1.0f ); }
    /**
     * @brief The note's own pitch bend (see setPressure).
     * @param[in] semitones  signed, unclamped; the engine has applied the bend range
     */
    void setBend( float semitones ) { m_bendTarget = semitones; }
    /** @return the smoothed pressure, 0 .. 1 */
    float pressure() const { return m_press; }
    /** @return the smoothed slide, 0 .. 1 */
    float slide() const { return m_slide; }
    /**
     * @brief Portamento: start at fromHz and slide to the note's frequency over `seconds`, slowing near
     *        consonant ratios to the root by `gravity` (0 = an even log-domain glide).
     *
     * Call right after noteOn. Does nothing for a non-positive start or duration.
     * @param[in] fromHz   where the slide starts (the previous key's frequency)
     * @param[in] seconds  the nominal duration of an even slide
     * @param[in] gravity  0 .. 1, the pull towards the just ratios (see Voice.cpp for the magnet)
     */
    void glideFrom( double fromHz, float seconds, float gravity );
    /** @return whether a portamento is still under way */
    bool gliding() const { return m_portaLeft > 0.0f; }
    /**
     * @brief A sounding note slides to another (a near Phrase's second degree): the note and its
     *        frequency change, and the slide runs on the portamento with its gravity.
     *
     * Ignored when the voice is not sounding.
     * @param[in] note     the new MIDI note
     * @param[in] hz       its frequency
     * @param[in] seconds  the slide's duration, at least 50 ms
     * @param[in] gravity  0 .. 1, as glideFrom
     */
    void glideTo( int note, double hz, float seconds, float gravity );
    /**
     * @brief The plane moves: to `distance` over `seconds` -- a near event arriving out of the far plane
     *        or leaving into it. Everything that hangs on the distance follows, as it does for Breath.
     * @param[in] distance  where to go, 0 .. 1 (clamped)
     * @param[in] seconds   how long the (exponential) approach takes, at least 10 ms
     */
    void moveTo( float distance, float seconds )
    {
        m_distTarget = clampv( distance, 0.0f, 1.0f );
        m_distSeconds = std::max( seconds, 0.01f );
    }
    /**
     * @brief Where this note stands in what its owner is sounding, for the slot roles (SlotRole): set by
     *        the engine before noteOn, and again whenever the cluster changes.
     * @param[in] lowest   this note is the lowest of its owner's cluster
     * @param[in] highest  this note is the highest
     */
    void setPlace( bool lowest, bool highest )
    {
        m_placeLowest = lowest;
        m_placeHighest = highest;
    }
    /**
     * @brief A near event's shape, set before its noteOn: the gate as a share of the release, the bloom
     *        as a factor on the cutoff, the near field's low lift in decibels, and its place in the field.
     * @param[in] releaseMul   factor on the release time, at least 0.05
     * @param[in] cutoffMul    factor on the filter cutoff, 0.05 .. 8
     * @param[in] proximityDb  the lift between 120 and 300 Hz on the near plane, in dB
     * @param[in] pan          -1 .. 1, the event's place, added to the centre pan
     * @param[in] gain         the event's Gain (fore_gain) as a linear factor, folded into the pan pair
     */
    void setNearShape( float releaseMul, float cutoffMul, float proximityDb, float pan, float gain = 1.0f )
    {
        m_releaseMul = std::max( releaseMul, 0.05f );
        m_cutoffMul = clampv( cutoffMul, 0.05f, 8.0f );
        m_proxDb = proximityDb;
        m_panOffset = clampv( pan, -1.0f, 1.0f );
        m_nearGain = std::max( gain, 0.0f );
    }
    uint64_t m_order = 0;   ///< allocation order for voice stealing

    /**
     * @brief For pictures only: the current amplitude of each partial of the first strand, exactly the
     *        numbers the oscillator is summing right now -- so a display drawn from them breathes with
     *        the shimmer and the drift instead of being a static illustration.
     *
     * Read without
     * synchronisation from the message thread; a torn float costs one wrong pixel.
     * @param[out] out       receives the amplitudes
     * @param[in]  maxCount  room in @p out
     * @return how many were written (the strand's active partials, at most @p maxCount)
     */
    int displayPartials( float *out, int maxCount ) const
    {
        const int n = maxCount < m_strands[0].m_active ? maxCount : m_strands[0].m_active;
        for ( int i = 0; i < n; ++i ) out[i] = m_strands[0].m_amp[i];
        return n < 0 ? 0 : n;
    }
    /**
     * @brief The same for a slot's own bank (Additive / Wavetable in Source 1..3), and its grains.
     * @param[in]  slot      0 .. Slots-1 (clamped)
     * @param[out] out       receives the amplitudes
     * @param[in]  maxCount  room in @p out
     * @return how many were written
     */
    int displaySlotPartials( int slot, float *out, int maxCount ) const
    {
        return m_slots[slot < 0 ? 0 : ( slot >= Slots ? Slots - 1 : slot )].displayAmps( out, maxCount );
    }
    /**
     * @brief The grains a slot is sounding, for the picture (see SourceSlot::displayGrains).
     * @param[in]  slot      0 .. Slots-1 (clamped)
     * @param[out] out       receives one entry per live grain
     * @param[in]  maxCount  room in @p out
     * @param[in]  clipLen   the clip's length in samples, to give positions as 0 .. 1
     * @return how many were written
     */
    int displayGrains( int slot, SourceSlot::GrainInfo *out, int maxCount, int clipLen ) const
    {
        return m_slots[slot < 0 ? 0 : ( slot >= Slots ? Slots - 1 : slot )].displayGrains( out, maxCount, clipLen );
    }
    /**
     * @brief Where a slot's table is actually being read, Pos Drift included (see SourceSlot::displayPosition).
     * @param[in] slot  0 .. Slots-1 (clamped)
     * @return the position 0 .. 1, or -1 until the slot has rendered a table
     */
    float displaySlotPosition( int slot ) const
    {
        return m_slots[slot < 0 ? 0 : ( slot >= Slots ? Slots - 1 : slot )].displayPosition();
    }
    /** @return where the voice's centre sits right now, -1..1 */
    float pan() const { return m_centre; }
    /**
     * @brief For the envelope editor's playhead: the time each slot's entrance shape was read at in the
     *        last control block (seconds of the shape, -1 while the slot follows none) and the gain it
     *        gave. Read without synchronisation, as above.
     * @param[in] k  the slot, 0 .. Slots-1 (clamped)
     * @return seconds into the shape, or -1
     */
    float slotEnvTime( int k ) const { return m_slotEnvT[k < 0 ? 0 : ( k >= Slots ? Slots - 1 : k )]; }
    /**
     * @brief The gain a slot's entrance gave in the last control block (see slotEnvTime).
     * @param[in] k  the slot, 0 .. Slots-1 (clamped)
     * @return 0 .. 1
     */
    float slotGain( int k ) const { return m_slotGain[k < 0 ? 0 : ( k >= Slots ? Slots - 1 : k )]; }

    /**
     * @brief Adds `n` samples into the near (dry plane) and far (reverb send) buses.
     *
     * `fm` (n samples,
     * may be null) phase-modulates the partials when p.fmAmount > 0 (the feedback loop).
     * `couple` (n samples, may be null) is the previous block's foreground, mixed into this
     * voice's own filter input when p.sympathy > 0 -- strings on a shared soundboard.
     *
     * Runs control() every ControlBlock samples and stops early once neither the envelope nor
     * the strike is going; guards the z-plane state at the end of the block.
     * @param[in,out] nearL   left of the near bus, added to
     * @param[in,out] nearR   right of the near bus, added to
     * @param[in,out] farL    left of the far bus, added to
     * @param[in,out] farR    right of the far bus, added to
     * @param[in]     n       samples
     * @param[in]     p       the parameters of this block
     * @param[in]     fm      the feedback signal, n samples or nullptr
     * @param[in]     couple  the previous block's foreground, n samples or nullptr
     */
    void render( float *nearL, float *nearR, float *farL, float *farR, int n, const VoiceParams &p,
                 const float *fm = nullptr, const float *couple = nullptr );

private:
    /**
     * @brief One detuned copy of the spectrum: a bank of MaxPartials rotating phasors and its place in the field.
     */
    struct Strand
    {
        /// Every partial is a rotating phasor (cos, sin) turned by its own per-sample rotation;
        /// independent across partials, so the loop pipelines and vectorises. Phasors are
        /// renormalised once per control block.
        float m_pc[MaxPartials] = {},          ///< @brief cos of each partial's phasor
            m_ps[MaxPartials] = {};            ///< sin of each partial's phasor (the sample is amp * ps)
        float m_rc[MaxPartials] = {},          ///< @brief cos of each partial's per-sample rotation
            m_rs[MaxPartials] = {};            ///< sin of each partial's per-sample rotation
        float   m_amp[MaxPartials] = {};       ///< each partial's amplitude now, ramped towards the block's target
        float   m_ampStep[MaxPartials] = {};   ///< the per-sample increment that reaches the target at the block's end
        Drifter m_shimmer[MaxPartials];        ///< each partial's own amplitude wander
        Drifter m_pitch;                       ///< the strand's slow pitch drift
        float   m_gainL = 0.7f,   ///< @brief the strand's left gain (equal power from its pan) times the norm
            m_gainR = 0.7f;       ///< the strand's right gain
        /// Partial Spread renders this strand as a stereo pair instead of one sum, and a pair
        /// cannot be panned by multiplying each side with its own gain: that does not move the
        /// field, it deletes whatever leans the wrong way. The strand's place is folded into the
        /// partials' own weights instead (see control()), so what is left here is one gain.
        float m_wL[MaxPartials] = {},   ///< @brief each partial's left weight with the strand's pan composed in
            m_wR[MaxPartials] = {};     ///< each partial's right weight
        float m_spreadGain = 0.7f;      ///< the one gain of the spread pair: norm / sqrt 2
        int   m_active = 0;             ///< partials with any amplitude left, the loop's length
    };
    /**
     * @brief The control-rate half of render(): parameters into coefficients, once per ControlBlock samples.
     *
     * Envelope times, the plane's approach and breath, the portamento and retune glides, the
     * rate wander, Partial Spread's weights, the expression smoothing, the doppler, the presence
     * bell, Bloom, each slot's entrance and role, the strands' rotations and amplitude targets,
     * the interaural delay and head shadow, the near field, externalisation and height, the
     * voice filter, the phase field, the z-plane frame and the air. Everything that costs a
     * transcendental lives here, so the per-sample loop has none.
     * @param[in] blockLen  samples this block covers (at most ControlBlock)
     * @param[in] p         the parameters
     */
    void control( int blockLen, const VoiceParams &p );

    Strand      m_strands[MaxStrands];   ///< the strands; the first `unison` are rendered
    SourceSlot  m_slots[Slots];          ///< the four source slots
    float       m_rateMul = 1.0f;        ///< Rate Wander's factor on every movement rate this block
    Envelope    m_env;                   ///< the amplitude envelope
    VoiceFilter m_filt;                  ///< the voice filter (one of the models in Filter.h)
    bool m_lastFilterOn = true;   ///< filterOn as of the last block, to reset the filter when it is switched back in
    /// The wavefolder at four times the rate (25.09.2026). A fold turns a sine into a whole family
    /// of upper partials, which is its point -- and at the base rate most of them came back down
    /// as aliases. One oversampler per ear, cleared when the fold comes back on.
    Oversampler4 m_foldOsL,   ///< @brief the left ear's folder, oversampled
        m_foldOsR;            ///< the right ear's
    bool m_folding = false;   ///< the fold was on at the last sample
    /// The dry sum a Parallel z-plane hears, delayed by the drive's oversampling (29.5 samples, as
    /// the mean of 29 and 30) so the two branches meet in time; without it they combed.
    static constexpr int DryRing = 32;               ///< the ring's length, a power of two over the latency
    float                m_dryRingL[DryRing] = {},   ///< @brief the dry sum, left
        m_dryRingR[DryRing] = {};                    ///< the dry sum, right
    int m_dryW = 0;                                  ///< the ring's write position
    /// Binaural phase field: two first-order all-passes per ear, corners drifting apart
    Drifter m_phaseDrift;        ///< the one slow curve both ears' corners follow, in opposite directions
    bool    m_phaseOn = false;   ///< phaseWidth > 0 this block
    float   m_apC[2][2] = {},    ///< @brief the all-pass coefficients, [ear][stage]
        m_apX[2][2] = {},        ///< @brief each all-pass's previous input
        m_apY[2][2] = {};        ///< each all-pass's previous output
    /// Doppler from the breathing distance
    float  m_prevDist = 0.0f;    ///< the effective distance of the previous block, for its velocity
    double m_dopplerMul = 1.0;   ///< the pitch factor the velocity gives, 0.97 .. 1.03
    /// Strike: a Karplus-Strong loop excited at note-on, on the near plane
    static constexpr int StrikeMax = 4096;       ///< the loop's longest period in samples
    float                m_ks[StrikeMax] = {};   ///< the delay loop
    int                  m_ksLen = 0,            ///< @brief the loop's length: the period in samples
        m_ksPos = 0,                             ///< @brief the read/write position in the loop
        m_ksLeft = 0,                            ///< @brief noise samples still to inject
        m_ksT = 0,                               ///< @brief samples since the strike began
        m_ksTotal = 0;                           ///< samples the strike runs for before it is declared over
    float m_ksG = 0.0f,                          ///< @brief loop gain per pass, from the decay time
        m_ksLpC = 0.5f,                          ///< @brief the damping low pass's coefficient
        m_ksLp = 0.0f,                           ///< @brief the low pass's state
        m_ksApK = 0.0f,                          ///< @brief the all-pass coefficient (Metal), 0 for none
        m_ksApX = 0.0f,                          ///< @brief the all-pass's previous input
        m_ksApY = 0.0f,                          ///< @brief the all-pass's previous output
        m_ksAmp = 0.0f;                          ///< the excitation's amplitude
    float m_ksGainL = 0.7f,                      ///< @brief the strike's left gain, from the centre pan at note-on
        m_ksGainR = 0.7f;                        ///< the strike's right gain
    bool m_ksOn = false;                         ///< the strike is running
    /**
     * @brief Starts the strike: fills the loop's parameters for the type and arms the noise burst.
     *
     * String rings at the note; Wood is two octaves up (clamped to 200 .. 2500 Hz), short and dull;
     * Metal puts an all-pass in the loop. The strike is placed at the voice's centre pan.
     * @param[in] hz  the note's frequency
     * @param[in] p   the parameters, for level, type, decay and damp
     */
    void strikeStart( double hz, const VoiceParams &p );
    /**
     * @brief One sample of the strike loop: inject noise while any is left, filter, feed back, count down.
     * @return the sample read from the loop
     */
    inline float strikeTick();
    Svf m_airL,                 ///< @brief the Air band pass, left
        m_airR;                 ///< the Air band pass, right
    Drifter m_filterDrift,      ///< @brief the cutoff's slow wander
        m_airDrift,             ///< @brief the Air band's wander
        m_panCenter,            ///< @brief the centre pan's wander
        m_breath,               ///< @brief the plane's breathing
        m_rateWander;           ///< the very slow curve that scales every other rate
    Drifter m_zDriftX,          ///< @brief the z-plane point's wander in X
        m_zDriftY;              ///< and in Y
    ZBiquad m_zbL[ZSections],   ///< @brief the z-plane cascade, left
        m_zbR[ZSections];       ///< the z-plane cascade, right (same coefficients)
    ZModal m_zModal;            ///< the same frame read as a bank of ringing modes
    /**
     * @brief Series of biquads, or a parallel bank of resonators: one call so the two paths cannot
     *        drift apart at the two places the filter is applied.
     * @param[in,out] l  left sample, filtered in place
     * @param[in,out] r  right sample, filtered in place
     */
    inline void zRun( float &l, float &r )
    {
        if ( m_zModeCur == 3 )
        {
            l = m_zModal.tick( 0, l );
            r = m_zModal.tick( 1, r );
            return;
        }
        for ( int k = 0; k < m_zUsed; ++k )
        {
            l = m_zbL[k].tick( l );
            r = m_zbR[k].tick( r );
        }
    }
    int   m_zUsed = 0;           ///< sections (or modes) in use this block
    float m_zNorm = 1.0f;        ///< the cascade's make-up gain from zBuildCascade (1 for the modal bank)
    float m_fmHpXL = 0.0f,       ///< @brief DC blocker, left: previous input
        m_fmHpXR = 0.0f,         ///< @brief DC blocker, right: previous input
        m_fmHpYL = 0.0f,         ///< @brief DC blocker, left: previous output
        m_fmHpYR = 0.0f,         ///< @brief DC blocker, right: previous output
        m_fmHpCoef = 0.9987f;    ///< DC blocker for feedback FM
    float m_zWet = 0.0f,         ///< @brief the z-plane's share of the output (zMix)
        m_zDry = 1.0f;           ///< the unfiltered share
    int    m_zModeCur = 0;       ///< the z-plane mode this block: 0 off, 1 Series, 2 Replace, 3 Modal
    Rng    m_rng;                ///< the voice's own random stream (phases, noise, strike)
    double m_sr = 48000.0;       ///< sample rate in Hz
    double m_freq = 220.0,       ///< @brief the bank's frequency now, glides included
        m_freqTarget = 220.0;    ///< where a retune or portamento is heading
    double m_portaFrom = 0.0;    ///< @brief where the portamento started
    float  m_portaLeft = 0.0f,   ///< @brief seconds of the slide still to go, 0 = none
        m_portaSeconds = 0.0f,   ///< @brief the slide's nominal duration
        m_portaGravity = 0.0f;   ///< the pull towards the just ratios, 0 .. 1
    Resonator m_ghostL[6],       ///< @brief Ghost mode's six resonators, left
        m_ghostR[6];             ///< and right
    float m_ghostGain = 0.0f;    ///< the Ghost noise's gain, 0 when the mode is off
    float m_velocity = 1.0f;     ///< the note's level from its velocity, 0.3 .. 1
    float m_press = 0.0f,        ///< @brief the smoothed pressure
        m_pressTarget = 0.0f;    ///< per-note expression, smoothed at control rate
    float m_slide = 0.0f,        ///< @brief the smoothed slide
        m_slideTarget = 0.0f;    ///< what the controller last said
    float m_bend = 0.0f,         ///< @brief the smoothed bend in semitones
        m_bendTarget = 0.0f;     ///< what the controller last said
    float m_centre = 0.0f;       ///< pan centre after drift and Source 1's Pan, for the stage picture
    float m_distance = 0.0f;     ///< the plane the note was placed on
    float m_layer = 0.5f;        ///< the role this note took: foundation 0, body .25, colour .5, air .75, shadow 1
    float m_attackMul = 1.0f;    ///< what Vel to Attack made of the written attack, fixed at note-on
    float m_rootComp = 1.0f;     ///< the frequency this voice keeps across a root change
    float m_distEff = 0.0f;      ///< distance after breathing, refreshed at control rate
    float m_gNear = 1.0f,        ///< @brief the plane's share into the near bus, cos
        m_gFar = 0.0f,           ///< @brief the plane's share into the far bus, sin
        m_gLevel = 1.0f;         ///< the level the plane leaves, 1 at the ear, 0.5 on the horizon
    float m_airGain = 0.0f;      ///< the Air band's gain, normalised so `air` reads as a level
    float m_bloomT = 0.0f;       ///< seconds since note start on the movement clock, for Bloom
    float m_slotT =
        0.0f;   ///< the same on real time, for each slot's entrance (Freeze stops the first, not the second)
    float m_slotGain[Slots] = { 1.0f, 1.0f, 1.0f, 1.0f };   ///< each slot's own envelope, at control rate
    /// Sustain Loop entrances: how far each slot's clock was set back when the note was let go, so
    /// the shape carries on from where it was held; where each shape was read last; and whether the
    /// note was down in the last control block.
    float m_slotShift[Slots] = {};                              ///< the set-back per slot, in seconds
    float m_slotEnvT[Slots] = { -1.0f, -1.0f, -1.0f, -1.0f };   ///< where each shape was read last, -1 for none
    bool  m_slotHeld = false;                                   ///< the note was down in the last control block
    /// The slot roles: where the note stands, and each slot's gain from its role, faded at control
    /// rate. All ones for a preset whose slots have no role, which is every preset before 13.09.2026.
    bool m_placeLowest = true,                              ///< @brief this note is the lowest of its owner's cluster
        m_placeHighest = true;                              ///< this note is the highest
    float m_roleGain[Slots] = { 1.0f, 1.0f, 1.0f, 1.0f };   ///< each slot's gain from its role, faded over 1.5 s
    /**
     * @brief The gain a role asks for at a given place: 1 when the note is where the role says, else 0.
     * @param[in] r        the slot's role
     * @param[in] lowest   the note is the lowest of the cluster
     * @param[in] highest  the note is the highest
     * @return 1 or 0 (always 1 for SlotRole::All)
     */
    static float roleTarget( SlotRoleE r, bool lowest, bool highest )
    {
        switch ( r )
        {
        case SlotRoleE::LOWEST: return lowest ? 1.0f : 0.0f;
        case SlotRoleE::HIGHEST: return highest ? 1.0f : 0.0f;
        case SlotRoleE::INNER: return ( !lowest && !highest ) ? 1.0f : 0.0f;
        default: return 1.0f;
        }
    }
    /// A near event's shape (see setNearShape), the low lift's one-pole per ear, its place in the
    /// field as an equal-power pair, and the plane it is travelling to.
    float m_releaseMul = 1.0f,   ///< @brief factor on the release time
        m_cutoffMul = 1.0f,      ///< @brief factor on the cutoff
        m_proxDb = 0.0f,         ///< @brief the near field's lift in dB
        m_panOffset = 0.0f;      ///< the event's pan, added to the centre
    float m_nearGain = 1.0f;     ///< the event's Gain (fore_gain), folded into the pan pair at note-on
    float m_proxAmt = 0.0f,      ///< @brief the lift as a linear gain minus one
        m_proxLoCoef = 0.0f,     ///< @brief one-pole coefficient at 120 Hz
        m_proxHiCoef = 0.0f;     ///< one-pole coefficient at 300 Hz
    float m_proxLoL = 0.0f,      ///< @brief the 120 Hz one-pole's state, left
        m_proxLoR = 0.0f,        ///< @brief and right
        m_proxHiL = 0.0f,        ///< @brief the 300 Hz one-pole's state, left
        m_proxHiR = 0.0f;        ///< and right
    float m_panL = 1.0f,         ///< @brief the event's left gain (sqrt 2 cos, times its Gain); 1 for any other note
        m_panR = 1.0f;           ///< the event's right gain
    float m_distTarget = 0.0f,   ///< @brief the plane moveTo() is heading for
        m_distSeconds = 0.0f;    ///< how long the approach takes, 0 when none is under way
    /// Control-rate caches: the spectral shape only changes when its parameters do.
    float m_tiltCache[MaxPartials + 1] = {};   ///< tilt and odd/even per harmonic, indexed 1 .. partials
    float m_cachedTilt = -1.0f,                ///< @brief the tilt the cache was built for
        m_cachedOddEven = -9.0f;               ///< the odd/even the cache was built for
    int    m_cachedPartials = -1;              ///< the partial count the cache was built for
    double m_stretchCache[MaxPartials] = {};   ///< the inharmonic stretch factor per harmonic
    float  m_cachedB = -1.0f;                  ///< the stiffness the stretch cache was built for
    float  m_itdBufL[ItdBuffer] = {},          ///< @brief the interaural delay ring, left
        m_itdBufR[ItdBuffer] = {};             ///< the interaural delay ring, right
    int m_itdW = 0;                            ///< the rings' write position
    /// The far send's gap (Depth Gap): a source at the ear reaches the far reverb this much later
    /// than its direct sound, a source on the horizon at once -- the pre-delay as a cue of distance,
    /// per voice, ahead of the far bus. Heap rings sized in prepare() (80 ms at the rate), so a bank
    /// of engines on a test's stack stays the size it was.
    std::vector<float> m_farDlyL,   ///< @brief the far send's delay ring, left
        m_farDlyR;                  ///< the far send's delay ring, right
    int m_farDlyMask = 0,           ///< @brief ring size - 1 (a power of two; 0 until prepare())
        m_farDlyW = 0;              ///< the rings' write position
    float m_farDly = 0.0f,          ///< @brief the far send's delay now, in samples (glided per sample)
        m_farDlyTarget = 0.0f;      ///< where it is heading: Gap x (1 - distance) at the rate
    float m_itdL = 0.0f,            ///< @brief the left delay now, in samples (smoothed)
        m_itdR = 0.0f,              ///< @brief the right delay now
        m_itdLTarget = 0.0f,        ///< @brief where the left delay is heading
        m_itdRTarget = 0.0f;        ///< where the right delay is heading
    float m_shadowL = 0.0f,         ///< @brief the far ear's one-pole state, left
        m_shadowR = 0.0f,           ///< @brief and right
        m_shadowCoefL = 1.0f,       ///< @brief its coefficient, left (1 = no shadow)
        m_shadowCoefR = 1.0f;       ///< head shadow on the far ear
    /// The binaural mode's head shadow is a one-pole/one-zero rather than a one-pole: the
    /// spherical-head model (Brown and Duda 1998), whose zero moves with the angle.
    bool  m_sphereShadow = false;        ///< the spherical-head filter is in use (binaural mode)
    float m_sphB0[2] = { 1.0f, 1.0f },   ///< @brief the filter's input gain per ear
        m_sphB1[2] = {},                 ///< @brief its zero's coefficient per ear
        m_sphA1[2] = {};                 ///< its pole's coefficient per ear
    float m_sphX1[2] = {},               ///< @brief its previous input per ear
        m_sphY1[2] = {};                 ///< its previous output per ear
    /// Externalisation (Brown and Duda's structural model, the parts that need no measured data):
    /// the pinna's notch, whose frequency moves with the source's angle, and the shoulder echo.
    Svf m_pinnaL,            ///< @brief the pinna notch, left
        m_pinnaR;            ///< the pinna notch, right
    float m_extAmt = 0.0f;   ///< externalise, 0 .. 1
    int   m_shoulder = 0;    ///< the shoulder echo's delay in samples (about 0.26 ms)
    /// Height: the notch above is shared with externalisation; the 8 kHz band is its own.
    Svf m_skyL,   ///< @brief Blauert's 8 kHz band, left
        m_skyR;   ///< and right
    /// Both of these depend on the sample rate and nothing else, and were being recomputed for
    /// every voice every 64 samples -- an exp and a tan each. Once, in prepare().
    Svf   m_skyProto;              ///< the 8 kHz band's coefficients, copied into m_skyL/m_skyR when needed
    float m_ildCoefConst = 0.0f;   ///< the near field's 1 kHz one-pole coefficient
    float m_pinnaAmt =
              0.0f,   ///< @brief how much of the notch path is mixed in: the larger of externalise and |elevation|
        m_skyGain = 0.0f;   ///< the 8 kHz band's gain, signed by elevation (0 = flat)
    /// Near field: a low shelf on each ear, cut on the far one and lifted on the near one.
    float m_ildAmt = 0.0f,   ///< @brief nearIld, 0 .. 1
        m_ildCoef = 0.0f,    ///< @brief the shelf's one-pole coefficient
        m_ildL = 0.0f,       ///< @brief the left ear's shelf amount (positive cuts, negative lifts)
        m_ildR = 0.0f,       ///< @brief the right ear's
        m_ildLpL = 0.0f,     ///< @brief the one-pole's state, left
        m_ildLpR = 0.0f;     ///< and right
    /// Partial Spread: per-partial left/right weights (equal power, so the sum of each pair is
    /// exactly what the mono path gives), and the slow turn of their pattern.
    float m_spreadAmt = 0.0f;              ///< partialSpread, 0 .. 1
    float m_spreadSum[MaxPartials] = {},   ///< @brief each partial's left plus right weight
        m_spreadDif[MaxPartials] = {};     ///< each partial's left minus right weight
    double   m_spreadPhase = 0.0;          ///< the pattern's turn, 0 .. 1, once every fifty seconds
    int      m_note = -1;                  ///< the MIDI note, -1 when none
    int      m_owner = 0;                  ///< who plays it (see noteOn)
    unsigned m_strikes = 0;                ///< how often this voice has struck
    int      m_lastUnison = 0;   ///< the strand count of the previous block, to silence strands that were switched off
};

}   // namespace ambient
