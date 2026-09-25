/**
 * @file Memory.cpp
 * @brief The drifting sound memory of Memory.h: the pool, the lines, the exchange and the grains.
 *
 * Memory.h says what the device is and where each of its knobs comes from; this file is how it is
 * done, in the order the audio thread meets it. prepare() lays the 90-second pool out, designs the
 * reading interpolator (an eight-tap Kaiser-windowed sinc in 256 phases) and the fingerprint's window
 * and bin-to-pitch-class map, and allocates everything -- nothing is allocated after it. The setters
 * only clamp and store; the work of a changed setting is done in stepControl(), once per control
 * block of kSub samples, where the number of lines is re-laid, the lengths are found as primes, the
 * gains follow Hold and Age, the Lorenz attractor is integrated, the tape speed glides, and every
 * line's slide, pan and anti-alias filter are advanced. process() then runs the lines sample by
 * sample: the sinc read, the scattering all-pass, the absorption, the DC blocker, the butterfly
 * exchange, the per-line limiter and saturation, and the splatting write head (tapeWrite()). Recall
 * runs first in each control block, so its grains read tape this block has not yet written over.
 *
 * The fingerprints that Seek listens for are computed one per control block at most (analyse()),
 * from a queue of pool segments the write heads have just finished, so no block ever pays for more
 * than one chroma FFT. The anonymous namespace holds the constants that give the lines their
 * incommensurable periods and the small number-theoretic helpers the layout needs.
 */
#include "ambient/Memory.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace ambient {

namespace {

constexpr double kLn1000 = 6.907755278982137;   ///< ln 1000, the fall to -60 dB: turns a T60 into a gain per pass in gains()
constexpr double kTau = 6.283185307179586;      ///< two pi in double, for the slide, pan and stage phases
constexpr float  kLimit = 0.7f;              ///< each line's loop is held under this level
constexpr int    kPoolMaxCells = 8640000;    ///< 90 s at 96 kHz, 45 s at 192 kHz
constexpr int    kQueueMask = 511;           ///< the fingerprint queue holds 512 segment indices and wraps by masking
/**
 * @brief The scattering all-passes, in milliseconds: mutually prime, so at full Blur an echo is spread over a
 * tenth to a sixth of a second on every pass, and no two lines spread it alike.
 */
constexpr float kApMs[Memory::kMaxLines] = { 41.1f, 53.3f, 67.9f, 79.3f, 97.1f, 113.3f, 131.9f, 149.3f };
/**
 * @brief The slides and the pans: periods between 19 and 83 seconds, none a multiple of another.
 *
 * This table is the slides: the rate, in Hz, of the sine each line's read head rides on before the
 * attractor bends it (stepControl()). kPanHz and kStageHz complete the set of periods.
 */
constexpr double kDriftHz[Memory::kMaxLines] = { 1.0 / 23.0, 1.0 / 29.0, 1.0 / 31.0, 1.0 / 37.0, 1.0 / 41.0, 1.0 / 47.0, 1.0 / 53.0, 1.0 / 61.0 };
/** @brief The pans: the rate, in Hz, of each line's wander across the stereo field (see kDriftHz). */
constexpr double kPanHz[Memory::kMaxLines]   = { 1.0 / 67.0, 1.0 / 59.0, 1.0 / 43.0, 1.0 / 71.0, 1.0 / 49.0, 1.0 / 79.0, 1.0 / 57.0, 1.0 / 83.0 };
/** @brief The rates, in Hz, of the sines on which the three stages of the exchange lean off the common angle. */
constexpr double kStageHz[3] = { 1.0 / 19.0, 1.0 / 27.0, 1.0 / 35.0 };

/**
 * @brief The modified Bessel function of the first kind, order zero: the Kaiser window's shape.
 *
 * The power series, summed until a term falls below 1e-16 of the sum; for the arguments a window of
 * beta 5 asks for that is well within the forty terms allowed.
 * @param x  the argument, non-negative where it is used
 * @return   I0(x), which is 1 at x = 0 and grows like exp(x)
 */
double besselI0(double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 40; ++k) {
        const double h = x / (2.0 * k);
        term *= h * h;
        sum += term;
        if (term < 1.0e-16 * sum) break;
    }
    return sum;
}

/**
 * @brief The smallest power of two not below @p n: the all-pass rings are sized so that a mask can wrap them.
 * @param n  the length that has to fit; 1 or less gives 1
 * @return   the power of two, at least @p n
 */
int pow2At(int n) { int p = 1; while (p < n) p <<= 1; return p; }

/**
 * @brief Whether @p n is prime, by trial division up to its square root.
 * @param n  any integer; anything below 2 is not prime
 * @return   true for a prime
 */
bool isPrime(int n)
{
    if (n < 2) return false;
    if (n % 2 == 0) return n == 2;
    for (int d = 3; d * d <= n; d += 2)
        if (n % d == 0) return false;
    return true;
}

/**
 * @brief The largest prime not above @p n: every line's length is one, so no two lines share a period.
 *
 * Counts down from @p n testing each candidate, which near two million is a few thousand divisions
 * per line; lengths() therefore asks only when the size has really moved.
 * @param n  the upper bound, in cells
 * @return   the prime, or 2 when @p n is below 3
 */
int primeAtMost(int n)
{
    for (int x = n; x > 2; --x)
        if (isPrime(x)) return x;
    return 2;
}

