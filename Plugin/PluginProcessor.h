/**
 * @file PluginProcessor.h
 * @brief The Noctuary plugin: the core's ambient::Engine under a JUCE processor.
 *
 * **What is here.** NoctuaryProcessor is the one juce::AudioProcessor of the instrument; the
 * standalone application and every plugin format are built from it. It owns the engines
 * (ambient::Engine, which is the whole synth), the host's parameter tree (`apvts`, one parameter per
 * entry of ambient::paramTable()), the preset layers and their files, the OSC receiver with its
 * gesture layer, the WAV recorder, the set timeline, MIDI learn, the journeys and the favourites.
 * The editor (PluginEditor.h) reads and writes through this class and reaches the engine only
 * through engine().
 *
 * **Threads.** processBlock() runs on the audio thread and does nothing that allocates or waits:
 * it pushes the parameter tree into the engine, drains the OSC note queue, reads the MIDI buffer,
 * renders, crossfades and records. Everything that opens a file or writes hundreds of parameters
 * -- a preset change, a texture, a room, a whole state -- runs on the message thread: from the
 * editor, from the host's program change, or from the preset pump, a 30 Hz timer that carries out
 * what OSC asked for and finishes a transition that had to wait for an engine. The OSC thread only
 * writes parameters through the host and pushes events into two lock-free queues (ambient::OscSink).
 * The processor is itself a juce::Timer, for the standalone's session recall.
 *
 * **Two engines.** A preset change that is meant to be heard as a transition is not a parameter
 * morph but a crossfade between two engines: the preset playing keeps playing, untouched, on the
 * engine it is on, while the new one is built on the other engine (message thread) and the audio
 * thread swaps them at a block boundary. selectPreset(), beginTransition() and processBlock() are
 * the three ends of that handshake, and the atomics `live_`, `swapTo_`, `fading_` and `endFade_`
 * carry it between the threads. A prepared engine is about 110 MB, so the second one exists only
 * while a transition runs.
 *
 * **State.** getStateInformation() writes the parameter tree and everything that is not a
 * parameter: the Scala scale, the gesture mappings, the modulation matrix and the envelope shapes,
 * the route, the paths of the loaded textures, wavetable and rooms, the names of the loaded
 * presets, the layout switches, the MIDI map and the two Morph snapshots. The favourites live
 * outside it, by name, in the player's own file (Documents/Noctuary/favourites.txt).
 *
 * **Environment.** `AMBIENT_MUTE=1`, `AMBIENT_MANUAL` and `AMBIENT_SHOT` silence the device output
 * while the engine, its meters and the recorder keep running (see `muteOutput_`). OSC listens on
 * port 9000.
 */
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "ambient/Engine.h"
#include "ambient/Presets.h"
#include "ambient/Gesture.h"
#include "ambient/Osc.h"
#include "ambient/Timeline.h"
#include "ambient/Journey.h"
#include <array>
#include <atomic>

/**
 * @brief The Noctuary plugin processor: the instrument's engines, presets, state and inputs behind
 *        the JUCE processor interface.
 *
 * Also an ambient::OscSink (OSC writes parameters and pushes notes and preset changes through it)
 * and a juce::Timer (the standalone's session recall). See the file comment for the threads and
 * the two-engine transition.
 */
class NoctuaryProcessor : public juce::AudioProcessor, private ambient::OscSink, private juce::Timer
{
public:
    /**
     * @brief Builds the instrument: the crash log, the first engine, the parameter listeners, the
     *        preset packs and favourites, the map warm-up, the preset pump, OSC on port 9000 and
     *        the session-recall timer.
     */
    NoctuaryProcessor();
    /** @brief Stops OSC and the map warm-up first, then the timers, and writes the session a last time. */
    ~NoctuaryProcessor() override;

    /**
     * @brief Prepares every engine that exists at the host's rate and block size and ends any
     *        transition in flight (both engines have just been cleared, so there is nothing to
     *        fade out of).
     * @param sampleRate       the host's sample rate in Hz (0 or less is taken as 48000)
     * @param samplesPerBlock  the largest block the host will ask for
     */
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    /** @brief Nothing to release: the engines keep their buffers, so stopping and starting the transport costs nothing. */
    void releaseResources() override {}
    /**
     * @brief Stereo or mono output only: the instrument is a synth and takes no audio in.
     * @param layouts  the layout the host proposes
     * @return true for a stereo or mono main output
     */
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    /**
     * @brief The audio thread's block: the engine swap of a transition, the macros and gestures,
     *        the OSC note queue, the parameter tree into the engine, the host's clock, the route
     *        mirror, the set timeline, the MIDI buffer, the render, the crossfade, the mono fold-down,
     *        the recorder and the mute.
     *
     * Never allocates and never waits; see the body for the order and the reasons.
     * @param buffer  the host's output buffer (stereo, or mono, which gets the two channels halved)
     * @param midi    notes, expression, MPE, clock and controllers; cleared on return
     */
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    /** @return a new NoctuaryEditor over this processor; the host owns and deletes it */
    juce::AudioProcessorEditor* createEditor() override;
    /** @return true: the instrument has its panel */
    bool hasEditor() const override { return true; }

    /** @return JucePlugin_Name, "Noctuary" (PRODUCT_NAME in Plugin/CMakeLists.txt) */
    const juce::String getName() const override { return JucePlugin_Name; }
    /** @return true: notes, expression, MPE, MIDI clock and learned controllers all arrive as MIDI (processBlock) */
    bool acceptsMidi() const override { return true; }
    /** @return false: nothing is sent back to the host; the set timeline replays its notes into the engine itself */
    bool producesMidi() const override { return false; }
    /** @return false: a synth with an audio output, not a MIDI processor */
    bool isMidiEffect() const override { return false; }
    /**
     * @return two minutes: the rooms, the delays and the slow envelopes go on long after the last
     *         note, and a host that renders only as long as the claimed tail would cut them off
     */
    double getTailLengthSeconds() const override { return 120.0; }

