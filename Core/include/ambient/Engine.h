// Noctuary -- the engine: voices, cluster brain, spatial routing, effects, parameters.
// Framework-free. `process()` never allocates; parameters are plain atomics so the
// host layer can write them from any thread.
//
// Signal flow:
//   voices -> near bus (dry plane) -> ensemble -> stereo delay -> near reverb ----+
//          -> far bus (reverb send) + delay "to far" -> far reverb (dark, wide) --+-> mid/side -> master
#pragma once
#include "Params.h"
#include "Tuning.h"
#include "Voice.h"
#include "Effects.h"
#include "Cloud.h"
#include "Memory.h"
#include "Cosmos.h"
#include "Convolution.h"
#include "Body.h"
#include "Route.h"
#include "Modulation.h"
#include "Clock.h"
#include <functional>
#include "ClusterBrain.h"
#include "Near.h"
#include "Presets.h"
#include "Loudness.h"
#include <atomic>
#include <vector>
#include <cstdint>

namespace ambient {

// How much of the finished output the engine keeps for the displays to look at. A power of two:
// the ring is indexed with a mask.
constexpr int kOutTapLen = 16384;

class Engine {
public:
    static constexpr int kMaxVoices = 16;

    Engine();
    void prepare(double sampleRate, int maxBlockSize);
    void reset();

    void  setParam(ParamId id, float v) { params_[static_cast<int>(id)].store(v, std::memory_order_relaxed); }
    float getParam(ParamId id) const    { return params_[static_cast<int>(id)].load(std::memory_order_relaxed); }
    bool  applyPreset(int index);       // any thread; sets every parameter (full preset)
    // The conductors alone -- no voices, no audio -- for `seconds` at `dt`, every event handed to
    // `sink` with its time and which conductor made it (1 or 2), every root change to `rootSink`.
    // The rule book's own check (its section 11): an hour of notes, measured, with the parameters
    // exactly as the render reads them and seeded as the render seeds them. What it leaves out is
    // the matrix -- a route onto a brain parameter is not run here -- and the adaptive tuning.
    void  auditConductor(double seconds, double dt,
                         const std::function<void(double, int, const BrainEvent&)>& sink,
                         const std::function<void(double, int)>& rootSink);
    bool  applySoundPreset(int index);  // sound layer only: everything except the Cosmos section
    bool  applyCosmosPreset(int index); // Cosmos layer only, from the Cosmos preset bank
    bool  applyZPreset(int index);      // Z-plane layer only: the filter and where its point sits
    bool  applyStrikePreset(int index); // Strike layer only: the Karplus-Strong pluck
    bool  applyNearPreset(int index);   // the near layer only: the Near Source and the Near Events
    // The near events (Near.h): whether one is sounding, how far it has come, how many there
    // have been; and the whole scheduler, so an engine taking over at a preset change can carry
    // on the sequence that was running rather than start the foreground again.
    bool  nearActive() const { return near_.active(); }
    float nearProgress() const { return near_.progress(); }
    int   nearEventsPlayed() const { return near_.events(); }
    int   nearMutations() const { return near_.mutations(); }
    const NearEvents& nearState() const { return near_; }
    void  adoptNear(const NearEvents& n) { near_ = n; }

    // MIDI, audio thread only.
    void noteOn(int note, float velocity);
    void noteOff(int note);
    void allNotesOff();
    // Autoplay: exchange a voice of the cluster now, whatever its timer says. Any thread.
    void autoplayStep() { autoStepAsked_.store(true, std::memory_order_relaxed); }
    // Per-note expression (MPE, polyphonic aftertouch, CC 74). `note` < 0 addresses every
    // sounding voice, which is what a plain keyboard's channel pressure and wheel mean.
    void setPressure(int note, float v);
    void setSlide(int note, float v);
    void setBend(int note, float normalised);   // -1 .. 1, scaled by the Bend Range parameter
    // The mod wheel (CC 1), 0..1. It belongs to the instrument rather than to a note, and reaches
    // the sound only through the matrix -- there is no fixed route from it.
    void setWheel(float v);
    // The head's yaw in degrees (0 ahead, positive to the right), from a headset or an OSC head
    // tracker. Only the Headphones binaural mode listens to it.
    void setHeadYaw(float degrees);

    // Renders `n` stereo samples (replaces L/R). Any n; larger than maxBlockSize is chunked.
    void process(float* L, float* R, int n);

    // Stems. Pass eight pointers -- near L/R, far L/R, cosmos L/R, room L/R -- each with room for
    // the n of the next process() call, and they are filled alongside the mix; pass nullptr to
    // stop. What is in them is what each plane contributes to the output at the point it joins
    // it, so the four sum to the mix before the master stage (the Cosmos path's own send into
    // the far plane is part of the far stem, where it is heard).
    static constexpr int kNumStems = 4;
    static const char* stemName(int i);
    void setStemBuffers(float* const* eightPointers) { stems_ = eightPointers; }

