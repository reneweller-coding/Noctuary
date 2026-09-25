/**
 * @file Engine.h
 * @brief The engine: voices, cluster brain, spatial routing, effects, parameters.
 *
 * Framework-free. `process()` never allocates; parameters are plain atomics so the
 * host layer can write them from any thread.
 *
 * Signal flow:
 * @code{.unparsed}
 *   voices -> near bus (dry plane) -> ensemble -> stereo delay -> near reverb ----+
 *          -> far bus (reverb send) + delay "to far" -> far reverb (dark, wide) --+-> mid/side -> master
 * @endcode
 *
 * One Engine is one instrument: the plugin, the standalone, the Quest app and the render tool each
 * own one (a preset change in the plugin builds a second and crossfades, which is what the adopt
 * and hand-over calls below are for). The host calls prepare() once, then process() from its audio
 * thread; everything else is a setter or an observer. The implementation is split over three
 * files: Engine.cpp (construction, presets, morph, textures, notes and the display taps),
 * EngineControl.cpp (what is decided once per block: the modulators, the clock and readParams(),
 * which turns the parameter atomics into the structures the voices and the effects are given) and
 * EngineRender.cpp (process() and renderChunk(), the signal path).
 *
 * Threads, in one place: parameters and the observers are relaxed atomics or plain reads that a
 * torn value cannot hurt; the larger data -- the user scale, the wavetable, the cycles, the
 * textures, the route, the matrix and the envelope shapes -- travel from the message thread into
 * pending copies that the audio thread picks up at the top of a block, published by a version
 * counter, a double buffer or the modulation lock, as each section says.
 */
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

/**
 * @brief How much of the finished output the engine keeps for the displays to look at.
 *
 * A power of two:
 * the ring is indexed with a mask.
 */
constexpr int kOutTapLen = 16384;

/**
 * @brief The instrument: sixteen voices, two conductors and the near-event scheduler, the two
 *        planes with their effects, the parameter store, and every hand-over and observer a host
 *        needs around them.
 *
 * See the file comment for where the pieces live and how the threads meet. In short: prepare() on
 * the message thread, process() on the audio thread, parameters from anywhere, and the setters of
 * larger data on the message thread, where several of them wait (microseconds, or one block) for
 * the audio thread to let go of what they are about to overwrite.
 */
class Engine {
public:
    static constexpr int kMaxVoices = 16;   ///< voices the engine renders at once; allocate() steals the quietest releasing or the oldest past that

    /**
     * @brief The engine as built, before prepare(): every parameter at its default, both morph slots
     *        following the knobs, the modulation envelopes on their default shapes, the built-in
     *        scales made, the Scala and Timbre slots named, and the master smoother at -6 dB.
     */
    Engine();
    /**
     * @brief Allocates and prepares everything for a sample rate and a largest block (message thread,
     *        before the first process(); again at a rate change).
     *
     * Every buffer, delay ring and effect is sized here and never again; the random streams are
     * seeded from the Seed parameter (the Memory, the Body, the Patina and the near layer on their
     * own constants, so switching one on never moves another's dice); a generated hall goes into
     * the Room unless a user impulse is kept; the conductors are reset an octave below the key
     * root; and readParams() runs once so the first block has its structures. The preset map's
     * vectors are deliberately NOT built here (see Engine.cpp; PresetMap::warmupAsync does it).
     *
     * @param sampleRate    Hz
     * @param maxBlockSize  the largest n process() will be given; raised to kControlBlock at least,
     *                      and larger blocks are chunked anyway
     */
    void prepare(double sampleRate, int maxBlockSize);
    /**
     * @brief Starts the piece over: kills every voice, forgets the held keys, and reseeds the first
     *        conductor, the strike coin and the near scheduler from the seed, so the same notes come
     *        back; the effects' tails are left to ring out.
     */
    void reset();

    /**
     * @brief Writes a parameter (any thread, a relaxed atomic store).
     * @param id  the parameter
     * @param v   its value in the parameter's own units (Params.h); nothing is clamped here
     */
    void  setParam(ParamId id, float v) { params_[static_cast<int>(id)].store(v, std::memory_order_relaxed); }
    /**
     * @brief Reads a parameter as the host set it (any thread); effectiveParam() is what is actually playing.
     * @param id  the parameter
     * @return    its value in the parameter's own units
     */
    float getParam(ParamId id) const    { return params_[static_cast<int>(id)].load(std::memory_order_relaxed); }
    /**
     * @brief Loads a preset of the main bank.
     *
     * any thread; sets every parameter (full preset) -- and the modulation with it: the matrix rows
     * and the envelope shapes of its mod and envs fields (applyPresetModulation()).
     *
     * @param index  0 .. numPresets() - 1
     * @return       false for an index outside the bank, or when a field of the preset did not parse
     */
    bool  applyPreset(int index);
    /**
     * @brief The conductors alone -- no voices, no audio -- for `seconds` at `dt`, every event handed to
     *        `sink` with its time and which conductor made it (1 or 2), every root change to `rootSink`.
     *
     * The rule book's own check (its section 11): an hour of notes, measured, with the parameters
     * exactly as the render reads them and seeded as the render seeds them. What it leaves out is
     * the matrix -- a route onto a brain parameter is not run here -- and the adaptive tuning.
     * The near events are reported as a third conductor (3): their notes on and off, a glide as
     * the new note beginning, a move not at all. Runs readParams() first; offline only, never
     * while process() is being called.
     *
     * @param seconds   how long to run
     * @param dt        the step in seconds (the render's control block, typically)
     * @param sink      receives (time, conductor 1/2/3, event) for every note on and off
     * @param rootSink  receives (time, root note) at the start and at every root change; may be empty
     */
    void  auditConductor(double seconds, double dt,
                         const std::function<void(double, int, const BrainEvent&)>& sink,
                         const std::function<void(double, int)>& rootSink);
    /**
     * @brief Loads a preset of the main bank as a sound layer.
     *
     * sound layer only: everything except the Cosmos section -- and the modulation, which belongs to
     * the sound (its matrix and envelope shapes come along).
     *
     * @param index  0 .. numPresets() - 1
     * @return       false for an index outside the bank or a field that did not parse
     */
    bool  applySoundPreset(int index);
    /**
     * @brief Loads a preset of the Cosmos bank.
     *
     * Cosmos layer only, from the Cosmos preset bank
     *
     * @param index  0 .. numCosmosPresets() - 1
     * @return       false for an index outside the bank
     */
    bool  applyCosmosPreset(int index);
    /**
     * @brief Loads a preset of the Z-plane bank.
     *
     * Z-plane layer only: the filter and where its point sits
     *
     * @param index  0 .. numZPresets() - 1
     * @return       false for an index outside the bank
     */
    bool  applyZPreset(int index);
    /**
     * @brief Loads a preset of the Strike bank.
     *
     * Strike layer only: the Karplus-Strong pluck
     *
     * @param index  0 .. numStrikePresets() - 1
     * @return       false for an index outside the bank
     */
    bool  applyStrikePreset(int index);
    /**
     * @brief Loads a preset of the Near bank.
     *
     * the near layer only: the Near Source and the Near Events
     *
     * @param index  0 .. numNearPresets() - 1
     * @return       false for an index outside the bank
     */
    bool  applyNearPreset(int index);
    /**
     * @name The near events (Near.h)
     * The near events (Near.h): whether one is sounding, how far it has come, how many there
     * have been; and the whole scheduler, so an engine taking over at a preset change can carry
     * on the sequence that was running rather than start the foreground again.
     * @{ */
    /** @brief Whether a near event is sounding right now. @return true while one is */
    bool  nearActive() const { return near_.active(); }
    /** @brief How far the current event has come. @return 0..1 of its length */
    float nearProgress() const { return near_.progress(); }
    /** @brief How many events the scheduler has played since it was reset. @return the count */
    int   nearEventsPlayed() const { return near_.events(); }
    /** @brief How many times a sequence has mutated. @return the count */
    int   nearMutations() const { return near_.mutations(); }
    /** @brief The whole scheduler, to be copied into the engine that takes over. @return the scheduler as it stands */
    const NearEvents& nearState() const { return near_; }
    /**
     * @brief Takes another engine's scheduler over, sequence and all (message thread, before this
     *        engine is heard).
     * @param n  the state the leaving engine's nearState() gave
     */
    void  adoptNear(const NearEvents& n) { near_ = n; }
    /** @} */

    /**
     * @name MIDI, audio thread only.
     * @{ */
    /**
     * @brief A key goes down: a voice starts on the keys' plane (KeysDepth), with portamento from the
     *        previous key when set; under Hold a key that is already sounding is released instead.
     * @param note      0 .. 127
     * @param velocity  0 .. 1
     */
    void noteOn(int note, float velocity);
    /**
     * @brief A key comes up: its voice is released, unless Hold latches the keys.
     * @param note  0 .. 127
     */
    void noteOff(int note);
    /** @brief Releases every voice of every owner and forgets the held keys. */
    void allNotesOff();
    /** @} */
    /** @brief Autoplay: exchange a voice of the cluster now, whatever its timer says. Any thread. */
    void autoplayStep() { autoStepAsked_.store(true, std::memory_order_relaxed); }
    /**
     * @name Per-note expression (MPE, polyphonic aftertouch, CC 74).
     * Per-note expression (MPE, polyphonic aftertouch, CC 74). `note` < 0 addresses every
     * sounding voice, which is what a plain keyboard's channel pressure and wheel mean.
     * @{ */
    /**
     * @brief Pressure on a sounding note.
     * @param note  the note, or < 0 for every sounding voice
     * @param v     0 .. 1
     */
    void setPressure(int note, float v);
    /**
     * @brief Slide (CC 74, the MPE Y axis) on a sounding note.
     * @param note  the note, or < 0 for every sounding voice
     * @param v     0 .. 1
     */
    void setSlide(int note, float v);
    /**
     * @brief Pitch bend on a sounding note.
     * @param note        the note, or < 0 for every sounding voice
     * @param normalised  -1 .. 1, scaled by the Bend Range parameter
     */
    void setBend(int note, float normalised);
    /** @} */
    /**
     * @brief The mod wheel (CC 1), 0..1.
     *
     * It belongs to the instrument rather than to a note, and reaches
     * the sound only through the matrix -- there is no fixed route from it. The value is a target
     * the modulation follows over 30 ms (a 7-bit controller steps).
     *
     * @param v  0 .. 1, clamped
     */
    void setWheel(float v);
    /**
     * @brief The head's yaw in degrees (0 ahead, positive to the right), from a headset or an OSC head
     *        tracker.
     *
     * Only the Headphones binaural mode listens to it. Any thread.
     *
     * @param degrees  the yaw
     */
    void setHeadYaw(float degrees);

    /**
     * @brief Renders `n` stereo samples (replaces L/R).
     *
     * Any n; larger than maxBlockSize is chunked. Audio thread. Denormals are switched off for the
     * duration; the pending scale and wavetable are taken over; the arc, the drifters, the Kuramoto
     * bank and the morph glide advance; the map blend, the clock and the modulation step; readParams()
     * turns the parameters into this block's structures; then renderChunk() runs the signal path in
     * chunks of the prepared size. Afterwards the sleep check, and the note masks, the voice count
     * and the root are published for the observers.
     *
     * @param L  left output, n samples, overwritten
     * @param R  right output, n samples, overwritten
     * @param n  samples to render
     */
    void process(float* L, float* R, int n);

    /**
     * @name Stems.
     * Stems. Pass eight pointers -- near L/R, far L/R, cosmos L/R, room L/R -- each with room for
     * the n of the next process() call, and they are filled alongside the mix; pass nullptr to
     * stop. What is in them is what each plane contributes to the output at the point it joins
     * it, so the four sum to the mix before the master stage (the Cosmos path's own send into
     * the far plane is part of the far stem, where it is heard).
     * @{ */
    static constexpr int kNumStems = 4;   ///< stereo stems: near, far, cosmos (the Memory counted in), room
    /**
     * @brief The name of a stem, for the render tool's file names.
     * @param i  0 .. kNumStems - 1
     * @return   "near", "far", "cosmos" or "room"; "" outside the range
     */
    static const char* stemName(int i);
    /**
     * @brief Where the next process() call writes the stems.
     * @param eightPointers  eight buffers in the order above, or null to stop writing stems
     */
    void setStemBuffers(float* const* eightPointers) { stems_ = eightPointers; }
    /** @} */

