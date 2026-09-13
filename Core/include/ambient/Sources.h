// Noctuary -- the sound sources per voice: three equal slots.
//
// Source 1 defaults to Additive, which is the voice's strand bank (Voice.h: up to six detuned or
// stacked copies of a 32-partial spectrum, the classic Oscillator); Additive in Source 2 or 3 is
// a single bank of the same spectrum inside the slot. Every slot can otherwise be one of:
//   Harmonic  -- not a table of samples but a table of SPECTRA (32 partial amplitudes per
//                frame); the position morphs between frames and the result is rendered by
//                the same rotating-phasor bank as the main oscillator. Alias-free, and every
//                partial keeps its own life (presence, low cut, feedback PM apply the same
//                way). Built-in tables are generated; a user table is analysed from the
//                file's frames. This type was called Wavetable until the classic kind came.
//   Wavetable -- the classic kind (CycleTable.h): up to 256 single cycles of 2048 samples,
//                read as samples the way Serum, Vital and Hive play them, band-limited per
//                octave. The same file and the same five built-in names as Harmonic.
//   FM        -- a two-operator pair (carrier at the slot pitch, modulator at FM Ratio),
//                index limited automatically for high notes.
//   Texture   -- a granular player over a loaded sample (field recording, flute air,
//                metal), grains around Position with a slow wander; Follow = Note pitches
//                the sample to the note (assuming it was recorded at C4).
//   Stretch   -- the same clip read as a continuum instead of as grains: a spectral time
//                stretch (window, transform, keep the magnitudes, random phases, overlap-add)
//                by a factor of 1 to 1000, so a twenty-second recording becomes hours of
//                weather with no grain rhythm and no transient left standing. Pitch is set by
//                resampling BEFORE the stretch, so a note played higher does not get shorter.
//                Loops with a crossfade at the seam, or without one for a clip that is marked
//                seamless in its file name ("_loop").
// Every slot has level, octave, a just ratio to the note and a pan, and goes through the
// voice's filter, envelope and distance like the main bank.
#pragma once
#include "Dsp.h"
#include "Cosmos.h"   // Fft, for the Stretch type
#include "CycleTable.h"
#include <cstdint>
#include <vector>