    // Any thread. Applied at the start of the next block.
    void setUserScale(const FixedScale& s);
    // User wavetable (Table = User in a source slot) and texture (Type = Texture), message
    // thread. The texture is double-buffered: the call waits (at most one block) until the
    // audio thread has left the buffer it is about to overwrite, then swaps.
    void setUserWavetable(const Wavetable& t);
    // The same file as single cycles, for the Wavetable type. Double-buffered like a texture rather
    // than copied on the audio thread: 256 frames at ten resolutions are megabytes.
    void setUserCycles(const CycleTable& t);
    // Reads the frames both ways -- spectra for the Harmonic type, cycles for the Wavetable type --
    // and sets both. `frameLen` is one cycle's length; readWavetableFile finds it for a file.
    bool loadUserWavetable(const float* mono, int n, int frameLen = 2048);
    // One clip per source slot, so four slots typed Texture (or Stretch) can play four different
    // recordings -- which is what makes the Vector's four corners four landscapes. The slotless
    // form loads the same clip into every slot: what the instrument always did, and what a preset
    // that names one file still means.
    void setTexture(int slot, const float* mono, int n, double sampleRate, double baseHz = 261.6256, bool seamless = false);
    void setTexture(const float* mono, int n, double sampleRate, double baseHz = 261.6256, bool seamless = false)
    { for (int k = 0; k < kSlots; ++k) setTexture(k, mono, n, sampleRate, baseHz, seamless); }
    // The same clip with both its channels. `R` may be null, which is the mono form above; when it
    // is not, the granular source reads the pair and keeps the recording's own image instead of
    // playing its mono sum. The mono sum is still made here, because the band model, the Paulstretch
    // and the Spectral resynthesis are all built on it.
    void setTexture(int slot, const float* L, const float* R, int n, double sampleRate, double baseHz = 261.6256, bool seamless = false);
    void setTexture(const float* L, const float* R, int n, double sampleRate, double baseHz = 261.6256, bool seamless = false)
    { for (int k = 0; k < kSlots; ++k) setTexture(k, L, R, n, sampleRate, baseHz, seamless); }
    // A clip that has already been read and measured, handed from one engine to another (a preset
    // change builds a new engine and the player's own sample has to survive it). Copies the band
    // model with it rather than measuring the same clip a second time.
    void setTexture(int slot, const Texture& src);
    void clearTexture(int slot);
    // The near source's own clips (13.09.2026): what a near event plays when its type reads a
    // recording -- a voice off a radio loop, a launch, the wind on Mars. A pool rather than one
    // clip, because a preset may name a folder of phrases and every event then takes one of
    // them, at random and never the one just played; a single clip is a pool of one. Its own
    // buffers, so the near layer's presets carry their clips and the sound preset's four slots
    // keep theirs; a near source with no clip of its own reads Source 4's. Double-buffered like
    // the slots' clips: the message thread fills the pool that is not active and flips the index.
    static Texture makeTexture(const float* L, const float* R, int n, double sampleRate, double baseHz = 261.6256,
                               bool seamless = false, double maxSeconds = 120.0);
    void setNearTexture(const float* L, const float* R, int n, double sampleRate, double baseHz = 261.6256, bool seamless = false);
    void setNearTexture(const Texture& src);
    void setNearTextures(std::vector<Texture> pool);   // the whole pool at once, moved in
    void clearNearTexture() { nearPoolActive_.store(-1, std::memory_order_release); }
    bool hasNearTexture() const { return displayNearPool() != nullptr; }
    int  nearTextureCount() const { const std::vector<Texture>* p = displayNearPool(); return p != nullptr ? static_cast<int>(p->size()) : 0; }
    const std::vector<Texture>* displayNearPool() const
    {
        const int a = nearPoolActive_.load(std::memory_order_relaxed);
        return a >= 0 && !nearPools_[a].empty() ? &nearPools_[a] : nullptr;
    }
    const Texture* displayNearTexture() const   // the clip the current (or next) event plays
    {
        const std::vector<Texture>* p = displayNearPool();
        if (p == nullptr) return nullptr;
        return &(*p)[std::min(static_cast<size_t>(std::max(0, nearPick_)), p->size() - 1)];
    }
    bool hasTexture(int slot = 0) const
    { return textureActive_[slot < 0 ? 0 : (slot >= kSlots ? kSlots - 1 : slot)].load(std::memory_order_relaxed) >= 0; }
    // Convolution room: a stereo (R may be null) impulse response, message thread.
    void  setImpulse(const float* L, const float* R, int n, double sampleRate) { room_.setImpulse(L, R, n, sampleRate); userImpulse_ = true; }
    // The room's second impulse: Room Morph blends it into the first inside the one convolution,
    // so a morph costs what one room costs.
    void  setImpulseB(const float* L, const float* R, int n, double sampleRate) { room_.setImpulseB(L, R, n, sampleRate); hasImpulseB_ = true; }
    bool  hasImpulseB() const { return hasImpulseB_; }
    // The second impulse gone: Room Morph has nothing to go to, and the room is A alone.
    void  clearImpulseB() { hasImpulseB_ = false; room_.clearImpulseB(); }
    float impulseSeconds() const { return room_.impulseSeconds(); }
    bool  hasUserImpulse() const { return userImpulse_; }
    // Longest impulse the Room keeps (memory grows with it: about 46 MB of delay line for a
    // minute); call before prepare(). A minute on the desktop; the Quest app sets 4 s, measured
    // there with the convolver before this one (a third of one of its cores).
    void  setRoomMaxSeconds(float s) { roomMaxSeconds_ = clampv(s, 0.5f, 60.0f); }
    float roomMaxSeconds() const { return roomMaxSeconds_; }
    int  userWavetableFrames() const { return userTableFrames_.load(std::memory_order_relaxed); }
    // For the displays (message thread, no synchronisation -- a torn read costs a pixel).
    const Wavetable* userWavetable() const { return userTable_.frames > 0 ? &userTable_ : nullptr; }
    const CycleTable* userCycles() const
    {
        const int a = cyclesActive_.load(std::memory_order_relaxed);
        return (a >= 0 && !userCycles_[a].empty()) ? &userCycles_[a] : nullptr;
    }
    const Texture*   displayTexture(int slot = 0) const
    {
        const int k = slot < 0 ? 0 : (slot >= kSlots ? kSlots - 1 : slot);
        const int a = textureActive_[k].load(std::memory_order_relaxed);
        return a >= 0 ? &textures_[k][a] : nullptr;
    }