    /**
     * @brief Sets the Scala slot's scale.
     *
     * Any thread. Applied at the start of the next block. Waits (microseconds) while the audio
     * thread is copying the previous one, then writes the pending copy and bumps its version.
     *
     * @param s  the scale for the User (Scala) slot
     */
    void setUserScale(const FixedScale& s);
    /**
     * @brief User wavetable (Table = User in a source slot) and texture (Type = Texture), message
     *        thread.
     *
     * The texture is double-buffered: the call waits (at most one block) until the
     * audio thread has left the buffer it is about to overwrite, then swaps. The wavetable itself
     * travels as a pending copy with a version (8 KB the audio thread copies at the top of a
     * block); this call waits only while that copy is in progress.
     *
     * @param t  the analysed table (Wavetable::analyse)
     */
    void setUserWavetable(const Wavetable& t);
    /**
     * @brief The same file as single cycles, for the Wavetable type.
     *
     * Double-buffered like a texture rather
     * than copied on the audio thread: 256 frames at ten resolutions are megabytes. Message
     * thread; waits for every block begun before the call to end (waitForQuiet()).
     *
     * @param t  the built cycle table (CycleTable::build)
     */
    void setUserCycles(const CycleTable& t);
    /**
     * @brief Reads the frames both ways -- spectra for the Harmonic type, cycles for the Wavetable type --
     *        and sets both.
     *
     * `frameLen` is one cycle's length; readWavetableFile finds it for a file. Message thread.
     *
     * @param mono      the file's samples, frames end to end
     * @param n         how many samples
     * @param frameLen  samples per frame (one cycle)
     * @return          false when the analysis or the cycle build fails (silence, or nothing either type could play)
     */
    bool loadUserWavetable(const float* mono, int n, int frameLen = 2048);
    /**
     * @brief One clip per source slot, so four slots typed Texture (or Stretch) can play four different
     *        recordings -- which is what makes the Vector's four corners four landscapes.
     *
     * The slotless
     * form loads the same clip into every slot: what the instrument always did, and what a preset
     * that names one file still means. Message thread: the mono clip is copied into the slot's
     * buffer that is not playing -- after waitForQuiet(), see Engine.cpp for why the flag that used
     * to stand there asked the wrong question -- its offset is removed, it is measured, and the
     * slot's active index flips to it.
     *
     * @param slot        0 .. kSlots - 1; anything else is ignored
     * @param mono        the samples
     * @param n           how many
     * @param sampleRate  the recording's rate in Hz (not positive reads as 48 kHz)
     * @param baseHz      the pitch the clip is heard as unshifted, Hz (not positive reads as middle C)
     * @param seamless    whether the clip loops without a seam
     */
    void setTexture(int slot, const float* mono, int n, double sampleRate, double baseHz = 261.6256, bool seamless = false);
    /**
     * @brief The same mono clip into every slot (see the slot form).
     * @param mono        the samples
     * @param n           how many
     * @param sampleRate  the recording's rate in Hz
     * @param baseHz      the pitch the clip is heard as unshifted, Hz
     * @param seamless    whether the clip loops without a seam
     */
    void setTexture(const float* mono, int n, double sampleRate, double baseHz = 261.6256, bool seamless = false)
    { for (int k = 0; k < kSlots; ++k) setTexture(k, mono, n, sampleRate, baseHz, seamless); }
    /**
     * @brief The same clip with both its channels.
     *
     * `R` may be null, which is the mono form above; when it
     * is not, the granular source reads the pair and keeps the recording's own image instead of
     * playing its mono sum. The mono sum is still made here, because the band model, the Paulstretch
     * and the Spectral resynthesis are all built on it.
     *
     * @param slot        0 .. kSlots - 1; anything else is ignored
     * @param L           left channel
     * @param R           right channel, or null for the mono form
     * @param n           samples per channel
     * @param sampleRate  the recording's rate in Hz
     * @param baseHz      the pitch the clip is heard as unshifted, Hz
     * @param seamless    whether the clip loops without a seam
     */
    void setTexture(int slot, const float* L, const float* R, int n, double sampleRate, double baseHz = 261.6256, bool seamless = false);
    /**
     * @brief The same stereo clip into every slot (see the slot form).
     * @param L           left channel
     * @param R           right channel, or null
     * @param n           samples per channel
     * @param sampleRate  the recording's rate in Hz
     * @param baseHz      the pitch the clip is heard as unshifted, Hz
     * @param seamless    whether the clip loops without a seam
     */
    void setTexture(const float* L, const float* R, int n, double sampleRate, double baseHz = 261.6256, bool seamless = false)
    { for (int k = 0; k < kSlots; ++k) setTexture(k, L, R, n, sampleRate, baseHz, seamless); }
    /**
     * @brief A clip that has already been read and measured, handed from one engine to another (a preset
     *        change builds a new engine and the player's own sample has to survive it).
     *
     * Copies the band
     * model with it rather than measuring the same clip a second time. Message thread, after
     * waitForQuiet(); an empty clip is ignored.
     *
     * @param slot  0 .. kSlots - 1
     * @param src   the other engine's displayTexture(slot)
     */
    void setTexture(int slot, const Texture& src);
    /**
     * @brief Empties a slot: its active index goes to -1 and the source falls back to silence (the buffers stay for the next clip).
     * @param slot  0 .. kSlots - 1; anything else is ignored
     */
    void clearTexture(int slot);
    /**
     * @name The near source's own clips (13.09.2026)
     * The near source's own clips (13.09.2026): what a near event plays when its type reads a
     * recording -- a voice off a radio loop, a launch, the wind on Mars. A pool rather than one
     * clip, because a preset may name a folder of phrases and every event then takes one of
     * them, at random and never the one just played; a single clip is a pool of one. Its own
     * buffers, so the near layer's presets carry their clips and the sound preset's four slots
     * keep theirs; a near source with no clip of its own reads Source 4's. Double-buffered like
     * the slots' clips: the message thread fills the pool that is not active and flips the index.
     * @{ */
    /**
     * @brief Builds a Texture from a recording without touching the engine: the mono sum (and the pair,
     *        when R is given), cut to at most `maxSeconds`, its offset removed, measured.
     * @param L           left channel
     * @param R           right channel, or null for mono
     * @param n           samples per channel
     * @param sampleRate  the recording's rate in Hz (not positive reads as 48 kHz)
     * @param baseHz      the pitch the clip is heard as unshifted, Hz (not positive reads as middle C)
     * @param seamless    whether the clip loops without a seam
     * @param maxSeconds  the most that is kept, from the start (at least one second)
     * @return            the clip; empty for a null pointer or n <= 0
     */
    static Texture makeTexture(const float* L, const float* R, int n, double sampleRate, double baseHz = 261.6256,
                               bool seamless = false, double maxSeconds = 120.0);
    /**
     * @brief One recording as the whole pool: at most two minutes of it (a rover driving for sixteen
     *        minutes is a bed, not an event).
     * @param L           left channel
     * @param R           right channel, or null
     * @param n           samples per channel
     * @param sampleRate  the recording's rate in Hz
     * @param baseHz      the pitch the clip is heard as unshifted, Hz
     * @param seamless    whether the clip loops without a seam
     */
    void setNearTexture(const float* L, const float* R, int n, double sampleRate, double baseHz = 261.6256, bool seamless = false);
    /**
     * @brief One already-measured clip as the whole pool (a hand-over from another engine).
     * @param src  the clip; an empty one is ignored
     */
    void setNearTexture(const Texture& src);
    /**
     * @brief The whole pool at once, moved in.
     *
     * Empty clips are dropped; an empty pool clears. The pool that is not active is written
     * after waitForQuiet() and published; the pick is left to the audio thread, which clamps it.
     *
     * @param pool  the clips, moved
     */
    void setNearTextures(std::vector<Texture> pool);
    /** @brief No pool: the near source reads Source 4's clip again. */
    void clearNearTexture() { nearPoolActive_.store(-1, std::memory_order_release); }
    /** @brief Whether a pool with at least one clip is active. @return true when there is one */
    bool hasNearTexture() const { return displayNearPool() != nullptr; }
    /** @brief How many clips the active pool holds. @return the count, 0 without a pool */
    int  nearTextureCount() const { const std::vector<Texture>* p = displayNearPool(); return p != nullptr ? static_cast<int>(p->size()) : 0; }
    /**
     * @brief The active pool, for the displays (message thread, no synchronisation).
     * @return the pool, or null when none is active or it is empty
     */
    const std::vector<Texture>* displayNearPool() const
    {
        const int a = nearPoolActive_.load(std::memory_order_relaxed);
        return a >= 0 && !nearPools_[a].empty() ? &nearPools_[a] : nullptr;
    }
    /**
     * @brief The clip the current (or next) event plays.
     * @return the picked clip of the active pool, or null without a pool
     */
    const Texture* displayNearTexture() const
    {
        const std::vector<Texture>* p = displayNearPool();
        if (p == nullptr) return nullptr;
        return &(*p)[std::min(static_cast<size_t>(std::max(0, nearPick_)), p->size() - 1)];
    }
    /** @} */
    /**
     * @brief Whether a slot holds a clip.
     * @param slot  0 .. kSlots - 1, clamped
     * @return      true when the slot's active index is set
     */
    bool hasTexture(int slot = 0) const
    { return textureActive_[slot < 0 ? 0 : (slot >= kSlots ? kSlots - 1 : slot)].load(std::memory_order_relaxed) >= 0; }
    /**
     * @brief Convolution room: a stereo (R may be null) impulse response, message thread.
     * @param L           left channel of the impulse
     * @param R           right channel, or null
     * @param n           samples per channel
     * @param sampleRate  the impulse's rate in Hz
     */
    void  setImpulse(const float* L, const float* R, int n, double sampleRate) { room_.setImpulse(L, R, n, sampleRate); userImpulse_ = true; }
    /**
     * @brief The room's second impulse: Room Morph blends it into the first inside the one convolution,
     *        so a morph costs what one room costs.
     * @param L           left channel of the impulse
     * @param R           right channel, or null
     * @param n           samples per channel
     * @param sampleRate  the impulse's rate in Hz
     */
    void  setImpulseB(const float* L, const float* R, int n, double sampleRate) { room_.setImpulseB(L, R, n, sampleRate); hasImpulseB_ = true; }
    /** @brief Whether a second impulse is loaded. @return true when Room Morph has something to go to */
    bool  hasImpulseB() const { return hasImpulseB_; }
    /** @brief The second impulse gone: Room Morph has nothing to go to, and the room is A alone. */
    void  clearImpulseB() { hasImpulseB_ = false; room_.clearImpulseB(); }
    /** @brief The length of the room's impulse. @return seconds */
    float impulseSeconds() const { return room_.impulseSeconds(); }
    /** @brief Whether the room plays a loaded impulse rather than the generated hall. @return true after setImpulse() */
    bool  hasUserImpulse() const { return userImpulse_; }
    /**
     * @brief Longest impulse the Room keeps (memory grows with it: about 46 MB of delay line for a
     *        minute); call before prepare().
     *
     * A minute on the desktop; the Quest app sets 4 s, measured
     * there with the convolver before this one (a third of one of its cores).
     *
     * @param s  seconds, clamped to 0.5 .. 60
     */
    void  setRoomMaxSeconds(float s) { roomMaxSeconds_ = clampv(s, 0.5f, 60.0f); }
    /** @brief The room's longest impulse. @return seconds, as setRoomMaxSeconds() left it */
    float roomMaxSeconds() const { return roomMaxSeconds_; }
    /** @brief How many frames the user wavetable has. @return the count, 0 when none is loaded */
    int  userWavetableFrames() const { return userTableFrames_.load(std::memory_order_relaxed); }
    /**
     * @name For the displays
     * For the displays (message thread, no synchronisation -- a torn read costs a pixel).
     * @{ */
    /** @brief The user wavetable the audio thread reads. @return the table, or null when none is loaded */
    const Wavetable* userWavetable() const { return userTable_.frames > 0 ? &userTable_ : nullptr; }
    /** @brief The user cycles the audio thread reads. @return the active buffer, or null when none is loaded */
    const CycleTable* userCycles() const
    {
        const int a = cyclesActive_.load(std::memory_order_relaxed);
        return (a >= 0 && !userCycles_[a].empty()) ? &userCycles_[a] : nullptr;
    }
    /**
     * @brief The clip a slot plays.
     * @param slot  0 .. kSlots - 1, clamped
     * @return      the slot's active clip, or null when it holds none
     */
    const Texture*   displayTexture(int slot = 0) const
    {
        const int k = slot < 0 ? 0 : (slot >= kSlots ? kSlots - 1 : slot);
        const int a = textureActive_[k].load(std::memory_order_relaxed);
        return a >= 0 ? &textures_[k][a] : nullptr;
    }
    /** @} */