/**
 * @brief Each line's place in the stereo field before it drifts: the left input's lines on the left, the
 * right's on the right, the longest furthest out.
 * @param k      the line, 0 .. lines - 1; even lines take the left input, odd ones the right
 * @param lines  how many lines are laid out, 2, 4 or 8
 * @return       the pan, -1 (left) .. 1 (right), between +-0.15 and +-0.85
 */
float panBase(int k, int lines)
{
    const float side = (k & 1) ? 1.0f : -1.0f;
    const float half = static_cast<float>(std::max(1, lines / 2));
    return side * (0.85f - 0.7f * static_cast<float>(k / 2) / half);
}

} // namespace

void Memory::prepare(double sampleRate, uint64_t seed)
{
    sr_ = sampleRate;
    rng_.seed(seed);
    poolCells_ = std::min(kPoolMaxCells, static_cast<int>(90.0 * sr_));
    pool_.assign(static_cast<size_t>(poolCells_), 0.0f);
    apSize_ = pow2At(static_cast<int>(0.2 * sr_) + 16);
    apMask_ = apSize_ - 1;
    ap_.assign(static_cast<size_t>(kMaxLines) * static_cast<size_t>(apSize_), 0.0f);
    apW_ = 0;
    for (int k = 0; k < kMaxLines; ++k) apLen_[k] = std::max(1, static_cast<int>(kApMs[k] * sr_ / 1000.0));
    // The reading interpolator: an eight-tap sinc under a Kaiser window (beta 5), in 256 phases with
    // a straight line between two, every row summing to one. At a whole cell it is exactly that cell,
    // so a head that does not slide reads its line without touching it.
    sinc_.assign(static_cast<size_t>((kPhases + 1) * kTaps), 0.0f);
    {
        const double beta = 5.0, norm = besselI0(beta);
        for (int ph = 0; ph <= kPhases; ++ph) {
            const double frac = static_cast<double>(ph) / kPhases;
            double row[kTaps], sum = 0.0;
            for (int j = 0; j < kTaps; ++j) {
                const double t = static_cast<double>(j - 3) - frac;   // tap j reads cell i1 - 3 + j
                const double nearest = std::round(t);
                double sinc;
                if (std::fabs(t - nearest) < 1.0e-9) sinc = nearest == 0.0 ? 1.0 : 0.0;
                else sinc = std::sin(3.141592653589793 * t) / (3.141592653589793 * t);
                const double u = t / 4.0;
                const double win = std::fabs(u) >= 1.0 ? 0.0 : besselI0(beta * std::sqrt(1.0 - u * u)) / norm;
                row[j] = sinc * win;
                sum += row[j];
            }
            for (int j = 0; j < kTaps; ++j) sinc_[static_cast<size_t>(ph * kTaps + j)] = static_cast<float>(row[j] / sum);
        }
    }
    const int segs = poolCells_ / kSeg + 2;
    chroma_.assign(static_cast<size_t>(segs) * 12, 0.0f);
    segRms_.assign(static_cast<size_t>(segs), 0.0f);
    queue_.assign(static_cast<size_t>(kQueueMask + 1), 0);
    qHead_ = qTail_ = 0;
    fftRe_.assign(kSeg, 0.0f);
    fftIm_.assign(kSeg, 0.0f);
    window_.resize(kSeg);
    for (int i = 0; i < kSeg; ++i) window_[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(kTau * i / kSeg));
    // Which pitch class each bin of the fingerprint's spectrum belongs to, from 55 Hz (below it a
    // bin is wider than a semitone) to 5 kHz (above it there are partials, not notes).
    binPc_.assign(kSeg / 2, -1);
    for (int b = 1; b < kSeg / 2; ++b) {
        const double hz = b * sr_ / kSeg;
        if (hz < 55.0 || hz > 5000.0) continue;
        const long pc = std::lround(12.0 * std::log2(hz / 16.351597831287414));
        binPc_[static_cast<size_t>(b)] = static_cast<signed char>(((pc % 12) + 12) % 12);
    }
    inBuf_.assign(kSeg, 0.0f);
    inPos_ = 0;
    std::memset(inputChroma_, 0, sizeof(inputChroma_));
    inputLevel_ = 0.0f;
    envAtk_ = 1.0f - std::exp(-1.0f / (0.01f * static_cast<float>(sr_)));
    envRel_ = 1.0f - std::exp(-1.0f / (0.3f * static_cast<float>(sr_)));
    limAtk_ = 1.0f - std::exp(-1.0f / (0.001f * static_cast<float>(sr_)));
    limRel_ = 1.0f - std::exp(-1.0f / (0.4f * static_cast<float>(sr_)));
    dcR_ = 1.0f - static_cast<float>(kTau * 3.0 / sr_);   // the loop's DC blocker: 3 Hz, below anything a line holds
    live_ = 0; started_ = 0; hazard_ = 0.0; nextHazard_ = 0.0;
    lorenz_[0] = 0.1; lorenz_[1] = 0.0; lorenz_[2] = 20.0;
    for (int s = 0; s < 3; ++s) stagePh_[s] = 0.37 * s;
    for (int k = 0; k < kMaxLines; ++k) {
        line_[k] = Line{};
        line_[k].driftPh = 0.61 * k;
        line_[k].panPh = 1.7 * k;
    }
    pendingLines_ = false;
    layout(linesWant_);
    v_ = vPrev_ = (reverse_ ? -1.0 : 1.0) * (half_ ? 0.5 : 1.0);
    gliding_ = false;
    angle_ = blur_ * 0.78539816f;
    {
        const float x = clampv((blur_ - 0.45f) / 0.55f, 0.0f, 1.0f);
        apCoef_ = 0.6f * x * x * (3.0f - 2.0f * x);
    }
    for (int s = 0; s < 3; ++s) { stageC_[s] = std::cos(angle_); stageS_[s] = std::sin(angle_); }
    recallCur_ = recall_; driveCur_ = drive_;
    inGainCur_ = (freeze_ || erase_) ? 0.0f : 1.0f;
    outGainCur_ = erase_ ? 0.0f : 1.0f;
    clearPos_ = -1; cleared_ = false;
    lengths();
    for (int k = 0; k < kMaxLines; ++k) line_[k].lenBase = line_[k].lenPrev = line_[k].lenNext = line_[k].lenTarget;
    quietFor_ = LLONG_MAX / 2;   // a memory that has heard nothing is quiet
    harmonySig_ = -1.0;
}