    /**
     * @name Programs == built-in presets from the core.
     * The host's program list is numPresets() long: the built-in presets followed by every loaded
     * pack, in the order ambient/Presets.h gives them.
     * @{ */
    /** @return numPresets(): the built-ins and every loaded pack */
    int getNumPrograms() override;
    /** @return the index of the preset playing; moved by setCurrentProgram, applySoundPreset and a loaded state */
    int getCurrentProgram() override { return currentProgram_; }
    /**
     * @brief Applies preset @p index in full (sound and Cosmos), brings its near foreground while
     *        Auto is on, and reads its files -- unless the change came within a sweep, in which case
     *        the files wait for the pump (see the body).
     * @param index  program number; out of range does nothing
     */
    void setCurrentProgram(int index) override;
    /**
     * @param index  program number
     * @return the preset's name, or an empty string out of range
     */
    const juce::String getProgramName(int index) override;
    /** @brief Nothing: the presets are read-only data of the core and its packs. */
    void changeProgramName(int, const juce::String&) override {}
    /** @} */

    /**
     * @brief Writes the whole state: the parameter tree plus the scale, the gesture mappings, the
     *        modulation matrix and envelope shapes, the route, the file paths, the loaded preset
     *        names, the layout switches, the MIDI map and the two Morph snapshots (see the body).
     * @param destData  receives the binary form of the state's XML
     */
    void getStateInformation(juce::MemoryBlock& destData) override;
    /**
     * @brief Restores a state written by getStateInformation(), older layouts included, and
     *        reloads the files it names from wherever they are on this machine.
     * @param data         the block a host or a preset file hands back
     * @param sizeInBytes  its length
     */
    void setStateInformation(const void* data, int sizeInBytes) override;

    /**
     * @brief Loads a Scala scale as the instrument's user tuning and selects it.
     *
     * Message thread. Returns false if the file is not a valid Scala scale.
     * @param text         the contents of a Scala (.scl) file
     * @param displayName  what the tuning box shows; empty takes the name from the file
     * @return false if the file is not a valid Scala scale
     */
    bool loadScalaText(const juce::String& text, const juce::String& displayName);
    /**
     * @name Source-slot data
     * Source-slot data: a texture sample (any format JUCE reads; assumed recorded at C4 for
     * Pitch = Note) and a user wavetable (2048-sample frames). Paths are kept in the state.
     * One clip per source slot; the slotless form loads the file into all four, which is what a
     * preset that names a single file means and what the instrument always did.
     * @{ */
    /**
     * @brief Loads a recording into one source slot of the engine being prepared (target()).
     * @param slot  0 .. ambient::kSlots - 1; out of range does nothing
     * @param file  any format JUCE reads; a "_A3" suffix names its pitch, else C4 is assumed
     * @return false if the slot is out of range or the file could not be read
     */
    bool loadTextureFile(int slot, const juce::File& file);
    /**
     * @brief Loads a recording into all four source slots.
     * @param file  any format JUCE reads
     * @return false if the file could not be read
     */
    bool loadTextureFile(const juce::File& file);
    /**
     * @brief Loads a user wavetable: the core's reader first (Serum, Vital, Surge frame lengths),
     *        then JUCE's with the cycle length worked out from the samples.
     * @param file  the wavetable file
     * @return false if neither reader could open it or the engine refused it
     */
    bool loadWavetableFile(const juce::File& file);
    /**
     * @brief convolution room, mono or stereo
     * @param file    the impulse response; as much as the Room keeps is read
     * @param second  true loads room B (Room Morph), false room A
     * @return false if the file could not be read
     */
    bool loadImpulseFile(const juce::File& file, bool second = false);
    void clearImpulseB();   ///< Room Morph's second room gone; the room is A alone
    /** @return the name of room A's file without extension, or empty when the room is generated */
    juce::String impulseName() const { return impulseFile_.existsAsFile() ? impulseFile_.getFileNameWithoutExtension() : juce::String(); }
    /** @return the name of room B's file without extension, or empty when there is none */
    juce::String impulseBName() const { return impulseBFile_.existsAsFile() ? impulseBFile_.getFileNameWithoutExtension() : juce::String(); }
    /** @} */
    /**
     * @brief Level matching: while it is on, loading a preset trims the master gain by the difference
     *        between the loudness that was measured for it and a common target, so auditioning a
     *        hundred presets is not a ride on the volume knob. It never touches a preset's own settings.
     * @param on  the switch; kept in the state
     */
    void setLevelMatch(bool on) { levelMatch_ = on; }
    /** @return whether level matching is on (see setLevelMatch) */
    bool levelMatch() const { return levelMatch_; }
    /**
     * @brief The compact layout switch (layout mode 1); kept in the state.
     * @param on  true wraps the wide rows
     */
    void setCompactLayout(bool on) { compact_ = on; }
    /** @return whether the editor is in the compact layout */
    bool compactLayout() const { return compact_; }
    /**
     * @brief 0 normal, 1 compact (wide rows wrap), 2 expanded (every page of every tab row laid out
     *        under one another -- the tabs gone, the page tall). Kept in the state like Compact.
     * @param m  the mode; 1 also sets the compact switch
     */
    void setLayoutMode(int m) { layoutMode_ = m; compact_ = m == 1; }
    /** @return the layout mode, 0 .. 2 (see setLayoutMode) */
    int  layoutMode() const { return layoutMode_; }
    /**
     * @param slot  source slot, clamped to 0 .. ambient::kSlots - 1
     * @return the name of the recording in that slot without extension, or empty
     */
    juce::String textureName(int slot = 0) const
    {
        const int k = juce::jlimit(0, ambient::kSlots - 1, slot);
        return textureFile_[k].existsAsFile() ? textureFile_[k].getFileNameWithoutExtension() : juce::String();
    }
    /** @return the name of the user wavetable's file without extension, or empty */
    juce::String wavetableName() const { return wavetableFile_.existsAsFile() ? wavetableFile_.getFileNameWithoutExtension() : juce::String(); }
    /**
     * @name User presets as files (full state including a loaded Scala scale).
     * @{ */
    /**
     * @brief Writes the whole state as XML.
     * @param file  the file to write (replaced)
     * @return false if the state could not be written
     */
    bool savePresetFile(const juce::File& file);
    /**
     * @brief Reads a file written by savePresetFile and applies it as a state.
     * @param file  the file to read
     * @return false if it does not parse as a Noctuary state
     */
    bool loadPresetFile(const juce::File& file);
    /** @} */

