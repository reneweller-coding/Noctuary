/**
 * @file Effects.cpp
 * @brief The stereo effects' inner loops, one section per class.
 *
 * Effects.h declares the classes and says what each is for; this is where they run. What the reader
 * finds only here: the three Ensemble modes' construction (the chorus taps, the microshifter's long
 * ramp and short hand-over, the velvet-noise sequence and how it is drawn), the delay's absorption
 * band and the ducking of its loop's brightness, the diffuser's prime-ratio stages, the Unmask's
 * three-band split and upward spread of masking, the Patina's wow, hiss and saturation with the
 * double-precision read head that cured a six-minute buzz, the reverb's line lengths, scattering
 * all-passes and turning matrix, and the MidSide's tilt and mono guard with the measurements behind
 * their thresholds. HaasBand and EarlyRoom are inline in the header and have nothing here.
 * Everything but prepare() runs on the audio thread and allocates nothing.
 */
#include "ambient/Effects.h"
#include <cmath>
#include <algorithm>

namespace ambient {

namespace {
/**
 * @brief The smallest power of two that is at least @p n: the ring sizes, so a mask can wrap them.
 * @param n  the number of samples a ring has to hold
 * @return   the first power of two >= n (1 for n <= 1)
 */
int pow2At(int n) { int p = 1; while (p < n) p <<= 1; return p; }
}

// ---------------------------------------------------------------- Ensemble

void Ensemble::prepare(double sampleRate)
{
    sr_ = sampleRate;
    // Room for the chorus taps (22 ms) and for the microshifter, whose longest read is the right
    // channel's 19 ms base plus two 200 ms ramps.
    const int size = pow2At(static_cast<int>(0.45 * sr_) + 8);
    bufL_.assign(static_cast<size_t>(size), 0.0f);
    bufR_.assign(static_cast<size_t>(size), 0.0f);
    mask_ = size - 1;
    w_ = 0;
}

void Ensemble::process(float* L, float* R, int n)
{
    // Mix at zero means the wet signal is multiplied by nothing -- and all three modes computed it
    // anyway: three chorus voices with their delay reads, two pitch shifters with their
    // cross-fades, or up to forty-eight velvet taps, per sample, to be thrown away. The delay line
    // is still fed, so the effect has its history the moment it is turned back on; what is skipped
    // is only the part whose result was going to be discarded.
    if (mix_ <= 0.0f) {
        float* bl = bufL_.data();
        float* br = bufR_.data();
        for (int i = 0; i < n; ++i) {
            bl[w_ & mask_] = L[i];
            br[w_ & mask_] = R[i];
            ++w_;
        }
        return;
    }
    if (mode_ == 2)      processVelvet(L, R, n);
    else if (mode_ == 1) processShift(L, R, n);
    else                 processChorus(L, R, n);
}

/**
 * @brief Velvet noise: kVelvetTaps impulses of +-1, one in each equal interval of the sequence, at a
 *        random position inside its interval.
 *
 * Because the impulses are sparse and signed, the sequence
 * is spectrally flat -- convolving with it decorrelates without colouring -- and because they are
 * one per interval, they never clump into an audible echo.
 */
void Ensemble::buildVelvet(uint64_t seed)
{
    velvetLen_ = std::max(64, static_cast<int>(0.030 * sr_));   // 30 ms
    const float gain = 1.0f / std::sqrt(static_cast<float>(kVelvetTaps));
    const double spacing = static_cast<double>(velvetLen_) / kVelvetTaps;
    uint64_t s = seed | 1ull;
    auto next = [&s]() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; };
    for (int ch = 0; ch < 2; ++ch) {
        for (int k = 0; k < kVelvetTaps; ++k) {
            const double u = static_cast<double>(next() >> 11) / 9007199254740992.0;
            int pos = static_cast<int>(k * spacing + u * spacing);
            velvetPos_[ch][k] = std::min(velvetLen_ - 1, std::max(0, pos));
            velvetSign_[ch][k] = (next() & 1ull) ? gain : -gain;
        }
    }
}

void Ensemble::processVelvet(float* L, float* R, int n)
{
    if (velvetLen_ <= 0) buildVelvet(0x9E3779B97F4A7C15ull);
    const float mix = clampv(mix_, 0.0f, 1.0f);
    // Depth shortens the sequence: a short one decorrelates less and sounds tighter, a long one
    // opens the picture further. Depth 1 is the whole 30 ms.
    const int taps = std::max(8, static_cast<int>(kVelvetTaps * clampv(depth_, 0.1f, 1.0f)));
    const float norm = std::sqrt(static_cast<float>(kVelvetTaps) / static_cast<float>(taps));
    float* bl = bufL_.data();
    float* br = bufR_.data();
    for (int i = 0; i < n; ++i) {
        bl[w_ & mask_] = L[i];
        br[w_ & mask_] = R[i];
        float wetL = 0.0f, wetR = 0.0f;
        for (int k = 0; k < taps; ++k) {
            wetL += velvetSign_[0][k] * bl[(w_ - velvetPos_[0][k]) & mask_];
            wetR += velvetSign_[1][k] * br[(w_ - velvetPos_[1][k]) & mask_];
        }
        L[i] = L[i] * (1.0f - mix) + wetL * norm * mix;
        R[i] = R[i] * (1.0f - mix) + wetR * norm * mix;
        ++w_;
    }
}

/**
 * @brief Two delay-line pitch shifters, +c cents on the left and -c on the right.
 *
 * A read pointer that
 * walks towards the write head raises the pitch and one that walks away lowers it, and since it
 * cannot walk for ever it is wrapped, with a short cross-fade to a second tap one ramp behind.
 *
 * The ramp is long (200 ms of travel) and the cross-fade short (25 ms), and that is the whole
 * design. The obvious construction -- two taps half a cycle apart under a Hann pair, which is how
 * a granular pitch shifter is drawn in every textbook -- has both taps audible all the time, at a
 * fixed delay difference, which for a sustained tone is a comb filter: the two cancel wherever
 * that difference happens to be half a wavelength, and at twelve cents the pattern crawls through
 * the spectrum once every few seconds. Measured, it lost a fifth of the signal. Here the two taps
 * overlap for a thousandth of the cycle instead, so the shifter is a plain delay line at a
 * slowly changing delay, which is what a micro-shift is.
 *
 * The two channels also sit at different base delays (11 and 19 ms). Opposite detune and unequal
 * delay together decorrelate the pair without ever holding them at a fixed phase difference.
 */
