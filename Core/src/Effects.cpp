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
 * @author Rene Weller
 */
#include "ambient/Effects.h"
#include <cmath>
#include <algorithm>

namespace ambient {

namespace {
/**
 * @brief The smallest power of two that is at least @p n: the ring sizes, so a mask can wrap them.
 * @param[in] n  the number of samples a ring has to hold
 * @return the first power of two >= n (1 for n <= 1)
 */
int pow2At(int n) { int p = 1; while (p < n) p <<= 1; return p; }
}

// ---------------------------------------------------------------- Ensemble

void Ensemble::prepare(double sampleRate)
{
    m_sr = sampleRate;
    // Room for the chorus taps (22 ms) and for the microshifter, whose longest read is the right
    // channel's 19 ms base plus two 200 ms ramps.
    const int size = pow2At(static_cast<int>(0.45 * m_sr) + 8);
    m_bufL.assign(static_cast<size_t>(size), 0.0f);
    m_bufR.assign(static_cast<size_t>(size), 0.0f);
    m_mask = size - 1;
    m_w = 0;
}

void Ensemble::process(float* L, float* R, int n)
{
    // Mix at zero means the wet signal is multiplied by nothing -- and all three modes computed it
    // anyway: three chorus voices with their delay reads, two pitch shifters with their
    // cross-fades, or up to forty-eight velvet taps, per sample, to be thrown away. The delay line
    // is still fed, so the effect has its history the moment it is turned back on; what is skipped
    // is only the part whose result was going to be discarded.
    if (m_mix <= 0.0f) {
        float* bl = m_bufL.data();
        float* br = m_bufR.data();
        for (int i = 0; i < n; ++i) {
            bl[m_w & m_mask] = L[i];
            br[m_w & m_mask] = R[i];
            ++m_w;
        }
        return;
    }
    if (m_mode == 2)      processVelvet(L, R, n);
    else if (m_mode == 1) processShift(L, R, n);
    else                 processChorus(L, R, n);
}

/**
 * @brief Velvet noise: VelvetTaps impulses of +-1, one in each equal interval of the sequence, at a
 *        random position inside its interval.
 *
 * Because the impulses are sparse and signed, the sequence
 * is spectrally flat -- convolving with it decorrelates without colouring -- and because they are
 * one per interval, they never clump into an audible echo.
 */
void Ensemble::buildVelvet(uint64_t seed)
{
    m_velvetLen = std::max(64, static_cast<int>(0.030 * m_sr));   // 30 ms
    const float gain = 1.0f / std::sqrt(static_cast<float>(VelvetTaps));
    const double spacing = static_cast<double>(m_velvetLen) / VelvetTaps;
    uint64_t s = seed | 1ull;
    auto next = [&s]() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; };
    for (int ch = 0; ch < 2; ++ch) {
        for (int k = 0; k < VelvetTaps; ++k) {
            const double u = static_cast<double>(next() >> 11) / 9007199254740992.0;
            int pos = static_cast<int>(k * spacing + u * spacing);
            m_velvetPos[ch][k] = std::min(m_velvetLen - 1, std::max(0, pos));
            m_velvetSign[ch][k] = (next() & 1ull) ? gain : -gain;
        }
    }
}