    /**
     * @brief Independent layers: the sound chain (everything but Cosmos) and the Cosmos chain.
     *
     * Applies the sound part of a preset, moves the program number and name with it, forgets the
     * Z-plane and Strike layer presets, reads the preset's files, matches the level and brings the
     * Auto foreground.
     * @param index  preset index; out of range does nothing
     */
    void applySoundPreset(int index);
    /**
     * @brief Applies the Cosmos part of a Cosmos preset, and nothing else.
     * @param index  index into the Cosmos presets; out of range does nothing
     */
    void applyCosmosPreset(int index);
    /**
     * @brief The two newer layers. Like Cosmos: each resets its own section and touches nothing else.
     * @param index  index into the Z-plane presets; out of range does nothing
     */
    void applyZPreset(int index);
    /**
     * @brief The Strike layer's preset, applied like a Z-plane one.
     * @param index  index into the Strike presets; out of range does nothing
     */
    void applyStrikePreset(int index);
    /**
     * @brief The near layer (13.09.2026): like the Cosmos, kept across sound presets. A near preset may
     *        name a clip of the library's archive for its source; the player may open one by hand.
     * @param index  index into the near presets; out of range does nothing
     */
    void applyNearPreset(int index);
    /**
     * @brief Auto: the foreground a pack preset brings (the artist's table); applied with the sound
     *        preset while fore_auto is on, and again when it is switched on for the preset playing.
     * @param soundIndex  the sound preset whose table entry is looked up
     */
    void applyNearAuto(int soundIndex);
    /** @brief Brings the Auto foreground for the sound preset playing now (the switch was just turned on). */
    void nearAutoNow() { applyNearAuto(soundIndex_); }
    /**
     * @brief Writes the fore_auto switch through the host.
     * @param on  the switch
     */
    void setNearAuto(bool on) { setParam(ambient::ParamId::ForeAuto, on ? 1.0f : 0.0f); }
    /**
     * @brief Loads one recording as the near layer's clip, into the engine being prepared.
     * @param file  any format JUCE reads
     * @return false if the file could not be read
     */
    bool loadNearClipFile(const juce::File& file);
    /**
     * @brief a pool: every event plays one of its recordings, at random
     * @param dir  a folder of recordings; up to kNearPoolMax of them are taken, spread over the folder
     * @return false if not one of them could be read
     */
    bool loadNearClipFolder(const juce::File& dir);
    /** @brief Takes the near layer's clip or pool away. */
    void clearNearClip();
    /** @return the clip's name, or "folder (count)" for a pool, or empty */
    juce::String nearClipName() const
    {
        if (nearClipFile_.existsAsFile()) return nearClipFile_.getFileNameWithoutExtension();
        if (nearClipFile_.isDirectory()) return nearClipFile_.getFileName() + " (" + juce::String(nearClipCount_) + ")";
        return {};
    }
    static constexpr int kNearPoolMax = 48;   ///< recordings a pool holds at most (loadNearClipFolder)
    /** @return the Z-plane layer preset loaded, or -1 when the sound preset brought its own */
    int  zPresetIndex() const { return zIndex_; }
    /** @return the Strike layer preset loaded, or -1 when the sound preset brought its own */
    int  strikePresetIndex() const { return strikeIndex_; }
    /** @return the near preset loaded, or -1 for none */
    int  nearPresetIndex() const { return nearIndex_; }
    /**
     * @name Session recall
     * Session recall (standalone). JUCE writes the whole state into its settings file when the
     * window is closed and reads it back on the next start -- but only then, so a crash, a kill
     * or a power cut loses the evening. The timer here writes it whenever it has actually
     * changed, and the switch turns the whole thing off and forgets what was stored.
     * @{ */
    /**
     * @brief The switch: on starts the timer and writes the session now, off stops it and drops the stored state.
     * @param on  the switch; kept in the standalone's settings file
     */
    void setSessionRecall(bool on);
    /** @return whether the session is being recalled */
    bool sessionRecall() const { return sessionRecall_; }
    /**
     * @brief Whether there is a settings file to recall a session from.
     * @return false in a plugin: there the host owns the state
     */
    static bool sessionRecallAvailable();
    /** @brief Writes the state into the standalone's settings file if it has changed since the last write. */
    void saveSession();
    /** @} */

    /** @return the sound preset loaded, or -1 when a state named one the library no longer has */
    int  soundPresetIndex() const  { return soundIndex_; }
    /** @return the Cosmos preset loaded, or -1 when the program brought its own Cosmos layer */
    int  cosmosPresetIndex() const { return cosmosIndex_; }

    /**
     * @name Morph slots and travelling preset changes
     * Morph slots A (0) and B (1).
     * Choosing a preset while another one is playing: instead of the cut, the instrument travels.
     * The state now becomes slot A, the chosen preset slot B, and the morph position glides from
     * 0 to 1 over `seconds`; when it arrives, the preset's own values are written into the
     * parameters and the morph switches itself off, so what is left is the preset and not a
     * blend. Off, or with the browser's switch off, a preset still lands the moment it is chosen.
     * @{ */
    /**
     * @brief The browser's and the journeys' way to a preset: a cut, or a crossfade of two engines
     *        (see the body and beginTransition).
     * @param index     preset index; out of range does nothing
     * @param viaMorph  true asks for the transition; false, or the browser's switch off, is a cut
     */
    void selectPreset(int index, bool viaMorph);
    /** @return the browser's switch: whether a chosen preset travels */
    bool morphOnSelect() const { return morphOnSelect_; }
    /**
     * @brief The browser's switch: whether a chosen preset travels.
     * @param on  the switch
     */
    void setMorphOnSelect(bool on) { morphOnSelect_ = on; }
    /** @return how long a transition takes, in seconds */
    float morphSelectSeconds() const { return morphSelectSeconds_.load(std::memory_order_relaxed); }
    /**
     * @brief How long a transition takes; a journey sets it per step.
     * @param s  seconds, clamped to 0.5 .. 600
     */
    void setMorphSelectSeconds(float s) { morphSelectSeconds_ = juce::jlimit(0.5f, 600.0f, s); }
    /** @} */

