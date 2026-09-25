/**
 * @file Loudness.cpp
 * @brief The loudness meter: K-weighting, BS.1770 gating, Zwicker's sones and the true peak.
 *
 * Four things live here, in the order the header declares them. KFilter::prepare() derives the two
 * K-weighting stages from the analogue prototypes BS.1770 names, so they hold at any sample rate
 * rather than only at 48 kHz. ZwickerLoudness turns a Hann-windowed FFT of the mid signal into
 * third-octave levels, spreads them along the Bark scale into an excitation pattern, applies
 * Zwicker's compressive law and integrates -- the ISO 532-1 chain -- and pins the result to the
 * definition of the sone. LoudnessMeter drives both, keeps the 400 ms blocks and the 3 s short-term
 * values that the gating and the range are computed from, and tracks the inter-sample peak. The
 * functions in the two anonymous namespaces are the small pieces of arithmetic those three share:
 * the Bark scale both ways, the mean-square-to-LUFS formula and the four-point interpolator of the
 * true peak.
 *
 * Everything but read() runs on the audio thread and allocates nothing after prepare(); read() runs
 * on whoever asks and works from snapshots of the two logs (LoudnessLog, Loudness.h). The Fft comes
 * from Cosmos.h, which is what its include is for.
 *
 * kPi arrives from Dsp.h by way of Cosmos.h. A second one in an anonymous namespace here is not
 * private to this file, it is ambiguous with that one, and the compiler says so.
 */
#include "ambient/Loudness.h"
#include "ambient/Cosmos.h"   // Fft
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ambient {

void KFilter::prepare(double sr)
{
    // Not the RBJ cookbook. The cookbook's shelf, given the standard's own f0, Q and gain, comes
    // out about two per cent away from the coefficients BS.1770 prints for 48 kHz -- close enough
    // to look right and wrong enough to be wrong. This is the formulation that reproduces them:
    // checked against the published numbers to sixteen digits, and it holds at any sample rate
    // because it is the analogue prototype rather than a copy of one table.
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan(kPi * f0 / sr);
        const double Vh = std::pow(10.0, G / 20.0);
        const double Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        shelf_.b0 = static_cast<float>((Vh + Vb * K / Q + K * K) / a0);
        shelf_.b1 = static_cast<float>(2.0 * (K * K - Vh) / a0);
        shelf_.b2 = static_cast<float>((Vh - Vb * K / Q + K * K) / a0);
        shelf_.a1 = static_cast<float>(2.0 * (K * K - 1.0) / a0);
        shelf_.a2 = static_cast<float>((1.0 - K / Q + K * K) / a0);
    }
    {   // Stage two: the RLB high-pass. Its numerator is exactly 1, -2, 1 -- the standard does not
        // normalise it the way the cookbook would, and that difference is 0.04 dB of gain.
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan(kPi * f0 / sr);
        const double d = 1.0 + K / Q + K * K;
        hp_.b0 = 1.0f; hp_.b1 = -2.0f; hp_.b2 = 1.0f;
        hp_.a1 = static_cast<float>(2.0 * (K * K - 1.0) / d);
        hp_.a2 = static_cast<float>((1.0 - K / Q + K * K) / d);
    }
    reset();
}

void KFilter::reset() { shelf_.reset(); hp_.reset(); }

float KFilter::process(float x) { return hp_.process(shelf_.process(x)); }

// ---------------------------------------------------------------- meter

// ---------------------------------------------------------------- loudness in sones

namespace {

/** @brief The centre frequencies of the third octaves, 25 Hz to 12.5 kHz. */
const float kThirdOctave[ZwickerLoudness::kBands] = {
    25.0f, 31.5f, 40.0f, 50.0f, 63.0f, 80.0f, 100.0f, 125.0f, 160.0f, 200.0f,
    250.0f, 315.0f, 400.0f, 500.0f, 630.0f, 800.0f, 1000.0f, 1250.0f, 1600.0f, 2000.0f,
    2500.0f, 3150.0f, 4000.0f, 5000.0f, 6300.0f, 8000.0f, 10000.0f, 12500.0f,
};

/**
 * @brief Frequency to critical-band rate: Zwicker and Terhardt's analytic fit to the Bark scale.
 * @param hz  frequency in Hz
 * @return    the critical-band rate in Bark, 0 at 0 Hz and about 24 at the top of hearing
 */
inline float barkOf(float hz)
{
    const float k = hz * 0.001f;
    return 13.0f * std::atan(0.76f * k) + 3.5f * std::atan((k / 7.5f) * (k / 7.5f));
}

/**
 * @brief The inverse of barkOf(): the frequency at a critical-band rate.
 *
 * Asked once per 0.1 Bark step of the excitation pattern in rawLoudness(), which is why it can
 * afford the forty bisection steps in its body.
 * @param z  critical-band rate in Bark
 * @return   frequency in Hz, bracketed between 10 Hz and 20 kHz
 */
inline float hzOfBark(float z)
{
    // Inverted by bisection, which is exact enough and needs no second fit that could disagree
    // with the first one.
    float lo = 10.0f, hi = 20000.0f;
    for (int i = 0; i < 40; ++i) {
        const float mid = 0.5f * (lo + hi);
        if (barkOf(mid) < z) lo = mid; else hi = mid;
    }
    return 0.5f * (lo + hi);
}

} // namespace