    /**
     * @name Morph
     * Morph: two full parameter snapshots (A = 0, B = 1). While MorphActive is on the
     * engine plays lerp(A, B, position); the position glides toward MorphPos at
     * 1/MorphGlide per second. Meant for a hand in VR: one continuous gesture moves
     * the whole instrument from one world to another.
     * @{ */
    /**
     * @brief Fills a slot from a parameter vector and marks it chosen.
     *
     * kNumParams values, any thread. The values are stored first and the flag after, so the audio
     * thread never sees "chosen" over a half-written snapshot.
     *
     * @param slot    0 for A, anything else for B
     * @param values  kNumParams values in the parameters' own units
     */
    void  setMorphSlot(int slot, const float* values);
    /**
     * @brief Chooses a slot from what sounds now: copy the live parameters into a slot (any thread).
     * @param slot  0 for A, anything else for B
     */
    void  captureMorphSlot(int slot);
    /**
     * @brief Reads a slot out.
     * @param slot  0 for A, anything else for B
     * @param out   receives kNumParams values
     */
    void  morphSlot(int slot, float* out) const;
    /**
     * @brief Forgets a slot: it follows the knobs again.
     *
     * A slot nobody has chosen is not a snapshot of anything, and plays as the knobs stand.
     * Until 13.09.2026 both slots began at the defaults, so switching Morph on before choosing
     * A or B held the whole instrument at Init: every knob dead, the near layer silent, a texture
     * preset without its texture. setMorphSlot and captureMorphSlot choose a slot; this forgets it.
     *
     * @param slot  0 for A, anything else for B
     */
    void  clearMorphSlot(int slot);
    /** @brief Whether a slot has been chosen. @param slot 0 for A, 1 for B @return true when it holds a snapshot */
    bool  morphSlotSet(int slot) const { return slotSet_[slot & 1].load(std::memory_order_relaxed); }
    /** @brief Where the gliding morph position stands. @return 0 (A) .. 1 (B) */
    float morphPosition() const { return morphCur_.load(std::memory_order_relaxed); }
    /**
     * @brief Put the gliding position somewhere without waiting for the glide: a preset change
     *        that travels has to start at A even if the morph was standing somewhere else.
     * @param p  0 .. 1
     */
    void  resetMorphPosition(float p) { morphCur_.store(p, std::memory_order_relaxed); }
    /**
     * @brief What is actually playing.
     *
     * The map blend while the map is active (performance parameters excepted), else the morph's
     * interpolation between the chosen slots -- in the skewed domain for floats, rounded for ints,
     * flipped halfway for choices, and from the live value where a slot is not chosen -- else the
     * live parameter. Any thread; atomics throughout.
     *
     * @param id  the parameter
     * @return    its effective value in the parameter's own units
     */
    float effectiveParam(ParamId id) const;
    /** @} */
    /**
     * @name Preset map (MapActive / MapX / MapY / MapRadius)
     * Preset map (MapActive / MapX / MapY / MapRadius): the engine blends the presets around
     * the cursor at control rate and glides every parameter toward the blend with the Morph
     * Glide time constant. While active it overrides the live parameters and the morph.
     * blendValue() is the gliding value the host should copy back into its parameters when
     * the map is switched off, so the sound stays where the map left it.
     * @{ */
    /**
     * @brief The gliding blend value of a parameter (any thread).
     * @param id  the parameter
     * @return    where the glide stands, in the parameter's own units
     */
    float blendValue(ParamId id) const { return blendCur_[static_cast<int>(id)].load(std::memory_order_relaxed); }
    /** @brief Whether the map blend is overriding the parameters. @return true while it is */
    bool  mapActive() const { return blendActive_.load(std::memory_order_relaxed); }
    /** @} */
    /**
     * @name Route over the map
     * Route over the map. The host calls routeStep() once per block (dt in seconds, before its
     * own speed scaling): while RouteActive the route moves the cursor and the engine plays the
     * map blend; the new cursor is returned so the host can mirror it into its parameters.
     * The route text is performance state (plugin state / OSC / Quest config), not a preset.
     * The audio thread walks route_; every edit goes into routePending_ and is published with a
     * version, the way the user scale and the texture already are. The lock that used to sit in
     * the plugin protected nothing: routeStep never took it.
     * @{ */
    /** @brief The route being played. @return what is playing (message thread may read) */
    const Route& route() const { return route_; }
    /** @brief The route as edited. @return what was last edited */
    const Route& routeEdit() const { return routePending_; }
    /**
     * @brief Replaces the pending route from its text form and publishes it (message thread).
     * @param text  the route (Route.h)
     * @return      false when the text does not parse; nothing is published then
     */
    bool setRouteText(const char* text)
    { if (!routePending_.parse(text)) return false; routeVersion_.fetch_add(1, std::memory_order_release); return true; }
    /**
     * @brief Appends a waypoint to the pending route and publishes it (message thread).
     * @param w  the waypoint
     * @return   false when the route is full
     */
    bool addRoutePoint(const Waypoint& w)
    { if (!routePending_.add(w)) return false; routeVersion_.fetch_add(1, std::memory_order_release); return true; }
    /** @brief Empties the pending route and publishes it (message thread). */
    void clearRoute() { routePending_.clear(); routeVersion_.fetch_add(1, std::memory_order_release); }
    /**
     * @brief The pending route as text.
     * @param buf  the buffer
     * @param cap  its size in bytes
     * @return     what Route::write returns: characters written
     */
    int  writeRoute(char* buf, size_t cap) const { return routePending_.write(buf, cap); }
    /**
     * @brief One block of the route (audio thread, or the host's timer, once per block).
     *
     * Picks up a published edit (and restarts from the cursor, so a route changed while it plays
     * does not jump), starts the route from the current cursor when RouteActive comes on, stops
     * it when it goes off, advances it by dt times RouteSpeed, writes the cursor into MapX, MapY
     * and MapRadius, forces MapActive on (a route always plays through the map), and clears
     * RouteActive when a non-looping route ends.
     *
     * @param dt      seconds since the previous call, before the speed scaling
     * @param x       receives the cursor's x (the parameter's value when the route is not active)
     * @param y       receives the cursor's y
     * @param radius  receives the cursor's radius
     * @return        true while the route is running
     */
    bool routeStep(double dt, float& x, float& y, float& radius);
    /** @brief Whether the playing route is under way. @return true while it moves the cursor */
    bool routeRunning() const { return route_.running(); }
    /** @} */
    /** @brief Whether the effects are asleep. @return no voice and no tail for two seconds: effects skipped */
    bool asleep() const { return asleep_; }
    /**
     * @brief One of the four Kuramoto oscillators' phases, for pictures.
     * @param i  0 .. 3 (masked)
     * @return   Kuramoto oscillator phases, for pictures -- radians, 0 .. 2 pi
     */
    float coherencePhase(int i) const { return kuraPhase_[i & 3]; }
    /**
     * @name The Lenia field
     * The Lenia field, for pictures and tests: a cell (0..1), how many full steps it has taken,
     * how often it had to be reseeded (dead or saturated), and the four readings the matrix sees.
     * @{ */
    static constexpr int kLeniaSize = 32;   ///< the torus is this many cells on a side
    /**
     * @brief One cell of the field; the coordinates wrap on the torus.
     * @param x  column, any integer
     * @param y  row, any integer
     * @return   the cell, 0..1
     */
    float leniaCell(int x, int y) const { return lenia_[((y % kLeniaSize + kLeniaSize) % kLeniaSize) * kLeniaSize + ((x % kLeniaSize + kLeniaSize) % kLeniaSize)]; }
    /** @brief Full field updates so far. @return the count */
    int   leniaSteps() const { return leniaSteps_; }
    /** @brief How often the field died out or filled up and was reseeded. @return the count */
    int   leniaReseeds() const { return leniaReseeds_; }
    /**
     * @brief One of the four readings the matrix sees, glided.
     * @param k  0 .. 3 (masked)
     * @return   0..1, the mean of a three-by-three patch near a corner
     */
    float leniaOut(int k) const { return leniaOut_[k & 3]; }
    /** @} */
    /**
     * @name The attractors
     * The attractors, for the tests: how many blocks they have been integrated, and a reading.
     * @{ */
    /** @brief Blocks the Lorenz and Roessler systems have been stepped. @return the count */
    int   chaosSteps() const { return chaosSteps_; }
    /**
     * @brief One of the six readings: Lorenz x, y, z, then Roessler x, y, z.
     * @param k  0 .. 5
     * @return   -1..1, the coordinate scaled by the attractor's extent and clamped
     */
    float chaosOut(int k) const { return chaosOut_[k % 6]; }
    /** @} */

    /**
     * @name Clock (Clock.h)
     * ---- clock (Clock.h). A host with a play head calls setHostClock() once per block, before
     * process(); MIDI clock messages arrive through midiClock*() (audio thread). Which of them
     * the engine follows is the ClockSource parameter; without either it runs its own tempo.
     * @{ */
    /**
     * @brief The host's transport for this block.
     * @param bpm           its tempo
     * @param beatPosition  its position in quarter notes
     * @param playing       whether it is running
     */
    void setHostClock(double bpm, double beatPosition, bool playing)
    { hostBpm_ = bpm; hostBeat_ = beatPosition; hostPlaying_ = playing; hostSeen_ = true; }
    /**
     * @brief A MIDI clock tick.
     *
     * one 0xF8; the interval may be 0 when unknown. Twenty-four to a quarter; the tempo settles
     * over a beat's worth of ticks so jitter does not wobble every synced LFO, the beat advances
     * by a twenty-fourth while running, and the silence timer restarts.
     *
     * @param secondsSinceLastTick  the interval, or 0 when it is not known (the tempo is then left)
     */
    void midiClockTick(double secondsSinceLastTick);
    /** @brief MIDI Start (0xFA): the beat goes back to zero and the clock runs. */
    void midiClockStart();
    /** @brief MIDI Continue (0xFB): the clock runs from where it stood. */
    void midiClockContinue();
    /** @brief MIDI Stop (0xFC): the clock stops; the beat keeps its position. */
    void midiClockStop();
    /** @} */
    /**
     * @name What the engine is following right now, for displays.
     * @{ */
    /** @brief The tempo in force. @return BPM */
    double tempo() const        { return tempoOut_.load(std::memory_order_relaxed); }
    /** @brief The beat position in force. @return quarter notes */
    double beatPosition() const { return beatOut_.load(std::memory_order_relaxed); }
    /** @brief Whether the clock in force is running. @return true while it counts */
    bool   clockRunning() const { return runningOut_.load(std::memory_order_relaxed); }
    /** @} */