void Ensemble::processShift(float* L, float* R, int n)
{
    const float msToSamples = static_cast<float>(sr_ / 1000.0);
    const float ramp = 200.0f * msToSamples;          // how far the read pointer travels
    const float xfadeSamples = 25.0f * msToSamples;   // and how long the hand-over takes
    const float baseL = 11.0f * msToSamples + 1.0f, baseR = 19.0f * msToSamples + 1.0f;
    const float mix = clampv(mix_, 0.0f, 1.0f);
    // Rate is a very slow wander of the detune itself: two channels whose difference never
    // settles cannot dig a fixed hole anywhere.
    const double wanderInc = static_cast<double>(rate_) * 0.25 / sr_;
    float* bl = bufL_.data();
    float* br = bufR_.data();
    for (int i = 0; i < n; ++i) {
        wanderPh_ += wanderInc;
        if (wanderPh_ >= 1.0) wanderPh_ -= 1.0;
        const float cents = clampv(depth_, 0.0f, 1.0f) * 12.0f * (1.0f + 0.15f * sin01(wanderPh_));
        bl[w_ & mask_] = L[i];
        br[w_ & mask_] = R[i];
        float out[2] = { 0.0f, 0.0f };
        for (int ch = 0; ch < 2; ++ch) {
            const float ratio = std::pow(2.0f, (ch == 0 ? cents : -cents) / 1200.0f);
            const float base = ch == 0 ? baseL : baseR;
            const float* buf = ch == 0 ? bl : br;
            const double inc = static_cast<double>(std::fabs(1.0f - ratio)) / ramp;
            shPh_[ch] += inc;
            if (shPh_[ch] >= 1.0) shPh_[ch] -= 1.0;
            const double p = shPh_[ch];
            // The cross-fade as a fraction of this cycle. As the detune goes to nothing the cycle
            // becomes infinitely long and the fraction goes to zero: at zero cents this is a
            // plain static delay, with no hand-over at all.
            const double xf = clampv(static_cast<double>(xfadeSamples) * inc, 1.0e-9, 0.4);
            // Up: the delay shrinks from base+ramp to base. Down: it grows.
            const float d1 = base + static_cast<float>(ratio > 1.0f ? 1.0 - p : p) * ramp;
            const float d2 = d1 + ramp;               // one ramp behind: where the tap will restart
            float y = ringRead(buf, mask_, w_, d1);
            if (p > 1.0 - xf) {                       // the hand-over, equal power
                const float t = static_cast<float>((p - (1.0 - xf)) / xf);
                const float gA = std::cos(0.5f * kPi * t), gB = std::sin(0.5f * kPi * t);
                y = gA * y + gB * ringRead(buf, mask_, w_, d2);
            }
            out[ch] = y;
        }
        L[i] = L[i] * (1.0f - mix) + out[0] * mix;
        R[i] = R[i] * (1.0f - mix) + out[1] * mix;
        ++w_;
    }
}

void Ensemble::processChorus(float* L, float* R, int n)
{
    static const float kBaseMs[3]  = { 13.0f, 17.0f, 22.0f };
    static const float kRateMul[3] = { 1.0f, 1.27f, 0.81f };
    const float msToSamples = static_cast<float>(sr_ / 1000.0);
    const float depthSamples = depth_ * 6.0f * msToSamples;
    const float mix = clampv(mix_, 0.0f, 1.0f);
    float* bl = bufL_.data();
    float* br = bufR_.data();
    for (int i = 0; i < n; ++i) {
        bl[w_ & mask_] = L[i];
        br[w_ & mask_] = R[i];
        float wetL = 0.0f, wetR = 0.0f;
        for (int k = 0; k < 3; ++k) {
            ph_[k] += rate_ * kRateMul[k] / sr_;
            if (ph_[k] >= 1.0) ph_[k] -= 1.0;
            double phR = ph_[k] + 0.25; if (phR >= 1.0) phR -= 1.0;
            const float dL = kBaseMs[k] * msToSamples + depthSamples * sin01(ph_[k]) + 1.0f;
            const float dR = kBaseMs[k] * msToSamples + depthSamples * sin01(phR) + 1.0f;
            wetL += ringRead(bl, mask_, w_, dL);
            wetR += ringRead(br, mask_, w_, dR);
        }
        wetL *= (1.0f / 3.0f);
        wetR *= (1.0f / 3.0f);
        L[i] = L[i] * (1.0f - mix) + wetL * mix;
        R[i] = R[i] * (1.0f - mix) + wetR * mix;
        ++w_;
    }
}

// ---------------------------------------------------------------- StereoDelay

void StereoDelay::prepare(double sampleRate)
{
    sr_ = sampleRate;
    const int size = pow2At(static_cast<int>(4.05 * sr_) + 64);
    bufL_.assign(static_cast<size_t>(size), 0.0f);
    bufR_.assign(static_cast<size_t>(size), 0.0f);
    mask_ = size - 1;
    w_ = 0;
    lpL_ = lpR_ = 0.0f;
    tLcur_ = tRcur_ = 0.0f;
}