namespace ambient {

constexpr int kSlots         = 4;
constexpr int kTableFrames   = 64;
constexpr int kTablePartials = 32;
// Unison in a slot. The main oscillator has had up to six detuned strands with their own pan
// since the beginning; a slot had ONE, mono, and against the bank it sounded exactly as small as
// that. The copies are not a second bank: the phasor bank is a flat list, so copy c simply takes
// entries c*32 .. c*32+31 at its own slightly detuned pitch, and the same SIMD loop steps all of
// them. At unison 1 the list is what it always was, entry for entry.
constexpr int kSlotUnison    = 4;
constexpr int kSlotGrains    = 128;  // ceiling; Grains sets how many a slot may use. Raised from
                                     // 64 when Density went to 200 a second: with the old ceiling
                                     // the top of that range could not be reached at all, and the
                                     // slot silently dropped the spawns it had no room for. Cheap:
                                     // measured over a whole preset, eight grains against sixty-four
                                     // is two percent of the render, the reverbs being the cost.

// Additive sat last so the indices the presets store for the other types stayed what they were;
// Stretch came after it and is appended for the same reason. So is the classic Wavetable: index 1
// is still the spectral table, which a saved session stores by number, and it is called Harmonic
// now. Packs name the types in words, and a pack written before this reads "Wavetable" as
// Harmonic (PresetPacks.cpp).
// The near sources (13.09.2026) are appended for the same reason: Flute (a blown pipe), Murmur (a
// voice that never says anything), Bowl and Ice (friction on a set of modes: a singing bowl, and
// ice or old wood creaking), Drops (water falling into a vessel).
// The signals (13.09.2026, the second foreground round) after them: a whistler falling through the
// magnetosphere, a seed pod shaken, bronze struck, a Geiger tube, a fluorescent tube, the Krell's
// circuits, a beacon's packet, a number station's Morse, a shortwave dial turned.
enum class SourceType : int { Off = 0, Harmonic, Fm, Texture, Noise, Additive, Stretch, Bow, Spectral, Wavetable,
                              Flute, Murmur, Bowl, Ice, Drops, Clip,
                              Whistler, Shaker, Chime, Geiger, Tube, Krell, Beacon, Morse, Dial };

constexpr int kNumSourceTypes = 25;
// Which of a voice's notes a slot sounds in (SlotParams::role). All is every note, as it always
// was. Lowest, Inner and Highest are the note's place in what its owner is sounding right now --
// the register IS the role in this music (Rene's table), and a slot that is the cello under the
// chord should not also be the chime on top of it. The place is re-read as the cluster changes
// and the slot fades over a second or two, so a note that was the top and is no longer hands
// the chime on rather than keeping it.
enum class SlotRole : int { All = 0, Lowest, Inner, Highest, Count };
constexpr int kNumSlotRoles = static_cast<int>(SlotRole::Count);
extern const char* const kSlotRoleNames[kNumSlotRoles];
// The longest spectral window the Stretch type analyses: 16384 samples, a third of a second at
// 48 kHz. Paulstretch's own default is a quarter of a second, which is where the smooth results
// start; longer windows are smoother still but cost memory in every slot of every voice.
constexpr int kStretchMaxN = 16384;
constexpr int kStretchMinN = 256;
constexpr int kNumTables      = 6;      // Classic, Organ, Vocal, Glass, Metal, User
constexpr int kNumSlotRatios  = 10;
extern const char* const kSourceTypeNames[kNumSourceTypes];
extern const char* const kTableNames[kNumTables];
extern const char* const kSlotRatioNames[kNumSlotRatios];
extern const double      kSlotRatios[kNumSlotRatios];
extern const char* const kFollowNames[2];

// Noise is a source in its own right, not just the Air band: an ambient instrument spends half
// its life in it. Ten colours, from the textbook slopes to the ones that are really textures.
enum class NoiseKind : int {
    White = 0, Pink, Brown, Blue, Violet, Grey, Band, Wind, Crackle, Digital, Cicada, Count
};
constexpr int kNumNoiseKinds = static_cast<int>(NoiseKind::Count);
extern const char* const kNoiseKindNames[kNumNoiseKinds];

struct Wavetable {
    int   frames = 0;
    float amp[kTableFrames][kTablePartials] = {};
    // Spectrum at position 0..1, 32 amplitudes. Between two frames the amplitudes are blended
    // linearly, which is what every wavetable does and what makes a morph between two formants
    // sound hollow halfway: the old peak fades out, the new one fades in, and between them the
    // energy dips (two half-height peaks hold half the energy of one). With `transport` the
    // blend becomes the displacement interpolation of optimal transport instead -- the
    // one-dimensional Wasserstein barycentre, which along a line is exact and cheap: each
    // frame's amplitudes are read as a distribution of mass over the partials, the two are
    // walked in step by cumulative mass, and every slice of mass is put down at the point
    // between where it sits in one frame and where in the other. Peaks SLIDE from one partial
    // to the next rather than fading, the total mass is the blend of the two totals, and the
    // hollow is gone (Roma, Green and Tremblay; measured in the selftest: the energy halfway
    // doubles, and the spread of the spectrum collapses from five partials to none).
    void spectrumAt(float pos, float* out, float transport = 0.0f) const;
    // Builds a table from raw samples laid out as consecutive single-cycle frames of
    // `frameLen` samples (2048 = Serum/Vital layout). Up to kTableFrames frames are kept
    // (evenly picked when there are more). Returns false if there is not one full frame.
    bool analyse(const float* mono, int n, int frameLen = 2048);
};

const Wavetable& builtinTable(int index);   // 0 .. kNumTables-2 (the last index is the user slot)

// Base pitch from a texture file name: a trailing "_A3" / "-C#4" / " Bb2" note token before
// the extension (as written by Tools/TextureGen) gives the frequency at A4 = 440 Hz; 0 if none.
double baseHzFromName(const char* fileName);
// Whether the file name marks the clip as seamless ("_loop" or "-loop" anywhere before the
// extension, any case): the Stretch type then wraps without a crossfade.
bool loopFromName(const char* fileName);

// What a recording is made of, band by band and frame by frame.
//
// A granular source cuts a clip into pieces and plays the pieces; a Paulstretch smears its
// spectrum. Neither can hold a recording still at one pitch and read it at another speed, because
// both still play samples. This does not play samples at all: the clip is measured once, into
// thirty-two bands on the ear's own frequency scale, and what is stored per frame is how loud each
// band is, where in the band its strongest partial sits, and how tonal it is -- how far above the
// local noise floor its content stands. Playback then builds the sound again from an oscillator
// and a band of noise per band, so pitch and speed are two separate numbers rather than one.
//
// This is the deterministic-plus-stochastic decomposition of Serra and Smith (1990), taken band by
// band instead of partial by partial. Doing it per band means no fundamental has to be found: a
// bell is nearly all oscillator, rain is nearly all noise, a voice is both, and none of the three
// needs a pitch tracker that could be wrong about it.
struct SpectralModel {
    static constexpr int kBands = kTablePartials;   // 32, the same width as the phasor bank
    int   frames = 0;
    float hop = 0.0f;                 // seconds between frames
    float centre[kBands] = {};        // band centre in Hz, and
    float bandQ[kBands] = {};         // centre / width, the Q the noise band is filtered at
    std::vector<float> amp;           // frames * kBands, linear
    std::vector<float> freq;          // frames * kBands, Hz
    std::vector<float> tone;          // frames * kBands, 0 = noise, 1 = a partial
    bool empty() const { return frames < 2; }
};

struct Texture {
    std::vector<float> mono;
    // The clip's two channels, interleaved (L0 R0 L1 R1 ...), empty when the file was mono.
    //
    // Ninety-five per cent of the library is stereo and all of it used to be summed to mono at
    // load: measured over 235 clips, the median correlation between the two channels is 0.90 for
    // the generated textures and 0.76 for the field recordings, and a third to a half of them sit
    // below 0.5. Summing those cancels exactly what is decorrelated, and what is decorrelated in a
    // recording of a room is the diffuse part -- which is the low, enveloping part. The direct
    // sound sits centred and survives, so the fold is a high-pass in disguise.
    //
    // Interleaved rather than two arrays because the grain loop gathers the two channels at the
    // same index: side by side they share a cache line, which is what that loop waits for.
    std::vector<float> lr;
    bool stereo() const { return lr.size() == 2 * mono.size() && !mono.empty(); }
    double sampleRate = 48000.0;
    double baseHz = 261.6256;   // assumed pitch of the sample for Follow = Note
    // Reading gain, from the clip's own RMS: the wavetable and FM slots normalise themselves to
    // unity, so without this a quiet recording enters the mix 20 dB below them at the same Level.
    float  gain = 1.0f;
    bool   seamless = false;    // the end runs into the start: no crossfade needed at the seam
    bool empty() const { return mono.size() < 64; }
    // The band model, measured once when the clip is loaded (on the loader thread, never in
    // render()). A clip too short to analyse simply has none, and a Spectral slot on it is silent.
    SpectralModel spectral;
    void measure();             // sets gain from mono, and builds the spectral model
    void analyse();             // the model alone
};

struct SlotParams {
    SourceType type = SourceType::Off;
    float level = 0.5f;
    int   octave = 0;            // -2 .. 2
    int   ratio = 0;             // index into kSlotRatios
    float pan = 0.0f;            // -1 .. 1
    int   table = 0;             // index into kTableNames (last = user)
    float position = 0.0f;       // wavetable frame position / texture position
    float positionDrift = 0.3f;  // how far the position wanders on its own
    float fmRatio = 2.0f;        // modulator / carrier
    float fmIndex = 1.0f;        // modulation index (radians / 2pi at low notes)
    float grainMs = 200.0f;
    float density = 12.0f;       // grains per second
    bool  follow = false;        // texture pitched to the note
    int   grains = 16;           // how many grains this slot may have sounding at once, 1..kSlotGrains
    float spread = 0.03f;        // start-point scatter around Position, as a fraction of the clip
    NoiseKind noise = NoiseKind::Pink;
    float noiseQ = 0.4f;         // width of Band and Wind, 0 = wide open, 1 = a whistle
    // Additive: the spectrum of the slot's own bank (same formula as the voice's strands)
    int   partials = 16;
    float tilt = 1.2f, bright = 0.7f, oddEven = 0.0f, inharm = 0.0f, shimmer = 0.4f, shimmerRate = 0.15f;
    float drift = 0.0f;          // cents of slow, independent pitch drift (the asymmetric detune)
    // Bow: how hard the bow presses and how fast it travels. The string is the slot's pitch.
    float bowForce = 0.4f, bowSpeed = 0.3f;
    // Spectral: how fast the model is read (1 = the speed it was recorded at, 0 = held still),
    // and which half of it is favoured (-1 = the partials only, +1 = the noise only).
    float specRate = 1.0f, specBreath = 0.0f;
    float transport = 0.0f;      // Harmonic: 0 crossfade between frames, 1 slide the spectral mass
    // Stretch: the factor, and the crossfade at the loop seam as a fraction of the clip (ignored
    // for a clip marked seamless). The spectral window is Grain, the read position Position.
    float stretch = 40.0f;
    float xfade = 0.1f;
    float root = 0.0f;           // Harmonic: give the fundamental at least this share of the energy
    int   unison = 1;            // detuned copies of the slot's bank, 1..kSlotUnison
    float uniDetune = 10.0f;     // cents between the outermost copies
    float uniWidth = 0.6f;       // how far the copies are placed apart across the field
    int   interp = 0;            // 0 linear, 1 Hermite (Catmull-Rom over four samples)
    // The slot's own entrance, counted from note-on in the voice that plays it. The slot is
    // silent for `delaySec`, then fades in over `riseSec` -- or follows a shape instead: one of the
    // preset's six modulation envelopes if `envIndex` names one, or its own if `ownEnv`. Defaults
    // are what the instrument did before slots could enter separately: no delay, and the voice's
    // one amplitude envelope.
    float delaySec = 0.0f;
    float riseSec = 1.0f;
    int   envIndex = -1;         // -1 = none, else 0..kNumModEnvs-1
    bool  ownEnv = false;        // the slot's own shape (VoiceParams::srcEnvShape), read as a level 0..1
    SlotRole role = SlotRole::All;   // which notes this slot sounds in (see SlotRole)
};

class SourceSlot {
public:
    void prepare(double sampleRate, uint64_t seed);
    void noteOn(bool fresh);
    // Adds n (<= kControlBlock) samples of this slot into outL/outR. Control values are
    // refreshed once per call; level and pan ramp across the block.
    void render(float* outL, float* outR, int n, double noteHz, const SlotParams& p,
                const Wavetable* table, const Texture* texture, float driftRate,
                const CycleTable* cycles = nullptr);

