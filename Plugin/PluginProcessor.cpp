#include "PluginProcessor.h"
#include "ambient/WavFile.h"
// For the standalone's settings file: the same one JUCE saves the state into when the window is
// closed. Reaching it here is what lets the session be written while the app is still running.
#if JucePlugin_Build_Standalone
 #include <juce_audio_utils/juce_audio_utils.h>
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif
#include "ambient/PresetMeta.h"
#include "ambient/PresetMap.h"
#include "PluginEditor.h"
#include "ambient/Tuning.h"
#include "ambient/Presets.h"
#include <cstdlib>
#include <unordered_map>
#include <cstdio>       // the crash log writes with C file I/O: no allocation in a broken process
#include <exception>    // std::set_terminate
#include <mutex>        // std::call_once

using namespace ambient;

juce::AudioProcessorValueTreeState::ParameterLayout NoctuaryProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (const ParamDesc& d : paramTable()) {
        const juce::ParameterID id(d.key, 1);
        switch (d.kind) {
        case ParamKind::Float: {
            juce::NormalisableRange<float> range(d.min, d.max, 0.0f, d.skew);
            const float span = d.max - d.min;
            const int decimals = span >= 100.0f ? 0 : (span >= 5.0f ? 1 : 2);
            layout.add(std::make_unique<juce::AudioParameterFloat>(
                id, d.name, range, d.def,
                juce::AudioParameterFloatAttributes()
                    .withLabel(d.unit)
                    // A rate of 0.004 Hz printed with one decimal is "0.0": below one, show enough
                    // digits that the number means something.
                    .withStringFromValueFunction([decimals](float v, int) {
                        const int dp = std::abs(v) < 1.0f ? juce::jmax(decimals, std::abs(v) < 0.1f ? 3 : 2) : decimals;
                        return juce::String(v, dp);
                    })));
            break;
        }
        case ParamKind::Int:
            layout.add(std::make_unique<juce::AudioParameterInt>(
                id, d.name, static_cast<int>(d.min), static_cast<int>(d.max), static_cast<int>(d.def),
                juce::AudioParameterIntAttributes().withLabel(d.unit)));
            break;
        case ParamKind::Bool:
            layout.add(std::make_unique<juce::AudioParameterBool>(id, d.name, d.def >= 0.5f));
            break;
        case ParamKind::Choice: {
            juce::StringArray names;
            for (int i = 0; i < d.numChoices; ++i) names.add(d.choices[i]);
            layout.add(std::make_unique<juce::AudioParameterChoice>(id, d.name, names, static_cast<int>(d.def)));
            break;
        }
        }
    }
    return layout;
}

// A crash writes a stack trace where the instrument can be asked for it.
//
// The occasion: the instrument disappeared while a preset was picked off the map, and Windows had
// written nothing at all -- no report in the event log, no minidump in CrashDumps, because local
// dumps are off on most machines and switching them on means the registry. So the instrument keeps
// its own account: a line with the version and the time, then the backtrace, appended to
// Documents/Noctuary/crash.log. Appended, because the second crash is the one that shows which
// part of the first was the accident.
//
// A handler runs in a process that has already lost, so it does the least it can: JUCE's backtrace
// (which walks the stack itself) and C file I/O. No allocation of ours, no locks, no JUCE objects
// built here.
static void installCrashLog()
{
    // Standalone only. Both of these are process-wide hooks, and a plugin that reaches into a
    // host's crash handling is a bad guest -- a DAW has its own, and it is the one the user will
    // send in. The standalone has nobody else to report to.
    if (juce::PluginHostType::getPluginLoadedAs() != juce::AudioProcessor::wrapperType_Standalone) return;
    static std::once_flag once;
    std::call_once(once, [] {
        static const juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                              .getChildFile("Noctuary").getChildFile("crash.log");
        logFile.getParentDirectory().createDirectory();
        static const auto write = [](const char* how) {
            const juce::String trace = juce::SystemStats::getStackBacktrace();
            if (FILE* f = std::fopen(logFile.getFullPathName().toRawUTF8(), "a")) {
                std::fprintf(f, "\n---- Noctuary %s %s %s\n%s\n", JucePlugin_VersionString, how,
                             juce::Time::getCurrentTime().toISO8601(true).toRawUTF8(), trace.toRawUTF8());
                std::fclose(f);
            }
        };
        juce::SystemStats::setApplicationCrashHandler([](void*) { write("crashed"); });
        // Two doors, and JUCE's handler only watches one of them. It installs an unhandled
        // exception filter, which catches an access violation; an uncaught C++ exception goes to
        // std::terminate instead and never passes that filter. The occasion for looking: the
        // instrument vanished while a preset was picked off the map, and Windows recorded neither
        // a dump nor an Application Error entry -- which is what a process that ends through
        // terminate or exit looks like, and not what an access violation looks like. So both.
        static const std::terminate_handler previous = std::set_terminate([] {
            write("terminated");
            if (previous != nullptr) previous();
            std::abort();
        });
    });
}

NoctuaryProcessor::NoctuaryProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "Noctuary", createLayout())
{
    installCrashLog();
    engines_[0] = std::make_unique<ambient::Engine>();   // the instrument; the second is built on demand
    for (int i = 0; i < kNumParams; ++i)
        raw_[static_cast<size_t>(i)] = apvts.getRawParameterValue(paramTable()[static_cast<size_t>(i)].key);
    for (auto* p : getParameters()) p->addListener(&paramWatch_);
    for (auto& c : ccMap_) c.store(-1);
    // Preset packs (thousands of presets as text files) before anything reads the preset list.
    loadDefaultPresetPacks();
    loadFavourites();   // by name, so after the packs: the names have to be there to be found
    // The map's parameter vectors for those presets, on a thread of its own: a second of work
    // that only the map needs, and nothing should wait for it to open a window or start playing.
    PresetMap::warmupAsync();
    // The pump runs for the life of the instrument: it carries out the preset changes OSC asks
    // for and finishes a transition that had to wait for an engine. Two atomic reads at 30 Hz.
    presetPump_.startTimerHz(30);
    // OSC on 9000; a second instance in a DAW simply reports the port as taken.
    osc_.start(9000, *this, gestures_);
    // Session recall. The switch lives in the same settings file as the state, so turning it off
    // survives a restart; off also drops the stored state, before JUCE can read it back -- this
    // constructor runs from the holder's, ahead of its reloadPluginState().
    if (auto* settings = standaloneSettings()) {
        sessionRecall_ = settings->getBoolValue("sessionRecall", true);
        if (!sessionRecall_) settings->removeValue("filterState");
        else startTimer(15000);
    }
}

NoctuaryProcessor::~NoctuaryProcessor()
{
    // The OSC thread first: it calls event() on this object, and the queue it pushes into is
    // declared after it, so it would be destroyed first. Then the warmup thread, which would be
    // executing code this library is about to give back.
    osc_.stop();
    ambient::PresetMap::shutdown();
    for (auto* p : getParameters()) p->removeListener(&paramWatch_);
    presetPump_.stopTimer();
    stopTimer();
    saveSession();
}

// The standalone's settings, or nothing at all when this is a plugin: there the host owns the
// state, saves it with the project and hands it back, and a synth that quietly loaded somebody
// else's last session into a fresh instance would be a bug, not a feature.
juce::PropertySet* NoctuaryProcessor::standaloneSettings()
{
   #if JucePlugin_Build_Standalone
    if (auto* holder = juce::StandalonePluginHolder::getInstance()) return holder->settings.get();
   #endif
    return nullptr;
}

bool NoctuaryProcessor::sessionRecallAvailable() { return standaloneSettings() != nullptr; }

void NoctuaryProcessor::setSessionRecall(bool on)
{
    sessionRecall_ = on;
    auto* settings = standaloneSettings();
    if (settings == nullptr) return;
    settings->setValue("sessionRecall", on);
    if (on) { savedStateHash_ = 0; startTimer(15000); saveSession(); }
    else    { stopTimer(); settings->removeValue("filterState"); }
    if (auto* file = dynamic_cast<juce::PropertiesFile*>(settings)) file->saveIfNeeded();
}

// Writes the state only when it has actually changed, so an instrument left running all night
// touches the disk once. Hashing the block is cheaper than deciding what counts as a change:
// every knob, the matrix, the tuning and the loaded files are in there already.
void NoctuaryProcessor::saveSession()
{
    auto* settings = standaloneSettings();
    if (settings == nullptr || !sessionRecall_) return;
    juce::MemoryBlock data;
    getStateInformation(data);
    if (data.getSize() == 0) return;
    juce::uint32 hash = 2166136261u;                     // FNV-1a over the block
    for (const auto* b = static_cast<const juce::uint8*>(data.getData()), *e = b + data.getSize(); b != e; ++b)
        hash = (hash ^ *b) * 16777619u;
    if (hash == savedStateHash_) return;
    savedStateHash_ = hash;
    settings->setValue("filterState", data.toBase64Encoding());
    if (auto* file = dynamic_cast<juce::PropertiesFile*>(settings)) file->saveIfNeeded();
}

void NoctuaryProcessor::timerCallback() { saveSession(); }

// ---------------------------------------------------------------- OSC sink + gestures

void NoctuaryProcessor::setParam(ParamId id, float value)
{
    if (auto* p = apvts.getParameter(paramTable()[static_cast<size_t>(id)].key))
        p->setValueNotifyingHost(p->convertTo0to1(value));
}

void NoctuaryProcessor::setParamNormalised(ParamId id, float norm)
{
    if (auto* p = apvts.getParameter(paramTable()[static_cast<size_t>(id)].key))
        p->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, norm));
}

void NoctuaryProcessor::event(const ControlEvent& e)
{
    // Notes to the audio thread, preset changes to the message thread: one is a handful of
    // atomics, the other opens files.
    if (e.type == ControlEvent::Type::NoteOn || e.type == ControlEvent::Type::NoteOff) events_.push(e);
    else presetEvents_.push(e);
}