void StereoDelay::set(float timeL, float timeR, float feedback, float cross, float damping, float absorb)
{
    const float maxT = static_cast<float>(mask_ - 64);
    tL_ = std::min(clampv(timeL, 0.001f, 4.0f) * static_cast<float>(sr_), maxT);
    tR_ = std::min(clampv(timeR, 0.001f, 4.0f) * static_cast<float>(sr_), maxT);
    if (tLcur_ <= 0.0f) tLcur_ = tL_;
    if (tRcur_ <= 0.0f) tRcur_ = tR_;
    fb_ = clampv(feedback, 0.0f, 0.98f);
    cross_ = clampv(cross, 0.0f, 1.0f);
    lpc_ = 1.0f - 0.9f * clampv(damping, 0.0f, 1.0f);
    absorb_ = clampv(absorb, 0.0f, 1.0f);
    if (absorb_ > 0.0f) {
        // The band narrows with the feedback: at full feedback and full absorb the loop keeps
        // 320 Hz .. 1 kHz, and every repeat passes through it again.
        const float lc = 20.0f * std::pow(2.0f, 4.0f * absorb_ * fb_);
        const float hc = 16000.0f * std::pow(2.0f, -4.0f * absorb_ * fb_);
        hpc_  = 1.0f - std::exp(-kTwoPi * lc / static_cast<float>(sr_));
        lpc2_ = 1.0f - std::exp(-kTwoPi * hc / static_cast<float>(sr_));
    }
}

void StereoDelay::process(const float* inL, const float* inR, float* wetL, float* wetR, int n)
{
    float* bl = bufL_.data();
    float* br = bufR_.data();
    const float modDepth = 0.0003f * static_cast<float>(sr_);   // 0.3 ms of slow wander
    const float glide = 0.0003f;
    for (int i = 0; i < n; ++i) {
        tLcur_ += (tL_ - tLcur_) * glide;
        tRcur_ += (tR_ - tRcur_) * glide;
        modPh_[0] += 0.07 / sr_; if (modPh_[0] >= 1.0) modPh_[0] -= 1.0;
        modPh_[1] += 0.053 / sr_; if (modPh_[1] >= 1.0) modPh_[1] -= 1.0;
        const float dL = tLcur_ + modDepth * sin01(modPh_[0]) + 1.0f;
        const float dR = tRcur_ + modDepth * sin01(modPh_[1]) + 1.0f;
        const float oL = ringRead(bl, mask_, w_, dL);
        const float oR = ringRead(br, mask_, w_, dR);
        // Ducking the loop's brightness. The envelope follows the input fast and lets go slowly,
        // so an attack darkens the feedback at once and the tail opens again over a second or so
        // as the note settles -- the echoes make room for the articulation instead of piling a
        // new attack onto the brightness of the last one.
        float lpc = lpc_;
        if (duck_ > 0.0f) {
            // Driven by the input's level against its own slow average, not by the level itself.
            // A drone has no transients and correctly ducks nothing; an attack stands well above
            // the average and ducks hard. A plain level follower would simply darken any loud
            // passage, which is a tone control with extra steps.
            const float x = 0.5f * (std::fabs(inL[i]) + std::fabs(inR[i]));
            // Fast up, slow down: the duck grabs an attack in a millisecond and lets go over
            // about a second, so a whole train of echoes stays out of the way rather than only
            // the first ten milliseconds of it. The release used to be 10 ms, which is why the
            // effect measured at five per cent and looked like nothing.
            duckFast_ += (x > duckFast_ ? 0.05f : 0.00002f) * (x - duckFast_);
            duckEnv_  += 0.000006f * (x - duckEnv_);                       // the slow average, ~3.5 s
            const float excess = duckFast_ - duckEnv_;
            const float amount = duck_ * clampv(excess * 14.0f, 0.0f, 1.0f);
            lpc = lpc_ * (1.0f - 0.92f * amount);       // a lower coefficient is a lower cut-off
        }
        lpL_ += lpc * (oL - lpL_);
        lpR_ += lpc * (oR - lpR_);
        float fdL = lpL_, fdR = lpR_;
        if (absorb_ > 0.0f) {   // the absorption band: low cut, then the sinking high cut
            loL_ += hpc_ * (fdL - loL_); loR_ += hpc_ * (fdR - loR_);
            hiL_ += lpc2_ * ((fdL - loL_) - hiL_); hiR_ += lpc2_ * ((fdR - loR_) - hiR_);
            fdL = hiL_; fdR = hiR_;
        }
        const float fbL = fb_ * ((1.0f - cross_) * fdL + cross_ * fdR);
        const float fbR = fb_ * ((1.0f - cross_) * fdR + cross_ * fdL);
        bl[w_ & mask_] = inL[i] + fbL;
        br[w_ & mask_] = inR[i] + fbR;
        wetL[i] = oL;
        wetR[i] = oR;
        ++w_;
    }
}

// ---------------------------------------------------------------- Diffuser

void Diffuser::prepare(double sampleRate)
{
    sr_ = sampleRate;
    // Sized from the rate: fixed at 8192 the ring was shorter than the 47.9 ms stage from
    // 176.4 kHz up, and that stage silently became a 5 ms one.
    int size = 1 << 13;
    while (size < static_cast<int>(0.05 * sr_) + 64) size <<= 1;
    // Lengths in the ratio of small primes, so the four stages never line up and the field stays
    // dense instead of ringing.
    const double ms[kStages] = { 13.7, 21.3, 33.1, 47.9 };
    for (int k = 0; k < kStages; ++k) {
        len_[k] = static_cast<int>(ms[k] * 0.001 * sr_);
        for (int c = 0; c < 2; ++c) buf_[c][k].assign(static_cast<size_t>(size), 0.0f);
    }
    mask_ = size - 1;
    w_ = 0;
}

void Diffuser::reset()
{
    for (int c = 0; c < 2; ++c) for (int k = 0; k < kStages; ++k) std::fill(buf_[c][k].begin(), buf_[c][k].end(), 0.0f);
    w_ = 0;
}

void Diffuser::set(float amount) { amount_ = clampv(amount, 0.0f, 1.0f); }