void Ensemble::processVelvet(float* L, float* R, int n)
{
    if (m_velvetLen <= 0) buildVelvet(0x9E3779B97F4A7C15ull);
    const float mix = clampv(m_mix, 0.0f, 1.0f);
    // Depth shortens the sequence: a short one decorrelates less and sounds tighter, a long one
    // opens the picture further. Depth 1 is the whole 30 ms.
    const int taps = std::max(8, static_cast<int>(VelvetTaps * clampv(m_depth, 0.1f, 1.0f)));
    const float norm = std::sqrt(static_cast<float>(VelvetTaps) / static_cast<float>(taps));
    float* bl = m_bufL.data();
    float* br = m_bufR.data();
    for (int i = 0; i < n; ++i) {
        bl[m_w & m_mask] = L[i];
        br[m_w & m_mask] = R[i];
        float wetL = 0.0f, wetR = 0.0f;
        for (int k = 0; k < taps; ++k) {
            wetL += m_velvetSign[0][k] * bl[(m_w - m_velvetPos[0][k]) & m_mask];
            wetR += m_velvetSign[1][k] * br[(m_w - m_velvetPos[1][k]) & m_mask];
        }
        L[i] = L[i] * (1.0f - mix) + wetL * norm * mix;
        R[i] = R[i] * (1.0f - mix) + wetR * norm * mix;
        ++m_w;
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
    const float msToSamples = static_cast<float>(m_sr / 1000.0);
    const float ramp = 200.0f * msToSamples;          // how far the read pointer travels
    const float xfadeSamples = 25.0f * msToSamples;   // and how long the hand-over takes
    const float baseL = 11.0f * msToSamples + 1.0f, baseR = 19.0f * msToSamples + 1.0f;
    const float mix = clampv(m_mix, 0.0f, 1.0f);
    // Rate is a very slow wander of the detune itself: two channels whose difference never
    // settles cannot dig a fixed hole anywhere.
    const double wanderInc = static_cast<double>(m_rate) * 0.25 / m_sr;
    float* bl = m_bufL.data();
    float* br = m_bufR.data();
    for (int i = 0; i < n; ++i) {
        m_wanderPh += wanderInc;
        if (m_wanderPh >= 1.0) m_wanderPh -= 1.0;
        const float cents = clampv(m_depth, 0.0f, 1.0f) * 12.0f * (1.0f + 0.15f * sin01(m_wanderPh));
        bl[m_w & m_mask] = L[i];
        br[m_w & m_mask] = R[i];
        float out[2] = { 0.0f, 0.0f };
        for (int ch = 0; ch < 2; ++ch) {
            const float ratio = std::pow(2.0f, (ch == 0 ? cents : -cents) / 1200.0f);
            const float base = ch == 0 ? baseL : baseR;
            const float* buf = ch == 0 ? bl : br;
            const double inc = static_cast<double>(std::fabs(1.0f - ratio)) / ramp;
            m_shPh[ch] += inc;
            if (m_shPh[ch] >= 1.0) m_shPh[ch] -= 1.0;
            const double p = m_shPh[ch];
            // The cross-fade as a fraction of this cycle. As the detune goes to nothing the cycle
            // becomes infinitely long and the fraction goes to zero: at zero cents this is a
            // plain static delay, with no hand-over at all.
            const double xf = clampv(static_cast<double>(xfadeSamples) * inc, 1.0e-9, 0.4);
            // Up: the delay shrinks from base+ramp to base. Down: it grows.
            const float d1 = base + static_cast<float>(ratio > 1.0f ? 1.0 - p : p) * ramp;
            const float d2 = d1 + ramp;               // one ramp behind: where the tap will restart
            float y = ringRead(buf, m_mask, m_w, d1);
            if (p > 1.0 - xf) {                       // the hand-over, equal power
                const float t = static_cast<float>((p - (1.0 - xf)) / xf);
                const float gA = std::cos(0.5f * Pi * t), gB = std::sin(0.5f * Pi * t);
                y = gA * y + gB * ringRead(buf, m_mask, m_w, d2);
            }
            out[ch] = y;
        }
        L[i] = L[i] * (1.0f - mix) + out[0] * mix;
        R[i] = R[i] * (1.0f - mix) + out[1] * mix;
        ++m_w;
    }
}

void Ensemble::processChorus(float* L, float* R, int n)
{
    static const float BaseMs[3]  = { 13.0f, 17.0f, 22.0f };
    static const float RateMul[3] = { 1.0f, 1.27f, 0.81f };
    const float msToSamples = static_cast<float>(m_sr / 1000.0);
    const float depthSamples = m_depth * 6.0f * msToSamples;
    const float mix = clampv(m_mix, 0.0f, 1.0f);
    float* bl = m_bufL.data();
    float* br = m_bufR.data();
    for (int i = 0; i < n; ++i) {
        bl[m_w & m_mask] = L[i];
        br[m_w & m_mask] = R[i];
        float wetL = 0.0f, wetR = 0.0f;
        for (int k = 0; k < 3; ++k) {
            m_ph[k] += m_rate * RateMul[k] / m_sr;
            if (m_ph[k] >= 1.0) m_ph[k] -= 1.0;
            double phR = m_ph[k] + 0.25; if (phR >= 1.0) phR -= 1.0;
            const float dL = BaseMs[k] * msToSamples + depthSamples * sin01(m_ph[k]) + 1.0f;
            const float dR = BaseMs[k] * msToSamples + depthSamples * sin01(phR) + 1.0f;
            wetL += ringRead(bl, m_mask, m_w, dL);
            wetR += ringRead(br, m_mask, m_w, dR);
        }
        wetL *= (1.0f / 3.0f);
        wetR *= (1.0f / 3.0f);
        L[i] = L[i] * (1.0f - mix) + wetL * mix;
        R[i] = R[i] * (1.0f - mix) + wetR * mix;
        ++m_w;
    }
}

// ---------------------------------------------------------------- StereoDelay

void StereoDelay::prepare(double sampleRate)
{
    m_sr = sampleRate;
    const int size = pow2At(static_cast<int>(4.05 * m_sr) + 64);
    m_bufL.assign(static_cast<size_t>(size), 0.0f);
    m_bufR.assign(static_cast<size_t>(size), 0.0f);
    m_mask = size - 1;
    m_w = 0;
    m_lpL = m_lpR = 0.0f;
    m_tLcur = m_tRcur = 0.0f;
}

void StereoDelay::set(float timeL, float timeR, float feedback, float cross, float damping, float absorb)
{
    const float maxT = static_cast<float>(m_mask - 64);
    m_tL = std::min(clampv(timeL, 0.001f, 4.0f) * static_cast<float>(m_sr), maxT);
    m_tR = std::min(clampv(timeR, 0.001f, 4.0f) * static_cast<float>(m_sr), maxT);
    if (m_tLcur <= 0.0f) m_tLcur = m_tL;
    if (m_tRcur <= 0.0f) m_tRcur = m_tR;
    m_fb = clampv(feedback, 0.0f, 0.98f);
    m_cross = clampv(cross, 0.0f, 1.0f);
    m_lpc = 1.0f - 0.9f * clampv(damping, 0.0f, 1.0f);
    m_absorb = clampv(absorb, 0.0f, 1.0f);
    if (m_absorb > 0.0f) {
        // The band narrows with the feedback: at full feedback and full absorb the loop keeps
        // 320 Hz .. 1 kHz, and every repeat passes through it again.
        const float lc = 20.0f * std::pow(2.0f, 4.0f * m_absorb * m_fb);
        const float hc = 16000.0f * std::pow(2.0f, -4.0f * m_absorb * m_fb);
        m_hpc  = 1.0f - std::exp(-TwoPi * lc / static_cast<float>(m_sr));
        m_lpc2 = 1.0f - std::exp(-TwoPi * hc / static_cast<float>(m_sr));
    }
}

void StereoDelay::process(const float* inL, const float* inR, float* wetL, float* wetR, int n)
{
    float* bl = m_bufL.data();
    float* br = m_bufR.data();
    const float modDepth = 0.0003f * static_cast<float>(m_sr);   // 0.3 ms of slow wander
    const float glide = 0.0003f;
    for (int i = 0; i < n; ++i) {
        m_tLcur += (m_tL - m_tLcur) * glide;
        m_tRcur += (m_tR - m_tRcur) * glide;
        m_modPh[0] += 0.07 / m_sr; if (m_modPh[0] >= 1.0) m_modPh[0] -= 1.0;
        m_modPh[1] += 0.053 / m_sr; if (m_modPh[1] >= 1.0) m_modPh[1] -= 1.0;
        const float dL = m_tLcur + modDepth * sin01(m_modPh[0]) + 1.0f;
        const float dR = m_tRcur + modDepth * sin01(m_modPh[1]) + 1.0f;
        const float oL = ringRead(bl, m_mask, m_w, dL);
        const float oR = ringRead(br, m_mask, m_w, dR);
        // Ducking the loop's brightness. The envelope follows the input fast and lets go slowly,
        // so an attack darkens the feedback at once and the tail opens again over a second or so
        // as the note settles -- the echoes make room for the articulation instead of piling a
        // new attack onto the brightness of the last one.
        float lpc = m_lpc;
        if (m_duck > 0.0f) {
            // Driven by the input's level against its own slow average, not by the level itself.
            // A drone has no transients and correctly ducks nothing; an attack stands well above
            // the average and ducks hard. A plain level follower would simply darken any loud
            // passage, which is a tone control with extra steps.
            const float x = 0.5f * (std::fabs(inL[i]) + std::fabs(inR[i]));
            // Fast up, slow down: the duck grabs an attack in a millisecond and lets go over
            // about a second, so a whole train of echoes stays out of the way rather than only
            // the first ten milliseconds of it. The release used to be 10 ms, which is why the
            // effect measured at five per cent and looked like nothing.
            m_duckFast += (x > m_duckFast ? 0.05f : 0.00002f) * (x - m_duckFast);
            m_duckEnv  += 0.000006f * (x - m_duckEnv);                       // the slow average, ~3.5 s
            const float excess = m_duckFast - m_duckEnv;
            const float amount = m_duck * clampv(excess * 14.0f, 0.0f, 1.0f);
            lpc = m_lpc * (1.0f - 0.92f * amount);       // a lower coefficient is a lower cut-off
        }
        m_lpL += lpc * (oL - m_lpL);
        m_lpR += lpc * (oR - m_lpR);
        float fdL = m_lpL, fdR = m_lpR;
        if (m_absorb > 0.0f) {   // the absorption band: low cut, then the sinking high cut
            m_loL += m_hpc * (fdL - m_loL); m_loR += m_hpc * (fdR - m_loR);
            m_hiL += m_lpc2 * ((fdL - m_loL) - m_hiL); m_hiR += m_lpc2 * ((fdR - m_loR) - m_hiR);
            fdL = m_hiL; fdR = m_hiR;
        }
        const float fbL = m_fb * ((1.0f - m_cross) * fdL + m_cross * fdR);
        const float fbR = m_fb * ((1.0f - m_cross) * fdR + m_cross * fdL);
        bl[m_w & m_mask] = inL[i] + fbL;
        br[m_w & m_mask] = inR[i] + fbR;
        wetL[i] = oL;
        wetR[i] = oR;
        ++m_w;
    }
}

// ---------------------------------------------------------------- Diffuser

void Diffuser::prepare(double sampleRate)
{
    m_sr = sampleRate;
    // Sized from the rate: fixed at 8192 the ring was shorter than the 47.9 ms stage from
    // 176.4 kHz up, and that stage silently became a 5 ms one.
    int size = 1 << 13;
    while (size < static_cast<int>(0.05 * m_sr) + 64) size <<= 1;
    // Lengths in the ratio of small primes, so the four stages never line up and the field stays
    // dense instead of ringing.
    const double ms[Stages] = { 13.7, 21.3, 33.1, 47.9 };
    for (int k = 0; k < Stages; ++k) {
        m_len[k] = static_cast<int>(ms[k] * 0.001 * m_sr);
        for (int c = 0; c < 2; ++c) m_buf[c][k].assign(static_cast<size_t>(size), 0.0f);
    }
    m_mask = size - 1;
    m_w = 0;
}

void Diffuser::reset()
{
    for (int c = 0; c < 2; ++c) for (int k = 0; k < Stages; ++k) std::fill(m_buf[c][k].begin(), m_buf[c][k].end(), 0.0f);
    m_w = 0;
}

void Diffuser::set(float amount) { m_amount = clampv(amount, 0.0f, 1.0f); }

void Diffuser::process(float* L, float* R, int n)
{
    if (m_amount <= 0.0f) return;
    const float g = 0.5f + 0.35f * m_amount;      // all-pass coefficient: denser the further it goes
    const float wet = m_amount;
    const float modDepth = 0.0004f * static_cast<float>(m_sr) * m_amount;
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < 2; ++c) {
            float x = c == 0 ? L[i] : R[i];
            const float dry = x;
            for (int k = 0; k < Stages; ++k) {
                // A slowly moving read point keeps the field from settling into a comb; the two
                // channels take opposite sides of the modulation, which widens the swell.
                const double inc = (0.031 + 0.017 * k) / m_sr;
                if (c == 0) { m_modPh[k] += inc; if (m_modPh[k] >= 1.0) m_modPh[k] -= 1.0; }
                const float m = modDepth * sin01(c == 0 ? m_modPh[k] : 1.0 - m_modPh[k]);
                const float delay = static_cast<float>(m_len[k]) + m + 1.0f;
                float* b = m_buf[c][k].data();
                const float d = ringRead(b, m_mask, m_w, delay);
                const float y = d - g * x;
                b[static_cast<size_t>(m_w & m_mask)] = x + g * y;
                x = y;
            }
            const float out = dry + (x - dry) * wet;
            if (c == 0) L[i] = out; else R[i] = out;
        }
        ++m_w;
    }
}

