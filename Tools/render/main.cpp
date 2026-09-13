// ambient_render -- offline renderer for Noctuary (no JUCE, deterministic).
// Renders the engine to a 32-bit float WAV and prints measurements, so a patch
// can be judged by numbers instead of by ear.
//
//   ambient_render [--out file.wav] [--seconds 60] [--sr 48000] [--block 256]
//                  [--preset "name"] [--set key=value]... [--notes 45,52,59]
//                  [--scl file.scl] [--stats] [--list] [--list-presets]
//                  [--texture file.wav [baseHz]] [--wavetable file.wav]
//                  [--mod "lfo1>cutoff:0.4;..."] [--env 1 "0:0/2:1/8:0"] [--src-env 2 "0:0/1:1"]
//                  [--map x y [radius]] (render the preset-map blend at a cursor) [--dump] (print all parameters)
//                  [--ir impulse.wav] (convolution room impulse, mono or stereo) [--ir-b impulse.wav] (Room Morph's second)
//                  [--write-default-ir file.wav] (the Room's built-in hall as a WAV at --sr, then exit)
//                  [--route "name or text" [speed]] (walk a route over the map; --list-routes)
//                  [--set-file set.ambientset] (replay a recorded set; length = set + 20 s unless --seconds)
#include "ambient/Engine.h"
#include "ambient/Params.h"
#include "ambient/Presets.h"
#include "ambient/WavFile.h"
#include "ambient/Sources.h"
#include "ambient/PresetMap.h"
#include "ambient/Timeline.h"
#include "ambient/Score.h"
#include "ambient/Journey.h"
#include "ambient/Cosmos.h"          // Fft, for the tonal probe
#include "ambient/ClusterBrain.h"   // peakRoughness: one Plomp-Levelt curve for the conductor and the map
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <cctype>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <memory>
#include <filesystem>

using namespace ambient;

namespace {

// The near source's clip from a file, or a pool of them from a folder (sorted by name, up to 48,
// each kept to twenty seconds -- phrases, not beds): the render's twin of the plugin's loader.
bool loadNearClips(Engine& engine, const std::string& path)
{
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        std::vector<std::string> files;
        for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".flac" || ext == ".wav" || ext == ".aif" || ext == ".aiff") files.push_back(entry.path().string());
        }
        std::sort(files.begin(), files.end());
        // More than the pool holds: every k-th of them, the whole folder represented (as the plugin does).
        const size_t kMax = 48;
        if (files.size() > kMax) {
            std::vector<std::string> some;
            for (size_t i = 0; i < kMax; ++i) some.push_back(files[static_cast<size_t>(std::lround(i * (files.size() - 1.0) / (kMax - 1.0)))]);
            files.swap(some);
        }
        std::vector<Texture> pool;
        for (const std::string& f : files) {
            std::vector<std::vector<float>> ch; int rate = 0;
            if (!readWavChannels(f.c_str(), ch, rate) || ch.empty()) continue;
            Texture t = Engine::makeTexture(ch[0].data(), ch.size() > 1 ? ch[1].data() : nullptr, static_cast<int>(ch[0].size()), rate,
                                            261.6256, loopFromName(f.c_str()), 20.0);
            if (!t.empty()) pool.push_back(std::move(t));
        }
        if (pool.empty()) return false;
        std::printf("near clips: %d of %s\n", static_cast<int>(pool.size()), path.c_str());
        engine.setNearTextures(std::move(pool));
        return true;
    }
    std::vector<std::vector<float>> ch; int rate = 0;
    if (!readWavChannels(path.c_str(), ch, rate) || ch.empty()) return false;
    engine.setNearTexture(ch[0].data(), ch.size() > 1 ? ch[1].data() : nullptr, static_cast<int>(ch[0].size()), rate,
                          261.6256, loopFromName(path.c_str()));
    std::printf("near clip: %s\n", path.c_str());
    return true;
}

bool writeWav(const std::string& path, const std::vector<float>& interleaved, int channels, int sampleRate)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t dataBytes = static_cast<uint32_t>(interleaved.size() * sizeof(float));
    f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(3); u16(static_cast<uint16_t>(channels));
    u32(static_cast<uint32_t>(sampleRate)); u32(static_cast<uint32_t>(sampleRate * channels * 4));
    u16(static_cast<uint16_t>(channels * 4)); u16(32);
    f.write("data", 4); u32(dataBytes);
    f.write(reinterpret_cast<const char*>(interleaved.data()), dataBytes);
    return static_cast<bool>(f);
}

} // namespace

// ---------------------------------------------------------------- --measure
//
// Renders and prints the descriptors instead of writing a file. The measurement pass over the
// library used to render each preset to a temporary WAV and read it straight back: five thousand
// presets times twelve seconds of stereo float is twenty-three gigabytes written and read again
// for nothing, and all of it stayed in the file cache afterwards.
namespace {

// In-place radix-2 FFT on interleaved real/imag, n a power of two.
void fft(std::vector<float>& re, std::vector<float>& im)
{
    const int n = static_cast<int>(re.size());
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * 3.14159265358979323846 / len;
        const float wr = static_cast<float>(std::cos(ang)), wi = static_cast<float>(std::sin(ang));
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; ++k) {
                const int a = i + k, b = i + k + len / 2;
                const float xr = re[b] * cr - im[b] * ci;
                const float xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr;        im[a] += xi;
                const float nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr; cr = nr;
            }
        }
    }
}