    /**
     * @name Modulation
     * ---- modulation (message thread for the setters; see Modulation.h for the text forms)
     * The matrix rows and the envelope shapes are data, published the way the route and the user
     * scale are: written into a pending copy and picked up at the next block.
     * @{ */
    /** @brief Back to nothing: the matrix cleared, the six envelopes back to default */
    void resetModulation();
    /**
     * @brief Loads a preset's modulation: the mod and envs fields of a preset
     *
     * Cleared, filled and announced once at the end, under the lock. The envs field holds the six
     * modulation envelopes and then the four sources' own, separated by '~'.
     *
     * @param p  the preset
     * @return   false when a field did not parse (what parsed is kept)
     */
    bool applyPresetModulation(const Preset& p);
    /** @} */
private:
    /**
     * @brief take the pending matrix and shapes (message thread waits)
     *
     * A spin on modLock_. Everything that writes into the pending copies has to hold it, because
     * the audio thread copies the very bytes the parser writes (EngineControl.cpp).
     */
    void lockModulation();
    /** @brief Releases the lock lockModulation() took. */
    void unlockModulation();
    /** @brief clear without announcing; call with the lock held -- the matrix emptied, the six envelopes and the four source envelopes back to their default shapes */
    void clearPendingModulation();
    /** @brief announce, once everything is written; with the lock held -- bumps modVersion_, and the audio thread copies at the top of its next block */
    void publishModulation();
public:
    /**
     * @brief Replaces the matrix from its text form (message thread, under the lock) and publishes it.
     * @param text  the rows (ModMatrix::parse)
     * @return      false when the text does not parse; nothing is published then
     */
    bool setModMatrixText(const char* text);
    /**
     * @brief The edited matrix as text.
     * @param buf  the buffer
     * @param cap  its size in bytes
     * @return     characters written, 0 when it did not fit
     */
    int  writeModMatrix(char* buf, size_t cap) const { return matrixPending_.write(buf, cap); }
    /** @brief The matrix as edited (the pending copy, what the editor shows). @return the rows */
    const ModMatrix& modMatrix() const { return matrixPending_; }
    /**
     * @brief Replaces one modulation envelope's shape from its text form and publishes it (message thread).
     * @param index  0 .. kNumModEnvs - 1
     * @param text   the shape (ModEnv::parse)
     * @return       false for an index outside the six or a text that does not parse
     */
    bool setEnvShape(int index, const char* text);
    /**
     * @brief One modulation envelope's edited shape as text.
     * @param index  0 .. kNumModEnvs - 1
     * @param buf    the buffer
     * @param cap    its size in bytes
     * @return       characters written; 0 for an index outside the six or a text that did not fit
     */
    int  writeEnvShape(int index, char* buf, size_t cap) const;
    /**
     * @brief One modulation envelope's shape as edited.
     * @param index  0 .. kNumModEnvs - 1, clamped
     * @return       the pending shape
     */
    const ModEnv& envShape(int index) const { return envPending_[index < 0 ? 0 : (index >= kNumModEnvs ? kNumModEnvs - 1 : index)]; }
    /**
     * @brief Each source's own envelope shape, for a slot whose Env is Own: the same text form, read as a
     *        level from 0 (silent) to 1.
     *
     * Travels after the six in a preset's envelope field (Presets.h).
     *
     * @param slot  0 .. kSlots - 1
     * @param text  the shape (ModEnv::parse)
     * @return      false for a slot outside the four or a text that does not parse
     */
    bool setSrcEnvShape(int slot, const char* text);
    /**
     * @brief One source's edited shape as text.
     * @param slot  0 .. kSlots - 1
     * @param buf   the buffer
     * @param cap   its size in bytes
     * @return      characters written; 0 for a slot outside the four or a text that did not fit
     */
    int  writeSrcEnvShape(int slot, char* buf, size_t cap) const;
    /**
     * @brief One source's shape as edited.
     * @param slot  0 .. kSlots - 1, clamped
     * @return      the pending shape
     */
    const ModEnv& srcEnvShape(int slot) const { return srcEnvPending_[slot < 0 ? 0 : (slot >= kSlots ? kSlots - 1 : slot)]; }
    /**
     * @brief For the editor's playhead: where the loudest voice is reading a slot's entrance shape
     *        (seconds of the shape) and the gain the slot has from its entrance.
     *
     * The gain is written
     * whenever a voice sounds; the result says whether the slot is following a shape at all.
     *
     * @param slot          0 .. kSlots - 1
     * @param shapeSeconds  receives the time in the shape, in the shape's own seconds (negative when no shape is followed)
     * @param gain          receives the slot's entrance gain, 0 .. 1
     * @return              true when a voice sounds and the slot follows a shape
     */
    bool displaySlotEnv(int slot, float& shapeSeconds, float& gain) const;
    /**
     * @brief For the displays: the current value of every source, and each LFO's phase.
     * @param source  a ModSource as an int; anything out of range reads as 0
     * @return        the source's value this block, -1..1
     */
    float modSource(int source) const { return (source >= 0 && source < kNumModSources) ? modSrc_[source] : 0.0f; }
    /**
     * @brief How fast the Beat source is turning, in hertz: zero when the chord is in tune.
     * @return the followed beat rate, Hz
     */
    float beatRate() const { return beatHz_; }
    /**
     * @brief One LFO's phase.
     * @param i  0 .. kNumLfos - 1, clamped
     * @return   0..1
     */
    float lfoPhase(int i) const { return lfo_[i < 0 ? 0 : (i >= kNumLfos ? kNumLfos - 1 : i)].phase(); }
    /**
     * @brief One running LFO, for shapeAt() with its live random material.
     * @param i  0 .. kNumLfos - 1, clamped
     * @return   the LFO
     */
    const Lfo& lfo(int i) const { return lfo_[i < 0 ? 0 : (i >= kNumLfos ? kNumLfos - 1 : i)]; }
    /**
     * @brief Loudness of what is leaving the instrument, to BS.1770.
     *
     * Any thread; the numbers are
     * written once a block and read as a snapshot.
     *
     * @return the meter's reading (LoudnessMeter::read, which computes the gated figures on the spot)
     */
    LoudnessReading loudness() const { return loudness_.read(); }
    /** @brief Starts the loudness measurement over. */
    void resetLoudness() { loudness_.reset(); }
    /**
     * @brief One modulation envelope's clock, for the editor's playhead.
     *
     * Each modulation envelope has its own clock, because Sustain Loop stops one of them where
     * the others keep running. Index-safe like the rest of these display accessors.
     *
     * @param i  0 .. kNumModEnvs - 1, clamped
     * @return   seconds since the phrase began, before the envelope's Time scaling
     */
    float envTime(int i = 0) const { return static_cast<float>(envTime_[i < 0 ? 0 : (i >= kNumModEnvs ? kNumModEnvs - 1 : i)]); }
    /**
     * @brief How much the matrix is currently adding to a parameter, in that parameter's own units.
     * @param id  the parameter
     * @return    the offset this block, 0 when nothing is routed at it
     */
    float modAmount(ParamId id) const { return modOut_[static_cast<int>(id)]; }
    /**
     * @brief Partial amplitudes of the loudest sounding voice, for the oscillator display.
     *
     * Returns how
     * many were written, 0 when nothing sounds. Message thread, no synchronisation (see Voice.h).
     *
     * @param out       receives the amplitudes
     * @param maxCount  room in out
     * @return          how many were written, 0 when nothing sounds
     */
    int  displayPartials(float* out, int maxCount) const;
    /** @brief The loudest voice's pitch. @return that voice's frequency in Hz, 0 when nothing sounds */
    float displayFrequency() const;
    /**
     * @brief The same voice's slot bank (Additive / Wavetable in a slot) and its grains (Texture).
     * @param slot      0 .. kSlots - 1
     * @param out       receives the slot's partial amplitudes
     * @param maxCount  room in out
     * @return          how many were written, 0 when nothing sounds
     */
    int  displaySlotPartials(int slot, float* out, int maxCount) const;
    /**
     * @brief Where that slot is reading its table right now -- the knob, the matrix and the slot's own
     *        Pos Drift together.
     *
     * -1 when nothing sounds or the slot plays no table: the display then
     * falls back to the knob, which is all it ever knew before.
     *
     * @param slot  0 .. kSlots - 1
     * @return      the table position 0..1, or -1
     */
    float displaySlotPosition(int slot) const;
    /**
     * @brief The loudest voice's grains in a Texture slot, for the display.
     * @param slot      0 .. kSlots - 1, clamped
     * @param out       receives one entry per live grain
     * @param maxCount  room in out
     * @return          how many were written; 0 when nothing sounds or the slot has no clip
     */
    int  displayGrains(int slot, SourceSlot::GrainInfo* out, int maxCount) const;
    /** @var float VoiceStage::pan
     *  @brief pan -1..1 */
    /** @var float VoiceStage::distance
     *  @brief distance 0 near .. 1 far */
    /** @var float VoiceStage::level
     *  @brief envelope level */
    /** @var int VoiceStage::note
     *  @brief the MIDI note */
    /** @var int VoiceStage::owner
     *  @brief owner (0 keys, 1 brain; 2 the second conductor, 3 a near event) */
    /** @brief One voice's place for the stage picture, as voiceStage() fills it. */
    struct VoiceStage { float pan, distance, level; int note, owner; };
    /**
     * @brief Every sounding voice's place, for the stage picture: pan -1..1, distance 0 near .. 1 far,
     *        envelope level, note, owner (0 keys, 1 brain).
     *
     * Returns how many were written.
     *
     * @param out       receives the places
     * @param maxCount  room in out
     * @return          how many were written
     */
    int  voiceStage(VoiceStage* out, int maxCount) const;
    /**
     * @brief The last n (<= 4096) samples of the Cosmos return, mono, oldest first -- for its spectrum.
     * @param out  receives the samples
     * @param n    how many, clamped to 0 .. 4096
     * @return     how many were written
     */
    int  cosmosTap(float* out, int n) const;
    /**
     * @brief The same for the finished output, after the master gain and the clipper.
     * @param out  receives the samples, mono, oldest first
     * @param n    how many, clamped to 0 .. kOutTapLen
     * @return     how many were written
     */
    int  outputTap(float* out, int n) const;


    /** @brief The scale in force, as the Scale parameter chose it. @return the scale */
    const FixedScale& scale() const { return *scale_; }
    /**
     * @brief The Scala slot itself, whatever the Scale parameter happens to be pointing at: a preset
     *        chooses its own scale, and that must not be read as "the tuning the player loaded is gone".
     * @return the User (Scala) scale
     */
    const FixedScale& userScale() const { return scales_[kUserScaleIndex]; }
    /**
     * @brief The frequency a note is tuned to right now: the scale blended with 12-TET by Purity, the
     *        note's adaptive offset and the shared comma, the transpose, and the stretched octave
     *        (Engine.cpp explains each).
     * @param note  MIDI note, 0 .. 127
     * @return      Hz
     */
    double frequencyOf(int note) const;
    /** @brief The rate prepare() was given. @return Hz */
    double sampleRate() const { return sr_; }

    /**
     * @name Observers for the GUI (approximate, lock-free).
     * @{ */
    /** @brief Voices sounding at the end of the last block. @return 0 .. kMaxVoices */
    int  activeVoices() const { return activeVoices_.load(std::memory_order_relaxed); }
    /**
     * @brief Every strike the instrument has played since it was prepared, keys and conductor together.
     * @return the count, summed over the voices
     */
    unsigned strikesFired() const { unsigned n = 0; for (const auto& v : voices_) n += v.strikes(); return n; }
    /** @brief The first conductor's root at the end of the last block. @return the MIDI note */
    int  brainRoot() const    { return brainRoot_.load(std::memory_order_relaxed); }
    /** @} */
    /**
     * @brief Both conductors fill their clusters now rather than at their event rate.
     *
     * Used when this
     * engine takes over from one that was already sounding; see ClusterBrain::requestFill.
     */
    void requestBrainFill() { brain_.requestFill(); brain2_.requestFill(); }
    /**
     * @brief Read out and hand over what the conductors are holding; see ClusterBrain::adopt.
     *
     * The two
     * are kept apart: the background conductor's cluster belongs to the background.
     *
     * @param notes   receives the cluster's notes
     * @param vels    receives their velocities
     * @param second  false for the first conductor, true for the background one
     * @return        how many notes were written
     */
    int soundingCluster(int* notes, float* vels, bool second = false) const
    { return (second ? brain2_ : brain_).soundingNotes(notes, vels); }
    /**
     * @brief Takes a cluster over from the engine that is leaving (message thread, before this one is
     *        heard): the notes are started as the conductor's own would be, already a minute old so
     *        their Bloom and source delays are past, and the conductor keeps filling when the cluster
     *        is thinner than its density asks (EngineRender.cpp).
     * @param notes   the notes soundingCluster() read out
     * @param vels    their velocities
     * @param count   how many
     * @param second  false for the first conductor, true for the background one
     */
    void adoptCluster(const int* notes, const float* vels, int count, bool second = false);
    /**
     * @brief The key the conductor has found itself in, measured from what has been sounding and
     *        for how long.
     *
     * Read on the message thread for the panel; never set from outside.
     *
     * @return the estimate (ClusterBrain::estimatedKey)
     */
    KeyEstimate brainKey() const { return brain_.estimatedKey(); }
    /**
     * @brief The arc's value when it follows the clock instead of its own drift: the night's bottom at
     *        four in the morning, its top at four in the afternoon, a cosine between.
     *
     * Pure, so the
     * selftest can hold it to that.
     *
     * @param hourOfDay  0 .. 24
     * @return           -1 at four in the morning, 1 at four in the afternoon
     */
    static float clockArcValue(double hourOfDay)
    {
        return static_cast<float>(-std::cos(2.0 * 3.14159265358979323846 * (hourOfDay - 4.0) / 24.0));
    }
    /** @brief The arc as the engine reads it this block, before the Amount. @return -1..1 */
    float arcNow() const { return arcOut_; }
    /**
     * @name The beat rate, the arc's lean and the guard
     * The beat rate the BEAT source is following, the arc's current lean on the harmony, and the
     * factor the fluctuation guard is applying to the purity drift. For the panel and the tests.
     * @{ */
    /** @brief The beat rate the BEAT source follows. @return Hz, 0 when the chord is in tune */
    float beatHz() const { return beatHz_; }
    /** @brief The arc's lean on the harmony. @return arc times Arc Harmony, -1..1 */
    float arcLean() const { return arcLean_; }
    /** @brief What the fluctuation guard is doing to the purity drift. @return the factor, 0 .. 2, 1 for nothing */
    float guardFactor() const { return guardFactor_; }
    /** @} */
    /**
     * @name The conductor's homeostat and the comma
     * The conductor's homeostat lean and its measured interval entropy, and the adaptive
     * intonation's common offset in cents. For the panel and the tests.
     * @{ */
    /** @brief The homeostat's lean. @return ClusterBrain::lean */
    float  brainLean() const { return brain_.lean(); }
    /** @brief The measured interval entropy. @return bits (ClusterBrain::entropyBits) */
    float  brainEntropyBits() const { return brain_.entropyBits(); }
    /** @brief The adaptive intonation's shared offset. @return cents */
    double commaCents() const { return commaCents_; }
    /** @} */
    /**
     * @name For the panel's Tuning and Coherence displays
     * For the panel's Tuning and Coherence displays: the timbre's partial template (the roughness
     * curve is drawn from it), the tide's current offset in cents, the conductor's deja-vu ring
     * and its cascade excitation.
     * @{ */
    /** @brief The partial template the conductor judges intervals with. @return amplitudes and ratios of the current timbre */
    const BrainSpectrum& brainSpectrum() const { return brainSpec_; }
    /** @brief The tide's current offset. @return cents */
    float  tideNow() const { return tide_ * tideDrift_.value(); }
    /** @brief One entry of the conductor's deja-vu ring. @param i the slot @return the note (ClusterBrain::ringNote) */
    int    brainRingNote(int i) const { return brain_.ringNote(i); }
    /** @brief Where the deja-vu ring is writing. @return the slot (ClusterBrain::ringPos) */
    int    brainRingPos() const { return brain_.ringPos(); }
    /** @brief The cascade's excitation right now. @return the Hawkes intensity, 0 and up */
    float  brainExcitation() const { return static_cast<float>(brain_.excitation()); }
    /** @} */
    /** @brief Whether the arc follows the clock. @return the Arc Clock parameter as read this block */
    bool   arcClockOn() const { return arcClock_; }
    /** @brief The hour the arc is following. @return 0 .. 24, fractional */
    double clockHour() const { return clockHour_; }
    /**
     * @brief Pin the hour the arc follows instead of asking the operating system.
     *
     * An instrument that
     * knows what time it is cannot be measured: five hundred presets in the library have the arc
     * clock on, and their descriptors came out different every time the pass ran, because the
     * sound really was different at four in the afternoon and at midnight. The measurement pins
     * it; playing does not (-1 = the wall clock, as always).
     *
     * @param h  the hour, 0 .. 24, or -1 for the wall clock
     */
    void   setClockHourOverride(double h) { clockHourOverride_ = h; }
    /**
     * @brief The offset, in cents, that tunes `note` pure against what is sounding now (0 if nothing is).
     *
     * Against every sounding voice the interval is reduced to an octave and matched to the nearest
     * small-integer ratio, the offsets weighted by the ratio's simplicity, capped at thirty cents
     * (Engine.cpp). Raw: frequencyOf() scales it by the Adapt amount.
     *
     * @param note  MIDI note, 0 .. 127
     * @return      cents, -30 .. 30
     */
    float  adaptiveOffset(int note) const;
    /**
     * @brief What Match would make of partial h (1-based) against the current scale: the ratio to f0.
     *
     * The degree of the scale nearest to the partial, counted in periods from the fundamental; the
     * plain harmonic for the timbre scale, which is itself computed from the spectrum.
     *
     * @param h  the partial, 1-based
     * @return   its ratio to the fundamental
     */
    double matchedPartialRatio(int h) const;
    /** @brief The arc times its Amount, as the panel draws it (any thread). @return -1..1 */
    float arcValue() const    { return arcValue_.load(std::memory_order_relaxed); }
    /**
     * @brief Which notes were sounding at the end of the last block (any thread, from two 64-bit masks).
     * @param out  receives a flag per MIDI note
     */
    void soundingNotes(bool (&out)[128]) const;
    /**
     * @brief Distance (0 near .. 1 far) of the voice sounding `note`, or -1 if none.
     * @param note  MIDI note, 0 .. 127
     * @return      the distance, or -1
     */
    float noteDistance(int note) const;
    /**
     * @brief Envelope level (0..1) of the loudest voice on `note`, 0 if none.
     * @param note  MIDI note, 0 .. 127
     * @return      the level, 0 for none or a note out of range
     */
    float noteLevel(int note) const { return (note >= 0 && note < 128) ? noteLevel_[note].load(std::memory_order_relaxed) : 0.0f; }

private:
    /**
     * @brief Who started a voice: the keys, the first conductor, the background conductor or a near
     *        event (OwnerMidi 0, OwnerBrain 1, OwnerBrain2 2, OwnerNear 3). A note is stopped by its
     *        owner alone, and the slot roles reckon the keys, the two conductors and the near
     *        events as three separate groups.
     */
    enum Owner { OwnerMidi = 0, OwnerBrain = 1, OwnerBrain2 = 2, OwnerNear = 3 };
    /**
     * @brief The voice a note goes into: the one already sounding that note for that owner, else a
     *        free one, else the quietest releasing one, else the oldest.
     * @param note   MIDI note
     * @param owner  an Owner
     * @return       never null
     */
    Voice* allocate(int note, int owner);
    /**
     * @name The near layer
     * The near layer: an event's note started on the near source, what the scheduler asks for,
     * the places of the notes for the slot roles, and the scheduler stepped with what it needs
     * to know -- from the render and from the audit alike.
     * @{ */
    /**
     * @brief A near event's note: on the near source, with the event's shape, never a strike unless the
     *        section has one, and placed by the scheduler rather than by the dice; the event's clip is
     *        picked from the pool here, never the one just played.
     * @param e  the event (type On)
     */
    void   startNearNote(const NearNote& e);
    /**
     * @brief Carries out one scheduler event: On starts a near note, Off stops it, Glide slides every
     *        held near voice to the new note, Move sends them to a new distance.
     * @param e  the event
     */
    void   nearEmit(const NearNote& e);
    /**
     * @brief Every sounding note's place in its group, for the slot roles.
     *
     * Releasing notes hand theirs on: they are left out of the reckoning, so the note above a bass
     * that is fading becomes the lowest while the bass is still audible, which is when the ear
     * wants the cello to move up to it (Engine.cpp).
     */
    void   updatePlaces();
    /**
     * @brief Steps the near scheduler with what it needs to know -- the root, the cluster, the
     *        excitation, the silence, the root's age, the tempo, the time since the last onset and
     *        whether a background voice is releasing -- and passes what the foreground asks of the
     *        background on: no new onset while a Note or a Phrase speaks, the root held and the pace
     *        halved under a sequence.
     * @tparam EmitFn  callable as emit(const NearNote&)
     * @param dt       seconds in this control block
     * @param emit     receives the scheduler's events (nearEmit() in the render, a recorder in the audit)
     */
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
    /** @} */
    /**
     * @brief Starts a voice on a note for an owner: the phrase clock and Retrigger LFOs restart when it
     *        arrives into silence, the adaptive offset is decided, the strike coin thrown for the
     *        conductor's notes, the place among the owner's notes set, and portamento applied to a key.
     *
     * `ageSeconds` is how long this note is to be treated as having been sounding already. It is
     * zero for a note that is played and only set where a cluster is handed from one engine to
     * another at a preset change: the note is not new there, it is the same note on another
     * instrument, and starting its Bloom and its source delays from zero would make the arriving
     * preset take half a minute to become itself.
     *
     * @param note        MIDI note, 0 .. 127
     * @param velocity    0 .. 1
     * @param owner       an Owner
     * @param distance    0 near .. 1 far, the voice's plane
     * @param ageSeconds  see above; 0 for a new note
     */
    void   startNote(int note, float velocity, int owner, float distance, float ageSeconds = 0.0f);
    /**
     * @brief Releases every voice sounding a note for an owner, and hands its place on.
     * @param note   MIDI note
     * @param owner  an Owner
     */
    void   stopNote(int note, int owner);
    /**
     * @brief Turns the parameter atomics into this block's structures: every effective parameter through
     *        the Inertia glide and the matrix offset into vp_, bp_, bp2_, np_, vpNear_ and the effects'
     *        setters; the Match ratios, the conductor's spectrum, the timbre scale, the tuning, the seed.
     *        Once per block, after stepModulation() (EngineControl.cpp).
     */
    void   readParams();
    /**
     * @brief The signal path for one chunk of at most maxBlock_ samples: the conductors and the near
     *        scheduler stepped per control block, the voices rendered onto the two planes, and the
     *        planes through blur, ensemble, delays, cloud, Cosmos, Memory, reverbs, room, feedback,
     *        Foundation, body, mid/side, DC blockers, Patina, subsonic, master and clipper into L/R,
     *        the stems and the output tap alongside (EngineRender.cpp).
     * @param L  left output for this chunk
     * @param R  right output for this chunk
     * @param n  samples in the chunk
     */
    void   renderChunk(float* L, float* R, int n);