// ---------------------------------------------------------------- Unmask

void Unmask::prepare(double sampleRate)
{
    m_sr = sampleRate;
    // Crossovers an octave apart from 150 Hz to 4.8 kHz, second-order Butterworth low-passes. The
    // one-poles the three-band version had were too gentle for seven: a 2 kHz foreground leaked
    // into every band down to 150 Hz and took the background's bass with it (measured: -1.0 dB at
    // 200 Hz for -2.4 dB at 2.1 kHz). The bands are still differences of neighbouring low-passes,
    // so whatever their slopes they add back to the input exactly.
    for (int k = 0; k < Bands - 1; ++k) {
        m_xNear[k].setQ(150.0f * static_cast<float>(1 << k), 0.7071f, static_cast<float>(m_sr));
        m_xBg[0][k].copyCoefficients(m_xNear[k]);
        m_xBg[1][k].copyCoefficients(m_xNear[k]);
    }
    m_aCoef = 1.0f - std::exp(-1.0f / (0.02f * static_cast<float>(m_sr)));    // duck in 20 ms (the guide's attack)
    m_rCoef = 1.0f - std::exp(-1.0f / (m_returnSec * static_cast<float>(m_sr)));
    reset();
}

void Unmask::reset()
{
    for (Svf& s : m_xNear) s.reset();
    for (auto& ch : m_xBg) for (Svf& s : ch) s.reset();
    for (float& e : m_env) e = 0.0f;
    for (float& g : m_gain) g = 1.0f;
}

