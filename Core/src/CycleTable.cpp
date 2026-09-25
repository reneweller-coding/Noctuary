/**
 * @file CycleTable.cpp
 * @brief Building the cycle stacks: analysis, resynthesis at ten levels, and the built-in tables.
 *
 * The playing side of the classic wavetable -- reading a stored cycle with a phase accumulator and a
 * four-point interpolation -- is inline in CycleTable.h. This file is the building side, which never
 * runs on the audio thread. build() takes a file's frames apart into Fourier coefficients
 * (analyseCycle), buildFromHarmonics() puts every frame back together at each of the ten levels
 * with only the harmonics that level keeps (synthesise) and scales the whole table so its loudest
 * frame sits at kTargetRms; cycleLevelFor() chooses the level a note reads, with hysteresis;
 * detectCycleLength() guesses the layout of a file that does not say; and BuiltinCycles writes the
 * five built-in tables from their spectra on first use. Sources.h supplies the spectra of the
 * built-in tables (builtinTable) and Cosmos.h the Fft.
 */
#include "ambient/CycleTable.h"
#include "ambient/Sources.h"   // the spectra of the built-in tables
#include "ambient/Cosmos.h"    // Fft
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace ambient {

namespace {

/** @brief One frame's harmonics: element h-1 is harmonic h as the complex amplitude of a cosine. */
using Coeffs = std::vector<std::complex<double>>;
constexpr double kPiD = 3.14159265358979323846;   ///< pi in double, for the plain Fourier sums

/**
 * @brief The harmonics of one cycle of `len` samples, as complex amplitudes of cosines.
 *
 * A power of two goes
 * through the FFT; any other length through the plain sum, which for a single cycle of a few hundred
 * samples costs nothing worth a second code path.
 *
 * @param x    the cycle's samples, `len` of them
 * @param len  the cycle length in samples
 * @param fft  a transform of size `len`, or nullptr when `len` is no power of two
 * @param re   scratch of at least `len` floats, used only with @p fft
 * @param im   scratch of at least `len` floats, used only with @p fft
 * @return     harmonics 1 .. top, top being the finest level's count or the cycle's own Nyquist,
 *             whichever is lower; empty for a cycle too short to hold a harmonic
 */
Coeffs analyseCycle(const float* x, int len, const Fft* fft, std::vector<float>& re, std::vector<float>& im)
{
    // Below the cycle's own Nyquist, and no more than the finest level keeps.
    const int top = std::min(CycleTable::levelHarmonics(0), (len - 1) / 2);
    Coeffs c(static_cast<size_t>(std::max(top, 0)));
    if (top <= 0) return c;
    const double scale = 2.0 / static_cast<double>(len);
    if (fft != nullptr) {
        std::memcpy(re.data(), x, sizeof(float) * static_cast<size_t>(len));
        std::fill(im.begin(), im.begin() + len, 0.0f);
        fft->transform(re.data(), im.data(), false);
        for (int h = 1; h <= top; ++h)
            c[static_cast<size_t>(h - 1)] = std::complex<double>(re[static_cast<size_t>(h)], im[static_cast<size_t>(h)]) * scale;
    } else {
        for (int h = 1; h <= top; ++h) {
            const double w = 2.0 * kPiD * h / static_cast<double>(len);
            double sr = 0.0, si = 0.0;
            for (int n = 0; n < len; ++n) { sr += x[n] * std::cos(w * n); si -= x[n] * std::sin(w * n); }
            c[static_cast<size_t>(h - 1)] = std::complex<double>(sr, si) * scale;
        }
    }
    return c;
}

/**
 * @brief One stored cycle at a level: the harmonics the level keeps, placed in a spectrum of its length and
 *        transformed back.
 *
 * `out` points at sample 0; the guards either side are written as well.
 *
 * @param c      the frame's harmonics, as analyseCycle() or the built-in spectra give them
 * @param gain   the table's scale: kTargetRms over the RMS of its loudest frame
 * @param level  0 .. kLevels - 1, the resolution to write
 * @param fft    a transform of size levelLength(level)
 * @param re     scratch of at least levelLength(level) floats
 * @param im     scratch of at least levelLength(level) floats
 * @param out    where sample 0 of the cycle goes; out[-1] and out[len], out[len + 1] are written too
 */
void synthesise(const Coeffs& c, double gain, int level, const Fft& fft, std::vector<float>& re, std::vector<float>& im, float* out)
{
    const int len = CycleTable::levelLength(level);
    const int top = std::min(CycleTable::levelHarmonics(level), static_cast<int>(c.size()));
    std::fill(re.begin(), re.begin() + len, 0.0f);
    std::fill(im.begin(), im.begin() + len, 0.0f);
    const double half = 0.5 * static_cast<double>(len) * gain;
    for (int h = 1; h <= top; ++h) {
        const std::complex<double> v = c[static_cast<size_t>(h - 1)] * half;
        re[static_cast<size_t>(h)] = static_cast<float>(v.real());
        im[static_cast<size_t>(h)] = static_cast<float>(v.imag());
        re[static_cast<size_t>(len - h)] = static_cast<float>(v.real());
        im[static_cast<size_t>(len - h)] = static_cast<float>(-v.imag());
    }
    fft.transform(re.data(), im.data(), true);
    for (int n = 0; n < len; ++n) out[n] = re[static_cast<size_t>(n)];
    out[-1] = out[len - 1];
    out[len] = out[0];
    out[len + 1] = out[1];
}

} // namespace

bool CycleTable::buildFromHarmonics(const std::vector<Coeffs>& in)
{
    clear();
    const int count = std::min(static_cast<int>(in.size()), kMaxFrames);
    if (count <= 0) return false;
    // The loudest frame, by Parseval: a cosine of amplitude A carries A^2 / 2.
    double loudest = 0.0;
    for (int k = 0; k < count; ++k) {
        double e = 0.0;
        const int top = std::min(static_cast<int>(in[static_cast<size_t>(k)].size()), levelHarmonics(0));
        for (int h = 0; h < top; ++h) e += std::norm(in[static_cast<size_t>(k)][static_cast<size_t>(h)]);
        loudest = std::max(loudest, std::sqrt(0.5 * e));
    }
    if (!(loudest > 1.0e-9)) return false;
    const double gain = static_cast<double>(kTargetRms) / loudest;

    size_t total = 0;
    for (int l = 0; l < kLevels; ++l) {
        offset[l] = static_cast<int>(total);
        total += static_cast<size_t>(count) * static_cast<size_t>(levelLength(l) + kGuard);
    }
    data.assign(total, 0.0f);
    std::vector<float> re(static_cast<size_t>(kStoreLen)), im(static_cast<size_t>(kStoreLen));
    std::unique_ptr<Fft> fft;
    for (int l = 0; l < kLevels; ++l) {
        const int len = levelLength(l);
        if (fft == nullptr || fft->size() != len) fft = std::make_unique<Fft>(len);
        for (int k = 0; k < count; ++k) {
            float* out = data.data() + static_cast<size_t>(offset[l]) + static_cast<size_t>(k) * static_cast<size_t>(len + kGuard) + 1;
            synthesise(in[static_cast<size_t>(k)], gain, l, *fft, re, im, out);
        }
    }
    frames = count;
    return true;
}

bool CycleTable::build(const float* mono, int n, int cycleLen)
{
    clear();
    if (mono == nullptr || cycleLen < 8 || n < cycleLen) return false;
    const int total = n / cycleLen;
    const int keep = std::min(total, kMaxFrames);
    const bool pow2 = (cycleLen & (cycleLen - 1)) == 0;
    std::unique_ptr<Fft> fft;
    std::vector<float> re, im;
    if (pow2) {
        fft = std::make_unique<Fft>(cycleLen);
        re.assign(static_cast<size_t>(cycleLen), 0.0f);
        im.assign(static_cast<size_t>(cycleLen), 0.0f);
    }
    std::vector<Coeffs> frameCoeffs(static_cast<size_t>(keep));
    for (int k = 0; k < keep; ++k) {
        const int src = (keep == total) ? k : static_cast<int>(static_cast<long long>(k) * (total - 1) / std::max(keep - 1, 1));
        frameCoeffs[static_cast<size_t>(k)] = analyseCycle(mono + static_cast<size_t>(src) * static_cast<size_t>(cycleLen), cycleLen, fft.get(), re, im);
    }
    return buildFromHarmonics(frameCoeffs);
}

int cycleLevelFor(double hz, double sampleRate, int current)
{
    const double nyquist = 0.5 * sampleRate;
    int floorLevel = 0;
    while (floorLevel < CycleTable::kLevels - 1 && CycleTable::levelHarmonics(floorLevel) * hz >= nyquist) ++floorLevel;
    if (current > floorLevel && current < CycleTable::kLevels) {
        int l = floorLevel;
        while (l < current && CycleTable::levelHarmonics(l) * hz > 0.9 * nyquist) ++l;
        return l;
    }
    return floorLevel;
}

int detectCycleLength(const float* x, int n)
{
    const int L = CycleTable::kLen;
    if (x == nullptr || n <= 0) return 0;
    if (n == 64 * 256) {
        // The size of a WaveEdit bank (64 cycles of 256 samples, the E352 layout) and of eight frames
        // of 2048. What tells the two apart is where the wave is continuous. In a bank, a cycle's last
        // sample leads round to its own first as smoothly as on to the next cycle's first; in a frame
        // of 2048 the sample after a 256th boundary follows on smoothly, and the one 255 samples back
        // is somewhere else entirely. A spectral test was tried first and does not separate them: a
        // bank whose cycles change puts energy everywhere. This does. Measured over the 705 banks of
        // WaveEdit Online and 2476 eight-frame cuts from real tables, a ratio under 3 takes 72 % of
        // the banks -- most of the rest hold no single cycles at all -- and 0.08 % of the tables.
        constexpr int c = 256;
        double wrap = 0.0, onward = 0.0;
        for (int k = 0; k < 64; ++k) wrap += std::fabs(static_cast<double>(x[k * c + c - 1]) - x[k * c]);
        for (int k = 0; k < 63; ++k) onward += std::fabs(static_cast<double>(x[k * c + c - 1]) - x[(k + 1) * c]);
        return wrap < 3.0 * onward * (64.0 / 63.0) ? c : L;
    }
    if (n % L == 0) return L;                              // the common layout
    if (n <= 2 * L) return n;                              // one cycle of its own length
    for (int len : { 1024, 512, 256 }) if (n % len == 0) return len;
    return L;                                              // the common layout, the remainder dropped
}

namespace {

/**
 * @brief The five built-in tables, made once from their spectra behind builtinCycleTable().
 *
 * A function-local static of this type is built on the first call -- a few FFTs per frame, paid
 * once, off the audio thread: the Classic table from the Fourier series of the real waveforms with
 * their phases, the other four from the Harmonic type's spectra written out with sine phases.
 */
struct BuiltinCycles {
    CycleTable t[kNumTables - 1];   ///< index 0 Classic, then Organ, Vocal, Glass and Metal
    /** @brief Writes all five tables; every frame is brought to the same RMS first. */
    BuiltinCycles()
    {
        const int H = CycleTable::levelHarmonics(0);
        // Each built-in frame is brought to the same RMS before the table is made, the way the
        // Harmonic type's built-ins are normalised frame by frame: a morph from sine to pulse should
        // not fade out as it goes.
        auto unit = [](Coeffs c) {
            double e = 0.0;
            for (const auto& v : c) e += std::norm(v);
            const double s = e > 0.0 ? 1.0 / std::sqrt(0.5 * e) : 0.0;
            for (auto& v : c) v *= s;
            return c;
        };
        const std::complex<double> sine(0.0, -1.0);   // sin x = cos(x - pi/2)
        // Classic: sine, triangle, saw, square, a pulse of a fifth -- the real waveforms, to the
        // 512th harmonic, with their phases.
        {
            std::vector<Coeffs> f(5, Coeffs(static_cast<size_t>(H)));
            f[0][0] = sine;
            for (int h = 1; h <= H; ++h) {
                const double sign = (h % 2 == 0) ? -1.0 : 1.0;
                if (h % 2 == 1) f[1][static_cast<size_t>(h - 1)] = sine * ((((h - 1) / 2) % 2 == 0 ? 1.0 : -1.0) * 8.0 / (kPiD * kPiD * h * h));
                f[2][static_cast<size_t>(h - 1)] = sine * (sign * 2.0 / (kPiD * h));
                if (h % 2 == 1) f[3][static_cast<size_t>(h - 1)] = sine * (4.0 / (kPiD * h));
                f[4][static_cast<size_t>(h - 1)] = std::complex<double>(2.0 / (kPiD * h) * std::sin(kPiD * h * 0.2), 0.0);
            }
            for (auto& c : f) c = unit(std::move(c));
            t[0].buildFromHarmonics(f);
        }
        // Organ, Vocal, Glass, Metal: the Harmonic type's spectra, written out with sine phases.
        for (int i = 1; i < kNumTables - 1; ++i) {
            const Wavetable& w = builtinTable(i);
            std::vector<Coeffs> f(static_cast<size_t>(w.frames), Coeffs(static_cast<size_t>(kTablePartials)));
            for (int k = 0; k < w.frames; ++k)
                for (int h = 0; h < kTablePartials; ++h)
                    f[static_cast<size_t>(k)][static_cast<size_t>(h)] = sine * static_cast<double>(w.amp[k][h]);
            for (auto& c : f) c = unit(std::move(c));
            t[i].buildFromHarmonics(f);
        }
    }
};

} // namespace

const CycleTable& builtinCycleTable(int index)
{
    static const BuiltinCycles b;
    return b.t[index < 0 ? 0 : (index > kNumTables - 2 ? kNumTables - 2 : index)];
}

} // namespace ambient