    std::atomic<float> params_[kNumParams];   ///< the parameters as the host set them, in their own units
    /** @var std::atomic<float> slotA_[kNumParams]
     *  @brief morph slot A's snapshot */
    std::atomic<float> slotA_[kNumParams], slotB_[kNumParams];   ///< slotB_: morph slot B's snapshot
    std::atomic<bool>  slotSet_[2] { false, false };   ///< chosen, or still following the knobs
    std::atomic<float> morphCur_{ 0.0f };   ///< the gliding morph position, 0 (A) .. 1 (B)
    /**
     * @name Preset map blend
     * Preset map blend (audio thread writes, host reads)
     * @{ */
    std::atomic<float> blendCur_[kNumParams];   ///< where each parameter's glide towards the blend stands
    std::atomic<bool>  blendActive_{ false };   ///< the map is on and its vectors are ready: the blend overrides the parameters
    float              blendTarget_[kNumParams] = {};   ///< the blend of the presets around the cursor, recomputed when the cursor moves
    /**
     * @brief The map's step, once per block: engages when MapActive is on and the map is ready (starting
     *        from what sounds now), recomputes the blended target when the cursor moved, and glides every
     *        parameter towards it with the Morph Glide time constant, in the skewed domain.
     * @param n  samples in the block
     */
    void updateBlend(int n);
    /** @} */
    Voice        voices_[kMaxVoices];   ///< the voices
    /** @var ClusterBrain brain_
     *  @brief the first conductor */
    ClusterBrain brain_, brain2_;       ///< brain2_: the background conductor, its root a fixed interval from the first's
    BrainParams  bp2_;                  ///< the background conductor's parameters, filled by readParams()
    float        brain2Depth_ = 0.9f;   ///< the background conductor's plane as a share of Depth
    float        layerDepth_ = 0.0f;   ///< how much of a note's plane comes from its role, not the draw
    float        cloudToNear_ = 0.0f;  ///< how much of the cloud comes back in front rather than behind
    int          brain2Interval_ = 0;   ///< semitones between the two conductors' roots
    bool         brain2On_ = false;     ///< the background conductor runs
    int          brainQuant_ = 0;      ///< SyncDiv: the conductor's decisions land on the grid
    std::atomic<bool> autoStepAsked_{ false };   ///< the Step trigger, from the panel or a controller
    bool         autoStepWas_ = false;           ///< the parameter's previous state, for the rising edge
    /** @var double quantAcc_
     *  @brief seconds held back for the quantised conductor, handed over at the next tick */
    double       quantAcc_ = 0.0, lastBeat_ = 0.0;   ///< lastBeat_: the beat position at the previous control block, for the tick
    float        bendRange_ = 2.0f;     ///< semitones a full bend moves
    Ensemble     ensemble_;             ///< the near bus's ensemble
    /** @var StereoDelay delay_
     *  @brief the first delay on the near bus */
    StereoDelay  delay_, delay2_;       ///< delay2_: the second, in series after the first
    GrainCloud   cloud_;                ///< the granular cloud, fed from the near bus
    /** @var float delay2Mix_
     *  @brief the second delay's share into the near bus */
    /** @var float delay2ToFar_
     *  @brief the second delay's share into the far bus */
    float        delay2Mix_ = 0.0f, delay2ToFar_ = 0.5f, cloudSend_ = 0.0f;   ///< cloudSend_: the near bus's share into the cloud
    /** @brief The Memory: a parallel send off the near bus like the Cosmos, with its own two returns. */
    Memory       memory_;
    /** @var float memSend_
     *  @brief the near bus's share into the Memory */
    /** @var float memReturn_
     *  @brief the Memory's return into the near bus */
    float        memSend_ = 0.0f, memReturn_ = 0.5f, memToFar_ = 0.4f;   ///< memToFar_: the Memory's return into the far bus
    /** @var Smoother smMemSend_
     *  @brief per-sample smoothing of memSend_ */
    /** @var Smoother smMemReturn_
     *  @brief per-sample smoothing of memReturn_ */
    Smoother     smMemSend_, smMemReturn_, smMemToFar_;   ///< smMemToFar_: per-sample smoothing of memToFar_
    /** @var std::vector<float> memL_
     *  @brief the Memory's send and return, left */
    std::vector<float> memL_, memR_;   ///< memR_: the same, right
    /** @var Reverb nearReverb_
     *  @brief the small room of the near plane */
    Reverb       nearReverb_, farReverb_;   ///< farReverb_: the dark, wide hall of the far plane
    Diffuser     diffuser_;             ///< the diffusion ahead of the far reverb
    std::vector<float> coupleBuf_;      ///< the previous block's foreground, for the sympathetic coupling
    float        sympathy_ = 0.0f;      ///< how much of the foreground the voices hear of each other
    Unmask       unmask_;               ///< the background giving way to the foreground band by band
    HaasBand     haas_;                 ///< the Haas widening of the foreground
    EarlyRoom    early_;                ///< the room's early reflections, on the near bus
    Body         body_;                 ///< the resonant body the whole mix passes through
    Patina       patina_;               ///< the master's age: wow, lost highs, a noise floor
    /** @var float bodyLevel_
     *  @brief how much of the body's answer is added */
    float        bodyLevel_ = 0.0f, bodyPitch_ = 1.0f;   ///< bodyPitch_: the body's pitch as a factor on the root
    Smoother     smBody_;               ///< per-sample smoothing of bodyLevel_
    MidSide      midSide_;              ///< the mid/side stage: bass mono, side air, width, tilt
    /**
     * @name Room (convolution) on the far plane
     * Room (convolution) on the far plane: level, source, pre-delay ring, tail low-pass
     * @{ */
    Convolver    room_;                 ///< the convolution reverb
    /** @var bool userImpulse_
     *  @brief a loaded impulse plays rather than the generated hall */
    bool         userImpulse_ = false, hasImpulseB_ = false;   ///< hasImpulseB_: a second impulse is loaded for Room Morph
    float        roomMorph_ = 0.0f;     ///< Room Morph as read this block (0 without a second impulse)
    Smoother     smRoomMorph_;          ///< the morph's glide, moved a block at a time
    float        masterGain_ = -6.0f;   ///< Master Gain in dB, through the matrix
    float        roomMaxSeconds_ = 60.0f;   ///< the longest impulse the room keeps, set before prepare()
    /** @} */
    /**
     * @name modulation
     * @{ */
    Lfo          lfo_[kNumLfos];        ///< the eight LFOs
    LfoSpec      lfoSpec_[kNumLfos];    ///< their settings this block
    /** @var ModEnv envShape_[kNumModEnvs]
     *  @brief the six envelope shapes the audio thread reads */
    ModEnv       envShape_[kNumModEnvs], envPending_[kNumModEnvs];   ///< envPending_: the six as edited, copied over under the lock
    ModEnvSpec   envSpec_[kNumModEnvs]; ///< the six envelopes' settings this block
    /**
     * @brief The sources' own envelopes, published with the six under the same lock.
     *
     * A fresh one rises to
     * the source's level over its first second (times Time) and stays there.
     */
    static constexpr const char* kSrcEnvDefault = "0:0/1:1";
    /** @var ModEnv srcEnvShape_[kSlots]
     *  @brief the four sources' own shapes the audio thread reads */
    ModEnv       srcEnvShape_[kSlots], srcEnvPending_[kSlots];   ///< srcEnvPending_: the four as edited
    ModEnvSpec   srcEnvSpec_[kSlots];   ///< the four sources' envelope settings this block
    /** @var ModMatrix matrix_
     *  @brief the matrix the audio thread sums */
    ModMatrix    matrix_, matrixPending_;   ///< matrixPending_: the matrix as edited
    std::atomic<int> modVersion_{ 0 };  ///< bumped by publishModulation(); the audio thread copies when it differs from modSeen_
    /**
     * @brief Held by whoever is touching the pending matrix and shapes.
     *
     * The message thread waits for
     * it; the audio thread only tries, and leaves an edit in progress alone until the next block.
     */
    std::atomic_flag modLock_ = ATOMIC_FLAG_INIT;
    int          modSeen_ = 0;          ///< the modVersion_ the audio thread last copied
    float        modSrc_[kNumModSources] = {};   ///< every source's value this block, -1..1
    float        modOut_[kNumParams] = {};       ///< what the matrix adds to each parameter this block, in its own units
    double       envTime_[kNumModEnvs] = {};     ///< each envelope's clock, seconds since the phrase began
    bool         envHeld_ = false;      ///< a non-releasing voice sounds: the envelopes count as held
    float        randomPerNote_ = 0.0f; ///< the RandomPerNote source: one draw per note
    bool         phraseStart_ = false;   ///< a note arrived into silence: envelopes and Retrigger LFOs restart
    /**
     * @brief The wheel: where it was put, and where the modulation has got to.
     *
     * A controller sends 128
     * steps and a step on a cutoff is audible, so what the matrix reads is the smoothed one.
     * wheelTarget_ is what setWheel() stored.
     */
    float        wheelTarget_ = 0.0f, wheel_ = 0.0f;   ///< wheel_: the smoothed value the matrix reads (30 ms)
    BrainSpectrum brainSpec_;     ///< the partial template the conductor judges intervals with (Timbre)
    /** @brief The stretched octave, in cents per octave away from the reference pitch (0 = exact 2:1). */
    float        stretchCents_ = 0.0f;
    bool         stretchChanged_ = false;   ///< the stretch moved this block: the sounding voices retune
    /**
     * @brief The head's yaw in degrees, for the Headphones binaural mode: the whole field turns the
     *        other way so a source stays where it is in the room.
     *
     * Written from any thread.
     */
    std::atomic<float> headYawDeg_{ 0.0f };
    /**
     * @brief One step of every modulator, then the matrix summed into modOut_.
     *
     * Once per block, before readParams(), so the values the parameters are read with already
     * carry the modulation: picks up a published edit (tried, never waited for), reads each LFO's
     * and envelope's settings with the previous block's modulation on them, steps them, fills the
     * rest of the source table (the Beat, the Lenia field and the attractors only when routed, the
     * cascade, the hands, the piece's own shape), and applies the matrix -- the performance
     * parameters excepted (EngineControl.cpp).
     *
     * @param dt  seconds in the block
     */
    void         stepModulation(float dt);
    /** @} */
    /**
     * @name clock
     * clock: the three candidates and the one resolved for this block
     * @{ */
    /** @var double hostBpm_
     *  @brief the host's tempo, from setHostClock() */
    double       hostBpm_ = 0.0, hostBeat_ = 0.0;   ///< hostBeat_: the host's beat position
    /** @var bool hostPlaying_
     *  @brief the host's transport is running */
    bool         hostPlaying_ = false, hostSeen_ = false;   ///< hostSeen_: setHostClock() has been called at all
    /** @var double midiBpm_
     *  @brief the tempo the MIDI clock ticks imply, smoothed */
    /** @var double midiBeat_
     *  @brief the beat position counted from the ticks */
    double       midiBpm_ = 0.0, midiBeat_ = 0.0, midiSilence_ = 0.0;   ///< midiSilence_: seconds since the last tick; past two the MIDI clock is not trusted
    int          midiTicks_ = 0;        ///< ticks received; the MIDI clock counts from twenty-four on
    bool         midiRunning_ = true;   ///< between Start/Continue and Stop
    double       intBeat_ = 0.0;        ///< the internal clock's beat position
    /** @var double bpm_
     *  @brief the tempo resolved for this block */
    double       bpm_ = 90.0, beat_ = 0.0;   ///< beat_: the beat position resolved for this block
    bool         running_ = true;       ///< whether the resolved clock runs
    /** @var std::atomic<double> tempoOut_
     *  @brief bpm_ for the displays */
    std::atomic<double> tempoOut_{ 90.0 }, beatOut_{ 0.0 };   ///< beatOut_: beat_ for the displays
    std::atomic<bool>   runningOut_{ true };   ///< running_ for the displays
    /**
     * @brief Resolves the clock for this block: the host's while ClockSource says so and one has been
     *        seen, the MIDI clock while it says so and ticks are arriving, otherwise the Tempo knob
     *        counting while Run is on; then publishes the three for the displays.
     * @param dt  seconds in the block
     */
    void         stepClock(double dt);
    /** @} */