void Unmask::set(float amount, float spread, float returnSeconds)
{
    m_amount = clampv(amount, 0.0f, 1.0f); m_spread = clampv(spread, 0.0f, 1.0f);
    const float r = clampv(returnSeconds, 0.05f, 30.0f);
    if (r != m_returnSec) { m_returnSec = r; m_rCoef = 1.0f - std::exp(-1.0f / (r * static_cast<float>(m_sr))); }
}

void Unmask::split(Svf* lp, float x, float* band)
{
    // Each crossover low-passes the input at its corner; a band is the difference of two
    // neighbours, so the bands telescope back to the input exactly.
    float below = 0.0f;
    for (int k = 0; k < Bands - 1; ++k) {
        float l, b, h;
        lp[k].tick(x, l, b, h);
        band[k] = l - below;
        below = l;
    }
    band[Bands - 1] = x - below;
}

void Unmask::process(const float* nearL, const float* nearR, float* farL, float* farR, int n)
{
    if (m_amount <= 0.0f) return;
    const float depth = 0.85f * m_amount;   // at most about 16 dB of duck
    // The band envelopes of a broadband foreground are smaller in seven bands than they were in
    // three; the scale keeps the duck of such a foreground where it was (sqrt(7 / 3)).
    const float k = 24.0f * 1.5275f;
    for (int i = 0; i < n; ++i) {
        float nb[Bands];
        split(m_xNear, 0.5f * (nearL[i] + nearR[i]), nb);
        for (int b = 0; b < Bands; ++b) {
            const float mag = std::fabs(nb[b]);
            m_env[b] += (mag > m_env[b] ? m_aCoef : m_rCoef) * (mag - m_env[b]);
        }
        for (int b = 0; b < Bands; ++b) {
            // The upward spread of masking: a band is also masked by the bands below it, at
            // half strength one band down and a quarter two down, and only a little by the
            // band above. At spread 0 each band hears itself alone.
            float e = m_env[b];
            if (m_spread > 0.0f) {
                if (b >= 1) e += m_spread * 0.5f * m_env[b - 1];
                if (b >= 2) e += m_spread * 0.25f * m_env[b - 2];
                if (b + 1 < Bands) e += m_spread * 0.1f * m_env[b + 1];
            }
            // A gain that falls smoothly with the foreground's level and returns on its own.
            const float target = 1.0f / (1.0f + depth * e * k);
            m_gain[b] += (target < m_gain[b] ? m_aCoef : m_rCoef) * (target - m_gain[b]);
        }
        // The background, the same split, each band scaled, then put back together.
        for (int ch = 0; ch < 2; ++ch) {
            float* f = ch == 0 ? farL : farR;
            float bb[Bands];
            split(m_xBg[ch], f[i], bb);
            float y = 0.0f;
            for (int b = 0; b < Bands; ++b) y += bb[b] * m_gain[b];
            f[i] = y;
        }
    }
}

// ---------------------------------------------------------------- Patina

void Patina::prepare(double sampleRate, uint64_t seed)
{
    m_sr = sampleRate;
    const int size = 1 << 12;   // 85 ms at 48 kHz: room for the wow's swing and its centre delay
    m_bufL.assign(static_cast<size_t>(size), 0.0f);
    m_bufR.assign(static_cast<size_t>(size), 0.0f);
    m_mask = size - 1;
    m_w = 0;
    m_rng.seed(seed);
    m_wowDrift.init(m_rng);
    reset();
}

