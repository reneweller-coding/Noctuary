// Noctuary -- the host contract.
//
// The self test measures the instrument; this one measures the plugin around it. Everything here
// is something a host does and a synth has to survive: prepare and release at rates and block
// sizes nobody develops at, a state that has to come back exactly as it went out, programs
// changed while audio is running, an editor opened and closed under load, and parameters written
// from the message thread while the audio thread reads them. None of it was ever checked, because
// the standalone only ever does one of these things at a time and always at 48 kHz.
//
// This is not a replacement for pluginval (which exercises the VST3 wrapper itself, and which is
// worth running on the built plugin); it is the part that can live in the repository and run on
// every build.
#include "PluginProcessor.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <fstream>
#include <thread>
#include <vector>

using namespace ambient;

namespace {
int failures = 0;
void check(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

bool finite(const juce::AudioBuffer<float>& b)
{
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (!std::isfinite(b.getReadPointer(c)[i])) return false;
    return true;
}

// A block of audio with a couple of notes and some expression, which is what a host really sends.
void feed(NoctuaryProcessor& p, juce::AudioBuffer<float>& buf, int blocks, bool notes)
{
    juce::MidiBuffer midi;
    for (int b = 0; b < blocks; ++b) {
        midi.clear();
        if (notes && b == 1) {
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
            midi.addEvent(juce::MidiMessage::noteOn(2, 67, 0.6f), 4);
            midi.addEvent(juce::MidiMessage::channelPressureChange(2, 90), 8);
            midi.addEvent(juce::MidiMessage::pitchWheel(2, 9000), 12);
            midi.addEvent(juce::MidiMessage::controllerEvent(2, 74, 100), 16);
        }
        if (notes && b == blocks - 2) {
            midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
            midi.addEvent(juce::MidiMessage::noteOff(2, 67), 0);
        }
        buf.clear();
        p.processBlock(buf, midi);
    }
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // ---------------------------------------------------------------- rates and block sizes
    for (double sr : { 44100.0, 48000.0, 96000.0 }) {
        for (int block : { 16, 64, 512, 2048 }) {
            auto p = std::make_unique<NoctuaryProcessor>();
            p->setPlayConfigDetails(0, 2, sr, block);
            p->prepareToPlay(sr, block);
            juce::AudioBuffer<float> buf(2, block);
            feed(*p, buf, 12, true);
            const juce::String what = "renders finite at " + juce::String(static_cast<int>(sr)) + " Hz, block " + juce::String(block);
            check(finite(buf), what.toRawUTF8());
            // A block smaller or larger than the one it was prepared with: hosts do this.
            juce::AudioBuffer<float> odd(2, juce::jmax(1, block / 3));
            feed(*p, odd, 4, false);
            check(finite(odd), (what + " (short block)").toRawUTF8());
            p->releaseResources();
            // Prepared, released and prepared again is the sequence around every device change.
            p->prepareToPlay(sr, block);
            feed(*p, buf, 4, true);
            check(finite(buf), (what + " (after a device change)").toRawUTF8());
            p->releaseResources();
        }
    }

    // ---------------------------------------------------------------- state round trip
    {
        auto a = std::make_unique<NoctuaryProcessor>();
        a->prepareToPlay(48000.0, 256);
        juce::Random rng(1234);
        // Move every parameter somewhere unusual, so a state that silently drops one shows up.
        for (const ParamDesc& d : paramTable())
            if (auto* par = a->apvts.getParameter(d.key)) par->setValueNotifyingHost(0.15f + 0.7f * rng.nextFloat());
        a->setMorphSlotFromCurrent(0);
        a->engine().setModMatrixText("lfo2>cutoff:0.4;env3>z_x:-0.2:macro_a");
        a->engine().setEnvShape(1, "0:0/2:1/5:-0.5!s1");
        a->engine().setSrcEnvShape(2, "0:0/1.5:0.8:-0.3/4:1!s1");
        juce::MemoryBlock blob;
        a->getStateInformation(blob);

        auto b = std::make_unique<NoctuaryProcessor>();
        b->prepareToPlay(48000.0, 256);
        b->setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
        int wrong = 0;
        for (const ParamDesc& d : paramTable()) {
            const float x = a->engine().getParam(d.id), y = b->engine().getParam(d.id);
            if (std::fabs(x - y) > 1.0e-4f * juce::jmax(1.0f, std::fabs(x))) { ++wrong; if (wrong < 4) std::printf("  state differs: %s %g vs %g\n", d.key, x, y); }
        }
        check(wrong == 0, "every parameter survives a state round trip");
        char m1[4096], m2[4096];
        a->engine().writeModMatrix(m1, sizeof(m1));
        b->engine().writeModMatrix(m2, sizeof(m2));
        check(juce::String(m1) == juce::String(m2), "the modulation matrix survives a state round trip");
        char e1[512], e2[512];
        a->engine().writeEnvShape(1, e1, sizeof(e1));
        b->engine().writeEnvShape(1, e2, sizeof(e2));
        check(juce::String(e1) == juce::String(e2), "an envelope shape survives a state round trip");
        char s1[512], s2[512];
        a->engine().writeSrcEnvShape(2, s1, sizeof(s1));
        b->engine().writeSrcEnvShape(2, s2, sizeof(s2));
        check(juce::String(s1) == juce::String(s2) && juce::String(s1).contains("!s1"), "a source's own envelope survives a state round trip");
        float va[kNumParams], vb[kNumParams];
        a->engine().morphSlot(0, va);
        b->engine().morphSlot(0, vb);
        int mw = 0;
        for (int i = 0; i < kNumParams; ++i) if (std::fabs(va[i] - vb[i]) > 1.0e-4f * juce::jmax(1.0f, std::fabs(va[i]))) ++mw;
        check(mw == 0, "a morph snapshot survives a state round trip");
    }

    // ---------------------------------------------------------------- what the host sees restored
    // The round trip above compares what the ENGINE holds, which is what the sound is made of.
    // A host compares something narrower and just as binding: the value of every parameter object
    // it can automate. pluginval found four that came back at whatever they had been set to
    // rather than at what the state said, and nothing here would have noticed, because the engine
    // value was right in each case.
    {
        auto p = std::make_unique<NoctuaryProcessor>();
        p->prepareToPlay(48000.0, 256);
        juce::MemoryBlock blob;
        p->getStateInformation(blob);
        std::vector<float> saved;
        for (auto* par : p->getParameters()) saved.push_back(par->getValue());
        juce::Random rng(99);
        for (auto* par : p->getParameters()) par->setValueNotifyingHost(rng.nextFloat());
        p->setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
        int wrong = 0, i = 0;
        for (auto* par : p->getParameters()) {
            if (std::fabs(par->getValue() - saved[static_cast<size_t>(i)]) > 0.01f) {
                if (wrong < 8) std::printf("  not restored: %-14s saved %.4f, now %.4f\n",
                                           par->getName(20).toRawUTF8(), saved[static_cast<size_t>(i)], par->getValue());
                ++wrong;
            }
            ++i;
        }
        check(wrong == 0, "every host parameter is restored by setStateInformation");
    }

    // ---------------------------------------------------------------- random parameter settings
    // AMBIENT_FUZZ=1: not part of the normal run. Sets every parameter to a random value, plays a
    // chord, and asks only that the result be a number. Then it narrows a failure down to the
    // parameters that actually cause it, by putting them back one at a time.
    // AMBIENT_TIMING=1: what one program change costs a host. pluginval hammers every parameter
    // it can see, and through the VST3 wrapper that includes the program change -- so this number,
    // times the number of writes it makes, is the fifteen minutes it gave up after.
    if (std::getenv("AMBIENT_TIMING") != nullptr) {
        auto p = std::make_unique<NoctuaryProcessor>();
        p->prepareToPlay(48000.0, 256);
        std::printf("  timing: %d programs are on offer\n", p->getNumPrograms());
        auto time = [&p](int from, int count, const char* what) {
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < count; ++i) p->setCurrentProgram(from + i);
            const double each = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / count;
            std::printf("  timing: %-28s %7.1f ms each\n", what, each * 1000.0);
            return each;
        };
        // Built-ins first: most of them name no files at all. Then presets out of the library,
        // which nearly all name a clip, a wavetable or a room, and those are read off the disk.
        const double built = time(0, 20, "a built-in program");
        const double pack = p->getNumPrograms() > 600 ? time(600, 20, "a program from the library") : built;
        const double each = std::max(built, pack);
        std::printf("  timing: fifteen minutes of the slower is %.0f changes\n", 900.0 / each);
        check(true, "timing done");
    }

    if (std::getenv("AMBIENT_FUZZ") != nullptr) {
        juce::AudioBuffer<float> buf(2, 256);
        // Plays a chord on a fresh instrument and says whether the sound stayed a number, while
        // `pick` is being written to from another thread the whole time.
        auto trial = [&buf](const char* section, double seconds, int seed) {
            auto p = std::make_unique<NoctuaryProcessor>();
            p->prepareToPlay(48000.0, 256);
            std::vector<juce::AudioProcessorParameter*> pick;
            int i = 0;
            for (auto* par : p->getParameters()) {
                if (section == nullptr || std::strcmp(paramTable()[static_cast<size_t>(i)].section, section) == 0) pick.push_back(par);
                ++i;
            }
            std::atomic<bool> stop { false };
            std::thread hammer([&pick, &stop, seed] {
                juce::Random rng(seed);
                while (!stop.load()) for (auto* par : pick) { par->setValueNotifyingHost(rng.nextFloat()); if (stop.load()) return; }
            });
            const auto t0 = std::chrono::steady_clock::now();
            bool ok = true;
            juce::MidiBuffer midi;
            for (int b = 0; ok && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds; ++b) {
                midi.clear();
                if (b == 1) { midi.addEvent(juce::MidiMessage::noteOn(1, 48, 0.9f), 0); midi.addEvent(juce::MidiMessage::noteOn(1, 55, 0.7f), 8); }
                buf.clear();
                p->processBlock(buf, midi);
                ok = finite(buf);
            }
            stop.store(true);
            hammer.join();
            return ok;
        };
        // Does a fresh instrument, left alone, stay a number? The control for everything below.
        std::printf("  fuzz: nothing written at all      -> %s\n", trial("no such section", 2.0, 1) ? "finite" : "NON-FINITE");
        std::printf("  fuzz: every parameter at once     -> %s\n", trial(nullptr, 4.0, 7) ? "finite" : "NON-FINITE");
        std::vector<const char*> sections;
        for (const ParamDesc& d : paramTable()) {
            bool have = false;
            for (const char* s2 : sections) if (std::strcmp(s2, d.section) == 0) { have = true; break; }
            if (!have) sections.push_back(d.section);
        }
        for (const char* sec : sections) {
            bool ok = true;
            for (int rep = 0; rep < 3 && ok; ++rep) ok = trial(sec, std::getenv("AMBIENT_FUZZ_LONG") ? 5.0 : 1.2, 7 + rep);
            if (!ok) std::printf("      %-16s *** NON-FINITE ***\n", sec);
        }
        std::printf("  fuzz: sections not listed above stayed finite\n");
        check(true, "fuzz finished");
    }

    // ---------------------------------------------------------------- a program brings its files
    // A program change that stands on its own still reads what the preset names, right there and
    // then. Only a sweep is held back, and only until it stops -- so this has to keep working, or
    // presets would quietly play with whatever clip happened to be loaded before.
    {
        auto p = std::make_unique<NoctuaryProcessor>();
        p->prepareToPlay(48000.0, 256);
        int withClip = -1;
        for (int i = 0; i < p->getNumPrograms() && withClip < 0; ++i)
            if (preset(i).texture != nullptr && *preset(i).texture != 0) withClip = i;
        if (withClip >= 0) {
            p->setCurrentProgram(withClip);
            bool any = false;
            for (int k = 0; k < ambient::kSlots; ++k) {
                const ambient::Texture* t = p->engine().displayTexture(k);
                if (t != nullptr && !t->empty()) any = true;
            }
            check(any, "a program change on its own loads the clip the preset names");
        }
    }

    // ---------------------------------------------------------------- a preset's two rooms
    // A pack preset names impulse A and, since the ninth field, impulse B. One that names both gives
    // the Room both; one that names only A takes the old B away -- Room Morph blended into whatever
    // B was left from before, so a preset sounded like its history. And a file longer than twelve
    // seconds is read whole now: the Room keeps a minute.
    {
        const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("noctuary_roomtest");
        dir.deleteRecursively();
        dir.createDirectory();
        auto writeIr = [](const juce::File& f, double seconds, uint32_t seed) {
            const int sr = 48000, n = static_cast<int>(seconds * sr);
            std::vector<float> inter(static_cast<size_t>(2 * n));
            uint32_t s = seed;
            for (int i = 0; i < n; ++i) {
                // A 16-second decay: the Room drops the end whose energy falls under its budget
                // (-107 dB), and an 8-second one would get there after 14 seconds.
                const float g = static_cast<float>(std::pow(10.0, -3.0 * i / (16.0 * sr)));
                for (int c = 0; c < 2; ++c) {
                    s = s * 1664525u + 1013904223u;
                    inter[static_cast<size_t>(2 * i + c)] = 0.5f * g * (static_cast<float>((s >> 8) & 0xFFFF) / 32767.5f - 1.0f);
                }
            }
            std::ofstream o(f.getFullPathName().toStdString(), std::ios::binary);
            auto u32 = [&](uint32_t v) { o.write(reinterpret_cast<const char*>(&v), 4); };
            auto u16 = [&](uint16_t v) { o.write(reinterpret_cast<const char*>(&v), 2); };
            const uint32_t bytes = static_cast<uint32_t>(inter.size() * 4);
            o.write("RIFF", 4); u32(36 + bytes); o.write("WAVE", 4);
            o.write("fmt ", 4); u32(16); u16(3); u16(2); u32(48000); u32(48000 * 8); u16(8); u16(32);
            o.write("data", 4); u32(bytes);
            o.write(reinterpret_cast<const char*>(inter.data()), bytes);
        };
        writeIr(dir.getChildFile("rooma.wav"), 1.0, 1);
        writeIr(dir.getChildFile("roomb.wav"), 1.0, 2);
        writeIr(dir.getChildFile("roomlong.wav"), 20.0, 3);
        {
            std::ofstream pack(dir.getChildFile("RoomTest.ambientpack").getFullPathName().toStdString());
            pack << "pack Room Test Pack\n";
            pack << "Room Test Both|room_level=0.5;room_morph=0.5|0.5 0.5 0.5 0.5 0.5 0.5 0.5 0.5 0|||rooma.wav|||roomb.wav\n";
            pack << "Room Test One|room_level=0.5|0.5 0.5 0.5 0.5 0.5 0.5 0.5 0.5 0|||rooma.wav||\n";
        }
        check(ambient::loadPresetPack(dir.getChildFile("RoomTest.ambientpack").getFullPathName().toRawUTF8()), "a pack with two rooms loads");
        int both = -1, one = -1;
        for (int i = 0; i < ambient::numPresets(); ++i) {
            if (std::strcmp(preset(i).name, "Room Test Both") == 0) both = i;
            if (std::strcmp(preset(i).name, "Room Test One") == 0) one = i;
        }
        check(both >= 0 && one >= 0, "its presets are in the list");
        auto p = std::make_unique<NoctuaryProcessor>();
        p->prepareToPlay(48000.0, 256);
        if (both >= 0 && one >= 0) {
            p->applySoundPreset(both);
            check(p->engine().hasImpulseB() && p->impulseBName() == "roomb", "a preset that names impulse B brings it");
            p->applySoundPreset(one);
            check(!p->engine().hasImpulseB() && p->impulseBName().isEmpty(), "a preset that names only its room takes the old B away");
            check(p->impulseName() == "rooma", "and keeps its own A");
        }
        check(p->loadImpulseFile(dir.getChildFile("roomlong.wav")), "a twenty-second impulse loads");
        check(p->engine().impulseSeconds() > 15.0f, "and is kept longer than the old twelve-second limit");
        p.reset();
        dir.deleteRecursively();
    }

    // ---------------------------------------------------------------- parameters under two threads
    // What a host does when it plays back automation on everything at once, which is what
    // pluginval's parameter thread safety test does -- and where it gave up after fifteen minutes.
    {
        auto p = std::make_unique<NoctuaryProcessor>();
        p->prepareToPlay(48000.0, 256);
        juce::AudioBuffer<float> buf(2, 256);
        std::atomic<bool> stop { false };
        std::atomic<long long> writes { 0 };
        auto hammer = [&p, &stop, &writes](int seed) {
            juce::Random rng(seed);
            while (!stop.load()) {
                for (auto* par : p->getParameters()) {
                    par->setValueNotifyingHost(rng.nextFloat());
                    writes.fetch_add(1, std::memory_order_relaxed);
                    if (stop.load()) return;
                }
            }
        };
        std::thread t1(hammer, 1), t2(hammer, 2);
        const auto t0 = std::chrono::steady_clock::now();
        long long blocks = 0;
        while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(4)) {
            juce::MidiBuffer midi;
            buf.clear();
            p->processBlock(buf, midi);
            ++blocks;
        }
        stop.store(true);
        t1.join(); t2.join();
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double audioSecs = blocks * 256.0 / 48000.0;
        std::printf("  stress: %lld blocks (%.1f s of audio) and %lld parameter writes in %.1f s wall -- %.2fx real time\n",
                    blocks, audioSecs, writes.load(), secs, audioSecs / secs);
        check(finite(buf), "audio stays finite while two threads write every parameter");
        // Real time is the bar a host holds it to. Well under it here means a host that
        // automates a lot cannot keep up either. Not asked of a sanitized build, which is several
        // times slower by design and would fail this every time while proving nothing.
       #if !defined(__SANITIZE_ADDRESS__)
        check(audioSecs > secs, "audio keeps up with real time while every parameter is automated");
       #else
        std::printf("  (sanitized build: the real-time bar is not asked of it)\n");
       #endif
    }