    /**
     * @name Journeys
     * Journeys (13.09.2026): presets in a row, each held for a while drawn from its range, each
     * crossfaded into the next over a drawn fade, round and round when cyclic -- an evening that
     * plays itself. A journey names presets, not indices, and skips a name the library no longer
     * has. The player advances on the preset pump; the editor asks for the status.
     * @{ */
    /**
     * @brief Loads a journey file and starts it.
     * @param file  a .journey file; its name is the journey's when the file names none
     * @return false if the file does not parse
     */
    bool startJourney(const juce::File& file);
    /**
     * @brief Starts a journey: lets Morph, the map and a route go, seeds the player from the clock
     *        and begins the first step now.
     * @param j  the journey; one with no steps does nothing
     * @return false if the journey has no steps
     */
    bool startJourney(const ambient::Journey& j);
    /** @brief Stops the player; the preset playing stays. */
    void stopJourney();
    /** @return whether a journey is running */
    bool journeyRunning() const { return journeyPlayer_.running(); }
    /** @return "\<name\>  3/12  4:12" while one runs, else empty */
    juce::String journeyStatus() const;
    /** @return the journey being edited or played */
    const ambient::Journey& journey() const { return journey_; }
    /**
     * @brief the sound preset playing, appended
     * @param dwellLo  shortest hold in seconds
     * @param dwellHi  longest hold in seconds
     * @param fadeLo   shortest crossfade into the next step, in seconds
     * @param fadeHi   longest crossfade, in seconds
     */
    void journeyAddCurrent(double dwellLo, double dwellHi, double fadeLo, double fadeHi);
    /** @brief Empties the journey being edited. */
    void journeyClear() { journey_ = ambient::Journey(); }
    /**
     * @brief Writes the journey being edited; its name is the file's when it has none.
     * @param file  the .journey file to write; its folder is created
     * @return false if the journey is empty or could not be written
     */
    bool saveJourney(const juce::File& file);
    /** @return Documents/Noctuary/Journeys, made if need be */
    static juce::File userJourneyFolder();
    /** @return the templates beside the library and the user's own */
    static juce::Array<juce::File> journeyFiles();
    /** @brief message thread, from the pump */
    void journeyTick();
    /** @} */
    /**
     * @name The transition in flight
     * Which preset the instrument is travelling towards, -1 when it is not, and how far it has
     * come (0..1) -- the browser draws both.
     * What the map draws as the travelling line: where the sound is coming from, where it is
     * going, and how far it has come. A change counts as travelling from the moment it is asked
     * for -- the incoming engine may be published a block before the audio thread takes it up,
     * and a line that appeared one frame late would look like a dropped click.
     * @{ */
    /** @return whether a change has been published or a crossfade is running */
    bool  transitionInFlight() const
    { return swapTo_.load(std::memory_order_acquire) >= 0 || fading_.load(std::memory_order_acquire) >= 0; }
    /** @return the preset the instrument is travelling towards, -1 when it is not */
    int   morphingTo() const { return transitionInFlight() ? soundIndex_ : -1; }
    /** @return the preset it is coming from, -1 when it is not travelling */
    int   morphingFrom() const { return transitionInFlight() ? fadingFrom_ : -1; }
    /** @return how far the crossfade has come, 0..1; 1 when nothing is in flight */
    float morphProgress() const { return transitionInFlight() ? fadePos_.load() : 1.0f; }
    /** @} */
    /**
     * @brief Fills a Morph slot with a preset's values over the knobs as they stand.
     * @param slot         0 (A) or 1 (B)
     * @param presetIndex  preset index; out of range does nothing
     */
    void setMorphSlotFromPreset(int slot, int presetIndex);
    /**
     * @brief Fills a Morph slot with the knobs as they stand, named "(captured)".
     * @param slot  0 (A) or 1 (B)
     */
    void setMorphSlotFromCurrent(int slot);
    /**
     * @brief The name the Morph box shows for a slot.
     *
     * An empty name is a slot nobody chose, which plays as the knobs stand (Engine::morphSlotSet).
     * It used to read "Init", and that was true: the slot held the defaults, which is the fault.
     * @param slot  0 (A) or 1 (B)
     * @return the preset's name, "(captured)", or "as played" for a slot nobody chose
     */
    juce::String morphSlotName(int slot) const { return slotName_[slot & 1].isEmpty() ? juce::String("as played") : slotName_[slot & 1]; }

    /**
     * @name OSC input and gesture layer (see ambient/Osc.h for the namespace).
     * @{ */
    /** @return the gesture layer: OSC inputs mapped onto parameters */
    ambient::GestureLayer& gestures() { return gestures_; }
    /** @return whether the OSC server is listening */
    bool oscRunning() const { return osc_.running(); }
    /** @return the UDP port it listens on (9000) */
    int  oscPort() const { return osc_.port(); }
    /** @return the server's last error, e.g. the port taken by another instance */
    juce::String oscError() const { return osc_.lastError(); }
    /** @return how many OSC messages have arrived */
    juce::uint64 oscMessages() const { return osc_.messagesReceived(); }
    /**
     * @brief Replaces the gesture mappings from their text form (kept in the state).
     * @param text  the mappings, one per line, as gestureMappings() writes them
     * @return false if the text does not parse; nothing changes then
     */
    bool setGestureMappings(const juce::String& text);
    /** @return the gesture mappings in their text form */
    juce::String gestureMappings() const;
    /** @} */

    /**
     * @name Recording the output to a 32-bit float WAV (message thread to start/stop).
     * @{ */
    /**
     * @brief Starts writing the output; a recording already running is stopped first.
     * @param file  the WAV to write (replaced)
     * @return false if the file or the writer could not be opened
     */
    bool startRecording(const juce::File& file);
    /** @brief Stops the recording and closes the file. */
    void stopRecording();
    /** @return whether the output is being written */
    bool isRecording() const { return recording_.load(); }
    /** @return the length of the recording so far, in seconds */
    double recordedSeconds() const { return recordedSamples_.load() / juce::jmax(1.0, getSampleRate()); }
    /** @} */