void Patina::reset()
{
    std::fill(m_bufL.begin(), m_bufL.end(), 0.0f);
    std::fill(m_bufR.begin(), m_bufR.end(), 0.0f);
    m_lpL = m_lpR = 0.0f;
    m_env = 0.0f;
    m_w = 0;
    m_osL.reset(); m_osR.reset();
}

void Patina::set(float amount, float wow, float hiss, float age)
{
    m_amount = clampv(amount, 0.0f, 1.0f);
    m_wow = clampv(wow, 0.0f, 1.0f);
    m_hiss = clampv(hiss, 0.0f, 1.0f);
    m_age = clampv(age, 0.0f, 1.0f);
    // Age: the top end a worn machine and worn tape no longer carry. 20 kHz down to 4 kHz.
    const float hc = 20000.0f * std::pow(2.0f, -2.32f * m_age * m_amount);
    m_lpC = hc >= 19000.0f ? 1.0f : 1.0f - std::exp(-TwoPi * hc / static_cast<float>(m_sr));
}

void Patina::process(float* L, float* R, int n)
{
    if (m_amount <= 0.0f) return;
    const float dt = static_cast<float>(n / m_sr);
    // Wow (slow, irregular) and flutter (a steady 6 Hz), as a moving read point in the ring. Its
    // centre delay is fixed, so the effect is a wavering pitch and not a delay.
    const float wowV = m_wowDrift.update(dt, 0.35f, m_rng);
    const float centre = 0.004f * static_cast<float>(m_sr);
    const float swing = 0.0022f * static_cast<float>(m_sr) * m_wow * m_amount;
    for (int i = 0; i < n; ++i) {
        m_flutterPh += 6.0 / m_sr; if (m_flutterPh >= 1.0) m_flutterPh -= 1.0;
        const float d = centre + swing * (0.8f * wowV + 0.2f * sin01(m_flutterPh));
        m_bufL[static_cast<size_t>(m_w & m_mask)] = L[i];
        m_bufR[static_cast<size_t>(m_w & m_mask)] = R[i];
        // In double, and with the write pointer kept inside the ring: as a float the pointer
        // stopped being an integer after 2^24 samples (six minutes at 48 kHz) and the read head
        // began to jitter by one sample, then two, then four against the write position -- a
        // Nyquist buzz on the master that got worse for as long as the instrument ran.
        const double rp = static_cast<double>(m_w) - static_cast<double>(d);
        const int i0 = static_cast<int>(std::floor(rp));
        const float f = static_cast<float>(rp - static_cast<double>(i0));
        auto read = [&](const std::vector<float>& b) {
            const float a = b[static_cast<size_t>(i0 & m_mask)], c = b[static_cast<size_t>((i0 + 1) & m_mask)];
            return a + f * (c - a);
        };
        float l = read(m_bufL), r = read(m_bufR);
        m_w = (m_w + 1) & m_mask;   // stays an exact integer for ever
        // The lost top end.
        if (m_lpC < 1.0f) { m_lpL += m_lpC * (l - m_lpL); m_lpR += m_lpC * (r - m_lpR); l = m_lpL; r = m_lpR; }
        // A noise floor that lives with the music: mostly constant, a little louder when the tape
        // is being asked to carry more (that is what modulation noise is).
        if (m_hiss > 0.0f) {
            const float mag = std::fabs(0.5f * (l + r));
            m_env += (mag > m_env ? 0.01f : 0.0002f) * (mag - m_env);
            const float floorLevel = m_hiss * m_amount * 0.0016f * (1.0f + 3.0f * m_env);
            l += floorLevel * m_rng.bipolar();
            r += floorLevel * m_rng.bipolar();
        }
        // Gentle asymmetric saturation: the third harmonic a tape adds before it ever clips. At four
        // times the rate (25.09.2026), so the harmonic of a bright top end does not fold back.
        const float drive = 1.0f + 1.5f * m_amount;
        auto tape = [drive](float v) { return softClip(v * drive) / drive; };
        l = m_osL.process(l, tape);
        r = m_osR.process(r, tape);
        L[i] = l;
        R[i] = r;
    }
}

// ---------------------------------------------------------------- Reverb