// Message thread, from the pump: the preset changes OSC asked for, and the parameters the map
// left behind when it was switched off.
void NoctuaryProcessor::servePresetRequests()
{
    if (mapExit_.exchange(false, std::memory_order_acq_rel)
        && juce::Time::getMillisecondCounterHiRes() >= mapExitDiscardUntil_) {   // not the exit a journey asked for
        for (const ParamDesc& d : paramTable()) {
            if (isMapParam(d.id) || isMorphParam(d.id) || isMacroParam(d.id)) continue;
            if (auto* p = apvts.getParameter(d.key)) p->setValueNotifyingHost(p->convertTo0to1(live().blendValue(d.id)));
        }
    }
    // A program sweep that has come to rest: read the files of the one that is left standing.
    // Held back until it is quiet, so a sweep that is still moving reads nothing at all.
    if (const int want = pendingFiles_.load(std::memory_order_acquire); want >= 0
        && juce::Time::getMillisecondCounterHiRes() - lastProgramAt_ > 250.0) {
        pendingFiles_.store(-1, std::memory_order_release);
        if (want == currentProgram_) loadPresetFiles(want);
    }
    ControlEvent ev;
    while (presetEvents_.pop(ev)) {
        switch (ev.type) {
        case ControlEvent::Type::Preset:       setCurrentProgram(ev.a); break;
        case ControlEvent::Type::SoundPreset:  applySoundPreset(ev.a); break;
        case ControlEvent::Type::CosmosPreset: applyCosmosPreset(ev.a); break;
        default: break;
        }
    }
}

bool NoctuaryProcessor::setGestureMappings(const juce::String& text)
{
    if (!gestures_.parseMappings(text.toRawUTF8())) return false;
    mappingText_ = text;
    return true;
}

juce::String NoctuaryProcessor::gestureMappings() const
{
    char buf[4096];
    const int n = gestures_.writeMappings(buf, sizeof(buf));
    return juce::String(juce::CharPointer_UTF8(buf), static_cast<size_t>(juce::jmax(0, n)));
}

void NoctuaryProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    lastSampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    lastBlockSize_ = samplesPerBlock;
    for (auto& e : engines_) {
        if (e == nullptr) continue;
        for (int i = 0; i < kNumParams; ++i)
            e->setParam(static_cast<ParamId>(i), raw_[static_cast<size_t>(i)]->load());
        e->prepare(sampleRate, samplesPerBlock);
    }
    scratch_.setSize(2, samplesPerBlock);
    fadeBuf_.setSize(2, samplesPerBlock);
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    // Both engines have just had their buffers cleared, so a transition that was in flight has
    // nothing left to fade out of: it ends here rather than crossfading from silence.
    fading_.store(-1, std::memory_order_relaxed);
    swapTo_.store(-1, std::memory_order_relaxed);
    paramTarget_.store(-1, std::memory_order_relaxed);
    fadePos_.store(1.0f, std::memory_order_relaxed);
    fadeHead_ = 0.0f;
}

