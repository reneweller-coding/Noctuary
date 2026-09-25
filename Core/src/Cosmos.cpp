/**
 * @file Cosmos.cpp
 * @brief The Cosmos processors and the two transforms the instrument shares.
 *
 * Cosmos.h lists the processors; this file is their inner loops. FreqShifter runs Niemitalo's
 * Hilbert pair (the tables below) on each channel and rotates the analytic signal by a phasor, the
 * right ear three per cent slower than the left; CombResonator is a stereo feedback comb with a
 * one-pole in the loop and glided delays, the right a hair longer than the left; VowelFilter
 * re-tunes three band passes every 64 samples along a drifting path through a-e-i-o-u; PitchShifter
 * is the two-head granular shifter of the shimmer loop. Fft is the plain radix-2 complex transform
 * with precomputed twiddles and bit reversal, and RealFft the real-signal transform built on half
 * of one -- its derivation and the in-place hazards are in the header, the step-by-step index
 * bookkeeping in the bodies here. Nebula takes a Hann frame of kN every kHop samples, smooths each
 * bin's magnitude with the Smear coefficient, gives every bin a fresh random phase and overlap-adds
 * the real inverse. Everything here runs on the audio thread except the constructors and prepare(),
 * which allocate. ringRead() and sin01() come from Effects.h and Dsp.h.
 */
#include "ambient/Cosmos.h"
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
/**
 * @name Hilbert pair
 * Olli Niemitalo's 90-degree phase-splitting all-pass pair (coefficients squared).
 * Two chains of four second-order all-passes whose outputs are 90 degrees apart across the band;
 * FreqShifter::Hilbert::tick() runs both on one input and takes chain A a sample late as the real
 * part and chain B as the imaginary part.
 * @{ */
const float kHilbertA[4] = { 0.6923878f, 0.9360654322959f, 0.9882295226860f, 0.9987488452737f };   ///< chain A, the real part (one sample delayed)
const float kHilbertB[4] = { 0.4021921162426f, 0.8561710882420f, 0.9722909545651f, 0.9952884791278f };   ///< chain B, the imaginary part
/** @} */
}

// ---------------------------------------------------------------- FreqShifter

void FreqShifter::Hilbert::tick(float in, float& re, float& im)
{
    float a = in;
    for (int k = 0; k < 4; ++k) {
        const float out = kHilbertA[k] * (a + y2[k]) - x2[k];
        x2[k] = x1[k]; x1[k] = a; y2[k] = y1[k]; y1[k] = out; a = out;
    }
    float b = in;
    for (int k = 0; k < 4; ++k) {
        const float out = kHilbertB[k] * (b + y2b[k]) - x2b[k];
        x2b[k] = x1b[k]; x1b[k] = b; y2b[k] = y1b[k]; y1b[k] = out; b = out;
    }
    re = delayed;   // chain A lags chain B by one sample
    delayed = a;
    im = b;
}

void FreqShifter::prepare(double sampleRate)
{
    sr_ = sampleRate;
    hL_ = Hilbert{};
    hR_ = Hilbert{};
    ph_ = 0.0;
}

void FreqShifter::process(float* L, float* R, int n)
{
    if (mix_ <= 0.0f) return;
    for (int i = 0; i < n; ++i) {
        ph_ += shift_ / sr_;
        if (ph_ > 1.0e6 || ph_ < -1.0e6) ph_ = 0.0;
        double pl = ph_ - std::floor(ph_);
        double pr = ph_ * 0.97; pr -= std::floor(pr);   // right ear shifted 3 % less: slow cosmic beating
        double ql = pl + 0.25; if (ql >= 1.0) ql -= 1.0;
        double qr = pr + 0.25; if (qr >= 1.0) qr -= 1.0;
        float re, im;
        hL_.tick(L[i], re, im);
        const float sl = re * sin01(ql) + im * sin01(pl);   // positive shift moves the spectrum up
        hR_.tick(R[i], re, im);
        const float sr = re * sin01(qr) + im * sin01(pr);
        L[i] = L[i] * (1.0f - mix_) + sl * mix_;
        R[i] = R[i] * (1.0f - mix_) + sr * mix_;
    }
}

// ---------------------------------------------------------------- CombResonator

