/**
 * @file ZPlane.cpp
 * @brief The z-plane filter's arithmetic: the modal bank, the corner frames and their
 *        interpolation, and the normalised biquad cascade.
 *
 * ZPlane.h describes the idea and declares the pieces; this file builds them. The generated bank
 * -- a hundred and fifty-five shapes of eight corner descriptors each, with their names and
 * families -- is compiled in from ZPlaneBank.inc, which Tools/make_zplane_bank.py writes.
 * zFrameFromSpec() turns one compact corner descriptor into a frame of up to six sections;
 * zInterpolate() blends the eight corners of a shape trilinearly in the log-frequency /
 * log-bandwidth domain, over logarithms that are taken once per shape and cached (ShapeCorners),
 * because the interpolation runs at control rate for every sounding voice and the corners never
 * change.
 *
 * zBuildCascade() is the cascade's normalisation: every section is scaled to its geometric mean
 * over a fixed 16-point logarithmic grid plus its own pole and zero angles, and the finished
 * cascade is measured over the same points so that its loudest point comes back to unity -- the
 * reason a shape can be extremely resonant without becoming loud. ZModal is the same frames read
 * as a parallel bank of ringing resonators, each normalised to unity at its own frequency and the
 * bank as a whole on expected power. The two warm-up functions build the static tables from the
 * message thread, so that no voice ever builds them under the lock a function-local static carries.
 */
#include <vector>
#include "ambient/ZPlane.h"
#include <algorithm>

namespace ambient {

const char* const kZModeNames[4] = { "Off", "Series", "Replace", "Modal" };

// ---------------------------------------------------------------- modal bank

void ZModal::reset()
{
    for (Mode& m : modes_) { m.z1[0] = m.z1[1] = 0.0f; m.z2[0] = m.z2[1] = 0.0f; }
}

void ZModal::set(const ZFrame& f, float decaySeconds, float damp)
{
    used_ = 0;
    const float nyq = sr_ * 0.49f;
    const float d = clampv(decaySeconds, 0.02f, 40.0f);
    const float dp = clampv(damp, 0.0f, 1.0f);
    // The lowest mode sets the reference: with damp = 1 every other mode's decay is shortened in
    // proportion to how far above it sits, which is what makes a bright object sound short and a
    // heavy one long without touching two knobs.
    float lowest = 1.0e9f;
    for (int i = 0; i < f.used; ++i) if (f.s[i].poleHz > 0.0f) lowest = std::min(lowest, f.s[i].poleHz);
    if (lowest > 1.0e8f) lowest = 100.0f;

    float power = 0.0f;
    for (int i = 0; i < f.used && used_ < kZSections; ++i) {
        const float hz = f.s[i].poleHz;
        if (hz <= 5.0f || hz >= nyq) continue;               // nothing above Nyquist can ring
        Mode& m = modes_[used_];
        const float t60 = std::max(0.01f, d * std::pow(lowest / hz, dp));
        // T60 to pole radius: 60 dB is a factor of 1000, so r^(t60*sr) = 1e-3.
        const float r = std::exp(-6.907755f / (t60 * sr_));
        const float w = 2.0f * kPi * hz / sr_;
        m.hz = hz;
        m.r  = std::min(r, 0.9999995f);                      // strictly inside the unit circle
        m.a1 = 2.0f * m.r * std::cos(w);
        m.a2 = -(m.r * m.r);
        // Unity gain at the mode's own frequency: |H| = b0 / |1 - a1 e^-jw - a2 e^-2jw|.
        const float c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2.0f * w), s2 = std::sin(2.0f * w);
        const float dr = 1.0f - m.a1 * c1 - m.a2 * c2, di = m.a1 * s1 + m.a2 * s2;
        m.b0 = std::sqrt(dr * dr + di * di);
        m.gain = f.s[i].gain;
        power += m.gain * m.gain;
        ++used_;
    }
    // Normalised on expected power rather than on the sum of the peaks. The modes are at
    // different frequencies and almost never in phase, so adding their peaks would leave a bank
    // of six far quieter than a bank of two; adding their powers keeps the level even.
    norm_ = power > 0.0f ? 1.0f / std::sqrt(power) : 0.0f;
}

