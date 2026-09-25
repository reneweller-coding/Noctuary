/**
 * @file ConvolverChecks.h
 * @brief The convolution room's own checks, without an engine.
 *
 * The convolution room's own checks, without an engine: known impulses, the direct sum through all
 * three stages (stereo, mono, morph), independence from the host's block sizes, the band limits and
 * the trimmed end, the length cap, resampling and the generated hall. The selftest runs them, and
 * ambient_convtest runs them once for every vector path the convolver has (see convtest.cpp).
 *
 * Include after a CHECK(cond, msg) macro is defined.
 */
#pragma once
#include "ambient/Convolution.h"
#include "ambient/Dsp.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

/**
 * @brief Runs every check of the Convolver at 48 kHz, in six blocks.
 *
 * In order: a unit impulse comes back one latency late at unity and a second tap in a later
 * partition lands where it should; a 1.25 s random stereo impulse through all three stages matches
 * the direct sum to better than -80 dB (stereo, mono and a 0.3 morph), bit for bit whatever block
 * sizes the host sends, and so do morphs between impulses that pack differently; silent partitions
 * are not stored, the trimmed end, the length cap and a 44.1 kHz file keep their lengths; the
 * generated hall is four seconds, decays, is decorrelated and dark; an impulse over the limit is
 * faded to -60 dB at the end rather than cut; and a forgotten impulse B leaves the room as A alone.
 * Every criterion is in the CHECK message beside it; the [probe] lines print the measured numbers.
 */