bool NoctuaryProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void NoctuaryProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    // Which engine is the instrument is decided here and nowhere else, at a block boundary, so
    // that the message thread can build the next preset on the other one without ever writing to
    // an engine that is being rendered.
    if (endFade_.exchange(false, std::memory_order_acq_rel)) {
        const int f = fading_.load(std::memory_order_relaxed);
        if (f >= 0) { engines_[f]->allNotesOff(); fading_.store(-1, std::memory_order_release); }
    }
    if (const int sw = swapTo_.load(std::memory_order_acquire); sw >= 0) {
        fading_.store(live_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        live_.store(sw, std::memory_order_release);
        // Where the volume stood at the change, on both sides: the leaving engine's own level, and
        // the parameter as the arriving preset left it. What the player turns from here on is the
        // difference between the two readings of the parameter, and it goes to both engines.
        fadeGainFrom_ = engines_[fading_.load(std::memory_order_relaxed)]->getParam(ParamId::MasterGain);
        fadeGainBase_ = raw_[static_cast<size_t>(ParamId::MasterGain)]->load();
        fadePos_.store(0.0f, std::memory_order_relaxed);
        fadeHead_ = 0.0f;
        paramTarget_.store(-1, std::memory_order_release);
        // Cleared LAST. The message thread gives an idle engine back when nothing is fading and
        // no change is on its way; clearing this first opened a window in which both were true
        // while the swap had not happened yet, and it freed the very engine about to be made
        // live. That is a crash a few seconds into every transition.
        swapTo_.store(-1, std::memory_order_release);
    }

    // Macros are gesture inputs Custom0..7: one knob, several parameters.
    for (int m = 0; m < 8; ++m)
        gestures_.setInput(static_cast<GestureInput>(static_cast<int>(GestureInput::Custom0) + m),
                           raw_[static_cast<size_t>(static_cast<int>(ParamId::MacroA) + m)]->load());

    // Gestures write through the host's parameter system, like a MIDI controller would.
    gestures_.update(buffer.getNumSamples() / getSampleRate(), [this](ParamId id, float v) {
        if (auto* p = apvts.getParameter(paramTable()[static_cast<size_t>(id)].key))
            p->setValueNotifyingHost(p->convertTo0to1(v));
    });
    // Notes from OSC arrive on the audio thread through the queue, where they belong. Preset
    // changes do not: applying one writes two hundred and ninety-three parameters through the
    // host and reads a sample, a wavetable and an impulse response off the disk. That was being
    // done here, in the middle of a block, for every preset an OSC client or the Quest app asked
    // for -- file I/O and allocation on the thread that must not wait for anything. They go to
    // the message thread now (see servePresetRequests).
    ControlEvent ev;
    while (events_.pop(ev)) {
        switch (ev.type) {
        case ControlEvent::Type::NoteOn:       noteOn(ev.a, ev.b); break;
        case ControlEvent::Type::NoteOff:      noteOff(ev.a); break;
        default: break;
        }
    }

    // Leaving the preset map: the sound stays where the map left it, so the gliding blend values
    // become the live parameters. Writing two hundred and ninety-three parameters through the host
    // is not work for the audio thread -- it is exactly what was moved off it for OSC preset
    // changes -- so the audio thread only notices that the map has been switched off and the
    // message thread does the writing a few milliseconds later. The blended values stay where they
    // are in the meantime: switching the map off leaves blendCur_ alone.
    //
    // (The block that used to sit here for the arrival of a travelling preset change is gone with
    // it. Nothing has set morphTarget_ since a preset change became a crossfade of two engines,
    // so it applied a preset from the audio thread that was never on its way.)
    if (raw_[static_cast<size_t>(ParamId::MapActive)]->load() < 0.5f && live().mapActive())
        mapExit_.store(true, std::memory_order_release);

    {   // Into the instrument -- or, while a transition is being built, into the engine that is
        // about to become it, so that the new preset's values do not also land on the old sound.
        const int pt = paramTarget_.load(std::memory_order_acquire);
        ambient::Engine& dst = *engines_[pt >= 0 ? pt : live_.load(std::memory_order_relaxed)];
        for (int i = 0; i < kNumParams; ++i)
            dst.setParam(static_cast<ParamId>(i), raw_[static_cast<size_t>(i)]->load());
    }

    // The host's play head, when there is one (the standalone has none and the engine then runs
    // its own clock).
    if (auto* ph = getPlayHead()) {
        if (const auto pos = ph->getPosition()) {
            const double bpm = pos->getBpm().hasValue() ? *pos->getBpm() : 0.0;
            const double ppq = pos->getPpqPosition().hasValue() ? *pos->getPpqPosition() : 0.0;
            live().setHostClock(bpm, ppq, pos->getIsPlaying());
        }
    }

    // Route: the engine walks it and moves the cursor; mirror the cursor (and the switches the
    // route flips) into the host parameters so the GUI and automation see it.
    {
        float rx, ry, rr;
        const bool wasActive = raw_[static_cast<size_t>(ParamId::RouteActive)]->load() >= 0.5f;
        live().routeStep(buffer.getNumSamples() / getSampleRate(), rx, ry, rr);
        // Telling the host about a parameter is what a plugin does when it moves one itself, and
        // it is how the map cursor reaches the display and the automation lane. But a route walks
        // it continuously, so at a 64-sample buffer this was five parameter changes seven hundred
        // and fifty times a second -- for a cursor that crosses the plane over minutes. Thirty
        // times a second is finer than any lane records and finer than any eye sees.
        routeMirrorLeft_ -= buffer.getNumSamples();
        if (wasActive && routeMirrorLeft_ <= 0) {
            routeMirrorLeft_ = static_cast<int>(getSampleRate() / 30.0);
            auto mirror = [this](ParamId id) {
                if (auto* p = apvts.getParameter(paramTable()[static_cast<size_t>(id)].key)) {
                    const float v = live().getParam(id);
                    if (std::fabs(p->convertFrom0to1(p->getValue()) - v) > 1e-6f) p->setValueNotifyingHost(p->convertTo0to1(v));
                }
            };
            mirror(ParamId::MapX); mirror(ParamId::MapY); mirror(ParamId::MapRadius); mirror(ParamId::MapActive); mirror(ParamId::RouteActive);
        }
    }

    // Set timeline: playback feeds parameters (through the host) and notes; recording logs what
    // changed since the last block plus the notes, at the block's time.
    const double blockSeconds = buffer.getNumSamples() / getSampleRate();
    if (setPlaying_.load()) {
        setPlayBusy_.store(true, std::memory_order_release);
        const double t0 = setClock_, t1 = setClock_ + blockSeconds;
        setPlay_.step(t0, t1, [this](const TimelineEvent& e) {
            switch (e.type) {
            case TimelineEvent::Type::Param:
                if (auto* p = apvts.getParameter(paramTable()[static_cast<size_t>(e.a)].key)) p->setValueNotifyingHost(p->convertTo0to1(e.v));
                live().setParam(static_cast<ParamId>(e.a), e.v);
                break;
            case TimelineEvent::Type::NoteOn:  noteOn(e.a, e.v); break;
            case TimelineEvent::Type::NoteOff: noteOff(e.a); break;
            }
        });
        setClock_ = t1;
        setTime_.store(setClock_);
        if (setPlay_.finished() && setClock_ > setPlay_.length() + 1.0) setPlaying_.store(false);
        setPlayBusy_.store(false, std::memory_order_release);
    }
    if (setRecording_.load()) {
        setRecBusy_.store(true, std::memory_order_release);
        for (int i = 0; i < kNumParams; ++i) {
            const float v = raw_[static_cast<size_t>(i)]->load();
            if (v != setLast_[i]) { setRec_.add({ setClock_, TimelineEvent::Type::Param, i, v }); setLast_[i] = v; }
        }
        for (const auto meta : midi) {
            const auto m = meta.getMessage();
            if (m.isNoteOn()) setRec_.add({ setClock_ + meta.samplePosition / getSampleRate(), TimelineEvent::Type::NoteOn, m.getNoteNumber(), m.getFloatVelocity() });
            else if (m.isNoteOff()) setRec_.add({ setClock_ + meta.samplePosition / getSampleRate(), TimelineEvent::Type::NoteOff, m.getNoteNumber(), 0.0f });
        }
        setClock_ += blockSeconds;
        setTime_.store(setClock_);
        setRecBusy_.store(false, std::memory_order_release);
    }

    const bool mpe = raw_[static_cast<size_t>(ParamId::MpeOn)]->load() >= 0.5f;
    for (const auto meta : midi) {
        const auto m = meta.getMessage();
        const int ch = juce::jlimit(1, 16, m.getChannel()) - 1;
        // With MPE every finger has its own channel; without it, expression addresses every
        // sounding voice, which is what channel pressure and the wheel mean on a plain keyboard.
        const int expressed = mpe ? mpeNote_[ch] : -1;
        if (m.isNoteOn())            { if (mpe) mpeNote_[ch] = m.getNoteNumber(); noteOn(m.getNoteNumber(), m.getFloatVelocity()); }
        else if (m.isNoteOff())      { if (mpe && mpeNote_[ch] == m.getNoteNumber()) mpeNote_[ch] = -1; noteOff(m.getNoteNumber()); }
        else if (m.isPitchWheel())   live().setBend(expressed, (m.getPitchWheelValue() - 8192) / 8192.0f);
        else if (m.isChannelPressure()) live().setPressure(expressed, m.getChannelPressureValue() / 127.0f);
        else if (m.isAftertouch())   live().setPressure(m.getNoteNumber(), m.getAfterTouchValue() / 127.0f);
        else if (m.isAllNotesOff() || m.isAllSoundOff()) allNotesOff();
        else if (m.isMidiClock()) {   // 24 a quarter; the interval between two carries the tempo
            const double t = clockSamples_ + meta.samplePosition;
            live().midiClockTick(lastClockSample_ >= 0.0 ? (t - lastClockSample_) / getSampleRate() : 0.0);
            lastClockSample_ = t;
        }
        else if (m.isMidiStart())    live().midiClockStart();
        else if (m.isMidiContinue()) live().midiClockContinue();
        else if (m.isMidiStop())     live().midiClockStop();
        else if (m.isController()) {
            const int cc = m.getControllerNumber();
            if (cc < 0 || cc >= 128) continue;
            // The wheel is a modulation source in its own right. It is not an "else": a player
            // may also have learned CC 1 onto a knob, and both should work.
            if (cc == 1) live().setWheel(m.getControllerValue() / 127.0f);
            // Slide (CC 74, the MPE third dimension) the same way: it used to be its own branch
            // ahead of this one, which meant CC 74 could never be learned onto a knob.
            if (cc == 74) live().setSlide(expressed, m.getControllerValue() / 127.0f);
            const int learn = learnTarget_.exchange(-1);
            if (learn >= 0) {
                for (auto& c : ccMap_) if (c.load() == learn) c.store(-1);   // one controller per parameter
                ccMap_[static_cast<size_t>(cc)].store(learn);
            }
            const int target = ccMap_[static_cast<size_t>(cc)].load();
            if (target >= 0)
                if (auto* p = apvts.getParameter(paramTable()[static_cast<size_t>(target)].key))
                    p->setValueNotifyingHost(static_cast<float>(m.getControllerValue()) / 127.0f);
        }
    }
    midi.clear();
    clockSamples_ += buffer.getNumSamples();

    const int n = buffer.getNumSamples();
    if (scratch_.getNumSamples() < n) scratch_.setSize(2, n, false, false, true);
    if (fadeBuf_.getNumSamples() < n) fadeBuf_.setSize(2, n, false, false, true);
    // Always into scratch_, so the two-engine mix and the mono fold-down read the same place.
    live().process(scratch_.getWritePointer(0), scratch_.getWritePointer(1), n);
    if (fading_ >= 0) {
        // A preset transition: the preset that is leaving is still playing, on its own engine,
        // with every parameter it had -- nothing in it moves. It goes out under a cosine and the
        // new one comes in under a sine, so the sum keeps its power the whole way and there is
        // no dip in the middle and no moment where anything switches. A parameter morph between
        // two unrelated presets cannot do this: its ninety-three switches all flipped at half way
        // and every source restarted, and that is what it sounded like -- a cut in the middle.
        const float secs = std::max(morphSelectSeconds_.load(std::memory_order_relaxed), 0.25f);
        const float t0 = fadePos_;
        // The ramp does not start before there is something to ramp in. Measured on the incoming
        // engine's own output, before any gain: -60 dBFS is audible, and a brain that has not
        // played its first note yet is not. The head start is capped, so a preset that is silent
        // by design still arrives.
        bool ramping = t0 > 0.0f;
        if (!ramping) {
            float e = 0.0f;
            for (int ch = 0; ch < 2; ++ch) {
                const float* s = scratch_.getReadPointer(ch);
                for (int i = 0; i < n; ++i) e += s[i] * s[i];
            }
            fadeHead_ += static_cast<float>(n / sampleRate_);
            ramping = std::sqrt(e / static_cast<float>(2 * n)) > 1e-3f || fadeHead_ >= kFadeHeadStart;
        }
        const float t1 = ramping ? std::min(1.0f, t0 + static_cast<float>(n) / static_cast<float>(sampleRate_ * secs)) : 0.0f;
        // The volume reaches the leaving preset too. Everything else of it stands still on
        // purpose -- it is a sound on its way out -- but the volume is the listener's, not the
        // preset's, and a journey fades for up to a minute and a half after a head start of up to
        // eight seconds in which the leaving engine is nearly all there is to hear. So the knob did
        // nothing at every step and half of what it should for a minute after (Rene, 13.09.2026).
        // As a difference, not a value: the arriving preset's own level would make the leaving one
        // jump the moment the fade began.
        {
            const float moved = raw_[static_cast<size_t>(ParamId::MasterGain)]->load() - fadeGainBase_;
            engines_[fading_]->setParam(ParamId::MasterGain, juce::jlimit(-40.0f, 12.0f, fadeGainFrom_ + moved));
        }
        engines_[fading_]->process(fadeBuf_.getWritePointer(0), fadeBuf_.getWritePointer(1), n);
        const float inA = std::sin(juce::MathConstants<float>::halfPi * t0), inB = std::sin(juce::MathConstants<float>::halfPi * t1);
        const float outA = std::cos(juce::MathConstants<float>::halfPi * t0), outB = std::cos(juce::MathConstants<float>::halfPi * t1);
        for (int ch = 0; ch < 2; ++ch) {
            scratch_.applyGainRamp(ch, 0, n, inA, inB);
            scratch_.addFromWithRamp(ch, 0, fadeBuf_.getReadPointer(ch), n, outA, outB);
        }
        fadePos_ = t1;
        if (t1 >= 1.0f) {   // arrived: the old one is silent by now and goes to sleep
            engines_[fading_]->allNotesOff();
            engines_[fading_]->reset();
            fading_ = -1;
        }
    }
    if (buffer.getNumChannels() >= 2) {
        buffer.copyFrom(0, 0, scratch_, 0, 0, n);
        buffer.copyFrom(1, 0, scratch_, 1, 0, n);
    } else if (buffer.getNumChannels() == 1) {
        buffer.copyFrom(0, 0, scratch_, 0, 0, n);
        buffer.addFrom(0, 0, scratch_, 1, 0, n);
        buffer.applyGain(0.5f);
    }
    for (int ch = 2; ch < buffer.getNumChannels(); ++ch) buffer.clear(ch, 0, n);

    if (recording_.load(std::memory_order_relaxed)) {
        const juce::ScopedTryLock sl(recordLock_);
        if (sl.isLocked() && recordWriter_ != nullptr) {
            const float* chans[2] = { buffer.getReadPointer(0), buffer.getNumChannels() > 1 ? buffer.getReadPointer(1) : buffer.getReadPointer(0) };
            recordWriter_->write(chans, n);
            recordedSamples_.fetch_add(n, std::memory_order_relaxed);
        }
    }
    // Muted: everything above has run -- the engine, its meters and taps, the recorder -- and
    // only what reaches the device is silenced. For the manual export, which plays a chord for
    // its pictures, and for any start meant as a test rather than as music (AMBIENT_MUTE=1).
    if (muteOutput_) buffer.clear();
}

bool NoctuaryProcessor::startRecording(const juce::File& file)
{
    stopRecording();
    file.deleteFile();
    auto stream = std::unique_ptr<juce::FileOutputStream>(file.createOutputStream());
    if (stream == nullptr || stream->failedToOpen()) return false;
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(stream.get(), getSampleRate(), 2, 32, {}, 0));
    if (writer == nullptr) return false;
    stream.release();   // the writer owns the stream now
    recordThread_.startThread();
    {
        const juce::ScopedLock sl(recordLock_);
        recordWriter_ = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(writer.release(), recordThread_, 1 << 18);
        recordedSamples_.store(0);
    }
    recording_.store(true);
    return true;
}

void NoctuaryProcessor::stopRecording()
{
    recording_.store(false);
    {
        const juce::ScopedLock sl(recordLock_);
        recordWriter_.reset();
    }
    recordThread_.stopThread(2000);
}

int NoctuaryProcessor::getNumPrograms() { return numPresets(); }

const juce::String NoctuaryProcessor::getProgramName(int index)
{
    return (index >= 0 && index < numPresets()) ? juce::String(preset(index).name) : juce::String();
}

