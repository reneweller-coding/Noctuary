/**
 * @file PluginProcessor.cpp
 * @brief Implementation of the Noctuary processor: parameters, the audio block, presets and their
 *        files, the two-engine transition, journeys, favourites, MIDI and state.
 *
 * The order of the file follows the life of the instrument: the parameter layout and the crash
 * log, construction and the standalone's session recall, the OSC sink, the audio block, the
 * recorder, programs and preset layers, file loading, the set timeline, preset files, the
 * travelling preset change, journeys, favourites, the engine handshake (ensureEngine,
 * beginTransition, servePendingPreset), notes, MIDI learn, and the state last. The threads each
 * function runs on are said at the function; the rules behind them are in PluginProcessor.h.
 *
 * The standalone's own headers are included under `JucePlugin_Build_Standalone` right below.
 * For the standalone's settings file: the same one JUCE saves the state into when the window is
 * closed. Reaching it here is what lets the session be written while the app is still running.
 */
#include "PluginProcessor.h"
#include "ambient/WavFile.h"
#include "Frame.h"
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
        const juce::ParameterID id(d.m_key, 1);
        switch (d.m_kind) {
        case ParamKindE::FLOAT: {
            juce::NormalisableRange<float> range(d.m_min, d.m_max, 0.0f, d.m_skew);
            const float span = d.m_max - d.m_min;
            const int decimals = span >= 100.0f ? 0 : (span >= 5.0f ? 1 : 2);
            layout.add(std::make_unique<juce::AudioParameterFloat>(
                id, d.m_name, range, d.m_def,
                juce::AudioParameterFloatAttributes()
                    .withLabel(d.m_unit)
                    // A rate of 0.004 Hz printed with one decimal is "0.0": below one, show enough
                    // digits that the number means something.
                    .withStringFromValueFunction([decimals](float v, int) {
                        const int dp = std::abs(v) < 1.0f ? juce::jmax(decimals, std::abs(v) < 0.1f ? 3 : 2) : decimals;
                        return juce::String(v, dp);
                    })));
            break;
        }
        case ParamKindE::INT:
            layout.add(std::make_unique<juce::AudioParameterInt>(
                id, d.m_name, static_cast<int>(d.m_min), static_cast<int>(d.m_max), static_cast<int>(d.m_def),
                juce::AudioParameterIntAttributes().withLabel(d.m_unit)));
            break;
        case ParamKindE::BOOL:
            layout.add(std::make_unique<juce::AudioParameterBool>(id, d.m_name, d.m_def >= 0.5f));
            break;
        case ParamKindE::CHOICE: {
            juce::StringArray names;
            for (int i = 0; i < d.m_numChoices; ++i) names.add(d.m_choices[i]);
            layout.add(std::make_unique<juce::AudioParameterChoice>(id, d.m_name, names, static_cast<int>(d.m_def)));
            break;
        }
        }
    }
    return layout;
}

/**
 * @brief A crash writes a stack trace where the instrument can be asked for it.
 *
 * The occasion: the instrument disappeared while a preset was picked off the map, and Windows had
 * written nothing at all -- no report in the event log, no minidump in CrashDumps, because local
 * dumps are off on most machines and switching them on means the registry. So the instrument keeps
 * its own account: a line with the version and the time, then the backtrace, appended to
 * Documents/Noctuary/crash.log. Appended, because the second crash is the one that shows which
 * part of the first was the accident.
 *
 * A handler runs in a process that has already lost, so it does the least it can: JUCE's backtrace
 * (which walks the stack itself) and C file I/O. No allocation of ours, no locks, no JUCE objects
 * built here.
 *
 * Standalone only, and once per process (std::call_once); called first thing in the processor's
 * constructor. Installs JUCE's crash handler and a std::terminate handler, both writing the same
 * line and backtrace.
 */
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
    : AudioProcessor(busLayout()),
      m_apvts(*this, nullptr, "Noctuary", createLayout())
{
    installCrashLog();
    m_engines[0] = std::make_unique<ambient::Engine>();   // the instrument; the second is built on demand
    m_engines[0]->setNoteTap(&m_noteTap);   // MIDI out (02.10.2026)
    for (int i = 0; i < NumParams; ++i)
        m_raw[static_cast<size_t>(i)] = m_apvts.getRawParameterValue(paramTable()[static_cast<size_t>(i)].m_key);
    for (auto* p : getParameters()) p->addListener(&m_paramWatch);
    for (auto& c : m_ccMap) c.store(-1);
    // Preset packs (thousands of presets as text files) before anything reads the preset list.
    loadDefaultPresetPacks();
    loadFavourites();   // by name, so after the packs: the names have to be there to be found
    // The map's parameter vectors for those presets, on a thread of its own: a second of work
    // that only the map needs, and nothing should wait for it to open a window or start playing.
    PresetMap::warmupAsync();
    // The pump runs for the life of the instrument: it carries out the preset changes OSC asks
    // for and finishes a transition that had to wait for an engine. Two atomic reads at 30 Hz.
    m_presetPump.startTimerHz(30);
    // OSC on 9000; a second instance in a DAW simply reports the port as taken.
    m_osc.start(9000, *this, m_gestures);
    // Session recall. The switch lives in the same settings file as the state, so turning it off
    // survives a restart; off also drops the stored state, before JUCE can read it back -- this
    // constructor runs from the holder's, ahead of its reloadPluginState().
    if (auto* settings = standaloneSettings()) {
        m_sessionRecall = settings->getBoolValue("sessionRecall", true);
        if (!m_sessionRecall) settings->removeValue("filterState");
        else startTimer(15000);
    }
}

NoctuaryProcessor::~NoctuaryProcessor()
{
    // The OSC thread first: it calls event() on this object, and the queue it pushes into is
    // declared after it, so it would be destroyed first. Then the warmup thread, which would be
    // executing code this library is about to give back.
    m_osc.stop();
    ambient::PresetMap::shutdown();
    for (auto* p : getParameters()) p->removeListener(&m_paramWatch);
    m_presetPump.stopTimer();
    stopTimer();
    saveSession();
}

/**
 * The standalone's settings, or nothing at all when this is a plugin: there the host owns the
 * state, saves it with the project and hands it back, and a synth that quietly loaded somebody
 * else's last session into a fresh instance would be a bug, not a feature.
 */
juce::PropertySet* NoctuaryProcessor::standaloneSettings()
{
   #if JucePlugin_Build_Standalone
    if (auto* holder = juce::StandalonePluginHolder::getInstance()) return holder->settings.get();
   #endif
    return nullptr;
}

void NoctuaryProcessor::linkTick()
{
    m_link.setEnabled(wrapperType == wrapperType_Standalone && (frame::Settings::of("Noctuary").link() || frame::LinkClock::forcedOn()));
    m_link.tick();
    // The family jam (02.10.2026, Jam.h): the role from the settings.
    m_jam.setRole(frame::Settings::of("Noctuary").jamRole());
    if (const char* logFile = std::getenv("FAMILY_JAM_LOG")) {   // a test aid: what the jam does, whenever it changes
        const juce::String line = m_jam.status() + "; root " + juce::String(live().brainRoot() % 12);
        if (line != m_jamLogged) {
            m_jamLogged = line;
            juce::File(logFile).appendText(juce::Time::getCurrentTime().toString(false, true, true, true) + "  " + line + "\n");
        }
    }
}

juce::String NoctuaryProcessor::cueStatus() const
{
    if (m_cuePortOpen == 0) return frame::Settings::of("Noctuary").cues() ? "no socket" : "off";
    return juce::String(m_cuesSent) + " sent";
}

void NoctuaryProcessor::cueTick()
{
    const frame::Settings& s = frame::Settings::of("Noctuary");
    const int want = s.cues() ? s.cuePort() : 0;
    if (want != m_cuePortOpen) {
        m_cueOut.disconnect();
        m_cuePortOpen = (want > 0 && m_cueOut.connect("127.0.0.1", want)) ? want : 0;
        m_cueBar = m_cueRoot = m_cueScene = -1;
        m_cuesSent = 0;
    }
    if (m_cuePortOpen == 0) return;
    ambient::Engine& e = live();
    if (e.clockRunning()) {
        const int bar = static_cast<int>(std::floor(e.beatPosition() / 4.0));
        if (bar != m_cueBar && m_cueOut.send("/noct/bar", static_cast<juce::int32>(bar))) { m_cueBar = bar; ++m_cuesSent; }
    }
    const int pc = ((e.brainRoot() % 12) + 12) % 12;
    const int tuning = juce::jlimit(0, ambient::NumScaleChoices - 1, static_cast<int>(e.getParam(ParamIdE::SCALE)));
    if (pc * 16 + tuning != m_cueRoot
        && m_cueOut.send("/noct/key", juce::String(ambient::RootNames[pc]) + " " + ambient::ScaleNames[tuning])) {
        m_cueRoot = pc * 16 + tuning;
        ++m_cuesSent;
    }
    if (m_currentProgram != m_cueScene && m_currentProgram >= 0 && m_currentProgram < ambient::numPresets()
        && m_cueOut.send("/noct/scene", juce::String(ambient::preset(m_currentProgram).m_name), 0.35f)) {
        m_cueScene = m_currentProgram;
        ++m_cuesSent;
    }
}

juce::String NoctuaryProcessor::linkStatus() const
{
    if (!m_link.enabled()) return "off";
    const int n = m_link.peers();
    return n == 0 ? "alone in the session" : juce::String(n) + (n == 1 ? " other app; Clock > Source: Host follows it"
                                                                       : " other apps; Clock > Source: Host follows them");
}

bool NoctuaryProcessor::sessionRecallAvailable() { return standaloneSettings() != nullptr; }