void Diffuser::process(float* L, float* R, int n)
{
    if (amount_ <= 0.0f) return;
    const float g = 0.5f + 0.35f * amount_;      // all-pass coefficient: denser the further it goes
    const float wet = amount_;
    const float modDepth = 0.0004f * static_cast<float>(sr_) * amount_;
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < 2; ++c) {
            float x = c == 0 ? L[i] : R[i];
            const float dry = x;
            for (int k = 0; k < kStages; ++k) {
                // A slowly moving read point keeps the field from settling into a comb; the two
                // channels take opposite sides of the modulation, which widens the swell.
                const double inc = (0.031 + 0.017 * k) / sr_;
                if (c == 0) { modPh_[k] += inc; if (modPh_[k] >= 1.0) modPh_[k] -= 1.0; }
                const float m = modDepth * sin01(c == 0 ? modPh_[k] : 1.0 - modPh_[k]);
                const float delay = static_cast<float>(len_[k]) + m + 1.0f;
                float* b = buf_[c][k].data();
                const float d = ringRead(b, mask_, w_, delay);
                const float y = d - g * x;
                b[static_cast<size_t>(w_ & mask_)] = x + g * y;
                x = y;
            }
            const float out = dry + (x - dry) * wet;
            if (c == 0) L[i] = out; else R[i] = out;
        }
        ++w_;
    }
}

// ---------------------------------------------------------------- Unmask

void Unmask::prepare(double sampleRate)
{
    sr_ = sampleRate;
    // Crossovers an octave apart from 150 Hz to 4.8 kHz, second-order Butterworth low-passes. The
    // one-poles the three-band version had were too gentle for seven: a 2 kHz foreground leaked
    // into every band down to 150 Hz and took the background's bass with it (measured: -1.0 dB at
    // 200 Hz for -2.4 dB at 2.1 kHz). The bands are still differences of neighbouring low-passes,
    // so whatever their slopes they add back to the input exactly.
    for (int k = 0; k < kBands - 1; ++k) {
        xNear_[k].setQ(150.0f * static_cast<float>(1 << k), 0.7071f, static_cast<float>(sr_));
        xBg_[0][k].copyCoefficients(xNear_[k]);
        xBg_[1][k].copyCoefficients(xNear_[k]);
    }
    aCoef_ = 1.0f - std::exp(-1.0f / (0.02f * static_cast<float>(sr_)));    // duck in 20 ms (the guide's attack)
    rCoef_ = 1.0f - std::exp(-1.0f / (returnSec_ * static_cast<float>(sr_)));
    reset();
}

void Unmask::reset()
{
    for (Svf& s : xNear_) s.reset();
    for (auto& ch : xBg_) for (Svf& s : ch) s.reset();
    for (float& e : env_) e = 0.0f;
    for (float& g : gain_) g = 1.0f;
}

void Unmask::set(float amount, float spread, float returnSeconds)
{
    amount_ = clampv(amount, 0.0f, 1.0f); spread_ = clampv(spread, 0.0f, 1.0f);
    const float r = clampv(returnSeconds, 0.05f, 30.0f);
    if (r != returnSec_) { returnSec_ = r; rCoef_ = 1.0f - std::exp(-1.0f / (r * static_cast<float>(sr_))); }
}

void Unmask::split(Svf* lp, float x, float* band)
{
    // Each crossover low-passes the input at its corner; a band is the difference of two
    // neighbours, so the bands telescope back to the input exactly.
    float below = 0.0f;
    for (int k = 0; k < kBands - 1; ++k) {
        float l, b, h;
        lp[k].tick(x, l, b, h);
        band[k] = l - below;
        below = l;
    }
    band[kBands - 1] = x - below;
}

void Unmask::process(const float* nearL, const float* nearR, float* farL, float* farR, int n)
{
    if (amount_ <= 0.0f) return;
    const float depth = 0.85f * amount_;   // at most about 16 dB of duck
    // The band envelopes of a broadband foreground are smaller in seven bands than they were in
    // three; the scale keeps the duck of such a foreground where it was (sqrt(7 / 3)).
    const float k = 24.0f * 1.5275f;
    for (int i = 0; i < n; ++i) {
        float nb[kBands];
        split(xNear_, 0.5f * (nearL[i] + nearR[i]), nb);
        for (int b = 0; b < kBands; ++b) {
            const float mag = std::fabs(nb[b]);
            env_[b] += (mag > env_[b] ? aCoef_ : rCoef_) * (mag - env_[b]);
        }
        for (int b = 0; b < kBands; ++b) {
            // The upward spread of masking: a band is also masked by the bands below it, at
            // half strength one band down and a quarter two down, and only a little by the
            // band above. At spread 0 each band hears itself alone.
            float e = env_[b];
            if (spread_ > 0.0f) {
                if (b >= 1) e += spread_ * 0.5f * env_[b - 1];
                if (b >= 2) e += spread_ * 0.25f * env_[b - 2];
                if (b + 1 < kBands) e += spread_ * 0.1f * env_[b + 1];
            }
            // A gain that falls smoothly with the foreground's level and returns on its own.
            const float target = 1.0f / (1.0f + depth * e * k);
            gain_[b] += (target < gain_[b] ? aCoef_ : rCoef_) * (target - gain_[b]);
        }
        // The background, the same split, each band scaled, then put back together.
        for (int ch = 0; ch < 2; ++ch) {
            float* f = ch == 0 ? farL : farR;
            float bb[kBands];
            split(xBg_[ch], f[i], bb);
            float y = 0.0f;
            for (int b = 0; b < kBands; ++b) y += bb[b] * gain_[b];
            f[i] = y;
        }
    }
}

// ---------------------------------------------------------------- Patina

void Patina::prepare(double sampleRate, uint64_t seed)
{
    sr_ = sampleRate;
    const int size = 1 << 12;   // 85 ms at 48 kHz: room for the wow's swing and its centre delay
    bufL_.assign(static_cast<size_t>(size), 0.0f);
    bufR_.assign(static_cast<size_t>(size), 0.0f);
    mask_ = size - 1;
    w_ = 0;
    rng_.seed(seed);
    wowDrift_.init(rng_);
    reset();
}