float ZModal::magnitudeAt(float hz) const
{
    // The modes add. |H| of the bank is the magnitude of the sum, and the phases matter, so this
    // sums the complex responses rather than the magnitudes.
    const float w = 2.0f * kPi * clampv(hz, 1.0f, sr_ * 0.5f) / sr_;
    const float c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2.0f * w), s2 = std::sin(2.0f * w);
    float re = 0.0f, im = 0.0f;
    for (int i = 0; i < used_; ++i) {
        const Mode& m = modes_[i];
        const float dr = 1.0f - m.a1 * c1 - m.a2 * c2, di = m.a1 * s1 + m.a2 * s2;
        const float den = std::max(dr * dr + di * di, 1e-20f);
        // b0 / (dr + j di) = b0 (dr - j di) / |den|
        re += m.gain * m.b0 * dr / den;
        im -= m.gain * m.b0 * di / den;
    }
    return std::sqrt(re * re + im * im) * norm_;
}


#include "ZPlaneBank.inc"

ZFrame zFrameFromSpec(const ZCornerSpec& c)
{
    ZFrame f;
    for (int i = 0; i < kZSections; ++i) {
        if (c.hz[i] <= 0.0f) break;
        ZSection& s = f.s[i];
        s.poleHz = c.hz[i];
        s.poleBw = std::max(c.hz[i] * c.bwRatio, 0.5f);
        s.zeroHz = c.zeroRatio > 0.0f ? c.hz[i] * c.zeroRatio : 0.0f;
        s.zeroBw = s.zeroHz * c.zeroBwRatio;
        s.gain   = std::pow(c.tilt, static_cast<float>(i));
        f.used = i + 1;
    }
    return f;
}

namespace {
/**
 * @brief The eight corners of a shape, and the logarithms the interpolation takes of them.
 *
 * Both depend
 * on the shape alone -- on a number that changes when somebody turns a knob, and not otherwise --
 * and both used to be computed afresh on every call: eight corner frames rebuilt, and then
 * thirty-two logarithms per section taken of the values that had just been rebuilt. This runs at
 * control rate for every sounding voice, so at eleven voices that was eight thousand times a
 * second of audio, always with the same answer. VTune put it at nine per cent of an entire
 * render, which is roughly what the whole reverb costs.
 *
 * Held here instead, worked out once for each shape the moment it is first asked for. The
 * interpolation itself is untouched -- the same weights over the same logarithms, so the same
 * numbers to the last bit; only the arithmetic that had no reason to be repeated is gone.
 */
struct ShapeCorners {
    int  used = 0;   ///< sections every one of the eight corners has; the interpolated frame has as many
    /**
     * @brief One section as seen from all eight corners: the values the interpolation blends,
     *        corner by corner.
     *
     * One value per corner in each array; the logarithms are log2 of the value floored at 1e-3,
     * exactly as zInterpolate used to take them on every call.
     */
    struct Sec {
        float lPole[8] = {},     ///< log2 of the pole frequency in Hz
              lBw[8] = {},       ///< log2 of the pole bandwidth in Hz
              lZeroHz[8] = {},   ///< log2 of the zero frequency in Hz (meaningful only when `zeros`)
              lZeroBw[8] = {},   ///< log2 of the zero bandwidth in Hz
              gain[8] = {};      ///< the section gain, blended linearly
        bool  zeros = false;   ///< a zero in every corner, or none at all
    } s[kZSections];   ///< the sections, the first `used` of them meaningful
};

/**
 * @brief The cached corners of a shape, built for every shape on the first call.
 *
 * The whole table is a function-local static, so the first caller builds it -- all shapes at once,
 * from kZCorners through zFrameFromSpec -- under the guard the language puts around the static.
 * zWarmShapes() makes sure that first caller is the message thread.
 *
 * @param shape  a valid shape index, 0 .. kZShapes - 1 (zInterpolate clamps before asking)
 * @return       the corners and their logarithms for that shape
 */
const ShapeCorners& shapeCorners(int shape)
{
    static const std::vector<ShapeCorners> all = [] {
        std::vector<ShapeCorners> v(static_cast<size_t>(kZShapes));
        for (int sh = 0; sh < kZShapes; ++sh) {
            ZFrame c[8];
            for (int k = 0; k < 8; ++k) c[k] = zFrameFromSpec(kZCorners[sh][k]);
            ShapeCorners& out = v[static_cast<size_t>(sh)];
            out.used = c[0].used;
            for (int k = 1; k < 8; ++k) out.used = std::min(out.used, c[k].used);
            for (int i = 0; i < out.used; ++i) {
                ShapeCorners::Sec& d = out.s[i];
                d.zeros = true;
                for (int k = 0; k < 8; ++k) if (c[k].s[i].zeroHz <= 0.0f) d.zeros = false;
                for (int k = 0; k < 8; ++k) {
                    d.lPole[k]   = std::log2(std::max(c[k].s[i].poleHz, 1e-3f));
                    d.lBw[k]     = std::log2(std::max(c[k].s[i].poleBw, 1e-3f));
                    d.lZeroHz[k] = std::log2(std::max(c[k].s[i].zeroHz, 1e-3f));
                    d.lZeroBw[k] = std::log2(std::max(c[k].s[i].zeroBw, 1e-3f));
                    d.gain[k]    = c[k].s[i].gain;
                }
            }
        }
        return v;
    }();
    return all[static_cast<size_t>(shape)];
}
}  // namespace

