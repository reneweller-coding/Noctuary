/**
 * @file Sources.h
 * @brief The sound sources per voice: three equal slots.
 *
 * Source 1 defaults to Additive, which is the voice's strand bank (Voice.h: up to six detuned or
 * stacked copies of a 32-partial spectrum, the classic Oscillator); Additive in Source 2 or 3 is
 * a single bank of the same spectrum inside the slot. Every slot can otherwise be one of:
 *   Harmonic  -- not a table of samples but a table of SPECTRA (32 partial amplitudes per
 *                frame); the position morphs between frames and the result is rendered by
 *                the same rotating-phasor bank as the main oscillator. Alias-free, and every
 *                partial keeps its own life (presence, low cut, feedback PM apply the same
 *                way). Built-in tables are generated; a user table is analysed from the
 *                file's frames. This type was called Wavetable until the classic kind came.
 *   Wavetable -- the classic kind (CycleTable.h): up to 256 single cycles of 2048 samples,
 *                read as samples the way Serum, Vital and Hive play them, band-limited per
 *                octave. The same file and the same five built-in names as Harmonic.
 *   FM        -- a two-operator pair (carrier at the slot pitch, modulator at FM Ratio),
 *                index limited automatically for high notes.
 *   Texture   -- a granular player over a loaded sample (field recording, flute air,
 *                metal), grains around Position with a slow wander; Follow = Note pitches
 *                the sample to the note (assuming it was recorded at C4).
 *   Stretch   -- the same clip read as a continuum instead of as grains: a spectral time
 *                stretch (window, transform, keep the magnitudes, random phases, overlap-add)
 *                by a factor of 1 to 1000, so a twenty-second recording becomes hours of
 *                weather with no grain rhythm and no transient left standing. Pitch is set by
 *                resampling BEFORE the stretch, so a note played higher does not get shorter.
 *                Loops with a crossfade at the seam, or without one for a clip that is marked
 *                seamless in its file name ("_loop").
 * Every slot has level, octave, a just ratio to the note and a pan, and goes through the
 * voice's filter, envelope and distance like the main bank.
 *
 * Since the file comment above was written the slots have become four (the Vector's corners),
 * and the types have grown: Noise (ten colours), Bow (a bowed string), Spectral (a clip played
 * back from its band model, pitch and speed apart), the near sources of 13.09.2026 (Flute,
 * Murmur, Bowl, Ice, Drops, Clip -- SourcesNear.cpp) and the signals after them (Whistler to
 * Dial). SourceSlot is the one class that renders all of them, a block at a time, into the
 * voice; prepare() does every allocation on the message thread and render() never allocates.
 */
#pragma once
#include "Dsp.h"
#include "Cosmos.h"   // Fft, for the Stretch type
#include "CycleTable.h"
#include <cstdint>
#include <vector>