    // Morph: two full parameter snapshots (A = 0, B = 1). While MorphActive is on the
    // engine plays lerp(A, B, position); the position glides toward MorphPos at
    // 1/MorphGlide per second. Meant for a hand in VR: one continuous gesture moves
    // the whole instrument from one world to another.
    void  setMorphSlot(int slot, const float* values);   // kNumParams values, any thread
    void  captureMorphSlot(int slot);                    // copy the live parameters into a slot
    void  morphSlot(int slot, float* out) const;
    // A slot nobody has chosen is not a snapshot of anything, and plays as the knobs stand.
    // Until 13.09.2026 both slots began at the defaults, so switching Morph on before choosing
    // A or B held the whole instrument at Init: every knob dead, the near layer silent, a texture
    // preset without its texture. setMorphSlot and captureMorphSlot choose a slot; this forgets it.
    void  clearMorphSlot(int slot);
    bool  morphSlotSet(int slot) const { return slotSet_[slot & 1].load(std::memory_order_relaxed); }
    float morphPosition() const { return morphCur_.load(std::memory_order_relaxed); }
    // Put the gliding position somewhere without waiting for the glide: a preset change
    // that travels has to start at A even if the morph was standing somewhere else.
    void  resetMorphPosition(float p) { morphCur_.store(p, std::memory_order_relaxed); }
    float effectiveParam(ParamId id) const;              // what is actually playing
    // Preset map (MapActive / MapX / MapY / MapRadius): the engine blends the presets around
    // the cursor at control rate and glides every parameter toward the blend with the Morph
    // Glide time constant. While active it overrides the live parameters and the morph.
    // blendValue() is the gliding value the host should copy back into its parameters when
    // the map is switched off, so the sound stays where the map left it.
    float blendValue(ParamId id) const { return blendCur_[static_cast<int>(id)].load(std::memory_order_relaxed); }
    bool  mapActive() const { return blendActive_.load(std::memory_order_relaxed); }
    // Route over the map. The host calls routeStep() once per block (dt in seconds, before its
    // own speed scaling): while RouteActive the route moves the cursor and the engine plays the
    // map blend; the new cursor is returned so the host can mirror it into its parameters.
    // The route text is performance state (plugin state / OSC / Quest config), not a preset.
    // The audio thread walks route_; every edit goes into routePending_ and is published with a
    // version, the way the user scale and the texture already are. The lock that used to sit in
    // the plugin protected nothing: routeStep never took it.
    const Route& route() const { return route_; }              // what is playing (message thread may read)
    const Route& routeEdit() const { return routePending_; }   // what was last edited
    bool setRouteText(const char* text)
    { if (!routePending_.parse(text)) return false; routeVersion_.fetch_add(1, std::memory_order_release); return true; }
    bool addRoutePoint(const Waypoint& w)
    { if (!routePending_.add(w)) return false; routeVersion_.fetch_add(1, std::memory_order_release); return true; }
    void clearRoute() { routePending_.clear(); routeVersion_.fetch_add(1, std::memory_order_release); }
    int  writeRoute(char* buf, size_t cap) const { return routePending_.write(buf, cap); }
    bool routeStep(double dt, float& x, float& y, float& radius);
    bool routeRunning() const { return route_.running(); }
    bool asleep() const { return asleep_; }   // no voice and no tail for two seconds: effects skipped
    float coherencePhase(int i) const { return kuraPhase_[i & 3]; }   // Kuramoto oscillator phases, for pictures
    // The Lenia field, for pictures and tests: a cell (0..1), how many full steps it has taken,
    // how often it had to be reseeded (dead or saturated), and the four readings the matrix sees.
    static constexpr int kLeniaSize = 32;
    float leniaCell(int x, int y) const { return lenia_[((y % kLeniaSize + kLeniaSize) % kLeniaSize) * kLeniaSize + ((x % kLeniaSize + kLeniaSize) % kLeniaSize)]; }
    int   leniaSteps() const { return leniaSteps_; }
    int   leniaReseeds() const { return leniaReseeds_; }
    float leniaOut(int k) const { return leniaOut_[k & 3]; }
    // The attractors, for the tests: how many blocks they have been integrated, and a reading.
    int   chaosSteps() const { return chaosSteps_; }
    float chaosOut(int k) const { return chaosOut_[k % 6]; }

    // ---- clock (Clock.h). A host with a play head calls setHostClock() once per block, before
    // process(); MIDI clock messages arrive through midiClock*() (audio thread). Which of them
    // the engine follows is the ClockSource parameter; without either it runs its own tempo.
    void setHostClock(double bpm, double beatPosition, bool playing)
    { hostBpm_ = bpm; hostBeat_ = beatPosition; hostPlaying_ = playing; hostSeen_ = true; }
    void midiClockTick(double secondsSinceLastTick);   // one 0xF8; the interval may be 0 when unknown
    void midiClockStart();
    void midiClockContinue();
    void midiClockStop();
    // What the engine is following right now, for displays.
    double tempo() const        { return tempoOut_.load(std::memory_order_relaxed); }
    double beatPosition() const { return beatOut_.load(std::memory_order_relaxed); }
    bool   clockRunning() const { return runningOut_.load(std::memory_order_relaxed); }

    // ---- modulation (message thread for the setters; see Modulation.h for the text forms)
    // The matrix rows and the envelope shapes are data, published the way the route and the user
    // scale are: written into a pending copy and picked up at the next block.
    void resetModulation();          // the matrix cleared, the six envelopes back to default
    bool applyPresetModulation(const Preset& p);   // the mod and envs fields of a preset
private:
    void lockModulation();           // take the pending matrix and shapes (message thread waits)
    void unlockModulation();
    void clearPendingModulation();   // clear without announcing; call with the lock held
    void publishModulation();        // announce, once everything is written; with the lock held
public:
    bool setModMatrixText(const char* text);
    int  writeModMatrix(char* buf, size_t cap) const { return matrixPending_.write(buf, cap); }
    const ModMatrix& modMatrix() const { return matrixPending_; }
    bool setEnvShape(int index, const char* text);
    int  writeEnvShape(int index, char* buf, size_t cap) const;
    const ModEnv& envShape(int index) const { return envPending_[index < 0 ? 0 : (index >= kNumModEnvs ? kNumModEnvs - 1 : index)]; }
    // Each source's own envelope shape, for a slot whose Env is Own: the same text form, read as a
    // level from 0 (silent) to 1. Travels after the six in a preset's envelope field (Presets.h).
    bool setSrcEnvShape(int slot, const char* text);
    int  writeSrcEnvShape(int slot, char* buf, size_t cap) const;
    const ModEnv& srcEnvShape(int slot) const { return srcEnvPending_[slot < 0 ? 0 : (slot >= kSlots ? kSlots - 1 : slot)]; }
    // For the editor's playhead: where the loudest voice is reading a slot's entrance shape
    // (seconds of the shape) and the gain the slot has from its entrance. The gain is written
    // whenever a voice sounds; the result says whether the slot is following a shape at all.
    bool displaySlotEnv(int slot, float& shapeSeconds, float& gain) const;
    // For the displays: the current value of every source, and each LFO's phase.
    float modSource(int source) const { return (source >= 0 && source < kNumModSources) ? modSrc_[source] : 0.0f; }
    // How fast the Beat source is turning, in hertz: zero when the chord is in tune.
    float beatRate() const { return beatHz_; }
    float lfoPhase(int i) const { return lfo_[i < 0 ? 0 : (i >= kNumLfos ? kNumLfos - 1 : i)].phase(); }
    const Lfo& lfo(int i) const { return lfo_[i < 0 ? 0 : (i >= kNumLfos ? kNumLfos - 1 : i)]; }
    // Each modulation envelope has its own clock, because Sustain Loop stops one of them where
    // the others keep running. Index-safe like the rest of these display accessors.
    // Loudness of what is leaving the instrument, to BS.1770. Any thread; the numbers are
    // written once a block and read as a snapshot.
    LoudnessReading loudness() const { return loudness_.read(); }
    void resetLoudness() { loudness_.reset(); }
    float envTime(int i = 0) const { return static_cast<float>(envTime_[i < 0 ? 0 : (i >= kNumModEnvs ? kNumModEnvs - 1 : i)]); }
    // How much the matrix is currently adding to a parameter, in that parameter's own units.
    float modAmount(ParamId id) const { return modOut_[static_cast<int>(id)]; }
    // Partial amplitudes of the loudest sounding voice, for the oscillator display. Returns how
    // many were written, 0 when nothing sounds. Message thread, no synchronisation (see Voice.h).
    int  displayPartials(float* out, int maxCount) const;
    float displayFrequency() const;   // that voice's frequency in Hz, 0 when nothing sounds
    // The same voice's slot bank (Additive / Wavetable in a slot) and its grains (Texture).
    int  displaySlotPartials(int slot, float* out, int maxCount) const;
    // Where that slot is reading its table right now -- the knob, the matrix and the slot's own
    // Pos Drift together. -1 when nothing sounds or the slot plays no table: the display then
    // falls back to the knob, which is all it ever knew before.
    float displaySlotPosition(int slot) const;
    int  displayGrains(int slot, SourceSlot::GrainInfo* out, int maxCount) const;
    // Every sounding voice's place, for the stage picture: pan -1..1, distance 0 near .. 1 far,
    // envelope level, note, owner (0 keys, 1 brain). Returns how many were written.
    struct VoiceStage { float pan, distance, level; int note, owner; };
    int  voiceStage(VoiceStage* out, int maxCount) const;
    // The last n (<= 4096) samples of the Cosmos return, mono, oldest first -- for its spectrum.
    int  cosmosTap(float* out, int n) const;
    // The same for the finished output, after the master gain and the clipper.
    int  outputTap(float* out, int n) const;