void Patina::reset()
{
    std::fill(bufL_.begin(), bufL_.end(), 0.0f);
    std::fill(bufR_.begin(), bufR_.end(), 0.0f);
    lpL_ = lpR_ = 0.0f;
    env_ = 0.0f;
    w_ = 0;
    osL_.reset(); osR_.reset();
}

void Patina::set(float amount, float wow, float hiss, float age)
{
    amount_ = clampv(amount, 0.0f, 1.0f);
    wow_ = clampv(wow, 0.0f, 1.0f);
    hiss_ = clampv(hiss, 0.0f, 1.0f);
    age_ = clampv(age, 0.0f, 1.0f);
    // Age: the top end a worn machine and worn tape no longer carry. 20 kHz down to 4 kHz.
    const float hc = 20000.0f * std::pow(2.0f, -2.32f * age_ * amount_);
    lpC_ = hc >= 19000.0f ? 1.0f : 1.0f - std::exp(-kTwoPi * hc / static_cast<float>(sr_));
}

void Patina::process(float* L, float* R, int n)
{
    if (amount_ <= 0.0f) return;
    const float dt = static_cast<float>(n / sr_);
    // Wow (slow, irregular) and flutter (a steady 6 Hz), as a moving read point in the ring. Its
    // centre delay is fixed, so the effect is a wavering pitch and not a delay.
    const float wowV = wowDrift_.update(dt, 0.35f, rng_);
    const float centre = 0.004f * static_cast<float>(sr_);
    const float swing = 0.0022f * static_cast<float>(sr_) * wow_ * amount_;
    for (int i = 0; i < n; ++i) {
        flutterPh_ += 6.0 / sr_; if (flutterPh_ >= 1.0) flutterPh_ -= 1.0;
        const float d = centre + swing * (0.8f * wowV + 0.2f * sin01(flutterPh_));
        bufL_[static_cast<size_t>(w_ & mask_)] = L[i];
        bufR_[static_cast<size_t>(w_ & mask_)] = R[i];
        // In double, and with the write pointer kept inside the ring: as a float the pointer
        // stopped being an integer after 2^24 samples (six minutes at 48 kHz) and the read head
        // began to jitter by one sample, then two, then four against the write position -- a
        // Nyquist buzz on the master that got worse for as long as the instrument ran.
        const double rp = static_cast<double>(w_) - static_cast<double>(d);
        const int i0 = static_cast<int>(std::floor(rp));
        const float f = static_cast<float>(rp - static_cast<double>(i0));
        auto read = [&](const std::vector<float>& b) {
            const float a = b[static_cast<size_t>(i0 & mask_)], c = b[static_cast<size_t>((i0 + 1) & mask_)];
            return a + f * (c - a);
        };
        float l = read(bufL_), r = read(bufR_);
        w_ = (w_ + 1) & mask_;   // stays an exact integer for ever
        // The lost top end.
        if (lpC_ < 1.0f) { lpL_ += lpC_ * (l - lpL_); lpR_ += lpC_ * (r - lpR_); l = lpL_; r = lpR_; }
        // A noise floor that lives with the music: mostly constant, a little louder when the tape
        // is being asked to carry more (that is what modulation noise is).
        if (hiss_ > 0.0f) {
            const float mag = std::fabs(0.5f * (l + r));
            env_ += (mag > env_ ? 0.01f : 0.0002f) * (mag - env_);
            const float floorLevel = hiss_ * amount_ * 0.0016f * (1.0f + 3.0f * env_);
            l += floorLevel * rng_.bipolar();
            r += floorLevel * rng_.bipolar();
        }
        // Gentle asymmetric saturation: the third harmonic a tape adds before it ever clips. At four
        // times the rate (25.09.2026), so the harmonic of a bright top end does not fold back.
        const float drive = 1.0f + 1.5f * amount_;
        auto tape = [drive](float v) { return softClip(v * drive) / drive; };
        l = osL_.process(l, tape);
        r = osR_.process(r, tape);
        L[i] = l;
        R[i] = r;
    }
}

// ---------------------------------------------------------------- Reverb

void Reverb::prepare(double sampleRate)
{
    sr_ = sampleRate;
    const int need = static_cast<int>(std::max(0.08 * 3.0 * 1.1 * sr_, 0.5 * sr_)) + 64;
    const int size = pow2At(need);
    mask_ = size - 1;
    w_ = 0;
    for (auto& l : line_) l.assign(static_cast<size_t>(size), 0.0f);
    for (auto& a : ap_)   a.assign(static_cast<size_t>(size), 0.0f);
    for (auto& a : apR_)  a.assign(static_cast<size_t>(size), 0.0f);
    pre_.assign(static_cast<size_t>(size), 0.0f);
    preR_.assign(static_cast<size_t>(size), 0.0f);
    const int outSize = pow2At(static_cast<int>(0.012 * sr_) + 8);
    outR_.assign(static_cast<size_t>(outSize), 0.0f);
    outMask_ = outSize - 1;
    dcR_ = 1.0f - kTwoPi * 5.0f / static_cast<float>(sr_);
    dcInX_[0] = dcInX_[1] = dcInY_[0] = dcInY_[1] = 0.0f;

    static const float kApMs[kAllpasses] = { 5.1f, 7.3f, 11.3f, 13.7f };
    for (int k = 0; k < kAllpasses; ++k) apLen_[k] = std::max(1, static_cast<int>(kApMs[k] * sr_ / 1000.0));
    // The scattering all-passes: short, mutually prime lengths, so no two lines scatter alike.
    static const float kScMs[kLines] = { 1.9f, 2.3f, 2.9f, 3.7f, 4.3f, 5.3f, 6.1f, 7.1f };
    for (int l = 0; l < kLines; ++l) { sc_[l].assign(static_cast<size_t>(size), 0.0f); scLen_[l] = std::max(1, static_cast<int>(kScMs[l] * sr_ / 1000.0)); }
    static const float kModHz[kLines] = { 0.11f, 0.13f, 0.17f, 0.19f, 0.23f, 0.29f, 0.31f, 0.37f };
    for (int l = 0; l < kLines; ++l) { modRate_[l] = kModHz[l]; modPh_[l] = l / static_cast<double>(kLines); lp_[l] = 0.0f; lenCur_[l] = 0.0f; }
    // The rotating mode's angles: eight rates, none a multiple of another, staggered in phase.
    static const float kRotHz[kRotations] = { 0.031f, 0.043f, 0.057f, 0.071f, 0.083f, 0.097f, 0.113f, 0.127f };
    for (int k = 0; k < kRotations; ++k) {
        const float ph = static_cast<float>(k) * 0.3927f;   // pi / 8 apart
        rotC_[k] = std::cos(ph); rotS_[k] = std::sin(ph);
        const float d = kTwoPi * kRotHz[k] / static_cast<float>(sr_);
        rotDc_[k] = std::cos(d); rotDs_[k] = std::sin(d);
    }
    rot_ = 0.0f; rotNorm_ = 0;
    preCur_ = 0.0f;
    outDelayCur_ = 0.0f;
    hcL_ = hcR_ = 0.0f;
    lcL1_ = lcR1_ = lcL2_ = lcR2_ = 0.0f;
    setSpace(asym_, 20000.0f);
    set(size_, decay_, damp_, 40.0f, false, mix_);
}