namespace ambient {

constexpr int kSlots         = 4;    ///< source slots per voice (Source 1 .. 4); the self test checks it against kSourceSlots in Params.h
constexpr int kTableFrames   = 64;   ///< the most frames a Harmonic table holds (a longer file is thinned)
constexpr int kTablePartials = 32;   ///< partials per frame, the same width as the voice's bank
/**
 * @brief Unison in a slot.
 *
 * The main oscillator has had up to six detuned strands with their own pan
 * since the beginning; a slot had ONE, mono, and against the bank it sounded exactly as small as
 * that. The copies are not a second bank: the phasor bank is a flat list, so copy c simply takes
 * entries c*32 .. c*32+31 at its own slightly detuned pitch, and the same SIMD loop steps all of
 * them. At unison 1 the list is what it always was, entry for entry.
 */
constexpr int kSlotUnison    = 4;
/**
 * @brief ceiling; Grains sets how many a slot may use.
 *
 * Raised from
 * 64 when Density went to 200 a second: with the old ceiling
 * the top of that range could not be reached at all, and the
 * slot silently dropped the spawns it had no room for. Cheap:
 * measured over a whole preset, eight grains against sixty-four
 * is two percent of the render, the reverbs being the cost.
 */
constexpr int kSlotGrains    = 128;

/**
 * @brief What a slot renders.
 *
 * Additive sat last so the indices the presets store for the other types stayed what they were;
 * Stretch came after it and is appended for the same reason. So is the classic Wavetable: index 1
 * is still the spectral table, which a saved session stores by number, and it is called Harmonic
 * now. Packs name the types in words, and a pack written before this reads "Wavetable" as
 * Harmonic (PresetPacks.cpp).
 * The near sources (13.09.2026) are appended for the same reason: Flute (a blown pipe), Murmur (a
 * voice that never says anything), Bowl and Ice (friction on a set of modes: a singing bowl, and
 * ice or old wood creaking), Drops (water falling into a vessel).
 * The signals (13.09.2026, the second foreground round) after them: a whistler falling through the
 * magnetosphere, a seed pod shaken, bronze struck, a Geiger tube, a fluorescent tube, the Krell's
 * circuits, a beacon's packet, a number station's Morse, a shortwave dial turned.
 */
enum class SourceType : int {
    Off = 0,     ///< @brief the slot is silent
    Harmonic,    ///< @brief a table of spectra, morphed and rendered by the phasor bank
    Fm,          ///< @brief two operators
    Texture,     ///< @brief grains over a clip
    Noise,       ///< @brief one of the NoiseKind colours
    Additive,    ///< @brief the voice's own spectrum formula in the slot (in Source 1: the strand bank)
    Stretch,     ///< @brief Paulstretch over a clip
    Bow,         ///< @brief a bowed string
    Spectral,    ///< @brief a clip from its band model
    Wavetable,   ///< @brief single cycles read as samples (CycleTable.h)
                              Flute,    ///< @brief a blown pipe
                              Murmur,   ///< @brief a voice on a radio
                              Bowl,     ///< @brief a singing bowl rubbed
                              Ice,      ///< @brief ice or old wood creaking
                              Drops,    ///< @brief water into a vessel
                              Clip,     ///< @brief the recording played straight through
                              Whistler,   ///< @brief a VLF whistler falling
                              Shaker,     ///< @brief a seed pod shaken
                              Chime,      ///< @brief bronze struck
                              Geiger,     ///< @brief a Geiger tube
                              Tube,       ///< @brief a fluorescent tube
                              Krell,      ///< @brief the Krell's circuits
                              Beacon,     ///< @brief a beacon's packet
                              Morse,      ///< @brief a number station
                              Dial        ///< @brief a shortwave dial turned
                              };

constexpr int kNumSourceTypes = 25;   ///< entries of SourceType and kSourceTypeNames
/**
 * @brief Which of a voice's notes a slot sounds in (SlotParams::role).
 *
 * All is every note, as it always
 * was. Lowest, Inner and Highest are the note's place in what its owner is sounding right now --
 * the register IS the role in this music (Rene's table), and a slot that is the cello under the
 * chord should not also be the chime on top of it. The place is re-read as the cluster changes
 * and the slot fades over a second or two, so a note that was the top and is no longer hands
 * the chime on rather than keeping it.
 */
enum class SlotRole : int {
    All = 0,   ///< @brief every note
    Lowest,    ///< @brief only the lowest note of the owner's cluster
    Inner,     ///< @brief only a note that is neither lowest nor highest
    Highest,   ///< @brief only the highest
    Count      ///< @brief how many roles there are
};
constexpr int kNumSlotRoles = static_cast<int>(SlotRole::Count);   ///< entries of kSlotRoleNames
extern const char* const kSlotRoleNames[kNumSlotRoles];            ///< "All", "Lowest", "Inner", "Highest"
/**
 * @brief The longest spectral window the Stretch type analyses: 16384 samples, a third of a second at
 *        48 kHz.
 *
 * Paulstretch's own default is a quarter of a second, which is where the smooth results
 * start; longer windows are smoother still but cost memory in every slot of every voice.
 */
constexpr int kStretchMaxN = 16384;
constexpr int kStretchMinN = 256;       ///< the shortest stretch window; the shared transforms cover every power of two between the two
constexpr int kNumTables      = 6;      ///< Classic, Organ, Vocal, Glass, Metal, User
constexpr int kNumSlotRatios  = 10;     ///< entries of kSlotRatios and kSlotRatioNames
extern const char* const kSourceTypeNames[kNumSourceTypes];   ///< the type names presets and packs use, in SourceType order
extern const char* const kTableNames[kNumTables];             ///< "Classic", "Organ", "Vocal", "Glass", "Metal", "User"
extern const char* const kSlotRatioNames[kNumSlotRatios];     ///< "1/1", "9/8", "6/5", "5/4", "4/3", "3/2", "8/5", "5/3", "7/4", "2/1"
extern const double      kSlotRatios[kNumSlotRatios];         ///< the just ratios behind kSlotRatioNames
extern const char* const kFollowNames[2];                     ///< "Free", "Note": whether a clip is pitched to the note

/**
 * @brief Noise is a source in its own right, not just the Air band: an ambient instrument spends half
 *        its life in it. Ten colours, from the textbook slopes to the ones that are really textures.
 */
enum class NoiseKind : int {
    White = 0,   ///< @brief flat
    Pink,        ///< @brief -3 dB per octave (Kellet's seven-term filter)
    Brown,       ///< @brief -6 dB per octave
    Blue,        ///< @brief +3 dB per octave
    Violet,      ///< @brief +6 dB per octave
    Grey,        ///< @brief equal loudness
    Band,        ///< @brief a resonant band that can track the note
    Wind,        ///< @brief a wandering band
    Crackle,     ///< @brief sparse pops
    Digital,     ///< @brief sample-and-hold steps
    Cicada,      ///< @brief two insects per ear in bursts
    Count        ///< @brief how many colours there are
};
constexpr int kNumNoiseKinds = static_cast<int>(NoiseKind::Count);   ///< entries of kNoiseKindNames
extern const char* const kNoiseKindNames[kNumNoiseKinds];            ///< the colours' names, in NoiseKind order

/** @brief A table of spectra: up to kTableFrames frames of kTablePartials partial amplitudes, the Harmonic type's material. */
struct Wavetable {
    int   frames = 0;                                ///< frames in use, 0 for an empty table
    float amp[kTableFrames][kTablePartials] = {};    ///< each frame's partial amplitudes, normalised to unit energy
    /**
     * @brief Spectrum at position 0..1, 32 amplitudes.
     *
     * Between two frames the amplitudes are blended
     * linearly, which is what every wavetable does and what makes a morph between two formants
     * sound hollow halfway: the old peak fades out, the new one fades in, and between them the
     * energy dips (two half-height peaks hold half the energy of one). With `transport` the
     * blend becomes the displacement interpolation of optimal transport instead -- the
     * one-dimensional Wasserstein barycentre, which along a line is exact and cheap: each
     * frame's amplitudes are read as a distribution of mass over the partials, the two are
     * walked in step by cumulative mass, and every slice of mass is put down at the point
     * between where it sits in one frame and where in the other. Peaks SLIDE from one partial
     * to the next rather than fading, the total mass is the blend of the two totals, and the
     * hollow is gone (Roma, Green and Tremblay; measured in the selftest: the energy halfway
     * doubles, and the spread of the spectrum collapses from five partials to none).
     *
     * @param pos        0 .. 1 across the frames (clamped)
     * @param out        receives kTablePartials amplitudes
     * @param transport  0 = the linear blend, 1 = the transport interpolation, between = a mix of the two
     */
    void spectrumAt(float pos, float* out, float transport = 0.0f) const;
    /**
     * @brief Builds a table from raw samples laid out as consecutive single-cycle frames of
     *        `frameLen` samples (2048 = Serum/Vital layout).
     *
     * Up to kTableFrames frames are kept
     * (evenly picked when there are more). Returns false if there is not one full frame.
     *
     * A power-of-two frame goes through the FFT; any other length is summed bin by bin. Each
     * frame is normalised to unit energy. Message thread: allocates the transform.
     *
     * @param mono      the samples
     * @param n         how many
     * @param frameLen  samples per cycle (at least 8)
     * @return          true when at least one frame was analysed
     */
    bool analyse(const float* mono, int n, int frameLen = 2048);
};

/**
 * @brief One of the five generated tables (built on first use, or by SourceSlot::prepare).
 * @param index  0 .. kNumTables-2 (the last index is the user slot); clamped
 * @return       the table
 */
const Wavetable& builtinTable(int index);   ///< 0 .. kNumTables-2 (the last index is the user slot)

/**
 * @brief Base pitch from a texture file name: a trailing "_A3" / "-C#4" / " Bb2" note token before
 *        the extension (as written by Tools/TextureGen) gives the frequency at A4 = 440 Hz; 0 if none.
 * @param fileName  the file's name or path; nullptr gives 0
 * @return          the frequency in Hz, or 0 when the name carries no note
 */
double baseHzFromName(const char* fileName);
/**
 * @brief Whether the file name marks the clip as seamless ("_loop" or "-loop" anywhere before the
 *        extension, any case): the Stretch type then wraps without a crossfade.
 * @param fileName  the file's name or path; nullptr gives false
 * @return          true for a seamless clip
 */
bool loopFromName(const char* fileName);

/**
 * @brief What a recording is made of, band by band and frame by frame.
 *
 * A granular source cuts a clip into pieces and plays the pieces; a Paulstretch smears its
 * spectrum. Neither can hold a recording still at one pitch and read it at another speed, because
 * both still play samples. This does not play samples at all: the clip is measured once, into
 * thirty-two bands on the ear's own frequency scale, and what is stored per frame is how loud each
 * band is, where in the band its strongest partial sits, and how tonal it is -- how far above the
 * local noise floor its content stands. Playback then builds the sound again from an oscillator
 * and a band of noise per band, so pitch and speed are two separate numbers rather than one.
 *
 * This is the deterministic-plus-stochastic decomposition of Serra and Smith (1990), taken band by
 * band instead of partial by partial. Doing it per band means no fundamental has to be found: a
 * bell is nearly all oscillator, rain is nearly all noise, a voice is both, and none of the three
 * needs a pitch tracker that could be wrong about it.
 */
struct SpectralModel {
    static constexpr int kBands = kTablePartials;   ///< 32, the same width as the phasor bank
    int   frames = 0;                 ///< analysis frames, 0 for no model
    float hop = 0.0f;                 ///< seconds between frames
    float centre[kBands] = {};        ///< band centre in Hz, and
    float bandQ[kBands] = {};         ///< centre / width, the Q the noise band is filtered at
    std::vector<float> amp;           ///< frames * kBands, linear
    std::vector<float> freq;          ///< frames * kBands, Hz
    std::vector<float> tone;          ///< frames * kBands, 0 = noise, 1 = a partial
    /** @return whether there is too little to play: fewer than two frames */
    bool empty() const { return frames < 2; }
};

/** @brief A loaded clip: its samples, its measured gain and pitch, and its band model. */
struct Texture {
    std::vector<float> mono;   ///< the clip folded to one channel, what the mono paths read
    /// The clip's two channels, interleaved (L0 R0 L1 R1 ...), empty when the file was mono.
    ///
    /// Ninety-five per cent of the library is stereo and all of it used to be summed to mono at
    /// load: measured over 235 clips, the median correlation between the two channels is 0.90 for
    /// the generated textures and 0.76 for the field recordings, and a third to a half of them sit
    /// below 0.5. Summing those cancels exactly what is decorrelated, and what is decorrelated in a
    /// recording of a room is the diffuse part -- which is the low, enveloping part. The direct
    /// sound sits centred and survives, so the fold is a high-pass in disguise.
    ///
    /// Interleaved rather than two arrays because the grain loop gathers the two channels at the
    /// same index: side by side they share a cache line, which is what that loop waits for.
    std::vector<float> lr;     ///< the interleaved stereo pair, or empty
    /** @return whether the clip has a stereo pair the grains can read */
    bool stereo() const { return lr.size() == 2 * mono.size() && !mono.empty(); }
    double sampleRate = 48000.0;   ///< the clip's own rate in Hz; the readers resample to the engine's
    double baseHz = 261.6256;   ///< assumed pitch of the sample for Follow = Note
    /// Reading gain, from the clip's own RMS: the wavetable and FM slots normalise themselves to
    /// unity, so without this a quiet recording enters the mix 20 dB below them at the same Level.
    float  gain = 1.0f;         ///< 0.3 / RMS, clamped to 0.5 .. 16
    bool   seamless = false;    ///< the end runs into the start: no crossfade needed at the seam
    /** @return whether the clip is too short to play at all (under 64 samples) */
    bool empty() const { return mono.size() < 64; }
    /// The band model, measured once when the clip is loaded (on the loader thread, never in
    /// render()). A clip too short to analyse simply has none, and a Spectral slot on it is silent.
    SpectralModel spectral;     ///< the band model, empty for a clip under two windows long
    /**
     * @brief sets gain from mono, and builds the spectral model
     *
     * The RMS is taken from what actually plays -- the pair for a stereo clip, the fold for a mono
     * one -- so a wide clip does not enter louder than it used to. Loader thread, once per clip.
     */
    void measure();
    /**
     * @brief the model alone
     *
     * 4096-sample Hann windows at a quarter hop (stretched so no more than 4096 frames result),
     * thirty-two bands one ERB step apart from 45 Hz to 16 kHz; per band and frame the power, the
     * parabola-refined peak frequency, and how far the content stands above the local noise floor.
     * Loader thread; a clip under two windows long gets no model.
     */
    void analyse();
};

/** @brief A slot's settings for one block, read from the parameter table by the engine (slotParamIds). */
struct SlotParams {
    SourceType type = SourceType::Off;   ///< what the slot renders
    float level = 0.5f;          ///< 0 .. 1, the slot's level before the voice's envelope
    int   octave = 0;            ///< -2 .. 2
    int   ratio = 0;             ///< index into kSlotRatios
    float pan = 0.0f;            ///< -1 .. 1
    int   table = 0;             ///< index into kTableNames (last = user)
    float position = 0.0f;       ///< wavetable frame position / texture position
    float positionDrift = 0.3f;  ///< how far the position wanders on its own
    float fmRatio = 2.0f;        ///< modulator / carrier
    float fmIndex = 1.0f;        ///< modulation index (radians / 2pi at low notes)
    float grainMs = 200.0f;      ///< grain length in milliseconds (and the Stretch window)
    float density = 12.0f;       ///< grains per second
    bool  follow = false;        ///< texture pitched to the note
    int   grains = 16;           ///< how many grains this slot may have sounding at once, 1..kSlotGrains
    float spread = 0.03f;        ///< start-point scatter around Position, as a fraction of the clip
    NoiseKind noise = NoiseKind::Pink;   ///< the Noise type's colour
    float noiseQ = 0.4f;         ///< width of Band and Wind, 0 = wide open, 1 = a whistle
    /// Additive: the spectrum of the slot's own bank (same formula as the voice's strands)
    int   partials = 16;         ///< harmonics rendered, 1 .. kTablePartials
    float tilt = 1.2f,           ///< @brief spectral tilt, h^-tilt
          bright = 0.7f,         ///< @brief 0 .. 1, where the spectrum's window closes
          oddEven = 0.0f,        ///< @brief -1 .. 1, odd against even partials
          inharm = 0.0f,         ///< @brief 0 .. 1, stiff-string stretch
          shimmer = 0.4f,        ///< @brief per-partial amplitude wander, 0 .. 1
          shimmerRate = 0.15f;   ///< the shimmer's rate in Hz
    float drift = 0.0f;          ///< cents of slow, independent pitch drift (the asymmetric detune)
    /// Bow: how hard the bow presses and how fast it travels. The string is the slot's pitch.
    float bowForce = 0.4f,   ///< @brief 0 .. 1, the bow's pressure
          bowSpeed = 0.3f;   ///< 0 .. 1, the bow's speed
    /// Spectral: how fast the model is read (1 = the speed it was recorded at, 0 = held still),
    /// and which half of it is favoured (-1 = the partials only, +1 = the noise only).
    float specRate = 1.0f,     ///< @brief the read rate, 0 .. 1 and beyond
          specBreath = 0.0f;   ///< -1 .. 1, partials against noise
    float transport = 0.0f;      ///< Harmonic: 0 crossfade between frames, 1 slide the spectral mass
    /// Stretch: the factor, and the crossfade at the loop seam as a fraction of the clip (ignored
    /// for a clip marked seamless). The spectral window is Grain, the read position Position.
    float stretch = 40.0f;       ///< the stretch factor, 1 .. 1000
    float xfade = 0.1f;          ///< the seam crossfade as a fraction of the clip
    float root = 0.0f;           ///< Harmonic: give the fundamental at least this share of the energy
    int   unison = 1;            ///< detuned copies of the slot's bank, 1..kSlotUnison
    float uniDetune = 10.0f;     ///< cents between the outermost copies
    float uniWidth = 0.6f;       ///< how far the copies are placed apart across the field
    int   interp = 0;            ///< 0 linear, 1 Hermite (Catmull-Rom over four samples)
    /// The slot's own entrance, counted from note-on in the voice that plays it. The slot is
    /// silent for `delaySec`, then fades in over `riseSec` -- or follows a shape instead: one of the
    /// preset's six modulation envelopes if `envIndex` names one, or its own if `ownEnv`. Defaults
    /// are what the instrument did before slots could enter separately: no delay, and the voice's
    /// one amplitude envelope.
    float delaySec = 0.0f;       ///< seconds of silence after the note
    float riseSec = 1.0f;        ///< seconds of the fade-in that follows
    int   envIndex = -1;         ///< -1 = none, else 0..kNumModEnvs-1
    bool  ownEnv = false;        ///< the slot's own shape (VoiceParams::srcEnvShape), read as a level 0..1
    SlotRole role = SlotRole::All;   ///< which notes this slot sounds in (see SlotRole)
};

/**
 * @brief One source slot of a voice: the state of every type it can be, and the one render() that
 *        dispatches a block to the type's routine.
 *
 * A slot is one type at a time, so several types share memory (the bow's two lines are the flute's
 * bore and jet, the signals share one set of clocks); switching type starts the new one clean.
 * prepare() on the message thread; noteOn() and render() on the audio thread.
 */
class SourceSlot {
public:
    /**
     * @brief Seeds the slot's dice and does every allocation: the stretch buffers, the shared transforms,
     *        the string, the built-in cycle tables.
     *
     * Message thread, once per voice (and again at a sample-rate change).
     *
     * @param sampleRate  in Hz
     * @param seed        the slot's own random stream
     */
    void prepare(double sampleRate, uint64_t seed);
    /**
     * @brief A note begins in the voice that owns this slot.
     *
     * A fresh note (the voice was idle) draws new phasor phases, silences the bank, kills the
     * grains, restarts the stretch read from Position, blows the pipe from rest, puts the stick to
     * a still bowl, starts a new phrase and plays the clip again; a retrigger changes nothing.
     *
     * @param fresh  true when the voice's envelope was idle
     */
    void noteOn(bool fresh);
    /**
     * @brief Adds n (<= kControlBlock) samples of this slot into outL/outR. Control values are
     *        refreshed once per call; level and pan ramp across the block.
     *
     * The slot's pitch is the note times the just ratio and the octave, with the slow pitch drift
     * on top. Noise, Texture and Clip write their own two channels (their level and pan applied
     * inside); every other type renders mono into a scratch buffer that the level and an
     * equal-power pan then place -- except a spread unison bank, which writes two channels of its
     * own. A change of type starts the new type clean.
     *
     * @param outL       left, added to
     * @param outR       right, added to
     * @param n          samples, at most kControlBlock; 0 (or type Off) only records the type
     * @param noteHz     the voice's frequency in Hz (tuning, glides, tide and doppler included)
     * @param p          the slot's settings
     * @param table      the Harmonic table (built-in or user), or nullptr
     * @param texture    the slot's clip, or nullptr
     * @param driftRate  the voice's drift rate in Hz, for the slot's pitch drift (0 falls back to 0.05)
     * @param cycles     the Wavetable type's cycle table, or nullptr
     */
    void render(float* outL, float* outR, int n, double noteHz, const SlotParams& p,
                const Wavetable* table, const Texture* texture, float driftRate,
                const CycleTable* cycles = nullptr);