void NoctuaryProcessor::loadPresetFiles(int index)
{
    // A pack preset can bring its own sample, wavetable and impulses; paths are relative to the pack.
    const juce::String tex(juce::CharPointer_UTF8(presetFilePath(index, 0)));
    const juce::String tab(juce::CharPointer_UTF8(presetFilePath(index, 1)));
    const juce::String imp(juce::CharPointer_UTF8(presetFilePath(index, 2)));
    const juce::String impB(juce::CharPointer_UTF8(presetFilePath(index, 3)));
    // The texture field is one path (every slot gets it, as always) or up to four separated by
    // ';', one per slot, an empty one meaning that slot has none. The per-slot form is explicit
    // about every slot, so a slot it leaves empty is cleared rather than left holding whatever
    // the previous preset put there.
    // What arrives on a user's machine is FLAC, while the pack names the .wav it was made from.
    // Every one of these lines used to ask the file system for the name in the pack and give up
    // when it was not there -- so with the shipped library installed, not one preset loaded its
    // sample. resolveAudioFile answers with whichever of the two is actually on disk.
    auto onDisk = [](const juce::String& ref) {
        const std::string got = ambient::resolveAudioFile(ref.toRawUTF8());
        return got.empty() ? juce::File() : juce::File(juce::String(juce::CharPointer_UTF8(got.c_str())));
    };
    if (tex.isNotEmpty()) {
        if (!tex.containsChar(';')) {
            const juce::File f = onDisk(tex);
            if (f != juce::File()) loadTextureFile(f);
        } else {
            const juce::StringArray parts = juce::StringArray::fromTokens(tex, ";", "");
            for (int k = 0; k < ambient::kSlots; ++k) {
                const juce::String p = k < parts.size() ? parts[k].trim() : juce::String();
                const juce::File f = p.isNotEmpty() ? onDisk(p) : juce::File();
                if (f != juce::File()) loadTextureFile(k, f);
                else { target().clearTexture(k); textureFile_[k] = juce::File(); }
            }
        }
    }
    if (tab.isNotEmpty()) { const juce::File f = onDisk(tab); if (f != juce::File()) loadWavetableFile(f); }
    if (imp.isNotEmpty()) { const juce::File f = onDisk(imp); if (f != juce::File()) loadImpulseFile(f); }
    // Impulse B travels with the preset like A does. A preset that names its room but no B takes
    // the old B away: Room Morph blended into whatever B was left from the preset before, or from
    // a file opened by hand an hour ago, and the same preset sounded different depending on what
    // had been played first.
    if (impB.isNotEmpty()) {
        const juce::File f = onDisk(impB);
        if (f == juce::File() || !loadImpulseFile(f, true)) clearImpulseB();
    } else if (imp.isNotEmpty()) {
        clearImpulseB();
    }
}

void NoctuaryProcessor::applyScoped(const Preset& pr, PresetScope scope)
{
    applyPreset(pr, [this](ParamId id, float v) {
        if (auto* p = apvts.getParameter(paramTable()[static_cast<size_t>(id)].key))
            p->setValueNotifyingHost(p->convertTo0to1(v));
    }, scope);
    // The matrix rows and the envelope shapes are data, not parameters, so they do not travel
    // through the parameter tree: the engine takes them straight from the preset.
    if (scope != PresetScope::Cosmos && scope != PresetScope::Near) target().applyPresetModulation(pr);
}

void NoctuaryProcessor::setCurrentProgram(int index)
{
    if (index < 0 || index >= numPresets()) return;
    currentProgram_ = index;
    soundIndex_ = index;
    soundName_ = preset(index).name;
    zIndex_ = -1; strikeIndex_ = -1;
    cosmosIndex_ = -1;   // the program brought its own Cosmos layer
    applyScoped(preset(index), PresetScope::Full);
    // The foreground the pack's artist table gives this preset, on this path as well. Until
    // 13.09.2026 the call stood only at the end of applySoundPreset -- the box's and the
    // browser's way in -- while this is the way a host's program change comes, and the way every
    // journey step comes, because a step crossfades and a crossfade is served through here. So
    // Auto could be on all night and never bring anything, and a journey played twelve minutes
    // with nothing in the near field. Nothing of the sort happens for a built-in or for a pack
    // with no table: nearAutoPick says so and applyNearAuto returns.
    applyNearAuto(index);
    // The knobs are a tenth of a millisecond. The files a preset names -- its clips, its
    // wavetable, its room -- are a sixth of a SECOND, because they are read off the disk and
    // decoded, and a host can automate the program number like any other parameter. Every step
    // of such a sweep paid that, on whichever thread the host asked from.
    //
    // So a change that stands on its own is served here, exactly as before; a change that
    // arrives while another one is still warm is only remembered, and the pump reads the files
    // once the sweep has stopped. Nothing is lost by it: a preset that lived a millisecond was
    // never heard, and only the one still standing at the end has files worth reading.
    const double now = juce::Time::getMillisecondCounterHiRes();
    const bool burst = now - lastProgramAt_ < 300.0;
    lastProgramAt_ = now;
    if (burst) { pendingFiles_.store(index, std::memory_order_release); return; }
    pendingFiles_.store(-1, std::memory_order_release);
    loadPresetFiles(index);
}

void NoctuaryProcessor::applySoundPreset(int index)
{
    if (index < 0 || index >= numPresets()) return;
    soundIndex_ = index;
    // ...and the program number with it. Only setCurrentProgram used to move this, which is the
    // DAW's way in -- so picking a preset in the box or in the browser left it at 0, and every
    // view that asks "what is playing" (the map's ring, the host's program display) answered
    // "Init" however long you had been playing something else.
    currentProgram_ = index;
    soundName_ = preset(index).name;
    // A sound preset brings its own Z-plane and its own Strike with it, so whatever layer preset
    // was picked before is no longer what is loaded. Cleared rather than left standing: a box
    // that names a filter the sound preset has just overwritten is worse than an empty one.
    zIndex_ = -1; strikeIndex_ = -1;
    applyScoped(preset(index), PresetScope::Sound);
    loadPresetFiles(index);
    applyLevelMatch(index);
    applyNearAuto(index);
}

// The foreground a pack preset brings, while Auto is on: the artist's table by the preset's
// name (nearAutoPick), the near preset applied like one chosen by hand, and its Every scaled by
// the class's factor. A built-in, or a pack without a table, leaves the foreground as it is.
void NoctuaryProcessor::applyNearAuto(int soundIndex)
{
    if (soundIndex < 0 || soundIndex >= numPresets()) return;
    if (live().getParam(ParamId::ForeAuto) < 0.5f) return;
    const int pack = presetPack(soundIndex);
    if (pack < 0) return;
    float factor = 1.0f;
    const int pick = nearAutoPick(presetPackName(pack), preset(soundIndex).name, factor);
    if (pick < 0) return;
    applyNearPreset(pick);
    if (pick > 0) {
        // The near preset's own Every, read where applyNearPreset has just written it: the
        // parameter tree. The engine gets it a block later, so reading the engine here scaled the
        // Every that was there BEFORE -- the default on a fresh instrument, and after that the
        // result of the last scaling, compounding with every preset change until the ceiling
        // (13.09.2026: Whistler's 200 s at a factor of 1.18 came out as 142 instead of 236).
        const float own = raw_[static_cast<size_t>(ParamId::ForeRate)]->load();
        setParam(ParamId::ForeRate, juce::jlimit(10.0f, 900.0f, own * factor));
    }
}

// The loudness of every preset was measured from a twelve-second render (Tools/preset_map.py
// for the built-ins, measure_packs.py for the library). A preset that was never measured has
// none, and is then left alone rather than guessed at.
void NoctuaryProcessor::applyLevelMatch(int index)
{
    if (!levelMatch_ || index < 0 || index >= numPresetMeta()) return;
    const float loud = presetMeta(index).loudDb;
    if (loud >= -0.5f || loud < -80.0f) return;         // 0 means "never measured"
    constexpr float kTarget = -20.5f;                   // the measured median of the built-in presets
    if (auto* p = apvts.getParameter(paramDesc(ParamId::MasterGain).key)) {
        const float now = live().getParam(ParamId::MasterGain);
        const float want = juce::jlimit(-40.0f, 12.0f, now + juce::jlimit(-12.0f, 12.0f, kTarget - loud));
        p->setValueNotifyingHost(p->convertTo0to1(want));
    }
}

void NoctuaryProcessor::applyCosmosPreset(int index)
{
    if (index < 0 || index >= numCosmosPresets()) return;
    cosmosIndex_ = index;
    cosmosName_ = cosmosPreset(index).name;
    applyScoped(cosmosPreset(index), PresetScope::Cosmos);
}

void NoctuaryProcessor::applyZPreset(int index)
{
    if (index < 0 || index >= numZPresets()) return;
    zIndex_ = index;
    applyScoped(zPreset(index), PresetScope::ZPlane);
}

void NoctuaryProcessor::applyStrikePreset(int index)
{
    if (index < 0 || index >= numStrikePresets()) return;
    strikeIndex_ = index;
    applyScoped(strikePreset(index), PresetScope::Strike);
}

void NoctuaryProcessor::applyNearPreset(int index)
{
    if (index < 0 || index >= numNearPresets()) return;
    nearIndex_ = index;
    const Preset& pr = nearPreset(index);
    nearName_ = pr.name;
    applyScoped(pr, PresetScope::Near);
    // The preset's clip, named relative to the library's root, or none: a preset that names no
    // clip takes the one that was loaded away, so the flute preset after the radio preset does
    // not find a voice in its slot.
    if (pr.texture != nullptr && *pr.texture != 0) {
        const std::string got = ambient::resolveLibraryFile(pr.texture);
        const juce::File f(juce::String(juce::CharPointer_UTF8(got.c_str())));
        // A folder is a pool of recordings, a file one clip; nothing found leaves no clip.
        if (got.empty() || !(f.isDirectory() ? loadNearClipFolder(f) : loadNearClipFile(f))) clearNearClip();
    } else clearNearClip();
}

bool NoctuaryProcessor::loadNearClipFile(const juce::File& file)
{
    std::vector<float> l, r; double rate = 0.0;
    if (!readStereo(file, l, r, rate)) return false;
    const double base = baseHzFromName(file.getFileName().toRawUTF8());
    target().setNearTexture(l.data(), r.empty() ? nullptr : r.data(), static_cast<int>(l.size()), rate,
                           base > 0.0 ? base : 261.6256, ambient::loopFromName(file.getFileName().toRawUTF8()));
    nearClipFile_ = file;
    nearClipCount_ = 1;
    return true;
}

bool NoctuaryProcessor::loadNearClipFolder(const juce::File& dir)
{
    // A folder of recordings: up to kNearPoolMax of them, sorted by name so the same folder gives
    // the same pool, each kept to twenty seconds -- these are phrases, not beds. Which of them an
    // event plays is the engine's draw.
    juce::Array<juce::File> all = dir.findChildFiles(juce::File::findFiles, false, "*.flac;*.wav;*.aif;*.aiff;*.ogg;*.mp3");
    all.sort();
    // More than the pool holds: every k-th of them rather than the first forty-eight, so a folder
    // of twelve episodes' phrases is heard from all twelve and not from the first five.
    juce::Array<juce::File> files;
    if (all.size() <= kNearPoolMax) files = all;
    else for (int i = 0; i < kNearPoolMax; ++i) files.add(all[static_cast<int>(std::lround(i * (all.size() - 1.0) / (kNearPoolMax - 1.0)))]);
    std::vector<ambient::Texture> pool;
    for (const juce::File& f : files) {
        std::vector<float> l, r; double rate = 0.0;
        if (!readStereo(f, l, r, rate)) continue;
        const double base = baseHzFromName(f.getFileName().toRawUTF8());
        ambient::Texture t = ambient::Engine::makeTexture(l.data(), r.empty() ? nullptr : r.data(), static_cast<int>(l.size()), rate,
                                                          base > 0.0 ? base : 261.6256, ambient::loopFromName(f.getFileName().toRawUTF8()), 20.0);
        if (!t.empty()) pool.push_back(std::move(t));
    }
    if (pool.empty()) return false;
    const int n = static_cast<int>(pool.size());
    target().setNearTextures(std::move(pool));
    nearClipFile_ = dir;
    nearClipCount_ = n;
    return true;
}