void Reverb::setSpace(float asymmetry, float highcutHz, float lowcutHz)
{
    asym_ = clampv(asymmetry, 0.0f, 1.0f);
    const float fc = clampv(highcutHz, 200.0f, 20000.0f);
    hcCoef_ = (fc >= 19000.0f) ? 1.0f : (1.0f - std::exp(-kTwoPi * fc / static_cast<float>(sr_)));
    // The knob says where the tail is 3 dB down, so the pole does not sit there: two cascaded
    // one-poles are 6 dB down at their own corner, and the pair reaches -3 dB at 1.554 times it.
    // Without this division a "400 Hz" low cut would take 6 dB out at 400 Hz, and the number on
    // the panel would be a number about the filter rather than about the sound.
    const float lf = clampv(lowcutHz, 20.0f, 1000.0f);
    lcCoef_ = (lf <= 21.0f) ? 0.0f
            : (1.0f - std::exp(-kTwoPi * (lf / 1.5538f) / static_cast<float>(sr_)));
    outDelayTarget_ = asym_ * 0.010f * static_cast<float>(sr_);
    set(size_, decay_, damp_, preTarget_ * 1000.0f / static_cast<float>(sr_), freeze_, mix_);
}

void Reverb::setSendLowcut(float hz)
{
    sendHpOn_ = hz > 21.0f;
    if (sendHpOn_) {
        sendHpL_.setQ(clampv(hz, 20.0f, 1000.0f), 0.7071f, static_cast<float>(sr_));   // Butterworth: -3 dB at the knob, 12 dB/oct below
        sendHpR_.copyCoefficients(sendHpL_);
    } else {
        sendHpL_.reset(); sendHpR_.reset();
    }
}

void Reverb::set(float size, float decaySeconds, float damping, float preDelayMs, bool freeze, float mix)
{
    static const float kBaseMs[kLines] = { 29.7f, 37.1f, 41.1f, 43.7f, 53.3f, 61.9f, 71.3f, 79.9f };
    // Searched by Tools/optimise_fdn.py (seed 7, 60 draws) over mutually prime lengths spread
    // across the same span, scored on the spread of the simulated tail's third-octave
    // magnitudes: 0.314 dB against the classic set's 0.474 dB.
    static const float kFlatMs[kLines] = { 29.7f, 31.4f, 39.3f, 46.6f, 55.1f, 58.6f, 68.4f, 89.0f };
    size_ = clampv(size, 0.5f, 3.0f);
    decay_ = std::max(decaySeconds, 0.1f);
    damp_ = clampv(damping, 0.0f, 1.0f);
    mix_ = clampv(mix, 0.0f, 1.0f);
    freeze_ = freeze;
    const float maxLen = static_cast<float>(mask_) - 8.0f;
    for (int l = 0; l < kLines; ++l) {
        const float stretch = (l >= kLines / 2) ? (1.0f + 0.08f * asym_) : 1.0f;   // right-hand group runs longer
        const float baseMs = (mode_ >= 2) ? kFlatMs[l] : kBaseMs[l];
        lenTarget_[l] = std::min(baseMs * size_ * stretch * static_cast<float>(sr_ / 1000.0), maxLen);
        if (lenCur_[l] <= 0.0f) lenCur_[l] = lenTarget_[l];
        gain_[l] = freeze_ ? 1.0f : std::pow(10.0f, -3.0f * lenTarget_[l] / (decay_ * static_cast<float>(sr_)));
    }
    preTarget_ = std::min(clampv(preDelayMs, 0.0f, 500.0f) * static_cast<float>(sr_ / 1000.0), maxLen);
    if (preCur_ <= 0.0f) preCur_ = preTarget_;
}