void CombResonator::prepare(double sampleRate)
{
    sr_ = sampleRate;
    const int size = pow2At(static_cast<int>(sr_ / 20.0) + 8);
    bufL_.assign(static_cast<size_t>(size), 0.0f);
    bufR_.assign(static_cast<size_t>(size), 0.0f);
    mask_ = size - 1;
    w_ = 0;
    lpL_ = lpR_ = 0.0f;
    dLcur_ = dRcur_ = 0.0f;
}

void CombResonator::set(float freqHz, float feedback, float mix)
{
    const float maxD = static_cast<float>(mask_) - 4.0f;
    const float f = clampv(freqHz, 20.0f, 4000.0f);
    dL_ = clampv(static_cast<float>(sr_) / f, 2.0f, maxD);
    dR_ = clampv(dL_ * 1.003f, 2.0f, maxD);
    if (dLcur_ <= 0.0f) { dLcur_ = dL_; dRcur_ = dR_; }
    fb_ = clampv(feedback, 0.0f, 0.97f);
    mix_ = clampv(mix, 0.0f, 1.0f);
}

void CombResonator::process(float* L, float* R, int n)
{
    if (mix_ <= 0.0f) { for (int i = 0; i < n; ++i) { bufL_[static_cast<size_t>(w_ & mask_)] = L[i]; bufR_[static_cast<size_t>(w_ & mask_)] = R[i]; ++w_; } return; }
    float* bl = bufL_.data(); float* br = bufR_.data();
    for (int i = 0; i < n; ++i) {
        dLcur_ += (dL_ - dLcur_) * 0.0005f;
        dRcur_ += (dR_ - dRcur_) * 0.0005f;
        const float yl = ringRead(bl, mask_, w_, dLcur_ + 1.0f);
        const float yr = ringRead(br, mask_, w_, dRcur_ + 1.0f);
        lpL_ += 0.5f * (yl - lpL_);
        lpR_ += 0.5f * (yr - lpR_);
        const float vl = L[i] + fb_ * lpL_;
        const float vr = R[i] + fb_ * lpR_;
        bl[w_ & mask_] = vl;
        br[w_ & mask_] = vr;
        const float norm = (1.0f - fb_) + 0.05f;   // resonant peak stays near unity gain
        L[i] = L[i] * (1.0f - mix_) + vl * mix_ * norm;
        R[i] = R[i] * (1.0f - mix_) + vr * mix_ * norm;
        ++w_;
    }
}

// ---------------------------------------------------------------- VowelFilter

void VowelFilter::prepare(double sampleRate, uint64_t seed)
{
    sr_ = sampleRate;
    rng_.seed(seed);
    morph_.init(rng_);
    for (auto& f : fL_) f.reset();
    for (auto& f : fR_) f.reset();
    counter_ = 0;
}

void VowelFilter::process(float* L, float* R, int n)
{
    static const float kFormants[5][3] = {
        { 800.0f, 1150.0f, 2900.0f },   // a
        { 400.0f, 1600.0f, 2700.0f },   // e
        { 350.0f, 1700.0f, 2700.0f },   // i
        { 450.0f,  800.0f, 2830.0f },   // o
        { 325.0f,  700.0f, 2530.0f },   // u
    };
    static const float kGain[3] = { 1.0f, 0.5f, 0.25f };
    if (mix_ <= 0.0f) return;
    for (int i = 0; i < n; ++i) {
        if (counter_-- <= 0) {
            counter_ = 63;
            const float m = 2.0f + 2.0f * morph_.update(64.0f / static_cast<float>(sr_), rate_, rng_);
            const int i0 = clampv(static_cast<int>(m), 0, 3);
            const float t = clampv(m - static_cast<float>(i0), 0.0f, 1.0f);
            for (int k = 0; k < 3; ++k) {
                const float f = std::pow(kFormants[i0][k], 1.0f - t) * std::pow(kFormants[i0 + 1][k], t);
                fL_[k].setQ(f, 8.0f, static_cast<float>(sr_));
                fR_[k].setQ(f * 1.01f, 8.0f, static_cast<float>(sr_));
            }
        }
        float wl = 0.0f, wr = 0.0f, lp, bp, hp;
        for (int k = 0; k < 3; ++k) {
            fL_[k].tick(L[i], lp, bp, hp); wl += bp * kGain[k];
            fR_[k].tick(R[i], lp, bp, hp); wr += bp * kGain[k];
        }
        L[i] = L[i] * (1.0f - mix_) + wl * mix_ * 0.8f;
        R[i] = R[i] * (1.0f - mix_) + wr * mix_ * 0.8f;
    }
}

