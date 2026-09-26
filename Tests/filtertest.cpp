/**
 * @file filtertest.cpp
 * @brief ambient_filtertest: the circuit filters' and the oversamplers' checks, once per vector path.
 *
 * The circuit models (CircuitFilter.h, 26.09.2026) run both channels in the lanes of one register,
 * F4, which is NEON on the Quest, SSE on the desktop and four floats anywhere else; the oversamplers'
 * sums (Oversample.h) are AVX2, NEON or four scalar accumulators. The selftest only ever runs the
 * desktop's paths. So, as for the convolver and the partial bank, Tests/CMakeLists.txt builds this
 * file three ways on x86 -- as the core is built (SSE lanes, AVX2 sums), through the NEON path on
 * the x86 shim, and scalar -- and the Android build of it is the real NEON path. Each variant says
 * which paths it expects, and a variant that did not get them fails.
 *
 * What is checked on every path:
 *   - the lanes compute what one float computes: every model through F4, a different signal in
 *     lane 0 and lane 1, against the `float` instantiation of the same template on each signal;
 *   - the sound: the Prophet sings on its own on Cutoff (440 Hz within 4 per cent) and the SEM at
 *     Morph 0.5 is a notch on Cutoff (more than 20 dB down), through VoiceFilter;
 *   - the sums: halfband::dot against a plain sum in double, and both oversamplers around nothing
 *     pass a 1 kHz sine at unity within 0.05 dB.
 */
#include "ambient/Filter.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

using namespace ambient;

static int failures = 0;   ///< how many CHECKs failed so far; decides the exit code

/**
 * @brief Records one check: prints a FAIL line with the file and line and counts it when @p cond is false.
 * @param cond  the condition that has to hold
 * @param msg   what was measured, as the FAIL line prints it
 */
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++failures; } } while (0)

namespace {

/**
 * @brief A chord of saws and a little noise, the same every run.
 * @param n      how many samples
 * @param seed   the noise's seed
 * @param level  the chord's scale: 0.5 is a quiet voice, 1.5 one that drives the circuits
 * @return       the signal
 */
std::vector<float> signal(int n, unsigned seed, float level)
{
    std::vector<float> x(static_cast<size_t>(n));
    unsigned s = seed;
    for (int i = 0; i < n; ++i) {
        s = s * 1664525u + 1013904223u;
        float v = 0.3f * (static_cast<float>(s >> 8) / 16777216.0f - 0.5f);
        for (float f : { 110.0f, 164.8f, 220.0f }) v += 2.0f * std::fmod(f * static_cast<float>(i) / 96000.0f, 1.0f) - 1.0f;
        x[static_cast<size_t>(i)] = level * v;
    }
    return x;
}

/**
 * @brief One model through F4 against the float template on each lane.
 * @param model  a generic callable `V(V* v, V* s, V x)` running one circuit, called with F4 and with float
 * @return       the largest difference between a lane and its float, relative to the largest output
 */
template <class Model>
float lanesAgainstFloat(Model&& model)
{
    using circuit::F4;
    const int n = 9600;
    const std::vector<float> a = signal(n, 1u, 0.5f), b = signal(n, 7u, 1.5f);
    F4 v4[4] = {}, s4[4] = {};
    float va[4] = {}, sa[4] = {}, vb[4] = {}, sb[4] = {};
    float worst = 0.0f, peak = 1e-9f;
    for (int i = 0; i < n; ++i) {
        float l, r;
        circuit::unpack(model(v4, s4, circuit::pack(a[static_cast<size_t>(i)], b[static_cast<size_t>(i)])), l, r);
        const float ya = model(va, sa, a[static_cast<size_t>(i)]);
        const float yb = model(vb, sb, b[static_cast<size_t>(i)]);
        worst = std::max(worst, std::max(std::fabs(l - ya), std::fabs(r - yb)));
        peak = std::max(peak, std::max(std::fabs(ya), std::fabs(yb)));
    }
    return worst / peak;
}

/**
 * @brief The settled peak of a sine through a filter, in dB against its input.
 * @param f    the filter, set
 * @param hz   the sine's frequency
 * @param amp  its amplitude
 * @return     the gain in dB
 */
float gainDb(VoiceFilter& f, float hz, float amp)
{
    const float sr = 48000.0f;
    const int n = static_cast<int>(sr);
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float x = amp * static_cast<float>(std::sin(6.283185307179586 * hz * i / sr));
        float l, r;
        f.tick(x, x, l, r);
        if (i > n / 2) peak = std::max(peak, std::fabs(l));
    }
    return 20.0f * std::log10(std::max(peak, 1e-9f) / amp);
}

/**
 * @brief The frequency of a ringing tone from its rising zero crossings.
 * @param y   the tone
 * @param sr  its sample rate
 * @return    the frequency in Hz, 0 without two crossings
 */
double freqOf(const std::vector<float>& y, double sr)
{
    double first = -1.0, last = -1.0;
    int count = 0;
    for (size_t i = 1; i < y.size(); ++i)
        if (y[i - 1] < 0.0f && y[i] >= 0.0f) {
            const double t = static_cast<double>(i - 1) + y[i - 1] / static_cast<double>(y[i - 1] - y[i]);
            if (first < 0.0) first = t;
            last = t;
            ++count;
        }
    return count > 1 ? (count - 1) * sr / (last - first) : 0.0;
}

} // namespace