/**
 * Builds the shape table now, off the audio thread. Engine::prepare calls it; without that the
 * first voice to reach a filter would build it under the lock the language puts around a static.
 */
void zWarmShapes() { (void)shapeCorners(0); }

ZFrame zInterpolate(int shape, float x, float y, float z)
{
    shape = clampv(shape, 0, kZShapes - 1);
    x = clampv(x, 0.0f, 1.0f); y = clampv(y, 0.0f, 1.0f); z = clampv(z, 0.0f, 1.0f);
    const ShapeCorners& c = shapeCorners(shape);
    ZFrame out;
    out.used = c.used;
    // Trilinear. At z = 0 only the first four terms have any weight, so this is exactly the
    // bilinear interpolation it replaces -- which is what makes the third axis free to add.
    auto lerp3 = [&](const float* v) {
        const float f0 = (v[0] * (1 - x) + v[1] * x) * (1 - y) + (v[2] * (1 - x) + v[3] * x) * y;
        const float f1 = (v[4] * (1 - x) + v[5] * x) * (1 - y) + (v[6] * (1 - x) + v[7] * x) * y;
        return f0 * (1 - z) + f1 * z;
    };
    for (int i = 0; i < out.used; ++i) {
        ZSection& s = out.s[i];
        const ShapeCorners::Sec& d = c.s[i];
        s.poleHz = std::exp2(lerp3(d.lPole));
        s.poleBw = std::exp2(lerp3(d.lBw));
        // A zero that exists in every corner is interpolated; if any corner has none, there is none.
        s.zeroHz = d.zeros ? std::exp2(lerp3(d.lZeroHz)) : 0.0f;
        s.zeroBw = d.zeros ? std::exp2(lerp3(d.lZeroBw)) : 0.0f;
        s.gain   = lerp3(d.gain);
    }
    return out;
}

namespace {
/**
 * @brief The normalisation grid: 16 logarithmically spaced points in normalised frequency (about
 *        24 Hz to 21 kHz at 48 kHz), with cos/sin of w and 2w precomputed once.
 */
constexpr int kGrid = 16;
/**
 * @brief The precomputed trigonometry of the normalisation grid, one set per grid point.
 *
 * The angular frequencies are w = pi * 0.001 * 900^(k / 15), so the grid runs from 0.001 pi to
 * 0.9 pi -- a fixed fraction of the sample rate, not a fixed frequency -- and the cos/sin pairs are
 * exactly what ZBiquad::magSqAt() takes, so a magnitude on the grid costs no transcendental call.
 */
struct NormGrid {
    float c1[kGrid],   ///< cos(w) at each grid point
          s1[kGrid],   ///< sin(w)
          c2[kGrid],   ///< cos(2w)
          s2[kGrid];   ///< sin(2w)
    /** @brief Fills the four tables; run once, when grid() is first called. */
    NormGrid()
    {
        for (int k = 0; k < kGrid; ++k) {
            const float w = kPi * 0.001f * std::pow(900.0f, static_cast<float>(k) / (kGrid - 1));
            c1[k] = std::cos(w); s1[k] = std::sin(w);
            c2[k] = std::cos(2.0f * w); s2[k] = std::sin(2.0f * w);
        }
    }
};
/**
 * @brief The one normalisation grid, built on the first call.
 * @return  the grid's cos/sin tables; the same object for the life of the process
 */
const NormGrid& grid() { static const NormGrid g; return g; }
}

/**
 * Builds the normalisation grid now, on whoever calls -- prepare() does, from the message thread.
 * Left alone, the first voice to build a cascade did it on the audio thread, inside the guard
 * the language puts around a function-local static: a lock, where no lock belongs.
 */
void zWarmTables() { (void)grid(); }

namespace {
}