void Memory::setShape(int lines, float sizeSeconds, float blur, float drift)
{
    linesWant_ = lines <= 2 ? 2 : (lines <= 4 ? 4 : 8);
    size_ = clampv(sizeSeconds, 0.2f, 60.0f);
    blur_ = clampv(blur, 0.0f, 1.0f);
    drift_ = clampv(drift, 0.0f, 1.0f);
}

void Memory::setDecay(float hold, float age, float renew, float drive)
{
    hold_ = clampv(hold, 0.0f, 1.0f);
    age_ = clampv(age, 0.0f, 1.0f);
    renew_ = clampv(renew, 0.0f, 1.0f);
    drive_ = clampv(drive, 0.0f, 1.0f);
}

void Memory::setRecall(float recall, float seek, float grainMs)
{
    recall_ = clampv(recall, 0.0f, 1.0f);
    seek_ = clampv(seek, 0.0f, 1.0f);
    grainMs_ = clampv(grainMs, 20.0f, 2000.0f);
}

void Memory::setTape(bool reverse, bool half, bool freeze, bool erase)
{
    reverse_ = reverse; half_ = half; freeze_ = freeze; erase_ = erase;
}

void Memory::setHarmony(const FixedScale& scale, double tonicHz)
{
    if (!(tonicHz > 1.0) || !std::isfinite(tonicHz)) return;
    const int count = clampv(scale.count, 1, FixedScale::kMax);
    double sig = tonicHz + count * 1000.003;
    for (int k = 0; k < count; ++k) sig += scale.ratios[k] * (k + 1.618);
    if (sig == harmonySig_) return;
    harmonySig_ = sig;
    // The scale as pitch classes: every degree where its frequency falls, the tonic weighted up.
    float c[12] = {};
    for (int k = 0; k < count; ++k) {
        const double ratio = (scale.ratios[k] > 0.0 && std::isfinite(scale.ratios[k])) ? scale.ratios[k] : 1.0;
        const long pc = std::lround(12.0 * std::log2(tonicHz * ratio / 16.351597831287414));
        c[((pc % 12) + 12) % 12] += (k == 0 ? 1.5f : 1.0f);
    }
    double norm = 0.0;
    for (float v : c) norm += static_cast<double>(v) * v;
    norm = std::sqrt(norm);
    for (int j = 0; j < 12; ++j) scaleChroma_[j] = norm > 0.0 ? static_cast<float>(c[j] / norm) : 0.0f;
}

void Memory::layout(int lines)
{
    lines_ = lines;
    cap_ = (poolCells_ / lines_) / kSeg * kSeg;
    for (int k = 0; k < kMaxLines; ++k) {
        Line& ln = line_[k];
        ln.w = std::fmod(ln.w, static_cast<double>(cap_));
        if (ln.w < 0.0) ln.w += cap_;
        ln.base = static_cast<int>(ln.w);
        ln.acc0 = ln.wt0 = ln.acc1 = ln.wt1 = 0.0f;
        ln.seg = -1;
    }
    live_ = 0;            // a grain's place belongs to the old division
    sizeDone_ = -1.0f;
}

void Memory::lengths()
{
    // The longest line is the size, as far as its share of the pool allows: short of it by the
    // slide's reach and the heads' margins. The others step down from it geometrically to six
    // tenths, every length a prime number of cells, so no two lines ever meet in a common period.
    const double guard = 0.25 * sr_ + 4.0 * kSub;
    const double longest = std::clamp(static_cast<double>(size_) * sr_, 0.2 * sr_, static_cast<double>(cap_) - guard);
    for (int k = 0; k < kMaxLines; ++k) {
        Line& ln = line_[k];
        const double ratio = lines_ > 1 ? std::pow(0.6, static_cast<double>(std::min(k, lines_ - 1)) / (lines_ - 1)) : 1.0;
        const double target = std::max(0.05 * sr_, longest * ratio - apLen_[k]);
        ln.lenTarget = static_cast<double>(primeAtMost(static_cast<int>(target)));
        if (ln.lenBase <= 0.0) ln.lenBase = ln.lenPrev = ln.lenNext = ln.lenTarget;
    }
    sizeDone_ = size_;
    quietAfter_ = static_cast<long long>(2.0 * (longest + apLen_[0]) + sr_);
    gains();
}