    // For pictures (message thread, torn reads cost a pixel): the bank's partial amplitudes as
    // they are being summed, and the grains that are sounding.
    int displayAmps(float* out, int maxCount) const
    {
        const int n = maxCount < active_ ? maxCount : active_;
        for (int i = 0; i < n; ++i) out[i] = amp_[i];
        return n < 0 ? 0 : n;
    }
    struct GrainInfo { float pos = 0.0f, age = 0.0f, gain = 0.0f, pan = 0.0f; };   // clip position 0..1, age 0..1, level, pan -1..1
    int displayGrains(GrainInfo* out, int maxCount, int clipLen) const;
    // Where the table is actually being read, Pos Drift included: the knob plus the slot's own
    // slow wander, which is what the ear hears moving and what no display could show as long as
    // it only knew the knob. -1 until this slot has rendered a table.
    float displayPosition() const { return dispPos_; }
    // The rubbed bodies' modes (SourcesNear.cpp): six, as ratios to the lowest.
    static constexpr int kRubModes = 6;

private:
    void renderWavetable(float* out, int n, double hz, const SlotParams& p, const Wavetable* table, float dt, float* outR = nullptr);
    void renderCycles(float* out, int n, double hz, const SlotParams& p, const CycleTable* table, float dt, float* outR = nullptr);
    void renderAdditive(float* out, int n, double hz, const SlotParams& p, float dt);
    // The phasor bank's block: targets, then the sum. `uni` copies of the spectrum end to end;
    // with more than one and an `outR`, they are spread across the field and it writes stereo.
    void renderBank(const float* spec, int H, int n, float* out, int uni = 1, float* outR = nullptr);
    // Rotations and stereo weights for `uni` copies around `hz`, detuned by `cents` end to end and
    // placed across `width`. Returns how many partials of one copy fit below Nyquist.
    int  setBankPitch(double hz, const SlotParams& p, int partials);
    void renderFm(float* out, int n, double hz, const SlotParams& p, float dt);
    void renderTexture(float* outL, int n, double hz, double speed, const SlotParams& p, const Texture* tex, float dt);
    void renderNoise(float* outL, int n, double hz, const SlotParams& p, float dt);
    void renderStretch(float* out, int n, double hz, double speed, const SlotParams& p, const Texture* tex, float dt);
    void renderBow(float* out, int n, double hz, const SlotParams& p, float dt);
    // The near sources (SourcesNear.cpp), all mono into `out`; their place in the field is the
    // slot's Pan like every other mono type.
    void renderFlute(float* out, int n, double hz, const SlotParams& p, float dt);
    void renderRub(float* out, int n, double hz, const SlotParams& p, float dt, bool ice);
    void renderMurmur(float* out, int n, double hz, const SlotParams& p, float dt);
    void renderDrops(float* out, int n, double hz, const SlotParams& p, float dt);
    void renderWhistler(float* out, int n, double hz, const SlotParams& p, float dt);
    void renderShaker(float* out, int n, double hz, const SlotParams& p, float dt);
    void renderChime(float* out, int n, double hz, const SlotParams& p, float dt);
    void renderGeiger(float* out, int n, double hz, const SlotParams& p, float dt);
    void renderTube(float* out, int n, double hz, const SlotParams& p, float dt);
    void renderKrell(float* out, int n, double hz, const SlotParams& p, float dt);
    void renderBeacon(float* out, int n, double hz, const SlotParams& p, float dt);
    void renderMorse(float* out, int n, double hz, const SlotParams& p, float dt);
    void renderDial(float* out, int n, double hz, const SlotParams& p, float dt);
    // Clip: the slot's recording played straight through, once, from Position -- a voice on a
    // radio, a launch, a recording of Mars -- at its own speed (Pitch = Free) or pitched to the
    // note. Writes left into outL and right into scratch_, as the granular Texture does.
    void renderClip(float* outL, int n, double hz, double speed, const SlotParams& p, const Texture* tex, float dt);
    void renderSpectral(float* out, int n, double transpose, const SlotParams& p, const Texture* tex, float dt);
    void stretchFrame(const SlotParams& p, const Texture* tex, double rate, int N);
    static const RealFft& stretchFft(int n);   // shared, read-only after prepare(): one per size