void Reverb::prepare(double sampleRate)
{
    m_sr = sampleRate;
    const int need = static_cast<int>(std::max(0.08 * 3.0 * 1.1 * m_sr, 0.5 * m_sr)) + 64;
    const int size = pow2At(need);
    m_mask = size - 1;
    m_w = 0;
    for (auto& l : m_line) l.assign(static_cast<size_t>(size), 0.0f);
    for (auto& a : m_ap)   a.assign(static_cast<size_t>(size), 0.0f);
    for (auto& a : m_apR)  a.assign(static_cast<size_t>(size), 0.0f);
    m_pre.assign(static_cast<size_t>(size), 0.0f);
    m_preR.assign(static_cast<size_t>(size), 0.0f);
    const int outSize = pow2At(static_cast<int>(0.012 * m_sr) + 8);
    m_outR.assign(static_cast<size_t>(outSize), 0.0f);
    m_outMask = outSize - 1;
    m_dcR = 1.0f - TwoPi * 5.0f / static_cast<float>(m_sr);
    m_dcInX[0] = m_dcInX[1] = m_dcInY[0] = m_dcInY[1] = 0.0f;

    static const float ApMs[Allpasses] = { 5.1f, 7.3f, 11.3f, 13.7f };
    for (int k = 0; k < Allpasses; ++k) m_apLen[k] = std::max(1, static_cast<int>(ApMs[k] * m_sr / 1000.0));
    // The scattering all-passes: short, mutually prime lengths, so no two lines scatter alike.
    static const float ScMs[Lines] = { 1.9f, 2.3f, 2.9f, 3.7f, 4.3f, 5.3f, 6.1f, 7.1f };
    for (int l = 0; l < Lines; ++l) { m_sc[l].assign(static_cast<size_t>(size), 0.0f); m_scLen[l] = std::max(1, static_cast<int>(ScMs[l] * m_sr / 1000.0)); }
    static const float ModHz[Lines] = { 0.11f, 0.13f, 0.17f, 0.19f, 0.23f, 0.29f, 0.31f, 0.37f };
    for (int l = 0; l < Lines; ++l) { m_modRate[l] = ModHz[l]; m_modPh[l] = l / static_cast<double>(Lines); m_lp[l] = 0.0f; m_lenCur[l] = 0.0f; }
    // The rotating mode's angles: eight rates, none a multiple of another, staggered in phase.
    static const float RotHz[Rotations] = { 0.031f, 0.043f, 0.057f, 0.071f, 0.083f, 0.097f, 0.113f, 0.127f };
    for (int k = 0; k < Rotations; ++k) {
        const float ph = static_cast<float>(k) * 0.3927f;   // pi / 8 apart
        m_rotC[k] = std::cos(ph); m_rotS[k] = std::sin(ph);
        const float d = TwoPi * RotHz[k] / static_cast<float>(m_sr);
        m_rotDc[k] = std::cos(d); m_rotDs[k] = std::sin(d);
    }
    m_rot = 0.0f; m_rotNorm = 0;
    m_preCur = 0.0f;
    m_outDelayCur = 0.0f;
    m_hcL = m_hcR = 0.0f;
    m_lcL1 = m_lcR1 = m_lcL2 = m_lcR2 = 0.0f;
    setSpace(m_asym, 20000.0f);
    set(m_size, m_decay, m_damp, 40.0f, false, m_mix);
}

void Reverb::setSpace(float asymmetry, float highcutHz, float lowcutHz)
{
    m_asym = clampv(asymmetry, 0.0f, 1.0f);
    const float fc = clampv(highcutHz, 200.0f, 20000.0f);
    m_hcCoef = (fc >= 19000.0f) ? 1.0f : (1.0f - std::exp(-TwoPi * fc / static_cast<float>(m_sr)));
    // The knob says where the tail is 3 dB down, so the pole does not sit there: two cascaded
    // one-poles are 6 dB down at their own corner, and the pair reaches -3 dB at 1.554 times it.
    // Without this division a "400 Hz" low cut would take 6 dB out at 400 Hz, and the number on
    // the panel would be a number about the filter rather than about the sound.
    const float lf = clampv(lowcutHz, 20.0f, 1000.0f);
    m_lcCoef = (lf <= 21.0f) ? 0.0f
            : (1.0f - std::exp(-TwoPi * (lf / 1.5538f) / static_cast<float>(m_sr)));
    m_outDelayTarget = m_asym * 0.010f * static_cast<float>(m_sr);
    set(m_size, m_decay, m_damp, m_preTarget * 1000.0f / static_cast<float>(m_sr), m_freeze, m_mix);
}

void Reverb::setSendLowcut(float hz)
{
    m_sendHpOn = hz > 21.0f;
    if (m_sendHpOn) {
        m_sendHpL.setQ(clampv(hz, 20.0f, 1000.0f), 0.7071f, static_cast<float>(m_sr));   // Butterworth: -3 dB at the knob, 12 dB/oct below
        m_sendHpR.copyCoefficients(m_sendHpL);
    } else {
        m_sendHpL.reset(); m_sendHpR.reset();
    }
}

void Reverb::set(float size, float decaySeconds, float damping, float preDelayMs, bool freeze, float mix)
{
    static const float BaseMs[Lines] = { 29.7f, 37.1f, 41.1f, 43.7f, 53.3f, 61.9f, 71.3f, 79.9f };
    // Searched by Tools/optimise_fdn.py (seed 7, 60 draws) over mutually prime lengths spread
    // across the same span, scored on the spread of the simulated tail's third-octave
    // magnitudes: 0.314 dB against the classic set's 0.474 dB.
    static const float FlatMs[Lines] = { 29.7f, 31.4f, 39.3f, 46.6f, 55.1f, 58.6f, 68.4f, 89.0f };
    m_size = clampv(size, 0.5f, 3.0f);
    m_decay = std::max(decaySeconds, 0.1f);
    m_damp = clampv(damping, 0.0f, 1.0f);
    m_mix = clampv(mix, 0.0f, 1.0f);
    m_freeze = freeze;
    const float maxLen = static_cast<float>(m_mask) - 8.0f;
    for (int l = 0; l < Lines; ++l) {
        const float stretch = (l >= Lines / 2) ? (1.0f + 0.08f * m_asym) : 1.0f;   // right-hand group runs longer
        const float baseMs = (m_mode >= 2) ? FlatMs[l] : BaseMs[l];
        m_lenTarget[l] = std::min(baseMs * m_size * stretch * static_cast<float>(m_sr / 1000.0), maxLen);
        if (m_lenCur[l] <= 0.0f) m_lenCur[l] = m_lenTarget[l];
        m_gain[l] = m_freeze ? 1.0f : std::pow(10.0f, -3.0f * m_lenTarget[l] / (m_decay * static_cast<float>(m_sr)));
    }
    m_preTarget = std::min(clampv(preDelayMs, 0.0f, 500.0f) * static_cast<float>(m_sr / 1000.0), maxLen);
    if (m_preCur <= 0.0f) m_preCur = m_preTarget;
}