// ---------------------------------------------------------------- PitchShifter

void PitchShifter::prepare(double sampleRate)
{
    sr_ = sampleRate;
    const int size = pow2At(static_cast<int>(0.2 * sr_) + 8);
    buf_.assign(static_cast<size_t>(size), 0.0f);
    mask_ = size - 1;
    w_ = 0;
    window_ = 0.08f * static_cast<float>(sr_);
    pos_ = 0.0f;
}

void PitchShifter::setSemitones(float st) { rate_ = std::pow(2.0f, st / 12.0f); }

void PitchShifter::process(const float* in, float* out, int n)
{
    float* b = buf_.data();
    const float step = (rate_ - 1.0f) / window_;
    for (int i = 0; i < n; ++i) {
        b[w_ & mask_] = in[i];
        pos_ += step;
        pos_ -= std::floor(pos_);
        float p2 = pos_ + 0.5f; if (p2 >= 1.0f) p2 -= 1.0f;
        const float d1 = (1.0f - pos_) * window_ + 1.0f;
        const float d2 = (1.0f - p2) * window_ + 1.0f;
        const float g1 = sin01(0.5 * pos_);   // sin(pi * p)
        const float g2 = sin01(0.5 * p2);
        out[i] = ringRead(b, mask_, w_, d1) * g1 + ringRead(b, mask_, w_, d2) * g2;
        ++w_;
    }
}

// ---------------------------------------------------------------- Fft

Fft::Fft(int n) : n_(n)
{
    cos_.resize(static_cast<size_t>(n / 2));
    sin_.resize(static_cast<size_t>(n / 2));
    for (int i = 0; i < n / 2; ++i) { cos_[static_cast<size_t>(i)] = std::cos(kTwoPi * i / n); sin_[static_cast<size_t>(i)] = std::sin(kTwoPi * i / n); }
    rev_.resize(static_cast<size_t>(n));
    int bits = 0; while ((1 << bits) < n) ++bits;
    for (int i = 0; i < n; ++i) { int r = 0; for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b); rev_[static_cast<size_t>(i)] = r; }
}

void Fft::transform(float* re, float* im, bool inverse) const
{
    for (int i = 0; i < n_; ++i) {
        const int j = rev_[static_cast<size_t>(i)];
        if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= n_; len <<= 1) {
        const int half = len / 2, stride = n_ / len;
        for (int start = 0; start < n_; start += len) {
            for (int k = 0; k < half; ++k) {
                const float c = cos_[static_cast<size_t>(k * stride)];
                const float s = inverse ? sin_[static_cast<size_t>(k * stride)] : -sin_[static_cast<size_t>(k * stride)];
                const int a = start + k, b = a + half;
                const float tr = re[b] * c - im[b] * s;
                const float ti = re[b] * s + im[b] * c;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
            }
        }
    }
    if (inverse) { const float inv = 1.0f / static_cast<float>(n_); for (int i = 0; i < n_; ++i) { re[i] *= inv; im[i] *= inv; } }
}

// ---------------------------------------------------------------- RealFft

RealFft::RealFft(int n) : n_(n), half_(n / 2)
{
    const int m = n / 2;
    tc_.resize(static_cast<size_t>(m + 1));
    ts_.resize(static_cast<size_t>(m + 1));
    for (int k = 0; k <= m; ++k) {
        tc_[static_cast<size_t>(k)] = static_cast<float>(std::cos(kTwoPi * k / n));
        ts_[static_cast<size_t>(k)] = static_cast<float>(std::sin(kTwoPi * k / n));
    }
    work_.assign(static_cast<size_t>(m), 0.0f);
}