    // Wavetable: phasor bank like Voice::Strand.
    static constexpr int kBank = kTablePartials * kSlotUnison;
    float  pc_[kBank] = {}, ps_[kBank] = {};
    float  rc_[kBank] = {}, rs_[kBank] = {};
    float  amp_[kBank] = {}, ampStep_[kBank] = {};
    float  wL_[kBank] = {}, wR_[kBank] = {};   // each copy's place in the field
    bool   bankStereo_ = false;                // set when more than one copy is sounding
    int    active_ = 0;
    // Wavetable (CycleTable): a phase per unison copy, the level and position the last block read,
    // and a stream of its own for the start phases, so the type draws nothing from the slot's dice.
    double cyPhase_[kSlotUnison] = {};
    float  cyPos_ = 0.0f;
    int    cyLevel_ = 0;
    bool   cyPrimed_ = false;
    Rng    cyRng_;
    // Additive: per-partial shimmer and the cached tilt/odd-even shape
    Drifter shim_[kTablePartials];
    float  tiltCache_[kTablePartials] = {};
    float  cTilt_ = -1.0f, cOdd_ = -9.0f;
    int    cPartials_ = -1;
    // FM
    double phC_ = 0.0, phM_ = 0.0;
    float  hpX_ = 0.0f, hpY_ = 0.0f;   // DC blocker state
    // Texture grains
    // The window is a rotating phasor, not a cosine call: at 64 grains a std::cos per sample per
    // grain is the single most expensive thing in the voice.
    // `start`: where in the block this grain begins. A grain is due at a moment the spawn clock
    // knows to the sample, and it used to begin at sample 0 of the block regardless -- so every
    // grain of a dense cloud started on one of 750 instants a second, and the cloud grew a comb
    // at the block rate that nothing in the music put there.
    struct Grain { double pos = 0.0; double rate = 1.0; int len = 0; int age = 0; int start = 0; float gain = 0.0f;
                   float gl = 0.0f, gr = 0.0f; float wc = 1.0f, ws = 0.0f, rc = 1.0f, rs = 0.0f; bool on = false; };
    Grain  grains_[kSlotGrains];
    // The live ones are grains_[0 .. live_), with no gaps. The array used to be a set of
    // slots with an `on` flag, which meant walking all 128 of them per block to find the two
    // that were sounding -- seven kilobytes touched to do nothing, per slot, per voice, per
    // block. A grain that dies is swapped with the last live one and the count drops, which
    // costs one copy and keeps the walk exactly as long as there is work in it. Spawning is
    // then an append rather than a search for a free slot.
    int    live_ = 0;
    double spawnIn_ = 0.0;   // seconds until the next grain
public:
    // Noise: one generator per side, so the two channels are fully decorrelated -- which is what
    // makes a noise bed sit around the listener instead of in the middle of the head.
    struct NoiseState {
        float pink[7] = {};          // Kellet's economy pink filter
        float brown = 0.0f;
        float prev = 0.0f;           // for the differentiated colours
        float bp1 = 0.0f, bp2 = 0.0f;   // state-variable band pass
        float hold = 0.0f;           // sample and hold
        double holdLeft = 0.0;
        double nextGrain = 0.0;      // crackle
        float crackle = 0.0f, crackleDecay = 0.0f;
        // Cicada: two insects per ear, each either in a burst of pulses or in the pause between
        // bursts, with a slow breathing of its own; the pulses ring in the band pass above.
        double burstLeft[2] = {}, pauseLeft[2] = {}, pulseLeft[2] = {};
        float  breathe[2] = { 1.0f, 1.0f }, breatheTo[2] = { 1.0f, 1.0f };
        float  pulseRate[2] = { 80.0f, 95.0f };
    };
private:
    NoiseState noise_[2];
    // Stretch: an overlap-add ring twice the longest window, the transform pair, and where the
    // analysis reads in the clip. Allocated in prepare(), never in render().
    // Bow: one period of string, its loop filter's state, and whether it has been started.
    static constexpr int kBowMax = 4096;   // 12 Hz at 48 kHz
    // In vectors, not in the object: two arrays of this size per slot is thirty-two kilobytes,
    // and four slots in each of many voices put an Engine built on the stack straight through it.
    std::vector<float> bowNut_, bowBridge_;
    int   bowW_ = 0;
    float bowLp_ = 0.0f;
    bool  bowReady_ = false;
    // Flute: the same two lines read as the pipe's bore and the air jet's travel (a slot is one
    // type at a time, so the string's memory is the pipe's), the reflection filter's state, the
    // breath rising at the start, the jet's noise, a DC blocker and the vibrato clock.
    bool   fluteReady_ = false;
    float  fluteRefl_ = 0.0f, fluteBreath_ = 0.0f, fluteHpX_ = 0.0f, fluteHpY_ = 0.0f, fluteJetLp_ = 0.0f;
    float  fluteNoiseBp1_ = 0.0f, fluteNoiseBp2_ = 0.0f;
    double flutePhase_ = 0.0, fluteVibHz_ = 5.0;
    // Bowl and Ice: six modes, each a pair of resonators a hair apart (the doublet every real
    // bowl has, which is where its beating comes from), and the ice's slip clock.
    float  rubY1_[kRubModes][2] = {}, rubY2_[kRubModes][2] = {};
    float  rubF1_ = 0.0f, rubF2_ = 0.0f;   // the friction force's last two values (the modes' zero at DC)
    double rubSlipLeft_ = 0.0, rubSlipOn_ = 0.0, rubContact_ = 0.0;
    bool   rubReady_ = false;
    // Murmur: the glottal pulse clock, the three formants, the syllable and phrase machine, the
    // radio chain's filters and the squelch and beep clocks.
    double murPhase_ = 0.0, murSylLeft_ = 0.0, murPauseLeft_ = 0.0;
    double murBeepLeft_ = 0.0, murSquelchLeft_ = 0.0, murBeepPhase_ = 0.0;
    float  murF_[3] = { 500.0f, 1500.0f, 2500.0f }, murTo_[3] = { 500.0f, 1500.0f, 2500.0f };
    float  murGlot1_ = 0.0f, murGlot2_ = 0.0f, murPitchSt_ = 0.0f, murPitchTo_ = 0.0f, murDecl_ = 0.0f;
    float  murVoice_ = 1.0f, murVoiceTo_ = 1.0f, murGap_ = 1.0f, murGapTo_ = 1.0f, murJitter_ = 1.0f;
    float  murBeepHz_ = 2525.0f, murHpX_ = 0.0f, murHpY_ = 0.0f, murHiss_ = 0.0f;
    int    murSyllables_ = 0;
    bool   murInPhrase_ = false, murReady_ = false;
    Svf    murForm_[3], murRadioHp_, murRadioLp_, murFric_;
    // Drops: up to eight falling at once, and the vessel they fall into (two resonators).
    static constexpr int kDrops = 8;
    struct Drop { double phase = 0.0, hz = 0.0, rise = 0.0; float amp = 0.0f, decay = 0.0f; int left = 0; bool on = false; };
    Drop   drops_[kDrops];
    double dropNext_ = 0.0;
    float  dropVesselY1_[2] = {}, dropVesselY2_[2] = {}, dropClick_ = 0.0f;
    int    dropClickLeft_ = 0;
    // Clip: where the read head stands in the recording (-1 until the note starts it at Position),
    // and whether it has reached the end.
    double clipPos_ = -1.0;
    bool   clipDone_ = false;
    // The signals: one set of clocks and memories that each of the nine reads its own way (a slot
    // is one type at a time). sigT_ is the time since the note began; the phases, the two
    // resonators' memories, an energy, a Poisson clock, a burst, counters and a bit pattern.
    double   sigT_ = 0.0, sigPhase_ = 0.0, sigPhase2_ = 0.0, sigPhase3_ = 0.0;
    float    sigY1_ = 0.0f, sigY2_ = 0.0f, sigZ1_ = 0.0f, sigZ2_ = 0.0f, sigLp_ = 0.0f, sigEnergy_ = 0.0f;
    double   sigNext_ = 0.0, sigBurst_ = 0.0, sigFlutter_ = 0.0, sigFlutterHz_ = 1.0;
    int      sigCount_ = 0, sigState_ = 0, sigPos_ = 0;
    unsigned sigBits_ = 0u;
    double   krX_ = 0.1, krY_ = 0.0, krZ_ = 0.0;           // the Rossler system's state
    float    chY1_[6] = {}, chY2_[6] = {};                  // the chime's modes
    double   dlF_[3] = {}, dlTo_[3] = {}, dlPh_[3] = {};    // the dial's three whistles
    bool     sigReady_ = false;