void Reverb::process(float* L, float* R, int n)
{
    const float lpc = m_freeze ? 1.0f : (1.0f - 0.92f * m_damp);
    const float inGain = m_freeze ? 0.0f : 0.5f;
    const float mix = m_mix;
    const float glide = 0.0005f;
    const float rotTarget = (m_mode == 3) ? 1.0f : 0.0f;
    float* outR = m_outR.data();
    for (int i = 0; i < n; ++i) {
        m_rot += (rotTarget - m_rot) * glide;
        if (m_rot < 1.0e-6f) m_rot = 0.0f;
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
        float inL = L[i] - m_dcInX[0] + m_dcR * m_dcInY[0]; m_dcInX[0] = L[i]; m_dcInY[0] = inL;
        float inR = R[i] - m_dcInX[1] + m_dcR * m_dcInY[1]; m_dcInX[1] = R[i]; m_dcInY[1] = inR;
        if (m_sendHpOn) {   // Send Low Cut: the fundamentals stay out of the tail (the dry share above is untouched)
            float lp, bp, hp;
            m_sendHpL.tick(inL, lp, bp, hp); inL = hp;
            m_sendHpR.tick(inR, lp, bp, hp); inR = hp;
        }
        m_pre[static_cast<size_t>(m_w & m_mask)] = inL;
        m_preR[static_cast<size_t>(m_w & m_mask)] = inR;
        m_preCur += (m_preTarget - m_preCur) * glide;
        float xl = ringRead(m_pre.data(), m_mask, m_w, m_preCur + 1.0f);
        float xr = ringRead(m_preR.data(), m_mask, m_w, m_preCur + 1.0f);

        for (int k = 0; k < Allpasses; ++k) {
            float* a = m_ap[k].data();
            float* ar = m_apR[k].data();
            const float d = a[(m_w - m_apLen[k]) & m_mask];
            const float dr = ar[(m_w - m_apLen[k]) & m_mask];
            const float y = d - 0.6f * xl;
            const float yr = dr - 0.6f * xr;
            a[m_w & m_mask] = xl + 0.6f * y;
            ar[m_w & m_mask] = xr + 0.6f * yr;
            xl = y;
            xr = yr;
        }

        float o[Lines];
        float sum = 0.0f;
        for (int l = 0; l < Lines; ++l) {
            m_lenCur[l] += (m_lenTarget[l] - m_lenCur[l]) * glide;
            m_modPh[l] += m_modRate[l] / m_sr;
            if (m_modPh[l] >= 1.0) m_modPh[l] -= 1.0;
            const float d = m_lenCur[l] + (1.5f * (1.0f - m_rot)) * sin01(m_modPh[l]) + 2.0f;   // the wobble fades as the matrix takes over
            float v = ringRead(m_line[l].data(), m_mask, m_w, d);
            if (m_mode != 0) {   // scattering (and colourless, which also scatters)
                float* sb = m_sc[l].data();
                const float sd = sb[(m_w - m_scLen[l]) & m_mask];
                const float sy = sd - 0.5f * v;
                sb[m_w & m_mask] = v + 0.5f * sy;
                v = sy;
            }
            m_lp[l] += lpc * (v - m_lp[l]);
            o[l] = m_lp[l];
            sum += o[l];
        }
        const float hh = sum * (2.0f / static_cast<float>(Lines));   // Householder reflection
        if (m_rot <= 0.0f) {
            for (int l = 0; l < Lines; ++l)
                m_line[l][static_cast<size_t>(m_w & m_mask)] = m_gain[l] * (o[l] - hh) + ((l & 1) ? -inGain : inGain) * (l < Lines / 2 ? xl : xr);
        } else {
            // The turning matrix: the reflection's output through two layers of Givens rotations,
            // neighbours first and then across, every angle advanced by its own tiny step per
            // sample (a rotation of a rotation, no trigonometry in the loop; the pair is pulled
            // back onto the unit circle every few thousand samples so rounding cannot walk it
            // off). Between the two matrices while the mode is switching.
            float v[Lines], u[Lines];
            for (int l = 0; l < Lines; ++l) v[l] = o[l] - hh;
            for (int k = 0; k < Rotations; ++k) {
                const float c = m_rotC[k], s = m_rotS[k];
                m_rotC[k] = c * m_rotDc[k] - s * m_rotDs[k];
                m_rotS[k] = s * m_rotDc[k] + c * m_rotDs[k];
            }
            if (++m_rotNorm >= 4096) {
                m_rotNorm = 0;
                for (int k = 0; k < Rotations; ++k) {
                    const float g = 1.0f / std::sqrt(m_rotC[k] * m_rotC[k] + m_rotS[k] * m_rotS[k]);
                    m_rotC[k] *= g; m_rotS[k] *= g;
                }
            }
            static const int Pair[Rotations][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 } };
            for (int k = 0; k < 4; ++k) {
                const int a = Pair[k][0], b = Pair[k][1];
                u[a] = m_rotC[k] * v[a] - m_rotS[k] * v[b];
                u[b] = m_rotS[k] * v[a] + m_rotC[k] * v[b];
            }
            for (int k = 4; k < Rotations; ++k) {
                const int a = Pair[k][0], b = Pair[k][1];
                const float ua = m_rotC[k] * u[a] - m_rotS[k] * u[b];
                const float ub = m_rotS[k] * u[a] + m_rotC[k] * u[b];
                u[a] = ua; u[b] = ub;
            }
            for (int l = 0; l < Lines; ++l)
                m_line[l][static_cast<size_t>(m_w & m_mask)] = m_gain[l] * (v[l] + m_rot * (u[l] - v[l])) + ((l & 1) ? -inGain : inGain) * (l < Lines / 2 ? xl : xr);
        }

        float wetL = 0.3f * (o[0] - o[1] + o[2] - o[3]);
        float wetR = 0.3f * (o[4] - o[5] + o[6] - o[7]);
        // Interaural disparity: the right output arrives a little later.
        outR[m_w & m_outMask] = wetR;
        m_outDelayCur += (m_outDelayTarget - m_outDelayCur) * glide;
        wetR = ringRead(outR, m_outMask, m_w, m_outDelayCur + 1.0f);
        // Tail darkening.
        m_hcL += m_hcCoef * (wetL - m_hcL);
        m_hcR += m_hcCoef * (wetR - m_hcR);
        // The other end of the funnel. Two one-pole high-passes, 12 dB/oct: what each low-pass
        // does not take is what passes. The tail's own value goes into `tailL/R` rather than back
        // into m_hcL/m_hcR -- those are the low-pass's state, and writing the filtered result into
        // them would feed the darkening filter its own output every sample.
        float tailL = m_hcL, tailR = m_hcR;
        if (m_lcCoef > 0.0f) {
            m_lcL1 += m_lcCoef * (tailL - m_lcL1);   const float aL = tailL - m_lcL1;
            m_lcR1 += m_lcCoef * (tailR - m_lcR1);   const float aR = tailR - m_lcR1;
            m_lcL2 += m_lcCoef * (aL - m_lcL2);      tailL = aL - m_lcL2;
            m_lcR2 += m_lcCoef * (aR - m_lcR2);      tailR = aR - m_lcR2;
        }
        L[i] = L[i] * (1.0f - mix) + tailL * mix;
        R[i] = R[i] * (1.0f - mix) + tailR * mix;
        ++m_w;
    }
}