void NoctuaryProcessor::clearNearClip()
{
    target().clearNearTexture();
    nearClipFile_ = juce::File();
    nearClipCount_ = 0;
}

juce::AudioProcessorEditor* NoctuaryProcessor::createEditor()
{
    return new NoctuaryEditor(*this);
}

bool NoctuaryProcessor::loadScalaText(const juce::String& text, const juce::String& displayName)
{
    FixedScale s;
    if (!parseScala(text.toRawUTF8(), s)) return false;
    live().setUserScale(s);
    scalaText_ = text;
    userScaleName_ = displayName.isNotEmpty() ? displayName : juce::String(s.name);
    if (auto* p = apvts.getParameter("scale")) {
        const float norm = p->convertTo0to1(static_cast<float>(kUserScaleIndex));
        p->beginChangeGesture();
        p->setValueNotifyingHost(norm);
        p->endChangeGesture();
    }
    return true;
}

bool NoctuaryProcessor::readMono(const juce::File& file, std::vector<float>& mono, double& sampleRate)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0) return false;
    const juce::int64 maxLen = static_cast<juce::int64>(reader->sampleRate * 120.0);   // two minutes is plenty for a texture
    const int n = static_cast<int>(juce::jmin(reader->lengthInSamples, maxLen));
    juce::AudioBuffer<float> buf(static_cast<int>(reader->numChannels), n);
    if (!reader->read(&buf, 0, n, 0, true, true)) return false;
    mono.assign(static_cast<size_t>(n), 0.0f);
    const float inv = 1.0f / static_cast<float>(buf.getNumChannels());
    for (int c = 0; c < buf.getNumChannels(); ++c) {
        const float* s = buf.getReadPointer(c);
        for (int i = 0; i < n; ++i) mono[static_cast<size_t>(i)] += s[i] * inv;
    }
    sampleRate = reader->sampleRate;
    return true;
}

// The same read, both channels kept. A texture slot plays the recording's own image now, so the
// fold to mono that readMono does would throw away what it is there to play; readMono stays for
// the wavetable loader, which really does want one signal.
bool NoctuaryProcessor::readStereo(const juce::File& file, std::vector<float>& left,
                                       std::vector<float>& right, double& sampleRate)
{
    left.clear();
    right.clear();
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0) return false;
    const juce::int64 maxLen = static_cast<juce::int64>(reader->sampleRate * 120.0);
    const int n = static_cast<int>(juce::jmin(reader->lengthInSamples, maxLen));
    const int chans = static_cast<int>(reader->numChannels);
    juce::AudioBuffer<float> buf(chans, n);
    if (!reader->read(&buf, 0, n, 0, true, true)) return false;
    left.assign(static_cast<size_t>(n), 0.0f);
    if (chans > 1) right.assign(static_cast<size_t>(n), 0.0f);
    int nl = 0, nr = 0;
    for (int c = 0; c < chans; ++c) {
        std::vector<float>& into = (c % 2 == 0 || chans == 1) ? left : right;
        ((c % 2 == 0 || chans == 1) ? nl : nr) += 1;
        const float* s = buf.getReadPointer(c);
        for (int i = 0; i < n; ++i) into[static_cast<size_t>(i)] += s[i];
    }
    for (int i = 0; i < n; ++i) {
        if (nl > 0) left[static_cast<size_t>(i)] /= static_cast<float>(nl);
        if (nr > 0) right[static_cast<size_t>(i)] /= static_cast<float>(nr);
    }
    sampleRate = reader->sampleRate;
    return true;
}

bool NoctuaryProcessor::loadTextureFile(int slot, const juce::File& file)
{
    if (slot < 0 || slot >= ambient::kSlots) return false;
    std::vector<float> l, r; double rate = 0.0;
    if (!readStereo(file, l, r, rate)) return false;
    const double base = baseHzFromName(file.getFileName().toRawUTF8());   // "_A3" suffix from TextureGen
    target().setTexture(slot, l.data(), r.empty() ? nullptr : r.data(), static_cast<int>(l.size()), rate,
                       base > 0.0 ? base : 261.6256, ambient::loopFromName(file.getFileName().toRawUTF8()));
    textureFile_[slot] = file;
    return true;
}

bool NoctuaryProcessor::loadTextureFile(const juce::File& file)
{
    std::vector<float> l, r; double rate = 0.0;
    if (!readStereo(file, l, r, rate)) return false;
    const double base = baseHzFromName(file.getFileName().toRawUTF8());
    target().setTexture(l.data(), r.empty() ? nullptr : r.data(), static_cast<int>(l.size()), rate,
                       base > 0.0 ? base : 261.6256, ambient::loopFromName(file.getFileName().toRawUTF8()));
    for (auto& f : textureFile_) f = file;
    return true;
}

bool NoctuaryProcessor::loadImpulseFile(const juce::File& file, bool second)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0) return false;
    // As much as the Room keeps, and a little more for the resampling. This read twelve seconds and
    // stopped, from the days when the Room held eight -- the Room holds a minute now, and every long
    // space ended in a gate at twelve seconds. What is longer than the Room keeps is shortened by the
    // Room itself, with a window instead of a cut (Convolver::load).
    const double keep = static_cast<double>(target().roomMaxSeconds()) * reader->sampleRate + 4096.0;
    const int n = static_cast<int>(juce::jmin(reader->lengthInSamples, static_cast<juce::int64>(keep)));
    juce::AudioBuffer<float> buf(static_cast<int>(reader->numChannels), n);
    if (!reader->read(&buf, 0, n, 0, true, true)) return false;
    const float* L = buf.getReadPointer(0);
    const float* R = buf.getNumChannels() > 1 ? buf.getReadPointer(1) : nullptr;
    if (second) { target().setImpulseB(L, R, n, reader->sampleRate); impulseBFile_ = file; }
    else        { target().setImpulse(L, R, n, reader->sampleRate);  impulseFile_ = file; }
    return true;
}

void NoctuaryProcessor::clearImpulseB()
{
    target().clearImpulseB();
    impulseBFile_ = juce::File();
}

bool NoctuaryProcessor::loadWavetableFile(const juce::File& file)
{
    // The core's reader first: it knows the chunk in which Serum and Vital name their frame length,
    // and Surge's .wt, and JUCE's readers know neither. What it cannot open (an AIFF, say) comes in
    // through JUCE, and its cycle length is then worked out from the samples.
    std::vector<float> mono;
    int cycle = 0;
    if (!ambient::readWavetableFile(file.getFullPathName().toRawUTF8(), mono, cycle)) {
        double rate = 0.0;
        if (!readMono(file, mono, rate)) return false;
        cycle = ambient::detectCycleLength(mono.data(), static_cast<int>(mono.size()));
    }
    if (!target().loadUserWavetable(mono.data(), static_cast<int>(mono.size()), cycle)) return false;
    wavetableFile_ = file;
    return true;
}

void NoctuaryProcessor::startSetRecording()
{
    setPlaying_.store(false);
    setRec_.clear();
    // All the room it will ever need, taken here on the message thread: from now on the audio
    // thread only writes into it, and it must not allocate while doing so. A quarter of a million
    // events is hours of playing -- past that the recording stops growing instead of stopping the
    // audio.
    setRec_.reserve(1u << 18);
    // The starting state goes in at t = 0 so playback begins from the same sound.
    for (int i = 0; i < kNumParams; ++i) {
        const float v = raw_[static_cast<size_t>(i)]->load();
        setLast_[i] = v;
        setRec_.add({ 0.0, TimelineEvent::Type::Param, i, v });
    }
    setClock_ = 0.0;
    setTime_.store(0.0);
    setRecording_.store(true);
}

bool NoctuaryProcessor::stopSetRecording(const juce::File& saveTo)
{
    // Stop, then wait for the audio thread to finish the block it may be writing into the
    // timeline. Without this the message thread walked the event list while the audio thread was
    // still appending to it -- and if the vector had grown at that moment, it walked freed memory.
    setRecording_.store(false, std::memory_order_release);
    for (int spin = 0; spin < 2000000 && setRecBusy_.load(std::memory_order_acquire); ++spin) { }
    if (saveTo == juce::File()) return true;
    return setRec_.save(saveTo.getFullPathName().toRawUTF8());
}

bool NoctuaryProcessor::playSetFile(const juce::File& file)
{
    setPlaying_.store(false);
    setRecording_.store(false);
    // The audio thread may be inside step() over the very events load() is about to replace.
    for (int spin = 0; spin < 2000000 && setPlayBusy_.load(std::memory_order_acquire); ++spin) { }
    if (!setPlay_.load(file.getFullPathName().toRawUTF8())) return false;
    setPlay_.seek(0.0);
    setClock_ = 0.0;
    setTime_.store(0.0);
    setPlaying_.store(true);
    return true;
}

bool NoctuaryProcessor::savePresetFile(const juce::File& file)
{
    juce::MemoryBlock block;
    getStateInformation(block);
    auto xml = getXmlFromBinary(block.getData(), static_cast<int>(block.getSize()));
    if (xml == nullptr) return false;
    return xml->writeTo(file);
}

bool NoctuaryProcessor::loadPresetFile(const juce::File& file)
{
    auto xml = juce::XmlDocument::parse(file);
    if (xml == nullptr || !xml->hasTagName(apvts.state.getType())) return false;
    juce::MemoryBlock block;
    copyXmlToBinary(*xml, block);
    setStateInformation(block.getData(), static_cast<int>(block.getSize()));
    return true;
}

