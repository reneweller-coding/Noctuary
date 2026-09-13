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

class NoctuaryProcessor : public juce::AudioProcessor, private ambient::OscSink, private juce::Timer
{
public:
    NoctuaryProcessor();
    ~NoctuaryProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 120.0; }

    // Programs == built-in presets from the core.
    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram_; }
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Message thread. Returns false if the file is not a valid Scala scale.
    bool loadScalaText(const juce::String& text, const juce::String& displayName);
    // Source-slot data: a texture sample (any format JUCE reads; assumed recorded at C4 for
    // Pitch = Note) and a user wavetable (2048-sample frames). Paths are kept in the state.
    // One clip per source slot; the slotless form loads the file into all four, which is what a
    // preset that names a single file means and what the instrument always did.
    bool loadTextureFile(int slot, const juce::File& file);
    bool loadTextureFile(const juce::File& file);
    bool loadWavetableFile(const juce::File& file);
    bool loadImpulseFile(const juce::File& file, bool second = false);   // convolution room, mono or stereo
    void clearImpulseB();   // Room Morph's second room gone; the room is A alone
    juce::String impulseName() const { return impulseFile_.existsAsFile() ? impulseFile_.getFileNameWithoutExtension() : juce::String(); }
    juce::String impulseBName() const { return impulseBFile_.existsAsFile() ? impulseBFile_.getFileNameWithoutExtension() : juce::String(); }
    // Level matching: while it is on, loading a preset trims the master gain by the difference
    // between the loudness that was measured for it and a common target, so auditioning a
    // hundred presets is not a ride on the volume knob. It never touches a preset's own settings.
    void setLevelMatch(bool on) { levelMatch_ = on; }
    bool levelMatch() const { return levelMatch_; }
    void setCompactLayout(bool on) { compact_ = on; }
    bool compactLayout() const { return compact_; }
    // 0 normal, 1 compact (wide rows wrap), 2 expanded (every page of every tab row laid out
    // under one another -- the tabs gone, the page tall). Kept in the state like Compact.
    void setLayoutMode(int m) { layoutMode_ = m; compact_ = m == 1; }
    int  layoutMode() const { return layoutMode_; }
    juce::String textureName(int slot = 0) const
    {
        const int k = juce::jlimit(0, ambient::kSlots - 1, slot);
        return textureFile_[k].existsAsFile() ? textureFile_[k].getFileNameWithoutExtension() : juce::String();
    }
    juce::String wavetableName() const { return wavetableFile_.existsAsFile() ? wavetableFile_.getFileNameWithoutExtension() : juce::String(); }
    // User presets as files (full state including a loaded Scala scale).
    bool savePresetFile(const juce::File& file);
    bool loadPresetFile(const juce::File& file);

    // Independent layers: the sound chain (everything but Cosmos) and the Cosmos chain.
    void applySoundPreset(int index);
    void applyCosmosPreset(int index);
    // The two newer layers. Like Cosmos: each resets its own section and touches nothing else.
    void applyZPreset(int index);
    void applyStrikePreset(int index);
    // The near layer (13.09.2026): like the Cosmos, kept across sound presets. A near preset may
    // name a clip of the library's archive for its source; the player may open one by hand.
    void applyNearPreset(int index);
    // Auto: the foreground a pack preset brings (the artist's table); applied with the sound
    // preset while fore_auto is on, and again when it is switched on for the preset playing.
    void applyNearAuto(int soundIndex);
    void nearAutoNow() { applyNearAuto(soundIndex_); }
    void setNearAuto(bool on) { setParam(ambient::ParamId::ForeAuto, on ? 1.0f : 0.0f); }
    bool loadNearClipFile(const juce::File& file);
    bool loadNearClipFolder(const juce::File& dir);   // a pool: every event plays one of its recordings, at random
    void clearNearClip();
    juce::String nearClipName() const
    {
        if (nearClipFile_.existsAsFile()) return nearClipFile_.getFileNameWithoutExtension();
        if (nearClipFile_.isDirectory()) return nearClipFile_.getFileName() + " (" + juce::String(nearClipCount_) + ")";
        return {};
    }
    static constexpr int kNearPoolMax = 48;
    int  zPresetIndex() const { return zIndex_; }
    int  strikePresetIndex() const { return strikeIndex_; }
    int  nearPresetIndex() const { return nearIndex_; }
    // Session recall (standalone). JUCE writes the whole state into its settings file when the
    // window is closed and reads it back on the next start -- but only then, so a crash, a kill
    // or a power cut loses the evening. The timer here writes it whenever it has actually
    // changed, and the switch turns the whole thing off and forgets what was stored.
    void setSessionRecall(bool on);
    bool sessionRecall() const { return sessionRecall_; }
    static bool sessionRecallAvailable();      // false in a plugin: there the host owns the state
    void saveSession();

    int  soundPresetIndex() const  { return soundIndex_; }
    int  cosmosPresetIndex() const { return cosmosIndex_; }

    // Morph slots A (0) and B (1).
    // Choosing a preset while another one is playing: instead of the cut, the instrument travels.
    // The state now becomes slot A, the chosen preset slot B, and the morph position glides from
    // 0 to 1 over `seconds`; when it arrives, the preset's own values are written into the
    // parameters and the morph switches itself off, so what is left is the preset and not a
    // blend. Off, or with the browser's switch off, a preset still lands the moment it is chosen.
    void selectPreset(int index, bool viaMorph);
    bool morphOnSelect() const { return morphOnSelect_; }
    void setMorphOnSelect(bool on) { morphOnSelect_ = on; }
    float morphSelectSeconds() const { return morphSelectSeconds_.load(std::memory_order_relaxed); }
    void setMorphSelectSeconds(float s) { morphSelectSeconds_ = juce::jlimit(0.5f, 600.0f, s); }

    // Journeys (13.09.2026): presets in a row, each held for a while drawn from its range, each
    // crossfaded into the next over a drawn fade, round and round when cyclic -- an evening that
    // plays itself. A journey names presets, not indices, and skips a name the library no longer
    // has. The player advances on the preset pump; the editor asks for the status.
    bool startJourney(const juce::File& file);        // false if the file does not parse
    bool startJourney(const ambient::Journey& j);
    void stopJourney();
    bool journeyRunning() const { return journeyPlayer_.running(); }
    juce::String journeyStatus() const;               // "<name>  3/12  4:12" while one runs, else empty
    const ambient::Journey& journey() const { return journey_; }
    void journeyAddCurrent(double dwellLo, double dwellHi, double fadeLo, double fadeHi);   // the sound preset playing, appended
    void journeyClear() { journey_ = ambient::Journey(); }
    bool saveJourney(const juce::File& file);
    static juce::File userJourneyFolder();            // Documents/Noctuary/Journeys, made if need be
    static juce::Array<juce::File> journeyFiles();    // the templates beside the library and the user's own
    void journeyTick();                               // message thread, from the pump
    // Which preset the instrument is travelling towards, -1 when it is not, and how far it has
    // come (0..1) -- the browser draws both.
    // What the map draws as the travelling line: where the sound is coming from, where it is
    // going, and how far it has come. A change counts as travelling from the moment it is asked
    // for -- the incoming engine may be published a block before the audio thread takes it up,
    // and a line that appeared one frame late would look like a dropped click.
    bool  transitionInFlight() const
    { return swapTo_.load(std::memory_order_acquire) >= 0 || fading_.load(std::memory_order_acquire) >= 0; }
    int   morphingTo() const { return transitionInFlight() ? soundIndex_ : -1; }
    int   morphingFrom() const { return transitionInFlight() ? fadingFrom_ : -1; }
    float morphProgress() const { return transitionInFlight() ? fadePos_.load() : 1.0f; }
    void setMorphSlotFromPreset(int slot, int presetIndex);
    void setMorphSlotFromCurrent(int slot);
    // An empty name is a slot nobody chose, which plays as the knobs stand (Engine::morphSlotSet).
    // It used to read "Init", and that was true: the slot held the defaults, which is the fault.
    juce::String morphSlotName(int slot) const { return slotName_[slot & 1].isEmpty() ? juce::String("as played") : slotName_[slot & 1]; }

    // OSC input and gesture layer (see ambient/Osc.h for the namespace).
    ambient::GestureLayer& gestures() { return gestures_; }
    bool oscRunning() const { return osc_.running(); }
    int  oscPort() const { return osc_.port(); }
    juce::String oscError() const { return osc_.lastError(); }
    juce::uint64 oscMessages() const { return osc_.messagesReceived(); }
    bool setGestureMappings(const juce::String& text);
    juce::String gestureMappings() const;

    // Recording the output to a 32-bit float WAV (message thread to start/stop).
    bool startRecording(const juce::File& file);
    void stopRecording();
    bool isRecording() const { return recording_.load(); }
    double recordedSeconds() const { return recordedSamples_.load() / juce::jmax(1.0, getSampleRate()); }

    // MIDI learn: arm a parameter, the next controller message binds to it.
    void armMidiLearn(ambient::ParamId id) { learnTarget_.store(static_cast<int>(id)); }
    void clearMidiLearn(ambient::ParamId id);
    int  midiCcFor(ambient::ParamId id) const;     // -1 if unmapped
    int  learnTarget() const { return learnTarget_.load(); }
    juce::String userScaleName() const { return userScaleName_; }

    // Set timeline: record every parameter change and note with its time, play a set back.
    void startSetRecording();
    bool stopSetRecording(const juce::File& saveTo);   // false if the file could not be written
    bool isRecordingSet() const { return setRecording_.load(); }
    bool playSetFile(const juce::File& file);
    void stopSetPlayback() { setPlaying_.store(false); }
    bool isPlayingSet() const { return setPlaying_.load(); }
    double setTime() const { return setTime_.load(); }
    // Route over the map (text form, see ambient/Route.h), kept in the plugin state.
    bool setRouteText(const juce::String& text) { if (!live().setRouteText(text.toRawUTF8())) return false; routeText_ = text; return true; }
    juce::String routeText() const { return routeText_; }
    void clearRoute() { live().clearRoute(); routeText_.clear(); }
    bool addRoutePoint(const ambient::Waypoint& w) { if (!live().addRoutePoint(w)) return false; char buf[4096]; live().writeRoute(buf, sizeof(buf)); routeText_ = buf; return true; }
    // Favourite presets (the browser's stars). By name, in a file of the player's own
    // (Documents\Noctuary\favourites.txt, one name a line), so they survive a library that is
    // generated again under the same names and are the same in the standalone and in every DAW.
    // The plugin state used to carry them by index, and the 2.0 library renumbered every index.
    // juce::BigInteger grows on demand, so the library's size is not a limit here.
    bool isFavourite(int preset) const { return preset >= 0 && favourites_[preset]; }
    void setFavourite(int preset, bool on);
    int  favouriteCount() const { return favourites_.countNumberOfSetBits(); }
    // The browser's "favourites first": the starred presets at the top of the list, whatever the
    // sort. Remembered in the same file.
    bool favouritesFirst() const { return favouritesFirst_; }
    void setFavouritesFirst(bool on) { favouritesFirst_ = on; saveFavourites(); }

    // Bumped whenever any parameter changes, from wherever. A display that draws nothing but
    // parameters -- the filter response, an envelope, the vector square -- has no business
    // repainting fifteen times a second while the panel stands still, and asks this instead.
    uint32_t paramGeneration() const { return paramGen_.load(std::memory_order_relaxed); }

    juce::AudioProcessorValueTreeState apvts;
    // The instrument that is sounding. There are two of them (see selectPreset): a preset change
    // that is meant to be heard as a transition lets the old one keep playing while the new one
    // fades in, and the two swap roles when it has arrived. Everything that means "the synth"
    // -- parameters, notes, the editor's displays -- means the live one.
    ambient::Engine& engine() { return live(); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    // One listener on every parameter; counting is all it does, so it costs nothing on the audio
    // thread when a host automates something.
    struct ParamWatch : juce::AudioProcessorParameter::Listener {
        explicit ParamWatch(std::atomic<uint32_t>& g) : gen(g) {}
        void parameterValueChanged(int, float) override { gen.fetch_add(1, std::memory_order_relaxed); }
        void parameterGestureChanged(int, bool) override {}
        std::atomic<uint32_t>& gen;
    };
    std::atomic<uint32_t> paramGen_ { 0 };
    ParamWatch paramWatch_ { paramGen_ };

    // Two engines, but only the one that is playing exists most of the time. A prepared engine is
    // about 110 MB -- delay lines, two convolution rooms, the cloud, the reverbs -- and keeping a
    // second one standing doubled what an instance costs for the seconds a transition lasts. The
    // incoming one is built when a change is asked for (message thread, where allocating is
    // allowed) and released once the crossfade has ended and nothing else is waiting.
    std::unique_ptr<ambient::Engine> engines_[2];
    double lastSampleRate_ = 48000.0;
    int    lastBlockSize_ = 512;
    ambient::Engine& engineAt(int i) { return *engines_[i]; }
    ambient::Engine& ensureEngine(int i);      // message thread: build and prepare it if it is gone
    void carryUserData(ambient::Engine& e);   // the scale, wavetable, clips and room the player loaded
    void releaseIdleEngine();                  // message thread: give back the one nothing is using
    // Which of the two is the instrument right now. Read by both threads and written only by the
    // audio thread, at a block boundary (see the swap in processBlock): the message thread
    // prepares the other engine in full and then asks for the change, so no engine is ever
    // written by one thread while the other renders it.
    std::atomic<int> live_ { 0 };
    ambient::Engine& live()  { return *engines_[live_.load(std::memory_order_relaxed)]; }
    // Where the message thread's engine writes go while a transition is being prepared: the
    // incoming engine, which nothing renders yet. Null the rest of the time, when they mean the
    // instrument itself. Message thread only.
    ambient::Engine* prepareTarget_ = nullptr;
    ambient::Engine& target() { return prepareTarget_ != nullptr ? *prepareTarget_ : live(); }
    // The engine the audio thread pushes the parameter tree into, or -1 for "whichever is live".
    // While a transition is being prepared it is the incoming one, so that the preset now being
    // written into the tree does not also land on the engine that is still playing the old sound.
    std::atomic<int>  paramTarget_ { -1 };
    // Published when the incoming engine is ready; the audio thread makes it live and starts the
    // crossfade at the next block. -1 = nothing waiting.
    std::atomic<int>  swapTo_ { -1 };
    // Asks the audio thread to let go of the engine that is fading out, so the message thread may
    // prepare it for the next change. A transition interrupted this way ends where it stands.
    std::atomic<bool> endFade_ { false };
    // The map was switched off: the message thread writes the blend it left into the parameters.
    std::atomic<bool> mapExit_ { false };
    // Until when an exit is one a journey caused, whose blend must not be written over the journey's
    // first preset (hi-res milliseconds; message thread only). See startJourney.
    double mapExitDiscardUntil_ = 0.0;
    int routeMirrorLeft_ = 0;   // samples until the route's cursor is told to the host again
    int  pendingPreset_ = -1;         // a change waiting for the audio thread to free an engine
    void beginTransition(int index);  // message thread: prepare the incoming engine and publish it
    // A change asked for while both engines were busy is served from here, a few milliseconds
    // later. A timer rather than a wait: the message thread must not block on the audio thread,
    // and with no audio device running it would never be let go.
    // It also carries out the preset changes OSC asks for: those write the whole parameter tree
    // and read files, which is not work for the audio thread.
    struct PresetPump : juce::Timer {
        explicit PresetPump(NoctuaryProcessor& p) : proc(p) {}
        void timerCallback() override { proc.servePresetRequests(); proc.servePendingPreset(); proc.journeyTick(); }
        NoctuaryProcessor& proc;
    };
    PresetPump presetPump_ { *this };
    ambient::Journey       journey_;
    ambient::JourneyPlayer journeyPlayer_;
    double                 journeyLastTick_ = 0.0;   // seconds (the hi-res counter) at the last tick
    ambient::EventQueue presetEvents_;   // OSC preset changes, waiting for the message thread
public:
    void servePendingPreset();        // message thread; does nothing until an engine is free
    void servePresetRequests();       // message thread; the preset changes OSC asked for
private:
    // A transition in flight: the engine on its way out, and how far the crossfade has come.
    // -1 when nothing is fading. Equal-power, so the sum never dips in the middle. Written on the
    // audio thread and read by the map, which draws the crossing, so both are atomic.
    std::atomic<int>   fading_  { -1 };
    std::atomic<float> fadePos_ { 0.0f };
    // Which preset the leaving engine is playing, so the map can draw the line from there. The
    // program number is the arriving one from the moment the change is made -- the name, the
    // parameters and the ring all move at once -- so the departure has to be remembered here.
    int   fadingFrom_ = -1;
    // Before the ramp starts, the incoming engine is given time to speak: a brain preset's first
    // note comes when the brain decides to play it, a sample preset's clip may still be loading,
    // a drone's attack may be ten seconds long. Until the incoming engine is audible (or
    // kFadeHeadStart seconds have passed) the ramp stands at zero and the outgoing one plays on
    // at full level -- otherwise the old sound fades into silence and the new one arrives into it.
    float fadeHead_ = 0.0f;
    static constexpr float kFadeHeadStart = 8.0f;
    // The volume across a fade (audio thread only): the leaving engine's own level at the change,
    // and the parameter at the change, so the player's turn reaches it as a difference.
    float fadeGainFrom_ = 0.0f, fadeGainBase_ = 0.0f;
public:
    // The engine on its way out while a transition fades, else null. For the checks; the audio
    // thread owns it for exactly as long as this returns it.
    const ambient::Engine* leavingEngine() const
    { const int f = fading_.load(std::memory_order_acquire); return f >= 0 ? engines_[f].get() : nullptr; }
private:
    // The notes held right now, from MIDI, OSC and the set timeline alike. They are handed to the
    // incoming engine of a transition: a chord held through a preset change stays held.
    std::array<std::atomic<float>, 128> heldVel_{};   // audio writes, beginTransition reads
    void noteOn(int note, float vel);
    void noteOff(int note);
    void allNotesOff();
    double sampleRate_ = 48000.0;   // from prepareToPlay; the fade cannot trust getSampleRate() before the host sets it
    juce::AudioBuffer<float> fadeBuf_;
    // Output muted at the device: set by AMBIENT_MUTE=1, and implied by AMBIENT_MANUAL (the
    // export plays a chord for its pictures; nobody asked to hear it). Read once, at start.
    const bool muteOutput_ = juce::SystemStats::getEnvironmentVariable("AMBIENT_MUTE", "").isNotEmpty()
                          || juce::SystemStats::getEnvironmentVariable("AMBIENT_MANUAL", "").isNotEmpty()
                          || juce::SystemStats::getEnvironmentVariable("AMBIENT_SHOT", "").isNotEmpty();
    std::array<std::atomic<float>*, ambient::kNumParams> raw_{};
    juce::AudioBuffer<float> scratch_;
    juce::String scalaText_, userScaleName_;
    juce::File textureFile_[ambient::kSlots], wavetableFile_, impulseFile_, impulseBFile_;
    bool       levelMatch_ = false, compact_ = false;
    // What the browser last set for a travelling preset change.
    bool       morphOnSelect_ = true;
    std::atomic<float> morphSelectSeconds_ { 20.0f };   // editor writes, audio reads
    int        layoutMode_ = 0;
    void       applyLevelMatch(int presetIndex);
    juce::BigInteger favourites_;
    juce::StringArray favouriteNames_;   // the file's names, in its order -- kept even for presets that are not installed
    bool favouritesFirst_ = false;
    static juce::File favouritesFile();
    void loadFavourites();
    void saveFavourites() const;
    juce::String routeText_;
    // set timeline (recording appends on the audio thread; save/load on the message thread while stopped)
    ambient::SetTimeline setRec_, setPlay_;
    std::atomic<bool> setRecording_{ false }, setPlaying_{ false };
    // Set while the audio thread is writing into the recording, so stopping can wait for it.
    std::atomic<bool> setRecBusy_{ false };
    std::atomic<bool> setPlayBusy_{ false };   // and while it is stepping through a set being played
    std::atomic<double> setTime_{ 0.0 };
    float setLast_[ambient::kNumParams] = {};
    double setClock_ = 0.0;
    bool readMono(const juce::File& file, std::vector<float>& mono, double& sampleRate);
    bool readStereo(const juce::File& file, std::vector<float>& left, std::vector<float>& right, double& sampleRate);
    int currentProgram_ = 0;
    // A program change reads files off the disk; these two hold a sweep back until it stops.
    std::atomic<int> pendingFiles_ { -1 };
    double lastProgramAt_ = -1.0e9;
    int soundIndex_ = 0, cosmosIndex_ = 0, zIndex_ = 0, strikeIndex_ = 0, nearIndex_ = 0;
    juce::File nearClipFile_;        // a file, or the folder of a pool
    int        nearClipCount_ = 0;
    // The names, not the indices: a pack added or removed between two sessions renumbers every
    // preset behind it, and an index would then name a different sound.
    juce::String soundName_, cosmosName_, nearName_;
    static juce::PropertySet* standaloneSettings();
    void timerCallback() override;             // session recall: save when the state has changed
    bool sessionRecall_ = true;
    juce::uint32 savedStateHash_ = 0;
    void applyScoped(const ambient::Preset& p, ambient::PresetScope scope);
    void loadPresetFiles(int index);   // a pack preset's own sample and wavetable
    std::array<std::atomic<int>, 128> ccMap_{};   // controller -> parameter index, -1 = none
    std::atomic<int> learnTarget_{ -1 };
    double clockSamples_ = 0.0, lastClockSample_ = -1.0;   // MIDI clock: running sample count, for the tick intervals
    // MPE: which note each channel is currently playing, so its bend, pressure and slide reach
    // the right voice. Channel 1 (index 0) is the master channel and holds no note.
    int   mpeNote_[16] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    juce::String slotName_[2] = { "", "" };   // empty = not chosen

    // OscSink
    void setParam(ambient::ParamId id, float value) override;
    void setParamNormalised(ambient::ParamId id, float norm) override;
    void event(const ambient::ControlEvent& e) override;
    void setHeadYaw(float degrees) override { live().setHeadYaw(degrees); }
    ambient::GestureLayer gestures_;
    ambient::OscServer    osc_;
    ambient::EventQueue   events_;
    juce::String          mappingText_;

    // Recording
    juce::TimeSliceThread recordThread_{ "Noctuary recorder" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> recordWriter_;
    juce::CriticalSection recordLock_;
    std::atomic<bool> recording_{ false };
    std::atomic<juce::int64> recordedSamples_{ 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoctuaryProcessor)
};