    // ---------------------------------------------------------------- which preset it says it is
    // The state carried every knob but never the name of the preset they came from, so a restored
    // session played the right sound under the label "Init" and looked like a feature that did
    // not work. The name is stored, not the index: a pack added between two sessions renumbers
    // every preset behind it.
    {
        auto a = std::make_unique<NoctuaryProcessor>();
        a->prepareToPlay(48000.0, 256);
        // Any preset but the empty one. It used to name a built-in, which tied a host test to a
        // library that is regenerated whole -- and what is under test here is the name, not which
        // preset carries it.
        const int which = numPresets() / 2;
        check(which > 0 && preset(which).name[0] != 0, "the preset the round trip is built on exists");
        a->applySoundPreset(which);
        a->applyCosmosPreset(3);
        juce::MemoryBlock blob;
        a->getStateInformation(blob);

        auto b = std::make_unique<NoctuaryProcessor>();
        b->prepareToPlay(48000.0, 256);
        b->setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
        check(b->soundPresetIndex() == which, "a restored session still knows which sound preset it holds");
        check(b->cosmosPresetIndex() == 3, "a restored session still knows which cosmos preset it holds");

        // A state from before the names existed must not blank the boxes: no name means keep
        // whatever the default is, not "nothing selected".
        auto xml = juce::AudioProcessor::getXmlFromBinary(blob.getData(), static_cast<int>(blob.getSize()));
        check(xml != nullptr, "the state is readable as XML");
        xml->removeAttribute("soundPreset");
        xml->removeAttribute("cosmosPreset");
        juce::MemoryBlock old;
        juce::AudioProcessor::copyXmlToBinary(*xml, old);
        auto c = std::make_unique<NoctuaryProcessor>();
        c->prepareToPlay(48000.0, 256);
        c->setStateInformation(old.getData(), static_cast<int>(old.getSize()));
        check(c->soundPresetIndex() >= 0 && c->cosmosPresetIndex() >= 0, "a state without preset names leaves the boxes on their default");
    }

