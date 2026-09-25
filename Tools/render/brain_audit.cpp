/**
 * @file brain_audit.cpp
 * @brief The conductor alone, for an hour, measured -- the rule book's own check (its section 11).
 *
 * @code
 *   ambient_brain_audit [--preset "name"] [--set key=value]... [--hours 1] [--dt 0.005] [--seed N]
 * @endcode
 *
 * Prints one line per event, E,\<seconds\>,\<conductor 1|2\>,\<on 1|0\>,\<note\>,\<velocity\>, and one per
 * root change, R,\<seconds\>,\<root\>. Tools/library/brain_audit.py turns that into the table of
 * section 11, the anti-rules of section 12 and the roles of table 2. No audio is rendered: the
 * engine's conductors are stepped at the control rate with the parameters exactly as a render
 * would read them, which is why this is an engine method and not a copy of its wiring.
 */
#include "ambient/Engine.h"
#include "ambient/Params.h"
#include "ambient/Presets.h"
#include "ambient/ClusterBrain.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace ambient;

/**
 * @brief Reads the options, sets the engine up as a render would, and runs Engine::auditConductor for the
 *        hours asked, printing the E and R lines (and the T lines under AMBIENT_BRAIN_TRACE) to stdout.
 * @param argc  argument count, as the runtime hands it over
 * @param argv  the options listed in the file header; a bad or unknown one is reported on stderr
 * @return 0 when the audit ran, 2 for a command-line error (unknown option, preset or parameter,
 *         a non-positive --hours or --dt)
 */
int main(int argc, char** argv)
{
    double hours = 1.0, dt = 0.005;
    long long seed = -1;
    std::string presetName;
    std::vector<std::pair<std::string, std::string>> sets;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--preset") presetName = next();
        else if (a == "--hours") hours = std::atof(next().c_str());
        else if (a == "--dt") dt = std::atof(next().c_str());
        else if (a == "--seed") seed = std::atoll(next().c_str());
        else if (a == "--set") {
            const std::string kv = next();
            const size_t eq = kv.find('=');
            if (eq == std::string::npos) { std::fprintf(stderr, "bad --set %s\n", kv.c_str()); return 2; }
            sets.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
        }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (dt <= 0.0 || hours <= 0.0) { std::fprintf(stderr, "hours and dt must be positive\n"); return 2; }
    loadDefaultPresetPacks();                  // $AMBIENT_PACKS, as the render tool
    auto engine = std::make_unique<Engine>();  // an Engine is large; the heap, not the stack
    engine->prepare(48000.0, 256);
    int presetIndex = -1;
    if (!presetName.empty()) {
        for (int p = 0; p < numPresets(); ++p) if (presetName == preset(p).name) presetIndex = p;
        if (presetIndex < 0) { std::fprintf(stderr, "unknown preset '%s'\n", presetName.c_str()); return 2; }
        engine->applyPreset(presetIndex);
    }
    for (const auto& kv : sets) {
        const ParamDesc* d = findParam(kv.first.c_str());
        if (!d) { std::fprintf(stderr, "unknown parameter '%s'\n", kv.first.c_str()); return 2; }
        engine->setParam(d->id, paramValueFromText(*d, kv.second.c_str()));
    }
    if (seed >= 0) engine->setParam(ParamId::Seed, static_cast<float>(seed));
    std::printf("# preset %s\n", presetIndex >= 0 ? preset(presetIndex).name : "(defaults)");
    // With AMBIENT_BRAIN_TRACE set, every stamp of the constellation memory and every verdict it
    // gives is printed as T,<seconds>,<what>,<note>,<key>: how a repeat is traced to its cause.
    static double now = 0.0;
    if (std::getenv("AMBIENT_BRAIN_TRACE"))
        ClusterBrain::trace = [](const char* what, int note, uint32_t key) { std::printf("T,%.4f,%s,%d,%u\n", now, what, note, key); };
    // The pitch class is the one the conductor itself hears -- from the note's frequency in the
    // instrument's own tuning, which is what the constellation memory keys -- not the MIDI number's.
    // With a just scale on D the two differ for some keys, and an audit that counted MIDI numbers
    // reported constellations coming back that never did.
    Engine* eng = engine.get();
    engine->auditConductor(hours * 3600.0, dt,
        [eng](double t, int which, const BrainEvent& e) {
            now = t;
            const int pc = pitchClassOf(eng->frequencyOf(e.note));
            std::printf("E,%.4f,%d,%d,%d,%.3f,%d\n", t, which, e.type == BrainEvent::Type::NoteOn ? 1 : 0, e.note, static_cast<double>(e.velocity), pc);
        },
        [](double t, int root) { now = t; std::printf("R,%.4f,%d\n", t, root); });
    return 0;
}