void Memory::gains()
{
    const bool forever = hold_ >= 0.999f;
    const double t60 = 4.0 * std::pow(300.0, static_cast<double>(hold_));
    for (int k = 0; k < kMaxLines; ++k) {
        Line& ln = line_[k];
        const double pass = (ln.lenTarget + apLen_[k]) / sr_;   // seconds per pass at normal speed
        ln.gain = forever ? 1.0f : static_cast<float>(std::exp(-kLn1000 * pass / t60));
        // Age: what a pass costs at Nyquist -- two decibels per second of line at full Age -- as a
        // one-pole whose gain at DC is one, so the lows keep what Hold gives them.
        const double r = std::pow(10.0, 2.0 * static_cast<double>(age_) * pass / 20.0);
        ln.absA = static_cast<float>((r - 1.0) / (r + 1.0));
    }
    holdDone_ = hold_;
    ageDone_ = age_;
}

void Memory::resetLines()
{
    for (int k = 0; k < kMaxLines; ++k) {
        Line& ln = line_[k];
        ln.acc0 = ln.wt0 = ln.acc1 = ln.wt1 = 0.0f;
        ln.absZ = ln.dcX = ln.dcY = ln.env = 0.0f;
        ln.sat.reset();
        ln.aa.ic1 = ln.aa.ic2 = 0.0f;
    }
}