    /**
     * @name MIDI learn: arm a parameter, the next controller message binds to it.
     * @{ */
    /**
     * @brief Arms a parameter; the next controller message binds to it.
     * @param id  the parameter
     */
    void armMidiLearn(ambient::ParamId id) { learnTarget_.store(static_cast<int>(id)); }
    /**
     * @brief Unbinds a parameter from its controller and disarms it if it was armed.
     * @param id  the parameter
     */
    void clearMidiLearn(ambient::ParamId id);
    /**
     * @param id  the parameter
     * @return the controller bound to it, -1 if unmapped
     */
    int  midiCcFor(ambient::ParamId id) const;
    /** @return the parameter armed for learning, -1 for none */
    int  learnTarget() const { return learnTarget_.load(); }
    /** @} */
    /** @return the name of the loaded Scala scale, or empty */
    juce::String userScaleName() const { return userScaleName_; }

    /**
     * @name Set timeline: record every parameter change and note with its time, play a set back.
     * @{ */
    /** @brief Starts recording: reserves the room, writes the starting state at t = 0. */
    void startSetRecording();
    /**
     * @brief Stops recording, waits for the audio thread to leave the timeline, and writes it.
     * @param saveTo  the file to write; an empty File only stops
     * @return false if the file could not be written
     */
    bool stopSetRecording(const juce::File& saveTo);
    /** @return whether a set is being recorded */
    bool isRecordingSet() const { return setRecording_.load(); }
    /**
     * @brief Loads a set file and plays it from the start; recording stops.
     * @param file  a file written by stopSetRecording
     * @return false if the file does not load
     */
    bool playSetFile(const juce::File& file);
    /** @brief Stops playback where it is. */
    void stopSetPlayback() { setPlaying_.store(false); }
    /** @return whether a set is being played */
    bool isPlayingSet() const { return setPlaying_.load(); }
    /** @return the position in the set being recorded or played, in seconds */
    double setTime() const { return setTime_.load(); }
    /** @} */
    /**
     * @name Route over the map (text form, see ambient/Route.h), kept in the plugin state.
     * @{ */
    /**
     * @brief Hands a route to the engine and keeps its text for the state.
     * @param text  the route in its text form
     * @return false if the engine does not accept the text; nothing changes then
     */
    bool setRouteText(const juce::String& text) { if (!live().setRouteText(text.toRawUTF8())) return false; routeText_ = text; return true; }
    /** @return the route in its text form, or empty */
    juce::String routeText() const { return routeText_; }
    /** @brief Takes the route away, in the engine and in the state. */
    void clearRoute() { live().clearRoute(); routeText_.clear(); }
    /**
     * @brief Appends a waypoint to the engine's route and reads the route's text back.
     * @param w  the waypoint
     * @return false if the engine refused it (the route is full)
     */
    bool addRoutePoint(const ambient::Waypoint& w) { if (!live().addRoutePoint(w)) return false; char buf[4096]; live().writeRoute(buf, sizeof(buf)); routeText_ = buf; return true; }
    /** @} */
    /**
     * @name Favourite presets
     * Favourite presets (the browser's stars). By name, in a file of the player's own
     * (Documents\\Noctuary\\favourites.txt, one name a line), so they survive a library that is
     * generated again under the same names and are the same in the standalone and in every DAW.
     * The plugin state used to carry them by index, and the 2.0 library renumbered every index.
     * juce::BigInteger grows on demand, so the library's size is not a limit here.
     * @{ */
    /**
     * @param preset  preset index
     * @return whether it is starred
     */
    bool isFavourite(int preset) const { return preset >= 0 && favourites_[preset]; }
    /**
     * @brief Stars or unstars a preset and writes the file.
     * @param preset  preset index; out of range does nothing
     * @param on      starred or not
     */
    void setFavourite(int preset, bool on);
    /** @return how many installed presets are starred */
    int  favouriteCount() const { return favourites_.countNumberOfSetBits(); }
    /**
     * @brief The browser's "favourites first": the starred presets at the top of the list, whatever the
     *        sort. Remembered in the same file.
     * @return the switch
     */
    bool favouritesFirst() const { return favouritesFirst_; }
    /**
     * @brief Sets "favourites first" and writes the file.
     * @param on  the switch
     */
    void setFavouritesFirst(bool on) { favouritesFirst_ = on; saveFavourites(); }
    /** @} */

    /**
     * @brief Bumped whenever any parameter changes, from wherever.
     *
     * A display that draws nothing but
     * parameters -- the filter response, an envelope, the vector square -- has no business
     * repainting fifteen times a second while the panel stands still, and asks this instead.
     * @return a counter that only ever grows; compare with the last value seen
     */
    uint32_t paramGeneration() const { return paramGen_.load(std::memory_order_relaxed); }

    juce::AudioProcessorValueTreeState apvts;   ///< the host's parameters: one per entry of ambient::paramTable(), by key
    /**
     * @brief The instrument that is sounding.
     *
     * There are two of them (see selectPreset): a preset change
     * that is meant to be heard as a transition lets the old one keep playing while the new one
     * fades in, and the two swap roles when it has arrived. Everything that means "the synth"
     * -- parameters, notes, the editor's displays -- means the live one.
     * @return the live engine
     */
    ambient::Engine& engine() { return live(); }

private:
    /**
     * @brief One host parameter per entry of ambient::paramTable(): Float (with a value text that
     *        shows more digits below one), Int, Bool or Choice, each under its key.
     * @return the layout the parameter tree is built from, once, in the constructor
     */
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    /**
     * @brief One listener on every parameter; counting is all it does, so it costs nothing on the audio
     *        thread when a host automates something.
     */
    struct ParamWatch : juce::AudioProcessorParameter::Listener {
        /**
         * @brief Binds the counter.
         * @param g  the counter to bump (the processor's paramGen_)
         */
        explicit ParamWatch(std::atomic<uint32_t>& g) : gen(g) {}
        /** @brief Bumps the counter; the value itself is not looked at. */
        void parameterValueChanged(int, float) override { gen.fetch_add(1, std::memory_order_relaxed); }
        /** @brief Nothing: a gesture's start and end are not a change. */
        void parameterGestureChanged(int, bool) override {}
        std::atomic<uint32_t>& gen;   ///< the counter, read by paramGeneration()
    };
    std::atomic<uint32_t> paramGen_ { 0 };   ///< the generation paramGeneration() reports
    ParamWatch paramWatch_ { paramGen_ };    ///< registered on every parameter in the constructor