    // ---------------------------------------------------------------- programs while playing
    {
        auto p = std::make_unique<NoctuaryProcessor>();
        p->prepareToPlay(48000.0, 256);
        juce::AudioBuffer<float> buf(2, 256);
        bool ok = true;
        const int n = p->getNumPrograms();
        for (int i = 0; i < juce::jmin(n, 40); ++i) {
            p->setCurrentProgram(i * juce::jmax(1, n / 40));
            feed(*p, buf, 2, i % 4 == 0);
            if (!finite(buf)) ok = false;
        }
        check(ok, "programs can be changed while audio is running");
        p->releaseResources();
    }

    // ---------------------------------------------------------------- editor, and writes from
    // the message thread while the audio thread renders
    {
        auto p = std::make_unique<NoctuaryProcessor>();
        p->prepareToPlay(48000.0, 256);
        juce::AudioBuffer<float> buf(2, 256);
        std::atomic<bool> stop{ false };
        std::atomic<bool> bad{ false };
        // The "audio thread": a separate thread, so the parameter writes below really are
        // concurrent rather than interleaved by the scheduler on one thread.
        std::thread audio([&] {
            juce::AudioBuffer<float> local(2, 256);
            juce::MidiBuffer midi;
            while (!stop.load()) {
                midi.clear();
                local.clear();
                p->processBlock(local, midi);
                if (!finite(local)) bad.store(true);
            }
        });
        juce::Random rng(99);
        for (int i = 0; i < 400; ++i) {
            const ParamDesc& d = paramTable()[static_cast<size_t>(rng.nextInt(kNumParams))];
            if (auto* par = p->apvts.getParameter(d.key)) par->setValueNotifyingHost(rng.nextFloat());
        }
        {   // An editor opened and thrown away twice while the audio thread keeps going.
            for (int i = 0; i < 2; ++i) {
                auto* ed = p->createEditorIfNeeded();
                check(ed != nullptr, "the editor can be created");
                if (ed != nullptr) {
                    // Lay it out and paint it. A plugin may not spin a message loop, so the
                    // editor's timers cannot be driven here; painting synchronously is what
                    // actually exercises every display, and it is where a bad pointer would show.
                    ed->setBounds(0, 0, 1600, 950);
                    const juce::Image shot = ed->createComponentSnapshot(ed->getLocalBounds(), true, 1.0f);
                    check(shot.isValid() && shot.getWidth() > 100, "the editor paints");
                    p->editorBeingDeleted(ed);
                    delete ed;
                }
            }
        }
        stop.store(true);
        audio.join();
        check(!bad.load(), "the output stays finite while parameters are written from another thread");
        p->releaseResources();
    }