    const FixedScale& scale() const { return *scale_; }
    // The Scala slot itself, whatever the Scale parameter happens to be pointing at: a preset
    // chooses its own scale, and that must not be read as "the tuning the player loaded is gone".
    const FixedScale& userScale() const { return scales_[kUserScaleIndex]; }
    double frequencyOf(int note) const;
    double sampleRate() const { return sr_; }

    // Observers for the GUI (approximate, lock-free).
    int  activeVoices() const { return activeVoices_.load(std::memory_order_relaxed); }
    // Every strike the instrument has played since it was prepared, keys and conductor together.
    unsigned strikesFired() const { unsigned n = 0; for (const auto& v : voices_) n += v.strikes(); return n; }
    int  brainRoot() const    { return brainRoot_.load(std::memory_order_relaxed); }
    // The key the conductor has found itself in, measured from what has been sounding and
    // for how long. Read on the message thread for the panel; never set from outside.
    // Both conductors fill their clusters now rather than at their event rate. Used when this
    // engine takes over from one that was already sounding; see ClusterBrain::requestFill.
    void requestBrainFill() { brain_.requestFill(); brain2_.requestFill(); }
    // Read out and hand over what the conductors are holding; see ClusterBrain::adopt. The two
    // are kept apart: the background conductor's cluster belongs to the background.
    int soundingCluster(int* notes, float* vels, bool second = false) const
    { return (second ? brain2_ : brain_).soundingNotes(notes, vels); }
    void adoptCluster(const int* notes, const float* vels, int count, bool second = false);
    KeyEstimate brainKey() const { return brain_.estimatedKey(); }
    // The arc's value when it follows the clock instead of its own drift: the night's bottom at
    // four in the morning, its top at four in the afternoon, a cosine between. Pure, so the
    // selftest can hold it to that.
    static float clockArcValue(double hourOfDay)
    {
        return static_cast<float>(-std::cos(2.0 * 3.14159265358979323846 * (hourOfDay - 4.0) / 24.0));
    }
    float arcNow() const { return arcOut_; }
    // The beat rate the BEAT source is following, the arc's current lean on the harmony, and the
    // factor the fluctuation guard is applying to the purity drift. For the panel and the tests.
    float beatHz() const { return beatHz_; }
    float arcLean() const { return arcLean_; }
    float guardFactor() const { return guardFactor_; }
    // The conductor's homeostat lean and its measured interval entropy, and the adaptive
    // intonation's common offset in cents. For the panel and the tests.
    float  brainLean() const { return brain_.lean(); }
    float  brainEntropyBits() const { return brain_.entropyBits(); }
    double commaCents() const { return commaCents_; }
    // For the panel's Tuning and Coherence displays: the timbre's partial template (the roughness
    // curve is drawn from it), the tide's current offset in cents, the conductor's deja-vu ring
    // and its cascade excitation.
    const BrainSpectrum& brainSpectrum() const { return brainSpec_; }
    float  tideNow() const { return tide_ * tideDrift_.value(); }
    int    brainRingNote(int i) const { return brain_.ringNote(i); }
    int    brainRingPos() const { return brain_.ringPos(); }
    float  brainExcitation() const { return static_cast<float>(brain_.excitation()); }
    bool   arcClockOn() const { return arcClock_; }
    double clockHour() const { return clockHour_; }
    // Pin the hour the arc follows instead of asking the operating system. An instrument that
    // knows what time it is cannot be measured: five hundred presets in the library have the arc
    // clock on, and their descriptors came out different every time the pass ran, because the
    // sound really was different at four in the afternoon and at midnight. The measurement pins
    // it; playing does not (-1 = the wall clock, as always).
    void   setClockHourOverride(double h) { clockHourOverride_ = h; }
    // The offset, in cents, that tunes `note` pure against what is sounding now (0 if nothing is).
    float  adaptiveOffset(int note) const;
    // What Match would make of partial h (1-based) against the current scale: the ratio to f0.
    double matchedPartialRatio(int h) const;
    float arcValue() const    { return arcValue_.load(std::memory_order_relaxed); }
    void soundingNotes(bool (&out)[128]) const;
    // Distance (0 near .. 1 far) of the voice sounding `note`, or -1 if none.
    float noteDistance(int note) const;
    // Envelope level (0..1) of the loudest voice on `note`, 0 if none.
    float noteLevel(int note) const { return (note >= 0 && note < 128) ? noteLevel_[note].load(std::memory_order_relaxed) : 0.0f; }

private:
    enum Owner { OwnerMidi = 0, OwnerBrain = 1, OwnerBrain2 = 2, OwnerNear = 3 };
    Voice* allocate(int note, int owner);
    // The near layer: an event's note started on the near source, what the scheduler asks for,
    // the places of the notes for the slot roles, and the scheduler stepped with what it needs
    // to know -- from the render and from the audit alike.
    void   startNearNote(const NearNote& e);
    void   nearEmit(const NearNote& e);
    void   updatePlaces();
    template <class EmitFn>
    void stepNear(double dt, EmitFn&& emit)
    {
        NearInputs in;
        in.root = brain_.root();
        float vels[ClusterBrain::kSlots];
        in.count = brain_.soundingNotes(in.cluster, vels);
        in.excitation = brain_.excitation();
        in.silence = brain_.inSilence();
        in.rootAge = brain_.rootAgeSeconds();
        in.bpm = bpm_;
        in.sinceOnset = brain_.sinceOnset();
        for (const auto& v : voices_) if (v.isActive() && v.isReleasing() && v.owner() != OwnerNear && v.owner() != OwnerMidi) { in.releasing = true; break; }
        near_.update(dt, np_, in, [this](int n) { return frequencyOf(n); },
                     [this](double fa, double fb) { return bp_.consonanceOf(fa, fb); }, emit);
        // What the foreground asks of the background: no new onset while a Note or a Phrase
        // speaks (and for a moment after), the root held and the pace halved under a sequence.
        const bool onsets = near_.holding(), seq = near_.sequenceRunning();
        brain_.holdOnsets(onsets);  brain2_.holdOnsets(onsets);
        brain_.holdRoot(seq);       brain2_.holdRoot(seq);
    }
    // `ageSeconds` is how long this note is to be treated as having been sounding already. It is
    // zero for a note that is played and only set where a cluster is handed from one engine to
    // another at a preset change: the note is not new there, it is the same note on another
    // instrument, and starting its Bloom and its source delays from zero would make the arriving
    // preset take half a minute to become itself.
    void   startNote(int note, float velocity, int owner, float distance, float ageSeconds = 0.0f);
    void   stopNote(int note, int owner);
    void   readParams();
    void   renderChunk(float* L, float* R, int n);

