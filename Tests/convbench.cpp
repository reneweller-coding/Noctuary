/**
 * @file convbench.cpp
 * @brief ambient_convbench: the convolution room's cost, measured by hand.
 *
 * The convolution room's cost, measured by hand: CPU time per second of audio, and the worst and
 * the 99.9th-percentile callback, for impulse lengths from 4 s to a minute -- stereo, mono, and a
 * stereo morph between two rooms. Release build only; nothing here passes or fails.
 *
 * @code
 *   ambient_convbench [callback samples = 128] [seconds of audio = 20] [longest impulse = 60]
 * @endcode
 *
 * Every case first runs one impulse length untimed, so the frequency-domain delay lines are full
 * and the cost is the steady one, not the cheap start.
 */
#include "ambient/Convolution.h"
#include "ambient/Dsp.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

using namespace ambient;

namespace {

/**
 * @brief A room-like impulse: three bands of decorrelated noise, each with its own decay, so the top is
 *        gone long before the bottom as in a real hall.
 *
 * rt60 is the low band's; the others are shorter.
 * @param seed  seeds the noise, so that the same seed is the same room every run
 * @param n     length in samples
 * @param sr    sample rate in Hz, for the band splits at 300 and 3000 Hz and the decays
 * @param rt60  decay to -60 dB of the low band in seconds; the mid band gets 0.6 of it, the top 0.25
 * @return      the n samples, unnormalised (the Convolver normalises the energy itself)
 */
std::vector<float> makeImpulse(uint64_t seed, int n, float sr, float rt60)
{
    Rng rng; rng.seed(seed);
    std::vector<float> h(static_cast<size_t>(n));
    float lp1 = 0.0f, lp2 = 0.0f;
    const float a1 = 1.0f - std::exp(-kTwoPi * 300.0f / sr), a2 = 1.0f - std::exp(-kTwoPi * 3000.0f / sr);
    for (int i = 0; i < n; ++i) {
        const float w = rng.bipolar();
        lp1 += a1 * (w - lp1);
        lp2 += a2 * (w - lp2);
        const float t = static_cast<float>(i) / sr;
        h[static_cast<size_t>(i)] = lp1 * std::pow(10.0f, -3.0f * t / rt60)
                                  + (lp2 - lp1) * std::pow(10.0f, -3.0f * t / (0.6f * rt60))
                                  + (w - lp2) * std::pow(10.0f, -3.0f * t / (0.25f * rt60));
    }
    return h;
}

/** @brief What one case measured: the three numbers of one row of the table. */
struct Result { double cpuPercent, worstMs, p999Ms; };
/** @var Result::cpuPercent
 *  CPU time spent in the callbacks as a percentage of the audio time they rendered
 */
/** @var Result::worstMs
 *  the slowest single callback, in milliseconds
 */
/** @var Result::p999Ms
 *  the 99.9th-percentile callback, in milliseconds
 */

/**
 * @brief Times one case: an impulse of @p len seconds through a Convolver, callback by callback.
 *
 * kind 0: stereo impulse, 1: mono impulse, 2: stereo morph halfway between two stereo impulses
 *
 * The convolver is first run for one impulse length plus four callbacks untimed (see the file
 * header), then for @p seconds of audio with every callback timed by the steady clock.
 * @param kind        the case, as above
 * @param len         impulse length in seconds; the rooms decay with rt60 = len / 2
 * @param maxSeconds  the longest impulse the Convolver is prepared for (what the Room keeps)
 * @param block       samples per callback
 * @param seconds     seconds of audio to time
 * @param sr          sample rate in Hz
 * @return            CPU percentage, worst and 99.9th-percentile callback of the timed part
 */
Result run(int kind, float len, float maxSeconds, int block, double seconds, float sr)
{
    const int n = static_cast<int>(len * sr);
    const auto L = makeImpulse(1, n, sr, 0.5f * len), R = makeImpulse(2, n, sr, 0.5f * len);
    Convolver a;
    a.prepare(sr, maxSeconds);
    a.setImpulse(L.data(), kind == 1 ? nullptr : R.data(), n, sr);
    if (kind == 2) {
        const auto L2 = makeImpulse(3, n, sr, 0.5f * len), R2 = makeImpulse(4, n, sr, 0.5f * len);
        a.setImpulseB(L2.data(), R2.data(), n, sr);
        a.setMorph(0.5f);
    }
    std::vector<float> inL(static_cast<size_t>(block)), inR(inL), outL(inL), outR(inL);
    Rng rng; rng.seed(9);
    auto callback = [&] {
        for (int i = 0; i < block; ++i) { inL[static_cast<size_t>(i)] = 0.25f * rng.bipolar(); inR[static_cast<size_t>(i)] = 0.25f * rng.bipolar(); }
        a.process(inL.data(), inR.data(), outL.data(), outR.data(), block);
    };
    const long warm = static_cast<long>(std::ceil(len * sr / block)) + 4;
    for (long k = 0; k < warm; ++k) callback();
    const long calls = static_cast<long>(seconds * sr / block);
    std::vector<float> ms(static_cast<size_t>(calls));
    double total = 0.0;
    for (long k = 0; k < calls; ++k) {
        const auto t0 = std::chrono::steady_clock::now();
        callback();
        const double d = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ms[static_cast<size_t>(k)] = static_cast<float>(d);
        total += d;
    }
    Result r;
    r.cpuPercent = 100.0 * (total / 1000.0) / (calls * block / static_cast<double>(sr));
    r.worstMs = *std::max_element(ms.begin(), ms.end());
    const size_t at = std::min(ms.size() - 1, static_cast<size_t>(0.999 * ms.size()));
    std::nth_element(ms.begin(), ms.begin() + static_cast<long>(at), ms.end());
    r.p999Ms = ms[at];
    return r;
}

} // namespace

/**
 * @brief Runs the three cases for every impulse length up to the longest asked and prints the table.
 * @param argc  argument count, as the runtime hands it over
 * @param argv  optional, in order: callback samples (at least 16, default 128), seconds of audio per
 *              case (default 20), longest impulse in seconds (default 60; the 4, 8, 12, 30, 60 s rows
 *              beyond it are skipped)
 * @return 0 always; the numbers are the result
 */
int main(int argc, char** argv)
{
#if defined(_M_X64) || defined(__x86_64__)
    _mm_setcsr(_mm_getcsr() | 0x8040);   // flush-to-zero and denormals-are-zero, as a host's ScopedNoDenormals sets them
#endif
    const int block = argc > 1 ? std::max(16, std::atoi(argv[1])) : 128;
    const double seconds = argc > 2 ? std::atof(argv[2]) : 20.0;
    const float longest = argc > 3 ? static_cast<float>(std::atof(argv[3])) : 60.0f;
    const float sr = 48000.0f;
    std::printf("callback %d samples (%.2f ms), %.0f s of audio per case\n", block, 1000.0 * block / sr, seconds);
    std::printf("%7s  %-14s %8s %10s %10s\n", "impulse", "case", "cpu %", "worst ms", "p99.9 ms");
    const char* const names[3] = { "stereo", "mono", "stereo morph" };
    for (float len : { 4.0f, 8.0f, 12.0f, 30.0f, 60.0f }) {
        if (len > longest) break;
        for (int kind = 0; kind < 3; ++kind) {
            const Result r = run(kind, len, longest, block, seconds, sr);
            std::printf("%6.0f s  %-14s %8.2f %10.3f %10.3f\n", len, names[kind], r.cpuPercent, r.worstMs, r.p999Ms);
            std::fflush(stdout);
        }
    }
    return 0;
}