    /**
     * @brief Two engines, but only the one that is playing exists most of the time.
     *
     * A prepared engine is
     * about 110 MB -- delay lines, two convolution rooms, the cloud, the reverbs -- and keeping a
     * second one standing doubled what an instance costs for the seconds a transition lasts. The
     * incoming one is built when a change is asked for (message thread, where allocating is
     * allowed) and released once the crossfade has ended and nothing else is waiting.
     */
    std::unique_ptr<ambient::Engine> engines_[2];
    double lastSampleRate_ = 48000.0;   ///< what prepareToPlay last saw; an engine built later is prepared at this rate
    int    lastBlockSize_ = 512;        ///< and this block size
    /**
     * @param i  0 or 1
     * @return the engine at that index, which must exist
     */
    ambient::Engine& engineAt(int i) { return *engines_[i]; }
    /**
     * @brief message thread: build and prepare it if it is gone
     * @param i  0 or 1
     * @return the engine at that index, built and prepared if it was not there
     */
    ambient::Engine& ensureEngine(int i);
    /**
     * @brief the scale, wavetable, clips and room the player loaded
     * @param e  the engine that has just been built, to receive them from the live one
     */
    void carryUserData(ambient::Engine& e);
    /** @brief message thread: give back the one nothing is using */
    void releaseIdleEngine();
    /**
     * @brief Which of the two is the instrument right now.
     *
     * Read by both threads and written only by the
     * audio thread, at a block boundary (see the swap in processBlock): the message thread
     * prepares the other engine in full and then asks for the change, so no engine is ever
     * written by one thread while the other renders it.
     */
    std::atomic<int> live_ { 0 };
    /** @return the live engine */
    ambient::Engine& live()  { return *engines_[live_.load(std::memory_order_relaxed)]; }
    /**
     * @brief Where the message thread's engine writes go while a transition is being prepared: the
     *        incoming engine, which nothing renders yet.
     *
     * Null the rest of the time, when they mean the
     * instrument itself. Message thread only.
     */
    ambient::Engine* prepareTarget_ = nullptr;
    /** @return the engine a file or a matrix should land in: the incoming one while preparing, else the live one */
    ambient::Engine& target() { return prepareTarget_ != nullptr ? *prepareTarget_ : live(); }
    /**
     * @brief The engine the audio thread pushes the parameter tree into, or -1 for "whichever is live".
     *
     * While a transition is being prepared it is the incoming one, so that the preset now being
     * written into the tree does not also land on the engine that is still playing the old sound.
     */
    std::atomic<int>  paramTarget_ { -1 };
    /**
     * @brief Published when the incoming engine is ready; the audio thread makes it live and starts the
     *        crossfade at the next block. -1 = nothing waiting.
     */
    std::atomic<int>  swapTo_ { -1 };
    /**
     * @brief Asks the audio thread to let go of the engine that is fading out, so the message thread may
     *        prepare it for the next change. A transition interrupted this way ends where it stands.
     */
    std::atomic<bool> endFade_ { false };
    /** @brief The map was switched off: the message thread writes the blend it left into the parameters. */
    std::atomic<bool> mapExit_ { false };
    /**
     * @brief Until when an exit is one a journey caused, whose blend must not be written over the journey's
     *        first preset (hi-res milliseconds; message thread only). See startJourney.
     */
    double mapExitDiscardUntil_ = 0.0;
    int routeMirrorLeft_ = 0;   ///< samples until the route's cursor is told to the host again
    int  pendingPreset_ = -1;         ///< a change waiting for the audio thread to free an engine
    /**
     * @brief message thread: prepare the incoming engine and publish it
     * @param index  the preset to build on the other engine
     */
    void beginTransition(int index);
    /**
     * @brief A change asked for while both engines were busy is served from here, a few milliseconds
     *        later.
     *
     * A timer rather than a wait: the message thread must not block on the audio thread,
     * and with no audio device running it would never be let go.
     * It also carries out the preset changes OSC asks for: those write the whole parameter tree
     * and read files, which is not work for the audio thread.
     */
    struct PresetPump : juce::Timer {
        /**
         * @brief Binds the processor.
         * @param p  the processor whose requests the pump serves
         */
        explicit PresetPump(NoctuaryProcessor& p) : proc(p) {}
        /** @brief 30 times a second: the OSC preset requests, a pending transition, the journey's step. */
        void timerCallback() override { proc.servePresetRequests(); proc.servePendingPreset(); proc.journeyTick(); }
        NoctuaryProcessor& proc;   ///< the processor
    };
    PresetPump presetPump_ { *this };   ///< runs for the life of the instrument
    ambient::Journey       journey_;         ///< the journey being edited or played
    ambient::JourneyPlayer journeyPlayer_;   ///< its clock and step counter
    double                 journeyLastTick_ = 0.0;   ///< seconds (the hi-res counter) at the last tick
    ambient::EventQueue presetEvents_;   ///< OSC preset changes, waiting for the message thread
public:
    void servePendingPreset();        ///< message thread; does nothing until an engine is free
    void servePresetRequests();       ///< message thread; the preset changes OSC asked for
private:
    /**
     * @brief A transition in flight: the engine on its way out, and how far the crossfade has come.
     *
     * -1 when nothing is fading. Equal-power, so the sum never dips in the middle. Written on the
     * audio thread and read by the map, which draws the crossing, so both are atomic.
     */
    std::atomic<int>   fading_  { -1 };
    std::atomic<float> fadePos_ { 0.0f };   ///< the crossfade's position, 0 .. 1
    /**
     * @brief Which preset the leaving engine is playing, so the map can draw the line from there.
     *
     * The
     * program number is the arriving one from the moment the change is made -- the name, the
     * parameters and the ring all move at once -- so the departure has to be remembered here.
     */
    int   fadingFrom_ = -1;
    /**
     * @brief Before the ramp starts, the incoming engine is given time to speak: a brain preset's first
     *        note comes when the brain decides to play it, a sample preset's clip may still be loading,
     *        a drone's attack may be ten seconds long.
     *
     * Until the incoming engine is audible (or
     * kFadeHeadStart seconds have passed) the ramp stands at zero and the outgoing one plays on
     * at full level -- otherwise the old sound fades into silence and the new one arrives into it.
     */
    float fadeHead_ = 0.0f;
    static constexpr float kFadeHeadStart = 8.0f;   ///< seconds the ramp waits at most for the incoming engine to be heard
    /**
     * @brief The volume across a fade (audio thread only): the leaving engine's own level at the change,
     *        and the parameter at the change, so the player's turn reaches it as a difference.
     */
    float fadeGainFrom_ = 0.0f, fadeGainBase_ = 0.0f;
    /** @var float NoctuaryProcessor::fadeGainBase_
     *  @brief the master gain parameter as the arriving preset left it; what the player turns is measured from here */
public:
    /**
     * @brief The engine on its way out while a transition fades, else null. For the checks; the audio
     *        thread owns it for exactly as long as this returns it.
     * @return the fading engine, or null
     */
    const ambient::Engine* leavingEngine() const
    { const int f = fading_.load(std::memory_order_acquire); return f >= 0 ? engines_[f].get() : nullptr; }
private:
    /**
     * @brief The notes held right now, from MIDI, OSC and the set timeline alike.
     *
     * They are handed to the
     * incoming engine of a transition: a chord held through a preset change stays held.
     */
    std::array<std::atomic<float>, 128> heldVel_{};   ///< audio writes, beginTransition reads
    /**
     * @brief A note to the live engine, remembered in heldVel_.
     * @param note  MIDI note 0 .. 127; anything else is ignored
     * @param vel   velocity 0 .. 1; remembered as at least 1/127 so a held note is never mistaken for none
     */
    void noteOn(int note, float vel);
    /**
     * @brief A note off to the live engine, forgotten in heldVel_.
     * @param note  MIDI note 0 .. 127; anything else is ignored
     */
    void noteOff(int note);
    /** @brief Every note off, in heldVel_ and in the live engine. */
    void allNotesOff();
    double sampleRate_ = 48000.0;   ///< from prepareToPlay; the fade cannot trust getSampleRate() before the host sets it
    juce::AudioBuffer<float> fadeBuf_;   ///< the leaving engine renders into this while a crossfade runs
    /**
     * @brief Output muted at the device: set by AMBIENT_MUTE=1, and implied by AMBIENT_MANUAL (the
     *        export plays a chord for its pictures; nobody asked to hear it). Read once, at start.
     */
    const bool muteOutput_ = juce::SystemStats::getEnvironmentVariable("AMBIENT_MUTE", "").isNotEmpty()
                          || juce::SystemStats::getEnvironmentVariable("AMBIENT_MANUAL", "").isNotEmpty()
                          || juce::SystemStats::getEnvironmentVariable("AMBIENT_SHOT", "").isNotEmpty();
    std::array<std::atomic<float>*, ambient::kNumParams> raw_{};   ///< the tree's raw values, one per parameter, for the audio thread
    juce::AudioBuffer<float> scratch_;   ///< the live engine renders into this; the host's buffer is filled from it
    /** @brief The loaded Scala file's text, kept for the state and for a new engine (carryUserData). */
    juce::String scalaText_, userScaleName_;   ///< the name the tuning box shows for the loaded scale (in the state)
    /** @brief The recording behind each source slot; the paths travel in the state and the files are read again on load. */
    juce::File textureFile_[ambient::kSlots], wavetableFile_, impulseFile_, impulseBFile_;   ///< room B's file, empty when there is none
    /** @var juce::File NoctuaryProcessor::wavetableFile_
     *  @brief the user wavetable's file, empty when the built-in table plays */
    /** @var juce::File NoctuaryProcessor::impulseFile_
     *  @brief room A's file, empty when the room is generated */
    /** @brief Level matching is on (setLevelMatch); in the state. */
    bool       levelMatch_ = false, compact_ = false;   ///< the compact layout (layout mode 1); in the state
    /** @brief What the browser last set for a travelling preset change. */
    bool       morphOnSelect_ = true;
    std::atomic<float> morphSelectSeconds_ { 20.0f };   ///< editor writes, audio reads
    int        layoutMode_ = 0;   ///< 0 normal, 1 compact, 2 expanded (setLayoutMode)
    /**
     * @brief Trims the master gain after a preset so its measured loudness meets the common target;
     *        does nothing while level matching is off or the preset was never measured.
     */
    void       applyLevelMatch(int presetIndex);
    juce::BigInteger favourites_;   ///< one bit per installed preset: starred
    juce::StringArray favouriteNames_;   ///< the file's names, in its order -- kept even for presets that are not installed
    bool favouritesFirst_ = false;   ///< the browser's "favourites first", remembered in the same file
    /** @return Documents/Noctuary/favourites.txt, its folder made if need be */
    static juce::File favouritesFile();
    /** @brief Reads the file: the "first" switch and the names, looked up once in the preset list. */
    void loadFavourites();
    /** @brief Writes the file: a header line, the "first" switch, one name a line. */
    void saveFavourites() const;
    juce::String routeText_;   ///< the route in its text form, for the state
    /** @brief set timeline (recording appends on the audio thread; save/load on the message thread while stopped) */
    ambient::SetTimeline setRec_, setPlay_;   ///< the set being played
    /** @brief A set is being recorded (the audio thread appends to setRec_). */
    std::atomic<bool> setRecording_{ false }, setPlaying_{ false };   ///< a set is being played (the audio thread steps through setPlay_)
    /** @brief Set while the audio thread is writing into the recording, so stopping can wait for it. */
    std::atomic<bool> setRecBusy_{ false };
    std::atomic<bool> setPlayBusy_{ false };   ///< and while it is stepping through a set being played
    std::atomic<double> setTime_{ 0.0 };   ///< the position in seconds, for the editor
    float setLast_[ambient::kNumParams] = {};   ///< the value last recorded per parameter; a change is what differs
    double setClock_ = 0.0;   ///< the audio thread's clock into the set, in seconds
    /**
     * @brief Reads an audio file folded to one channel, at most two minutes of it.
     * @param file        any format JUCE reads
     * @param mono        receives the samples, all channels averaged
     * @param sampleRate  receives the file's rate
     * @return false if the file could not be read
     */
    bool readMono(const juce::File& file, std::vector<float>& mono, double& sampleRate);
    /**
     * @brief Reads an audio file keeping its image: even channels to the left, odd to the right.
     * @param file        any format JUCE reads
     * @param left        receives the left samples (the only ones for a mono file)
     * @param right       receives the right samples; left empty for a mono file
     * @param sampleRate  receives the file's rate
     * @return false if the file could not be read
     */
    bool readStereo(const juce::File& file, std::vector<float>& left, std::vector<float>& right, double& sampleRate);
    int currentProgram_ = 0;   ///< the host's program number: the preset playing
    /** @brief A program change reads files off the disk; these two hold a sweep back until it stops. */
    std::atomic<int> pendingFiles_ { -1 };   ///< the preset whose files still wait to be read, -1 none
    double lastProgramAt_ = -1.0e9;   ///< hi-res milliseconds of the last program change
    /** @brief The sound preset loaded, -1 when a state named one the library no longer has. */
    int soundIndex_ = 0, cosmosIndex_ = 0, zIndex_ = 0, strikeIndex_ = 0, nearIndex_ = 0;   ///< the near preset loaded, -1 for none
    /** @var int NoctuaryProcessor::cosmosIndex_
     *  @brief the Cosmos preset loaded, -1 when the program brought its own Cosmos layer */
    /** @var int NoctuaryProcessor::zIndex_
     *  @brief the Z-plane layer preset loaded, -1 when the sound preset brought its own */
    /** @var int NoctuaryProcessor::strikeIndex_
     *  @brief the Strike layer preset loaded, -1 when the sound preset brought its own */
    juce::File nearClipFile_;        ///< a file, or the folder of a pool
    int        nearClipCount_ = 0;   ///< recordings in the pool, 1 for a single clip, 0 for none
    /**
     * @brief The names, not the indices: a pack added or removed between two sessions renumbers every
     *        preset behind it, and an index would then name a different sound.
     */
    juce::String soundName_, cosmosName_, nearName_;   ///< the near preset's name, for the state
    /** @var juce::String NoctuaryProcessor::cosmosName_
     *  @brief the Cosmos preset's name, for the state */
    /** @return the standalone's settings, or null in a plugin */
    static juce::PropertySet* standaloneSettings();
    void timerCallback() override;             ///< session recall: save when the state has changed
    bool sessionRecall_ = true;   ///< the session-recall switch
    juce::uint32 savedStateHash_ = 0;   ///< FNV-1a of the state last written, so an unchanged state is not written again
    /**
     * @brief Applies a preset's parameters through the host and, for the layers that have it, hands
     *        its modulation to the engine being prepared.
     * @param p      the preset
     * @param scope  which of its parameters to apply (ambient::PresetScope)
     */
    void applyScoped(const ambient::Preset& p, ambient::PresetScope scope);
    /**
     * @brief a pack preset's own sample and wavetable
     * @param index  the preset whose files are read
     */
    void loadPresetFiles(int index);
    std::array<std::atomic<int>, 128> ccMap_{};   ///< controller -> parameter index, -1 = none
    std::atomic<int> learnTarget_{ -1 };   ///< the parameter armed for MIDI learn, -1 none
    /** @brief Samples rendered so far: the time base the MIDI clock ticks are measured against. */
    double clockSamples_ = 0.0, lastClockSample_ = -1.0;   ///< MIDI clock: running sample count, for the tick intervals
    /**
     * @brief MPE: which note each channel is currently playing, so its bend, pressure and slide reach
     *        the right voice. Channel 1 (index 0) is the master channel and holds no note.
     */
    int   mpeNote_[16] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    juce::String slotName_[2] = { "", "" };   ///< empty = not chosen