/**
 * @brief Names the paths this build took, checks them against AMBIENT_EXPECT_PATH ("lanes/sums"),
 *        and runs the checks.
 * @return 0 when every check passed, 1 otherwise
 */
int main()
{
    char path[64];
    std::snprintf(path, sizeof(path), "%s/%s", circuit::kLanePath, halfband::kDotPath);
    std::printf("circuit lanes / oversampler sums: %s\n", path);
#ifdef AMBIENT_EXPECT_PATH
    CHECK(std::strcmp(path, AMBIENT_EXPECT_PATH) == 0, "the build runs the vector paths it was made for");
#endif

    // The lanes against one float, every model; g for 1 kHz at twice 48 kHz, a resonance near the top.
    using circuit::F4;
    const float g = std::tan(3.14159265f * 1000.0f / 96000.0f);
    struct { const char* name; float err; } lanes[] = {
        { "Moog", lanesAgainstFloat([g](auto* v, auto* s, auto x) { using V = std::remove_reference_t<decltype(x)>;
            return circuit::ladderMoog(v, s, x, circuit::lanes<V>(g), circuit::lanes<V>(3.5f)); }) },
        { "Prophet", lanesAgainstFloat([g](auto* v, auto* s, auto x) { using V = std::remove_reference_t<decltype(x)>;
            return circuit::otaCascade(v, s, x, circuit::lanes<V>(g), circuit::lanes<V>(3.8f), 0.7f, 1.6f); }) },
        { "SEM", lanesAgainstFloat([g](auto* v, auto* s, auto x) { using V = std::remove_reference_t<decltype(x)>;
            return circuit::svfSem(v, s, x, circuit::lanes<V>(g), circuit::lanes<V>(0.05f), 0.7f); }) },
        { "Diode", lanesAgainstFloat([g](auto* v, auto* s, auto x) { using V = std::remove_reference_t<decltype(x)>;
            return circuit::diodeLadder(v, s, x, circuit::lanes<V>(g * 0.70710678f), circuit::lanes<V>(15.0f)); }) },
    };
    for (const auto& l : lanes) {
        std::printf("  %-8s lanes against one float: %.2e of the peak\n", l.name, l.err);
        CHECK(l.err < 1e-4f, (std::string(l.name) + ": the lanes compute what one float computes").c_str());
    }

    // The sound, through VoiceFilter.
    {
        VoiceFilter f;
        f.prepare(48000.0);
        f.set(FilterModel::Prophet, 440.0f, 1.0f, 0.0f);
        std::vector<float> tail;
        for (int i = 0; i < 120000; ++i) {
            const float x = i < 4 ? 0.5f : 0.0f;
            float l, r;
            f.tick(x, x, l, r);
            if (i >= 120000 - 9600) tail.push_back(l);
        }
        const double hz = freqOf(tail, 48000.0);
        std::printf("  Prophet at Resonance 1 sings at %.1f Hz\n", hz);
        CHECK(std::fabs(hz / 440.0 - 1.0) < 0.04, "the Prophet sings on its own on Cutoff");
        VoiceFilter sem;
        sem.prepare(48000.0);
        sem.set(FilterModel::Sem, 1000.0f, 0.0f, 0.0f, 0.5f);
        const float notch = gainDb(sem, 1000.0f, 0.1f);
        std::printf("  SEM at Morph 0.5: %.1f dB on Cutoff\n", notch);
        CHECK(notch < -20.0f, "the SEM at Morph 0.5 is a notch on Cutoff");
    }

    // The sums.
    {
        float h[32], v[32];
        unsigned s = 3u;
        for (int k = 0; k < 32; ++k) {
            s = s * 1664525u + 1013904223u; h[k] = static_cast<float>(s >> 8) / 16777216.0f - 0.5f;
            s = s * 1664525u + 1013904223u; v[k] = static_cast<float>(s >> 8) / 16777216.0f - 0.5f;
        }
        double want = 0.0, scale = 0.0;
        for (int k = 0; k < 32; ++k) { want += static_cast<double>(h[k]) * v[k]; scale += std::fabs(static_cast<double>(h[k]) * v[k]); }
        const double got = halfband::dot(h, v, 32);
        CHECK(std::fabs(got - want) < 1e-6 * scale, "halfband::dot adds what a plain sum adds");
        Oversampler4 os4;
        StereoOversampler2 os2;
        float peak4 = 0.0f, peak2 = 0.0f;
        for (int i = 0; i < 48000; ++i) {
            const float x = static_cast<float>(std::sin(6.283185307179586 * 1000.0 * i / 48000.0));
            const float y4 = os4.process(x, [](float u) { return u; });
            float l = x, r = x;
            os2.process(l, r, [](float a, float b, float& ya, float& yb) { ya = a; yb = b; });
            if (i > 24000) { peak4 = std::max(peak4, std::fabs(y4)); peak2 = std::max(peak2, std::fabs(l)); }
        }
        const float d4 = 20.0f * std::log10(peak4), d2 = 20.0f * std::log10(peak2);
        std::printf("  a 1 kHz sine around nothing: %+.3f dB four times, %+.3f dB twice\n", d4, d2);
        CHECK(std::fabs(d4) < 0.05f && std::fabs(d2) < 0.05f, "the oversamplers pass a sine at unity");
    }

    if (failures == 0) std::printf("filtertest: all checks passed\n");
    else std::printf("filtertest: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