// Centroid, flatness, flux, low-band share, stereo width and level of the settled half -- and,
// for the preset map, three descriptors that say what a drone is like rather than what a note is
// like: how much it changes over a minute, how rough its spectrum is, and how wet it stands.
// stemE holds the energy of the four buses (near, far, cosmos, room) or is null.
void printMeasurements(const std::vector<float>& L, const std::vector<float>& R, int sr, double voices,
                       const double* stemE = nullptr)
{
    const size_t n = L.size();
    if (n < 4096) { std::printf("measure: rms=-120 centroid=0 flatness=0 flux=0 bass=0 width=0 voices=%.2f peak=0 jump=0 dc=0 monoloss=0\n", voices); return; }
    const size_t half = n / 2;
    const int win = 2048, hop = 1024;
    std::vector<float> re(win), im(win), mag(win / 2 + 1), prev(win / 2 + 1, 0.0f), hann(win);
    for (int i = 0; i < win; ++i) hann[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(6.28318530718f * i / (win - 1));
    double centroid = 0.0, flatness = 0.0, flux = 0.0, bass = 0.0;
    int frames = 0;
    std::vector<double> avgMag(static_cast<size_t>(win / 2 + 1), 0.0);   // for the roughness
    for (size_t start = half; start + static_cast<size_t>(win) < n; start += static_cast<size_t>(hop)) {
        for (int i = 0; i < win; ++i) {
            re[static_cast<size_t>(i)] = 0.5f * (L[start + static_cast<size_t>(i)] + R[start + static_cast<size_t>(i)]) * hann[static_cast<size_t>(i)];
            im[static_cast<size_t>(i)] = 0.0f;
        }
        fft(re, im);
        double sum = 0.0, wsum = 0.0, logsum = 0.0, low = 0.0, d = 0.0;
        for (int k = 0; k <= win / 2; ++k) {
            const float m = std::sqrt(re[static_cast<size_t>(k)] * re[static_cast<size_t>(k)] + im[static_cast<size_t>(k)] * im[static_cast<size_t>(k)]) + 1e-12f;
            mag[static_cast<size_t>(k)] = m;
            const double f = static_cast<double>(k) * sr / win;
            const double p = static_cast<double>(m) * m;
            sum += p; wsum += p * f; logsum += std::log(static_cast<double>(m));
            if (f < 150.0) low += p;
        }
        if (frames > 0) {
            double na = 0.0, nb = 0.0;
            for (int k = 0; k <= win / 2; ++k) { na += mag[static_cast<size_t>(k)]; nb += prev[static_cast<size_t>(k)]; }
            for (int k = 0; k <= win / 2; ++k)
                d += std::fabs(mag[static_cast<size_t>(k)] / std::max(na, 1e-12) - prev[static_cast<size_t>(k)] / std::max(nb, 1e-12));
            flux += d;
        }
        prev = mag;
        for (int k = 0; k <= win / 2; ++k) avgMag[static_cast<size_t>(k)] += mag[static_cast<size_t>(k)];
        centroid += wsum / std::max(sum, 1e-20);
        flatness += std::exp(logsum / (win / 2 + 1)) / std::max(sum > 0.0 ? std::sqrt(sum / (win / 2 + 1)) : 1e-12, 1e-12);
        bass += low / std::max(sum, 1e-20);
        ++frames;
    }
    const double inv = 1.0 / std::max(frames, 1);
    double sq = 0.0, ml = 0.0, mr = 0.0, vl = 0.0, vr = 0.0, cov = 0.0;
    for (size_t i = half; i < n; ++i) { sq += L[i] * L[i] + R[i] * R[i]; ml += L[i]; mr += R[i]; }
    const double cnt = static_cast<double>(n - half);
    ml /= cnt; mr /= cnt;
    for (size_t i = half; i < n; ++i) { const double a = L[i] - ml, b = R[i] - mr; vl += a * a; vr += b * b; cov += a * b; }
    const double corr = (vl > 1e-18 && vr > 1e-18) ? cov / std::sqrt(vl * vr) : 1.0;
    const double rms = std::sqrt(sq / (2.0 * cnt));
    // The sound test's numbers as well: peak, the largest sample-to-sample step (a click), the
    // offset of the settled half, and how much level the mono sum loses against the stereo signal.
    double peak = 0.0, jump = 0.0, monoSq = 0.0, stSq = 0.0;
    float pl = 0.0f, pr = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const float l = L[i], r = R[i];
        peak = std::max(peak, static_cast<double>(std::max(std::fabs(l), std::fabs(r))));
        const float m = 0.5f * (l + r), pm = 0.5f * (pl + pr);
        if (i > 0) jump = std::max(jump, static_cast<double>(std::fabs(m - pm)));
        pl = l; pr = r;
        if (i >= half) { monoSq += m * m; stSq += 0.5 * (l * l + r * r); }
    }
    const double dc = std::fabs(0.5 * (ml + mr));
    const double monoLoss = 20.0 * std::log10((std::sqrt(stSq / cnt) + 1e-12) / (std::sqrt(monoSq / cnt) + 1e-12));
    // A fingerprint of the audio itself. The descriptors are averages: two renders can agree on
    // every one of them and still not be the same sound. The samples are quantised to about
    // -120 dB first, so this catches a change in what is played and not the last bit of a sum
    // (the vectorised partial bank moves those, and moving them is not a change).
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
        const int32_t q[2] = { static_cast<int32_t>(std::lround(L[i] * 1048576.0f)),
                               static_cast<int32_t>(std::lround(R[i] * 1048576.0f)) };
        for (int k = 0; k < 2; ++k)
            for (int b = 0; b < 4; ++b) { h ^= static_cast<uint64_t>((q[k] >> (b * 8)) & 0xff); h *= 1099511628211ull; }
    }
    // ---- what a drone is like -------------------------------------------------------------
    // Evolution: how far the sound travels over the whole render, not from one block to the next.
    // A second at a time, the level in decibels and the centroid in octaves; the spread of those
    // two series is what separates a preset that stands still from one that goes somewhere.
    // Flux cannot say this -- a fast tremolo has flux and goes nowhere.
    double evoTone = 0.0, evoLevel = 0.0;
    {
        const size_t sec = static_cast<size_t>(sr);
        const int fftN = 4096;
        std::vector<double> tone, level;
        std::vector<float> fr(static_cast<size_t>(fftN)), fi(static_cast<size_t>(fftN));
        for (size_t s = 0; s + sec <= n; s += sec) {
            double e = 0.0;
            for (size_t i = s; i < s + sec; ++i) e += static_cast<double>(L[i]) * L[i] + static_cast<double>(R[i]) * R[i];
            level.push_back(10.0 * std::log10(e / (2.0 * static_cast<double>(sec)) + 1e-20));
            if (s + static_cast<size_t>(fftN) > n) continue;
            for (int i = 0; i < fftN; ++i) {
                const double w = 0.5 - 0.5 * std::cos(6.28318530718 * i / (fftN - 1));
                fr[static_cast<size_t>(i)] = static_cast<float>(0.5 * (L[s + static_cast<size_t>(i)] + R[s + static_cast<size_t>(i)]) * w);
                fi[static_cast<size_t>(i)] = 0.0f;
            }
            fft(fr, fi);
            double su = 0.0, ws = 0.0;
            for (int k = 1; k <= fftN / 2; ++k) {
                const double m = std::sqrt(static_cast<double>(fr[static_cast<size_t>(k)]) * fr[static_cast<size_t>(k)] + static_cast<double>(fi[static_cast<size_t>(k)]) * fi[static_cast<size_t>(k)]);
                const double p = m * m, f = static_cast<double>(k) * sr / fftN;
                su += p; ws += p * f;
            }
            tone.push_back(std::log2(std::max(ws / std::max(su, 1e-20), 20.0)));
        }
        auto spread = [](const std::vector<double>& v) {
            if (v.size() < 3) return 0.0;
            double m = 0.0;
            for (double x : v) m += x;
            m /= static_cast<double>(v.size());
            double s = 0.0;
            for (double x : v) s += (x - m) * (x - m);
            return std::sqrt(s / static_cast<double>(v.size()));
        };
        // The loudest second is dropped from the level series: a preset whose first note arrives
        // late otherwise reads as "evolving" when all it did was start.
        evoTone = spread(tone);
        evoLevel = spread(level);
    }
    // Roughness: the Plomp-Levelt curve over the peaks of the average spectrum, the same one the
    // conductor judges its chords with (ambient::peakRoughness). Smooth and fused against
    // beating and grinding -- the axis a drone lives on.
    double rough = 0.0;
    {
        double pf[32] = {}, pa[32] = {};
        int np = 0;
        double loudest = 0.0;
        for (int k = 1; k < win / 2; ++k) loudest = std::max(loudest, avgMag[static_cast<size_t>(k)]);
        for (int k = 2; k < win / 2 - 1 && np < 32; ++k) {
            const double m = avgMag[static_cast<size_t>(k)];
            if (m < loudest * 0.02) continue;
            if (m <= avgMag[static_cast<size_t>(k - 1)] || m < avgMag[static_cast<size_t>(k + 1)]) continue;
            pf[np] = static_cast<double>(k) * sr / win;
            pa[np] = m;
            ++np;
        }
        rough = ambient::peakRoughness(pf, pa, np);
    }
    // ---- the fingerprint --------------------------------------------------------------------
    // Nine numbers say what a preset is LIKE -- dark, still, wide, far. They cannot say what it
    // IS: a goods yard and a beehive can agree on every one of them. This is the usual answer to
    // that in the literature, a mel-cepstral fingerprint: forty bands on a mel scale, the log of
    // their energy, a discrete cosine transform of that, and the first sixteen coefficients kept
    // with their spread over the render. Thirty-two numbers that carry the shape of the spectrum
    // rather than its averages, computed from the frames the descriptors already cost.
    constexpr int kMel = 40, kCeps = 16;
    double mfccMean[kCeps] = {}, mfccVar[kCeps] = {};
    {
        auto hzToMel = [](double f) { return 2595.0 * std::log10(1.0 + f / 700.0); };
        auto melToHz = [](double m) { return 700.0 * (std::pow(10.0, m / 2595.0) - 1.0); };
        const double melLo = hzToMel(30.0), melHi = hzToMel(std::min(16000.0, sr * 0.45));
        int centre[kMel + 2];
        for (int b = 0; b < kMel + 2; ++b) {
            const double hz = melToHz(melLo + (melHi - melLo) * b / (kMel + 1));
            centre[b] = std::max(1, std::min(win / 2, static_cast<int>(hz * win / sr + 0.5)));
        }
        std::vector<double> ceps(static_cast<size_t>(kCeps), 0.0);
        std::vector<float> fre(static_cast<size_t>(win)), fim(static_cast<size_t>(win));
        int used = 0;
        for (size_t start = half; start + static_cast<size_t>(win) < n; start += static_cast<size_t>(hop) * 2) {
            for (int i = 0; i < win; ++i) {
                fre[static_cast<size_t>(i)] = 0.5f * (L[start + static_cast<size_t>(i)] + R[start + static_cast<size_t>(i)]) * hann[static_cast<size_t>(i)];
                fim[static_cast<size_t>(i)] = 0.0f;
            }
            fft(fre, fim);
            double band[kMel];
            for (int b = 0; b < kMel; ++b) {
                double e = 0.0;
                for (int k = centre[b]; k <= centre[b + 2]; ++k) {
                    const double m = std::sqrt(static_cast<double>(fre[static_cast<size_t>(k)]) * fre[static_cast<size_t>(k)]
                                             + static_cast<double>(fim[static_cast<size_t>(k)]) * fim[static_cast<size_t>(k)]);
                    const double w = k <= centre[b + 1] ? (k - centre[b] + 1.0) / (centre[b + 1] - centre[b] + 1.0)
                                                        : (centre[b + 2] - k + 1.0) / (centre[b + 2] - centre[b + 1] + 1.0);
                    e += m * m * w;
                }
                band[b] = std::log(e + 1e-12);
            }
            for (int c = 0; c < kCeps; ++c) {
                double s2 = 0.0;
                for (int b = 0; b < kMel; ++b)
                    s2 += band[b] * std::cos(3.14159265358979323846 * (c + 1) * (b + 0.5) / kMel);
                ceps[static_cast<size_t>(c)] = s2 / kMel;
                mfccMean[c] += ceps[static_cast<size_t>(c)];
                mfccVar[c] += ceps[static_cast<size_t>(c)] * ceps[static_cast<size_t>(c)];
            }
            ++used;
        }
        const double inv2 = 1.0 / std::max(used, 1);
        for (int c = 0; c < kCeps; ++c) {
            mfccMean[c] *= inv2;
            mfccVar[c] = std::sqrt(std::max(0.0, mfccVar[c] * inv2 - mfccMean[c] * mfccMean[c]));
        }
    }
    // Wet: how much of what is heard comes back from the far reverb, the room and the Cosmos
    // rather than standing in the near plane. The spatial model's own axis, near to far.
    double wet = 0.0;
    if (stemE != nullptr) {
        const double all = stemE[0] + stemE[1] + stemE[2] + stemE[3];
        if (all > 1e-18) wet = (stemE[1] + stemE[2] + stemE[3]) / all;
    }
    std::printf("measure: rms=%.3f centroid=%.1f flatness=%.6f flux=%.6f bass=%.6f width=%.6f voices=%.2f "
                "peak=%.4f jump=%.4f dc=%.5f monoloss=%.3f evo_tone=%.5f evo_level=%.4f rough=%.6f wet=%.5f hash=%016llx\n",
                20.0 * std::log10(rms + 1e-12), centroid * inv, flatness * inv,
                frames > 1 ? flux / (frames - 1) : 0.0, bass * inv, 1.0 - std::fabs(corr), voices,
                peak, jump, dc, monoLoss, evoTone, evoLevel, rough, wet, static_cast<unsigned long long>(h));
    // The fingerprint on its own line, so nothing that parses the measure line has to learn a
    // new field: 16 cepstral means, then their 16 spreads.
    std::printf("timbre:");
    for (int c = 0; c < kCeps; ++c) std::printf(" %.4f", mfccMean[c]);
    for (int c = 0; c < kCeps; ++c) std::printf(" %.4f", mfccVar[c]);
    std::printf("\n");
}

} // namespace

