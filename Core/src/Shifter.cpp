/**
 * @file Shifter.cpp
 * @brief The spectral pitch shifter: frame analysis, peak-locked shifting and overlap-add.
 *
 * Shifter.h explains why the shimmer shifts in the spectrum rather than with two read heads; this
 * file is the phase vocoder itself. process() runs sample by sample on the audio thread: every
 * input sample goes into a kN ring, every output sample comes out of a 2 kN overlap-add buffer,
 * and every kHop samples frame() is run once per channel. frame() windows the last kN samples
 * with a Hann, takes the FFT, finds the spectral peaks (local maxima over two bins either side,
 * no more than 70 dB under the loudest), and for each peak works out its true frequency from the
 * phase advance since the previous frame. The peak and every bin of its region -- up to the trough
 * before the next peak -- are then moved by the same number of bins to the shifted frequency and
 * turned by one angle, so that the region keeps its internal phase relations (Laroche and
 * Dolson's identity phase locking) and the output bin continues the phase it had at the shifted
 * frequency in the previous frame. The shifted half-spectrum is mirrored into a real signal,
 * inverse transformed, windowed again and added into the output ring.
 *
 * Everything is allocated in prepare(); process() may run in place, since the input sample is
 * read before the output sample is written. The shift ratio is set by setSemitones() and is read
 * at the next frame.
 */
#include "ambient/Shifter.h"
#include <algorithm>
#include <cmath>