    /**
     * @brief For pictures (message thread, torn reads cost a pixel): the bank's partial amplitudes as
     *        they are being summed, and the grains that are sounding.
     * @param out       receives the amplitudes
     * @param maxCount  room in @p out
     * @return          how many were written (the bank's active entries, at most @p maxCount)
     */
    int displayAmps(float* out, int maxCount) const
    {
        const int n = maxCount < active_ ? maxCount : active_;
        for (int i = 0; i < n; ++i) out[i] = amp_[i];
        return n < 0 ? 0 : n;
    }
    /** @brief One sounding grain, for the picture: clip position 0..1, age 0..1, level, pan -1..1 */
    struct GrainInfo {
        float pos = 0.0f,   ///< @brief where in the clip, 0..1
        age = 0.0f,         ///< @brief how far through its window, 0..1
        gain = 0.0f,        ///< @brief its level
        pan = 0.0f;         ///< @brief its place, -1..1
    };
    /**
     * @brief The grains sounding right now, for the picture.
     * @param out       receives one entry per live grain
     * @param maxCount  room in @p out
     * @param clipLen   the clip's length in samples, to give positions as 0 .. 1
     * @return          how many were written
     */
    int displayGrains(GrainInfo* out, int maxCount, int clipLen) const;
    /**
     * @brief Where the table is actually being read, Pos Drift included: the knob plus the slot's own
     *        slow wander, which is what the ear hears moving and what no display could show as long as
     *        it only knew the knob. -1 until this slot has rendered a table.
     * @return the position 0 .. 1, or -1
     */
    float displayPosition() const { return dispPos_; }
    /** @brief The rubbed bodies' modes (SourcesNear.cpp): six, as ratios to the lowest. */
    static constexpr int kRubModes = 6;

private:
    /**
     * @brief The Harmonic type: the table's spectrum at the (drifting) position, rendered by the bank.
     * @param out    mono output (or the left channel when @p outR is given)
     * @param n      samples
     * @param hz     the slot's pitch in Hz
     * @param p      the settings
     * @param table  the spectra
     * @param dt     the block's length in seconds
     * @param outR   the right channel for a spread unison bank, or nullptr
     */
    void renderWavetable(float* out, int n, double hz, const SlotParams& p, const Wavetable* table, float dt, float* outR = nullptr);
    /**
     * @brief The Wavetable type: single cycles read as samples, band-limited per octave, a phase per copy.
     * @param out    mono output (or the left channel when @p outR is given)
     * @param n      samples
     * @param hz     the slot's pitch in Hz
     * @param p      the settings
     * @param table  the cycles
     * @param dt     the block's length in seconds
     * @param outR   the right channel for a spread unison bank, or nullptr
     */
    void renderCycles(float* out, int n, double hz, const SlotParams& p, const CycleTable* table, float dt, float* outR = nullptr);
    /**
     * @brief Additive in a slot: the voice's spectrum formula on the slot's own single bank.
     * @param out  mono output
     * @param n    samples
     * @param hz   the slot's pitch in Hz
     * @param p    the settings
     * @param dt   the block's length in seconds
     */
    void renderAdditive(float* out, int n, double hz, const SlotParams& p, float dt);
    /**
     * @brief The phasor bank's block: targets, then the sum. `uni` copies of the spectrum end to end;
     *        with more than one and an `outR`, they are spread across the field and it writes stereo.
     * @param spec  the kTablePartials amplitudes to head for
     * @param H     how many of them are below Nyquist (from setBankPitch)
     * @param n     samples
     * @param out   mono output, or the left channel
     * @param uni   copies of the spectrum in the flat bank
     * @param outR  the right channel when the copies are spread, or nullptr
     */
    void renderBank(const float* spec, int H, int n, float* out, int uni = 1, float* outR = nullptr);
    /**
     * @brief Rotations and stereo weights for `uni` copies around `hz`, detuned by `cents` end to end and
     *        placed across `width`. Returns how many partials of one copy fit below Nyquist.
     * @param hz        the slot's pitch in Hz
     * @param p         the settings (unison, uniDetune, uniWidth, pan)
     * @param partials  how many partials each copy renders
     * @return          how many partials of one copy fit below Nyquist
     */
    int  setBankPitch(double hz, const SlotParams& p, int partials);
    /**
     * @brief Two-operator FM with a DC blocker; the index shrinks above 3 kHz and wanders with Pos Drift.
     * @param out  mono output
     * @param n    samples
     * @param hz   the carrier's pitch in Hz
     * @param p    the settings (fmRatio, fmIndex, positionDrift)
     * @param dt   the block's length in seconds
     */
    void renderFm(float* out, int n, double hz, const SlotParams& p, float dt);
    /**
     * @brief The granular player: spawns grains around Position at Density a second and sums the live ones.
     *
     * Writes the left channel into @p outL and the right into scratch_; the caller adds both.
     * The instrument's largest single cost, and written accordingly (Sources.cpp).
     *
     * @param outL   the left channel
     * @param n      samples
     * @param hz     the slot's pitch in Hz (for Follow = Note)
     * @param speed  the playback speed for Follow = Free: the slot's pitch over the note's
     * @param p      the settings
     * @param tex    the clip, or nullptr (silence)
     * @param dt     the block's length in seconds
     */
    void renderTexture(float* outL, int n, double hz, double speed, const SlotParams& p, const Texture* tex, float dt);
    /**
     * @brief Ten colours of noise, one generator per ear, each normalised so Level means the same loudness.
     * @param outL  the left channel; the right goes into scratch_
     * @param n     samples
     * @param hz    the slot's pitch in Hz, for the Band colour tracking the note
     * @param p     the settings (noise, noiseQ, positionDrift)
     * @param dt    the block's length in seconds
     */
    void renderNoise(float* outL, int n, double hz, const SlotParams& p, float dt);
    /**
     * @brief Paulstretch over the clip: pulls from the overlap-add ring and calls stretchFrame() when it runs dry.
     * @param out    mono output
     * @param n      samples
     * @param hz     the slot's pitch in Hz (for Follow = Note)
     * @param speed  the resampling speed for Follow = Free
     * @param p      the settings (stretch, xfade, grainMs as the window, position)
     * @param tex    the clip, or nullptr
     * @param dt     the block's length in seconds
     */
    void renderStretch(float* out, int n, double hz, double speed, const SlotParams& p, const Texture* tex, float dt);
    /**
     * @brief A bowed string: two waveguides meeting at the bow, the Stribeck friction between them (Sources.cpp).
     * @param out  mono output
     * @param n    samples
     * @param hz   the string's pitch in Hz
     * @param p    the settings (bowForce, bowSpeed, position as the bow's place)
     * @param dt   the block's length in seconds
     */
    void renderBow(float* out, int n, double hz, const SlotParams& p, float dt);
    /**
     * @brief The near sources (SourcesNear.cpp), all mono into `out`; their place in the field is the
     *        slot's Pan like every other mono type.
     *
     * The flute: a jet-drive waveguide after Cook, Position the embouchure that overblows it.
     *
     * @param out  mono output
     * @param n    samples
     * @param hz   the pipe's pitch in Hz
     * @param p    the settings (bowForce as breath, bowSpeed as the jet's noise, position)
     * @param dt   the block's length in seconds
     */
    void renderFlute(float* out, int n, double hz, const SlotParams& p, float dt);
    /**
     * @brief Bowl and Ice: a modal body under a stick, the bow's friction driving doublet modes.
     * @param out  mono output
     * @param n    samples
     * @param hz   the lowest mode's pitch in Hz
     * @param p    the settings (bowForce, bowSpeed, position as the stick's place, positionDrift as the doublet's split)
     * @param dt   the block's length in seconds
     * @param ice  false for the singing bowl's modes, true for the ice's dense, short ones with the slip clock
     */
    void renderRub(float* out, int n, double hz, const SlotParams& p, float dt, bool ice);
    /**
     * @brief Speech without words on a radio: a glottal pulse through walking formants, syllables and pauses, the carrier's hiss and squelch.
     * @param out  mono output
     * @param n    samples
     * @param hz   the voice's pitch in Hz
     * @param p    the settings
     * @param dt   the block's length in seconds
     */
    void renderMurmur(float* out, int n, double hz, const SlotParams& p, float dt);
    /**
     * @brief Water falling into a vessel, after van den Doel: rising bubbles on a Poisson clock and the vessel's ring.
     * @param out  mono output
     * @param n    samples
     * @param hz   the note, for Pitch = Note bubbles on its partials
     * @param p    the settings (density, bright as the bubble size, position as the vessel)
     * @param dt   the block's length in seconds
     */
    void renderDrops(float* out, int n, double hz, const SlotParams& p, float dt);
    /**
     * @brief A VLF whistler: a tone falling as one over the square of time, a thread of noise under it.
     * @param out  mono output
     * @param n    samples
     * @param hz   where the whistle ends, in Hz
     * @param p    the settings (bright as the start, bowSpeed as the tau)
     * @param dt   the block's length in seconds
     */
    void renderWhistler(float* out, int n, double hz, const SlotParams& p, float dt);
    /**
     * @brief A seed pod shaken: bursts of filtered grains on a slow hand.
     * @param out  mono output
     * @param n    samples
     * @param hz   the rattle's resonance in Hz
     * @param p    the settings
     * @param dt   the block's length in seconds
     */
    void renderShaker(float* out, int n, double hz, const SlotParams& p, float dt);
    /**
     * @brief Struck bronze: a modal bank of five doublets, ringing and beating.
     * @param out  mono output
     * @param n    samples
     * @param hz   the strike tone in Hz
     * @param p    the settings
     * @param dt   the block's length in seconds
     */
    void renderChime(float* out, int n, double hz, const SlotParams& p, float dt);
    /**
     * @brief A Geiger-Mueller tube: discharges on a Poisson clock at Density a second, now and then a shower.
     * @param out  mono output
     * @param n    samples
     * @param hz   the click's resonance in Hz
     * @param p    the settings (density)
     * @param dt   the block's length in seconds
     */
    void renderGeiger(float* out, int n, double hz, const SlotParams& p, float dt);
    /**
     * @brief A fluorescent tube: the starter's clicks, the choke's hum at twice the mains, the tube's hiss.
     * @param out  mono output
     * @param n    samples
     * @param hz   the note (the hum sits on the mains, Position choosing 50 or 60 Hz)
     * @param p    the settings
     * @param dt   the block's length in seconds
     */
    void renderTube(float* out, int n, double hz, const SlotParams& p, float dt);
    /**
     * @brief The Krell's machines: FM steered by a Roessler attractor, wandering and never repeating.
     * @param out  mono output
     * @param n    samples
     * @param hz   the centre of the band the pitch wanders in, Hz
     * @param p    the settings
     * @param dt   the block's length in seconds
     */
    void renderKrell(float* out, int n, double hz, const SlotParams& p, float dt);
    /**
     * @brief A deep-space beacon's packet: a chirp, then eight bits of frequency-shift keying, repeated.
     * @param out  mono output
     * @param n    samples
     * @param hz   the space frequency in Hz (the mark a major third over it)
     * @param p    the settings
     * @param dt   the block's length in seconds
     */
    void renderBeacon(float* out, int n, double hz, const SlotParams& p, float dt);
    /**
     * @brief A number station: five-figure groups in Morse at Speed words a minute, under the ionosphere's flutter.
     * @param out  mono output
     * @param n    samples
     * @param hz   the keyed tone in Hz
     * @param p    the settings (bowSpeed as words per minute, position for letters, bright as the flutter)
     * @param dt   the block's length in seconds
     */
    void renderMorse(float* out, int n, double hz, const SlotParams& p, float dt);
    /**
     * @brief A shortwave set with its dial turned: sliding heterodyne whistles over the band's noise.
     * @param out  mono output
     * @param n    samples
     * @param hz   the centre of the whistles' band in Hz
     * @param p    the settings (position for how many whistles, bright for the noise)
     * @param dt   the block's length in seconds
     */
    void renderDial(float* out, int n, double hz, const SlotParams& p, float dt);
    /**
     * @brief Clip: the slot's recording played straight through, once, from Position -- a voice on a
     *        radio, a launch, a recording of Mars -- at its own speed (Pitch = Free) or pitched to the
     *        note. Writes left into outL and right into scratch_, as the granular Texture does.
     * @param outL   the left channel
     * @param n      samples
     * @param hz     the slot's pitch in Hz (for Pitch = Note)
     * @param speed  the playback speed for Pitch = Free
     * @param p      the settings
     * @param tex    the clip, or nullptr
     * @param dt     the block's length in seconds
     */
    void renderClip(float* outL, int n, double hz, double speed, const SlotParams& p, const Texture* tex, float dt);
    /**
     * @brief A clip played back from its band model: an oscillator and a band of noise per band, pitch and speed apart.
     * @param out        mono output
     * @param n          samples
     * @param transpose  the factor on every band's frequency (the note over the clip's pitch, or the slot's ratio)
     * @param p          the settings (specRate, specBreath, position)
     * @param tex        the clip with its model, or nullptr
     * @param dt         the block's length in seconds
     */
    void renderSpectral(float* out, int n, double transpose, const SlotParams& p, const Texture* tex, float dt);
    /**
     * @brief One Paulstretch frame: window the clip at the read position, keep the magnitudes, draw new phases, overlap-add.
     * @param p     the settings
     * @param tex   the clip
     * @param rate  the resampling step through the clip (the pitch)
     * @param N     the window length, a power of two between kStretchMinN and kStretchMaxN
     */
    void stretchFrame(const SlotParams& p, const Texture* tex, double rate, int N);
    /**
     * @brief The shared real transform of one size, built on first request (prepare() asks for every size).
     * @param n  the transform length
     * @return   the transform; shared, read-only after prepare(): one per size
     */
    static const RealFft& stretchFft(int n);   ///< shared, read-only after prepare(): one per size