    /** @var Route route_
     *  @brief the route the audio thread walks */
    Route        route_, routePending_;   ///< routePending_: the route as edited
    std::atomic<int> routeVersion_{ 0 };  ///< bumped by every edit; the audio thread copies when it differs from routeSeen_
    int          routeSeen_ = 0;          ///< the routeVersion_ last copied
    bool         routeWasActive_ = false; ///< RouteActive at the previous block, for the start and stop edges
    /** @var float roomLevel_
     *  @brief Room Level as read this block */
    /** @var float roomLevelCur_
     *  @brief the same, glided per sample (50 ms) */
    float        roomLevel_ = 0.0f, roomLevelCur_ = 0.0f, roomHighcut_ = 5000.0f;   ///< roomHighcut_: the tail's low-pass corner, Hz
    float        roomLowcut_ = 20.0f;     ///< the room's high-pass corner, Hz; at or below 21 it is off
    /** @var float roomHpL1_
     *  @brief the room's high-pass, first stage, left */
    /** @var float roomHpR1_
     *  @brief first stage, right */
    /** @var float roomHpL2_
     *  @brief second stage, left */
    float        roomHpL1_ = 0.0f, roomHpR1_ = 0.0f, roomHpL2_ = 0.0f, roomHpR2_ = 0.0f;   ///< roomHpR2_: second stage, right
    /**
     * @name Subsonic
     * Subsonic: two cascaded one-pole high-passes on the finished output, 24 dB/oct with the
     * master DC blocker in front of them. Zero hertz means the whole thing is skipped.
     * @{ */
    LoudnessMeter loudness_;    ///< the meter on the finished output (loudness())
    float        subsonicHz_ = 0.0f;   ///< the -3 dB corner, Hz; 0 = off
    /** @var float subL1_
     *  @brief first one-pole, left */
    /** @var float subR1_
     *  @brief first one-pole, right */
    /** @var float subL2_
     *  @brief second one-pole, left */
    float        subL1_ = 0.0f, subR1_ = 0.0f, subL2_ = 0.0f, subR2_ = 0.0f;   ///< subR2_: second one-pole, right
    /** @var float subL3_
     *  @brief third one-pole, left */
    /** @var float subR3_
     *  @brief third one-pole, right */
    /** @var float subL4_
     *  @brief fourth one-pole, left */
    float        subL3_ = 0.0f, subR3_ = 0.0f, subL4_ = 0.0f, subR4_ = 0.0f;   ///< subR4_: fourth one-pole, right
    /** @} */
    /** @var int roomSource_
     *  @brief what the room hears: 0 the far sends, 1 the finished near bus */
    int          roomSource_ = 0, roomPreDelay_ = 0;   ///< roomPreDelay_: the pre-delay in samples (the convolver's latency comes off it)
    long         roomTailLeft_ = 0;     ///< samples the room keeps running after its level reached zero, so the tail can finish
    /** @var std::vector<float> roomInL_
     *  @brief the room's input after the pre-delay, left */
    /** @var std::vector<float> roomInR_
     *  @brief the room's input, right */
    /** @var std::vector<float> roomOutL_
     *  @brief the convolver's output, left */
    /** @var std::vector<float> roomOutR_
     *  @brief the convolver's output, right */
    /** @var std::vector<float> roomDelayL_
     *  @brief the pre-delay ring, left (at least 320 ms) */
    std::vector<float> roomInL_, roomInR_, roomOutL_, roomOutR_, roomDelayL_, roomDelayR_;   ///< roomDelayR_: the pre-delay ring, right
    /** @var int roomDelayW_
     *  @brief the pre-delay ring's write position */
    int          roomDelayW_ = 0, roomDelayMask_ = 0;   ///< roomDelayMask_: the ring's length minus one
    /** @var float roomLpL_
     *  @brief the tail low-pass's state, left */
    float        roomLpL_ = 0.0f, roomLpR_ = 0.0f;   ///< roomLpR_: the same, right
    /**
     * @name Cosmos path
     * Cosmos path (parallel send from the near bus) and the shimmer loop around the far reverb.
     * @{ */
    FreqShifter   shifter_;     ///< the Cosmos frequency shifter
    CombResonator resonator_;   ///< the Cosmos comb resonator on the root
    VowelFilter   vowel_;       ///< the Cosmos vowel filter
    Nebula        nebula_;      ///< the Cosmos spectral smear
    /** @} */
    /**
     * @name Blur, the tide, the turning far field
     * Blur on the near bus (a second Nebula), the pitch tide, the turning far field. Their
     * drifters run on a side stream so enabling them never moves the brain's dice.
     * @{ */
    Nebula        blur_;        ///< the near bus's spectral smear
    float         blurMix_ = 0.0f;   ///< how much of the smeared near bus replaces the plain one
    Smoother      smBlur_;      ///< per-sample smoothing of blurMix_
    /** @var Drifter tideDrift_
     *  @brief the pitch tide's slow drift */
    Drifter       tideDrift_, rotDrift_;   ///< rotDrift_: the far field's slow rotation
    /** @var float tide_
     *  @brief the tide's depth in cents */
    /** @var float tidePeriod_
     *  @brief the tide's period in minutes */
    /** @var float farRotate_
     *  @brief how far the far field turns */
    float         tide_ = 0.0f, tidePeriod_ = 12.0f, farRotate_ = 0.0f, farWidth_ = 1.0f;   ///< farWidth_: the far plane's width alone, 1 = as the reverb made it
    /** @} */
    /** @brief Envelopment: the far bus's low-mid side channel, between Bass Mono and 500 Hz. */
    float         envelop_ = 0.0f;
    Svf           envBell_;       ///< the side channel's band, from Bass Mono to 500 Hz
    Smoother      smEnvelop_;     ///< per-sample smoothing of envelop_
    /**
     * @brief Comodulation.
     *
     * The background's gain follows one slow random envelope, shared by every
     * band of it at once. Hall, Haggard and Fernandes (1984): a tone in a noise whose bands
     * rise and fall together is heard ten to fifteen decibels further down than in a noise
     * whose bands move independently -- comodulation masking release. The ear groups what
     * moves together into one object and hears past it. Nothing in the foreground follows
     * the envelope, so the foreground is what is heard past it.
     */
    float         comod_ = 0.0f;
    Smoother      smComod_;       ///< per-sample smoothing of comod_
    Drifter       comodDrift_;    ///< the shared random envelope, about nine new targets a second
    Rng           comodRng_;      ///< the envelope's own stream
    bool          comodInit_ = false;   ///< the stream and the drifter have been seeded (on first use)
    Rng           auxRng_;        ///< the side stream for the tide, the rotation and the Vector's wander
    /**
     * @brief The strike's own coin, on its own stream: drawn only when a chance below one asks for it,
     *        so an instrument that strikes on every note renders exactly as it did before there was a
     *        chance at all.
     *
     * (A draw taken from a shared stream would move every preset that follows.)
     */
    Rng           strikeRng_;
    /** @var float strikeChance_
     *  @brief Strike Chance: the probability a conductor's note strikes */
    float         strikeChance_ = 1.0f, strikeCluster_ = 0.0f;   ///< strikeCluster_: how much the cascade's excitation weights that coin
    /**
     * @name The near layer (13.09.2026)
     * The near layer (13.09.2026): the scheduler, its parameters, and the voice parameters a near
     * event's voice renders with -- the sound preset's, with the four slots put out and the Near
     * Source in the last of them, and the section's own envelope, filter and strike.
     * @{ */
    NearEvents    near_;          ///< the scheduler
    NearParams    np_;            ///< its parameters this block
    float         nearGain_ = 1.0f;   ///< fore_gain as a factor, read with the rest of the near layer
    VoiceParams   vpNear_;        ///< what a near event's voice renders with
    bool          rolesUsed_ = false;   ///< any slot with a role other than All: the places are then kept
    /** @} */
    /**
     * @brief The excitation the cascade has been running at lately, so Cluster can weigh a note against
     *        the piece's own average rather than an absolute number: above it the strike grows likelier,
     *        below it rarer, and the count over an hour still follows Chance.
     *
     * Without this the lift
     * saturated -- at any excitation worth having, every note struck, which is what Chance is for.
     */
    double        strikeExcAvg_ = 0.0;
    /**
     * @brief The same excitation as a smoothed signal (the modulation source `cascade`) and its slow
     *        average: the Cosmos swells with it by default, and anything else can be routed from it.
     *
     * cascadeNow_ is the smoothed signal (two seconds).
     */
    float         cascadeNow_ = 0.0f, cascadeAvg_ = 0.0f;   ///< cascadeAvg_: its slow average (ninety seconds)
    float         cosmosSwell_ = 0.5f;   ///< how much the Cosmos send follows the cascade above and below its average
    /** @var PitchShifter shimmerL_
     *  @brief the grain shimmer's shifter, left */
    PitchShifter  shimmerL_, shimmerR_;   ///< shimmerR_: the same, right
    SpectralShifter shimmerSpec_;   ///< the shimmer's shifter in the spectrum (Shimmer Mode = Spectral)
    int           shimmerMode_ = 0;   ///< 0 spectral, 1 grain
    bool          shimmerWasOn_ = false;   ///< the shimmer ran last block; the spectral shifter starts empty when it opens
    Drifter       shiftDrift_;    ///< the Cosmos shift's drift
    /** @var Drifter vecDriftX_
     *  @brief the Vector's wander in x (its rate the golden ratio off the y's) */
    Drifter       vecDriftX_, vecDriftY_;   ///< the Vector's point wandering on its own (vecDriftY_: the wander in y)
    /** @var float cosmosSend_
     *  @brief the near bus's share into the Cosmos */
    /** @var float cosmosReturn_
     *  @brief the Cosmos return into the near bus */
    /** @var float cosmosToFar_
     *  @brief the Cosmos return into the far bus */
    float         cosmosSend_ = 0.0f, cosmosReturn_ = 0.5f, cosmosToFar_ = 0.5f, cosmosNebula_ = 0.0f;   ///< cosmosNebula_: how much of the Cosmos goes through its smear
    /** @var float cosmosShimmer_
     *  @brief the shimmer's feedback amount */
    /** @var float shimmerLpL_
     *  @brief the shimmer loop's 4 kHz low-pass, left */
    /** @var float shimmerLpR_
     *  @brief the same, right */
    float         cosmosShimmer_ = 0.0f, shimmerLpL_ = 0.0f, shimmerLpR_ = 0.0f, shimmerEnv_ = 0.0f;   ///< shimmerEnv_: the far reverb's level follower that throttles the shimmer loop
    /** @var std::vector<float> cosL_
     *  @brief the Cosmos (and the cloud's send) scratch, left */
    /** @var std::vector<float> cosR_
     *  @brief the same, right */
    /** @var std::vector<float> nebL_
     *  @brief the smear's output, left (the blur's as well) */
    /** @var std::vector<float> nebR_
     *  @brief the same, right */
    /** @var std::vector<float> shimL_
     *  @brief the shimmer's feedback for the next block, left */
    std::vector<float> cosL_, cosR_, nebL_, nebR_, shimL_, shimR_;   ///< shimR_: the same, right
    /** @var std::vector<float> cloudL_
     *  @brief the cloud's own output, left, when it is shared between the planes */
    std::vector<float> cloudL_, cloudR_;   ///< the cloud on its own, when it is shared between planes (cloudR_: right)
    float         cosTap_[4096] = {};   ///< ring of the cosmos return for the display (torn reads cost a pixel)
    int           cosTapW_ = 0;   ///< the cosmos ring's write position
    /**
     * @brief The same for the finished output, but four times as long.
     *
     * A spectrum of a drone is only
     * worth drawing if it can tell one partial from the next, and 4096 samples cannot: measured
     * on a 40 Hz tone with its octave, the trough between the two peaks is 14 dB down at 4096 and
     * 58 dB down at 16384 -- one hump against two lines. A low just fifth (60 and 90 Hz) behaves
     * the same way. The signal is stationary for seconds at a time, so the long window costs
     * nothing but 64 kB.
     */
    float         outTap_[kOutTapLen] = {};
    int           outTapW_ = 0;   ///< the output ring's write position
    /**
     * @name Feedback loop
     * Feedback loop: the previous chunk's output mix, low-passed, saturated and throttled,
     * kept in a ring so any chunk length reads back exactly the samples just written.
     * @{ */
    /** @var std::vector<float> fbRingL_
     *  @brief the loop's ring, left */
    /** @var std::vector<float> fbRingR_
     *  @brief the loop's ring, right */
    /** @var std::vector<float> fbInL_
     *  @brief what comes back into the near bus this chunk, throttled, left */
    /** @var std::vector<float> fbInR_
     *  @brief the same, right */
    std::vector<float> fbRingL_, fbRingR_, fbInL_, fbInR_, fbMono_;   ///< fbMono_: the mono sum for the phase modulation, unthrottled
    /** @var int fbW_
     *  @brief the ring's write position */
    int           fbW_ = 0, fbMask_ = 0;   ///< fbMask_: the ring's length minus one
    /** @var float fbBus_
     *  @brief Feedback Bus: how much comes back as signal */
    /** @var float fbFm_
     *  @brief Feedback FM: how much comes back as phase modulation of the partials */
    /** @var float fbTone_
     *  @brief the loop's low-pass corner, Hz */
    float         fbBus_ = 0.0f, fbFm_ = 0.0f, fbTone_ = 1500.0f, fbDrive_ = 0.5f;   ///< fbDrive_: the loop's saturation drive
    /** @var float fbLpL_
     *  @brief the loop low-pass's state, left */
    /** @var float fbLpR_
     *  @brief the same, right */
    /** @var float fbEnv_
     *  @brief the mix's level follower that throttles the bus path */
    float         fbLpL_ = 0.0f, fbLpR_ = 0.0f, fbEnv_ = 0.0f, fbReg_ = 1.0f;   ///< fbReg_: the throttle, 1 quiet .. 0 at a mean level of 0.1
    /** @var float fbHpXL_
     *  @brief loop DC blocker, previous input, left */
    /** @var float fbHpXR_
     *  @brief loop DC blocker, previous input, right */
    /** @var float fbHpYL_
     *  @brief loop DC blocker, previous output, left */
    float         fbHpXL_ = 0.0f, fbHpXR_ = 0.0f, fbHpYL_ = 0.0f, fbHpYR_ = 0.0f;   ///< loop DC blocker (fbHpYR_: previous output, right)
    /** @} */
    VoiceParams  vp_;   ///< what the voices render with this block
    BrainParams  bp_;   ///< the first conductor's parameters this block
    Drifter      arc_;  ///< the hour-scale arc's own drift
    /** @var float arcAmount_
     *  @brief how far the arc leans on density, brightness and depth */
    float        arcAmount_ = 0.0f, arcPeriodMin_ = 40.0f;   ///< arcPeriodMin_: the arc's period in minutes (or the synced division's)
    /** @var float arcHarmony_
     *  @brief how far the arc leans on the harmony */
    float        arcHarmony_ = 0.0f, arcLean_ = 0.0f;    ///< how far the arc is leaning the harmony right now
    /**
     * @brief The arc as the rest of the engine reads it: the drifter's value, or the clock's, and for
     *        twenty seconds after a switch between them a glide from one to the other.
     *
     * arcClock_ is Arc Clock as read this block.
     */
    bool         arcClock_ = false, arcClockWas_ = false;   ///< arcClockWas_: its previous state, for the switch
    /** @var float arcOut_
     *  @brief the arc as read, -1..1 */
    float        arcOut_ = 0.0f, arcGlide_ = 0.0f;   ///< arcGlide_: seconds of glide left after a switch
    double       clockHour_ = 12.0;   ///< the hour the arc follows, 0 .. 24
    double       clockHourOverride_ = -1.0;   ///< >= 0: use this hour, do not read the clock
    int          clockCheck_ = 0;     ///< blocks since the wall clock was last asked (once a second)
    float        guardFactor_ = 1.0f;                     ///< what the fluctuation guard last did to the drift
    /**
     * @name Foundation sub voice
     * @{ */
    /** @var double subPhaseL_
     *  @brief the sub's phase, left ear, 0..1 */
    /** @var double subPhaseR_
     *  @brief the sub's phase, right ear */
    double       subPhaseL_ = 0.0, subPhaseR_ = 0.0, subFreqCur_ = 0.0;   ///< subFreqCur_: the sub's frequency, glided in the log domain (0 = not started)
    /** @var float subLevel_
     *  @brief Sub Level as read this block */
    /** @var float subLevelCur_
     *  @brief the same, glided per sample (2 s) */
    /** @var float subGlide_
     *  @brief the pitch glide's time constant, seconds */
    /** @var float subBinaural_
     *  @brief Hz between the two ears */
    float        subLevel_ = 0.0f, subLevelCur_ = 0.0f, subGlide_ = 8.0f, subBinaural_ = 0.0f, subTone_ = 0.2f;   ///< subTone_: sine (0) to triangle (1)
    /**
     * @brief Pulse: the Foundation amplitude-modulated at the Binaural rate, a raised cosine.
     *
     * The
     * binaural offset alone makes a beat only inside the brainstem, where the two ears' phases
     * are compared; an amplitude modulation is a beat on the basilar membrane itself, and it
     * drives the auditory steady-state response several times harder (the hybrid "isochronic"
     * stimulation of the entrainment literature). Whether that entrains anything worth the name
     * is a separate and less settled question; what is built here is the modulation.
     * subPulse_ is the depth as read this block.
     */
    float        subPulse_ = 0.0f, subPulseCur_ = 0.0f;   ///< subPulseCur_: the depth, glided
    double       subPulsePhase_ = 0.0;   ///< the pulse's phase, 0..1
    int          subOctave_ = 1;         ///< octaves under the root: 1 or 2
    /**
     * @brief Where the Foundation takes its pitch: 0 the conductor's root, 1 the ghost tone of the two
     *        lowest voices, 2 the lowest voice that is actually sounding.
     */
    int          subSource_ = 0;
    /** @} */
    bool         hold_ = false;   ///< Hold: the keys latch
    /** @var float depth_
     *  @brief Depth: how far back the conductor's notes are placed */
    float        depth_ = 0.7f, keysDepth_ = 0.0f;   ///< keysDepth_: the plane the keys play on
    /** @var float delayMix_
     *  @brief the first delay's share into the near bus */
    /** @var float delayToFar_
     *  @brief the first delay's share into the far bus */
    float        delayMix_ = 0.25f, delayToFar_ = 0.4f, farLevel_ = 0.8f;   ///< farLevel_: the far plane's level into the mix