void Memory::stepControl(int m)
{
    const double dt = static_cast<double>(m) / sr_;
    const float fm = static_cast<float>(m);
    const float srf = static_cast<float>(sr_);

    // A new number of lines: the output fades, the pool is re-divided, the output comes back.
    if (linesWant_ != lines_) pendingLines_ = true;
    const bool mute = pendingLines_ || erase_;
    const float fade = fm / (0.06f * srf);
    outGainCur_ = mute ? std::max(0.0f, outGainCur_ - fade) : std::min(1.0f, outGainCur_ + fade);
    if (pendingLines_ && outGainCur_ <= 0.0f) {
        layout(linesWant_);
        lengths();
        for (int k = 0; k < kMaxLines; ++k) line_[k].lenBase = line_[k].lenPrev = line_[k].lenNext = line_[k].lenTarget;
        pendingLines_ = false;
    }
    // Erase: once the output is down, the pool is emptied a little at a time -- all of it at once
    // would be one very long block -- and everything the lines hold with it.
    if (erase_) {
        if (!cleared_ && outGainCur_ <= 0.0f) {
            if (clearPos_ < 0) clearPos_ = 0;
            const int end = std::min(poolCells_, clearPos_ + (1 << 17));
            std::fill(pool_.begin() + clearPos_, pool_.begin() + end, 0.0f);
            clearPos_ = end;
            if (clearPos_ >= poolCells_) {
                std::fill(ap_.begin(), ap_.end(), 0.0f);
                std::fill(segRms_.begin(), segRms_.end(), 0.0f);
                resetLines();
                live_ = 0;
                clearPos_ = -1;
                cleared_ = true;
            }
        }
    } else {
        cleared_ = false;
        clearPos_ = -1;
    }
    // The lengths only when the size has moved by more than half a millisecond: finding eight primes
    // near two million is a few thousand divisions each, which a modulated size must not ask for on
    // every block.
    if (std::fabs(size_ - sizeDone_) > 0.0005f) lengths();
    else if (hold_ != holdDone_ || age_ != ageDone_) gains();

    // The attractor that bends the slides and the pans: one of its time units to eight seconds.
    {
        const double h = dt / 8.0;
        const auto f = [](const double* s, double* d) {
            d[0] = 10.0 * (s[1] - s[0]);
            d[1] = s[0] * (28.0 - s[2]) - s[1];
            d[2] = s[0] * s[1] - (8.0 / 3.0) * s[2];
        };
        double k1[3], k2[3], k3[3], k4[3], t[3];
        f(lorenz_, k1);
        for (int i = 0; i < 3; ++i) t[i] = lorenz_[i] + 0.5 * h * k1[i];
        f(t, k2);
        for (int i = 0; i < 3; ++i) t[i] = lorenz_[i] + 0.5 * h * k2[i];
        f(t, k3);
        for (int i = 0; i < 3; ++i) t[i] = lorenz_[i] + h * k3[i];
        f(t, k4);
        for (int i = 0; i < 3; ++i) lorenz_[i] += h / 6.0 * (k1[i] + 2.0 * k2[i] + 2.0 * k3[i] + k4[i]);
        if (!std::isfinite(lorenz_[0] + lorenz_[1] + lorenz_[2])) { lorenz_[0] = 0.1; lorenz_[1] = 0.0; lorenz_[2] = 20.0; }
        chaos_[0] = clampv(static_cast<float>(lorenz_[0] / 20.0), -1.0f, 1.0f);
        chaos_[1] = clampv(static_cast<float>(lorenz_[1] / 27.0), -1.0f, 1.0f);
        chaos_[2] = clampv(static_cast<float>((lorenz_[2] - 25.0) / 22.0), -1.0f, 1.0f);
    }

    // The tape: a change of speed glides, a change of direction runs through a standstill. A full
    // turn from forwards to backwards takes three tenths of a second.
    const double vt = (reverse_ ? -1.0 : 1.0) * (half_ ? 0.5 : 1.0);
    const double step = static_cast<double>(m) / (0.15 * sr_);
    vPrev_ = v_;
    if (v_ != vt) {
        v_ += std::clamp(vt - v_, -step, step);
        gliding_ = true;
    }

    // The exchange: the matrix's angle and the scattering, glided; each of the butterfly's stages
    // leans a little off the common angle on its own slow sine, so the mix itself drifts.
    const float sm = 1.0f - std::exp(-fm / (0.08f * srf));
    angle_ += (blur_ * 0.78539816f - angle_) * sm;
    {
        const float x = clampv((blur_ - 0.45f) / 0.55f, 0.0f, 1.0f);
        apCoef_ += (0.6f * x * x * (3.0f - 2.0f * x) - apCoef_) * sm;
    }
    for (int s = 0; s < 3; ++s) {
        stagePh_[s] += kTau * kStageHz[s] * dt;
        if (stagePh_[s] > kTau) stagePh_[s] -= kTau;
        const float a = angle_ * (1.0f + 0.08f * drift_ * static_cast<float>(std::sin(stagePh_[s])));
        stageC_[s] = std::cos(a);
        stageS_[s] = std::sin(a);
    }
    recallCur_ += (recall_ - recallCur_) * sm;
    driveCur_ += (drive_ - driveCur_) * sm;
    const float inTarget = (freeze_ || erase_) ? 0.0f : 1.0f;
    const float inStep = fm / (0.02f * srf);
    inGainCur_ = inGainCur_ < inTarget ? std::min(inTarget, inGainCur_ + inStep) : std::max(inTarget, inGainCur_ - inStep);

    // The lines: their slides, their places in the field, and a slow tape's anti-alias filter.
    const double slope = std::pow(2.0, 15.0 * static_cast<double>(drift_) / 1200.0) - 1.0;   // cells of slide per cell, at most
    const float aaHz = std::min(static_cast<float>(0.45 * std::fabs(v_) * sr_), 0.45f * srf);
    for (int k = 0; k < lines_; ++k) {
        Line& ln = line_[k];
        const double bend = 1.0 + 0.35 * (std::cos(1.3 * k) * chaos_[0] + std::sin(1.3 * k) * chaos_[1]);
        ln.driftPh += kTau * kDriftHz[k] * bend * dt;
        if (ln.driftPh > kTau) ln.driftPh -= kTau;
        // The amplitude that keeps the slope of the slide under the bound even with the rate bent
        // up by a third, and never more than two fifths of the line.
        const double ampWant = std::min(slope * sr_ / (kTau * kDriftHz[k] * 1.35), 0.4 * ln.lenBase);
        ln.driftAmp += (static_cast<float>(ampWant) - ln.driftAmp) * std::min(1.0f, fm / (2.0f * srf));
        const double reach = 0.25 * m;   // a quarter of a cell a sample: a new size slides in, as on a tape delay
        ln.lenBase += std::clamp(ln.lenTarget - ln.lenBase, -reach, reach);
        ln.lenPrev = ln.lenNext;
        ln.lenNext = std::max(4.0 * kSub, ln.lenBase + static_cast<double>(ln.driftAmp) * std::sin(ln.driftPh));
        const double bendPan = 1.0 + 0.35 * (std::sin(0.7 * k) * chaos_[0] - std::cos(0.7 * k) * chaos_[2]);
        ln.panPh += kTau * kPanHz[k] * bendPan * dt;
        if (ln.panPh > kTau) ln.panPh -= kTau;
        const float pan = clampv(panBase(k, lines_) + 0.45f * drift_ * static_cast<float>(std::sin(ln.panPh)), -1.0f, 1.0f);
        const float th = (pan + 1.0f) * 0.78539816f;
        ln.glPrev = ln.glNext;
        ln.grPrev = ln.grNext;
        ln.glNext = std::cos(th);
        ln.grNext = std::sin(th);
        if (std::fabs(v_) < 0.999) ln.aa.setQ(aaHz, 0.7071f, srf);
    }
}

void Memory::tapeWrite(int k, float x, double v)
{
    // The write head spreads each sample over the two cells it falls between, weighted by where it
    // is, and keeps the running sums until it has left a cell for good. At full speed on a whole
    // cell that is exactly the sample; at half speed each cell is the average of the samples that
    // crossed it; at a standstill the sums just grow.
    Line& ln = line_[k];
    float* ring = pool_.data() + static_cast<size_t>(k) * static_cast<size_t>(cap_);
    const int fl = static_cast<int>(ln.w);
    if (fl != ln.base) {
        const int next = ln.base + 1 == cap_ ? 0 : ln.base + 1;
        const int prev = ln.base == 0 ? cap_ - 1 : ln.base - 1;
        if (fl == next) {                       // on by a cell: the one left behind is finished
            if (ln.wt0 > 0.0f) ring[ln.base] = ln.acc0 / ln.wt0;
            ln.acc0 = ln.acc1; ln.wt0 = ln.wt1;
            ln.acc1 = 0.0f; ln.wt1 = 0.0f;
        } else if (fl == prev) {                // back by a cell: the one above it is finished
            if (ln.wt1 > 0.0f) ring[next] = ln.acc1 / ln.wt1;
            ln.acc1 = ln.acc0; ln.wt1 = ln.wt0;
            ln.acc0 = 0.0f; ln.wt0 = 0.0f;
        } else {                                // moved by more: the heads were placed anew
            ln.acc0 = ln.wt0 = ln.acc1 = ln.wt1 = 0.0f;
        }
        ln.base = fl;
        const int seg = static_cast<int>((static_cast<long long>(k) * cap_ + fl) >> kSegBits);
        if (seg != ln.seg) {
            if (ln.seg >= 0) {                  // the stretch it has just finished is fingerprinted
                const int nt = (qTail_ + 1) & kQueueMask;
                if (nt != qHead_) { queue_[static_cast<size_t>(qTail_)] = ln.seg; qTail_ = nt; }
            }
            ln.seg = seg;
        }
    }
    const float f = static_cast<float>(ln.w - fl);
    ln.acc0 += (1.0f - f) * x; ln.wt0 += 1.0f - f;
    ln.acc1 += f * x;          ln.wt1 += f;
    ln.w += v;
    if (ln.w >= cap_) ln.w -= cap_;
    else if (ln.w < 0.0) ln.w += cap_;
}