    std::atomic<float> params_[kNumParams];
    std::atomic<float> slotA_[kNumParams], slotB_[kNumParams];
    std::atomic<bool>  slotSet_[2] { false, false };   // chosen, or still following the knobs
    std::atomic<float> morphCur_{ 0.0f };
    // Preset map blend (audio thread writes, host reads)
    std::atomic<float> blendCur_[kNumParams];
    std::atomic<bool>  blendActive_{ false };
    float              blendTarget_[kNumParams] = {};
    void updateBlend(int n);
    Voice        voices_[kMaxVoices];
    ClusterBrain brain_, brain2_;
    BrainParams  bp2_;
    float        brain2Depth_ = 0.9f;
    float        layerDepth_ = 0.0f;   // how much of a note's plane comes from its role, not the draw
    float        cloudToNear_ = 0.0f;  // how much of the cloud comes back in front rather than behind
    int          brain2Interval_ = 0;
    bool         brain2On_ = false;
    int          brainQuant_ = 0;      // SyncDiv: the conductor's decisions land on the grid
    std::atomic<bool> autoStepAsked_{ false };   // the Step trigger, from the panel or a controller
    bool         autoStepWas_ = false;           // the parameter's previous state, for the rising edge
    double       quantAcc_ = 0.0, lastBeat_ = 0.0;
    float        bendRange_ = 2.0f;
    Ensemble     ensemble_;
    StereoDelay  delay_, delay2_;
    GrainCloud   cloud_;
    float        delay2Mix_ = 0.0f, delay2ToFar_ = 0.5f, cloudSend_ = 0.0f;
    // The Memory: a parallel send off the near bus like the Cosmos, with its own two returns.
    Memory       memory_;
    float        memSend_ = 0.0f, memReturn_ = 0.5f, memToFar_ = 0.4f;
    Smoother     smMemSend_, smMemReturn_, smMemToFar_;
    std::vector<float> memL_, memR_;
    Reverb       nearReverb_, farReverb_;
    Diffuser     diffuser_;
    std::vector<float> coupleBuf_;      // the previous block's foreground, for the sympathetic coupling
    float        sympathy_ = 0.0f;
    Unmask       unmask_;
    HaasBand     haas_;
    EarlyRoom    early_;
    Body         body_;
    Patina       patina_;
    float        bodyLevel_ = 0.0f, bodyPitch_ = 1.0f;
    Smoother     smBody_;
    MidSide      midSide_;
    // Room (convolution) on the far plane: level, source, pre-delay ring, tail low-pass
    Convolver    room_;
    bool         userImpulse_ = false, hasImpulseB_ = false;
    float        roomMorph_ = 0.0f;
    Smoother     smRoomMorph_;
    float        masterGain_ = -6.0f;
    float        roomMaxSeconds_ = 60.0f;
    // modulation
    Lfo          lfo_[kNumLfos];
    LfoSpec      lfoSpec_[kNumLfos];
    ModEnv       envShape_[kNumModEnvs], envPending_[kNumModEnvs];
    ModEnvSpec   envSpec_[kNumModEnvs];
    // The sources' own envelopes, published with the six under the same lock. A fresh one rises to
    // the source's level over its first second (times Time) and stays there.
    static constexpr const char* kSrcEnvDefault = "0:0/1:1";
    ModEnv       srcEnvShape_[kSlots], srcEnvPending_[kSlots];
    ModEnvSpec   srcEnvSpec_[kSlots];
    ModMatrix    matrix_, matrixPending_;
    std::atomic<int> modVersion_{ 0 };
    // Held by whoever is touching the pending matrix and shapes. The message thread waits for
    // it; the audio thread only tries, and leaves an edit in progress alone until the next block.
    std::atomic_flag modLock_ = ATOMIC_FLAG_INIT;
    int          modSeen_ = 0;
    float        modSrc_[kNumModSources] = {};
    float        modOut_[kNumParams] = {};
    double       envTime_[kNumModEnvs] = {};
    bool         envHeld_ = false;
    float        randomPerNote_ = 0.0f;
    bool         phraseStart_ = false;   // a note arrived into silence: envelopes and Retrigger LFOs restart
    // The wheel: where it was put, and where the modulation has got to. A controller sends 128
    // steps and a step on a cutoff is audible, so what the matrix reads is the smoothed one.
    float        wheelTarget_ = 0.0f, wheel_ = 0.0f;
    BrainSpectrum brainSpec_;     // the partial template the conductor judges intervals with (Timbre)
    // The stretched octave, in cents per octave away from the reference pitch (0 = exact 2:1).
    float        stretchCents_ = 0.0f;
    bool         stretchChanged_ = false;
    // The head's yaw in degrees, for the Headphones binaural mode: the whole field turns the
    // other way so a source stays where it is in the room. Written from any thread.
    std::atomic<float> headYawDeg_{ 0.0f };
    void         stepModulation(float dt);
    // clock: the three candidates and the one resolved for this block
    double       hostBpm_ = 0.0, hostBeat_ = 0.0;
    bool         hostPlaying_ = false, hostSeen_ = false;
    double       midiBpm_ = 0.0, midiBeat_ = 0.0, midiSilence_ = 0.0;
    int          midiTicks_ = 0;
    bool         midiRunning_ = true;
    double       intBeat_ = 0.0;
    double       bpm_ = 90.0, beat_ = 0.0;
    bool         running_ = true;
    std::atomic<double> tempoOut_{ 90.0 }, beatOut_{ 0.0 };
    std::atomic<bool>   runningOut_{ true };
    void         stepClock(double dt);