void Reverb::process(float* L, float* R, int n)
{
    const float lpc = freeze_ ? 1.0f : (1.0f - 0.92f * damp_);
    const float inGain = freeze_ ? 0.0f : 0.5f;
    const float mix = mix_;
    const float glide = 0.0005f;
    const float rotTarget = (mode_ == 3) ? 1.0f : 0.0f;
    float* outR = outR_.data();
    for (int i = 0; i < n; ++i) {
        rot_ += (rotTarget - rot_) * glide;
        if (rot_ < 1.0e-6f) rot_ = 0.0f;
        // Both channels, each through its own pre-delay and diffusion and into its own half of the
        // network: the left into lines 0-3, whose taps are the left output, the right into 4-7.
        //
        // The input used to be the mono sum, which hears nothing of what a recording keeps between
        // its two channels -- measured, an anti-phase pair reached the near hall 16.7 dB down --
        // and since the far plane is nothing but this hall, every voice placed there lost its side
        // and its width on the way in. The Householder feedback still mixes all eight lines, so
        // the tail spreads across the field as it grows; what arrives first keeps its side.
        // Identical channels inject exactly what the mono sum did, and a hard-panned source now
        // excites the hall as strongly as a centred one of the same power (the sum gave it 3 dB less).
        // No direct current into the network (see the header): what a source leaves as an offset
        // -- a recording that carries one, FM at an integer ratio, an asymmetric saturation --
        // would be integrated by the loop for the length of the tail. The dry path below keeps it;
        // the output blocker at the end of the chain takes it there.
        float inL = L[i] - dcInX_[0] + dcR_ * dcInY_[0]; dcInX_[0] = L[i]; dcInY_[0] = inL;
        float inR = R[i] - dcInX_[1] + dcR_ * dcInY_[1]; dcInX_[1] = R[i]; dcInY_[1] = inR;
        if (sendHpOn_) {   // Send Low Cut: the fundamentals stay out of the tail (the dry share above is untouched)
            float lp, bp, hp;
            sendHpL_.tick(inL, lp, bp, hp); inL = hp;
            sendHpR_.tick(inR, lp, bp, hp); inR = hp;
        }
        pre_[static_cast<size_t>(w_ & mask_)] = inL;
        preR_[static_cast<size_t>(w_ & mask_)] = inR;
        preCur_ += (preTarget_ - preCur_) * glide;
        float xl = ringRead(pre_.data(), mask_, w_, preCur_ + 1.0f);
        float xr = ringRead(preR_.data(), mask_, w_, preCur_ + 1.0f);

        for (int k = 0; k < kAllpasses; ++k) {
            float* a = ap_[k].data();
            float* ar = apR_[k].data();
            const float d = a[(w_ - apLen_[k]) & mask_];
            const float dr = ar[(w_ - apLen_[k]) & mask_];
            const float y = d - 0.6f * xl;
            const float yr = dr - 0.6f * xr;
            a[w_ & mask_] = xl + 0.6f * y;
            ar[w_ & mask_] = xr + 0.6f * yr;
            xl = y;
            xr = yr;
        }

        float o[kLines];
        float sum = 0.0f;
        for (int l = 0; l < kLines; ++l) {
            lenCur_[l] += (lenTarget_[l] - lenCur_[l]) * glide;
            modPh_[l] += modRate_[l] / sr_;
            if (modPh_[l] >= 1.0) modPh_[l] -= 1.0;
            const float d = lenCur_[l] + (1.5f * (1.0f - rot_)) * sin01(modPh_[l]) + 2.0f;   // the wobble fades as the matrix takes over
            float v = ringRead(line_[l].data(), mask_, w_, d);
            if (mode_ != 0) {   // scattering (and colourless, which also scatters)
                float* sb = sc_[l].data();
                const float sd = sb[(w_ - scLen_[l]) & mask_];
                const float sy = sd - 0.5f * v;
                sb[w_ & mask_] = v + 0.5f * sy;
                v = sy;
            }
            lp_[l] += lpc * (v - lp_[l]);
            o[l] = lp_[l];
            sum += o[l];
        }
        const float hh = sum * (2.0f / static_cast<float>(kLines));   // Householder reflection
        if (rot_ <= 0.0f) {
            for (int l = 0; l < kLines; ++l)
                line_[l][static_cast<size_t>(w_ & mask_)] = gain_[l] * (o[l] - hh) + ((l & 1) ? -inGain : inGain) * (l < kLines / 2 ? xl : xr);
        } else {
            // The turning matrix: the reflection's output through two layers of Givens rotations,
            // neighbours first and then across, every angle advanced by its own tiny step per
            // sample (a rotation of a rotation, no trigonometry in the loop; the pair is pulled
            // back onto the unit circle every few thousand samples so rounding cannot walk it
            // off). Between the two matrices while the mode is switching.
            float v[kLines], u[kLines];
            for (int l = 0; l < kLines; ++l) v[l] = o[l] - hh;
            for (int k = 0; k < kRotations; ++k) {
                const float c = rotC_[k], s = rotS_[k];
                rotC_[k] = c * rotDc_[k] - s * rotDs_[k];
                rotS_[k] = s * rotDc_[k] + c * rotDs_[k];
            }
            if (++rotNorm_ >= 4096) {
                rotNorm_ = 0;
                for (int k = 0; k < kRotations; ++k) {
                    const float g = 1.0f / std::sqrt(rotC_[k] * rotC_[k] + rotS_[k] * rotS_[k]);
                    rotC_[k] *= g; rotS_[k] *= g;
                }
            }
            static const int kPair[kRotations][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 } };
            for (int k = 0; k < 4; ++k) {
                const int a = kPair[k][0], b = kPair[k][1];
                u[a] = rotC_[k] * v[a] - rotS_[k] * v[b];
                u[b] = rotS_[k] * v[a] + rotC_[k] * v[b];
            }
            for (int k = 4; k < kRotations; ++k) {
                const int a = kPair[k][0], b = kPair[k][1];
                const float ua = rotC_[k] * u[a] - rotS_[k] * u[b];
                const float ub = rotS_[k] * u[a] + rotC_[k] * u[b];
                u[a] = ua; u[b] = ub;
            }
            for (int l = 0; l < kLines; ++l)
                line_[l][static_cast<size_t>(w_ & mask_)] = gain_[l] * (v[l] + rot_ * (u[l] - v[l])) + ((l & 1) ? -inGain : inGain) * (l < kLines / 2 ? xl : xr);
        }

        float wetL = 0.3f * (o[0] - o[1] + o[2] - o[3]);
        float wetR = 0.3f * (o[4] - o[5] + o[6] - o[7]);
        // Interaural disparity: the right output arrives a little later.
        outR[w_ & outMask_] = wetR;
        outDelayCur_ += (outDelayTarget_ - outDelayCur_) * glide;
        wetR = ringRead(outR, outMask_, w_, outDelayCur_ + 1.0f);
        // Tail darkening.
        hcL_ += hcCoef_ * (wetL - hcL_);
        hcR_ += hcCoef_ * (wetR - hcR_);
        // The other end of the funnel. Two one-pole high-passes, 12 dB/oct: what each low-pass
        // does not take is what passes. The tail's own value goes into `tailL/R` rather than back
        // into hcL_/hcR_ -- those are the low-pass's state, and writing the filtered result into
        // them would feed the darkening filter its own output every sample.
        float tailL = hcL_, tailR = hcR_;
        if (lcCoef_ > 0.0f) {
            lcL1_ += lcCoef_ * (tailL - lcL1_);   const float aL = tailL - lcL1_;
            lcR1_ += lcCoef_ * (tailR - lcR1_);   const float aR = tailR - lcR1_;
            lcL2_ += lcCoef_ * (aL - lcL2_);      tailL = aL - lcL2_;
            lcR2_ += lcCoef_ * (aR - lcR2_);      tailR = aR - lcR2_;
        }
        L[i] = L[i] * (1.0f - mix) + tailL * mix;
        R[i] = R[i] * (1.0f - mix) + tailR * mix;
        ++w_;
    }
}