/**
 * Terhardt's approximation of the absolute threshold of hearing, in dB SPL. Computed rather than
 * tabulated: the standard's table lists the twenty critical bands, and this gives the same curve
 * at whatever centre frequency it is asked about.
 */
float ZwickerLoudness::thresholdInQuiet(float hz)
{
    const float k = std::max(0.02f, hz * 0.001f);
    const float d = k - 3.3f;
    float t = 3.64f * std::pow(k, -0.8f) - 6.5f * std::exp(-0.6f * d * d) + 0.001f * k * k * k * k;
    return std::min(80.0f, std::max(-5.0f, t));
}

float ZwickerLoudness::rawLoudness(const float* levelsDb)
{
    // The excitation pattern: every band's energy spread along the critical-band rate. Towards
    // lower frequencies the slope is steep and fixed; towards higher ones it gets shallower the
    // louder the band is, which is the asymmetry of masking and the reason a loud low note covers
    // so much of what is above it.
    double e[kSteps] = {};
    for (int b = 0; b < kBands; ++b) {
        const float lvl = levelsDb[b];
        if (lvl < -20.0f) continue;
        // The band is not a line: a third octave at a kilohertz is a third of a Bark wide, and its
        // energy sits across that width before any slope begins.
        const float zLo = barkOf(kThirdOctave[b] * 0.8909f);
        const float zHi = barkOf(kThirdOctave[b] * 1.1225f);
        // Terhardt's slopes. The 230 over f is in HERTZ, and putting kilohertz there instead makes
        // the upper slope 246 dB per Bark at a kilohertz -- the masking pattern then has no upper
        // side at all, and the whole scale came out four times too quiet while every ratio in it
        // stayed right. A model can be wrong by a constant and still pass every relative test.
        const float upper = std::max(2.0f, 22.0f + std::min(10.0f, 230.0f / std::max(20.0f, kThirdOctave[b])) - 0.2f * lvl);
        const float lower = 27.0f;
        const double intensity = std::pow(10.0, 0.1 * static_cast<double>(lvl));
        for (int i = 0; i < kSteps; ++i) {
            const float z = 0.1f * static_cast<float>(i);
            const float d = (z < zLo) ? z - zLo : (z > zHi ? z - zHi : 0.0f);
            const float drop = (d < 0.0f) ? lower * (-d) : upper * d;
            if (drop > 60.0f) continue;                 // a millionth of the band: not worth adding
            e[i] += intensity * std::pow(10.0, -0.1 * static_cast<double>(drop));
        }
    }
    // Zwicker's compressive law, band by band, and then the integral over the whole scale. The
    // 0.08 and the 0.23 are his; what they encode is that loudness doubles for every ten decibels
    // well above threshold and falls away far faster close to it.
    double total = 0.0;
    for (int i = 0; i < kSteps; ++i) {
        const float z = 0.1f * static_cast<float>(i);
        const float ltq = thresholdInQuiet(hzOfBark(z));
        const double etq = std::pow(10.0, 0.1 * static_cast<double>(ltq));
        const double ratio = e[i] / etq;
        if (ratio <= 1.0) continue;                     // under the threshold in quiet: not heard
        const double n = 0.08 * std::pow(etq, 0.23) * (std::pow(0.5 + 0.5 * ratio, 0.23) - 1.0);
        if (n > 0.0) total += n * 0.1;                  // 0.1 Bark per step
    }
    return static_cast<float>(total);
}