// Choosing a preset as a journey rather than a cut: the old one plays on while the new one comes
// up under it, and the map draws the crossing from one to the other while it happens.
void NoctuaryProcessor::selectPreset(int index, bool viaMorph)
{
    if (index < 0 || index >= numPresets()) return;
    if (!viaMorph || !morphOnSelect_) { endFade_.store(true, std::memory_order_release); setCurrentProgram(index); return; }
    // A transition, not a morph. The preset that is playing keeps playing, untouched, on the
    // engine it is on; the new one is built on the other engine and only then made the
    // instrument, and the two are crossfaded in processBlock over morphSelectSeconds_. Nothing is
    // interpolated, so nothing has to be interpolable: the two presets may share nothing at all
    // and still meet in the air.
    //
    // Both engines are busy while a transition is running -- one playing, one leaving -- so a
    // change asked for mid-flight ends the running one and waits for the audio thread to let go.
    // It is served a few milliseconds later, by the pump. What must never happen is what this
    // used to do: reset an engine, hand it notes and switch it live from the message thread while
    // the audio thread was rendering it.
    pendingPreset_ = index;
    if (fading_.load(std::memory_order_acquire) >= 0) endFade_.store(true, std::memory_order_release);
    servePendingPreset();
}

// ---------------------------------------------------------------- journeys

bool NoctuaryProcessor::startJourney(const juce::File& file)
{
    ambient::Journey j;
    if (!j.load(file.getFullPathName().toRawUTF8()) || j.steps.empty()) return false;
    if (j.name.empty()) j.name = file.getFileNameWithoutExtension().toStdString();
    return startJourney(j);
}

bool NoctuaryProcessor::startJourney(const ambient::Journey& j)
{
    if (j.steps.empty()) return false;
    journey_ = j;
    // A journey is presets in a row, and Morph is the instrument held between two chosen ones:
    // with both on, every preset the journey loaded was overruled by the blend and the journey
    // appeared to do nothing. Starting one means playing its presets, so Morph lets go.
    setParam(ParamId::MorphActive, 0.0f);
    // The map blend is the same kind of hold, measured the same day: the engine plays the blend of
    // the presets around the cursor and nothing the journey loads. A route plays through the map
    // and switches it back on, so the route goes first. Leaving the map normally writes its blend
    // into the parameters so the sound stays where it was -- here that write would land over the
    // journey's first preset, so it is let go of instead, for a few seconds only, lest a later
    // exit by hand lose its blend to a flag that was set for this one.
    if (raw_[static_cast<size_t>(ParamId::MapActive)]->load() >= 0.5f || raw_[static_cast<size_t>(ParamId::RouteActive)]->load() >= 0.5f) {
        mapExitDiscardUntil_ = juce::Time::getMillisecondCounterHiRes() + 3000.0;
        setParam(ParamId::RouteActive, 0.0f);
        setParam(ParamId::MapActive, 0.0f);
    }
    // Seeded from the clock: the same journey runs differently every evening. A render that
    // wants it repeatable seeds the player itself (ambient_render --journey-seed).
    journeyPlayer_.start(journey_, static_cast<uint64_t>(juce::Time::currentTimeMillis()) | 1ull, 0);
    journeyLastTick_ = 0.0;
    journeyTick();   // the first step begins now
    return true;
}

void NoctuaryProcessor::stopJourney() { journeyPlayer_.stop(); }

juce::String NoctuaryProcessor::journeyStatus() const
{
    if (!journeyPlayer_.running()) return {};
    const int n = static_cast<int>(journey_.steps.size());
    return juce::String(journey_.name) + "  " + juce::String(journeyPlayer_.step() + 1) + "/" + juce::String(n)
         + "  " + juce::String(ambient::Journey::timeText(std::max(0.0, journeyPlayer_.remaining())));
}

void NoctuaryProcessor::journeyAddCurrent(double dwellLo, double dwellHi, double fadeLo, double fadeHi)
{
    if (soundIndex_ < 0 || soundIndex_ >= numPresets()) return;
    ambient::JourneyStep st;
    st.preset = preset(soundIndex_).name;
    st.dwellLo = dwellLo; st.dwellHi = dwellHi; st.fadeLo = fadeLo; st.fadeHi = fadeHi;
    journey_.steps.push_back(st);
}

bool NoctuaryProcessor::saveJourney(const juce::File& file)
{
    if (journey_.steps.empty()) return false;
    if (journey_.name.empty()) journey_.name = file.getFileNameWithoutExtension().toStdString();
    file.getParentDirectory().createDirectory();
    return journey_.save(file.getFullPathName().toRawUTF8());
}

// ---------------------------------------------------------------- favourites

juce::File NoctuaryProcessor::favouritesFile()
{
    const juce::File dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Noctuary");
    dir.createDirectory();
    return dir.getChildFile("favourites.txt");
}

void NoctuaryProcessor::loadFavourites()
{
    favourites_.clear();
    favouriteNames_.clear();
    favouritesFirst_ = false;
    const juce::File f = favouritesFile();
    if (!f.existsAsFile()) return;
    // Name to index once, not a scan of fourteen thousand names per line. The first preset of a
    // name wins, which is what the preset boxes do with a name as well.
    std::unordered_map<std::string, int> index;
    for (int i = 0; i < numPresets(); ++i) index.emplace(preset(i).name, i);
    juce::StringArray lines;
    f.readLines(lines);
    for (const juce::String& raw : lines) {
        const juce::String line = raw.trim();
        if (line.isEmpty() || line.startsWith("#")) continue;
        if (line.startsWithIgnoreCase("first ")) { favouritesFirst_ = line.substring(6).trim().equalsIgnoreCase("on"); continue; }
        favouriteNames_.addIfNotAlreadyThere(line);
        const auto it = index.find(line.toStdString());
        if (it != index.end()) favourites_.setBit(it->second, true);
    }
}

void NoctuaryProcessor::saveFavourites() const
{
    juce::String text;
    text << "# Noctuary favourites: one preset name a line. A name whose preset is not installed is kept.\n"
         << "first " << (favouritesFirst_ ? "on" : "off") << "\n";
    for (const juce::String& n : favouriteNames_) text << n << "\n";
    favouritesFile().replaceWithText(text);
}

void NoctuaryProcessor::setFavourite(int index, bool on)
{
    if (index < 0 || index >= numPresets()) return;
    favourites_.setBit(index, on);
    const juce::String name(preset(index).name);
    if (on) favouriteNames_.addIfNotAlreadyThere(name);
    else    favouriteNames_.removeString(name);
    saveFavourites();
}

juce::File NoctuaryProcessor::userJourneyFolder()
{
    const juce::File dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Noctuary").getChildFile("Journeys");
    dir.createDirectory();
    return dir;
}

// The user's own first, then the templates wherever the library lies (the installer's folders,
// the source tree's Library): every *.journey, each folder once, sorted by name.
juce::Array<juce::File> NoctuaryProcessor::journeyFiles()
{
    juce::Array<juce::File> out;
    juce::StringArray seen;
    auto add = [&](const juce::File& dir) {
        if (!dir.isDirectory() || seen.contains(dir.getFullPathName())) return;
        seen.add(dir.getFullPathName());
        juce::Array<juce::File> files = dir.findChildFiles(juce::File::findFiles, false, "*.journey");
        files.sort();
        out.addArray(files);
    };
    add(userJourneyFolder());
    char roots[4096] = {};
    libraryRoots(roots, static_cast<int>(sizeof(roots)));
    for (const juce::String& r : juce::StringArray::fromTokens(juce::String(juce::CharPointer_UTF8(roots)), ";", ""))
        if (r.isNotEmpty()) add(juce::File::isAbsolutePath(r) ? juce::File(r).getChildFile("Journeys") : juce::File::getCurrentWorkingDirectory().getChildFile(r).getChildFile("Journeys"));
    return out;
}

// The pump's tick: the seconds since the last, and the step the player hands over when one
// begins -- the preset by name, brought up over the drawn fade, and the foreground it asks for.
void NoctuaryProcessor::journeyTick()
{
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    if (journeyLastTick_ <= 0.0) journeyLastTick_ = now;
    const double dt = juce::jlimit(0.0, 5.0, now - journeyLastTick_);
    journeyLastTick_ = now;
    if (!journeyPlayer_.running()) return;
    ambient::JourneyStep st;
    double fade = 30.0;
    if (!journeyPlayer_.advance(dt, st, fade)) return;
    int index = -1;
    for (int i = 0; i < numPresets(); ++i) if (st.preset == preset(i).name) { index = i; break; }
    if (index < 0) return;   // a name the library no longer has: nothing changes until the next step
    // near=keep pins what is playing (Auto off) before the sound preset could bring its own;
    // a name pins that one; nothing, or auto, leaves it to the sound preset's Auto.
    if (st.nearPreset == "keep") setNearAuto(false);
    setMorphSelectSeconds(static_cast<float>(fade));
    selectPreset(index, true);
    if (!st.nearPreset.empty() && st.nearPreset != "auto" && st.nearPreset != "keep")
        for (int i = 0; i < numNearPresets(); ++i)
            if (st.nearPreset == nearPreset(i).name) { setNearAuto(false); applyNearPreset(i); break; }
}

// Message thread: the engine at `i`, built and prepared if it is not there. Allocating a hundred
// megabytes and generating a room impulse is fine here and nowhere near the audio thread.
ambient::Engine& NoctuaryProcessor::ensureEngine(int i)
{
    if (engines_[i] == nullptr) {
        auto e = std::make_unique<ambient::Engine>();
        for (int k = 0; k < kNumParams; ++k)
            e->setParam(static_cast<ParamId>(k), raw_[static_cast<size_t>(k)]->load());
        e->prepare(lastSampleRate_, lastBlockSize_);
        engines_[i] = std::move(e);
        carryUserData(*engines_[i]);
    }
    return *engines_[i];
}