// One render, exactly as the command line asks for it. main() below calls this once, or once
// per preset when --batch is given.
// ---------------------------------------------------------------- does it follow the note?
//
// Plays the same two-note chord twice, a tritone apart, and asks how much of the sound moved with
// it. Everything a preset makes divides into two: material that is pitched to what is played -- the
// slots that follow the note, their partials, the filters riding on them -- and material that is
// not: the Foundation on the conductor's root, a Free sample bed, noise, the tail of a reverb. Only
// the first can be heard as harmony. A listener who plays a chord, then another, and hears the same
// thing both times is hearing a preset whose second kind drowns out its first.
//
// The overlap of the two normalised spectra is exactly that share: what sits at the same place in
// both is what did not move. 0 % means everything followed, 100 % means nothing did.
//
// The second number is where the weight is. The Foundation lives under about 130 Hz and the played
// material mostly above 150; their ratio in decibels says whether the bass is a foundation under
// the music or a lid on top of it.
//
// A tritone because it is the largest move in pitch class that keeps the register: a chord an
// octave up would leave a register-folded bass exactly where it was and look like a preset that
// does not follow, which is how the first version of this measurement fooled its author.
struct TonalProbe { double staticShare = 0.0, bassDb = 0.0, rms = -120.0; };

TonalProbe tonalProbe(Engine& engine, int sr, double seconds)
{
    constexpr int kFft = 1 << 15;
    const int block = 256;
    const long total = static_cast<long>(seconds * sr);
    const long from = total / 3;              // the settled part, as every other measurement here
    std::vector<double> spec[2];
    double energy = 0.0;
    long counted = 0;

    for (int pass = 0; pass < 2; ++pass) {
        const int lowNote = pass == 0 ? 57 : 63;   // A3, then D#4
        engine.allNotesOff();
        engine.reset();
        engine.noteOn(lowNote, 0.8f);
        engine.noteOn(lowNote + 7, 0.7f);
        std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block)), mono;
        mono.reserve(static_cast<size_t>(total - from));
        for (long done = 0; done < total; done += block) {
            const int n = static_cast<int>(std::min<long>(block, total - done));
            engine.process(L.data(), R.data(), n);
            for (int i = 0; i < n; ++i) {
                const double m = 0.5 * (static_cast<double>(L[static_cast<size_t>(i)]) + R[static_cast<size_t>(i)]);
                if (done + i >= from) {
                    mono.push_back(static_cast<float>(m));
                    energy += m * m;
                    ++counted;
                }
            }
        }
        // Welch: half-overlapping Hann windows, averaged. One number per bin, so the two passes
        // can be compared bin by bin.
        spec[pass].assign(kFft / 2 + 1, 0.0);
        Fft fft(kFft);
        std::vector<float> re(static_cast<size_t>(kFft)), im(static_cast<size_t>(kFft));
        int windows = 0;
        for (size_t s = 0; s + kFft <= mono.size(); s += kFft / 2) {
            for (int i = 0; i < kFft; ++i) {
                const float w = 0.5f - 0.5f * std::cos(kTwoPi * i / static_cast<float>(kFft));
                re[static_cast<size_t>(i)] = mono[s + static_cast<size_t>(i)] * w;
                im[static_cast<size_t>(i)] = 0.0f;
            }
            fft.transform(re.data(), im.data(), false);
            for (int k = 0; k <= kFft / 2; ++k)
                spec[pass][static_cast<size_t>(k)] += static_cast<double>(re[static_cast<size_t>(k)]) * re[static_cast<size_t>(k)]
                                                    + static_cast<double>(im[static_cast<size_t>(k)]) * im[static_cast<size_t>(k)];
            ++windows;
        }
        if (windows == 0) return {};
    }

    TonalProbe out;
    double sum[2] = { 0.0, 0.0 };
    for (int p = 0; p < 2; ++p) for (double v : spec[p]) sum[p] += v;
    if (sum[0] <= 0.0 || sum[1] <= 0.0) return {};
    double overlap = 0.0, lo = 0.0, hi = 0.0;
    const double binHz = static_cast<double>(sr) / kFft;
    for (size_t k = 0; k < spec[0].size(); ++k) {
        overlap += std::min(spec[0][k] / sum[0], spec[1][k] / sum[1]);
        const double f = k * binHz;
        const double e = spec[0][k] + spec[1][k];
        if (f >= 20.0 && f < 130.0) lo += e;
        else if (f >= 150.0 && f <= 5000.0) hi += e;
    }
    out.staticShare = 100.0 * overlap;
    out.bassDb = 10.0 * std::log10((lo + 1e-30) / (hi + 1e-30));
    out.rms = 10.0 * std::log10(energy / std::max<long>(counted, 1) + 1e-20);
    return out;
}