void Memory::fingerprint(const float* x, float* chroma, float& rms)
{
    double sq = 0.0;
    for (int i = 0; i < kSeg; ++i) {
        fftRe_[static_cast<size_t>(i)] = x[i] * window_[static_cast<size_t>(i)];
        sq += static_cast<double>(x[i]) * x[i];
    }
    rms = static_cast<float>(std::sqrt(sq / kSeg));
    for (int j = 0; j < 12; ++j) chroma[j] = 0.0f;
    if (rms < 1.0e-6f) return;
    fft_.forward(fftRe_.data(), fftRe_.data(), fftIm_.data());
    for (int b = 1; b < kSeg / 2; ++b) {
        const int pc = binPc_[static_cast<size_t>(b)];
        if (pc < 0) continue;
        const float re = fftRe_[static_cast<size_t>(b)], im = fftIm_[static_cast<size_t>(b)];
        chroma[pc] += std::sqrt(re * re + im * im);
    }
    double norm = 0.0;
    for (int j = 0; j < 12; ++j) norm += static_cast<double>(chroma[j]) * chroma[j];
    norm = std::sqrt(norm);
    if (norm > 0.0) for (int j = 0; j < 12; ++j) chroma[j] = static_cast<float>(chroma[j] / norm);
}

void Memory::analyse()
{
    // One fingerprint per control block at most: the last seconds of input when a window of it is
    // full, otherwise the next stretch of tape a write head has finished.
    float c[12];
    float rms = 0.0f;
    if (inPos_ >= kSeg) {
        fingerprint(inBuf_.data(), c, rms);
        inPos_ = 0;
        const float wgt = rms > 1.0e-4f ? 0.07f : 0.0f;   // about four seconds of memory
        for (int j = 0; j < 12; ++j) inputChroma_[j] += wgt * (c[j] - inputChroma_[j]);
        inputLevel_ += 0.07f * (rms - inputLevel_);
        return;
    }
    if (qHead_ == qTail_) return;
    const int seg = queue_[static_cast<size_t>(qHead_)];
    qHead_ = (qHead_ + 1) & kQueueMask;
    const long long start = static_cast<long long>(seg) << kSegBits;
    if (start + kSeg > poolCells_) return;
    fingerprint(pool_.data() + start, c, rms);
    std::memcpy(chroma_.data() + static_cast<size_t>(seg) * 12, c, sizeof(c));
    segRms_[static_cast<size_t>(seg)] = rms;
}

void Memory::spawnGrain(int offset)
{
    if (live_ >= kMaxGrains || cap_ <= 0) return;
    const int glen = std::max(64, static_cast<int>(static_cast<double>(grainMs_) * (0.8 + 0.4 * static_cast<double>(rng_.uniform())) * sr_ / 1000.0));
    // A grain reads forwards at the speed it was recorded, and it must not meet a write head while
    // it plays: it starts far enough behind one that the head, moving at the tape's speed, cannot
    // reach it, and far enough ahead of it the other way round the ring.
    const double margin = 2.0 * kSub + 16.0;
    const double behind = std::max(0.0, 1.0 - v_) * glen + margin;
    const double room = static_cast<double>(cap_) - behind - margin - glen;
    if (room < kSeg) return;
    // What the grains listen for: the scale, and as much of the last seconds of input as there was.
    float target[12];
    {
        const float inW = clampv(inputLevel_ / 0.02f, 0.0f, 1.0f);
        double norm = 0.0;
        for (int j = 0; j < 12; ++j) { target[j] = scaleChroma_[j] + inW * inputChroma_[j]; norm += static_cast<double>(target[j]) * target[j]; }
        norm = std::sqrt(norm);
        for (int j = 0; j < 12; ++j) target[j] = norm > 0.0 ? static_cast<float>(target[j] / norm) : 0.0f;
    }
    // A dozen places drawn at random; Seek decides how much their harmony counts against chance.
    int bestK = -1;
    double bestPos = 0.0;
    float bestScore = -1.0e9f;
    for (int c = 0; c < 12; ++c) {
        const int k = rng_.below(lines_);
        const double delta = behind + static_cast<double>(rng_.uniform()) * room;
        double pos = line_[k].w - delta;
        while (pos < 0.0) pos += cap_;
        const int seg = static_cast<int>((static_cast<long long>(k) * cap_ + static_cast<long long>(pos)) >> kSegBits);
        const float u = rng_.uniform();
        float score = -1.0f + 0.01f * u;         // silence, which is taken only if there is nothing else
        if (segRms_[static_cast<size_t>(seg)] >= 1.0e-4f) {
            const float* ch = chroma_.data() + static_cast<size_t>(seg) * 12;
            float dot = 0.0f;
            for (int j = 0; j < 12; ++j) dot += ch[j] * target[j];
            score = seek_ * dot + (1.0f - seek_) * u;
        }
        if (score > bestScore) { bestScore = score; bestK = k; bestPos = pos; }
    }
    if (bestK < 0 || bestScore < 0.0f) return;   // an empty memory has nothing to recall
    RingGrain& g = grains_[live_++];
    g.pos = bestPos;
    g.rate = 1.0;
    g.len = glen;
    g.age = 0;
    g.start = offset;
    g.ring = bestK;
    g.bus = 0;
    const float side = (bestK & 1) ? 1.0f : -1.0f;
    const float pan = clampv(0.45f * side + 0.35f * rng_.bipolar(), -1.0f, 1.0f);
    const float th = (pan + 1.0f) * 0.78539816f;
    g.gl = std::cos(th);
    g.gr = std::sin(th);
    g.wc = 1.0f; g.ws = 0.0f;
    phasorFrom(1.0 / glen, g.rc, g.rs);
    ++started_;
}