    FixedScale        scales_[kNumScaleChoices];   ///< every scale the Scale parameter can choose, the Scala and Timbre slots among them
    FixedScale        userPending_;   ///< the Scala scale as set, waiting to be copied at the top of a block
    double            timbreScaleSig_ = 0.0;   ///< what the spectrum was when the scale was last built
    int               timbreScaleWait_ = 96;   ///< blocks since the timbre scale was last rebuilt; at most a few times a second
    std::atomic<int>  userVersion_{ 0 };   ///< bumped by setUserScale(); the audio thread copies when it differs from userSeen_
    std::atomic<bool> userBusy_{ false };  ///< the audio thread is copying the pending scale
    int               userSeen_ = 0;       ///< the userVersion_ last copied
    /**
     * @brief User wavetable: pending copy + version (8 KB, copied by the audio thread).
     *
     * userTable_ is the copy the audio thread reads.
     */
    Wavetable         userTable_, userTablePending_;   ///< userTablePending_: the table as set, waiting to be copied
    std::atomic<int>  tableVersion_{ 0 };   ///< bumped by setUserWavetable()
    std::atomic<bool> tableBusy_{ false };  ///< the audio thread is copying the pending table
    int               tableSeen_ = 0;       ///< the tableVersion_ last copied
    std::atomic<int>  userTableFrames_{ 0 };   ///< the pending table's frame count, for userWavetableFrames()
    /** @brief User cycles: two buffers, the audio thread reads the active one (see setUserCycles). */
    CycleTable        userCycles_[2];
    std::atomic<int>  cyclesActive_{ -1 };   ///< which of the two is active, -1 for none
    /** @brief Texture: two buffers per slot, the audio thread reads the active one and publishes which. */
    Texture           textures_[kSlots][2];
    std::atomic<int>  textureActive_[kSlots] = { -1, -1, -1, -1 };   ///< per slot, which buffer is active; -1 for none
    std::vector<Texture> nearPools_[2];         ///< the near source's own clips, the same way
    std::atomic<int>     nearPoolActive_{ -1 };   ///< which pool is active, -1 for none
    int                  nearPick_ = 0;         ///< which of them the current event plays (audio thread)
    uint32_t             nearPickRng_ = 0x9E3779B9u;   ///< the pick's own xorshift stream, seeded from the seed, never zero
    /**
     * @brief Blocks begun and blocks finished.
     *
     * Two counters rather than one, because the question a
     * loader has to answer is not "how many have gone by" but "is anything still holding what it
     * picked up before I looked" -- and those differ exactly while a block is in flight. Reading
     * `started` and then waiting for `finished` to reach it answers it without guessing: when
     * nothing is rendering the two are already equal and there is no wait at all, and when a
     * block is running the wait is exactly as long as that block, however long that happens to be.
     * blocksBegun_ is incremented as process() enters.
     */
    std::atomic<unsigned long long> blocksBegun_ { 0 }, blocksDone_ { 0 };   ///< blocksDone_: incremented as process() leaves
    /**
     * @brief message thread: until every block begun before now has ended
     *
     * Spins (50 microseconds at a time, at most 8000 times) until blocksDone_ has reached the
     * blocksBegun_ read on entry; returns at once when nothing is rendering (Engine.cpp).
     */
    void waitForQuiet();
    const FixedScale* scale_ = nullptr;   ///< the scale in force, one of scales_
    int               rootNote_ = 62;     ///< the key root as a MIDI note, 60 + Root Note
    double            refPitch_ = 440.0;  ///< the reference pitch of A4, Hz
    bool              snapKeys_ = true;   ///< Key Map: keys snap to the scale's degrees
    float             inertiaCur_[kNumParams] = {};   ///< where each float parameter's Inertia glide stands
    float             lastBlockSeconds_ = 0.005f;      ///< the previous block's length, the glides' dt
    float             kuraPhase_[4] = { 0.0f, 1.3f, 2.9f, 4.4f };   ///< Kuramoto bank phases
    /**
     * @name The Lenia field
     * The Lenia field. Thirty-two by thirty-two on a torus, a ring kernel of radius five, the
     * update spread over the blocks so that one full step costs a few rows each and never a
     * spike; stepped only while a route in the matrix reads one of its four sources, so a patch
     * that does not use it pays nothing for it. Reseeded with a few soft blobs when it dies out
     * or fills up, which on a grid this small it sometimes does.
     * @{ */
    static constexpr int kLeniaRadius = 5;   ///< the ring kernel's radius in cells
    /** @var float lenia_[kLeniaSize * kLeniaSize]
     *  @brief the field, row-major, cells 0..1 */
    float             lenia_[kLeniaSize * kLeniaSize] = {}, leniaNext_[kLeniaSize * kLeniaSize] = {};   ///< leniaNext_: the next field, built a few rows a block
    float             leniaKernel_[(2 * kLeniaRadius + 1) * (2 * kLeniaRadius + 1)] = {};   ///< the normalised ring kernel
    /** @var float leniaTarget_[4]
     *  @brief the four corner readings of the last full step */
    float             leniaTarget_[4] = {}, leniaOut_[4] = {};   ///< leniaOut_: the readings glided, what the matrix sees
    /** @var float leniaRate_
     *  @brief full steps per second */
    /** @var float leniaMu_
     *  @brief the growth rule's centre */
    float             leniaRate_ = 4.0f, leniaMu_ = 0.15f, leniaRowAcc_ = 0.0f;   ///< leniaRowAcc_: rows owed, carried between blocks
    /** @var int leniaRow_
     *  @brief the next row to update */
    /** @var int leniaSteps_
     *  @brief full steps taken */
    int               leniaRow_ = 0, leniaSteps_ = 0, leniaReseeds_ = 0;   ///< leniaReseeds_: how often the field was reseeded
    /** @var bool leniaInit_
     *  @brief the kernel is built and the field seeded */
    bool              leniaInit_ = false, leniaUsed_ = false;   ///< leniaUsed_: a route reads one of the four sources
    Rng               leniaRng_;   ///< the field's own stream for the seeding
    /**
     * @brief Advances the field by as many rows as the block owes at leniaRate_ (building the kernel and
     *        seeding on first use), reseeds it when it died or saturated, and glides the four readings.
     * @param dt  seconds in the block
     */
    void stepLenia(float dt);
    /** @brief A few soft blobs on an empty torus. From the field's own random stream, so the sound's is not touched. */
    void seedLenia();
    /** @} */
    /**
     * @brief The attractors' states in their own units, the six readings in -1..1, and the gate.
     *
     * lorenz_ is the Lorenz system's state.
     */
    double            lorenz_[3] = { 1.0, 1.0, 20.0 }, rossler_[3] = { 1.0, 1.0, 0.0 };   ///< rossler_: the Roessler system's state
    float             chaosOut_[6] = {};   ///< the six readings, Lorenz x, y, z, Roessler x, y, z, in -1..1
    float             chaosPeriod_ = 120.0f;   ///< Chaos Period: about the time between lobe changes and about one turn of the spiral, seconds
    int               chaosSteps_ = 0;     ///< blocks the attractors have been stepped
    bool              chaosUsed_ = false;  ///< a route reads one of the six sources: the gate
    /**
     * @brief Steps both systems by fourth-order Runge-Kutta in their own time, substepped to at most a
     *        hundredth, reseeds a state that left the finite world, and scales the readings.
     * @param dt  seconds in the block
     */
    void stepChaos(float dt);
    /**
     * @brief The Beat modulation source: the instrument listening to its own tuning.
     *
     * beatPhase_ is the source's phase in radians.
     */
    float             beatPhase_ = 0.0f, beatHz_ = 0.0f;   ///< beatHz_: the followed beat rate, Hz
    /**
     * @brief Finds the two lowest distinct pitches, the simplest ratio near their interval and the beat
     *        of the harmonics that would coincide if it were just, follows that rate over two seconds,
     *        and turns the phase (held below a fiftieth of a hertz).
     * @param dt  seconds in the block
     * @return    the source's value, the sine of its phase
     */
    float             updateBeat(float dt);
    /** @brief The followed beat rate. @return Hz -- for the card's readout */
    float             beatRateHz() const { return beatHz_; }
    float             farDiffuse_ = 0.0f;                           ///< how hard the air ahead of the far reverb saturates
    /** @var float portamento_
     *  @brief Portamento: seconds a key slides in from the previous key, 0 = off */
    float             portamento_ = 0.0f, portaGravity_ = 0.5f;   ///< portaGravity_: the glide's curve
    double            lastKeyHz_ = 0.0;   ///< frequency of the last key pressed, for portamento
    float             fbTape_ = 0.0f;     ///< Feedback Tape: wow, flutter, asymmetry and hiss in the loop
    /**
     * @var float fbBias_
     * @brief Bias: the feedback shaper's operating point follows the level of the bass going into it.
     *
     * A static curve makes the same harmonics whatever came before; a transformer or a
     * capacitor-coupled tube stage does not -- low-frequency energy charges the coupling and
     * shifts where the curve is being used, so a bass swell changes how the highs distort (the
     * reactive nonlinearities of the wave-digital literature, Chowdhury among others). This is
     * the cheapest honest form of that: the loop's content below sixty hertz, rectified and
     * followed at five hertz, pushes the shaper's input off centre as it rises, in the curve's units after Drive, saturating at 1.2 once the bass is a hundredth of full scale.
     * fbBias_ is Feedback Bias as read this block.
     */
    /** @var float fbBiasLpL_
     *  @brief the 60 Hz low-pass of the loop, left */
    /** @var float fbBiasLpR_
     *  @brief the same, right */
    /** @var float fbBiasEnvL_
     *  @brief the rectified bass followed at 5 Hz, left */
    float             fbBias_ = 0.0f, fbBiasLpL_ = 0.0f, fbBiasLpR_ = 0.0f, fbBiasEnvL_ = 0.0f, fbBiasEnvR_ = 0.0f;   ///< fbBiasEnvR_: the same, right
    Drifter           tapeWow_;   ///< the tape wow's slow irregular drift
    double            tapeFlutterPhase_ = 0.0;   ///< the 6 Hz flutter's phase, 0..1
    double            purityCur_ = 1.0;   ///< Purity plus its drift, evaluated per block
    Drifter           purityDrift_;       ///< Purity Drift's drifter, advanced in process()
    bool              retune_ = false;    ///< purity below 1 or drifting: sounding voices follow
    /**
     * @brief Adaptive: a note that starts is tuned pure against the notes already sounding, not against
     *        the root, and the offset it was given is kept for as long as it sounds.
     *
     * Those offsets
     * accumulate -- a progression through pure fifths and thirds walks the pitch away by a
     * syntonic comma (81/80, 21.5 cents) per cycle -- so a common offset shared by every voice
     * pays the drift back at three cents a minute, which is far below the ear's threshold for a
     * pitch change and leaves every interval exactly as pure as it was, because all the voices
     * move together.
     * adaptAmt_ is Tune Adapt, the amount, 0..1.
     */
    float             adaptAmt_ = 0.0f;
    float             adaptCents_[128] = {};   ///< the offset each note was given when it started
    /** @var double commaCents_
     *  @brief the shared offset, cents, moving three cents a minute */
    double            commaCents_ = 0.0, commaTarget_ = 0.0;   ///< commaTarget_: minus the mean of the sounding notes' offsets, where it is heading
    /**
     * @brief Transpose: a pure interval on everything that sounds, in log2 units, glided at an octave
     *        per two seconds so that a press is a slide and never a jump (SOMA Terra's interval keys).
     *
     * transposeTarget_ is the interval the parameter chose.
     */
    double            transposeTarget_ = 0.0, transposeCur_ = 0.0;   ///< transposeCur_: where the glide stands
    /**
     * @brief Sleep: after two seconds of silence (no voice, output below -90 dBFS) the effects sleep
     *
     * 64-bit: a long is 32 bits here and wrapped after twelve hours
     */
    long long         silentSamples_ = 0;
    bool              asleep_ = false;    ///< the effects are being skipped
    /**
     * @var float blendX_
     * @brief The map cursor the blend target was computed for, so a cursor that stands still is not
     *        searched for and blended again on every block.
     *
     * blendX_ is its x.
     */
    /** @var float blendY_
     *  @brief its y */
    float             blendX_ = 0.0f, blendY_ = 0.0f, blendR_ = 0.0f;   ///< blendR_: its radius
    bool              blendHave_ = false;   ///< a target has been computed at all
    /** @brief The matched partial ratios and what they were computed for (see readParams). */
    float             matchRatio_[kMaxPartials] = {};
    /** @var float matchLast_
     *  @brief the Match amount they were computed for */
    float             matchLast_ = -1.0f, matchB_ = -1.0f;   ///< matchB_: the inharmonicity they were computed for
    /** @var int matchScale_
     *  @brief the scale index they were computed for */
    int               matchScale_ = -1, matchRoot_ = -1;   ///< matchRoot_: the root they were computed for
    bool              matchHave_ = false;   ///< they have been computed at all