namespace ambient {

namespace {
constexpr double kTau = 6.283185307179586;   ///< 2 pi: the Hann window and every phase in frame() are in radians
/**
 * @brief Gain of the overlap-add, so that a frame passed through unchanged comes out at unity.
 *
 * Hann analysis and Hann synthesis at a quarter-frame hop add up to one and a half.
 */
constexpr float kOverlapGain = 2.0f / 3.0f;
}

void SpectralShifter::prepare(double)
{
    window_.resize(kN);
    for (int i = 0; i < kN; ++i) window_[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(kTau * i / kN));
    const size_t half = static_cast<size_t>(kN / 2 + 1);
    for (auto& c : ch_) {
        c.in.assign(kN, 0.0f);
        c.out.assign(2 * kN, 0.0f);
        c.re.assign(kN, 0.0f);
        c.im.assign(kN, 0.0f);
        c.power.assign(half, 0.0f);
        c.prevRe.assign(half, 0.0f);
        c.prevIm.assign(half, 0.0f);
        c.accRe.assign(half, 0.0f);
        c.accIm.assign(half, 0.0f);
        c.prevOutRe.assign(half, 0.0f);
        c.prevOutIm.assign(half, 0.0f);
        c.inPos = 0;
        c.outPos = 0;
    }
    peaks_.assign(static_cast<size_t>(kN / 2), 0);
    hopCounter_ = 0;
}

void SpectralShifter::reset()
{
    for (auto& c : ch_) {
        for (auto* v : { &c.in, &c.out, &c.re, &c.im, &c.power, &c.prevRe, &c.prevIm, &c.accRe, &c.accIm, &c.prevOutRe, &c.prevOutIm })
            std::fill(v->begin(), v->end(), 0.0f);
    }
    hopCounter_ = 0;
}

void SpectralShifter::setSemitones(float semitones)
{
    ratio_ = std::pow(2.0, static_cast<double>(clampv(semitones, -36.0f, 36.0f)) / 12.0);
}

void SpectralShifter::frame(Channel& c)
{
    constexpr int K = kN / 2;
    const int inMask = kN - 1, outMask = 2 * kN - 1;
    for (int i = 0; i < kN; ++i)
        c.re[static_cast<size_t>(i)] = c.in[static_cast<size_t>((c.inPos - kN + i) & inMask)] * window_[static_cast<size_t>(i)];
    fft_.forward(c.re.data(), c.re.data(), c.im.data());
    float top = 0.0f;
    for (int k = 0; k <= K; ++k) {
        const float p = c.re[static_cast<size_t>(k)] * c.re[static_cast<size_t>(k)] + c.im[static_cast<size_t>(k)] * c.im[static_cast<size_t>(k)];
        c.power[static_cast<size_t>(k)] = p;
        top = std::max(top, p);
    }
    std::fill(c.accRe.begin(), c.accRe.end(), 0.0f);
    std::fill(c.accIm.begin(), c.accIm.end(), 0.0f);
    if (top > 1.0e-18f) {
        // The peaks: local maxima over two bins either side, no more than 70 dB under the loudest.
        const float floor = top * 1.0e-7f;
        int np = 0;
        for (int k = 2; k < K - 2; ++k) {
            const float p = c.power[static_cast<size_t>(k)];
            if (p > floor && p >= c.power[static_cast<size_t>(k - 1)] && p > c.power[static_cast<size_t>(k + 1)]
                && p >= c.power[static_cast<size_t>(k - 2)] && p > c.power[static_cast<size_t>(k + 2)])
                peaks_[static_cast<size_t>(np++)] = k;
        }
        const double w0 = kTau / kN;
        int lo = 1;
        for (int j = 0; j < np; ++j) {
            const int kp = peaks_[static_cast<size_t>(j)];
            // A peak's region runs to the trough between it and the next peak.
            int hi = K - 1;
            if (j + 1 < np) {
                const int kn = peaks_[static_cast<size_t>(j + 1)];
                hi = kp;
                float least = c.power[static_cast<size_t>(kp)];
                for (int k = kp + 1; k < kn; ++k)
                    if (c.power[static_cast<size_t>(k)] < least) { least = c.power[static_cast<size_t>(k)]; hi = k; }
            }
            const int kq = static_cast<int>(std::lround(static_cast<double>(kp) * ratio_));
            if (kq >= 1 && kq < K - 1) {
                const int dk = kq - kp;
                // The peak's own frequency, from how far its phase moved since the last frame beyond
                // what the centre of its bin accounts for.
                const double ph = std::atan2(c.im[static_cast<size_t>(kp)], c.re[static_cast<size_t>(kp)]);
                const double prev = std::atan2(c.prevIm[static_cast<size_t>(kp)], c.prevRe[static_cast<size_t>(kp)]);
                double dev = ph - prev - w0 * kp * kHop;
                dev -= kTau * std::round(dev / kTau);
                const double omega = w0 * kp + dev / kHop;
                // At its new place it continues the phase that bin had at the shifted frequency, and
                // the whole region is turned by the same angle, so it moves as one.
                const double outPrev = std::atan2(c.prevOutIm[static_cast<size_t>(kq)], c.prevOutRe[static_cast<size_t>(kq)]);
                const double rot = outPrev + omega * ratio_ * kHop - ph;
                const float cr = static_cast<float>(std::cos(rot)), sr = static_cast<float>(std::sin(rot));
                for (int k = lo; k <= hi; ++k) {
                    const int q = k + dk;
                    if (q < 1 || q >= K) continue;
                    const float re = c.re[static_cast<size_t>(k)], im = c.im[static_cast<size_t>(k)];
                    c.accRe[static_cast<size_t>(q)] += re * cr - im * sr;
                    c.accIm[static_cast<size_t>(q)] += re * sr + im * cr;
                }
            }
            lo = hi + 1;
        }
    }
    // What the next frame measures its phases against.
    std::copy(c.re.begin(), c.re.begin() + K + 1, c.prevRe.begin());
    std::copy(c.im.begin(), c.im.begin() + K + 1, c.prevIm.begin());
    std::copy(c.accRe.begin(), c.accRe.end(), c.prevOutRe.begin());
    std::copy(c.accIm.begin(), c.accIm.end(), c.prevOutIm.begin());
    // Back to time: the shifted half-spectrum mirrored into a real signal, windowed and overlapped.
    std::fill(c.re.begin(), c.re.end(), 0.0f);
    std::fill(c.im.begin(), c.im.end(), 0.0f);
    for (int k = 1; k < K; ++k) {
        c.re[static_cast<size_t>(k)] = c.accRe[static_cast<size_t>(k)];
        c.im[static_cast<size_t>(k)] = c.accIm[static_cast<size_t>(k)];
        c.re[static_cast<size_t>(kN - k)] = c.accRe[static_cast<size_t>(k)];
        c.im[static_cast<size_t>(kN - k)] = -c.accIm[static_cast<size_t>(k)];
    }
    fft_.inverse(c.re.data(), c.im.data(), c.re.data());
    for (int i = 0; i < kN; ++i)
        c.out[static_cast<size_t>((c.outPos + i) & outMask)] += c.re[static_cast<size_t>(i)] * window_[static_cast<size_t>(i)] * kOverlapGain;
}

void SpectralShifter::process(const float* inL, const float* inR, float* outL, float* outR, int n)
{
    const int inMask = kN - 1, outMask = 2 * kN - 1;
    const float* ins[2] = { inL, inR };
    float* outs[2] = { outL, outR };
    for (int i = 0; i < n; ++i) {
        for (int ci = 0; ci < 2; ++ci) {
            Channel& c = ch_[ci];
            const float x = ins[ci][i];   // read before the write: in place is allowed
            c.in[static_cast<size_t>(c.inPos)] = x;
            c.inPos = (c.inPos + 1) & inMask;
            const size_t o = static_cast<size_t>(c.outPos);
            outs[ci][i] = c.out[o];
            c.out[o] = 0.0f;
            c.outPos = (c.outPos + 1) & outMask;
        }
        if (++hopCounter_ >= kHop) {
            hopCounter_ = 0;
            frame(ch_[0]);
            frame(ch_[1]);
        }
    }
}

} // namespace ambient