    /// Wavetable: phasor bank like Voice::Strand.
    static constexpr int kBank = kTablePartials * kSlotUnison;   ///< entries of the flat bank: every copy's partials end to end
    float  pc_[kBank] = {},   ///< @brief cos of each phasor
           ps_[kBank] = {};   ///< sin of each phasor
    float  rc_[kBank] = {},   ///< @brief cos of each per-sample rotation
           rs_[kBank] = {};   ///< sin of each per-sample rotation
    float  amp_[kBank] = {},       ///< @brief each entry's amplitude now
           ampStep_[kBank] = {};   ///< its per-sample step towards the block's target
    float  wL_[kBank] = {},   ///< @brief each copy's left weight
           wR_[kBank] = {};   ///< each copy's place in the field
    bool   bankStereo_ = false;                ///< set when more than one copy is sounding
    int    active_ = 0;                        ///< entries with any amplitude left, the loop's length
    /// Wavetable (CycleTable): a phase per unison copy, the level and position the last block read,
    /// and a stream of its own for the start phases, so the type draws nothing from the slot's dice.
    double cyPhase_[kSlotUnison] = {};         ///< each copy's phase, 0 .. 1
    float  cyPos_ = 0.0f;                      ///< the position the last block read, for the glide
    int    cyLevel_ = 0;                       ///< the band-limit level the last block read
    bool   cyPrimed_ = false;                  ///< cyPos_ and cyLevel_ hold a value from a previous block
    Rng    cyRng_;                             ///< the start phases' own stream
    /// Additive: per-partial shimmer and the cached tilt/odd-even shape
    Drifter shim_[kTablePartials];             ///< each partial's amplitude wander
    float  tiltCache_[kTablePartials] = {};    ///< tilt and odd/even per partial, for the cached settings
    float  cTilt_ = -1.0f,   ///< @brief the tilt the cache was built for
           cOdd_ = -9.0f;    ///< the odd/even the cache was built for
    int    cPartials_ = -1;                    ///< the partial count the cache was built for
    /// FM
    double phC_ = 0.0,   ///< @brief the carrier's phase, 0 .. 1
           phM_ = 0.0;   ///< the modulator's phase, 0 .. 1
    float  hpX_ = 0.0f,   ///< @brief DC blocker: previous input
           hpY_ = 0.0f;   ///< DC blocker state
    /// Texture grains
    /// The window is a rotating phasor, not a cosine call: at 64 grains a std::cos per sample per
    /// grain is the single most expensive thing in the voice.
    /// `start`: where in the block this grain begins. A grain is due at a moment the spawn clock
    /// knows to the sample, and it used to begin at sample 0 of the block regardless -- so every
    /// grain of a dense cloud started on one of 750 instants a second, and the cloud grew a comb
    /// at the block rate that nothing in the music put there.
    struct Grain {
        double pos = 0.0;    ///< @brief read position in the clip, in samples
        double rate = 1.0;   ///< @brief samples of clip per output sample
        int len = 0;         ///< @brief the grain's length in samples
        int age = 0;         ///< @brief samples played so far
        int start = 0;       ///< @brief the sample of the block it begins at
        float gain = 0.0f;   ///< @brief its level
                   float gl = 0.0f,         ///< @brief left gain
                         gr = 0.0f;         ///< @brief right gain
                         float wc = 1.0f,   ///< @brief the window phasor's cos
                         ws = 0.0f,         ///< @brief the window phasor's sin
                         rc = 1.0f,         ///< @brief the window phasor's rotation, cos
                         rs = 0.0f;         ///< @brief the window phasor's rotation, sin
                         bool on = false;   ///< @brief the grain is live
                   };
    Grain  grains_[kSlotGrains];               ///< the grains; the live ones come first
    /// The live ones are grains_[0 .. live_), with no gaps. The array used to be a set of
    /// slots with an `on` flag, which meant walking all 128 of them per block to find the two
    /// that were sounding -- seven kilobytes touched to do nothing, per slot, per voice, per
    /// block. A grain that dies is swapped with the last live one and the count drops, which
    /// costs one copy and keeps the walk exactly as long as there is work in it. Spawning is
    /// then an append rather than a search for a free slot.
    int    live_ = 0;                          ///< how many grains are live
    double spawnIn_ = 0.0;   ///< seconds until the next grain
public:
    /**
     * @brief Noise: one generator per side, so the two channels are fully decorrelated -- which is what
     *        makes a noise bed sit around the listener instead of in the middle of the head.
     */
    struct NoiseState {
        float pink[7] = {};          ///< Kellet's economy pink filter
        float brown = 0.0f;          ///< the brown integrator
        float prev = 0.0f;           ///< for the differentiated colours
        float bp1 = 0.0f,   ///< @brief state-variable band pass, first integrator
              bp2 = 0.0f;   ///< state-variable band pass
        float hold = 0.0f;           ///< sample and hold
        double holdLeft = 0.0;       ///< samples until the next hold value
        double nextGrain = 0.0;      ///< crackle
        float crackle = 0.0f,        ///< @brief the crackle's current pop
              crackleDecay = 0.0f;   ///< its per-sample decay
        /// Cicada: two insects per ear, each either in a burst of pulses or in the pause between
        /// bursts, with a slow breathing of its own; the pulses ring in the band pass above.
        double burstLeft[2] = {},   ///< @brief samples left of each insect's burst
               pauseLeft[2] = {},   ///< @brief samples left of its pause
               pulseLeft[2] = {};   ///< samples to its next pulse
        float  breathe[2] = { 1.0f, 1.0f },     ///< @brief each insect's slow level
               breatheTo[2] = { 1.0f, 1.0f };   ///< where the level is heading
        float  pulseRate[2] = { 80.0f, 95.0f };   ///< each insect's pulse rate in Hz
    };
private:
    NoiseState noise_[2];                      ///< left and right
    /// Stretch: an overlap-add ring twice the longest window, the transform pair, and where the
    /// analysis reads in the clip. Allocated in prepare(), never in render().
    /// Bow: one period of string, its loop filter's state, and whether it has been started.
    static constexpr int kBowMax = 4096;   ///< 12 Hz at 48 kHz
    /// In vectors, not in the object: two arrays of this size per slot is thirty-two kilobytes,
    /// and four slots in each of many voices put an Engine built on the stack straight through it.
    std::vector<float> bowNut_,      ///< @brief the waveguide from the bow to the nut
                       bowBridge_;   ///< the waveguide from the bow to the bridge
    int   bowW_ = 0;                           ///< the write position in both lines
    float bowLp_ = 0.0f;                       ///< the bridge's loss filter state
    bool  bowReady_ = false;                   ///< the string has been started for this note
    /// Flute: the same two lines read as the pipe's bore and the air jet's travel (a slot is one
    /// type at a time, so the string's memory is the pipe's), the reflection filter's state, the
    /// breath rising at the start, the jet's noise, a DC blocker and the vibrato clock.
    bool   fluteReady_ = false;                ///< the pipe has been started for this note
    float  fluteRefl_ = 0.0f,     ///< @brief the reflection filter's state
           fluteBreath_ = 0.0f,   ///< @brief the breath pressure, rising at the start
           fluteHpX_ = 0.0f,      ///< @brief DC blocker: previous input
           fluteHpY_ = 0.0f,      ///< @brief DC blocker: previous output
           fluteJetLp_ = 0.0f;    ///< the jet noise's low pass
    float  fluteNoiseBp1_ = 0.0f,   ///< @brief the breath noise band pass, first integrator
           fluteNoiseBp2_ = 0.0f;   ///< its second integrator
    double flutePhase_ = 0.0,   ///< @brief the vibrato's phase
           fluteVibHz_ = 5.0;   ///< the vibrato's rate in Hz
    /// Bowl and Ice: six modes, each a pair of resonators a hair apart (the doublet every real
    /// bowl has, which is where its beating comes from), and the ice's slip clock.
    float  rubY1_[kRubModes][2] = {},   ///< @brief each doublet resonator's previous output
           rubY2_[kRubModes][2] = {};   ///< and the one before
    float  rubF1_ = 0.0f,   ///< @brief the friction force's previous value
           rubF2_ = 0.0f;   ///< the friction force's last two values (the modes' zero at DC)
    double rubSlipLeft_ = 0.0,   ///< @brief samples until the ice's next slip
           rubSlipOn_ = 0.0,     ///< @brief samples the current slip still lasts
           rubContact_ = 0.0;    ///< the stick's contact, rising at the start
    bool   rubReady_ = false;                  ///< the body has been started for this note
    /// Murmur: the glottal pulse clock, the three formants, the syllable and phrase machine, the
    /// radio chain's filters and the squelch and beep clocks.
    double murPhase_ = 0.0,       ///< @brief the glottal pulse's phase, 0 .. 1
           murSylLeft_ = 0.0,     ///< @brief samples left of the current syllable
           murPauseLeft_ = 0.0;   ///< samples left of the pause between phrases
    double murBeepLeft_ = 0.0,      ///< @brief samples until the Quindar beep
           murSquelchLeft_ = 0.0,   ///< @brief samples the squelch stays open after a phrase
           murBeepPhase_ = 0.0;     ///< the beep's phase
    float  murF_[3] = { 500.0f, 1500.0f, 2500.0f },    ///< @brief the three formants now, Hz
           murTo_[3] = { 500.0f, 1500.0f, 2500.0f };   ///< the vowel they walk towards
    float  murGlot1_ = 0.0f,     ///< @brief the glottal flow's first one-pole
           murGlot2_ = 0.0f,     ///< @brief its second
           murPitchSt_ = 0.0f,   ///< @brief the pitch's offset in semitones now
           murPitchTo_ = 0.0f,   ///< @brief where the syllable's pitch is heading
           murDecl_ = 0.0f;      ///< the phrase's declination
    float  murVoice_ = 1.0f,     ///< @brief voiced against fricative, now
           murVoiceTo_ = 1.0f,   ///< @brief and the syllable's target
           murGap_ = 1.0f,       ///< @brief the stop's gap gain, now
           murGapTo_ = 1.0f,     ///< @brief and its target
           murJitter_ = 1.0f;    ///< this period's jitter factor
    float  murBeepHz_ = 2525.0f,   ///< @brief the Quindar tone's frequency
           murHpX_ = 0.0f,         ///< @brief DC blocker: previous input
           murHpY_ = 0.0f,         ///< @brief DC blocker: previous output
           murHiss_ = 0.0f;        ///< the carrier's hiss level
    int    murSyllables_ = 0;                  ///< syllables left in the phrase
    bool   murInPhrase_ = false,   ///< @brief speaking rather than pausing
           murReady_ = false;      ///< the voice has been started for this note
    Svf    murForm_[3],   ///< @brief the three formant filters
           murRadioHp_,   ///< @brief the radio band's high pass
           murRadioLp_,   ///< @brief the radio band's low pass
           murFric_;      ///< the fricatives' band pass
    /// Drops: up to eight falling at once, and the vessel they fall into (two resonators).
    static constexpr int kDrops = 8;           ///< the most drops in the air at once
    /** @brief One bubble: a sine whose pitch rises as it decays. */
    struct Drop {
        double phase = 0.0,   ///< @brief the sine's phase
        hz = 0.0,             ///< @brief its frequency now
        rise = 0.0;           ///< @brief how fast the frequency rises
        float amp = 0.0f,     ///< @brief its amplitude now
        decay = 0.0f;         ///< @brief its per-sample decay
        int left = 0;         ///< @brief samples left
        bool on = false;      ///< @brief the drop is live
    };
    Drop   drops_[kDrops];                     ///< the drops
    double dropNext_ = 0.0;                    ///< samples until the next drop
    float  dropVesselY1_[2] = {},   ///< @brief the vessel's two resonators, previous output
           dropVesselY2_[2] = {},   ///< @brief and the one before
           dropClick_ = 0.0f;       ///< the impact click's level
    int    dropClickLeft_ = 0;                 ///< samples of click left
    /// Clip: where the read head stands in the recording (-1 until the note starts it at Position),
    /// and whether it has reached the end.
    double clipPos_ = -1.0;                    ///< the read position in clip samples, -1 before the note
    bool   clipDone_ = false;                  ///< the clip has been played to its end (and was not seamless)
    /// The signals: one set of clocks and memories that each of the nine reads its own way (a slot
    /// is one type at a time). sigT_ is the time since the note began; the phases, the two
    /// resonators' memories, an energy, a Poisson clock, a burst, counters and a bit pattern.
    double   sigT_ = 0.0,        ///< @brief seconds since the note began
             sigPhase_ = 0.0,    ///< @brief a phase, 0 .. 1
             sigPhase2_ = 0.0,   ///< @brief a second phase
             sigPhase3_ = 0.0;   ///< a third phase
    float    sigY1_ = 0.0f,       ///< @brief the first resonator's previous output
             sigY2_ = 0.0f,       ///< @brief and the one before
             sigZ1_ = 0.0f,       ///< @brief the second resonator's previous output
             sigZ2_ = 0.0f,       ///< @brief and the one before
             sigLp_ = 0.0f,       ///< @brief a one-pole's state
             sigEnergy_ = 0.0f;   ///< a decaying energy (a click, a discharge)
    double   sigNext_ = 0.0,        ///< @brief samples until the next event of a Poisson clock
             sigBurst_ = 0.0,       ///< @brief samples a burst still lasts
             sigFlutter_ = 0.0,     ///< @brief the ionosphere's flutter, now
             sigFlutterHz_ = 1.0;   ///< the flutter's rate
    int      sigCount_ = 0,   ///< @brief a counter (bits sent, clicks made)
             sigState_ = 0,   ///< @brief a state machine's state
             sigPos_ = 0;     ///< a position in a pattern
    unsigned sigBits_ = 0u;                    ///< the beacon's bit pattern
    double   krX_ = 0.1,   ///< @brief the Rossler system's state, x
             krY_ = 0.0,   ///< @brief y
             krZ_ = 0.0;   ///< the Rossler system's state
    float    chY1_[6] = {},   ///< @brief the chime's modes, previous output
             chY2_[6] = {};   ///< the chime's modes
    double   dlF_[3] = {},    ///< @brief the dial's three whistles, frequency now
             dlTo_[3] = {},   ///< @brief where each is drifting to
             dlPh_[3] = {};   ///< the dial's three whistles
    bool     sigReady_ = false;                ///< the signal's clocks have been started for this note