void NoctuaryProcessor::setSessionRecall(bool on)
{
    m_sessionRecall = on;
    auto* settings = standaloneSettings();
    if (settings == nullptr) return;
    settings->setValue("sessionRecall", on);
    if (on) { m_savedStateHash = 0; startTimer(15000); saveSession(); }
    else    { stopTimer(); settings->removeValue("filterState"); }
    if (auto* file = dynamic_cast<juce::PropertiesFile*>(settings)) file->saveIfNeeded();
}

/**
 * Writes the state only when it has actually changed, so an instrument left running all night
 * touches the disk once. Hashing the block is cheaper than deciding what counts as a change:
 * every knob, the matrix, the tuning and the loaded files are in there already.
 */
void NoctuaryProcessor::saveSession()
{
    auto* settings = standaloneSettings();
    if (settings == nullptr || !m_sessionRecall) return;
    juce::MemoryBlock data;
    getStateInformation(data);
    if (data.getSize() == 0) return;
    juce::uint32 hash = 2166136261u;                     // FNV-1a over the block
    for (const auto* b = static_cast<const juce::uint8*>(data.getData()), *e = b + data.getSize(); b != e; ++b)
        hash = (hash ^ *b) * 16777619u;
    if (hash == m_savedStateHash) return;
    m_savedStateHash = hash;
    settings->setValue("filterState", data.toBase64Encoding());
    if (auto* file = dynamic_cast<juce::PropertiesFile*>(settings)) file->saveIfNeeded();
}

void NoctuaryProcessor::timerCallback() { saveSession(); }

// ---------------------------------------------------------------- OSC sink + gestures

void NoctuaryProcessor::setParam(ParamIdE id, float value)
{
    if (auto* p = m_apvts.getParameter(paramTable()[static_cast<size_t>(id)].m_key))
        p->setValueNotifyingHost(p->convertTo0to1(value));
}

void NoctuaryProcessor::setParamNormalised(ParamIdE id, float norm)
{
    if (auto* p = m_apvts.getParameter(paramTable()[static_cast<size_t>(id)].m_key))
        p->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, norm));
}

void NoctuaryProcessor::event(const ControlEvent& e)
{
    // Notes to the audio thread, preset changes to the message thread: one is a handful of
    // atomics, the other opens files.
    if (e.m_type == ControlEvent::TypeE::NOTE_ON || e.m_type == ControlEvent::TypeE::NOTE_OFF) m_events.push(e);
    else m_presetEvents.push(e);
}

/**
 * Message thread, from the pump: the preset changes OSC asked for, and the parameters the map
 * left behind when it was switched off.
 */
void NoctuaryProcessor::servePresetRequests()
{
    if (m_mapExit.exchange(false, std::memory_order_acq_rel)
        && juce::Time::getMillisecondCounterHiRes() >= m_mapExitDiscardUntil) {   // not the exit a journey asked for
        for (const ParamDesc& d : paramTable()) {
            if (isMapParam(d.m_id) || isMorphParam(d.m_id) || isMacroParam(d.m_id)) continue;
            if (auto* p = m_apvts.getParameter(d.m_key)) p->setValueNotifyingHost(p->convertTo0to1(live().blendValue(d.m_id)));
        }
    }
    // A program sweep that has come to rest: read the files of the one that is left standing.
    // Held back until it is quiet, so a sweep that is still moving reads nothing at all.
    if (const int want = m_pendingFiles.load(std::memory_order_acquire); want >= 0
        && juce::Time::getMillisecondCounterHiRes() - m_lastProgramAt > 250.0) {
        m_pendingFiles.store(-1, std::memory_order_release);
        if (want == m_currentProgram) loadPresetFiles(want);
    }
    ControlEvent ev;
    while (m_presetEvents.pop(ev)) {
        switch (ev.m_type) {
        case ControlEvent::TypeE::PRESET:       setCurrentProgram(ev.m_a); break;
        case ControlEvent::TypeE::SOUND_PRESET:  applySoundPreset(ev.m_a); break;
        case ControlEvent::TypeE::COSMOS_PRESET: applyCosmosPreset(ev.m_a); break;
        default: break;
        }
    }
}

bool NoctuaryProcessor::setGestureMappings(const juce::String& text)
{
    if (!m_gestures.parseMappings(text.toRawUTF8())) return false;
    m_mappingText = text;
    return true;
}

juce::String NoctuaryProcessor::gestureMappings() const
{
    char buf[4096];
    const int n = m_gestures.writeMappings(buf, sizeof(buf));
    return juce::String(juce::CharPointer_UTF8(buf), static_cast<size_t>(juce::jmax(0, n)));
}

void NoctuaryProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    m_lastSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    m_lastBlockSize = samplesPerBlock;
    for (auto& e : m_engines) {
        if (e == nullptr) continue;
        for (int i = 0; i < NumParams; ++i)
            e->setParam(static_cast<ParamIdE>(i), m_raw[static_cast<size_t>(i)]->load());
        e->prepare(sampleRate, samplesPerBlock);
    }
    m_scratch.setSize(2, samplesPerBlock);
    m_fadeBuf.setSize(2, samplesPerBlock);
    m_planeBuf.setSize(2 * ambient::Engine::NumStems, juce::jmax(1, samplesPerBlock));   // the planes' outputs (02.10.2026)
    for (int c = 0; c < 2 * ambient::Engine::NumStems; ++c) m_planePtr[static_cast<size_t>(c)] = m_planeBuf.getWritePointer(c);
    for (auto& e : m_engines) if (e != nullptr) e->setStemBuffers(nullptr);
    m_planesOn = false;
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    // Both engines have just had their buffers cleared, so a transition that was in flight has
    // nothing left to fade out of: it ends here rather than crossfading from silence.
    m_fading.store(-1, std::memory_order_relaxed);
    m_swapTo.store(-1, std::memory_order_relaxed);
    m_paramTarget.store(-1, std::memory_order_relaxed);
    m_fadePos.store(1.0f, std::memory_order_relaxed);
    m_fadeHead = 0.0f;
}

juce::AudioProcessor::BusesProperties NoctuaryProcessor::busLayout()
{
    BusesProperties b = BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true);
    for (int s = 0; s < ambient::Engine::NumStems; ++s) {
        const juce::String name(ambient::Engine::stemName(s));
        b = b.withOutput(name.substring(0, 1).toUpperCase() + name.substring(1), juce::AudioChannelSet::stereo(), false);
    }
    return b;
}

bool NoctuaryProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono()) return false;
    for (int b = 1; b < layouts.outputBuses.size(); ++b) {   // a plane's bus (02.10.2026): stereo, or off
        const juce::AudioChannelSet& set = layouts.outputBuses.getReference(b);
        if (!set.isDisabled() && set != juce::AudioChannelSet::stereo()) return false;
    }
    return true;
}

void NoctuaryProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    m_noteTap.clear();   // MIDI out (02.10.2026): what the conductors play in this block, written out at its end

    // Which engine is the instrument is decided here and nowhere else, at a block boundary, so
    // that the message thread can build the next preset on the other one without ever writing to
    // an engine that is being rendered.
    if (m_endFade.exchange(false, std::memory_order_acq_rel)) {
        const int f = m_fading.load(std::memory_order_relaxed);
        if (f >= 0) { m_engines[f]->allNotesOff(); m_fading.store(-1, std::memory_order_release); }
    }
    if (const int sw = m_swapTo.load(std::memory_order_acquire); sw >= 0) {
        m_fading.store(m_live.load(std::memory_order_relaxed), std::memory_order_relaxed);
        m_live.store(sw, std::memory_order_release);
        // Where the volume stood at the change, on both sides: the leaving engine's own level, and
        // the parameter as the arriving preset left it. What the player turns from here on is the
        // difference between the two readings of the parameter, and it goes to both engines.
        m_fadeGainFrom = m_engines[m_fading.load(std::memory_order_relaxed)]->getParam(ParamIdE::MASTER_GAIN);
        m_fadeGainBase = m_raw[static_cast<size_t>(ParamIdE::MASTER_GAIN)]->load();
        m_fadePos.store(0.0f, std::memory_order_relaxed);
        m_fadeHead = 0.0f;
        m_paramTarget.store(-1, std::memory_order_release);
        // Cleared LAST. The message thread gives an idle engine back when nothing is fading and
        // no change is on its way; clearing this first opened a window in which both were true
        // while the swap had not happened yet, and it freed the very engine about to be made
        // live. That is a crash a few seconds into every transition.
        m_swapTo.store(-1, std::memory_order_release);
    }

    // Macros are gesture inputs Custom0..7: one knob, several parameters.
    for (int m = 0; m < 8; ++m)
        m_gestures.setInput(static_cast<GestureInputE>(static_cast<int>(GestureInputE::CUSTOM0) + m),
                           m_raw[static_cast<size_t>(static_cast<int>(ParamIdE::MACRO_A) + m)]->load());

    // Gestures write through the host's parameter system, like a MIDI controller would.
    m_gestures.update(buffer.getNumSamples() / getSampleRate(), [this](ParamIdE id, float v) {
        if (auto* p = m_apvts.getParameter(paramTable()[static_cast<size_t>(id)].m_key))
            p->setValueNotifyingHost(p->convertTo0to1(v));
    });
    // Notes from OSC arrive on the audio thread through the queue, where they belong. Preset
    // changes do not: applying one writes two hundred and ninety-three parameters through the
    // host and reads a sample, a wavetable and an impulse response off the disk. That was being
    // done here, in the middle of a block, for every preset an OSC client or the Quest app asked
    // for -- file I/O and allocation on the thread that must not wait for anything. They go to
    // the message thread now (see servePresetRequests).
    ControlEvent ev;
    while (m_events.pop(ev)) {
        switch (ev.m_type) {
        case ControlEvent::TypeE::NOTE_ON:       noteOn(ev.m_a, ev.m_b); break;
        case ControlEvent::TypeE::NOTE_OFF:      noteOff(ev.m_a); break;
        default: break;
        }
    }

    // Leaving the preset map: the sound stays where the map left it, so the gliding blend values
    // become the live parameters. Writing two hundred and ninety-three parameters through the host
    // is not work for the audio thread -- it is exactly what was moved off it for OSC preset
    // changes -- so the audio thread only notices that the map has been switched off and the
    // message thread does the writing a few milliseconds later. The blended values stay where they
    // are in the meantime: switching the map off leaves m_blendCur alone.
    //
    // (The block that used to sit here for the arrival of a travelling preset change is gone with
    // it. Nothing has set morphTarget_ since a preset change became a crossfade of two engines,
    // so it applied a preset from the audio thread that was never on its way.)
    if (m_raw[static_cast<size_t>(ParamIdE::MAP_ACTIVE)]->load() < 0.5f && live().mapActive())
        m_mapExit.store(true, std::memory_order_release);

    {   // Into the instrument -- or, while a transition is being built, into the engine that is
        // about to become it, so that the new preset's values do not also land on the old sound.
        const int pt = m_paramTarget.load(std::memory_order_acquire);
        ambient::Engine& dst = *m_engines[pt >= 0 ? pt : m_live.load(std::memory_order_relaxed)];
        for (int i = 0; i < NumParams; ++i)
            dst.setParam(static_cast<ParamIdE>(i), m_raw[static_cast<size_t>(i)]->load());
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
    // Ableton Link (02.10.2026, LinkClock.h), the standalone only: with other apps in the session its tempo, beat and
    // start/stop reach the engine as a host's clock does -- Clock > Source: Host follows it. Alone in it, nothing changes.
    if (wrapperType == wrapperType_Standalone && m_link.enabled() && m_link.peers() > 0) {
        const frame::LinkClock::Now ln = m_link.capture(static_cast<double>(buffer.getNumSamples()) / getSampleRate(), 4.0);
        live().setHostClock(ln.m_bpm, ln.m_beat, ln.m_playing);
    }

    // Route: the engine walks it and moves the cursor; mirror the cursor (and the switches the
    // route flips) into the host parameters so the GUI and automation see it.
    {
        float rx, ry, rr;
        const bool wasActive = m_raw[static_cast<size_t>(ParamIdE::ROUTE_ACTIVE)]->load() >= 0.5f;
        live().routeStep(buffer.getNumSamples() / getSampleRate(), rx, ry, rr);
        // Telling the host about a parameter is what a plugin does when it moves one itself, and
        // it is how the map cursor reaches the display and the automation lane. But a route walks
        // it continuously, so at a 64-sample buffer this was five parameter changes seven hundred
        // and fifty times a second -- for a cursor that crosses the plane over minutes. Thirty
        // times a second is finer than any lane records and finer than any eye sees.
        m_routeMirrorLeft -= buffer.getNumSamples();
        if (wasActive && m_routeMirrorLeft <= 0) {
            m_routeMirrorLeft = static_cast<int>(getSampleRate() / 30.0);
            auto mirror = [this](ParamIdE id) {
                if (auto* p = m_apvts.getParameter(paramTable()[static_cast<size_t>(id)].m_key)) {
                    const float v = live().getParam(id);
                    if (std::fabs(p->convertFrom0to1(p->getValue()) - v) > 1e-6f) p->setValueNotifyingHost(p->convertTo0to1(v));
                }
            };
            mirror(ParamIdE::MAP_X); mirror(ParamIdE::MAP_Y); mirror(ParamIdE::MAP_RADIUS); mirror(ParamIdE::MAP_ACTIVE); mirror(ParamIdE::ROUTE_ACTIVE);
        }
    }

    // Set timeline: playback feeds parameters (through the host) and notes; recording logs what
    // changed since the last block plus the notes, at the block's time.
    const double blockSeconds = buffer.getNumSamples() / getSampleRate();
    if (m_setPlaying.load()) {
        m_setPlayBusy.store(true, std::memory_order_release);
        const double t0 = m_setClock, t1 = m_setClock + blockSeconds;
        m_setPlay.step(t0, t1, [this](const TimelineEvent& e) {
            switch (e.m_type) {
            case TimelineEvent::TypeE::PARAM:
                if (auto* p = m_apvts.getParameter(paramTable()[static_cast<size_t>(e.m_a)].m_key)) p->setValueNotifyingHost(p->convertTo0to1(e.m_v));
                live().setParam(static_cast<ParamIdE>(e.m_a), e.m_v);
                break;
            case TimelineEvent::TypeE::NOTE_ON:  noteOn(e.m_a, e.m_v); break;
            case TimelineEvent::TypeE::NOTE_OFF: noteOff(e.m_a); break;
            }
        });
        m_setClock = t1;
        m_setTime.store(m_setClock);
        if (m_setPlay.finished() && m_setClock > m_setPlay.length() + 1.0) m_setPlaying.store(false);
        m_setPlayBusy.store(false, std::memory_order_release);
    }
    if (m_setRecording.load()) {
        m_setRecBusy.store(true, std::memory_order_release);
        for (int i = 0; i < NumParams; ++i) {
            const float v = m_raw[static_cast<size_t>(i)]->load();
            if (v != m_setLast[i]) { m_setRec.add({ m_setClock, TimelineEvent::TypeE::PARAM, i, v }); m_setLast[i] = v; }
        }
        for (const auto meta : midi) {
            const auto m = meta.getMessage();
            if (m.isNoteOn()) m_setRec.add({ m_setClock + meta.samplePosition / getSampleRate(), TimelineEvent::TypeE::NOTE_ON, m.getNoteNumber(), m.getFloatVelocity() });
            else if (m.isNoteOff()) m_setRec.add({ m_setClock + meta.samplePosition / getSampleRate(), TimelineEvent::TypeE::NOTE_OFF, m.getNoteNumber(), 0.0f });
        }
        m_setClock += blockSeconds;
        m_setTime.store(m_setClock);
        m_setRecBusy.store(false, std::memory_order_release);
    }

    const bool mpe = m_raw[static_cast<size_t>(ParamIdE::MPE_ON)]->load() >= 0.5f;
    for (const auto meta : midi) {
        const auto m = meta.getMessage();
        const int ch = juce::jlimit(1, 16, m.getChannel()) - 1;
        // With MPE every finger has its own channel; without it, expression addresses every
        // sounding voice, which is what channel pressure and the wheel mean on a plain keyboard.
        const int expressed = mpe ? m_mpeNote[ch] : -1;
        if (m.isNoteOn())            { if (mpe) m_mpeNote[ch] = m.getNoteNumber(); noteOn(m.getNoteNumber(), m.getFloatVelocity()); }
        else if (m.isNoteOff())      { if (mpe && m_mpeNote[ch] == m.getNoteNumber()) m_mpeNote[ch] = -1; noteOff(m.getNoteNumber()); }
        else if (m.isPitchWheel())   live().setBend(expressed, (m.getPitchWheelValue() - 8192) / 8192.0f);
        else if (m.isChannelPressure()) live().setPressure(expressed, m.getChannelPressureValue() / 127.0f);
        else if (m.isAftertouch())   live().setPressure(m.getNoteNumber(), m.getAfterTouchValue() / 127.0f);
        else if (m.isAllNotesOff() || m.isAllSoundOff()) allNotesOff();
        else if (m.isMidiClock()) {   // 24 a quarter; the interval between two carries the tempo
            const double t = m_clockSamples + meta.samplePosition;
            live().midiClockTick(m_lastClockSample >= 0.0 ? (t - m_lastClockSample) / getSampleRate() : 0.0);
            m_lastClockSample = t;
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
            const int learn = m_learnTarget.exchange(-1);
            if (learn >= 0) {
                for (auto& c : m_ccMap) if (c.load() == learn) c.store(-1);   // one controller per parameter
                m_ccMap[static_cast<size_t>(cc)].store(learn);
            }
            const int target = m_ccMap[static_cast<size_t>(cc)].load();
            if (target >= 0)
                if (auto* p = m_apvts.getParameter(paramTable()[static_cast<size_t>(target)].m_key))
                    p->setValueNotifyingHost(static_cast<float>(m.getControllerValue()) / 127.0f);
        }
    }
    midi.clear();
    m_clockSamples += buffer.getNumSamples();

    const int n = buffer.getNumSamples();
    if (m_scratch.getNumSamples() < n) m_scratch.setSize(2, n, false, false, true);
    if (m_fadeBuf.getNumSamples() < n) m_fadeBuf.setSize(2, n, false, false, true);
    {   // The planes' outputs (02.10.2026): the instrument's engine writes them while the host has one of their buses on.
        bool want = false;
        for (int b = 1; b < getBusCount(false) && !want; ++b)
            if (const auto* bus = getBus(false, b)) want = bus->isEnabled();
        want = want && n <= m_planeBuf.getNumSamples();
        for (auto& e : m_engines) if (e != nullptr) e->setStemBuffers(nullptr);
        if (want) live().setStemBuffers(m_planePtr.data());
        m_planesOn = want;
    }
    // Always into m_scratch, so the two-engine mix and the mono fold-down read the same place.
    // The family jam (02.10.2026, Jam.h): a follower's conductor holds the leader's root and its filters take the
    // leader's energy; a leader says its root and its tuning when they change (its sections are scenes, not breaks).
    {
        ambient::Engine& e = live();
        if (m_jam.role() == frame::Settings::JamRoleE::FOLLOW) {
            const frame::JamState js = m_jam.stateAt(-1.0);
            e.setJam(js.m_root, js.m_root >= 0 ? js.m_energy : -1.0f);
        } else {
            e.setJam(-1, -1.0f);
            if (m_jam.role() == frame::Settings::JamRoleE::LEAD) {
                const int pc = ((e.brainRoot() % 12) + 12) % 12;
                const int tuning = juce::jlimit(0, ambient::NumScaleChoices - 1, static_cast<int>(e.getParam(ParamIdE::SCALE)));
                if (pc * 16 + tuning != m_jamLeadKey) {
                    m_jamLeadKey = pc * 16 + tuning;
                    m_jam.postKey(pc, ambient::ScaleNames[tuning]);
                }
            } else {
                m_jamLeadKey = -1;
            }
        }
    }
    live().process(m_scratch.getWritePointer(0), m_scratch.getWritePointer(1), n);
    if (m_fading >= 0) {
        // A preset transition: the preset that is leaving is still playing, on its own engine,
        // with every parameter it had -- nothing in it moves. It goes out under a cosine and the
        // new one comes in under a sine, so the sum keeps its power the whole way and there is
        // no dip in the middle and no moment where anything switches. A parameter morph between
        // two unrelated presets cannot do this: its ninety-three switches all flipped at half way
        // and every source restarted, and that is what it sounded like -- a cut in the middle.
        const float secs = std::max(m_morphSelectSeconds.load(std::memory_order_relaxed), 0.25f);
        const float t0 = m_fadePos;
        // The ramp does not start before there is something to ramp in. Measured on the incoming
        // engine's own output, before any gain: -60 dBFS is audible, and a brain that has not
        // played its first note yet is not. The head start is capped, so a preset that is silent
        // by design still arrives.
        bool ramping = t0 > 0.0f;
        if (!ramping) {
            float e = 0.0f;
            for (int ch = 0; ch < 2; ++ch) {
                const float* s = m_scratch.getReadPointer(ch);
                for (int i = 0; i < n; ++i) e += s[i] * s[i];
            }
            m_fadeHead += static_cast<float>(n / m_sampleRate);
            ramping = std::sqrt(e / static_cast<float>(2 * n)) > 1e-3f || m_fadeHead >= FadeHeadStart;
        }
        const float t1 = ramping ? std::min(1.0f, t0 + static_cast<float>(n) / static_cast<float>(m_sampleRate * secs)) : 0.0f;
        // The volume reaches the leaving preset too. Everything else of it stands still on
        // purpose -- it is a sound on its way out -- but the volume is the listener's, not the
        // preset's, and a journey fades for up to a minute and a half after a head start of up to
        // eight seconds in which the leaving engine is nearly all there is to hear. So the knob did
        // nothing at every step and half of what it should for a minute after (Rene, 13.09.2026).
        // As a difference, not a value: the arriving preset's own level would make the leaving one
        // jump the moment the fade began.
        {
            const float moved = m_raw[static_cast<size_t>(ParamIdE::MASTER_GAIN)]->load() - m_fadeGainBase;
            m_engines[m_fading]->setParam(ParamIdE::MASTER_GAIN, juce::jlimit(-40.0f, 12.0f, m_fadeGainFrom + moved));
        }
        m_engines[m_fading]->process(m_fadeBuf.getWritePointer(0), m_fadeBuf.getWritePointer(1), n);
        const float inA = std::sin(juce::MathConstants<float>::halfPi * t0), inB = std::sin(juce::MathConstants<float>::halfPi * t1);
        const float outA = std::cos(juce::MathConstants<float>::halfPi * t0), outB = std::cos(juce::MathConstants<float>::halfPi * t1);
        for (int ch = 0; ch < 2; ++ch) {
            m_scratch.applyGainRamp(ch, 0, n, inA, inB);
            m_scratch.addFromWithRamp(ch, 0, m_fadeBuf.getReadPointer(ch), n, outA, outB);
        }
        m_fadePos = t1;
        if (t1 >= 1.0f) {   // arrived: the old one is silent by now and goes to sleep
            m_engines[m_fading]->allNotesOff();
            m_engines[m_fading]->reset();
            m_fading = -1;
        }
    }
    // MIDI out (02.10.2026): the conductors' notes of this block -- channel 1 the first, 2 the second, 3 the near events.
    for (int i = 0; i < m_noteTap.m_count; ++i) {
        const ambient::NoteTap::Note& nt = m_noteTap.m_notes[i];
        const int at = juce::jlimit(0, juce::jmax(0, n - 1), nt.m_offset);
        if (nt.m_velocity == 0) midi.addEvent(juce::MidiMessage::noteOff(nt.m_channel, nt.m_pitch), at);
        else midi.addEvent(juce::MidiMessage::noteOn(nt.m_channel, nt.m_pitch, static_cast<juce::uint8>(nt.m_velocity)), at);
    }
    // The main bus only: with the planes' outputs on, the channels after it are theirs (02.10.2026).
    const auto* mainBus = getBus(false, 0);
    const int mainChannels = mainBus != nullptr ? mainBus->getNumberOfChannels() : buffer.getNumChannels();
    if (mainChannels >= 2) {
        buffer.copyFrom(0, 0, m_scratch, 0, 0, n);
        buffer.copyFrom(1, 0, m_scratch, 1, 0, n);
    } else if (mainChannels == 1) {
        buffer.copyFrom(0, 0, m_scratch, 0, 0, n);
        buffer.addFrom(0, 0, m_scratch, 1, 0, n);
        buffer.applyGain(0, 0, n, 0.5f);
    }
    for (int ch = mainChannels; ch < buffer.getNumChannels(); ++ch) buffer.clear(ch, 0, n);
    if (m_planesOn)
        for (int b = 1; b < getBusCount(false) && b - 1 < ambient::Engine::NumStems; ++b) {
            const auto* bus = getBus(false, b);
            if (bus == nullptr || !bus->isEnabled()) continue;
            auto out = getBusBuffer(buffer, false, b);
            if (out.getNumChannels() < 2) continue;
            out.copyFrom(0, 0, m_planeBuf, 2 * (b - 1), 0, n);
            out.copyFrom(1, 0, m_planeBuf, 2 * (b - 1) + 1, 0, n);
        }

    if (m_recording.load(std::memory_order_relaxed)) {
        const juce::ScopedTryLock sl(m_recordLock);
        if (sl.isLocked() && m_recordWriter != nullptr) {
            const float* chans[2] = { buffer.getReadPointer(0), buffer.getNumChannels() > 1 ? buffer.getReadPointer(1) : buffer.getReadPointer(0) };
            m_recordWriter->write(chans, n);
            m_recordedSamples.fetch_add(n, std::memory_order_relaxed);
        }
    }
    // Muted: everything above has run -- the engine, its meters and taps, the recorder -- and
    // only what reaches the device is silenced. For the manual export, which plays a chord for
    // its pictures, and for any start meant as a test rather than as music (AMBIENT_MUTE=1).
    if (m_muteOutput) buffer.clear();
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
    m_recordThread.startThread();
    {
        const juce::ScopedLock sl(m_recordLock);
        m_recordWriter = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(writer.release(), m_recordThread, 1 << 18);
        m_recordedSamples.store(0);
    }
    m_recording.store(true);
    return true;
}