// ---------------------------------------------------------------- MidSide

void MidSide::prepare(double sampleRate)
{
    sr_ = sampleRate;
    hp_.reset();
    air_.reset();
    set(150.0f, 2.0f, 1.2f);
}

void MidSide::set(float bassMonoHz, float sideAirDb, float width)
{
    hp_.setQ(clampv(bassMonoHz, 20.0f, 400.0f), 0.707f, static_cast<float>(sr_));
    air_.setQ(3000.0f, 0.6f, static_cast<float>(sr_));           // broad, gentle upper-mid bell
    airGain_ = std::pow(10.0f, clampv(sideAirDb, 0.0f, 12.0f) / 20.0f) - 1.0f;
    width_ = clampv(width, 0.0f, 2.0f);
}

void MidSide::setTilt(float dB, float pivotHz)
{
    tiltOn_ = std::fabs(dB) > 0.05f;
    if (!tiltOn_) { tiltLo_ = tiltHi_ = 1.0f; return; }
    // Half the tilt each way, so the level through the middle stays where it was.
    tiltLo_ = std::pow(10.0f, -0.5f * dB / 20.0f);
    tiltHi_ = std::pow(10.0f,  0.5f * dB / 20.0f);
    tiltC_ = 1.0f - std::exp(-kTwoPi * clampv(pivotHz, 20.0f, 0.45f * static_cast<float>(sr_)) / static_cast<float>(sr_));
}

void MidSide::process(float* L, float* R, int n)
{
    if (tiltOn_)
        for (int i = 0; i < n; ++i) {
            tiltState_[0] += tiltC_ * (L[i] - tiltState_[0]);
            tiltState_[1] += tiltC_ * (R[i] - tiltState_[1]);
            L[i] = tiltState_[0] * tiltLo_ + (L[i] - tiltState_[0]) * tiltHi_;
            R[i] = tiltState_[1] * tiltLo_ + (R[i] - tiltState_[1]) * tiltHi_;
        }
    for (int i = 0; i < n; ++i) {
        const float m = 0.5f * (L[i] + R[i]);
        float s = 0.5f * (L[i] - R[i]);
        float lp, bp, hp;
        hp_.tick(s, lp, bp, hp);          // everything below the crossover collapses to the centre
        s = hp;
        air_.tick(s, lp, bp, hp);
        s += airGain_ * bp;               // lift the side's upper mids
        const float sWide = s * width_;         // what the side would be without the guard
        s = sWide * guard_;
        L[i] = m + s;
        R[i] = m - s;
        if (guardOn_) {
            // Both powers on a very slow average -- about a second and a half at 48 kHz -- so a
            // single wide transient does nothing and a drone that has settled into anti-phase is
            // caught. The correction is a trim on the width, never on the mid: the centre of the
            // mix is not touched, only how far the sides are allowed to go.
            constexpr float kA = 1.5e-5f;
            // Measured on the side BEFORE the guard's own trim. Watching its own output makes
            // the guard a feedback loop that hunts around the threshold; watching the mix means
            // it decides once and holds.
            midPow_  += kA * (m * m - midPow_);
            sidePow_ += kA * (sWide * sWide - sidePow_);
            // Side louder than mid means a mono sum loses more than it keeps. The trim follows
            // sqrt(mid/side), floored at a third, and moves at about 4 % per second either way.
            // A quarter is as far as it goes. This is a safety net, not a stereo policy: the
            // point is that a mono sum keeps most of what it had, not that the width is
            // "corrected" to something the mix never asked for.
            // The threshold is 1.3 and not 1.0 on purpose. At 1.02 the guard engaged, minutely,
            // on thirty of the thirty-seven reference presets -- by an amount too small to move
            // any descriptor, but it engaged. A safety net that is always slightly on is not a
            // safety net, it is a change to the sound. At 1.3 the side has to be half again the
            // power of the mid, which is a mix that really is collapsing in mono, and everything
            // else is left bit for bit alone.
            const float want = sidePow_ > midPow_ * 1.3f
                             ? clampv(std::sqrt(midPow_ / std::max(sidePow_, 1e-12f)), 0.75f, 1.0f)
                             : 1.0f;
            const float step = 5.0e-7f;                      // about 2 % of the range per second
            guard_ += clampv(want - guard_, -step, step);
            guard_ = clampv(guard_, 0.75f, 1.0f);
        } else {
            guard_ = 1.0f;
        }
    }
}

} // namespace ambient