    // Spectral: the two amplitude ramps per band (the partial and its noise), the state of the
    // band's noise filter, and how far the read head has travelled from Position, in frames.
    float specA_[kTablePartials] = {}, specAStep_[kTablePartials] = {};
    float specN_[kTablePartials] = {}, specNStep_[kTablePartials] = {};
    float specBp_[kTablePartials] = {}, specLp_[kTablePartials] = {};
    float specF_[kTablePartials] = {}, specQ_[kTablePartials] = {};
    double specAdvance_ = 0.0;

    struct StretchState {
        std::vector<float> out, re, im, win;
        int    n = 0;            // the window in use (0 = none yet)
        int    outPos = 0, hopLeft = 0;
        double advance = 0.0;    // how far the read has travelled from Position, in clip samples
    };
    StretchState st_;
    Drifter noiseDrift_;
    Drifter posDrift_, idxDrift_, pitchDrift_;
    float   dispPos_ = -1.0f;    // the position last read, for the picture (see displayPosition)
    Rng    rng_;
    double sr_ = 48000.0;
    float  gL_ = 0.0f, gR_ = 0.0f;
    float  scratch_[64] = {};
    float  scratchR_[64] = {};   // the bank's right channel when its copies are spread
    SourceType lastType_ = SourceType::Off;
};

} // namespace ambient