    Route        route_, routePending_;
    std::atomic<int> routeVersion_{ 0 };
    int          routeSeen_ = 0;
    bool         routeWasActive_ = false;
    float        roomLevel_ = 0.0f, roomLevelCur_ = 0.0f, roomHighcut_ = 5000.0f;
    float        roomLowcut_ = 20.0f;
    float        roomHpL1_ = 0.0f, roomHpR1_ = 0.0f, roomHpL2_ = 0.0f, roomHpR2_ = 0.0f;
    // Subsonic: two cascaded one-pole high-passes on the finished output, 24 dB/oct with the
    // master DC blocker in front of them. Zero hertz means the whole thing is skipped.
    LoudnessMeter loudness_;
    float        subsonicHz_ = 0.0f;
    float        subL1_ = 0.0f, subR1_ = 0.0f, subL2_ = 0.0f, subR2_ = 0.0f;
    float        subL3_ = 0.0f, subR3_ = 0.0f, subL4_ = 0.0f, subR4_ = 0.0f;
    int          roomSource_ = 0, roomPreDelay_ = 0;
    long         roomTailLeft_ = 0;
    std::vector<float> roomInL_, roomInR_, roomOutL_, roomOutR_, roomDelayL_, roomDelayR_;
    int          roomDelayW_ = 0, roomDelayMask_ = 0;
    float        roomLpL_ = 0.0f, roomLpR_ = 0.0f;
    // Cosmos path (parallel send from the near bus) and the shimmer loop around the far reverb.
    FreqShifter   shifter_;
    CombResonator resonator_;
    VowelFilter   vowel_;
    Nebula        nebula_;
    // Blur on the near bus (a second Nebula), the pitch tide, the turning far field. Their
    // drifters run on a side stream so enabling them never moves the brain's dice.
    Nebula        blur_;
    float         blurMix_ = 0.0f;
    Smoother      smBlur_;
    Drifter       tideDrift_, rotDrift_;
    float         tide_ = 0.0f, tidePeriod_ = 12.0f, farRotate_ = 0.0f, farWidth_ = 1.0f;
    // Envelopment: the far bus's low-mid side channel, between Bass Mono and 500 Hz.
    float         envelop_ = 0.0f;
    Svf           envBell_;       // the side channel's band, from Bass Mono to 500 Hz
    Smoother      smEnvelop_;
    // Comodulation. The background's gain follows one slow random envelope, shared by every
    // band of it at once. Hall, Haggard and Fernandes (1984): a tone in a noise whose bands
    // rise and fall together is heard ten to fifteen decibels further down than in a noise
    // whose bands move independently -- comodulation masking release. The ear groups what
    // moves together into one object and hears past it. Nothing in the foreground follows
    // the envelope, so the foreground is what is heard past it.
    float         comod_ = 0.0f;
    Smoother      smComod_;
    Drifter       comodDrift_;
    Rng           comodRng_;
    bool          comodInit_ = false;
    Rng           auxRng_;
    // The strike's own coin, on its own stream: drawn only when a chance below one asks for it,
    // so an instrument that strikes on every note renders exactly as it did before there was a
    // chance at all. (A draw taken from a shared stream would move every preset that follows.)
    Rng           strikeRng_;
    float         strikeChance_ = 1.0f, strikeCluster_ = 0.0f;
    // The near layer (13.09.2026): the scheduler, its parameters, and the voice parameters a near
    // event's voice renders with -- the sound preset's, with the four slots put out and the Near
    // Source in the last of them, and the section's own envelope, filter and strike.
    NearEvents    near_;
    NearParams    np_;
    VoiceParams   vpNear_;
    bool          rolesUsed_ = false;   // any slot with a role other than All: the places are then kept
    // The excitation the cascade has been running at lately, so Cluster can weigh a note against
    // the piece's own average rather than an absolute number: above it the strike grows likelier,
    // below it rarer, and the count over an hour still follows Chance. Without this the lift
    // saturated -- at any excitation worth having, every note struck, which is what Chance is for.
    double        strikeExcAvg_ = 0.0;
    // The same excitation as a smoothed signal (the modulation source `cascade`) and its slow
    // average: the Cosmos swells with it by default, and anything else can be routed from it.
    float         cascadeNow_ = 0.0f, cascadeAvg_ = 0.0f;
    float         cosmosSwell_ = 0.5f;
    PitchShifter  shimmerL_, shimmerR_;
    SpectralShifter shimmerSpec_;   // the shimmer's shifter in the spectrum (Shimmer Mode = Spectral)
    int           shimmerMode_ = 0;
    bool          shimmerWasOn_ = false;
    Drifter       shiftDrift_;
    Drifter       vecDriftX_, vecDriftY_;   // the Vector's point wandering on its own
    float         cosmosSend_ = 0.0f, cosmosReturn_ = 0.5f, cosmosToFar_ = 0.5f, cosmosNebula_ = 0.0f;
    float         cosmosShimmer_ = 0.0f, shimmerLpL_ = 0.0f, shimmerLpR_ = 0.0f, shimmerEnv_ = 0.0f;
    std::vector<float> cosL_, cosR_, nebL_, nebR_, shimL_, shimR_;
    std::vector<float> cloudL_, cloudR_;   // the cloud on its own, when it is shared between planes
    float         cosTap_[4096] = {};   // ring of the cosmos return for the display (torn reads cost a pixel)
    int           cosTapW_ = 0;
    // The same for the finished output, but four times as long. A spectrum of a drone is only
    // worth drawing if it can tell one partial from the next, and 4096 samples cannot: measured
    // on a 40 Hz tone with its octave, the trough between the two peaks is 14 dB down at 4096 and
    // 58 dB down at 16384 -- one hump against two lines. A low just fifth (60 and 90 Hz) behaves
    // the same way. The signal is stationary for seconds at a time, so the long window costs
    // nothing but 64 kB.
    float         outTap_[kOutTapLen] = {};
    int           outTapW_ = 0;
    // Feedback loop: the previous chunk's output mix, low-passed, saturated and throttled,
    // kept in a ring so any chunk length reads back exactly the samples just written.
    std::vector<float> fbRingL_, fbRingR_, fbInL_, fbInR_, fbMono_;
    int           fbW_ = 0, fbMask_ = 0;
    float         fbBus_ = 0.0f, fbFm_ = 0.0f, fbTone_ = 1500.0f, fbDrive_ = 0.5f;
    float         fbLpL_ = 0.0f, fbLpR_ = 0.0f, fbEnv_ = 0.0f, fbReg_ = 1.0f;
    float         fbHpXL_ = 0.0f, fbHpXR_ = 0.0f, fbHpYL_ = 0.0f, fbHpYR_ = 0.0f;   // loop DC blocker
    VoiceParams  vp_;
    BrainParams  bp_;
    Drifter      arc_;
    float        arcAmount_ = 0.0f, arcPeriodMin_ = 40.0f;
    float        arcHarmony_ = 0.0f, arcLean_ = 0.0f;    // how far the arc is leaning the harmony right now
    // The arc as the rest of the engine reads it: the drifter's value, or the clock's, and for
    // twenty seconds after a switch between them a glide from one to the other.
    bool         arcClock_ = false, arcClockWas_ = false;
    float        arcOut_ = 0.0f, arcGlide_ = 0.0f;
    double       clockHour_ = 12.0;
    double       clockHourOverride_ = -1.0;   // >= 0: use this hour, do not read the clock
    int          clockCheck_ = 0;
    float        guardFactor_ = 1.0f;                     // what the fluctuation guard last did to the drift
    // Foundation sub voice
    double       subPhaseL_ = 0.0, subPhaseR_ = 0.0, subFreqCur_ = 0.0;
    float        subLevel_ = 0.0f, subLevelCur_ = 0.0f, subGlide_ = 8.0f, subBinaural_ = 0.0f, subTone_ = 0.2f;
    // Pulse: the Foundation amplitude-modulated at the Binaural rate, a raised cosine. The
    // binaural offset alone makes a beat only inside the brainstem, where the two ears' phases
    // are compared; an amplitude modulation is a beat on the basilar membrane itself, and it
    // drives the auditory steady-state response several times harder (the hybrid "isochronic"
    // stimulation of the entrainment literature). Whether that entrains anything worth the name
    // is a separate and less settled question; what is built here is the modulation.
    float        subPulse_ = 0.0f, subPulseCur_ = 0.0f;
    double       subPulsePhase_ = 0.0;
    int          subOctave_ = 1;
    // Where the Foundation takes its pitch: 0 the conductor's root, 1 the ghost tone of the two
    // lowest voices, 2 the lowest voice that is actually sounding.
    int          subSource_ = 0;
    bool         hold_ = false;
    float        depth_ = 0.7f, keysDepth_ = 0.0f;
    float        delayMix_ = 0.25f, delayToFar_ = 0.4f, farLevel_ = 0.8f;