float zBuildCascade(const ZFrame& f, ZBiquad* ch, float sr)
{
    const NormGrid& g = grid();
    const int n = clampv(f.used, 0, kZSections);
    if (n == 0) return 1.0f;
    // The probe points: the fixed grid plus every pole and zero angle of this frame. A needle-
    // sharp resonance has its maximum at its own pole angle, which no fixed grid would catch.
    constexpr int kMaxPoints = kGrid + 2 * kZSections;
    float c1[kMaxPoints], s1[kMaxPoints], c2[kMaxPoints], s2[kMaxPoints];
    for (int k = 0; k < kGrid; ++k) { c1[k] = g.c1[k]; s1[k] = g.s1[k]; c2[k] = g.c2[k]; s2[k] = g.s2[k]; }
    int points = kGrid;
    auto addAngle = [&](float hz) {
        if (hz <= 0.0f || points >= kMaxPoints) return;
        const float w = kTwoPi * clampv(hz, 10.0f, sr * 0.48f) / sr;
        c1[points] = std::cos(w); s1[points] = std::sin(w);
        c2[points] = std::cos(2.0f * w); s2[points] = std::sin(2.0f * w);
        ++points;
    };
    for (int i = 0; i < n; ++i) { addAngle(f.s[i].poleHz); addAngle(f.s[i].zeroHz); }
    // One pass gives both normalisations: the row maximum per section, and the product down
    // each column for the finished cascade.
    float mag[kZSections][kMaxPoints];
    for (int i = 0; i < n; ++i) {
        ch[i].set(f.s[i], sr);
        float rowMax = 1e-20f, logSum = 0.0f;
        for (int k = 0; k < points; ++k) {
            mag[i][k] = std::max(ch[i].magSqAt(c1[k], s1[k], c2[k], s2[k]), 1e-20f);
            rowMax = std::max(rowMax, mag[i][k]);
            logSum += std::log(mag[i][k]);
        }
        // Normalise the section to its geometric mean over the grid, not to its peak: a bell
        // (zero on the pole, wider) then keeps unity background and boosts only its peak, so
        // six of them in series shape a spectrum instead of cancelling each other out. The
        // boost is capped at 30 dB per section so the cascade cannot overflow.
        const float gmean = std::exp(0.5f * logSum / static_cast<float>(points));
        const float scale = std::min(f.s[i].gain / gmean, 32.0f / std::sqrt(rowMax));
        ch[i].b0 *= scale; ch[i].b1 *= scale; ch[i].b2 *= scale;
        const float sq = scale * scale;
        for (int k = 0; k < points; ++k) mag[i][k] *= sq;
    }
    float peakSq = 1e-20f;
    for (int k = 0; k < points; ++k) {
        float prod = 1.0f;
        for (int i = 0; i < n; ++i) prod *= mag[i][k];
        peakSq = std::max(peakSq, prod);
    }
    return clampv(1.0f / std::sqrt(peakSq), 1e-4f, 16.0f);
}

void ZBiquad::set(const ZSection& s, float sr)
{
    // Poles: radius from the bandwidth, angle from the centre frequency. The radius is capped
    // well inside the unit circle (about 0.4 s of ringing), so no interpolated point can blow up.
    const float f = clampv(s.poleHz, 20.0f, sr * 0.45f);
    const float r = std::min(std::exp(-kPi * std::max(s.poleBw, 0.5f) / sr), 0.99995f);
    const float th = kTwoPi * f / sr;
    a1 = -2.0f * r * std::cos(th);
    a2 = r * r;
    if (s.zeroHz > 0.0f) {
        const float fz = clampv(s.zeroHz, 20.0f, sr * 0.48f);
        const float rz = std::min(std::exp(-kPi * std::max(s.zeroBw, 0.5f) / sr), 0.9999f);
        const float tz = kTwoPi * fz / sr;
        b0 = 1.0f; b1 = -2.0f * rz * std::cos(tz); b2 = rz * rz;
    } else {
        b0 = 1.0f; b1 = 0.0f; b2 = 0.0f;
    }
    // The gain is applied by zBuildCascade, which normalises the section over the whole grid.
}

float ZBiquad::magnitudeAt(float w) const
{
    const float c1 = std::cos(w), s1 = std::sin(w);
    const float c2 = std::cos(2.0f * w), s2 = std::sin(2.0f * w);
    const float nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
    const float dr = 1.0f + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
    const float den = dr * dr + di * di;
    return std::sqrt((nr * nr + ni * ni) / std::max(den, 1e-20f));
}

} // namespace ambient