    /**
     * @name OscSink
     * The ambient::OscSink overrides, called from the OSC thread, and the OSC machinery behind them.
     * @{ */
    /**
     * @brief Writes a parameter's real value through the host, so automation and the editor see it.
     * @param id     the parameter
     * @param value  its real value (Hz, dB, ...), converted to the host's 0..1
     */
    void setParam(ambient::ParamId id, float value) override;
    /**
     * @brief Writes a parameter through the host from its normalised form.
     * @param id    the parameter
     * @param norm  0..1 along the knob curve, clamped
     */
    void setParamNormalised(ambient::ParamId id, float norm) override;
    /**
     * @brief Notes to the audio thread's queue, preset changes to the message thread's.
     * @param e  the event
     */
    void event(const ambient::ControlEvent& e) override;
    /**
     * @brief The head's yaw for the live engine's binaural mode.
     * @param degrees  the yaw from /ambient/head
     */
    void setHeadYaw(float degrees) override { live().setHeadYaw(degrees); }
    ambient::GestureLayer gestures_;   ///< OSC inputs mapped onto parameters, updated once a block
    ambient::OscServer    osc_;        ///< the UDP receiver on port 9000
    ambient::EventQueue   events_;     ///< notes from OSC, popped on the audio thread
    juce::String          mappingText_;   ///< the gesture mappings as text, for the state
    /** @} */

    /**
     * @name Recording
     * @{ */
    juce::TimeSliceThread recordThread_{ "Noctuary recorder" };   ///< the thread the WAV writer flushes on
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> recordWriter_;   ///< the writer while recording, else null
    juce::CriticalSection recordLock_;   ///< guards recordWriter_; the audio thread only tries it
    std::atomic<bool> recording_{ false };   ///< whether the audio thread writes into the recorder
    std::atomic<juce::int64> recordedSamples_{ 0 };   ///< samples written so far
    /** @} */

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoctuaryProcessor)
};