    FixedScale        scales_[kNumScaleChoices];
    FixedScale        userPending_;
    double            timbreScaleSig_ = 0.0;   // what the spectrum was when the scale was last built
    int               timbreScaleWait_ = 96;
    std::atomic<int>  userVersion_{ 0 };
    std::atomic<bool> userBusy_{ false };
    int               userSeen_ = 0;
    // User wavetable: pending copy + version (8 KB, copied by the audio thread).
    Wavetable         userTable_, userTablePending_;
    std::atomic<int>  tableVersion_{ 0 };
    std::atomic<bool> tableBusy_{ false };
    int               tableSeen_ = 0;
    std::atomic<int>  userTableFrames_{ 0 };
    // User cycles: two buffers, the audio thread reads the active one (see setUserCycles).
    CycleTable        userCycles_[2];
    std::atomic<int>  cyclesActive_{ -1 };
    // Texture: two buffers per slot, the audio thread reads the active one and publishes which.
    Texture           textures_[kSlots][2];
    std::atomic<int>  textureActive_[kSlots] = { -1, -1, -1, -1 };
    std::vector<Texture> nearPools_[2];         // the near source's own clips, the same way
    std::atomic<int>     nearPoolActive_{ -1 };
    int                  nearPick_ = 0;         // which of them the current event plays (audio thread)
    uint32_t             nearPickRng_ = 0x9E3779B9u;
    // Blocks begun and blocks finished. Two counters rather than one, because the question a
    // loader has to answer is not "how many have gone by" but "is anything still holding what it
    // picked up before I looked" -- and those differ exactly while a block is in flight. Reading
    // `started` and then waiting for `finished` to reach it answers it without guessing: when
    // nothing is rendering the two are already equal and there is no wait at all, and when a
    // block is running the wait is exactly as long as that block, however long that happens to be.
    std::atomic<unsigned long long> blocksBegun_ { 0 }, blocksDone_ { 0 };
    void waitForQuiet();   // message thread: until every block begun before now has ended
    const FixedScale* scale_ = nullptr;
    int               rootNote_ = 62;
    double            refPitch_ = 440.0;
    bool              snapKeys_ = true;
    float             inertiaCur_[kNumParams] = {};
    float             lastBlockSeconds_ = 0.005f;
    float             kuraPhase_[4] = { 0.0f, 1.3f, 2.9f, 4.4f };   // Kuramoto bank phases
    // The Lenia field. Thirty-two by thirty-two on a torus, a ring kernel of radius five, the
    // update spread over the blocks so that one full step costs a few rows each and never a
    // spike; stepped only while a route in the matrix reads one of its four sources, so a patch
    // that does not use it pays nothing for it. Reseeded with a few soft blobs when it dies out
    // or fills up, which on a grid this small it sometimes does.
    static constexpr int kLeniaRadius = 5;
    float             lenia_[kLeniaSize * kLeniaSize] = {}, leniaNext_[kLeniaSize * kLeniaSize] = {};
    float             leniaKernel_[(2 * kLeniaRadius + 1) * (2 * kLeniaRadius + 1)] = {};
    float             leniaTarget_[4] = {}, leniaOut_[4] = {};
    float             leniaRate_ = 4.0f, leniaMu_ = 0.15f, leniaRowAcc_ = 0.0f;
    int               leniaRow_ = 0, leniaSteps_ = 0, leniaReseeds_ = 0;
    bool              leniaInit_ = false, leniaUsed_ = false;
    Rng               leniaRng_;
    void stepLenia(float dt);
    void seedLenia();
    // The attractors' states in their own units, the six readings in -1..1, and the gate.
    double            lorenz_[3] = { 1.0, 1.0, 20.0 }, rossler_[3] = { 1.0, 1.0, 0.0 };
    float             chaosOut_[6] = {};
    float             chaosPeriod_ = 120.0f;
    int               chaosSteps_ = 0;
    bool              chaosUsed_ = false;
    void stepChaos(float dt);
    // The Beat modulation source: the instrument listening to its own tuning.
    float             beatPhase_ = 0.0f, beatHz_ = 0.0f;
    float             updateBeat(float dt);
    float             beatRateHz() const { return beatHz_; }        // for the card's readout
    float             farDiffuse_ = 0.0f;                           // how hard the air ahead of the far reverb saturates
    float             portamento_ = 0.0f, portaGravity_ = 0.5f;
    double            lastKeyHz_ = 0.0;   // frequency of the last key pressed, for portamento
    float             fbTape_ = 0.0f;
    // Bias: the feedback shaper's operating point follows the level of the bass going into it.
    // A static curve makes the same harmonics whatever came before; a transformer or a
    // capacitor-coupled tube stage does not -- low-frequency energy charges the coupling and
    // shifts where the curve is being used, so a bass swell changes how the highs distort (the
    // reactive nonlinearities of the wave-digital literature, Chowdhury among others). This is
    // the cheapest honest form of that: the loop's content below sixty hertz, rectified and
    // followed at five hertz, pushes the shaper's input off centre as it rises, in the curve's units after Drive, saturating at 1.2 once the bass is a hundredth of full scale.
    float             fbBias_ = 0.0f, fbBiasLpL_ = 0.0f, fbBiasLpR_ = 0.0f, fbBiasEnvL_ = 0.0f, fbBiasEnvR_ = 0.0f;
    Drifter           tapeWow_;
    double            tapeFlutterPhase_ = 0.0;
    double            purityCur_ = 1.0;   // Purity plus its drift, evaluated per block
    Drifter           purityDrift_;
    bool              retune_ = false;    // purity below 1 or drifting: sounding voices follow
    // Adaptive: a note that starts is tuned pure against the notes already sounding, not against
    // the root, and the offset it was given is kept for as long as it sounds. Those offsets
    // accumulate -- a progression through pure fifths and thirds walks the pitch away by a
    // syntonic comma (81/80, 21.5 cents) per cycle -- so a common offset shared by every voice
    // pays the drift back at three cents a minute, which is far below the ear's threshold for a
    // pitch change and leaves every interval exactly as pure as it was, because all the voices
    // move together.
    float             adaptAmt_ = 0.0f;
    float             adaptCents_[128] = {};   // the offset each note was given when it started
    double            commaCents_ = 0.0, commaTarget_ = 0.0;
    // Transpose: a pure interval on everything that sounds, in log2 units, glided at an octave
    // per two seconds so that a press is a slide and never a jump (SOMA Terra's interval keys).
    double            transposeTarget_ = 0.0, transposeCur_ = 0.0;
    // Sleep: after two seconds of silence (no voice, output below -90 dBFS) the effects sleep
    long long         silentSamples_ = 0;   // 64-bit: a long is 32 bits here and wrapped after twelve hours
    bool              asleep_ = false;
    // The map cursor the blend target was computed for, so a cursor that stands still is not
    // searched for and blended again on every block.
    float             blendX_ = 0.0f, blendY_ = 0.0f, blendR_ = 0.0f;
    bool              blendHave_ = false;
    // The matched partial ratios and what they were computed for (see readParams).
    float             matchRatio_[kMaxPartials] = {};
    float             matchLast_ = -1.0f, matchB_ = -1.0f;
    int               matchScale_ = -1, matchRoot_ = -1;
    bool              matchHave_ = false;