static int runOnce(int argc, char** argv)
{
    std::string out = "ambient.wav";
    double seconds = 60.0;
    double skipSeconds = 0.0;   // played and thrown away before the measured window begins
    int sr = 48000, block = 256;
    bool stats = false, dump = false, useMap = false, measure = false, loudness = false;
    // --near-log: one line per near event -- when, what near layer was in effect, and how loud the
    // mix was around it. Rene waited twenty minutes in a journey and heard none (13.09.2026), and
    // "the level is above zero" was never an answer to whether one plays or can be heard.
    bool nearLog = false;
    bool tonal = false;
    double tonalSeconds = 14.0;
    std::string tapPath, tapDir;
    double clockHour = -1.0;
    double mapX = 0.5, mapY = 0.5, mapRadius = 0.08;
    std::vector<std::vector<float>> irChannels; int irRate = 0; std::string irPath;
    std::vector<std::vector<float>> irBChannels; int irBRate = 0; std::string irBPath;
    std::string routeText; double routeSpeed = 1.0;
    SetTimeline setFile; bool haveSet = false; bool secondsGiven = false;
    Score score; bool haveScore = false;
    std::string stemPrefix;
    int presetIndex = -1;
    bool nearAuto = false;
    Journey journey; bool haveJourney = false; uint64_t journeySeed = 1;
    loadDefaultPresetPacks();   // $AMBIENT_PACKS or ~/Documents/Noctuary/Packs; --packs adds more
    std::vector<int> notes;
    std::string sclPath;
    Engine engine;
    // A journey's steps are crossfaded rather than cut, as they are in the instrument: the preset
    // that is leaving keeps playing on the engine it is on, with everything it had, while the next
    // one is built on a second engine, and the two are mixed under a sine/cosine pair so the sum
    // keeps its power all the way across. Nothing is interpolated -- two presets that share no
    // parameter still meet in the air, which is the whole reason a preset change became a
    // crossfade of two engines in the plugin (13.09.2026; before this the offline render cut, and
    // a journey rendered to a file sounded nothing like the same journey played).
    //
    // The second engine is built the first time a journey actually crosses, so every other render
    // -- every measurement of the library, every --preset -- is one engine, exactly as before.
    std::unique_ptr<Engine> engineB;
    Engine* liveEngine = &engine;      // the one the instrument is
    Engine* fadingEngine = nullptr;    // the one on its way out, or none
    double fadePos = 0.0;              // 0 .. 1 across the crossfade
    double fadeSeconds = 0.0, fadeHead = 0.0;
    auto live = [&]() -> Engine& { return *liveEngine; };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--out") out = next();
        else if (a == "--seconds") { seconds = std::atof(next().c_str()); secondsGiven = true; }
        else if (a == "--skip") skipSeconds = std::atof(next().c_str());
        else if (a == "--sr") sr = std::atoi(next().c_str());
        else if (a == "--block") block = std::atoi(next().c_str());
        else if (a == "--stats") stats = true;
        else if (a == "--near-log") nearLog = true;
        else if (a == "--measure") measure = true;   // print descriptors instead of writing a file
        else if (a == "--tonal") tonal = true;       // print how much of it follows the note
        else if (a == "--tonal-seconds") tonalSeconds = std::atof(next().c_str());
        // A short mono excerpt of the settled part, for whatever wants to listen to the render
        // rather than read its numbers -- a learned audio embedding, say. 22.05 kHz is plenty for
        // that and keeps a whole library's worth of excerpts to a few gigabytes.
        else if (a == "--tap") tapPath = next();
        // --tap-dir <dir>: the same excerpt, named after the preset, so a whole batch can write
        // its taps without a --tap per preset. The name is the preset's, with everything that is
        // not a letter or a digit turned into an underscore -- the rule measure_packs.py uses.
        else if (a == "--tap-dir") tapDir = next();
        // --hour <0..24>: what time the arc clock thinks it is. Without it a preset with the
        // clock arc on renders differently at four in the afternoon than at midnight, which is
        // the point of the feature and the end of any reproducible measurement -- 496 presets in
        // the library have it on, and their descriptors moved with the time of day.
        else if (a == "--hour") clockHour = std::atof(next().c_str());
        else if (a == "--loudness") loudness = true; // and a second line to BS.1770 (its own line so
                                                     // nothing that parses the measure line has to change)
        else if (a == "--dump") dump = true;
        else if (a == "--map") {   // render at a map position: x y [radius]
            mapX = std::atof(next().c_str()); mapY = std::atof(next().c_str());
            if (i + 1 < argc && std::atof(argv[i + 1]) > 0.0) mapRadius = std::atof(argv[++i]);
            useMap = true;
        }
        else if (a == "--scl") sclPath = next();
        else if (a == "--texture") {
            const std::string path = next();
            double baseHz = baseHzFromName(path.c_str());   // "_A3" suffix (TextureGen), else C4
            if (baseHz <= 0.0) baseHz = 261.6256;
            if (i + 1 < argc && std::atof(argv[i + 1]) > 0.0) baseHz = std::atof(argv[++i]);
            std::vector<float> l, r; int rate = 0;
            if (!readWavStereo(path.c_str(), l, r, rate)) { std::fprintf(stderr, "cannot read texture %s\n", path.c_str()); return 2; }
            engine.setTexture(l.data(), r.empty() ? nullptr : r.data(), static_cast<int>(l.size()), rate, baseHz, loopFromName(path.c_str()));
            std::printf("texture: %s (%.1f s @ %d Hz, %s, base %.1f Hz%s)\n", path.c_str(), l.size() / static_cast<double>(rate), rate,
                        r.empty() ? "mono" : "stereo", baseHz, loopFromName(path.c_str()) ? ", seamless" : "");
        }
        else if (a == "--route") {   // walk a route preset (by name) or a route text over the map
            const std::string spec = next();
            int found = -1;
            for (int r = 0; r < numRoutePresets(); ++r) if (spec == routePreset(r).name) found = r;
            routeText = found >= 0 ? routePreset(found).points : spec;
            if (i + 1 < argc && std::atof(argv[i + 1]) > 0.0) routeSpeed = std::atof(argv[++i]);
            std::printf("route: %s (speed %g)\n", found >= 0 ? routePreset(found).name : "custom", routeSpeed);
        }
        else if (a == "--packs") { const std::string dir = next(); std::printf("packs: %d loaded from %s\n", loadPresetPacksIn(dir.c_str()), dir.c_str()); }
        else if (a == "--list-packs") {
            loadDefaultPresetPacks();
            for (int k = 0; k < numPresetPacks(); ++k) std::printf("%s\n", presetPackName(k));
            std::printf("%d presets total (%d built in)\n", numPresets(), builtinPresetCount());
            return 0;
        }
        else if (a == "--mod") {   // modulation matrix, see Core/include/ambient/Modulation.h
            const std::string text = next();
            if (!engine.setModMatrixText(text.c_str())) { std::fprintf(stderr, "bad matrix text\n"); return 2; }
            std::printf("matrix: %d route(s)\n", engine.modMatrix().count());
        }
        else if (a == "--env") {   // --env <1..6> "<breakpoints>"
            const int idx = std::atoi(next().c_str()) - 1;
            const std::string text = next();
            if (!engine.setEnvShape(idx, text.c_str())) { std::fprintf(stderr, "bad envelope %d\n", idx + 1); return 2; }
            std::printf("env %d: %d points\n", idx + 1, engine.envShape(idx).count());
        }
        else if (a == "--src-env") {   // --src-env <1..4> "<breakpoints>": a source's own envelope (its Env set to Own)
            const int idx = std::atoi(next().c_str()) - 1;
            const std::string text = next();
            if (!engine.setSrcEnvShape(idx, text.c_str())) { std::fprintf(stderr, "bad source envelope %d\n", idx + 1); return 2; }
            std::printf("source env %d: %d points\n", idx + 1, engine.srcEnvShape(idx).count());
        }
        else if (a == "--score") {   // a written score of timed ramps; length defaults to the score's
            const std::string path = next();
            if (!score.load(path.c_str())) { std::fprintf(stderr, "cannot read the score %s\n", path.c_str()); return 1; }
            haveScore = true;
            std::printf("score: %d events, %.0f s\n", score.count(), score.length());
        }
        else if (a == "--stems") stemPrefix = next();   // also write near/far/cosmos/room
        else if (a == "--set-file") {   // play a recorded set (.ambientset) while rendering; length defaults to the set's
            const std::string path = next();
            if (!setFile.load(path.c_str())) { std::fprintf(stderr, "cannot read set %s\n", path.c_str()); return 2; }
            haveSet = true;
            std::printf("set: %s (%zu events, %.1f s)\n", path.c_str(), setFile.size(), setFile.length());
        }
        else if (a == "--ir") {   // impulse response for the Room (mono or stereo WAV)
            const std::string path = next();
            std::vector<std::vector<float>> ch; int rate = 0;
            if (!readWavChannels(path.c_str(), ch, rate) || ch.empty()) { std::fprintf(stderr, "cannot read impulse %s\n", path.c_str()); return 2; }
            irChannels = ch; irRate = rate; irPath = path;
        }
        else if (a == "--ir-b") {   // the second impulse, the one Room Morph goes to
            const std::string path = next();
            std::vector<std::vector<float>> ch; int rate = 0;
            if (!readWavChannels(path.c_str(), ch, rate) || ch.empty()) { std::fprintf(stderr, "cannot read impulse %s\n", path.c_str()); return 2; }
            irBChannels = ch; irBRate = rate; irBPath = path;
        }
        else if (a == "--write-default-ir") {   // the built-in hall, for measuring it; --sr before this one sets the rate
            const std::string path = next();
            std::vector<float> L, R;
            Convolver::makeDefaultImpulse(sr, 7, 4.0f, L, R);
            std::vector<float> inter(L.size() * 2);
            for (size_t k = 0; k < L.size(); ++k) { inter[2 * k] = L[k]; inter[2 * k + 1] = R[k]; }
            if (!writeWav(path, inter, 2, sr)) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return 2; }
            std::printf("default impulse: %s (%.2f s at %d Hz)\n", path.c_str(), L.size() / static_cast<double>(sr), sr);
            return 0;
        }
        else if (a == "--wavetable") {
            const std::string path = next();
            std::vector<float> mono; int cycle = 0;
            if (!readWavetableFile(path.c_str(), mono, cycle) || !engine.loadUserWavetable(mono.data(), static_cast<int>(mono.size()), cycle))
            { std::fprintf(stderr, "cannot read wavetable %s\n", path.c_str()); return 2; }
            std::printf("wavetable: %s (%d frames of %d samples)\n", path.c_str(), static_cast<int>(mono.size()) / cycle, cycle);
        }
        else if (a == "--notes") {
            std::stringstream ss(next()); std::string tok;
            while (std::getline(ss, tok, ',')) if (!tok.empty()) notes.push_back(std::atoi(tok.c_str()));
        }
        else if (a == "--preset") {
            const std::string name = next();
            int found = -1;
            for (int p = 0; p < numPresets(); ++p) if (name == preset(p).name) found = p;
            if (found < 0) { std::fprintf(stderr, "unknown preset '%s' (see --list-presets)\n", name.c_str()); return 2; }
            engine.applyPreset(found);
            presetIndex = found;
            std::printf("preset: %s\n", preset(found).name);
        }
        else if (a == "--set") {
            std::string kv = next();
            const size_t eq = kv.find('=');
            if (eq == std::string::npos) { std::fprintf(stderr, "bad --set %s\n", kv.c_str()); return 2; }
            const ParamDesc* d = findParam(kv.substr(0, eq).c_str());
            if (!d) { std::fprintf(stderr, "unknown parameter '%s'\n", kv.substr(0, eq).c_str()); return 2; }
            engine.setParam(d->id, paramValueFromText(*d, kv.substr(eq + 1).c_str()));
        }
        else if (a == "--list-choices") {
            // Every choice parameter with its value names, so a tool never has to keep its own copy.
            for (const auto& d : paramTable()) {
                if (d.kind != ParamKind::Choice) continue;
                std::printf("%s:", d.key);
                for (int c = 0; c < d.numChoices; ++c) std::printf("%s%s", c ? "|" : "", d.choices[c]);
                std::printf("\n");
            }
            return 0;
        }
        else if (a == "--list") {
            for (const auto& d : paramTable())
                std::printf("%-18s %-14s [%g .. %g] default %g %s\n", d.key, d.section, d.min, d.max, d.def, d.unit);
            return 0;
        }
        else if (a == "--sound-preset" || a == "--cosmos-preset") {
            const bool cosmos = (a == "--cosmos-preset");
            const std::string name = next();
            const int count = cosmos ? numCosmosPresets() : numPresets();
            int found = -1;
            for (int p = 0; p < count; ++p) if (name == (cosmos ? cosmosPreset(p) : preset(p)).name) found = p;
            if (found < 0) { std::fprintf(stderr, "unknown %s preset '%s'\n", cosmos ? "cosmos" : "sound", name.c_str()); return 2; }
            if (cosmos) engine.applyCosmosPreset(found); else engine.applySoundPreset(found);
            std::printf("%s preset: %s\n", cosmos ? "cosmos" : "sound", name.c_str());
        }
        else if (a == "--describe") {   // the browser's line of prose for a preset, or for all of them
            const std::string want = next();
            for (int p = 0; p < numPresets(); ++p)
                if (want == "all" || want == preset(p).name)
                    std::printf("%-30s %s\n", preset(p).name, presetDescription(p).c_str());
            return 0;
        }
        else if (a == "--list-presets") {
            for (int p = 0; p < numPresets(); ++p) std::printf("%s\n", preset(p).name);
            return 0;
        }
        else if (a == "--list-routes") {
            for (int r = 0; r < numRoutePresets(); ++r) std::printf("%-22s %s\n", routePreset(r).name, routePreset(r).points);
            return 0;
        }
        else if (a == "--list-cosmos-presets") {
            for (int p = 0; p < numCosmosPresets(); ++p) std::printf("%s\n", cosmosPreset(p).name);
            return 0;
        }
        else if (a == "--near-preset") {   // the near layer's bank, on top of whatever sound is loaded
            const std::string name = next();
            int found = -1;
            for (int p = 0; p < numNearPresets(); ++p) if (name == nearPreset(p).name) found = p;
            if (found < 0) { std::fprintf(stderr, "unknown near preset '%s'\n", name.c_str()); return 2; }
            engine.applyNearPreset(found);
            std::printf("near preset: %s\n", name.c_str());
            // Its clip, if it names one, from the library's archive -- a file, or a folder of them.
            const Preset& np = nearPreset(found);
            if (np.texture != nullptr && *np.texture != 0) {
                const std::string got = resolveLibraryFile(np.texture);
                if (got.empty() || !loadNearClips(engine, got)) std::fprintf(stderr, "near preset's clip not found: %s\n", np.texture);
            }
        }
        else if (a == "--near-clip") {   // a clip for the near source: a file, or a folder of them (one per event, at random)
            const std::string file = next();
            if (!loadNearClips(engine, file)) { std::fprintf(stderr, "cannot read %s\n", file.c_str()); return 2; }
        }
        else if (a == "--list-near-presets") {
            for (int p = 0; p < numNearPresets(); ++p) std::printf("%-28s %s\n", nearPreset(p).name, nearPresetFamily(nearPresetCategory(p)));
            return 0;
        }
        else if (a == "--near-auto") nearAuto = true;   // the foreground the pack preset brings (off by default: the library is measured without one)
        else if (a == "--journey") {   // presets in a row with dwell and fade ranges (Journey.h); the tool cuts where the plugin crossfades
            const std::string path = next();
            if (!journey.load(path.c_str()) || journey.steps.empty()) { std::fprintf(stderr, "cannot read the journey %s\n", path.c_str()); return 2; }
            haveJourney = true;
            std::printf("journey: %s (%d steps, %s a pass, %s)\n", journey.name.c_str(), static_cast<int>(journey.steps.size()),
                        Journey::timeText(journey.meanLength()).c_str(), journey.cyclic ? "cyclic" : "once");
        }
        else if (a == "--journey-seed") journeySeed = static_cast<uint64_t>(std::strtoull(next().c_str(), nullptr, 10)) | 1ull;
        else if (a == "--bench") {
            // Realtime factor per preset (rendered seconds per wall second). Run on the device via adb
            // to see what the core costs there; below ~3 the headset would be at its limit.
            const double secs = 10.0;
            double worst = 1e9; const char* worstName = "";
            std::printf("%-24s %8s %8s\n", "preset", "rms dB", "x rt");
            for (int p = 0; p < numPresets(); ++p) {
                Engine e;
                e.applyPreset(p);
                e.setParam(ParamId::BrainRate, 3.0f);
                e.prepare(sr, block);
                e.noteOn(48, 0.8f); e.noteOn(55, 0.8f); e.noteOn(64, 0.8f);
                std::vector<float> bl(static_cast<size_t>(block)), br(static_cast<size_t>(block));
                double sq = 0; long cnt = 0;
                const auto t0 = std::chrono::steady_clock::now();
                for (long done = 0; done < static_cast<long>(secs * sr); done += block) {
                    e.process(bl.data(), br.data(), block);
                    for (int k = 0; k < block; ++k) { sq += bl[static_cast<size_t>(k)] * bl[static_cast<size_t>(k)]; ++cnt; }
                }
                const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                const double rt = secs / std::max(wall, 1e-6);
                if (rt < worst) { worst = rt; worstName = preset(p).name; }
                std::printf("%-24s %8.1f %8.1f\n", preset(p).name, 10.0 * std::log10(sq / std::max<long>(cnt, 1) + 1e-20), rt);
            }
            std::printf("slowest: %s at %.1f x realtime\n", worstName, worst);
            return 0;
        }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }

    if (!sclPath.empty()) {
        std::ifstream f(sclPath);
        std::stringstream ss; ss << f.rdbuf();
        FixedScale s;
        if (!parseScala(ss.str().c_str(), s)) { std::fprintf(stderr, "cannot parse %s\n", sclPath.c_str()); return 2; }
        engine.setUserScale(s);
        engine.setParam(ParamId::Scale, static_cast<float>(kNumScaleChoices - 1));
        std::printf("scale: %s (%d degrees, period %.4f)\n", s.name, s.count, s.period);
    }

    if (useMap) {
        engine.setParam(ParamId::MapActive, 1.0f);
        engine.setParam(ParamId::MapX, static_cast<float>(mapX));
        engine.setParam(ParamId::MapY, static_cast<float>(mapY));
        engine.setParam(ParamId::MapRadius, static_cast<float>(mapRadius));
        engine.setParam(ParamId::MorphGlide, 0.1f);   // arrive at the blend right away
        PresetMap::warmup();
        const PresetMap::Blend b = PresetMap::neighbours(static_cast<float>(mapX), static_cast<float>(mapY), static_cast<float>(mapRadius));
        std::printf("map %.3f %.3f (radius %.3f):", mapX, mapY, mapRadius);
        for (int k = 0; k < b.count; ++k) std::printf(" %s %.2f;", preset(b.index[k]).name, b.weight[k]);
        std::printf("\n");
    }
    if (dump) {   // every parameter as key = value (choices by name), for tools that analyse presets
        for (const ParamDesc& d : paramTable()) {
            const float v = engine.getParam(d.id);
            if (d.kind == ParamKind::Choice) std::printf("param %s = %s\n", d.key, d.choices[clampv(static_cast<int>(std::lround(v)), 0, d.numChoices - 1)]);
            else if (d.kind == ParamKind::Bool) std::printf("param %s = %s\n", d.key, v >= 0.5f ? "on" : "off");
            else std::printf("param %s = %g\n", d.key, v);
        }
    }

    // A pack preset may name a sample, a wavetable and an impulse of its own. As a function of the
    // index, because a journey brings up one preset after another while the render runs; the rooms
    // go straight into the engine then (it has been prepared), where the first preset's are
    // collected here and set after prepare like an --ir.
    auto loadPresetMedia = [&](Engine& engine, int index, bool roomsNow) {
        const char* tex = presetFilePath(index, 0);
        const char* tab = presetFilePath(index, 1);
        std::vector<float> mono; int rate = 0;
        if (tex && *tex) {
            // One path goes into every slot; up to four separated by ';' go one per slot, an empty
            // one meaning that slot has none -- the same reading the plugin gives the field.
            const std::string all(tex);
            const bool perSlot = all.find(';') != std::string::npos;
            size_t start = 0;
            for (int slot = 0; slot < ambient::kSlots; ++slot) {
                const size_t semi = all.find(';', start);
                std::string one = all.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
                while (!one.empty() && one.front() == ' ') one.erase(one.begin());
                while (!one.empty() && one.back() == ' ') one.pop_back();
                if (!one.empty()) {
                    std::vector<float> right;
                    if (readWavStereo(one.c_str(), mono, right, rate)) {
                        double base = baseHzFromName(one.c_str()); if (base <= 0.0) base = 261.6256;
                        const bool seamless = loopFromName(one.c_str());
                        const float* rp = right.empty() ? nullptr : right.data();
                        if (perSlot) engine.setTexture(slot, mono.data(), rp, static_cast<int>(mono.size()), rate, base, seamless);
                        else         engine.setTexture(mono.data(), rp, static_cast<int>(mono.size()), rate, base, seamless);
                        std::printf("preset texture%s: %s\n", perSlot ? (" " + std::to_string(slot + 1)).c_str() : "", one.c_str());
                    } else std::fprintf(stderr, "preset texture missing: %s\n", one.c_str());
                }
                if (semi == std::string::npos) break;
                start = semi + 1;
            }
        }
        if (tab && *tab) {
            int cycle = 0;
            if (readWavetableFile(tab, mono, cycle) && engine.loadUserWavetable(mono.data(), static_cast<int>(mono.size()), cycle))
                std::printf("preset wavetable: %s (%d frames)\n", tab, engine.userWavetableFrames());
            else std::fprintf(stderr, "preset wavetable missing or unusable: %s\n", tab);
        }
        // ...and its rooms. The render tool read the sample and the wavetable and never the impulse,
        // so every measurement of the library -- loudness, the map, what a preset sounds like --
        // heard the built-in hall instead of the preset's own room. An --ir on the command line wins.
        const char* imp = presetFilePath(index, 2);
        const char* impB = presetFilePath(index, 3);
        if (imp && *imp && (roomsNow || irChannels.empty())) {
            std::vector<std::vector<float>> ch; int rate2 = 0;
            if (readWavChannels(imp, ch, rate2) && !ch.empty()) {
                if (roomsNow) engine.setImpulse(ch[0].data(), ch.size() > 1 ? ch[1].data() : nullptr, static_cast<int>(ch[0].size()), rate2);
                else { irChannels = ch; irRate = rate2; irPath = imp; }
            } else std::fprintf(stderr, "preset impulse missing: %s\n", imp);
        }
        if (impB && *impB && (roomsNow || irBChannels.empty())) {
            std::vector<std::vector<float>> ch; int rate2 = 0;
            if (readWavChannels(impB, ch, rate2) && !ch.empty()) {
                if (roomsNow) engine.setImpulseB(ch[0].data(), ch.size() > 1 ? ch[1].data() : nullptr, static_cast<int>(ch[0].size()), rate2);
                else { irBChannels = ch; irBRate = rate2; irBPath = impB; }
            } else std::fprintf(stderr, "preset impulse B missing: %s\n", impB);
        }
    };
    if (presetIndex >= 0) loadPresetMedia(engine, presetIndex, false);
    // A journey begins on its first step's preset: applied here like --preset, so its media is
    // read before the render starts; the steps after it are brought up in the loop.
    JourneyPlayer journeyPlayer;
    if (haveJourney) {
        journeyPlayer.start(journey, journeySeed, 0);
        JourneyStep st; double fade = 0.0;
        if (journeyPlayer.advance(0.0, st, fade)) {
            int idx = -1;
            for (int p = 0; p < numPresets(); ++p) if (st.preset == preset(p).name) { idx = p; break; }
            if (idx < 0) { std::fprintf(stderr, "the journey's first preset is not in the library: %s\n", st.preset.c_str()); return 2; }
            engine.applyPreset(idx); presetIndex = idx; loadPresetMedia(engine, idx, false);
            std::printf("journey step 1: %s (%s)\n", st.preset.c_str(), Journey::timeText(journeyPlayer.remaining()).c_str());
        }
        if (!secondsGiven) seconds = std::min(journey.meanLength(), 4.0 * 3600.0);
    }

    if (tapPath.empty() && !tapDir.empty() && presetIndex >= 0) {
        // A RUN of non-alphanumerics becomes ONE underscore, which is what
        // re.sub(r"[^A-Za-z0-9]+", "_", name) does in measure_packs.py -- the taps are read back
        // by that name, so the two rules have to be the same one.
        std::string slug;
        bool lastWasSep = false;
        for (const char* c = preset(presetIndex).name; *c && slug.size() < 80; ++c) {
            if (std::isalnum(static_cast<unsigned char>(*c))) { slug += *c; lastWasSep = false; }
            else if (!lastWasSep) { slug += '_'; lastWasSep = true; }
        }
        tapPath = tapDir + "/" + slug + ".wav";
    }
    engine.setClockHourOverride(clockHour);
    // As the plugin does when a pack preset is chosen with Auto on: the artist's table, by the
    // preset's name, and the near preset's Every scaled by the class's factor. A lambda rather
    // than a block, because a journey has to do this again at every step -- the plugin's program
    // change brings a foreground, so a step of a journey rendered here has to bring the same one
    // or the offline take is not the take you heard (13.09.2026).
    auto applyNearAutoTo = [&](Engine& e, int idx, bool announce) {
        if (!nearAuto || idx < 0) return;
        const int pack = presetPack(idx);
        float factor = 1.0f;
        const int pick = pack >= 0 ? nearAutoPick(presetPackName(pack), preset(idx).name, factor) : -1;
        if (pick > 0) {
            e.applyNearPreset(pick);
            const Preset& np = nearPreset(pick);
            if (np.texture != nullptr && *np.texture != 0) { const std::string got = resolveLibraryFile(np.texture); if (!got.empty()) loadNearClips(e, got); }
            e.setParam(ParamId::ForeRate, clampv(e.getParam(ParamId::ForeRate) * factor, 10.0f, 900.0f));
            if (announce) std::printf("near auto: %s (Every x%.2f)\n", np.name, factor);
        } else if (announce) std::printf("near auto: none for this preset\n");
    };
    applyNearAutoTo(engine, presetIndex, true);
    engine.prepare(sr, block);
    if (!irChannels.empty()) {   // after prepare: the convolver's buffers exist now
        engine.setImpulse(irChannels[0].data(), irChannels.size() > 1 ? irChannels[1].data() : nullptr, static_cast<int>(irChannels[0].size()), irRate);
        std::printf("impulse: %s (%zu ch, %.2f s @ %d Hz -> %.2f s used)\n", irPath.c_str(), irChannels.size(), irChannels[0].size() / static_cast<double>(irRate), irRate, engine.impulseSeconds());
    }
    if (!irBChannels.empty()) {
        engine.setImpulseB(irBChannels[0].data(), irBChannels.size() > 1 ? irBChannels[1].data() : nullptr, static_cast<int>(irBChannels[0].size()), irBRate);
        std::printf("impulse B: %s (%zu ch, %.2f s @ %d Hz)\n", irBPath.c_str(), irBChannels.size(), irBChannels[0].size() / static_cast<double>(irBRate), irBRate);
    }
    for (int n : notes) engine.noteOn(n, 0.8f);
    if (!routeText.empty()) {
        if (!engine.setRouteText(routeText.c_str())) { std::fprintf(stderr, "bad route text (unknown preset name or malformed point)\n"); return 2; }
        engine.setParam(ParamId::RouteSpeed, static_cast<float>(routeSpeed));
        engine.setParam(ParamId::RouteLoop, 1.0f);
        engine.setParam(ParamId::RouteActive, 1.0f);
        const Waypoint& w0 = engine.routeEdit().point(0);
        engine.setParam(ParamId::MapX, w0.x); engine.setParam(ParamId::MapY, w0.y); engine.setParam(ParamId::MapRadius, w0.radius);   // start on the first point
        engine.setParam(ParamId::MorphGlide, 20.0f);
    }

    if (haveSet) { if (!secondsGiven) seconds = setFile.length() + 20.0; setFile.seek(0.0); }
    if (haveScore) { if (!secondsGiven) seconds = score.length() + 20.0; score.rewind(); }
    if (!(seconds >= 0.0) || !(seconds < 1.0e7) || block <= 0 || block > (1 << 16) || !(sr > 0.0)) {
        std::fprintf(stderr, "bad --seconds, --block or --rate\n");
        return 2;
    }
    const long total = static_cast<long>(seconds * sr);
    std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block));
    std::vector<float> wav; wav.reserve(static_cast<size_t>(total) * 2);
    // Stems: four stereo pairs written alongside the mix, so a piece can be balanced afterwards.
    std::vector<std::vector<float>> stemOut(static_cast<size_t>(Engine::kNumStems) * 2);
    std::vector<float> stemChunk(static_cast<size_t>(Engine::kNumStems) * 2 * static_cast<size_t>(block), 0.0f);
    float* stemPtr[Engine::kNumStems * 2] = {};
    // The measurement wants them too, though it writes no files: the near / far / room / cosmos
    // energies are how wet a preset stands, which is the spatial model's own axis on the map.
    double stemE[Engine::kNumStems] = {};
    const bool wantStems = !stemPrefix.empty() || measure;
    if (wantStems) {
        for (int c = 0; c < Engine::kNumStems * 2; ++c)
            stemPtr[c] = stemChunk.data() + static_cast<size_t>(c) * static_cast<size_t>(block);
        engine.setStemBuffers(stemPtr);
    }

    // The next step of a journey, brought up on the other engine and crossfaded in -- what
    // beginTransition does in the plugin, with the same handover. A fade of nothing (or one that
    // would not fit in what is left to render) is a cut on the engine that is playing, which is
    // what this tool always did.
    std::vector<float> fadeL(static_cast<size_t>(block)), fadeR(static_cast<size_t>(block));
    auto beginJourneyStep = [&](int idx, double fade) {
        Engine& out = live();
        if (fade < 0.05) { out.applyPreset(idx); loadPresetMedia(out, idx, true); applyNearAutoTo(out, idx, false); return; }
        // Only ever two engines: a step whose fade is still running arrives on the one that is
        // going out, so the fade in flight is finished here rather than abandoned half way.
        if (fadingEngine != nullptr) { fadingEngine->allNotesOff(); fadingEngine->reset(); fadingEngine = nullptr; }
        if (engineB == nullptr) engineB = std::make_unique<Engine>();
        Engine& in = (liveEngine == &engine) ? *engineB : engine;
        in.allNotesOff();
        in.reset();
        // Everything the instrument stands at now, and the preset's own on top of it: a parameter
        // the new preset does not name keeps the value it has, which is what applying a preset to
        // the engine that is playing did, and is how --set survives a step.
        for (int i = 0; i < kNumParams; ++i) in.setParam(static_cast<ParamId>(i), out.getParam(static_cast<ParamId>(i)));
        in.applyPreset(idx);
        applyNearAutoTo(in, idx, false);
        in.setClockHourOverride(clockHour);
        in.prepare(sr, block);
        // After prepare, in this order: the command line's room (the convolver's buffers exist
        // now), then the preset's own over it, as the setup above does for the first preset.
        if (!irChannels.empty())
            in.setImpulse(irChannels[0].data(), irChannels.size() > 1 ? irChannels[1].data() : nullptr, static_cast<int>(irChannels[0].size()), irRate);
        if (!irBChannels.empty())
            in.setImpulseB(irBChannels[0].data(), irBChannels.size() > 1 ? irBChannels[1].data() : nullptr, static_cast<int>(irBChannels[0].size()), irBRate);
        loadPresetMedia(in, idx, true);
        // The conductor takes the chord over from the one it is replacing: a crossfade is meant to
        // change the instrument and not the music. With nothing to inherit it fills instead.
        {
            int cn[ClusterBrain::kSlots]; float cv[ClusterBrain::kSlots];
            for (int which = 0; which < 2; ++which) {
                const bool second = which == 1;
                const int n = out.soundingCluster(cn, cv, second);
                if (n > 0) in.adoptCluster(cn, cv, n, second);
                else if (!second) in.requestBrainFill();
            }
            in.adoptNear(out.nearState());
        }
        for (int n : notes) in.noteOn(n, 0.8f);   // a chord held by hand is held on the new one too
        // The stems follow the instrument, so they are the new engine's from here; the one going
        // out would otherwise add its own into the same buffers under no gain at all.
        if (wantStems) { in.setStemBuffers(stemPtr); out.setStemBuffers(nullptr); }
        liveEngine = &in;
        fadingEngine = &out;
        fadePos = 0.0;
        fadeHead = 0.0;
        fadeSeconds = fade;
    };

    // --skip: play this much first and throw it away, then measure what follows. A preset is
    // described by its FIRST minute otherwise, and the 2.0 library has a median attack of eighteen
    // seconds and an eighth of it above forty: those presets were being described, and their gain
    // set, while they were still on their way up. Measured on six of them, the voice stands 4.0 dB
    // lower against its bed at forty seconds than at ninety. Nothing changes for a preset that has
    // arrived by then -- skip 0 renders exactly what it always did, sample for sample.
    if (skipSeconds > 0.0) {
        const long warm = static_cast<long>(skipSeconds * sr);
        for (long done = 0; done < warm; done += block) {
            const int n = static_cast<int>(std::min<long>(block, warm - done));
            engine.process(L.data(), R.data(), n);
        }
    }

    double sumSq[2] = { 0, 0 }; float peak = 0.0f; long nans = 0;
    double secSq[2] = { 0, 0 }; long secCount = 0; int sec = 0;
    // How many voices are sounding, averaged over the settled half of the render. It used to be
    // engine.activeVoices() at the last sample -- one instantaneous reading of a number that a
    // generative conductor changes every few seconds, which made the map's density axis a coin
    // toss. Sampled per block and averaged over the same half every other descriptor uses.
    double voiceSum = 0.0; long voiceBlocks = 0;
    const Engine* nearLogEngine = nullptr;   // --near-log: the engine whose count was read last
    int nearLogCount = 0;
    if (tonal) {
        const TonalProbe t = tonalProbe(engine, sr, tonalSeconds);
        std::printf("tonal: static=%.1f bass_db=%+.1f rms=%.1f\n", t.staticShare, t.bassDb, t.rms);
        return 0;
    }
    if (stats) std::printf("sec, rmsL_dB, rmsR_dB, peak, voices, root, arc\n");
    float secPeak = 0.0f;

    for (long done = 0; done < total; done += block) {
        const int n = static_cast<int>(std::min<long>(block, total - done));
        if (!routeText.empty()) { float rx, ry, rr; live().routeStep(static_cast<double>(n) / sr, rx, ry, rr); }
        if (haveSet) {
            const double t0 = static_cast<double>(done) / sr, t1 = static_cast<double>(done + n) / sr;
            setFile.step(t0, t1, [&](const TimelineEvent& e) {
                switch (e.type) {
                case TimelineEvent::Type::Param:   live().setParam(static_cast<ParamId>(e.a), e.v); break;
                case TimelineEvent::Type::NoteOn:  live().noteOn(e.a, e.v); break;
                case TimelineEvent::Type::NoteOff: live().noteOff(e.a); break;
                }
            });
        }
        if (haveScore)
            score.step(static_cast<double>(n) / sr,
                       [&](ParamId id) { return live().getParam(id); },
                       [&](ParamId id, float v) { live().setParam(id, v); });
        if (haveJourney) {   // the next step, when its time has come, crossfaded in as in the plugin
            JourneyStep st; double fade = 0.0;
            if (journeyPlayer.advance(static_cast<double>(n) / sr, st, fade)) {
                int idx = -1;
                for (int p = 0; p < numPresets(); ++p) if (st.preset == preset(p).name) { idx = p; break; }
                if (idx >= 0) {
                    // Never longer than what is left to render: a two-minute fade at the end of a
                    // ten-minute render would leave the file on the crossfade rather than on the
                    // step, which is not what the journey says.
                    const double left = static_cast<double>(total - done) / sr;
                    beginJourneyStep(idx, std::min(fade, std::max(0.0, left - 1.0)));
                    std::printf("journey step %d at %s: %s (fade %s, %s)\n", journeyPlayer.step() + 1, Journey::timeText(static_cast<double>(done) / sr).c_str(),
                                st.preset.c_str(), Journey::timeText(fade).c_str(), Journey::timeText(journeyPlayer.remaining()).c_str());
                } else std::fprintf(stderr, "journey: preset not in the library, kept the last: %s\n", st.preset.c_str());
            }
        }
        live().process(L.data(), R.data(), n);
        if (fadingEngine != nullptr) {
            // The ramp waits for the arriving preset to be audible, up to eight seconds, exactly as
            // the plugin's does: the library's median attack is eighteen seconds, and a fade that
            // began while the new engine was still silent would be over before it spoke.
            const double t0 = fadePos;
            bool ramping = t0 > 0.0;
            if (!ramping) {
                double e = 0.0;
                for (int i = 0; i < n; ++i) e += static_cast<double>(L[static_cast<size_t>(i)]) * L[static_cast<size_t>(i)]
                                              + static_cast<double>(R[static_cast<size_t>(i)]) * R[static_cast<size_t>(i)];
                fadeHead += n / sr;
                ramping = std::sqrt(e / (2.0 * n)) > 1.0e-3 || fadeHead >= 8.0;
            }
            const double t1 = ramping ? std::min(1.0, t0 + n / (sr * std::max(fadeSeconds, 0.25))) : 0.0;
            fadingEngine->process(fadeL.data(), fadeR.data(), n);
            for (int i = 0; i < n; ++i) {
                const double t = t0 + (t1 - t0) * (i + 1.0) / n;
                const float gIn = static_cast<float>(std::sin(1.5707963267948966 * t));
                const float gOut = static_cast<float>(std::cos(1.5707963267948966 * t));
                L[static_cast<size_t>(i)] = L[static_cast<size_t>(i)] * gIn + fadeL[static_cast<size_t>(i)] * gOut;
                R[static_cast<size_t>(i)] = R[static_cast<size_t>(i)] * gIn + fadeR[static_cast<size_t>(i)] * gOut;
            }
            fadePos = t1;
            if (t1 >= 1.0) { fadingEngine->allNotesOff(); fadingEngine->reset(); fadingEngine = nullptr; }
        }
        if (nearLog) {
            // Counted on the engine that plays. A journey step hands the scheduler over with its
            // count (adoptNear), so a change of engine rebases rather than counting twice.
            const Engine* le = &live();
            const int ev = le->nearEventsPlayed();
            if (le != nearLogEngine) { nearLogEngine = le; nearLogCount = ev; }
            for (; nearLogCount < ev; ++nearLogCount)
                std::printf("near event %d at %s (%.1f s): level %.2f, every %.0f s, type %d, length %.1f s, distance %.2f, dry %.2f\n",
                            nearLogCount + 1, Journey::timeText(static_cast<double>(done) / sr).c_str(), static_cast<double>(done) / sr,
                            le->effectiveParam(ParamId::ForeLevel), le->effectiveParam(ParamId::ForeRate),
                            static_cast<int>(le->effectiveParam(ParamId::ForeType)), le->effectiveParam(ParamId::ForeLength),
                            le->effectiveParam(ParamId::ForeDistance), le->effectiveParam(ParamId::ForeDry));
        }
        if (done >= total / 2) { voiceSum += live().activeVoices(); ++voiceBlocks; }
        if (!stemPrefix.empty())
            for (int c = 0; c < Engine::kNumStems * 2; ++c)
                stemOut[static_cast<size_t>(c)].insert(stemOut[static_cast<size_t>(c)].end(), stemPtr[c], stemPtr[c] + n);
        if (measure && done >= total / 2)   // the settled half, as every other descriptor
            for (int st = 0; st < Engine::kNumStems; ++st)
                for (int i = 0; i < n; ++i)
                    stemE[st] += static_cast<double>(stemPtr[st * 2][i]) * stemPtr[st * 2][i]
                               + static_cast<double>(stemPtr[st * 2 + 1][i]) * stemPtr[st * 2 + 1][i];
        for (int i = 0; i < n; ++i) {
            const float l = L[static_cast<size_t>(i)], r = R[static_cast<size_t>(i)];
            if (std::isnan(l) || std::isnan(r) || std::isinf(l) || std::isinf(r)) ++nans;
            wav.push_back(l); wav.push_back(r);
            sumSq[0] += l * l; sumSq[1] += r * r;
            secSq[0] += l * l; secSq[1] += r * r;
            peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
            secPeak = std::max(secPeak, std::max(std::fabs(l), std::fabs(r)));
            if (++secCount >= sr) {
                if (stats) std::printf("%d, %.1f, %.1f, %.3f, %d, %d, %.2f\n", sec,
                    10.0 * std::log10(secSq[0] / secCount + 1e-20), 10.0 * std::log10(secSq[1] / secCount + 1e-20),
                    secPeak, live().activeVoices(), live().brainRoot(), live().arcValue());
                secSq[0] = secSq[1] = 0; secCount = 0; secPeak = 0.0f; ++sec;
            }
        }
    }

    const double rmsL = std::sqrt(sumSq[0] / std::max<long>(total, 1)), rmsR = std::sqrt(sumSq[1] / std::max<long>(total, 1));
    std::printf("rendered %.1f s @ %d Hz: rms %.1f / %.1f dBFS, peak %.3f, non-finite %ld, voices at end %d\n",
                seconds, sr, 20.0 * std::log10(rmsL + 1e-20), 20.0 * std::log10(rmsR + 1e-20), peak, nans, live().activeVoices());
    if (loudness) {
        // The engine's own meter, which has seen every sample of the render rather than a window
        // of it. Its own line, so nothing that parses the measure line has to learn a new field.
        const LoudnessReading ld = live().loudness();
        // Sones as well as LUFS. The two disagree whenever the spectrum changes, which for an
        // instrument that makes beds rather than tracks is most of the time, and the sone figure
        // is the one that says whether a preset will feel loud after an hour of it.
        std::printf("loudness: lufs_i=%.2f lufs_s=%.2f lufs_m=%.2f lra=%.2f truepeak=%.2f crest=%.2f"
                    " sone=%.2f sone_n5=%.2f sone_max=%.2f seconds=%.1f\n",
                    ld.integrated, ld.shortTerm, ld.momentary, ld.range, ld.truePeak, ld.crest,
                    ld.sones, ld.sonesN5, ld.sonesMax, ld.seconds);
    }
    if (!tapPath.empty()) {
        // The last twelve seconds, mono, decimated to about 22 kHz by averaging -- a plain box
        // filter, which is enough for an embedding and costs nothing.
        const int step = std::max(1, sr / 22050);
        const long want = std::min<long>(total, static_cast<long>(12.0 * sr));
        const size_t frames = wav.size() / 2;
        const size_t from = frames > static_cast<size_t>(want) ? frames - static_cast<size_t>(want) : 0;
        std::vector<float> tap;
        tap.reserve((frames - from) / static_cast<size_t>(step) + 1);
        for (size_t i = from; i + static_cast<size_t>(step) <= frames; i += static_cast<size_t>(step)) {
            float acc = 0.0f;
            for (int k = 0; k < step; ++k) acc += 0.5f * (wav[(i + static_cast<size_t>(k)) * 2] + wav[(i + static_cast<size_t>(k)) * 2 + 1]);
            tap.push_back(acc / static_cast<float>(step));
        }
        if (!writeWav(tapPath, tap, 1, sr / step)) std::fprintf(stderr, "cannot write tap %s\n", tapPath.c_str());
    }
    if (measure) {   // descriptors straight from the buffer: no temporary file at all
        std::vector<float> ml(wav.size() / 2), mr(wav.size() / 2);
        for (size_t k = 0; k + 1 < wav.size(); k += 2) { ml[k / 2] = wav[k]; mr[k / 2] = wav[k + 1]; }
        printMeasurements(ml, mr, sr, voiceBlocks > 0 ? voiceSum / voiceBlocks : live().activeVoices(), stemE);
        return nans == 0 ? 0 : 1;
    }
    if (!writeWav(out, wav, 2, sr)) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }
    if (!stemPrefix.empty()) {
        for (int st = 0; st < Engine::kNumStems; ++st) {
            std::vector<float> inter;
            inter.reserve(stemOut[static_cast<size_t>(st) * 2].size() * 2);
            for (size_t k = 0; k < stemOut[static_cast<size_t>(st) * 2].size(); ++k) {
                inter.push_back(stemOut[static_cast<size_t>(st) * 2][k]);
                inter.push_back(stemOut[static_cast<size_t>(st) * 2 + 1][k]);
            }
            const std::string path = stemPrefix + "_" + Engine::stemName(st) + ".wav";
            if (!writeWav(path, inter, 2, sr)) { std::fprintf(stderr, "cannot write a stem\n"); return 1; }
            std::printf("stem: %s\n", path.c_str());
        }
    }
    std::printf("wrote %s\n", out.c_str());
    return nans == 0 ? 0 : 1;
}