    /** @var std::vector<float> nearL_
     *  @brief the near bus (dry plane), left */
    /** @var std::vector<float> nearR_
     *  @brief the near bus, right */
    /** @var std::vector<float> farL_
     *  @brief the far bus (reverb send), left */
    /** @var std::vector<float> farR_
     *  @brief the far bus, right */
    /** @var std::vector<float> wetL_
     *  @brief the delays' output and general scratch, left */
    std::vector<float> nearL_, nearR_, farL_, farR_, wetL_, wetR_;   ///< wetR_: the same, right
    /** @var std::vector<float> dryL_
     *  @brief the near layer's Dry share, left */
    std::vector<float> dryL_, dryR_;   ///< the near layer's Dry share: past every reverb and delay, straight to the output (dryR_: right)
    /**
     * @var std::vector<float> foreD2L_
     * @brief The foreground's two sends, gathered per event and added where the effect takes its input:
     *        into the second delay's, and into the Cosmos'.
     *
     * Empty buffers cost nothing when both are 0.
     * foreD2L_ is the send into the second delay, left.
     */
    /** @var std::vector<float> foreD2R_
     *  @brief the send into the second delay, right */
    /** @var std::vector<float> foreCoL_
     *  @brief the send into the Cosmos, left */
    std::vector<float> foreD2L_, foreD2R_, foreCoL_, foreCoR_;   ///< foreCoR_: the send into the Cosmos, right
    float* const* stems_ = nullptr;   ///< eight pointers or null; valid for one process() call
    int    stemPos_ = 0;              ///< where in them this chunk starts
    double   sr_ = 48000.0;   ///< the sample rate
    int      maxBlock_ = 512; ///< the chunk size renderChunk() is given at most; the buffers' length
    uint64_t order_ = 0;      ///< a running count of note starts, so allocate() can find the oldest voice
    Rng      rng_;            ///< the engine's main stream, seeded from Seed: voices, conductor, depth draws
    int      seed_ = -1;      ///< the Seed parameter as last read; a change reseeds
    int      lastRootPc_ = -1;   ///< the Root Note pitch class last handed to the conductor
    bool     midiHeld_[128] = {};   ///< which keys are down (or latched under Hold)
    Smoother masterSmooth_;   ///< per-sample smoothing of the master gain
    /** @var float dcXL_
     *  @brief output DC blocker, previous input, left */
    /** @var float dcXR_
     *  @brief previous input, right */
    /** @var float dcYL_
     *  @brief previous output, left */
    float    dcXL_ = 0.0f, dcXR_ = 0.0f, dcYL_ = 0.0f, dcYR_ = 0.0f;   ///< output DC blocker (dcYR_: previous output, right)
    /** @var float pdcXL_
     *  @brief the DC blocker ahead of the Patina, previous input, left */
    /** @var float pdcXR_
     *  @brief previous input, right */
    /** @var float pdcYL_
     *  @brief previous output, left */
    float    pdcXL_ = 0.0f, pdcXR_ = 0.0f, pdcYL_ = 0.0f, pdcYR_ = 0.0f;   ///< the same ahead of the Patina's clipper (pdcYR_: previous output, right)
    /**
     * @var Smoother smDelayMix_
     * @brief Per-sample smoothing of the level-type parameters in the effect chain (20 ms), so
     *        automation, gestures, morph and map blend never step a gain by a whole block.
     *
     * smDelayMix_ smooths delayMix_.
     */
    /** @var Smoother smDelayToFar_
     *  @brief smooths delayToFar_ */
    /** @var Smoother smDelay2Mix_
     *  @brief smooths delay2Mix_ */
    /** @var Smoother smDelay2ToFar_
     *  @brief smooths delay2ToFar_ */
    /** @var Smoother smCloudSend_
     *  @brief smooths cloudSend_ */
    /** @var Smoother smCosmosSend_
     *  @brief smooths the swelled Cosmos send */
    /** @var Smoother smCosmosReturn_
     *  @brief smooths cosmosReturn_ */
    /** @var Smoother smCosmosToFar_
     *  @brief smooths cosmosToFar_ */
    /** @var Smoother smFarLevel_
     *  @brief smooths farLevel_ */
    Smoother smDelayMix_, smDelayToFar_, smDelay2Mix_, smDelay2ToFar_, smCloudSend_, smCosmosSend_, smCosmosReturn_, smCosmosToFar_, smFarLevel_, smFarWidth_;   ///< smFarWidth_: smooths farWidth_

    std::atomic<uint64_t> mask_[2]{ 0, 0 };   ///< which notes sounded at the end of the last block, a bit each: 0 .. 63 and 64 .. 127
    /** @brief The active voice with the highest envelope level, for the displays. @return the voice, or null when nothing sounds */
    const Voice* loudestVoice() const;
    std::atomic<int> activeVoices_{ 0 };   ///< voices active at the end of the last block
    std::atomic<int> brainRoot_{ 50 };     ///< the first conductor's root at the end of the last block
    std::atomic<float> arcValue_{ 0.0f };  ///< the arc times its Amount, for arcValue()
    std::atomic<float> noteDistance_[128]; ///< per note, the distance of the voice sounding it, -1 for none
    std::atomic<float> noteLevel_[128];    ///< per note, the loudest sounding voice's envelope level, 0 for none
};

} // namespace ambient