// ---------------------------------------------------------------- MidSide

void MidSide::prepare(double sampleRate)
{
    m_sr = sampleRate;
    m_hp.reset();
    m_air.reset();
    set(150.0f, 2.0f, 1.2f);
}

void MidSide::set(float bassMonoHz, float sideAirDb, float width)
{
    m_hp.setQ(clampv(bassMonoHz, 20.0f, 400.0f), 0.707f, static_cast<float>(m_sr));
    m_air.setQ(3000.0f, 0.6f, static_cast<float>(m_sr));           // broad, gentle upper-mid bell
    m_airGain = std::pow(10.0f, clampv(sideAirDb, 0.0f, 12.0f) / 20.0f) - 1.0f;
    m_width = clampv(width, 0.0f, 2.0f);
}

void MidSide::setTilt(float dB, float pivotHz)
{
    m_tiltOn = std::fabs(dB) > 0.05f;
    if (!m_tiltOn) { m_tiltLo = m_tiltHi = 1.0f; return; }
    // Half the tilt each way, so the level through the middle stays where it was.
    m_tiltLo = std::pow(10.0f, -0.5f * dB / 20.0f);
    m_tiltHi = std::pow(10.0f,  0.5f * dB / 20.0f);
    m_tiltC = 1.0f - std::exp(-TwoPi * clampv(pivotHz, 20.0f, 0.45f * static_cast<float>(m_sr)) / static_cast<float>(m_sr));
}

void MidSide::process(float* L, float* R, int n)
{
    if (m_tiltOn)
        for (int i = 0; i < n; ++i) {
            m_tiltState[0] += m_tiltC * (L[i] - m_tiltState[0]);
            m_tiltState[1] += m_tiltC * (R[i] - m_tiltState[1]);
            L[i] = m_tiltState[0] * m_tiltLo + (L[i] - m_tiltState[0]) * m_tiltHi;
            R[i] = m_tiltState[1] * m_tiltLo + (R[i] - m_tiltState[1]) * m_tiltHi;
        }
    for (int i = 0; i < n; ++i) {
        const float m = 0.5f * (L[i] + R[i]);
        float s = 0.5f * (L[i] - R[i]);
        float lp, bp, hp;
        m_hp.tick(s, lp, bp, hp);          // everything below the crossover collapses to the centre
        s = hp;
        m_air.tick(s, lp, bp, hp);
        s += m_airGain * bp;               // lift the side's upper mids
        const float sWide = s * m_width;         // what the side would be without the guard
        s = sWide * m_guard;
        L[i] = m + s;
        R[i] = m - s;
        if (m_guardOn) {
            // Both powers on a very slow average -- about a second and a half at 48 kHz -- so a
            // single wide transient does nothing and a drone that has settled into anti-phase is
            // caught. The correction is a trim on the width, never on the mid: the centre of the
            // mix is not touched, only how far the sides are allowed to go.
            constexpr float A = 1.5e-5f;
            // Measured on the side BEFORE the guard's own trim. Watching its own output makes
            // the guard a feedback loop that hunts around the threshold; watching the mix means
            // it decides once and holds.
            m_midPow  += A * (m * m - m_midPow);
            m_sidePow += A * (sWide * sWide - m_sidePow);
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
            const float want = m_sidePow > m_midPow * 1.3f
                             ? clampv(std::sqrt(m_midPow / std::max(m_sidePow, 1e-12f)), 0.75f, 1.0f)
                             : 1.0f;
            const float step = 5.0e-7f;                      // about 2 % of the range per second
            m_guard += clampv(want - m_guard, -step, step);
            m_guard = clampv(m_guard, 0.75f, 1.0f);
        } else {
            m_guard = 1.0f;
        }
    }
}

} // namespace ambient