inline void convolverChecks()
{
    using ambient::Convolver;
    using ambient::Rng;
    const int sr = 48000;
    {   // A unit impulse response reproduces the input one latency late; two taps give two copies.
        Convolver c; c.prepare(sr, 2.0f);
        std::vector<float> ir(4000, 0.0f); ir[0] = 1.0f;
        c.setImpulse(ir.data(), nullptr, 4000, sr);
        const int n = 4096;   // room for the second tap at 1500 + latency
        std::vector<float> inL(n, 0.0f), inR(n, 0.0f), outL(n), outR(n);
        inL[100] = 1.0f; inR[100] = 0.5f;
        c.process(inL.data(), inR.data(), outL.data(), outR.data(), n);
        int peakAt = 0; for (int i = 1; i < n; ++i) if (std::fabs(outL[static_cast<size_t>(i)]) > std::fabs(outL[static_cast<size_t>(peakAt)])) peakAt = i;
        CHECK(peakAt == 100 + c.latency(), "unit impulse comes back one latency late");
        // energy normalisation keeps a unit impulse at unity
        CHECK(std::fabs(std::fabs(outL[static_cast<size_t>(peakAt)]) - 1.0f) < 1e-3f && std::fabs(std::fabs(outR[static_cast<size_t>(peakAt)]) - 0.5f) < 1e-3f, "unit impulse passes the level and both channels");
        double other = 0; for (int i = 0; i < n; ++i) if (i != peakAt) other += outL[static_cast<size_t>(i)] * outL[static_cast<size_t>(i)];
        CHECK(other < 1e-6, "nothing but the impulse comes out");
        std::vector<float> ir2(4000, 0.0f); ir2[0] = 1.0f; ir2[1500] = 1.0f;   // two taps: second one spans partitions
        c.setImpulse(ir2.data(), nullptr, 4000, sr);
        c.reset();
        c.process(inL.data(), inR.data(), outL.data(), outR.data(), n);
        const float a = std::fabs(outL[static_cast<size_t>(100 + c.latency())]), b = std::fabs(outL[static_cast<size_t>(1600 + c.latency())]);
        CHECK(std::fabs(a - b) < 1e-3f && a > 0.5f, "a second tap in a later partition arrives at the right place");
    }
    {   // Exact against the direct sum through all three stages -- stereo, mono and a morph -- and
        // the same to the last bit whatever sizes the host's blocks come in.
        const int len = 60000;   // 1.25 s: partitions in every stage
        Rng rng; rng.seed(31);
        std::vector<float> hl(static_cast<size_t>(len)), hr(static_cast<size_t>(len));
        for (int i = 0; i < len; ++i) {
            const float env = std::exp(-4.0f * static_cast<float>(i) / static_cast<float>(len));
            hl[static_cast<size_t>(i)] = rng.bipolar() * env;
            hr[static_cast<size_t>(i)] = rng.bipolar() * env;
        }
        // The reference, normalised the way the convolver normalises (both channels together).
        auto normalised = [&](const std::vector<float>& x, const std::vector<float>* other) {
            double e = 0.0;
            for (int i = 0; i < len; ++i) {
                const double a = x[static_cast<size_t>(i)];
                const double b = other ? static_cast<double>((*other)[static_cast<size_t>(i)]) : a;
                e += 0.5 * (a * a + b * b);
            }
            std::vector<double> out(static_cast<size_t>(len));
            for (int i = 0; i < len; ++i) out[static_cast<size_t>(i)] = x[static_cast<size_t>(i)] / std::sqrt(e);
            return out;
        };
        const int n = 2 * sr;
        std::vector<float> inL(static_cast<size_t>(n), 0.0f), inR(static_cast<size_t>(n), 0.0f);
        for (int k = 0; k < 40; ++k) {
            const int at = rng.below(sr / 2);
            inL[static_cast<size_t>(at)] += rng.bipolar();
            inR[static_cast<size_t>(at)] += rng.bipolar();
        }
        auto direct = [&](const std::vector<float>& in, const std::vector<double>& h, int lat) {
            std::vector<double> y(static_cast<size_t>(n), 0.0);
            for (int t = 0; t < n; ++t)
                if (in[static_cast<size_t>(t)] != 0.0f)
                    for (int j = 0; j < len && t + j + lat < n; ++j) y[static_cast<size_t>(t + j + lat)] += in[static_cast<size_t>(t)] * h[static_cast<size_t>(j)];
            return y;
        };
        auto play = [&](Convolver& c, bool ragged, std::vector<float>& oL, std::vector<float>& oR) {
            oL.assign(static_cast<size_t>(n), 0.0f); oR.assign(static_cast<size_t>(n), 0.0f);
            Rng br; br.seed(5);
            for (int i = 0; i < n;) {
                const int m = std::min(n - i, ragged ? 1 + br.below(700) : 64);
                c.process(inL.data() + i, inR.data() + i, oL.data() + i, oR.data() + i, m);
                i += m;
            }
        };
        auto errorDb = [&](const std::vector<float>& oL, const std::vector<float>& oR, const std::vector<double>& yL, const std::vector<double>& yR) {
            double err = 0.0, ref = 0.0;
            for (int t = 0; t < n; ++t) {
                const double dl = oL[static_cast<size_t>(t)] - yL[static_cast<size_t>(t)], dr = oR[static_cast<size_t>(t)] - yR[static_cast<size_t>(t)];
                err += dl * dl + dr * dr;
                ref += yL[static_cast<size_t>(t)] * yL[static_cast<size_t>(t)] + yR[static_cast<size_t>(t)] * yR[static_cast<size_t>(t)];
            }
            return 10.0 * std::log10(err / ref + 1e-30);
        };
        Convolver c; c.prepare(sr, 2.0f);
        CHECK(c.stageStart(2) + c.stageBlock(2) < len, "the test impulse reaches into the third stage");
        c.setImpulse(hl.data(), hr.data(), len, sr);
        const auto nl = normalised(hl, &hr), nr = normalised(hr, &hl);
        const auto yL = direct(inL, nl, c.latency()), yR = direct(inR, nr, c.latency());
        std::vector<float> aL, aR, bL, bR;
        play(c, false, aL, aR);
        const double stereoDb = errorDb(aL, aR, yL, yR);
        c.reset();
        play(c, true, bL, bR);
        CHECK(aL == bL && aR == bR, "the room's output does not depend on the host's block sizes");
        Convolver m; m.prepare(sr, 2.0f);
        m.setImpulse(hl.data(), nullptr, len, sr);
        const auto nm = normalised(hl, nullptr);
        play(m, false, bL, bR);
        const double monoDb = errorDb(bL, bR, direct(inL, nm, m.latency()), direct(inR, nm, m.latency()));
        Convolver mo; mo.prepare(sr, 2.0f);
        mo.setImpulse(hl.data(), hr.data(), len, sr);
        mo.setImpulseB(hr.data(), hl.data(), len, sr);   // the channels swapped: another room of the same energy
        mo.setMorph(0.3f);
        play(mo, false, bL, bR);
        const auto zL = direct(inL, nr, mo.latency()), zR = direct(inR, nl, mo.latency());
        std::vector<double> wL(static_cast<size_t>(n)), wR(static_cast<size_t>(n));
        for (int t = 0; t < n; ++t) {
            wL[static_cast<size_t>(t)] = 0.7 * yL[static_cast<size_t>(t)] + 0.3 * zL[static_cast<size_t>(t)];
            wR[static_cast<size_t>(t)] = 0.7 * yR[static_cast<size_t>(t)] + 0.3 * zR[static_cast<size_t>(t)];
        }
        const double morphDb = errorDb(bL, bR, wL, wR);
        std::printf("  [probe] room against the direct sum: stereo %.1f dB, mono %.1f dB, morph 0.3 %.1f dB\n", stereoDb, monoDb, morphDb);
        CHECK(stereoDb < -80.0 && monoDb < -80.0 && morphDb < -80.0, "partitioned convolution matches the direct sum (stereo, mono, morph)");

        // Morphs between impulses that pack differently, so that every kernel and branch of the sum
        // runs with data that would show a wrong operand or offset: two mono impulses (blendTwo),
        // stereo against mono either way round (blendOne with one side mono), impulses of different
        // lengths (the longer one alone, weighted, where the shorter has no partition), and one whose
        // late partitions are band-limited (a range that splits where the narrower impulse stops).
        const double pi = 3.14159265358979323846;
        auto normOwn = [](const std::vector<float>& x, const std::vector<float>* other) {
            double e = 0.0;
            for (size_t i = 0; i < x.size(); ++i) {
                const double a = x[i];
                const double b = other ? static_cast<double>((*other)[i]) : a;
                e += 0.5 * (a * a + b * b);
            }
            std::vector<double> out(x.size());
            for (size_t i = 0; i < x.size(); ++i) out[i] = x[i] / std::sqrt(e);
            return out;
        };
        auto directOwn = [&](const std::vector<float>& in, const std::vector<double>& h, int lat) {
            std::vector<double> y(static_cast<size_t>(n), 0.0);
            const int hn = static_cast<int>(h.size());
            for (int t = 0; t < n; ++t)
                if (in[static_cast<size_t>(t)] != 0.0f)
                    for (int j = 0; j < hn && t + j + lat < n; ++j) y[static_cast<size_t>(t + j + lat)] += in[static_cast<size_t>(t)] * h[static_cast<size_t>(j)];
            return y;
        };
        const std::vector<float> shortL(hl.begin(), hl.begin() + 20000), shortR(hr.begin(), hr.begin() + 20000);
        // Noise over the first stage, then a Hann-windowed low tone filling each later partition
        // exactly: its spectrum falls away fast enough that the upper bins go under the band limit.
        std::vector<float> band(static_cast<size_t>(len), 0.0f);
        {
            Rng nr2; nr2.seed(77);
            for (int i = 0; i < c.stageStart(1); ++i)
                band[static_cast<size_t>(i)] = nr2.bipolar() * std::exp(-4.0f * static_cast<float>(i) / static_cast<float>(len));
            for (int s = 1; s <= 2; ++s) {
                const int stop = s == 1 ? c.stageStart(2) : len;
                for (int from = c.stageStart(s); from < stop; from += c.stageBlock(s)) {
                    const int to = std::min(stop, from + c.stageBlock(s));
                    const double hz = 150.0 + 40.0 * (from % 7), amp = 0.5 * std::exp(-4.0 * from / len);
                    for (int i = from; i < to; ++i) {
                        const double w = 0.5 - 0.5 * std::cos(2.0 * pi * (i - from) / (to - from));
                        band[static_cast<size_t>(i)] += static_cast<float>(amp * w * std::sin(2.0 * pi * hz * i / sr));
                    }
                }
            }
        }
        {
            Convolver probe; probe.prepare(sr, 2.0f);
            probe.setImpulse(band.data(), nullptr, len, sr);
            CHECK(probe.keptBins() < probe.fullBins() / 2, "the band-limited test impulse keeps far fewer bins than a full spectrum");
        }
        auto morphOf = [&](const std::vector<float>& aL, const std::vector<float>* aR, const std::vector<float>& bL_, const std::vector<float>* bR_, bool ragged) {
            Convolver mc; mc.prepare(sr, 2.0f);
            mc.setImpulse(aL.data(), aR ? aR->data() : nullptr, static_cast<int>(aL.size()), sr);
            mc.setImpulseB(bL_.data(), bR_ ? bR_->data() : nullptr, static_cast<int>(bL_.size()), sr);
            mc.setMorph(0.3f);
            std::vector<float> oL, oR;
            play(mc, false, oL, oR);
            if (ragged) {
                std::vector<float> rL, rR;
                mc.reset();
                play(mc, true, rL, rR);
                CHECK(oL == rL && oR == rR, "a morph's output does not depend on the host's block sizes either");
            }
            const auto aLn = normOwn(aL, aR), bLn = normOwn(bL_, bR_);
            const auto aRn = aR ? normOwn(*aR, &aL) : aLn, bRn = bR_ ? normOwn(*bR_, &bL_) : bLn;
            const auto yaL = directOwn(inL, aLn, mc.latency()), yaR = directOwn(inR, aRn, mc.latency());
            const auto ybL = directOwn(inL, bLn, mc.latency()), ybR = directOwn(inR, bRn, mc.latency());
            std::vector<double> eL(static_cast<size_t>(n)), eR(static_cast<size_t>(n));
            for (int t = 0; t < n; ++t) {
                eL[static_cast<size_t>(t)] = 0.7 * yaL[static_cast<size_t>(t)] + 0.3 * ybL[static_cast<size_t>(t)];
                eR[static_cast<size_t>(t)] = 0.7 * yaR[static_cast<size_t>(t)] + 0.3 * ybR[static_cast<size_t>(t)];
            }
            return errorDb(oL, oR, eL, eR);
        };
        const double monoBand    = morphOf(hl, nullptr, band, nullptr, true);        // blendTwo up to the band limit, then A alone (sumTwo, 0.7)
        const double monoShort   = morphOf(shortL, nullptr, hl, nullptr, false);     // blendTwo, then B alone (sumTwo, 0.3)
        const double stereoBand  = morphOf(hl, &hr, band, nullptr, false);           // blendOne with a mono B, then A alone (sumOne, 0.7)
        const double monoStereo  = morphOf(hl, nullptr, shortL, &shortR, false);     // blendOne with a mono A, then A alone (sumTwo, 0.7)
        const double stereoShort = morphOf(shortL, &shortR, hr, &hl, false);         // blendOne, then B alone (sumOne, 0.3)
        std::printf("  [probe] room morphs of unlike impulses: mono/band %.1f, short/long %.1f, stereo/band %.1f, mono/stereo %.1f, stereo short/long %.1f dB\n",
                    monoBand, monoShort, stereoBand, monoStereo, stereoShort);
        CHECK(monoBand < -80.0 && monoShort < -80.0 && stereoBand < -80.0 && monoStereo < -80.0 && stereoShort < -80.0,
              "morphs between impulses that pack differently match the direct sum");
    }
    {   // Silence inside an impulse is not stored and a tap after it still lands right; silence at
        // the end is trimmed; a longer file is cut at the maximum; another rate keeps its length.
        Convolver c; c.prepare(sr, 30.0f);
        const int len = sr * 2 + 12000;
        std::vector<float> ir(static_cast<size_t>(len), 0.0f);
        Rng rng; rng.seed(8);
        for (int i = 0; i < sr / 5; ++i) ir[static_cast<size_t>(i)] = rng.bipolar() * std::exp(-10.0f * static_cast<float>(i) / static_cast<float>(sr));
        ir[static_cast<size_t>(len - 1)] = 0.5f;   // one late tap after two seconds of nothing
        c.setImpulse(ir.data(), nullptr, len, sr);
        const double kept = static_cast<double>(c.keptBins()) / static_cast<double>(std::max(1LL, c.fullBins()));
        std::printf("  [probe] an impulse with two silent seconds keeps %.0f %% of its spectrum bins\n", 100.0 * kept);
        CHECK(kept < 0.5, "silent partitions are not stored");
        const int n = len + sr / 2;
        std::vector<float> inL(static_cast<size_t>(n), 0.0f), inR(static_cast<size_t>(n), 0.0f), outL(static_cast<size_t>(n)), outR(static_cast<size_t>(n));
        inL[100] = 1.0f;
        c.process(inL.data(), inR.data(), outL.data(), outR.data(), n);
        double e = 0.0;
        for (int i = 0; i < len; ++i) e += static_cast<double>(ir[static_cast<size_t>(i)]) * ir[static_cast<size_t>(i)];
        const float expect = static_cast<float>(0.5 / std::sqrt(e));
        CHECK(std::fabs(outL[static_cast<size_t>(100 + len - 1 + c.latency())] - expect) < 1e-3f * expect, "the late tap arrives where and as loud as it should");
        std::vector<float> ir2(static_cast<size_t>(sr * 3), 0.0f);
        for (int i = 0; i < sr; ++i) ir2[static_cast<size_t>(i)] = rng.bipolar();
        c.setImpulse(ir2.data(), nullptr, sr * 3, sr);
        CHECK(std::fabs(c.impulseSeconds() - 1.0f) < 0.01f, "the silence after an impulse is trimmed");
        std::vector<float> longIr(static_cast<size_t>(sr * 40));
        for (auto& v : longIr) v = 0.1f * rng.bipolar();
        c.setImpulse(longIr.data(), nullptr, sr * 40, sr);
        // Shortened to the maximum: the window's last milliseconds fall under the drop budget and go
        // with the silence at the end (about 12 ms of this one).
        CHECK(c.impulseSeconds() > 29.95f && c.impulseSeconds() < 30.001f, "a longer impulse ends at the maximum");
        std::vector<float> ir441(44100);
        for (int i = 0; i < 44100; ++i) ir441[static_cast<size_t>(i)] = rng.bipolar() * std::exp(-3.0f * static_cast<float>(i) / 44100.0f);
        c.setImpulse(ir441.data(), nullptr, 44100, 44100.0);
        CHECK(std::fabs(c.impulseSeconds() - 1.0f) < 0.01f, "an impulse recorded at 44.1 kHz keeps its length");
    }
    {   // The generated hall decays, is stereo, and is dark: its treble dies before its middle. The
        // one that stood here before had a power centroid of 8.7 kHz and a top band that rang as
        // long as its mids.
        Convolver c; c.prepare(sr, 8.0f);
        c.generateDefault(5, 4.0f);
        CHECK(c.impulseSeconds() > 3.9f && c.impulseSeconds() < 4.2f, "default hall is four seconds");
        const int n = sr * 5;
        std::vector<float> inL(n, 0.0f), inR(n, 0.0f), outL(n), outR(n);
        inL[0] = inR[0] = 1.0f;
        c.process(inL.data(), inR.data(), outL.data(), outR.data(), n);
        auto energy = [&](int from, int to) { double e = 0; for (int i = from; i < to; ++i) e += outL[static_cast<size_t>(i)] * outL[static_cast<size_t>(i)]; return e; };
        CHECK(energy(0, sr) > 10.0 * energy(2 * sr, 3 * sr) && energy(2 * sr, 3 * sr) > energy(4 * sr, 5 * sr), "hall decays");
        double dot = 0, el = 0, er = 0;
        for (int i = 0; i < sr; ++i) { dot += outL[static_cast<size_t>(i)] * outR[static_cast<size_t>(i)]; el += outL[static_cast<size_t>(i)] * outL[static_cast<size_t>(i)]; er += outR[static_cast<size_t>(i)] * outR[static_cast<size_t>(i)]; }
        CHECK(std::fabs(dot / std::sqrt(el * er + 1e-12)) < 0.3, "hall is decorrelated between the ears");
        // A first difference rises 6 dB an octave, so how much of the energy it keeps is a centroid
        // in disguise: 2 for white noise, about 0.07 for a sound centred on 2 kHz at 48 kHz.
        auto diffShare = [&](int from, int to) {
            double e = 0.0, d = 0.0;
            for (int i = from + 1; i < to; ++i) {
                const double x = outL[static_cast<size_t>(i)], p = outL[static_cast<size_t>(i - 1)];
                e += x * x; d += (x - p) * (x - p);
            }
            return d / std::max(e, 1.0e-30);
        };
        const double early = diffShare(c.latency() + sr / 5, c.latency() + 7 * sr / 10);
        const double late = diffShare(c.latency() + 3 * sr / 2, c.latency() + 2 * sr);
        CHECK(early < 0.5, "the default hall is dark");
        CHECK(late < 0.7 * early, "and its treble dies before its middle");
        std::vector<float> dl, dr, el2, er2;
        Convolver::makeDefaultImpulse(48000.0, 5, 4.0f, dl, dr);
        Convolver::makeDefaultImpulse(48000.0, 5, 4.0f, el2, er2);
        CHECK(dl.size() == static_cast<size_t>(4 * 48000) && dl == el2 && dr == er2, "the built-in hall is the same every time");
    }
    {   // An impulse longer than the Room keeps is shortened, not cut: its tail reaches -60 dB at the
        // limit instead of stopping like a gate. A room with a 40-second decay, twenty seconds of it,
        // kept at six.
        Convolver c; c.prepare(sr, 6.0f);
        Rng rng; rng.seed(9);
        const int len = sr * 20;
        std::vector<float> L(static_cast<size_t>(len)), R(static_cast<size_t>(len));
        for (int i = 0; i < len; ++i) {
            const float g = std::pow(10.0f, -3.0f * static_cast<float>(i) / (40.0f * static_cast<float>(sr)));
            L[static_cast<size_t>(i)] = g * rng.bipolar();
            R[static_cast<size_t>(i)] = g * rng.bipolar();
        }
        c.setImpulse(L.data(), R.data(), len, sr);
        const int n = sr * 7;
        std::vector<float> inL(n, 0.0f), inR(n, 0.0f), outL(n), outR(n);
        inL[0] = inR[0] = 1.0f;
        c.process(inL.data(), inR.data(), outL.data(), outR.data(), n);
        const int lat = c.latency(), w = sr / 20;
        auto level = [&](int from) {
            double e = 0.0;
            for (int i = from; i < from + w; ++i) e += outL[static_cast<size_t>(i)] * outL[static_cast<size_t>(i)] + outR[static_cast<size_t>(i)] * outR[static_cast<size_t>(i)];
            return e;
        };
        double loud = 0.0;
        for (int from = lat; from < lat + sr / 2; from += sr / 200) loud = std::max(loud, level(from));
        const double endDb = 10.0 * std::log10(level(lat + 6 * sr - 2 * w) / loud + 1e-30);
        const double midDb = 10.0 * std::log10(level(lat + 3 * sr) / loud + 1e-30);
        CHECK(endDb < -50.0, "a long impulse under the limit ends 60 dB down instead of being cut");
        CHECK(midDb > -45.0 && midDb < -15.0, "and still decays through the middle of what is kept");
    }
    {   // Impulse B forgotten: the room is A alone again.
        Convolver c; c.prepare(sr, 2.0f);
        Convolver ref; ref.prepare(sr, 2.0f);
        Rng rng; rng.seed(10);
        const int len = sr / 2;
        std::vector<float> A(static_cast<size_t>(len)), B(static_cast<size_t>(len));
        for (int i = 0; i < len; ++i) {
            const float g = std::exp(-6.0f * static_cast<float>(i) / static_cast<float>(len));
            A[static_cast<size_t>(i)] = g * rng.bipolar();
            B[static_cast<size_t>(i)] = g * rng.bipolar();
        }
        c.setImpulse(A.data(), nullptr, len, sr);
        c.setImpulseB(B.data(), nullptr, len, sr);
        CHECK(c.hasImpulseB(), "impulse B loads");
        c.clearImpulseB();
        CHECK(!c.hasImpulseB(), "and can be forgotten");
        ref.setImpulse(A.data(), nullptr, len, sr);
        const int n = sr;
        std::vector<float> inL(n, 0.0f), inR(n, 0.0f), o1L(n), o1R(n), o2L(n), o2R(n);
        inL[0] = inR[0] = 1.0f;
        c.process(inL.data(), inR.data(), o1L.data(), o1R.data(), n);
        ref.process(inL.data(), inR.data(), o2L.data(), o2R.data(), n);
        double worst = 0.0;
        for (int i = 0; i < n; ++i) worst = std::max(worst, static_cast<double>(std::fabs(o1L[static_cast<size_t>(i)] - o2L[static_cast<size_t>(i)])));
        CHECK(worst < 1.0e-6, "after which the room is A alone");
    }
}