    std::vector<float> nearL_, nearR_, farL_, farR_, wetL_, wetR_;
    std::vector<float> dryL_, dryR_;   // the near layer's Dry share: past every reverb and delay, straight to the output
    // The foreground's two sends, gathered per event and added where the effect takes its input:
    // into the second delay's, and into the Cosmos'. Empty buffers cost nothing when both are 0.
    std::vector<float> foreD2L_, foreD2R_, foreCoL_, foreCoR_;
    float* const* stems_ = nullptr;   // eight pointers or null; valid for one process() call
    int    stemPos_ = 0;              // where in them this chunk starts
    double   sr_ = 48000.0;
    int      maxBlock_ = 512;
    uint64_t order_ = 0;
    Rng      rng_;
    int      seed_ = -1;
    int      lastRootPc_ = -1;
    bool     midiHeld_[128] = {};
    Smoother masterSmooth_;
    float    dcXL_ = 0.0f, dcXR_ = 0.0f, dcYL_ = 0.0f, dcYR_ = 0.0f;   // output DC blocker
    float    pdcXL_ = 0.0f, pdcXR_ = 0.0f, pdcYL_ = 0.0f, pdcYR_ = 0.0f;   // the same ahead of the Patina's clipper
    // Per-sample smoothing of the level-type parameters in the effect chain (20 ms), so
    // automation, gestures, morph and map blend never step a gain by a whole block.
    Smoother smDelayMix_, smDelayToFar_, smDelay2Mix_, smDelay2ToFar_, smCloudSend_, smCosmosSend_, smCosmosReturn_, smCosmosToFar_, smFarLevel_, smFarWidth_;

    std::atomic<uint64_t> mask_[2]{ 0, 0 };
    const Voice* loudestVoice() const;
    std::atomic<int> activeVoices_{ 0 };
    std::atomic<int> brainRoot_{ 50 };
    std::atomic<float> arcValue_{ 0.0f };
    std::atomic<float> noteDistance_[128];
    std::atomic<float> noteLevel_[128];
};

} // namespace ambient