void NoctuaryProcessor::stopRecording()
{
    m_recording.store(false);
    {
        const juce::ScopedLock sl(m_recordLock);
        m_recordWriter.reset();
    }
    m_recordThread.stopThread(2000);
}

int NoctuaryProcessor::getNumPrograms() { return numPresets(); }

const juce::String NoctuaryProcessor::getProgramName(int index)
{
    return (index >= 0 && index < numPresets()) ? juce::String(preset(index).m_name) : juce::String();
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
            for (int k = 0; k < ambient::Slots; ++k) {
                const juce::String p = k < parts.size() ? parts[k].trim() : juce::String();
                const juce::File f = p.isNotEmpty() ? onDisk(p) : juce::File();
                if (f != juce::File()) loadTextureFile(k, f);
                else { target().clearTexture(k); m_textureFile[k] = juce::File(); }
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

void NoctuaryProcessor::applyScoped(const Preset& pr, PresetScopeE scope)
{
    applyPreset(pr, [this](ParamIdE id, float v) {
        if (auto* p = m_apvts.getParameter(paramTable()[static_cast<size_t>(id)].m_key))
            p->setValueNotifyingHost(p->convertTo0to1(v));
    }, scope);
    // The matrix rows and the envelope shapes are data, not parameters, so they do not travel
    // through the parameter tree: the engine takes them straight from the preset.
    if (scope != PresetScopeE::COSMOS && scope != PresetScopeE::NEAR_LAYER) target().applyPresetModulation(pr);
}

void NoctuaryProcessor::setCurrentProgram(int index)
{
    if (index < 0 || index >= numPresets()) return;
    m_currentProgram = index;
    m_soundIndex = index;
    m_soundName = preset(index).m_name;
    m_zIndex = -1; m_strikeIndex = -1;
    m_cosmosIndex = -1;   // the program brought its own Cosmos layer
    applyScoped(preset(index), PresetScopeE::FULL);
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
    const bool burst = now - m_lastProgramAt < 300.0;
    m_lastProgramAt = now;
    if (burst) { m_pendingFiles.store(index, std::memory_order_release); return; }
    m_pendingFiles.store(-1, std::memory_order_release);
    loadPresetFiles(index);
}

void NoctuaryProcessor::applySoundPreset(int index)
{
    if (index < 0 || index >= numPresets()) return;
    m_soundIndex = index;
    // ...and the program number with it. Only setCurrentProgram used to move this, which is the
    // DAW's way in -- so picking a preset in the box or in the browser left it at 0, and every
    // view that asks "what is playing" (the map's ring, the host's program display) answered
    // "Init" however long you had been playing something else.
    m_currentProgram = index;
    m_soundName = preset(index).m_name;
    // A sound preset brings its own Z-plane and its own Strike with it, so whatever layer preset
    // was picked before is no longer what is loaded. Cleared rather than left standing: a box
    // that names a filter the sound preset has just overwritten is worse than an empty one.
    m_zIndex = -1; m_strikeIndex = -1;
    applyScoped(preset(index), PresetScopeE::SOUND);
    loadPresetFiles(index);
    applyLevelMatch(index);
    applyNearAuto(index);
}

/**
 * The foreground a pack preset brings, while Auto is on: the artist's table by the preset's
 * name (nearAutoPick), the near preset applied like one chosen by hand, and its Every scaled by
 * the class's factor. A built-in, or a pack without a table, leaves the foreground as it is.
 */
void NoctuaryProcessor::applyNearAuto(int soundIndex)
{
    if (soundIndex < 0 || soundIndex >= numPresets()) return;
    if (live().getParam(ParamIdE::FORE_AUTO) < 0.5f) return;
    const int pack = presetPack(soundIndex);
    if (pack < 0) return;
    float factor = 1.0f;
    const int pick = nearAutoPick(presetPackName(pack), preset(soundIndex).m_name, factor);
    if (pick < 0) return;
    applyNearPreset(pick);
    if (pick > 0) {
        // The near preset's own Every, read where applyNearPreset has just written it: the
        // parameter tree. The engine gets it a block later, so reading the engine here scaled the
        // Every that was there BEFORE -- the default on a fresh instrument, and after that the
        // result of the last scaling, compounding with every preset change until the ceiling
        // (13.09.2026: Whistler's 200 s at a factor of 1.18 came out as 142 instead of 236).
        const float own = m_raw[static_cast<size_t>(ParamIdE::FORE_RATE)]->load();
        setParam(ParamIdE::FORE_RATE, juce::jlimit(10.0f, 900.0f, own * factor));
    }
}

/**
 * The loudness of every preset was measured from a twelve-second render (Tools/preset_map.py
 * for the built-ins, measure_packs.py for the library). A preset that was never measured has
 * none, and is then left alone rather than guessed at.
 * @param index  the preset just loaded, whose measured loudness (ambient::presetMeta) is matched
 */
void NoctuaryProcessor::applyLevelMatch(int index)
{
    if (!m_levelMatch || index < 0 || index >= numPresetMeta()) return;
    const float loud = presetMeta(index).m_loudDb;
    if (loud >= -0.5f || loud < -80.0f) return;         // 0 means "never measured"
    constexpr float Target = -20.5f;                   // the measured median of the built-in presets
    if (auto* p = m_apvts.getParameter(paramDesc(ParamIdE::MASTER_GAIN).m_key)) {
        const float now = live().getParam(ParamIdE::MASTER_GAIN);
        const float want = juce::jlimit(-40.0f, 12.0f, now + juce::jlimit(-12.0f, 12.0f, Target - loud));
        p->setValueNotifyingHost(p->convertTo0to1(want));
    }
}

void NoctuaryProcessor::applyCosmosPreset(int index)
{
    if (index < 0 || index >= numCosmosPresets()) return;
    m_cosmosIndex = index;
    m_cosmosName = cosmosPreset(index).m_name;
    applyScoped(cosmosPreset(index), PresetScopeE::COSMOS);
}

void NoctuaryProcessor::applyZPreset(int index)
{
    if (index < 0 || index >= numZPresets()) return;
    m_zIndex = index;
    applyScoped(zPreset(index), PresetScopeE::Z_PLANE);
}

void NoctuaryProcessor::applyStrikePreset(int index)
{
    if (index < 0 || index >= numStrikePresets()) return;
    m_strikeIndex = index;
    applyScoped(strikePreset(index), PresetScopeE::STRIKE);
}

void NoctuaryProcessor::applyNearPreset(int index)
{
    if (index < 0 || index >= numNearPresets()) return;
    m_nearIndex = index;
    const Preset& pr = nearPreset(index);
    m_nearName = pr.m_name;
    applyScoped(pr, PresetScopeE::NEAR_LAYER);
    // The preset's clip, named relative to the library's root, or none: a preset that names no
    // clip takes the one that was loaded away, so the flute preset after the radio preset does
    // not find a voice in its slot.
    if (pr.m_texture != nullptr && *pr.m_texture != 0) {
        const std::string got = ambient::resolveLibraryFile(pr.m_texture);
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
    m_nearClipFile = file;
    m_nearClipCount = 1;
    return true;
}

bool NoctuaryProcessor::loadNearClipFolder(const juce::File& dir)
{
    // A folder of recordings: up to NearPoolMax of them, sorted by name so the same folder gives
    // the same pool, each kept to twenty seconds -- these are phrases, not beds. Which of them an
    // event plays is the engine's draw.
    juce::Array<juce::File> all = dir.findChildFiles(juce::File::findFiles, false, "*.flac;*.wav;*.aif;*.aiff;*.ogg;*.mp3");
    all.sort();
    // More than the pool holds: every k-th of them rather than the first forty-eight, so a folder
    // of twelve episodes' phrases is heard from all twelve and not from the first five.
    juce::Array<juce::File> files;
    if (all.size() <= NearPoolMax) files = all;
    else for (int i = 0; i < NearPoolMax; ++i) files.add(all[static_cast<int>(std::lround(i * (all.size() - 1.0) / (NearPoolMax - 1.0)))]);
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
    m_nearClipFile = dir;
    m_nearClipCount = n;
    return true;
}

void NoctuaryProcessor::clearNearClip()
{
    target().clearNearTexture();
    m_nearClipFile = juce::File();
    m_nearClipCount = 0;
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
    m_scalaText = text;
    m_userScaleName = displayName.isNotEmpty() ? displayName : juce::String(s.m_name);
    if (auto* p = m_apvts.getParameter("scale")) {
        const float norm = p->convertTo0to1(static_cast<float>(UserScaleIndex));
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

/**
 * The same read, both channels kept. A texture slot plays the recording's own image now, so the
 * fold to mono that readMono does would throw away what it is there to play; readMono stays for
 * the wavetable loader, which really does want one signal.
 */
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
    if (slot < 0 || slot >= ambient::Slots) return false;
    std::vector<float> l, r; double rate = 0.0;
    if (!readStereo(file, l, r, rate)) return false;
    const double base = baseHzFromName(file.getFileName().toRawUTF8());   // "_A3" suffix from TextureGen
    target().setTexture(slot, l.data(), r.empty() ? nullptr : r.data(), static_cast<int>(l.size()), rate,
                       base > 0.0 ? base : 261.6256, ambient::loopFromName(file.getFileName().toRawUTF8()));
    m_textureFile[slot] = file;
    return true;
}

bool NoctuaryProcessor::loadTextureFile(const juce::File& file)
{
    std::vector<float> l, r; double rate = 0.0;
    if (!readStereo(file, l, r, rate)) return false;
    const double base = baseHzFromName(file.getFileName().toRawUTF8());
    target().setTexture(l.data(), r.empty() ? nullptr : r.data(), static_cast<int>(l.size()), rate,
                       base > 0.0 ? base : 261.6256, ambient::loopFromName(file.getFileName().toRawUTF8()));
    for (auto& f : m_textureFile) f = file;
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
    if (second) { target().setImpulseB(L, R, n, reader->sampleRate); m_impulseBFile = file; }
    else        { target().setImpulse(L, R, n, reader->sampleRate);  m_impulseFile = file; }
    return true;
}

void NoctuaryProcessor::clearImpulseB()
{
    target().clearImpulseB();
    m_impulseBFile = juce::File();
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
    m_wavetableFile = file;
    return true;
}

void NoctuaryProcessor::startSetRecording()
{
    m_setPlaying.store(false);
    m_setRec.clear();
    // All the room it will ever need, taken here on the message thread: from now on the audio
    // thread only writes into it, and it must not allocate while doing so. A quarter of a million
    // events is hours of playing -- past that the recording stops growing instead of stopping the
    // audio.
    m_setRec.reserve(1u << 18);
    // The starting state goes in at t = 0 so playback begins from the same sound.
    for (int i = 0; i < NumParams; ++i) {
        const float v = m_raw[static_cast<size_t>(i)]->load();
        m_setLast[i] = v;
        m_setRec.add({ 0.0, TimelineEvent::TypeE::PARAM, i, v });
    }
    m_setClock = 0.0;
    m_setTime.store(0.0);
    m_setRecording.store(true);
}

bool NoctuaryProcessor::stopSetRecording(const juce::File& saveTo)
{
    // Stop, then wait for the audio thread to finish the block it may be writing into the
    // timeline. Without this the message thread walked the event list while the audio thread was
    // still appending to it -- and if the vector had grown at that moment, it walked freed memory.
    m_setRecording.store(false, std::memory_order_release);
    for (int spin = 0; spin < 2000000 && m_setRecBusy.load(std::memory_order_acquire); ++spin) { }
    if (saveTo == juce::File()) return true;
    return m_setRec.save(saveTo.getFullPathName().toRawUTF8());
}

bool NoctuaryProcessor::playSetFile(const juce::File& file)
{
    m_setPlaying.store(false);
    m_setRecording.store(false);
    // The audio thread may be inside step() over the very events load() is about to replace.
    for (int spin = 0; spin < 2000000 && m_setPlayBusy.load(std::memory_order_acquire); ++spin) { }
    if (!m_setPlay.load(file.getFullPathName().toRawUTF8())) return false;
    m_setPlay.seek(0.0);
    m_setClock = 0.0;
    m_setTime.store(0.0);
    m_setPlaying.store(true);
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
    if (xml == nullptr || !xml->hasTagName(m_apvts.state.getType())) return false;
    juce::MemoryBlock block;
    copyXmlToBinary(*xml, block);
    setStateInformation(block.getData(), static_cast<int>(block.getSize()));
    return true;
}

/**
 * Choosing a preset as a journey rather than a cut: the old one plays on while the new one comes
 * up under it, and the map draws the crossing from one to the other while it happens.
 */
void NoctuaryProcessor::selectPreset(int index, bool viaMorph)
{
    if (index < 0 || index >= numPresets()) return;
    if (!viaMorph || !m_morphOnSelect) { m_endFade.store(true, std::memory_order_release); setCurrentProgram(index); return; }
    // A transition, not a morph. The preset that is playing keeps playing, untouched, on the
    // engine it is on; the new one is built on the other engine and only then made the
    // instrument, and the two are crossfaded in processBlock over m_morphSelectSeconds. Nothing is
    // interpolated, so nothing has to be interpolable: the two presets may share nothing at all
    // and still meet in the air.
    //
    // Both engines are busy while a transition is running -- one playing, one leaving -- so a
    // change asked for mid-flight ends the running one and waits for the audio thread to let go.
    // It is served a few milliseconds later, by the pump. What must never happen is what this
    // used to do: reset an engine, hand it notes and switch it live from the message thread while
    // the audio thread was rendering it.
    m_pendingPreset = index;
    if (m_fading.load(std::memory_order_acquire) >= 0) m_endFade.store(true, std::memory_order_release);
    servePendingPreset();
}

// ---------------------------------------------------------------- journeys

bool NoctuaryProcessor::startJourney(const juce::File& file)
{
    ambient::Journey j;
    if (!j.load(file.getFullPathName().toRawUTF8()) || j.m_steps.empty()) return false;
    if (j.m_name.empty()) j.m_name = file.getFileNameWithoutExtension().toStdString();
    return startJourney(j);
}

bool NoctuaryProcessor::startJourney(const ambient::Journey& j)
{
    if (j.m_steps.empty()) return false;
    m_journey = j;
    // A journey is presets in a row, and Morph is the instrument held between two chosen ones:
    // with both on, every preset the journey loaded was overruled by the blend and the journey
    // appeared to do nothing. Starting one means playing its presets, so Morph lets go.
    setParam(ParamIdE::MORPH_ACTIVE, 0.0f);
    // The map blend is the same kind of hold, measured the same day: the engine plays the blend of
    // the presets around the cursor and nothing the journey loads. A route plays through the map
    // and switches it back on, so the route goes first. Leaving the map normally writes its blend
    // into the parameters so the sound stays where it was -- here that write would land over the
    // journey's first preset, so it is let go of instead, for a few seconds only, lest a later
    // exit by hand lose its blend to a flag that was set for this one.
    if (m_raw[static_cast<size_t>(ParamIdE::MAP_ACTIVE)]->load() >= 0.5f || m_raw[static_cast<size_t>(ParamIdE::ROUTE_ACTIVE)]->load() >= 0.5f) {
        m_mapExitDiscardUntil = juce::Time::getMillisecondCounterHiRes() + 3000.0;
        setParam(ParamIdE::ROUTE_ACTIVE, 0.0f);
        setParam(ParamIdE::MAP_ACTIVE, 0.0f);
    }
    // Seeded from the clock: the same journey runs differently every evening. A render that
    // wants it repeatable seeds the player itself (ambient_render --journey-seed).
    m_journeyPlayer.start(m_journey, static_cast<uint64_t>(juce::Time::currentTimeMillis()) | 1ull, 0);
    m_journeyLastTick = 0.0;
    journeyTick();   // the first step begins now
    return true;
}

void NoctuaryProcessor::stopJourney() { m_journeyPlayer.stop(); }

void NoctuaryProcessor::skipJourneyTo(int step)
{
    if (!m_journeyPlayer.running()) return;
    const ambient::Journey j = m_journeyPlayer.journey();
    m_journeyPlayer.start(j, static_cast<uint64_t>(juce::Time::currentTimeMillis()) | 1ull, step);
    m_journeyLastTick = 0.0;
    journeyTick();   // that step begins now
}

juce::String NoctuaryProcessor::journeyStatus() const
{
    if (!m_journeyPlayer.running()) return {};
    const int n = static_cast<int>(m_journey.m_steps.size());
    return juce::String(m_journey.m_name) + "  " + juce::String(m_journeyPlayer.step() + 1) + "/" + juce::String(n)
         + "  " + juce::String(ambient::Journey::timeText(std::max(0.0, m_journeyPlayer.remaining())));
}

void NoctuaryProcessor::journeyAddCurrent(double dwellLo, double dwellHi, double fadeLo, double fadeHi)
{
    if (m_soundIndex < 0 || m_soundIndex >= numPresets()) return;
    ambient::JourneyStep st;
    st.m_preset = preset(m_soundIndex).m_name;
    st.m_dwellLo = dwellLo; st.m_dwellHi = dwellHi; st.m_fadeLo = fadeLo; st.m_fadeHi = fadeHi;
    m_journey.m_steps.push_back(st);
}

bool NoctuaryProcessor::saveJourney(const juce::File& file)
{
    if (m_journey.m_steps.empty()) return false;
    if (m_journey.m_name.empty()) m_journey.m_name = file.getFileNameWithoutExtension().toStdString();
    file.getParentDirectory().createDirectory();
    return m_journey.save(file.getFullPathName().toRawUTF8());
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
    m_favourites.clear();
    m_favouriteNames.clear();
    m_favouritesFirst = false;
    const juce::File f = favouritesFile();
    if (!f.existsAsFile()) return;
    // Name to index once, not a scan of fourteen thousand names per line. The first preset of a
    // name wins, which is what the preset boxes do with a name as well.
    std::unordered_map<std::string, int> index;
    for (int i = 0; i < numPresets(); ++i) index.emplace(preset(i).m_name, i);
    juce::StringArray lines;
    f.readLines(lines);
    for (const juce::String& raw : lines) {
        const juce::String line = raw.trim();
        if (line.isEmpty() || line.startsWith("#")) continue;
        if (line.startsWithIgnoreCase("first ")) { m_favouritesFirst = line.substring(6).trim().equalsIgnoreCase("on"); continue; }
        m_favouriteNames.addIfNotAlreadyThere(line);
        const auto it = index.find(line.toStdString());
        if (it != index.end()) m_favourites.setBit(it->second, true);
    }
}

void NoctuaryProcessor::saveFavourites() const
{
    juce::String text;
    text << "# Noctuary favourites: one preset name a line. A name whose preset is not installed is kept.\n"
         << "first " << (m_favouritesFirst ? "on" : "off") << "\n";
    for (const juce::String& n : m_favouriteNames) text << n << "\n";
    favouritesFile().replaceWithText(text);
}

void NoctuaryProcessor::setFavourite(int index, bool on)
{
    if (index < 0 || index >= numPresets()) return;
    m_favourites.setBit(index, on);
    const juce::String name(preset(index).m_name);
    if (on) m_favouriteNames.addIfNotAlreadyThere(name);
    else    m_favouriteNames.removeString(name);
    saveFavourites();
}

juce::File NoctuaryProcessor::userJourneyFolder()
{
    const juce::File dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Noctuary").getChildFile("Journeys");
    dir.createDirectory();
    return dir;
}

/**
 * The user's own first, then the templates wherever the library lies (the installer's folders,
 * the source tree's Library): every *.journey, each folder once, sorted by name.
 */
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

/**
 * The pump's tick: the seconds since the last, and the step the player hands over when one
 * begins -- the preset by name, brought up over the drawn fade, and the foreground it asks for.
 */
void NoctuaryProcessor::journeyTick()
{
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    if (m_journeyLastTick <= 0.0) m_journeyLastTick = now;
    const double dt = juce::jlimit(0.0, 5.0, now - m_journeyLastTick);
    m_journeyLastTick = now;
    if (!m_journeyPlayer.running()) return;
    ambient::JourneyStep st;
    double fade = 30.0;
    if (!m_journeyPlayer.advance(dt, st, fade)) return;
    int index = -1;
    for (int i = 0; i < numPresets(); ++i) if (st.m_preset == preset(i).m_name) { index = i; break; }
    if (index < 0) return;   // a name the library no longer has: nothing changes until the next step
    // near=keep pins what is playing (Auto off) before the sound preset could bring its own;
    // a name pins that one; nothing, or auto, leaves it to the sound preset's Auto.
    if (st.m_nearPreset == "keep") setNearAuto(false);
    setMorphSelectSeconds(static_cast<float>(fade));
    selectPreset(index, true);
    if (!st.m_nearPreset.empty() && st.m_nearPreset != "auto" && st.m_nearPreset != "keep")
        for (int i = 0; i < numNearPresets(); ++i)
            if (st.m_nearPreset == nearPreset(i).m_name) { setNearAuto(false); applyNearPreset(i); break; }
}

/**
 * Message thread: the engine at `i`, built and prepared if it is not there. Allocating a hundred
 * megabytes and generating a room impulse is fine here and nowhere near the audio thread.
 */
ambient::Engine& NoctuaryProcessor::ensureEngine(int i)
{
    if (m_engines[i] == nullptr) {
        auto e = std::make_unique<ambient::Engine>();
        for (int k = 0; k < NumParams; ++k)
            e->setParam(static_cast<ParamIdE>(k), m_raw[static_cast<size_t>(k)]->load());
        e->prepare(m_lastSampleRate, m_lastBlockSize);
        e->setNoteTap(&m_noteTap);   // MIDI out (02.10.2026)
        m_engines[i] = std::move(e);
        carryUserData(*m_engines[i]);
    }
    return *m_engines[i];
}

/**
 * What the player loaded by hand, into an engine that has just been built. A preset brings its own
 * sample, wavetable and impulse and overwrites these a moment later; what it does NOT bring is a
 * Scala scale a player tuned the instrument to, or a wavetable, clip or room they opened
 * themselves. Those live in the engine and nowhere else, and a fresh engine starts without them --
 * so a preset change silently retuned the instrument to twelve-tone equal temperament and put the
 * built-in table back. Copied from the engine that is playing, not read from disk again.
 */
void NoctuaryProcessor::carryUserData(ambient::Engine& e)
{
    ambient::Engine& from = live();
    if (m_scalaText.isNotEmpty()) {
        FixedScale sc;
        if (parseScala(m_scalaText.toRawUTF8(), sc)) e.setUserScale(sc);
    }
    if (const ambient::Wavetable* wt = from.userWavetable()) e.setUserWavetable(*wt);
    if (const ambient::CycleTable* ct = from.userCycles()) e.setUserCycles(*ct);
    for (int k = 0; k < ambient::Slots; ++k)
        if (const ambient::Texture* t = from.displayTexture(k))
            if (!t->empty())
                e.setTexture(k, *t);
    if (const std::vector<ambient::Texture>* pool = from.displayNearPool())   // the near layer's clips survive the change too
        e.setNearTextures(*pool);
    // The impulse responses are the one thing the engine cannot hand over -- it keeps them as
    // spectra, not as samples -- so a room the player opened is read from its file again. A
    // generated room needs nothing: a new engine makes its own.
    if (from.hasUserImpulse() && m_impulseFile.existsAsFile()) {
        ambient::Engine* was = m_prepareTarget;
        m_prepareTarget = &e;
        loadImpulseFile(m_impulseFile, false);
        if (m_impulseBFile.existsAsFile()) loadImpulseFile(m_impulseBFile, true);
        m_prepareTarget = was;
    }
}

/**
 * Message thread: the engine nobody is using goes back. Only when nothing is fading and no change
 * is on its way, which is exactly when the audio thread touches the live one and nothing else.
 */
void NoctuaryProcessor::releaseIdleEngine()
{
    // m_swapTo FIRST, then m_fading. The audio thread writes m_fading before it clears m_swapTo, so
    // a reader that sees m_swapTo cleared is guaranteed to see m_fading set. Read the other way
    // round there was a two-load window -- m_fading still -1, m_swapTo already -1 -- in which a
    // crossfade that had just begun looked like nothing at all, and the engine freed here was the
    // one the audio thread was rendering.
    if (m_swapTo.load(std::memory_order_acquire) >= 0) return;
    if (m_fading.load(std::memory_order_acquire) >= 0) return;
    if (m_pendingPreset >= 0 || m_prepareTarget != nullptr) return;
    m_engines[m_live.load(std::memory_order_relaxed) ^ 1].reset();
}

/**
 * Message thread. Runs when neither engine is being rendered but one: the one that is not live is
 * then ours to build on.
 */
void NoctuaryProcessor::servePendingPreset()
{
    if (m_pendingPreset < 0) { releaseIdleEngine(); return; }
    // Still busy: a fade in flight (both engines rendered), or a swap already published and not
    // yet taken up. Come back in a moment.
    if (m_swapTo.load(std::memory_order_acquire) >= 0 || m_fading.load(std::memory_order_acquire) >= 0) return;   // this order, see releaseIdleEngine
    const int index = m_pendingPreset;
    m_pendingPreset = -1;
    beginTransition(index);
}

void NoctuaryProcessor::beginTransition(int index)
{
    const int incoming = m_live.load(std::memory_order_relaxed) ^ 1;
    // Where the sound is leaving from, for the map's line.
    m_fadingFrom = m_currentProgram;
    // The engine has to exist before anything is pointed at it. Publishing m_paramTarget first
    // told the audio thread to push the parameter tree into an engine that was built on the very
    // next line -- a null dereference in processBlock, a second into every transition.
    ambient::Engine& in = ensureEngine(incoming);
    // From here until the swap, everything about the new preset goes to the incoming engine and
    // nothing to the one that is still playing: the parameter tree's next push (m_paramTarget),
    // the modulation matrix and the sample files (m_prepareTarget), the notes.
    m_paramTarget.store(incoming, std::memory_order_release);
    m_prepareTarget = &in;
    in.allNotesOff();
    in.reset();
    setCurrentProgram(index);
    // The parameter tree reaches the engine once a block; this one has to be complete before it
    // is heard, so it is filled in here as well.
    for (int i = 0; i < NumParams; ++i)
        in.setParam(static_cast<ParamIdE>(i), m_raw[static_cast<size_t>(i)]->load());
    // A change that came within the burst window of the one before it had its files put off to
    // the pump (see setCurrentProgram). A transition cannot have that: the engine is built here
    // and only then made the instrument, and its samples are part of the build. Put off, they
    // were read a quarter of a second later into whichever engine was live by then -- the right
    // one when the audio thread had already swapped, the one on its way out when it had not (a
    // host with the transport stopped, the host test's two changes 0.2 s of wall time apart) --
    // and a preset made of textures arrived with nothing to play.
    if (m_pendingFiles.exchange(-1, std::memory_order_acq_rel) == index) loadPresetFiles(index);
    // The conductor takes the chord over from the one it is replacing. A crossfade is meant to
    // change the instrument and not the music: picking its own notes made it two pieces of music
    // at once for the length of the fade, and picking them at its own event rate -- ninety-nine
    // seconds in places -- made it minutes of a single note. It inherits the cluster and carries
    // on with it, letting go of what does not suit it and adding what it wants at its own pace.
    // With nothing to inherit, from silence or from a preset whose conductor was off, it fills
    // instead, so a change never lands on an empty instrument either.
    {
        // The CONDUCTOR's cluster, which is twelve notes -- not ambient::Slots, which is the four
        // source slots. Two constants of the same name, and these arrays had the wrong one: a
        // conductor holding more than four notes wrote up to eight ints and eight floats past the
        // end of them. /GS answers that with __report_gsfailure and int 29h, which kills the
        // process on the spot -- no crash handler, no minidump, no entry in the event log, and an
        // exit that looks clean from outside. That is the "instrument vanished while a preset was
        // picked" this file's crash log was written for; it takes a preset whose conductor is
        // holding a full chord, which is why a change made a second after the last one never
        // showed it.
        int notes[ambient::ClusterBrain::Slots]; float vels[ambient::ClusterBrain::Slots];
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
                float v[NumParams];
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
        if (m_heldVel[static_cast<size_t>(i)] > 0.0f) in.noteOn(i, m_heldVel[static_cast<size_t>(i)]);
    m_prepareTarget = nullptr;
    m_fadePos.store(0.0f, std::memory_order_relaxed);      // nothing is fading yet: the map reads this
    m_swapTo.store(incoming, std::memory_order_release);   // the audio thread takes it from here
}

/**
 * Notes go to the live engine and are remembered, so a transition can hand them on.
 */
void NoctuaryProcessor::noteOn(int note, float vel)
{
    if (note < 0 || note >= 128) return;
    m_heldVel[static_cast<size_t>(note)] = std::max(vel, 1.0f / 127.0f);
    live().noteOn(note, vel);
}

void NoctuaryProcessor::noteOff(int note)
{
    if (note < 0 || note >= 128) return;
    m_heldVel[static_cast<size_t>(note)] = 0.0f;
    live().noteOff(note);
}

void NoctuaryProcessor::allNotesOff()
{
    for (auto& h : m_heldVel) h.store(0.0f, std::memory_order_relaxed);
    live().allNotesOff();
}

void NoctuaryProcessor::setMorphSlotFromPreset(int slot, int presetIndex)
{
    if (presetIndex < 0 || presetIndex >= numPresets()) return;
    float values[NumParams];
    for (int i = 0; i < NumParams; ++i) values[i] = m_raw[static_cast<size_t>(i)]->load();
    applyPreset(preset(presetIndex), [&](ParamIdE id, float v) { values[static_cast<int>(id)] = v; });
    live().setMorphSlot(slot, values);
    m_slotName[slot & 1] = preset(presetIndex).m_name;
}

void NoctuaryProcessor::setMorphSlotFromCurrent(int slot)
{
    float values[NumParams];
    for (int i = 0; i < NumParams; ++i) values[i] = m_raw[static_cast<size_t>(i)]->load();
    live().setMorphSlot(slot, values);
    m_slotName[slot & 1] = "(captured)";
}

void NoctuaryProcessor::clearMidiLearn(ParamIdE id)
{
    for (auto& c : m_ccMap) if (c.load() == static_cast<int>(id)) c.store(-1);
    if (m_learnTarget.load() == static_cast<int>(id)) m_learnTarget.store(-1);
}

int NoctuaryProcessor::midiCcFor(ParamIdE id) const
{
    for (int cc = 0; cc < 128; ++cc) if (m_ccMap[static_cast<size_t>(cc)].load() == static_cast<int>(id)) return cc;
    return -1;
}

void NoctuaryProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = m_apvts.copyState();
    if (m_scalaText.isNotEmpty()) {
        state.setProperty("scalaText", m_scalaText, nullptr);
        state.setProperty("scalaName", m_userScaleName, nullptr);
    }
    state.setProperty("gestureMappings", gestureMappings(), nullptr);
    {   // modulation: the matrix, the six envelope shapes and the four sources' own (see ambient/Modulation.h)
        char buf[4096];
        if (live().writeModMatrix(buf, sizeof(buf)) > 0) state.setProperty("modMatrix", juce::String(buf), nullptr);
        juce::String envs;
        const int shapes = ambient::NumModEnvs + ambient::Slots;
        for (int i = 0; i < shapes; ++i) {
            const int n = i < ambient::NumModEnvs ? live().writeEnvShape(i, buf, sizeof(buf))
                                                   : live().writeSrcEnvShape(i - ambient::NumModEnvs, buf, sizeof(buf));
            if (n > 0) envs += juce::String(buf);
            if (i + 1 < shapes) envs += "~";
        }
        state.setProperty("modEnvs", envs, nullptr);
    }
    // The favourites are no longer in the state (by index, which a regenerated library renumbers);
    // they live by name in the player's own file. An old state's property is read no more.
    if (m_routeText.isNotEmpty()) state.setProperty("route", m_routeText, nullptr);
    // One property per slot. An older state carries a single "textureFile", which is read below
    // as "the same clip in every slot" -- the only thing it could have meant at the time.
    for (int k = 0; k < ambient::Slots; ++k)
        if (m_textureFile[k].existsAsFile())
            state.setProperty("textureFile" + juce::String(k + 1), m_textureFile[k].getFullPathName(), nullptr);
    if (m_wavetableFile.existsAsFile()) state.setProperty("wavetableFile", m_wavetableFile.getFullPathName(), nullptr);
    if (m_impulseFile.existsAsFile())   state.setProperty("impulseFile", m_impulseFile.getFullPathName(), nullptr);
    if (m_impulseBFile.existsAsFile())  state.setProperty("impulseBFile", m_impulseBFile.getFullPathName(), nullptr);
    // The names of the two loaded presets, so the boxes still say what is loaded after a restart.
    if (m_soundName.isNotEmpty())  state.setProperty("soundPreset", m_soundName, nullptr);
    if (m_cosmosName.isNotEmpty()) state.setProperty("cosmosPreset", m_cosmosName, nullptr);
    if (m_nearName.isNotEmpty())   state.setProperty("nearPreset", m_nearName, nullptr);
    if (m_compact) state.setProperty("compact", 1, nullptr);          // how the editor is laid out
    if (m_layoutMode != 0) state.setProperty("layout", m_layoutMode, nullptr);
    if (m_levelMatch) state.setProperty("levelMatch", 1, nullptr);
    juce::ValueTree midi("midi");
    for (int cc = 0; cc < 128; ++cc) {
        const int target = m_ccMap[static_cast<size_t>(cc)].load();
        if (target >= 0) midi.setProperty("cc" + juce::String(cc), paramTable()[static_cast<size_t>(target)].m_key, nullptr);
    }
    state.addChild(midi, -1, nullptr);
    for (int slot = 0; slot < 2; ++slot) {
        juce::ValueTree m(slot == 0 ? "morphA" : "morphB");
        m.setProperty("name", m_slotName[slot], nullptr);
        m.setProperty("set", live().morphSlotSet(slot), nullptr);   // a slot nobody chose is not a snapshot
        float values[NumParams];
        live().morphSlot(slot, values);
        for (int i = 0; i < NumParams; ++i) m.setProperty(paramTable()[static_cast<size_t>(i)].m_key, values[i], nullptr);
        state.addChild(m, -1, nullptr);
    }
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, destData);
}

void NoctuaryProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes)) {
        if (xml->hasTagName(m_apvts.state.getType())) {
            auto tree = juce::ValueTree::fromXml(*xml);
            const juce::String text = tree.getProperty("scalaText").toString();
            const juce::String name = tree.getProperty("scalaName").toString();
            for (auto& c : m_ccMap) c.store(-1);
            const juce::String mappings = tree.getProperty("gestureMappings").toString();
            if (mappings.isNotEmpty()) setGestureMappings(mappings);
            auto midi = tree.getChildWithName("midi");
            if (midi.isValid())
                for (int cc = 0; cc < 128; ++cc) {
                    const juce::String key = midi.getProperty("cc" + juce::String(cc)).toString();
                    if (const ParamDesc* d = findParam(key.toRawUTF8())) m_ccMap[static_cast<size_t>(cc)].store(static_cast<int>(d->m_id));
                }
            for (int slot = 0; slot < 2; ++slot) {
                auto m = tree.getChildWithName(slot == 0 ? "morphA" : "morphB");
                if (!m.isValid()) continue;
                float values[NumParams];
                for (int i = 0; i < NumParams; ++i) {
                    const ParamDesc& d = paramTable()[static_cast<size_t>(i)];
                    values[i] = m.hasProperty(d.m_key) ? static_cast<float>(static_cast<double>(m.getProperty(d.m_key))) : d.m_def;
                }
                // Whether the slot was chosen. A state from before 2.0.2 does not say, and wrote both
                // slots every time -- so every one of them read back as a snapshot of the defaults,
                // and the session recall carried the trap into every start. For those the name
                // decides: "Init" is what an untouched slot was called, so it counts as not chosen.
                const juce::String nm = m.getProperty("name").toString();
                const bool chosen = m.hasProperty("set") ? static_cast<bool>(m.getProperty("set")) : (nm.isNotEmpty() && nm != "Init");
                if (chosen) { live().setMorphSlot(slot, values); m_slotName[slot] = nm; }
                else        { live().clearMorphSlot(slot);       m_slotName[slot] = {}; }
            }
            tree.removeChild(tree.getChildWithName("midi"), nullptr);
            tree.removeChild(tree.getChildWithName("morphA"), nullptr);
            tree.removeChild(tree.getChildWithName("morphB"), nullptr);
            const juce::String texPath = tree.getProperty("textureFile").toString();
            juce::String texPaths[ambient::Slots];
            for (int k = 0; k < ambient::Slots; ++k) texPaths[k] = tree.getProperty("textureFile" + juce::String(k + 1)).toString();
            const juce::String tabPath = tree.getProperty("wavetableFile").toString();
            const juce::String irPath = tree.getProperty("impulseFile").toString();
            const juce::String irBPath = tree.getProperty("impulseBFile").toString();
            // Which presets the two boxes should say are loaded. Looked up by name, and simply
            // not selected when the pack that held it is gone -- the sound is in the state
            // either way, only the label would have been a lie.
            m_soundName  = tree.getProperty("soundPreset").toString();
            m_cosmosName = tree.getProperty("cosmosPreset").toString();
            // A whole state came in: the layer boxes have no name to show, because the state is
            // the parameters themselves and not the presets they may once have come from.
            m_zIndex = -1; m_strikeIndex = -1;
            if (m_soundName.isNotEmpty()) {
                m_soundIndex = -1;
                for (int i = 0; i < numPresets(); ++i) if (m_soundName == preset(i).m_name) { m_soundIndex = i; break; }
                // The host's program index and the map's ring follow the same name; without this
                // they kept showing whatever was loaded before the state came in.
                if (m_soundIndex >= 0) m_currentProgram = m_soundIndex;
            }
            if (m_cosmosName.isNotEmpty()) {
                m_cosmosIndex = -1;
                for (int i = 0; i < numCosmosPresets(); ++i) if (m_cosmosName == cosmosPreset(i).m_name) { m_cosmosIndex = i; break; }
            }
            m_nearName = tree.getProperty("nearPreset").toString();
            m_nearIndex = -1;
            if (m_nearName.isNotEmpty())
                for (int i = 0; i < numNearPresets(); ++i) if (m_nearName == nearPreset(i).m_name) { m_nearIndex = i; break; }
            m_compact = static_cast<int>(tree.getProperty("compact", 0)) != 0;
            m_layoutMode = static_cast<int>(tree.getProperty("layout", m_compact ? 1 : 0));
            m_compact = m_layoutMode == 1;
            m_levelMatch = static_cast<int>(tree.getProperty("levelMatch", 0)) != 0;
            const juce::String route = tree.getProperty("route").toString();
            const juce::String modMatrix = tree.getProperty("modMatrix").toString();
            const juce::String modEnvs = tree.getProperty("modEnvs").toString();
            if (modMatrix.isNotEmpty()) live().setModMatrixText(modMatrix.toRawUTF8());
            if (modEnvs.isNotEmpty()) {
                const juce::StringArray parts = juce::StringArray::fromTokens(modEnvs, "~", "");
                // The six, then the sources' own; a state saved before those existed has six.
                for (int i = 0; i < juce::jmin(parts.size(), ambient::NumModEnvs + ambient::Slots); ++i) {
                    if (parts[i].isEmpty()) continue;
                    if (i < ambient::NumModEnvs) live().setEnvShape(i, parts[i].toRawUTF8());
                    else live().setSrcEnvShape(i - ambient::NumModEnvs, parts[i].toRawUTF8());
                }
            }
            if (route.isNotEmpty()) setRouteText(route);
            tree.removeProperty("route", nullptr);
            tree.removeProperty("textureFile", nullptr);
            for (int k = 0; k < ambient::Slots; ++k) tree.removeProperty("textureFile" + juce::String(k + 1), nullptr);
            tree.removeProperty("wavetableFile", nullptr);
            tree.removeProperty("impulseFile", nullptr);
            tree.removeProperty("favourites", nullptr);
            tree.removeProperty("soundPreset", nullptr);
            tree.removeProperty("cosmosPreset", nullptr);
            m_apvts.replaceState(tree);
            // Make the parameter objects agree with the state that was just restored. Replacing
            // the state moves a parameter only where the value IN THE TREE changes, and a switch
            // puts only 0 or 1 there while the object keeps whatever raw number the host set it
            // to: a switch a host had left at 0.87 -- which is "on" -- stayed at 0.87 when a
            // state saying 1.0 came in, and the host read 0.87 back out of a session it had
            // saved as 1.0. The sound was right either way, because the engine reads the
            // switched value and not the raw one, so nothing here ever heard it. pluginval did.
            for (const ParamDesc& d : paramTable()) {
                auto* par = m_apvts.getParameter(d.m_key);
                const auto* raw = m_apvts.getRawParameterValue(d.m_key);
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
            for (int k = 0; k < ambient::Slots; ++k)
                if (texPaths[k].isNotEmpty()) { const juce::File f = onDisk(texPaths[k]); if (f != juce::File()) loadTextureFile(k, f); }
            if (tabPath.isNotEmpty()) { const juce::File f = onDisk(tabPath); if (f != juce::File()) loadWavetableFile(f); }
            if (irPath.isNotEmpty())  { const juce::File f = onDisk(irPath);  if (f != juce::File()) loadImpulseFile(f); }
            if (irBPath.isNotEmpty()) { const juce::File f = onDisk(irBPath); if (f != juce::File()) loadImpulseFile(f, true); }
        }
    }
}

/**
 * @brief JUCE's entry point: one processor per instance, for every format and for the standalone.
 * @return the new processor; the host (or the standalone holder) owns and deletes it
 */
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NoctuaryProcessor();
}