void RealFft::forward(const float* x, float* re, float* im) const
{
    const int m = n_ / 2;
    // The pairs, read forward so that writing re[k] cannot reach a sample not yet taken: it wants
    // x[2k] and x[2k+1], and 2k >= k.
    for (int k = 0; k < m; ++k) {
        const float a = x[2 * k], b = x[2 * k + 1];
        re[k] = a; im[k] = b;
    }
    half_.transform(re, im, false);
    // Apart again. k and m-k are done together because each needs the other's value, so the loop
    // runs to m/2 and the two ends are written in one step; the middle bin (k = m/2, where k and
    // m-k are the same bin) and the two ends fall out of the same formula.
    const float z0r = re[0], z0i = im[0];
    for (int k = 1; k <= m / 2; ++k) {
        const int j = m - k;
        const float akr = re[k], aki = im[k], bkr = re[j], bki = im[j];
        // Fe = (Z[k] + conj(Z[m-k])) / 2,  Fo = -i (Z[k] - conj(Z[m-k])) / 2
        const float er = 0.5f * (akr + bkr), ei = 0.5f * (aki - bki);
        const float or_ = 0.5f * (aki + bki), oi = -0.5f * (akr - bkr);
        const float wc = tc_[static_cast<size_t>(k)], ws = -ts_[static_cast<size_t>(k)];   // e^(-2 pi i k / n)
        // X[k] = Fe + W * Fo
        const float xr = er + (or_ * wc - oi * ws), xi = ei + (or_ * ws + oi * wc);
        // X[m-k] is the same with W at m-k, which is the conjugate of the rotation at k reflected:
        // e^(-2 pi i (m-k) / n) = -conj(e^(-2 pi i k / n)). Fe and Fo at m-k are the conjugates.
        const float er2 = er, ei2 = -ei, or2 = or_, oi2 = -oi;
        const float wc2 = -wc, ws2 = ws;
        const float yr = er2 + (or2 * wc2 - oi2 * ws2), yi = ei2 + (or2 * ws2 + oi2 * wc2);
        re[k] = xr; im[k] = xi;
        re[j] = yr; im[j] = yi;
    }
    // The ends: X[0] and X[n/2] are both real, and both come out of Z[0] alone.
    re[0] = z0r + z0i; im[0] = 0.0f;
    re[m] = z0r - z0i; im[m] = 0.0f;
    // The conjugate mirror, so the array reads like the one Fft leaves behind.
    for (int k = 1; k < m; ++k) { re[n_ - k] = re[k]; im[n_ - k] = -im[k]; }
}

void RealFft::inverse(const float* re, const float* im, float* x, float* scratch) const
{
    const int m = n_ / 2;
    // Z's real parts go into x[0 .. m-1] and its imaginary parts into the scratch half, which is
    // what lets x be the caller's own `re` array: every write at k or m-k lands on a bin this
    // step has already read.
    float* zi = scratch;
    const float x0 = re[0], xm = re[m];
    for (int k = 1; k < m - k; ++k) {
        const int j = m - k;
        const float akr = re[k], aki = im[k], bkr = re[j], bki = im[j];
        // Fe = (X[k] + conj(X[m-k])) / 2
        const float er = 0.5f * (akr + bkr), ei = 0.5f * (aki - bki);
        // Fo = (X[k] - conj(X[m-k])) / 2 * e^(+2 pi i k / n), undoing the rotation the forward put on
        const float dr = 0.5f * (akr - bkr), di = 0.5f * (aki + bki);
        const float wc = tc_[static_cast<size_t>(k)], ws = ts_[static_cast<size_t>(k)];
        const float fr = dr * wc - di * ws, fi = dr * ws + di * wc;
        // Z[k] = Fe + i Fo,  Z[m-k] = conj(Fe) + i conj(Fo)
        x[k] = er - fi;   zi[k] = ei + fr;
        x[j] = er + fi;   zi[j] = fr - ei;
    }
    // Z[0], out of the two bins that are real: X[0] = Re Z0 + Im Z0, X[n/2] = Re Z0 - Im Z0.
    x[0] = 0.5f * (x0 + xm); zi[0] = 0.5f * (x0 - xm);
    // The middle bin, where k and m-k are the same one and the pair above would write it twice:
    // the forward leaves X[m/2] = conj(Z[m/2]), so this is its own inverse.
    if (m > 1 && (m & 1) == 0) {
        const int k = m / 2;
        x[k] = re[k]; zi[k] = -im[k];
    }
    half_.transform(x, zi, true);
    // z[k] -> x[2k], x[2k+1], backward: the write at 2k is past every x[k'] with k' < k still to
    // be read, and the imaginary parts come from the scratch, which nothing here writes.
    for (int k = m - 1; k >= 0; --k) {
        const float a = x[k];
        x[2 * k] = a; x[2 * k + 1] = zi[k];
    }
}

// ---------------------------------------------------------------- Nebula

