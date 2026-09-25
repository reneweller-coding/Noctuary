/**
 * @file racetest.cpp
 * @brief The handovers between threads, under load.
 *
 * The self test measures the instrument and the host test measures the plugin around it, and both
 * of them ask their questions one at a time. This one asks only one question, and asks it of two
 * threads at once: while the engine is rendering, everything a player can change from a window is
 * changed underneath it, as fast as it can be changed.
 *
 * It exists because of what a day of reading found. Nearly every serious fault in this instrument
 * has been a handover between the thread that renders and the thread that answers the mouse: a
 * buffer swapped while it was being read, a matrix announced before it was written, a flag that
 * said "in use" but meant "used last", an engine freed while the audio thread was still inside it.
 * Reading found them one at a time, and reading is the only tool that works on Windows: the
 * sanitizer that finds a data race by watching one happen exists for Linux and macOS and not for
 * this compiler. The core builds on Linux without a framework, though, so this program is the
 * workload that makes that sanitizer worth running:
 *
 * @code
 *   cmake -S . -B build-tsan -DAMBIENT_BUILD_PLUGIN=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo \
 *         -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
 *   cmake --build build-tsan -j
 *   setarch $(uname -m) -R ./build-tsan/Tests/ambient_racetest        # -R: the sanitizer needs the
 *                                                                     # address space left alone
 * @endcode
 *
 * On Windows it still earns its place: it is a stress test, and it says whether the sound survives
 * having everything changed under it and whether the audio thread keeps up while that happens.
 */
#include "ambient/Engine.h"
#include "ambient/Params.h"
#include "ambient/Tuning.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

using namespace ambient;

namespace {

int failures = 0;   ///< how many checks failed; decides the exit code

/**
 * @brief Records one check: prints a FAIL line and counts it when @p ok is false.
 * @param ok    the condition that has to hold
 * @param what  what was measured, as the FAIL line prints it
 */
void check(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

/**
 * @brief A short clip to hand to the sampler slots.
 *
 * Different lengths on purpose: the slot's buffers are
 * reassigned when the length changes, which is the moment that used to be unsafe.
 * @param n   length in samples
 * @param hz  the sine's frequency at 48 kHz
 * @return    n samples of a sine at 0.3 peak
 */
std::vector<float> clip(int n, float hz)
{
    std::vector<float> v(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) v[static_cast<size_t>(i)] = 0.3f * std::sin(kTwoPi * hz * i / 48000.0f);
    return v;
}

} // namespace

/**
 * @brief Runs the two threads against one Engine for the seconds asked and reports.
 *
 * The audio thread renders 128-sample blocks at 48 kHz with notes coming and going; the control
 * thread cycles through every handover a window can make (all knobs, clips into slots, the matrix,
 * envelope shapes, a user scale, a user wavetable). Passes when both threads ran and no sample was
 * ever non-finite; the real-time factor is printed, not judged.
 * @param argc  argument count, as the runtime hands it over
 * @param argv  argv[1], if given: how many seconds to run (default 6; ctest runs it with 4)
 * @return 0 when every check passed, 1 otherwise
 */
int main(int argc, char** argv)
{
    const double seconds = argc > 1 ? std::atof(argv[1]) : 6.0;
    const int block = 128;

    Engine engine;
    engine.prepare(48000.0, block);

    std::atomic<bool> stop { false };
    std::atomic<long long> blocks { 0 }, changes { 0 };
    std::atomic<bool> sawNonFinite { false };

    // The audio thread: nothing but render, the way a host calls it.
    std::thread audio([&] {
        std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block));
        int note = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            // Notes come and go the whole time, so voices are being started and ended while the
            // rest of it is being changed.
            if ((blocks.load(std::memory_order_relaxed) % 64) == 0) {
                engine.noteOff(45 + (note % 12));
                ++note;
                engine.noteOn(45 + (note % 12), 0.8f);
            }
            engine.process(L.data(), R.data(), block);
            for (int i = 0; i < block; ++i)
                if (!std::isfinite(L[static_cast<size_t>(i)]) || !std::isfinite(R[static_cast<size_t>(i)]))
                    sawNonFinite.store(true, std::memory_order_relaxed);
            blocks.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // The message thread: everything a window can do, as fast as it can be done. Each of these is
    // a handover with its own protocol, and each of those protocols has been wrong at least once.
    std::thread control([&] {
        const std::vector<float> a = clip(4096, 220.0f), b = clip(9973, 330.0f);   // one length is not a power of two on purpose
        std::vector<float> table(2048 * 4);
        for (size_t i = 0; i < table.size(); ++i)
            table[i] = std::sin(kTwoPi * static_cast<float>(i) / 2048.0f) * (1.0f - static_cast<float>(i) / table.size());
        FixedScale scale;
        unsigned s = 12345;
        auto rnd = [&s] { s = s * 1664525u + 1013904223u; return static_cast<float>((s >> 8) & 0xFFFF) / 65535.0f; };
        int turn = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            switch (turn++ % 8) {
            case 0: {   // every knob, at a value it has never held
                for (const ParamDesc& d : paramTable())
                    engine.setParam(d.id, d.min + (d.max - d.min) * rnd());
                break;
            }
            case 1:   // a clip into a slot: the double buffer and its "in use" flag
                engine.setTexture(turn % kSlots, a.data(), static_cast<int>(a.size()), 48000.0, 220.0, false);
                break;
            case 2:
                engine.setTexture(turn % kSlots, b.data(), static_cast<int>(b.size()), 44100.0, 330.0, true);
                break;
            case 3:
                engine.clearTexture(turn % kSlots);
                break;
            case 4:   // the modulation matrix: parsed here, copied there
                engine.setModMatrixText(turn % 2 ? "lfo1>cutoff:0.5;env2>z_x:-0.3:macro_a"
                                                 : "lfo3>drift:0.2;brain>partials:0.4");
                break;
            case 5:   // the envelope shapes, the six and the sources' own, under the same lock
                engine.setEnvShape(turn % kNumModEnvs, turn % 2 ? "0:0/1:1/3:0.2!s1" : "0:1/2:-1/4:0");
                engine.setSrcEnvShape(turn % kSlots, turn % 2 ? "0:0/2:1!s1" : "0:1/1:0.5/3:0");
                break;
            case 6:   // a tuning of the player's own
                if (parseScala("! r.scl\nRace\n 3\n 100.0\n 3/2\n 2/1\n", scale)) engine.setUserScale(scale);
                break;
            case 7:   // a wavetable of the player's own
                engine.loadUserWavetable(table.data(), static_cast<int>(table.size()), 2048);
                break;
            }
            changes.fetch_add(1, std::memory_order_relaxed);
        }
    });

    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stop.store(true, std::memory_order_relaxed);
    audio.join();
    control.join();

    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double audioSecs = static_cast<double>(blocks.load()) * block / 48000.0;
    std::printf("  race: %lld blocks (%.1f s of audio) against %lld changes in %.1f s -- %.2fx real time\n",
                blocks.load(), audioSecs, changes.load(), wall, audioSecs / wall);

    check(blocks.load() > 0 && changes.load() > 0, "both threads actually ran");
    check(!sawNonFinite.load(), "the sound stays a number while everything is changed under it");

    std::printf(failures == 0 ? "racetest: all checks passed\n" : "racetest: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