    /// Spectral: the two amplitude ramps per band (the partial and its noise), the state of the
    /// band's noise filter, and how far the read head has travelled from Position, in frames.
    float specA_[kTablePartials] = {},       ///< @brief each band's partial amplitude now
          specAStep_[kTablePartials] = {};   ///< its per-sample step
    float specN_[kTablePartials] = {},       ///< @brief each band's noise amplitude now
          specNStep_[kTablePartials] = {};   ///< its per-sample step
    float specBp_[kTablePartials] = {},   ///< @brief each band's noise filter, band-pass state
          specLp_[kTablePartials] = {};   ///< and low-pass state
    float specF_[kTablePartials] = {},   ///< @brief each band's filter frequency coefficient
          specQ_[kTablePartials] = {};   ///< and its damping
    double specAdvance_ = 0.0;                 ///< frames travelled from Position

    /** @brief The Stretch type's buffers and read position. */
    struct StretchState {
        std::vector<float> out,   ///< @brief the overlap-add ring, 2 kStretchMaxN
                           re,    ///< @brief the frame's real part
                           im,    ///< @brief the frame's imaginary part
                           win;   ///< the window
        int    n = 0;            ///< the window in use (0 = none yet)
        int    outPos = 0,    ///< @brief the read position in the ring
               hopLeft = 0;   ///< samples until the next frame is due
        double advance = 0.0;    ///< how far the read has travelled from Position, in clip samples
    };
    StretchState st_;                          ///< the stretch state
    Drifter noiseDrift_;                       ///< the Wind colour's wandering band
    Drifter posDrift_,     ///< @brief the position's slow wander (Pos Drift)
            idxDrift_,     ///< @brief the FM index's wander
            pitchDrift_;   ///< the slot's slow pitch drift
    float   dispPos_ = -1.0f;    ///< the position last read, for the picture (see displayPosition)
    Rng    rng_;                               ///< the slot's own random stream
    double sr_ = 48000.0;                      ///< sample rate in Hz
    float  gL_ = 0.0f,   ///< @brief the left gain at the end of the last block (level and pan)
           gR_ = 0.0f;   ///< the right gain
    float  scratch_[64] = {};    ///< a block of mono output, or the right channel of a two-channel type
    float  scratchR_[64] = {};   ///< the bank's right channel when its copies are spread
    SourceType lastType_ = SourceType::Off;   ///< the type of the last block, to start a new type clean
};

} // namespace ambient