/**
 * The scale, pinned where the unit itself is defined: one sone IS a one-kilohertz tone at 40 dB
 * SPL, heard from the front in a free field. The model above reproduces every RELATIVE property
 * of loudness -- the doubling every ten decibels, the threshold, the growth with bandwidth -- and
 * then sits about a third above that anchor, because it works from third-octave levels where
 * Zwicker's own method works from grouped critical bands and adds a free-field correction.
 *
 * So the anchor is applied, and it is computed rather than written down: the model is asked what
 * it makes of that tone, once, and everything it says afterwards is divided by the answer. If the
 * model is ever changed the constant follows it, and the value that has to be checked against the
 * standard is not this one but the next one along -- 60 dB, which should come out at four sones
 * and does, to within a few per cent, with nothing pinning it there.
 */
float ZwickerLoudness::fromBandLevels(const float* levelsDb)
{
    static const float k = [] {
        float bands[kBands];
        for (int b = 0; b < kBands; ++b) bands[b] = -100.0f;
        bands[16] = 40.0f;                              // the 1 kHz third octave
        const float raw = rawLoudness(bands);
        return raw > 1.0e-6f ? 1.0f / raw : 1.0f;
    }();
    return rawLoudness(levelsDb) * k;
}

void ZwickerLoudness::prepare(double sampleRate)
{
    sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    ring_.assign(static_cast<size_t>(kN), 0.0f);
    re_.assign(static_cast<size_t>(kN), 0.0f);
    im_.assign(static_cast<size_t>(kN), 0.0f);
    window_.assign(static_cast<size_t>(kN), 0.0f);
    for (int i = 0; i < kN; ++i)
        window_[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(static_cast<float>(2.0 * kPi * i / kN));
    // Which bins belong to which third octave, once.
    const double binHz = sr_ / kN;
    for (int b = 0; b < kBands; ++b) {
        const double lo = kThirdOctave[b] * 0.8909, hi = kThirdOctave[b] * 1.1225;   // 2^(-1/6), 2^(1/6)
        binLo_[b] = std::max(1, static_cast<int>(lo / binHz + 0.5));
        binHi_[b] = std::min(kN / 2 - 1, std::max(binLo_[b], static_cast<int>(hi / binHz + 0.5)));
    }
    history_.prepare(kHistory);
    reset();
}

void ZwickerLoudness::reset()
{
    std::fill(ring_.begin(), ring_.end(), 0.0f);
    pos_ = 0; filled_ = 0;
    now_ = 0.0f; max_ = 0.0f;
    history_.clear();
}

void ZwickerLoudness::process(const float* L, const float* R, int n)
{
    if (ring_.empty()) return;
    for (int i = 0; i < n; ++i) {
        // The two channels as one: loudness is what reaches the listener, and two ears hearing the
        // same bed do not hear it twice.
        ring_[static_cast<size_t>(pos_)] = 0.5f * (L[i] + R[i]);
        pos_ = (pos_ + 1) & (kN - 1);
        if (++filled_ >= kHop) { filled_ = 0; frame(); }
    }
}

void ZwickerLoudness::frame()
{
    for (int i = 0; i < kN; ++i) {
        re_[static_cast<size_t>(i)] = ring_[static_cast<size_t>((pos_ + i) & (kN - 1))] * window_[static_cast<size_t>(i)];
        im_[static_cast<size_t>(i)] = 0.0f;
    }
    static const Fft fft(kN);
    fft.transform(re_.data(), im_.data(), false);
    // A Hann window of this length loses half the amplitude and spreads a sinusoid over three
    // bins; both are taken out here, so a full-scale sine reads as full scale.
    const double norm = 2.0 / kN * 2.0;
    float levels[kBands];
    for (int b = 0; b < kBands; ++b) {
        double p = 0.0;
        for (int k = binLo_[b]; k <= binHi_[b]; ++k) {
            const double v = (re_[static_cast<size_t>(k)] * re_[static_cast<size_t>(k)]
                            + im_[static_cast<size_t>(k)] * im_[static_cast<size_t>(k)]) * norm * norm;
            p += v;
        }
        // Amplitude to a level, then to dB SPL against the stated full scale. The 0.5 is because a
        // sinusoid of amplitude one has a mean square of a half, and full scale means the sine.
        levels[b] = 10.0f * std::log10(static_cast<float>(p * 0.5) + 1.0e-20f) + kFullScaleSpl;
    }
    now_ = fromBandLevels(levels);
    if (now_ > max_) max_ = now_;
    history_.push(now_);
}

float ZwickerLoudness::sonesN5() const
{
    if (history_.empty()) return 0.0f;
    std::vector<float> v = history_.snapshot();
    const size_t k = v.size() - 1 - static_cast<size_t>(0.05 * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<long>(k), v.end());
    return v[k];
}

void LoudnessMeter::prepare(double sampleRate)
{
    sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    hopLen_   = std::max(1, static_cast<int>(sr_ * 0.1));
    blockLen_ = hopLen_ * kHopsPerBlock;
    kL_.prepare(sr_);
    kR_.prepare(sr_);
    zwicker_.prepare(sr_);
    sumL_.assign(static_cast<size_t>(kHopsPerShort), 0.0);
    sumR_.assign(static_cast<size_t>(kHopsPerShort), 0.0);
    blocks_.prepare(kBlockLog);
    shortBlocks_.prepare(kBlockLog);
    reset();
}

void LoudnessMeter::reset()
{
    kL_.reset(); kR_.reset();
    zwicker_.reset();
    std::fill(sumL_.begin(), sumL_.end(), 0.0);
    std::fill(sumR_.begin(), sumR_.end(), 0.0);
    ringPos_ = 0; ringFilled_ = 0; hopPos_ = 0;
    hopL_ = hopR_ = 0.0; hopSamples_ = 0;
    blocks_.clear(); shortBlocks_.clear();
    truePeak_ = 0.0; seconds_ = 0.0; lastShort_ = -120.0f;
    for (int i = 0; i < 4; ++i) { tpHistL_[i] = 0.0f; tpHistR_[i] = 0.0f; }
}

namespace {
/**
 * @brief Loudness of a mean-square pair, the standard's formula with both channel weights at one.
 * @param msL  mean square of the K-weighted left channel over the block
 * @param msR  the same for the right channel
 * @return     the loudness in LUFS, or -120 when both are as good as zero
 */
inline float lufs(double msL, double msR)
{
    const double s = msL + msR;
    return s > 1.0e-12 ? static_cast<float>(-0.691 + 10.0 * std::log10(s)) : -120.0f;
}

/**
 * @brief Four-phase interpolation for the inter-sample peak.
 *
 * Not the standard's 48-tap filter -- a
 * four-point Lagrange, which is honest about being an estimate and catches the overshoot a
 * resampler would produce. It reads within a couple of tenths of a decibel of the long filter on
 * material like this, and it is the difference between "0.0 dBFS, fine" and "this will clip in an
 * mp3 decoder" that matters here.
 * @param h  the last four samples of one channel, oldest first; the interval looked at is the one
 *           between h[1] and h[2]
 * @return   the largest magnitude among h[1] and the three points interpolated after it
 */
inline double interPeak(const float* h)
{
    double peak = std::fabs(static_cast<double>(h[1]));
    for (int k = 1; k < 4; ++k) {
        const double t = k * 0.25;
        const double a = h[0], b = h[1], c = h[2], d = h[3];
        const double v = b + 0.5 * t * (c - a + t * (2.0 * a - 5.0 * b + 4.0 * c - d + t * (3.0 * (b - c) + d - a)));
        peak = std::max(peak, std::fabs(v));
    }
    return peak;
}
}

void LoudnessMeter::pushBlock()
{
    // One hop finished: it joins the ring, and the last four hops make a 400 ms block.
    sumL_[static_cast<size_t>(ringPos_)] = hopSamples_ > 0 ? hopL_ / static_cast<double>(hopSamples_) : 0.0;
    sumR_[static_cast<size_t>(ringPos_)] = hopSamples_ > 0 ? hopR_ / static_cast<double>(hopSamples_) : 0.0;
    ringPos_ = (ringPos_ + 1) % kHopsPerShort;
    if (ringFilled_ < kHopsPerShort) ++ringFilled_;
    hopL_ = hopR_ = 0.0; hopSamples_ = 0;

    auto meanOver = [this](int hops, double& l, double& r) {
        const int n = std::min(hops, ringFilled_);
        l = r = 0.0;
        for (int k = 1; k <= n; ++k) {
            const int idx = (ringPos_ - k + kHopsPerShort) % kHopsPerShort;
            l += sumL_[static_cast<size_t>(idx)];
            r += sumR_[static_cast<size_t>(idx)];
        }
        if (n > 0) { l /= n; r /= n; }
        return n;
    };

    double bl = 0.0, br = 0.0;
    if (meanOver(kHopsPerBlock, bl, br) == kHopsPerBlock) blocks_.push(lufs(bl, br));
    double sl = 0.0, sr = 0.0;
    if (meanOver(kHopsPerShort, sl, sr) == kHopsPerShort) {
        lastShort_ = lufs(sl, sr);
        shortBlocks_.push(lastShort_);
    }
}

void LoudnessMeter::process(const float* L, const float* R, int n)
{
    zwicker_.process(L, R, n);
    for (int i = 0; i < n; ++i) {
        const float l = L[i], r = R[i];
        // True peak first, on the unweighted signal: it is a peak, not a loudness.
        tpHistL_[0] = tpHistL_[1]; tpHistL_[1] = tpHistL_[2]; tpHistL_[2] = tpHistL_[3]; tpHistL_[3] = l;
        tpHistR_[0] = tpHistR_[1]; tpHistR_[1] = tpHistR_[2]; tpHistR_[2] = tpHistR_[3]; tpHistR_[3] = r;
        truePeak_ = std::max(truePeak_, std::max(interPeak(tpHistL_), interPeak(tpHistR_)));

        const float kl = kL_.process(l), kr = kR_.process(r);
        hopL_ += static_cast<double>(kl) * kl;
        hopR_ += static_cast<double>(kr) * kr;
        ++hopSamples_;
        if (++hopPos_ >= hopLen_) { hopPos_ = 0; pushBlock(); }
    }
    seconds_ += static_cast<double>(n) / sr_;
}

LoudnessReading LoudnessMeter::read() const
{
    LoudnessReading out;
    out.seconds = static_cast<float>(seconds_);
    out.shortTerm = lastShort_;
    out.sones = zwicker_.sones();
    out.sonesMax = zwicker_.sonesMax();
    out.sonesN5 = zwicker_.sonesN5();

    {   // Momentary: the last four hops, which is the last 400 ms.
        double l = 0.0, r = 0.0;
        const int n = std::min(kHopsPerBlock, ringFilled_);
        for (int k = 1; k <= n; ++k) {
            const int idx = (ringPos_ - k + kHopsPerShort) % kHopsPerShort;
            l += sumL_[static_cast<size_t>(idx)];
            r += sumR_[static_cast<size_t>(idx)];
        }
        if (n > 0) out.momentary = lufs(l / n, r / n);
    }

    // Integrated, gated twice: everything below -70 LUFS is not programme at all, and then
    // everything more than 10 LU under the mean of what is left is a gap between the loud parts.
    // Without the second gate a piece that is mostly silence measures as mostly silence.
    if (!blocks_.empty()) {
        const std::vector<float> bl = blocks_.snapshot();
        double sum = 0.0; int count = 0;
        for (float b : bl) if (b > -70.0f) { sum += std::pow(10.0, (b + 0.691) / 10.0); ++count; }
        if (count > 0) {
            const float absMean = static_cast<float>(-0.691 + 10.0 * std::log10(sum / count));
            const float gate = absMean - 10.0f;
            double s2 = 0.0; int c2 = 0;
            for (float b : bl) if (b > -70.0f && b > gate) { s2 += std::pow(10.0, (b + 0.691) / 10.0); ++c2; }
            if (c2 > 0) out.integrated = static_cast<float>(-0.691 + 10.0 * std::log10(s2 / c2));
        }
    }

    // Loudness range: the spread of the short-term values between the 10th and 95th centile, over
    // those above the usual relative gate. It is the number that says whether a piece still has
    // its dynamics -- ambient wants it wide, and a limiter is what makes it narrow.
    if (shortBlocks_.size() >= 10) {
        const std::vector<float> sb = shortBlocks_.snapshot();
        std::vector<float> v;
        v.reserve(sb.size());
        double sum = 0.0; int count = 0;
        for (float b : sb) if (b > -70.0f) { sum += std::pow(10.0, (b + 0.691) / 10.0); ++count; }
        if (count > 0) {
            const float gate = static_cast<float>(-0.691 + 10.0 * std::log10(sum / count)) - 20.0f;
            for (float b : sb) if (b > gate) v.push_back(b);
            if (v.size() >= 10) {
                std::sort(v.begin(), v.end());
                const size_t lo = static_cast<size_t>(0.10 * (v.size() - 1));
                const size_t hi = static_cast<size_t>(0.95 * (v.size() - 1));
                out.range = v[hi] - v[lo];
            }
        }
    }

    out.truePeak = truePeak_ > 1.0e-9 ? static_cast<float>(20.0 * std::log10(truePeak_)) : -120.0f;
    // Crest: how far the loudest instant stands above the loudness of the last three seconds.
    // Under a limiter this collapses; it is the single number the ambient literature asks for.
    if (out.shortTerm > -119.0f && out.truePeak > -119.0f) out.crest = out.truePeak - out.shortTerm;
    return out;
}

}   // namespace ambient