void Memory::process(float* L, float* R, int n)
{
    float peakIn = 0.0f, peakOut = 0.0f;
    for (int p = 0; p < n; ) {
        const int m = std::min(kSub, n - p);
        stepControl(m);
        const int N = lines_;

        // Recall: the grains are rendered before the lines write this block. They read no closer
        // than two blocks to a write head, so nothing they read changes under them.
        float gL[kSub] = {}, gR[kSub] = {};
        const bool recallOn = (recall_ > 0.0f || recallCur_ > 1.0e-4f) && !pendingLines_ && !erase_;
        if (recallOn) {
            const double density = 4000.0 / static_cast<double>(grainMs_);   // an overlap of four
            for (int i = 0; i < m; ++i) {
                hazard_ += density / sr_;
                if (hazard_ >= nextHazard_) {
                    hazard_ = 0.0;
                    nextHazard_ = -std::log(1.0 - static_cast<double>(rng_.uniform()) + 1.0e-12);
                    spawnGrain(i);
                }
            }
            for (int c = 0; c < live_; ) {
                RingGrain& g = grains_[c];
                {
                    const float r2 = g.wc * g.wc + g.ws * g.ws, fix = 1.5f - 0.5f * r2;
                    g.wc *= fix; g.ws *= fix;
                }
                const int begin = g.start < m ? g.start : m;
                g.start = 0;
                const int count = std::min(m - begin, g.len - g.age);
                if (count > 0 && g.ring < N) {
                    renderRingGrain(pool_.data() + static_cast<size_t>(g.ring) * static_cast<size_t>(cap_), cap_, 1, g, gL + begin, gR + begin, count);
                    g.age += count;
                }
                if (g.age >= g.len || g.ring >= N) grains_[c] = grains_[--live_];
                else ++c;
            }
        } else {
            live_ = 0;
        }
        if (qHead_ != qTail_ || inPos_ >= kSeg) analyse();

        // The lines, sample by sample.
        const float inv = 1.0f / static_cast<float>(m);
        const double v0 = vPrev_, v1 = v_;
        const bool slow = std::fabs(v0) < 0.999 || std::fabs(v1) < 0.999;
        const float inGain = inGainCur_ * std::sqrt(2.0f / static_cast<float>(N));
        const float apc = apCoef_;
        const float drv = driveCur_;
        const float rc = recallCur_;
        const float outG = outGainCur_;
        const float grainGain = 0.815f * std::sqrt(0.5f * static_cast<float>(N));
        for (int i = 0; i < m; ++i) {
            const float a = static_cast<float>(i + 1) * inv;
            const double v = v0 + (v1 - v0) * static_cast<double>(a);
            const float xl = L[p + i], xr = R[p + i];
            const float lev = std::max(std::fabs(xl), std::fabs(xr));
            peakIn = std::max(peakIn, lev);
            inEnv_ += (lev > inEnv_ ? envAtk_ : envRel_) * (lev - inEnv_);
            const float keep = freeze_ ? 1.0f : 1.0f - renew_ * clampv(inEnv_ / 0.08f, 0.0f, 1.0f);
            float y[kMaxLines];
            float outL = 0.0f, outR = 0.0f;
            for (int k = 0; k < N; ++k) {
                Line& ln = line_[k];
                const float* ring = pool_.data() + static_cast<size_t>(k) * static_cast<size_t>(cap_);
                const double len = ln.lenPrev + (ln.lenNext - ln.lenPrev) * static_cast<double>(a);
                double r = ln.w - len;
                if (r < 0.0) r += cap_;
                const int i1 = static_cast<int>(r);
                const float pf = static_cast<float>(r - i1) * static_cast<float>(kPhases);
                const int ph = std::min(static_cast<int>(pf), kPhases - 1);
                const float pw = pf - static_cast<float>(ph);
                const float* h0 = sinc_.data() + static_cast<size_t>(ph * kTaps);
                const float* h1 = h0 + kTaps;
                float s = 0.0f;
                int c = i1 - 3;
                if (c >= 0 && c + kTaps <= cap_) {
                    for (int j = 0; j < kTaps; ++j) s += ring[c + j] * (h0[j] + pw * (h1[j] - h0[j]));
                } else {
                    if (c < 0) c += cap_;
                    for (int j = 0; j < kTaps; ++j) {
                        s += ring[c] * (h0[j] + pw * (h1[j] - h0[j]));
                        if (++c == cap_) c = 0;
                    }
                }
                outL += s * (ln.glPrev + (ln.glNext - ln.glPrev) * a);
                outR += s * (ln.grPrev + (ln.grNext - ln.grPrev) * a);
                // In the loop: the scattering all-pass, the absorption, a DC blocker.
                float* ab = ap_.data() + static_cast<size_t>(k) * static_cast<size_t>(apSize_);
                const float d = ab[(apW_ - apLen_[k]) & apMask_];
                const float sy = d - apc * s;
                ab[apW_ & apMask_] = s + apc * sy;
                const float absA = freeze_ ? 0.0f : ln.absA;
                ln.absZ = sy + absA * (ln.absZ - sy);
                // Scaled by (1 + R) / 2, so its gain is one at Nyquist and under one everywhere else:
                // unscaled it lifted the highs by a thousandth of a decibel a pass, and a memory held
                // for ever is nothing but passes.
                const float hp = 0.5f * (1.0f + dcR_) * (ln.absZ - ln.dcX) + dcR_ * ln.dcY;
                ln.dcX = ln.absZ;
                ln.dcY = std::fabs(hp) < 1.0e-20f ? 0.0f : hp;
                y[k] = ln.dcY;
            }
            apW_ = (apW_ + 1) & apMask_;
            // The exchange: every stage turns pairs of lines a span apart, the spans doubling.
            {
                int stage = 0;
                for (int span = 1; span < N; span <<= 1, ++stage) {
                    const float cs = stageC_[stage], sn = stageS_[stage];
                    for (int b = 0; b < N; b += 2 * span)
                        for (int j = b; j < b + span; ++j) {
                            const float u = y[j], w = y[j + span];
                            y[j] = cs * u - sn * w;
                            y[j + span] = sn * u + cs * w;
                        }
                }
            }
            for (int k = 0; k < N; ++k) {
                Line& ln = line_[k];
                float fb = y[k] * (freeze_ ? 1.0f : ln.gain * keep);
                const float mag = std::fabs(fb);
                ln.env += (mag > ln.env ? limAtk_ : limRel_) * (mag - ln.env);
                if (ln.env > kLimit) fb *= kLimit / ln.env;
                if (drv > 1.0e-4f && !freeze_) {
                    const float gd = 1.0f + 5.0f * drv;
                    const float sat = ln.sat(fb * gd) / gd;
                    fb += std::min(1.0f, 20.0f * drv) * (sat - fb);
                }
                float x = erase_ ? 0.0f : fb + inGain * ((k & 1) ? xr : xl);
                if (slow) {
                    float lp, bp, hp;
                    ln.aa.tick(x, lp, bp, hp);
                    x = lp;
                }
                tapeWrite(k, x, v);
            }
            const float oL = (outL + (gL[i] * grainGain - outL) * rc) * outG;
            const float oR = (outR + (gR[i] * grainGain - outR) * rc) * outG;
            L[p + i] = oL;
            R[p + i] = oR;
            peakOut = std::max(peakOut, std::max(std::fabs(oL), std::fabs(oR)));
            if (inPos_ < kSeg) inBuf_[static_cast<size_t>(inPos_++)] = 0.5f * (xl + xr);
        }
        // A glide that has arrived at full speed puts the heads back on whole cells, where a splat
        // is exact: half a cell of offset would low-pass every pass, and passes add up.
        if (gliding_ && v_ == (reverse_ ? -1.0 : 1.0) * (half_ ? 0.5 : 1.0)) {
            gliding_ = false;
            if (std::fabs(v_) == 1.0)
                for (int k = 0; k < N; ++k) {
                    double w = std::round(line_[k].w);
                    if (w >= cap_) w -= cap_;
                    line_[k].w = w;
                }
        }
        p += m;
    }
    quietFor_ = (peakIn < 1.0e-6f && peakOut < 1.0e-6f) ? quietFor_ + n : 0;
}

double Memory::energy() const
{
    double e = 0.0;
    for (int k = 0; k < lines_; ++k) {
        const float* ring = pool_.data() + static_cast<size_t>(k) * static_cast<size_t>(cap_);
        const int len = static_cast<int>(line_[k].lenTarget);
        int c = static_cast<int>(std::floor(line_[k].w)) - len;
        while (c < 0) c += cap_;
        for (int j = 0; j < len; ++j) {
            e += static_cast<double>(ring[c]) * ring[c];
            if (++c == cap_) c = 0;
        }
        const float* ab = ap_.data() + static_cast<size_t>(k) * static_cast<size_t>(apSize_);
        for (int j = 1; j <= apLen_[k]; ++j) {
            const float v = ab[(apW_ - j) & apMask_];
            e += static_cast<double>(v) * v;
        }
    }
    return e;
}

} // namespace ambient