// What the player loaded by hand, into an engine that has just been built. A preset brings its own
// sample, wavetable and impulse and overwrites these a moment later; what it does NOT bring is a
// Scala scale a player tuned the instrument to, or a wavetable, clip or room they opened
// themselves. Those live in the engine and nowhere else, and a fresh engine starts without them --
// so a preset change silently retuned the instrument to twelve-tone equal temperament and put the
// built-in table back. Copied from the engine that is playing, not read from disk again.
void NoctuaryProcessor::carryUserData(ambient::Engine& e)
{
    ambient::Engine& from = live();
    if (scalaText_.isNotEmpty()) {
        FixedScale sc;
        if (parseScala(scalaText_.toRawUTF8(), sc)) e.setUserScale(sc);
    }
    if (const ambient::Wavetable* wt = from.userWavetable()) e.setUserWavetable(*wt);
    if (const ambient::CycleTable* ct = from.userCycles()) e.setUserCycles(*ct);
    for (int k = 0; k < ambient::kSlots; ++k)
        if (const ambient::Texture* t = from.displayTexture(k))
            if (!t->empty())
                e.setTexture(k, *t);
    if (const std::vector<ambient::Texture>* pool = from.displayNearPool())   // the near layer's clips survive the change too
        e.setNearTextures(*pool);
    // The impulse responses are the one thing the engine cannot hand over -- it keeps them as
    // spectra, not as samples -- so a room the player opened is read from its file again. A
    // generated room needs nothing: a new engine makes its own.
    if (from.hasUserImpulse() && impulseFile_.existsAsFile()) {
        ambient::Engine* was = prepareTarget_;
        prepareTarget_ = &e;
        loadImpulseFile(impulseFile_, false);
        if (impulseBFile_.existsAsFile()) loadImpulseFile(impulseBFile_, true);
        prepareTarget_ = was;
    }
}

// Message thread: the engine nobody is using goes back. Only when nothing is fading and no change
// is on its way, which is exactly when the audio thread touches the live one and nothing else.
void NoctuaryProcessor::releaseIdleEngine()
{
    // swapTo_ FIRST, then fading_. The audio thread writes fading_ before it clears swapTo_, so
    // a reader that sees swapTo_ cleared is guaranteed to see fading_ set. Read the other way
    // round there was a two-load window -- fading_ still -1, swapTo_ already -1 -- in which a
    // crossfade that had just begun looked like nothing at all, and the engine freed here was the
    // one the audio thread was rendering.
    if (swapTo_.load(std::memory_order_acquire) >= 0) return;
    if (fading_.load(std::memory_order_acquire) >= 0) return;
    if (pendingPreset_ >= 0 || prepareTarget_ != nullptr) return;
    engines_[live_.load(std::memory_order_relaxed) ^ 1].reset();
}

// Message thread. Runs when neither engine is being rendered but one: the one that is not live is
// then ours to build on.
void NoctuaryProcessor::servePendingPreset()
{
    if (pendingPreset_ < 0) { releaseIdleEngine(); return; }
    // Still busy: a fade in flight (both engines rendered), or a swap already published and not
    // yet taken up. Come back in a moment.
    if (swapTo_.load(std::memory_order_acquire) >= 0 || fading_.load(std::memory_order_acquire) >= 0) return;   // this order, see releaseIdleEngine
    const int index = pendingPreset_;
    pendingPreset_ = -1;
    beginTransition(index);
}

void NoctuaryProcessor::beginTransition(int index)
{
    const int incoming = live_.load(std::memory_order_relaxed) ^ 1;
    // Where the sound is leaving from, for the map's line.
    fadingFrom_ = currentProgram_;
    // The engine has to exist before anything is pointed at it. Publishing paramTarget_ first
    // told the audio thread to push the parameter tree into an engine that was built on the very
    // next line -- a null dereference in processBlock, a second into every transition.
    ambient::Engine& in = ensureEngine(incoming);
    // From here until the swap, everything about the new preset goes to the incoming engine and
    // nothing to the one that is still playing: the parameter tree's next push (paramTarget_),
    // the modulation matrix and the sample files (prepareTarget_), the notes.
    paramTarget_.store(incoming, std::memory_order_release);
    prepareTarget_ = &in;
    in.allNotesOff();
    in.reset();
    setCurrentProgram(index);
    // The parameter tree reaches the engine once a block; this one has to be complete before it
    // is heard, so it is filled in here as well.
    for (int i = 0; i < kNumParams; ++i)
        in.setParam(static_cast<ParamId>(i), raw_[static_cast<size_t>(i)]->load());
    // A change that came within the burst window of the one before it had its files put off to
    // the pump (see setCurrentProgram). A transition cannot have that: the engine is built here
    // and only then made the instrument, and its samples are part of the build. Put off, they
    // were read a quarter of a second later into whichever engine was live by then -- the right
    // one when the audio thread had already swapped, the one on its way out when it had not (a
    // host with the transport stopped, the host test's two changes 0.2 s of wall time apart) --
    // and a preset made of textures arrived with nothing to play.
    if (pendingFiles_.exchange(-1, std::memory_order_acq_rel) == index) loadPresetFiles(index);
    // The conductor takes the chord over from the one it is replacing. A crossfade is meant to
    // change the instrument and not the music: picking its own notes made it two pieces of music
    // at once for the length of the fade, and picking them at its own event rate -- ninety-nine
    // seconds in places -- made it minutes of a single note. It inherits the cluster and carries
    // on with it, letting go of what does not suit it and adding what it wants at its own pace.
    // With nothing to inherit, from silence or from a preset whose conductor was off, it fills
    // instead, so a change never lands on an empty instrument either.
    {
        // The CONDUCTOR's cluster, which is twelve notes -- not ambient::kSlots, which is the four
        // source slots. Two constants of the same name, and these arrays had the wrong one: a
        // conductor holding more than four notes wrote up to eight ints and eight floats past the
        // end of them. /GS answers that with __report_gsfailure and int 29h, which kills the
        // process on the spot -- no crash handler, no minidump, no entry in the event log, and an
        // exit that looks clean from outside. That is the "instrument vanished while a preset was
        // picked" this file's crash log was written for; it takes a preset whose conductor is
        // holding a full chord, which is why a change made a second after the last one never
        // showed it.
        int notes[ambient::ClusterBrain::kSlots]; float vels[ambient::ClusterBrain::kSlots];
        for (int which = 0; which < 2; ++which) {
            const bool second = which == 1;
            const int n = live().soundingCluster(notes, vels, second);
            if (n > 0) in.adoptCluster(notes, vels, n, second);
            else if (!second) in.requestBrainFill();
        }
        // The foreground carries on too: a sequence that was running keeps its ring and its
        // place in it, a gap that was half over stays half over. What it does not carry is the
        // note that was sounding -- that voice belongs to the engine that is leaving.
        in.adoptNear(live().nearState());
        // And the Morph snapshots. They live in the engine, and a crossfade hands the sound to the
        // other one -- so a snapshot chosen before a journey step was gone after it, while the box
        // went on showing its name over the new engine's own, which were the defaults (13.09.2026).
        for (int slot = 0; slot < 2; ++slot) {
            if (live().morphSlotSet(slot)) {
                float v[kNumParams];
                live().morphSlot(slot, v);
                in.setMorphSlot(slot, v);
            } else {
                in.clearMorphSlot(slot);
            }
        }
    }
    // The chord that is being held is held on the new instrument too. Without this a player
    // holding a chord through a preset change heard it die with the old preset and nothing take
    // its place -- the notes had gone to an engine that was on its way out.
    for (int i = 0; i < 128; ++i)
        if (heldVel_[static_cast<size_t>(i)] > 0.0f) in.noteOn(i, heldVel_[static_cast<size_t>(i)]);
    prepareTarget_ = nullptr;
    fadePos_.store(0.0f, std::memory_order_relaxed);      // nothing is fading yet: the map reads this
    swapTo_.store(incoming, std::memory_order_release);   // the audio thread takes it from here
}

// Notes go to the live engine and are remembered, so a transition can hand them on.
void NoctuaryProcessor::noteOn(int note, float vel)
{
    if (note < 0 || note >= 128) return;
    heldVel_[static_cast<size_t>(note)] = std::max(vel, 1.0f / 127.0f);
    live().noteOn(note, vel);
}

void NoctuaryProcessor::noteOff(int note)
{
    if (note < 0 || note >= 128) return;
    heldVel_[static_cast<size_t>(note)] = 0.0f;
    live().noteOff(note);
}

void NoctuaryProcessor::allNotesOff()
{
    for (auto& h : heldVel_) h.store(0.0f, std::memory_order_relaxed);
    live().allNotesOff();
}

void NoctuaryProcessor::setMorphSlotFromPreset(int slot, int presetIndex)
{
    if (presetIndex < 0 || presetIndex >= numPresets()) return;
    float values[kNumParams];
    for (int i = 0; i < kNumParams; ++i) values[i] = raw_[static_cast<size_t>(i)]->load();
    applyPreset(preset(presetIndex), [&](ParamId id, float v) { values[static_cast<int>(id)] = v; });
    live().setMorphSlot(slot, values);
    slotName_[slot & 1] = preset(presetIndex).name;
}

void NoctuaryProcessor::setMorphSlotFromCurrent(int slot)
{
    float values[kNumParams];
    for (int i = 0; i < kNumParams; ++i) values[i] = raw_[static_cast<size_t>(i)]->load();
    live().setMorphSlot(slot, values);
    slotName_[slot & 1] = "(captured)";
}

void NoctuaryProcessor::clearMidiLearn(ParamId id)
{
    for (auto& c : ccMap_) if (c.load() == static_cast<int>(id)) c.store(-1);
    if (learnTarget_.load() == static_cast<int>(id)) learnTarget_.store(-1);
}

int NoctuaryProcessor::midiCcFor(ParamId id) const
{
    for (int cc = 0; cc < 128; ++cc) if (ccMap_[static_cast<size_t>(cc)].load() == static_cast<int>(id)) return cc;
    return -1;
}

void NoctuaryProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    if (scalaText_.isNotEmpty()) {
        state.setProperty("scalaText", scalaText_, nullptr);
        state.setProperty("scalaName", userScaleName_, nullptr);
    }
    state.setProperty("gestureMappings", gestureMappings(), nullptr);
    {   // modulation: the matrix, the six envelope shapes and the four sources' own (see ambient/Modulation.h)
        char buf[4096];
        if (live().writeModMatrix(buf, sizeof(buf)) > 0) state.setProperty("modMatrix", juce::String(buf), nullptr);
        juce::String envs;
        const int shapes = ambient::kNumModEnvs + ambient::kSlots;
        for (int i = 0; i < shapes; ++i) {
            const int n = i < ambient::kNumModEnvs ? live().writeEnvShape(i, buf, sizeof(buf))
                                                   : live().writeSrcEnvShape(i - ambient::kNumModEnvs, buf, sizeof(buf));
            if (n > 0) envs += juce::String(buf);
            if (i + 1 < shapes) envs += "~";
        }
        state.setProperty("modEnvs", envs, nullptr);
    }
    // The favourites are no longer in the state (by index, which a regenerated library renumbers);
    // they live by name in the player's own file. An old state's property is read no more.
    if (routeText_.isNotEmpty()) state.setProperty("route", routeText_, nullptr);
    // One property per slot. An older state carries a single "textureFile", which is read below
    // as "the same clip in every slot" -- the only thing it could have meant at the time.
    for (int k = 0; k < ambient::kSlots; ++k)
        if (textureFile_[k].existsAsFile())
            state.setProperty("textureFile" + juce::String(k + 1), textureFile_[k].getFullPathName(), nullptr);
    if (wavetableFile_.existsAsFile()) state.setProperty("wavetableFile", wavetableFile_.getFullPathName(), nullptr);
    if (impulseFile_.existsAsFile())   state.setProperty("impulseFile", impulseFile_.getFullPathName(), nullptr);
    if (impulseBFile_.existsAsFile())  state.setProperty("impulseBFile", impulseBFile_.getFullPathName(), nullptr);
    // The names of the two loaded presets, so the boxes still say what is loaded after a restart.
    if (soundName_.isNotEmpty())  state.setProperty("soundPreset", soundName_, nullptr);
    if (cosmosName_.isNotEmpty()) state.setProperty("cosmosPreset", cosmosName_, nullptr);
    if (nearName_.isNotEmpty())   state.setProperty("nearPreset", nearName_, nullptr);
    if (compact_) state.setProperty("compact", 1, nullptr);          // how the editor is laid out
    if (layoutMode_ != 0) state.setProperty("layout", layoutMode_, nullptr);
    if (levelMatch_) state.setProperty("levelMatch", 1, nullptr);
    juce::ValueTree midi("midi");
    for (int cc = 0; cc < 128; ++cc) {
        const int target = ccMap_[static_cast<size_t>(cc)].load();
        if (target >= 0) midi.setProperty("cc" + juce::String(cc), paramTable()[static_cast<size_t>(target)].key, nullptr);
    }
    state.addChild(midi, -1, nullptr);
    for (int slot = 0; slot < 2; ++slot) {
        juce::ValueTree m(slot == 0 ? "morphA" : "morphB");
        m.setProperty("name", slotName_[slot], nullptr);
        m.setProperty("set", live().morphSlotSet(slot), nullptr);   // a slot nobody chose is not a snapshot
        float values[kNumParams];
        live().morphSlot(slot, values);
        for (int i = 0; i < kNumParams; ++i) m.setProperty(paramTable()[static_cast<size_t>(i)].key, values[i], nullptr);
        state.addChild(m, -1, nullptr);
    }
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, destData);
}

void NoctuaryProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes)) {
        if (xml->hasTagName(apvts.state.getType())) {
            auto tree = juce::ValueTree::fromXml(*xml);
            const juce::String text = tree.getProperty("scalaText").toString();
            const juce::String name = tree.getProperty("scalaName").toString();
            for (auto& c : ccMap_) c.store(-1);
            const juce::String mappings = tree.getProperty("gestureMappings").toString();
            if (mappings.isNotEmpty()) setGestureMappings(mappings);
            auto midi = tree.getChildWithName("midi");
            if (midi.isValid())
                for (int cc = 0; cc < 128; ++cc) {
                    const juce::String key = midi.getProperty("cc" + juce::String(cc)).toString();
                    if (const ParamDesc* d = findParam(key.toRawUTF8())) ccMap_[static_cast<size_t>(cc)].store(static_cast<int>(d->id));
                }
            for (int slot = 0; slot < 2; ++slot) {
                auto m = tree.getChildWithName(slot == 0 ? "morphA" : "morphB");
                if (!m.isValid()) continue;
                float values[kNumParams];
                for (int i = 0; i < kNumParams; ++i) {
                    const ParamDesc& d = paramTable()[static_cast<size_t>(i)];
                    values[i] = m.hasProperty(d.key) ? static_cast<float>(static_cast<double>(m.getProperty(d.key))) : d.def;
                }
                // Whether the slot was chosen. A state from before 2.0.2 does not say, and wrote both
                // slots every time -- so every one of them read back as a snapshot of the defaults,
                // and the session recall carried the trap into every start. For those the name
                // decides: "Init" is what an untouched slot was called, so it counts as not chosen.
                const juce::String nm = m.getProperty("name").toString();
                const bool chosen = m.hasProperty("set") ? static_cast<bool>(m.getProperty("set")) : (nm.isNotEmpty() && nm != "Init");
                if (chosen) { live().setMorphSlot(slot, values); slotName_[slot] = nm; }
                else        { live().clearMorphSlot(slot);       slotName_[slot] = {}; }
            }
            tree.removeChild(tree.getChildWithName("midi"), nullptr);
            tree.removeChild(tree.getChildWithName("morphA"), nullptr);
            tree.removeChild(tree.getChildWithName("morphB"), nullptr);
            const juce::String texPath = tree.getProperty("textureFile").toString();
            juce::String texPaths[ambient::kSlots];
            for (int k = 0; k < ambient::kSlots; ++k) texPaths[k] = tree.getProperty("textureFile" + juce::String(k + 1)).toString();
            const juce::String tabPath = tree.getProperty("wavetableFile").toString();
            const juce::String irPath = tree.getProperty("impulseFile").toString();
            const juce::String irBPath = tree.getProperty("impulseBFile").toString();
            // Which presets the two boxes should say are loaded. Looked up by name, and simply
            // not selected when the pack that held it is gone -- the sound is in the state
            // either way, only the label would have been a lie.
            soundName_  = tree.getProperty("soundPreset").toString();
            cosmosName_ = tree.getProperty("cosmosPreset").toString();
            // A whole state came in: the layer boxes have no name to show, because the state is
            // the parameters themselves and not the presets they may once have come from.
            zIndex_ = -1; strikeIndex_ = -1;
            if (soundName_.isNotEmpty()) {
                soundIndex_ = -1;
                for (int i = 0; i < numPresets(); ++i) if (soundName_ == preset(i).name) { soundIndex_ = i; break; }
                // The host's program index and the map's ring follow the same name; without this
                // they kept showing whatever was loaded before the state came in.
                if (soundIndex_ >= 0) currentProgram_ = soundIndex_;
            }
            if (cosmosName_.isNotEmpty()) {
                cosmosIndex_ = -1;
                for (int i = 0; i < numCosmosPresets(); ++i) if (cosmosName_ == cosmosPreset(i).name) { cosmosIndex_ = i; break; }
            }
            nearName_ = tree.getProperty("nearPreset").toString();
            nearIndex_ = -1;
            if (nearName_.isNotEmpty())
                for (int i = 0; i < numNearPresets(); ++i) if (nearName_ == nearPreset(i).name) { nearIndex_ = i; break; }
            compact_ = static_cast<int>(tree.getProperty("compact", 0)) != 0;
            layoutMode_ = static_cast<int>(tree.getProperty("layout", compact_ ? 1 : 0));
            compact_ = layoutMode_ == 1;
            levelMatch_ = static_cast<int>(tree.getProperty("levelMatch", 0)) != 0;
            const juce::String route = tree.getProperty("route").toString();
            const juce::String modMatrix = tree.getProperty("modMatrix").toString();
            const juce::String modEnvs = tree.getProperty("modEnvs").toString();
            if (modMatrix.isNotEmpty()) live().setModMatrixText(modMatrix.toRawUTF8());
            if (modEnvs.isNotEmpty()) {
                const juce::StringArray parts = juce::StringArray::fromTokens(modEnvs, "~", "");
                // The six, then the sources' own; a state saved before those existed has six.
                for (int i = 0; i < juce::jmin(parts.size(), ambient::kNumModEnvs + ambient::kSlots); ++i) {
                    if (parts[i].isEmpty()) continue;
                    if (i < ambient::kNumModEnvs) live().setEnvShape(i, parts[i].toRawUTF8());
                    else live().setSrcEnvShape(i - ambient::kNumModEnvs, parts[i].toRawUTF8());
                }
            }
            if (route.isNotEmpty()) setRouteText(route);
            tree.removeProperty("route", nullptr);
            tree.removeProperty("textureFile", nullptr);
            for (int k = 0; k < ambient::kSlots; ++k) tree.removeProperty("textureFile" + juce::String(k + 1), nullptr);
            tree.removeProperty("wavetableFile", nullptr);
            tree.removeProperty("impulseFile", nullptr);
            tree.removeProperty("favourites", nullptr);
            tree.removeProperty("soundPreset", nullptr);
            tree.removeProperty("cosmosPreset", nullptr);
            apvts.replaceState(tree);
            // Make the parameter objects agree with the state that was just restored. Replacing
            // the state moves a parameter only where the value IN THE TREE changes, and a switch
            // puts only 0 or 1 there while the object keeps whatever raw number the host set it
            // to: a switch a host had left at 0.87 -- which is "on" -- stayed at 0.87 when a
            // state saying 1.0 came in, and the host read 0.87 back out of a session it had
            // saved as 1.0. The sound was right either way, because the engine reads the
            // switched value and not the raw one, so nothing here ever heard it. pluginval did.
            for (const ParamDesc& d : paramTable()) {
                auto* par = apvts.getParameter(d.key);
                const auto* raw = apvts.getRawParameterValue(d.key);
                if (par == nullptr || raw == nullptr) continue;
                const float norm = par->convertTo0to1(raw->load());
                if (std::fabs(par->getValue() - norm) > 1.0e-6f) par->setValueNotifyingHost(norm);
            }
            if (text.isNotEmpty()) loadScalaText(text, name);
            // A saved state names the file the way it was when it was saved; the same two
            // spellings apply (see loadPresetFiles), so the same rule answers here.
            auto onDisk = [](const juce::String& ref) {
                const std::string got = ambient::resolveAudioFile(ref.toRawUTF8());
                return got.empty() ? juce::File() : juce::File(juce::String(juce::CharPointer_UTF8(got.c_str())));
            };
            if (texPath.isNotEmpty()) { const juce::File f = onDisk(texPath); if (f != juce::File()) loadTextureFile(f); }
            for (int k = 0; k < ambient::kSlots; ++k)
                if (texPaths[k].isNotEmpty()) { const juce::File f = onDisk(texPaths[k]); if (f != juce::File()) loadTextureFile(k, f); }
            if (tabPath.isNotEmpty()) { const juce::File f = onDisk(tabPath); if (f != juce::File()) loadWavetableFile(f); }
            if (irPath.isNotEmpty())  { const juce::File f = onDisk(irPath);  if (f != juce::File()) loadImpulseFile(f); }
            if (irBPath.isNotEmpty()) { const juce::File f = onDisk(irBPath); if (f != juce::File()) loadImpulseFile(f, true); }
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NoctuaryProcessor();
}