void Nebula::prepare(double, uint64_t seed)
{
    window_.resize(kN);
    for (int i = 0; i < kN; ++i) window_[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(kTwoPi * i / kN);
    Rng r; r.seed(seed);
    for (auto& c : ch_) {
        c.in.assign(kN, 0.0f);
        c.out.assign(2 * kN, 0.0f);
        c.mag.assign(kN / 2 + 1, 0.0f);
        c.re.assign(kN, 0.0f);
        c.im.assign(kN, 0.0f);
        c.inPos = 0; c.outPos = 0;
        c.rng.seed(r.fork());
    }
    hopCounter_ = 0;
}

void Nebula::set(float smear)
{
    const float s = clampv(smear, 0.0f, 1.0f);
    // The floor matters. Without it, Smear = 1 gives alpha = 0 exactly, the magnitude smoother
    // never updates, every bin stays at the zero it started from, and the Nebula falls silent --
    // so the top of the knob was not "the smoothest setting" but "off", with nothing to say so.
    // At 2e-4 and this hop rate the time constant is minutes, which is what the top of a smear
    // control should be. Found by rendering the preset bank: one preset out of 257 sat exactly
    // on that corner and measured as the carrier, untouched.
    alpha_ = std::max((1.0f - s) * (1.0f - s), 2.0e-4f);
}

void Nebula::frame(Channel& c)
{
    const int inMask = kN - 1, outMask = 2 * kN - 1;
    // The windowed frame, written into the buffer the spectrum will take its place in: the real
    // transform reads and writes the same array (13.09.2026 -- this was a complex transform on a
    // signal whose imaginary half was a row of zeros, and the row had to be written first).
    for (int i = 0; i < kN; ++i)
        c.re[static_cast<size_t>(i)] = c.in[static_cast<size_t>((c.inPos - kN + i) & inMask)] * window_[static_cast<size_t>(i)];
    fft_.forward(c.re.data(), c.re.data(), c.im.data());
    for (int k = 0; k <= kN / 2; ++k) {
        const float m = std::sqrt(c.re[static_cast<size_t>(k)] * c.re[static_cast<size_t>(k)] + c.im[static_cast<size_t>(k)] * c.im[static_cast<size_t>(k)]);
        c.mag[static_cast<size_t>(k)] += alpha_ * (m - c.mag[static_cast<size_t>(k)]);
    }
    for (int k = 0; k <= kN / 2; ++k) {
        const float ph = c.rng.uniform();
        const float m = c.mag[static_cast<size_t>(k)];
        const float r = m * sin01(ph + 0.25 >= 1.0 ? ph - 0.75 : ph + 0.25), q = m * sin01(ph);
        c.re[static_cast<size_t>(k)] = r; c.im[static_cast<size_t>(k)] = q;
        if (k > 0 && k < kN / 2) { c.re[static_cast<size_t>(kN - k)] = r; c.im[static_cast<size_t>(kN - k)] = -q; }
    }
    c.im[0] = 0.0f; c.im[kN / 2] = 0.0f;
    // The spectrum above is built conjugate symmetric on purpose, so the way back is the real
    // inverse; the mirror it writes above kN/2 is read by nothing and could be left out.
    fft_.inverse(c.re.data(), c.im.data(), c.re.data());
    for (int i = 0; i < kN; ++i)
        c.out[static_cast<size_t>((c.outPos + i) & outMask)] += c.re[static_cast<size_t>(i)] * window_[static_cast<size_t>(i)] * 1.3f;   // measured: unity level for random-phase resynthesis
}

void Nebula::process(const float* inL, const float* inR, float* wetL, float* wetR, int n)
{
    const int inMask = kN - 1, outMask = 2 * kN - 1;
    const float* ins[2] = { inL, inR };
    float* wets[2] = { wetL, wetR };
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < 2; ++c) {
            Channel& ch = ch_[c];
            ch.in[static_cast<size_t>(ch.inPos & inMask)] = ins[c][i];
            ++ch.inPos;
            const size_t o = static_cast<size_t>(ch.outPos & outMask);
            wets[c][i] = ch.out[o];
            ch.out[o] = 0.0f;
            ++ch.outPos;
        }
        if (++hopCounter_ >= kHop) {
            hopCounter_ = 0;
            frame(ch_[0]);
            frame(ch_[1]);
        }
    }
}

} // namespace ambient