// --batch <file>: render every preset named in the file (one name per line, '#' comments), one
// after another, in this one process.
//
// It is not a second code path: the command line is used exactly as it stands, with the name
// after --preset replaced for each line, and runOnce does the rest. So a batch of eight thousand
// measures every preset the same way eight thousand separate calls would -- checked by comparing
// the measure lines of both -- while paying the fixed cost of starting a process and reading the
// pack files once instead of eight thousand times. On this machine that was 0.7 s a preset.
int main(int argc, char** argv)
{
    std::string batchFile;
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--batch" && i + 1 < argc) { batchFile = argv[++i]; continue; }
        args.push_back(a);
    }
    if (batchFile.empty()) return runOnce(argc, argv);

    std::vector<std::string> names;
    {
        std::ifstream f(batchFile);
        if (!f) { std::fprintf(stderr, "cannot read the batch list %s\n", batchFile.c_str()); return 2; }
        std::string line;
        while (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            size_t b = line.find_first_not_of(" \t");
            if (b == std::string::npos) continue;
            line = line.substr(b);
            if (line[0] == '#') continue;
            names.push_back(line);
        }
    }
    if (names.empty()) { std::fprintf(stderr, "the batch list %s names no preset\n", batchFile.c_str()); return 2; }

    // Where the preset name goes. Without a --preset on the line one is inserted at the FRONT,
    // never appended: a preset applies its own values to every parameter, so one that arrived
    // after a --set would undo it. Appended, "--set brain_rate=6" was silently lost and the batch
    // measured a different sound than the same command line one preset at a time.
    size_t slot = 0;
    for (size_t i = 1; i + 1 < args.size(); ++i) if (args[i] == "--preset") { slot = i + 1; break; }
    if (slot == 0) {
        // Behind a --packs, never in front of it: the arguments are read in order, so a preset
        // inserted at the very front is looked up before the packs are loaded and a batch reports
        // every pack preset as unknown (it worked only because the callers also set AMBIENT_PACKS).
        // Still before any --set, which the preset would otherwise undo.
        size_t at = 1;
        for (size_t i = 1; i + 1 < args.size(); ++i) if (args[i] == "--packs") { at = i + 2; break; }
        args.insert(args.begin() + static_cast<std::ptrdiff_t>(at), { "--preset", "" });
        slot = at + 1;
    }

    int worst = 0;
    for (const std::string& name : names) {
        args[slot] = name;
        std::vector<char*> av;
        av.reserve(args.size());
        for (std::string& a : args) av.push_back(a.data());
        std::printf("batch: %s\n", name.c_str());
        std::fflush(stdout);
        const int rc = runOnce(static_cast<int>(av.size()), av.data());
        if (rc != 0) { std::fprintf(stderr, "batch: %s returned %d\n", name.c_str(), rc); worst = rc; }
    }
    return worst;
}