    {   // A preset transition is a crossfade between two engines, not a parameter morph. The
        // morph flipped every switch at half way and every source restarted -- audibly a cut in
        // the middle. What is checked here is what the ear checks: that the level never jumps
        // from one block to the next while the change travels, that the displays know what is
        // travelling and when it has arrived, and that both engines stay finite throughout.
        const double sr = 48000.0; const int block = 256;
        auto p = std::make_unique<NoctuaryProcessor>();
        p->prepareToPlay(sr, block);
        p->setMorphSelectSeconds(2.0f);
        // Two presets that share as little as possible: the first built-in bank, and the first
        // pack preset with a grain texture in its first slot if a library is loaded.
        int a = 1, b = -1;
        for (int i = 0; i < numPresets() && b < 0; ++i) {
            const char* st = preset(i).settings;
            if (st != nullptr && std::strstr(st, "src1_type=Texture") != nullptr) b = i;
        }
        if (b < 0) b = 2;
        juce::AudioBuffer<float> buf(2, block);
        juce::MidiBuffer midi;
        auto rmsOf = [&]() {
            double e = 0.0;
            for (int c = 0; c < 2; ++c) for (int i = 0; i < block; ++i) { const float v = buf.getReadPointer(c)[i]; e += v * v; }
            return std::sqrt(e / (2.0 * block)) + 1e-9;
        };
        {   // a wavetable and a scale of the player's own, before any preset change
            std::vector<float> frame(2048);
            for (int i = 0; i < 2048; ++i) frame[static_cast<size_t>(i)] = std::sin(6.2831853f * i / 2048.0f);
            check(p->engine().loadUserWavetable(frame.data(), static_cast<int>(frame.size())), "a user wavetable loads");
            check(p->loadScalaText("! test.scl\nTest scale\n 3\n!\n 100.0\n 200.0\n 2/1\n", "Test"), "a Scala scale loads");
        }
        p->selectPreset(a, false);
        midi.addEvent(juce::MidiMessage::noteOn(1, 57, 0.8f), 0);
        midi.addEvent(juce::MidiMessage::noteOn(1, 64, 0.7f), 0);
        for (int k = 0; k < 400; ++k) { buf.clear(); p->processBlock(buf, midi); midi.clear(); }   // settle, chord held
        const double before = rmsOf();
        p->selectPreset(b, true);
        check(p->morphingTo() == b, "a travelling preset change names its destination");
        // Both ends, because the map draws a line between them: without the departure the line ran
        // from the new preset to itself, and its label was printed over the arrival's own name.
        check(p->morphingFrom() == a, "and where it is travelling from");
        check(p->morphingFrom() != p->morphingTo(), "the two ends of the line are two different presets");
        // The ramp waits for the incoming engine to be audible (up to a capped head start) and only
        // then runs its two seconds; the checks below count from where it starts.
        // Level is compared between neighbouring windows of sixteen blocks (85 ms): one block is
        // shorter than a period of the sub these presets carry, and the block RMS of a 40 Hz tone
        // swings by several dB on its own. A cut in the middle would still show as a step of many
        // dB between two 85 ms windows; the same measure on the arrived preset alone, sounding
        // steadily, is printed beside it as the yardstick for what "no step" looks like here.
        bool ok = true; double worstJump = 0.0, mid = -1.0, jumpFrom = 0.0, jumpTo = 0.0; int jumpAt = -1, head = 0;
        double win = 0.0, prevWin = -1.0;
        auto step = [&](int k, double& worst, int* at, double* from, double* to) {
            win += rmsOf() * rmsOf();
            if ((k + 1) % 16 != 0) return;
            const double now = std::sqrt(win / 16.0); win = 0.0;
            if (prevWin > 0.0 && std::max(now, prevWin) > 3e-4) {   // a step in the noise floor is not a step
                const double j = std::fabs(20.0 * std::log10(now / prevWin));
                if (j > worst) { worst = j; if (at) { *at = k; *from = prevWin; *to = now; } }
            }
            prevWin = now;
        };
        while (p->morphProgress() <= 0.0f && head < 2000) { buf.clear(); p->processBlock(buf, midi); ++head; }
        for (int k = 0; k < 416; ++k) {
            buf.clear(); p->processBlock(buf, midi);
            ok = ok && finite(buf);
            step(k, worstJump, &jumpAt, &jumpFrom, &jumpTo);
            if (k == 187) mid = p->morphProgress();
        }
        const double arrived = rmsOf();
        // The yardstick, and it has to be a long one. A preset carrying a looping modulation
        // envelope moves its own level by several dB on a cycle of ten or twenty seconds -- Fell
        // Span, which this test happened to pick out of the 2.0 library, swings 3.3 dB at six
        // seconds and again at ten, cold, with no transition anywhere near it. Measured over four
        // seconds it looked perfectly steady and the transition looked broken; measured over forty
        // the preset is seen for what it is. What this check is for is a CUT in the middle of a
        // crossfade, which is twenty dB and more, not a preset breathing on its own clock.
        double steadyJump = 0.0; win = 0.0; prevWin = -1.0;
        for (int k = 0; k < 3800; ++k) { buf.clear(); p->processBlock(buf, midi); step(k, steadyJump, nullptr, nullptr, nullptr); }
        const double later = rmsOf();
        std::printf("  [probe] transition %d -> %d: level before %.1f dBFS, head start %d blocks, worst 85 ms step %.2f dB at block %d (%.1f -> %.1f dBFS; steady preset alone %.2f dB), half way at %.2f, on arrival %.1f dBFS, 4 s later %.1f dBFS, voices %d\n",
                    a, b, 20.0 * std::log10(before), head, worstJump, jumpAt, 20.0 * std::log10(jumpFrom), 20.0 * std::log10(jumpTo), steadyJump, mid,
                    20.0 * std::log10(arrived), 20.0 * std::log10(later), p->engine().activeVoices());
        check(ok, "both engines stay finite through a transition");
        check(head < 2000, "the incoming engine speaks and the ramp starts");
        check(worstJump < std::max(3.0, steadyJump + 1.5),
              "a transition never steps in level more than the arriving preset does on its own");
        check(mid > 0.3 && mid < 0.7, "half way through the time given, the fade is about half way");
        check(arrived > 1e-3, "when the old preset is gone the new one is already audible");
        // What the player loaded by hand has to survive the change of engine -- and the engine
        // has changed by now, so this is asked of the new one. A preset brings its own sample
        // and wavetable; a Scala scale and a table opened from a file live in the engine and
        // nowhere else, and a transition builds a fresh one.
        check(p->engine().userWavetable() != nullptr, "a wavetable the player loaded survives a transition");
        check(std::strstr(p->engine().userScale().name, "Test") != nullptr, "and so does the scale they tuned it to");
        // The conductor arrives having already made up its mind. Growing an empty chord one note
        // per event is an entrance from silence and a failure here: the preset being faded out was
        // sounding a full cluster. At the library's slower rates -- Interior Bloom asks six voices
        // at an event every 98.8 seconds -- that was two voices after a minute and four after
        // seven. A few seconds of audio after the change, the new one should be at its density.
        {
            auto q = std::make_unique<NoctuaryProcessor>();
            q->prepareToPlay(sr, block);
            q->setMorphSelectSeconds(2.0f);
            int slow = -1;
            for (int i = 0; i < numPresets() && slow < 0; ++i) {
                const char* st = preset(i).settings;
                if (st != nullptr && std::strstr(st, "brain_rate=9") != nullptr && std::strstr(st, "brain_density=") != nullptr)
                    slow = i;
            }
            if (slow >= 0) {
                juce::AudioBuffer<float> buf(2, block);
                q->selectPreset(1, false);
                feed(*q, buf, 40, false);
                q->selectPreset(slow, true);
                q->servePresetRequests();
                feed(*q, buf, static_cast<int>(4.0 * sr / block), false);   // four seconds
                const int want = static_cast<int>(q->engine().getParam(ParamId::BrainDensity));
                const int got = q->engine().activeVoices();
                // The conductor's own count beside the voices: they answer different questions.
                // Notes it holds but that never became voices mean the voices were the problem;
                // one note held means the filling itself never ran.
                int cn[ambient::ClusterBrain::kSlots]; float cv[ambient::ClusterBrain::kSlots];
                const int cluster = q->engine().soundingCluster(cn, cv);
                std::printf("  [probe] four seconds after a change into %s: %d of %d voices, conductor holds %d\n",
                            preset(slow).name, got, want, cluster);
                check(got >= juce::jmin(want, 3), "the conductor fills its cluster when it takes over from one that was sounding");
            }
        }
        {   // A change made while the conductor holds a FULL chord. The handover reads the leaving
            // conductor's cluster into two arrays, and those were sized by the number of source
            // slots (four) instead of the conductor's twelve: with more than four notes sounding it
            // wrote past their end, and /GS ended the process on the spot -- no crash handler, no
            // dump, no event, an exit that looked clean. The instrument simply vanished when a
            // preset was picked, which is what the crash log in PluginProcessor.cpp was written for.
            //
            // The transition test above never showed it: two seconds after its own change the
            // conductor is still holding one or two notes. So this one waits for five.
            auto q = std::make_unique<NoctuaryProcessor>();
            q->prepareToPlay(sr, block);
            q->setMorphSelectSeconds(1.0f);
            q->selectPreset(1, false);
            auto set = [&](const char* key, float v) {
                if (auto* p = q->apvts.getParameter(key)) p->setValueNotifyingHost(p->convertTo0to1(v));
            };
            set("brain_density", 10.0f);     // a wide cluster...
            set("brain_rate", 2.0f);         // ...filled quickly, so the test is seconds and not minutes
            set("brain_density_slew", 0.0f); // ...and at once: the library's presets ramp a change of
                                             // density over minutes now (Anti 9), which is right and
                                             // which would keep this test under five voices for ever
            set("brain_onset_guard", 0.0f);  // and two onsets may be closer than three seconds here
            set("brain_sync", 0.0f);         // Free. Sync does not put the events on a grid, it REPLACES
                                             // the rate with a number of bars -- the preset this test
                                             // starts from carried one, and Event Rate 2 s meant nothing:
                                             // three voices in a minute instead of thirteen.
            set("brain_hold_min", 600.0f);
            set("brain_hold_max", 1200.0f);
            juce::AudioBuffer<float> buf(2, block);
            int held = 0;
            for (int k = 0; k < static_cast<int>(60.0 * sr / block) && held <= 4; ++k) {
                feed(*q, buf, 1, false);
                held = q->engine().activeVoices();
            }
            std::printf("  [probe] conductor holding %d voices at the change\n", held);
            check(held > 4, "the conductor filled a cluster wider than the four source slots");
            q->selectPreset(2, true);        // the handover: this is where it used to die
            q->servePresetRequests();
            feed(*q, buf, static_cast<int>(3.0 * sr / block), false);
            check(q->engine().activeVoices() > 0, "a preset change under a full cluster survives and goes on sounding");
        }
        check(p->morphingTo() < 0 && p->morphingFrom() < 0 && p->morphProgress() >= 1.0f,
              "when the time is up the change has arrived and nothing travels");
        p->releaseResources();
    }

    if (failures == 0) std::printf("hosttest: all checks passed\n");
    else std::printf("hosttest: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
