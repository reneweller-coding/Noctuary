/**
 * @file FftChecks.h
 * @brief The real-input FFT (RealFft in Cosmos.h) against the complex one it is meant to replace.
 *
 * The real-input FFT (RealFft in Cosmos.h) against the complex one it is meant to replace, which
 * is the right oracle here: the complex transform has been in the instrument since the Cosmos was
 * built and every spectral check in the self test rests on it, so what has to be shown is that
 * the cheaper route lands in the same place -- not that the arithmetic is a Fourier transform.
 *
 * Checked at every length the instrument uses, on signals chosen to catch what the folding gets
 * wrong: an impulse (all bins equal, so a misplaced rotation shows everywhere), a single bin at
 * and around the middle (where k and n/2-k are the same bin and the pair formula degenerates),
 * noise, and a constant (all the energy in bin 0). Then the round trip, which is what the effects
 * actually do: forward, touch nothing, inverse, and the samples have to come back.
 *
 * Include after a CHECK(cond, msg) macro is defined.
 */
#pragma once
#include "ambient/Cosmos.h"
#include "ambient/Dsp.h"
#include <cmath>
#include <cstdio>
#include <vector>

/** @brief The real FFT's checks, kept out of the global namespace so that the selftest's own helpers cannot collide with them. */
namespace fftchecks {

/**
 * @brief Holds RealFft against the complex Fft at every length the effects use.
 *
 * Per length: the forward transform of seven test signals against the complex one, bin by bin over
 * the whole array (mirror included), within 4e-5 of the largest bin; the round trip back to the
 * samples within 4e-5 of the peak; the inverse of a random Hermitian spectrum against the complex
 * inverse, which must also come back real; and the in-place use the callers rely on (the input
 * array as the output array), which must be bit-identical to the out-of-place result.
 */
inline void fftChecks()
{
    // The lengths the effects use: the Nebula and the shifter at 2048, the Memory smaller, the
    // spectral source larger. 8 is there because the pair loop and the middle bin have almost no
    // room to hide at that size.
    const int sizes[] = { 8, 16, 64, 256, 1024, 2048, 4096 };
    for (int n : sizes) {
        ambient::Fft ref(n);
        ambient::RealFft rf(n);
        CHECK(rf.size() == n, "the real FFT is the length it was asked for");
        const int m = n / 2;

        // Four signals, each aimed at something the folding can get wrong.
        for (int kind = 0; kind < 4 + 3; ++kind) {
            std::vector<float> x(static_cast<size_t>(n), 0.0f);
            ambient::Rng rng; rng.seed(static_cast<uint64_t>(97 + kind * 31 + n));
            if (kind == 0) x[1] = 1.0f;                                  // an impulse, off the origin
            else if (kind == 1) for (auto& v : x) v = 0.75f;             // a constant: bin 0 alone
            else if (kind == 2) for (auto& v : x) v = rng.bipolar();     // noise
            else if (kind == 3) for (int i = 0; i < n; ++i) x[static_cast<size_t>(i)] = static_cast<float>(i) / n - 0.5f;   // a ramp: every bin, falling
            else {
                // A single frequency at the middle bin and its two neighbours, where k and m-k
                // meet: bin m/2 of the half transform is its own conjugate and has its own line
                // in both directions.
                const int bin = m / 2 + (kind - 5);                      // m/2 - 1, m/2, m/2 + 1
                for (int i = 0; i < n; ++i)
                    x[static_cast<size_t>(i)] = std::cos(static_cast<float>(ambient::kTwoPi * bin * i / n));
            }

            // What the complex transform says, on the same samples with a row of zeros beside them.
            std::vector<float> cr(x.begin(), x.end()), ci(static_cast<size_t>(n), 0.0f);
            ref.transform(cr.data(), ci.data(), false);
            // And what the real one says.
            std::vector<float> rr(static_cast<size_t>(n), 0.0f), ri(static_cast<size_t>(n), 0.0f);
            rf.forward(x.data(), rr.data(), ri.data());

            double err = 0.0, mag = 0.0;
            for (int k = 0; k < n; ++k) {          // the whole array: the mirror has to be there too
                err = std::max(err, static_cast<double>(std::fabs(rr[static_cast<size_t>(k)] - cr[static_cast<size_t>(k)])));
                err = std::max(err, static_cast<double>(std::fabs(ri[static_cast<size_t>(k)] - ci[static_cast<size_t>(k)])));
                mag = std::max(mag, static_cast<double>(std::fabs(cr[static_cast<size_t>(k)])) + std::fabs(ci[static_cast<size_t>(k)]));
            }
            CHECK(err <= 4.0e-5 * (mag + 1.0), "the real FFT gives the spectrum the complex one gives");

            // The round trip, which is what the effects do: forward, inverse, and the samples back.
            std::vector<float> back(static_cast<size_t>(n), 0.0f);
            rf.inverse(rr.data(), ri.data(), back.data());
            double rt = 0.0, peak = 0.0;
            for (int i = 0; i < n; ++i) {
                rt = std::max(rt, static_cast<double>(std::fabs(back[static_cast<size_t>(i)] - x[static_cast<size_t>(i)])));
                peak = std::max(peak, static_cast<double>(std::fabs(x[static_cast<size_t>(i)])));
            }
            CHECK(rt <= 4.0e-5 * (peak + 1.0), "and its inverse hands the samples back");
        }

        {   // The inverse against the complex one as well, on a spectrum built the way the Nebula
            // builds one -- a Hermitian half with random phases -- so the check does not only ever
            // see spectra that the forward has just made.
            ambient::Rng rng; rng.seed(static_cast<uint64_t>(555 + n));
            std::vector<float> hr(static_cast<size_t>(n), 0.0f), hi(static_cast<size_t>(n), 0.0f);
            for (int k = 0; k <= m; ++k) {
                const float a = 0.2f + rng.uniform();
                const float ph = rng.uniform();
                hr[static_cast<size_t>(k)] = a * std::cos(static_cast<float>(ambient::kTwoPi * ph));
                hi[static_cast<size_t>(k)] = a * std::sin(static_cast<float>(ambient::kTwoPi * ph));
            }
            hi[0] = 0.0f; hi[static_cast<size_t>(m)] = 0.0f;          // the two real bins
            for (int k = 1; k < m; ++k) { hr[static_cast<size_t>(n - k)] = hr[static_cast<size_t>(k)]; hi[static_cast<size_t>(n - k)] = -hi[static_cast<size_t>(k)]; }
            std::vector<float> cr(hr), ci(hi);
            ref.transform(cr.data(), ci.data(), true);                 // the complex inverse: real in cr
            std::vector<float> got(static_cast<size_t>(n), 0.0f);
            rf.inverse(hr.data(), hi.data(), got.data());
            double err = 0.0, peak = 0.0;
            for (int i = 0; i < n; ++i) {
                err = std::max(err, static_cast<double>(std::fabs(got[static_cast<size_t>(i)] - cr[static_cast<size_t>(i)])));
                peak = std::max(peak, static_cast<double>(std::fabs(cr[static_cast<size_t>(i)])));
                CHECK(std::fabs(ci[static_cast<size_t>(i)]) < 1.0e-4 * (peak + 1.0), "a Hermitian spectrum comes back real");
            }
            CHECK(err <= 4.0e-5 * (peak + 1.0), "the real inverse gives what the complex inverse gives");
        }

        {   // The aliasing the callers rely on: x may be the caller's own re array.
            ambient::Rng rng; rng.seed(static_cast<uint64_t>(7 + n));
            std::vector<float> x(static_cast<size_t>(n)), re(static_cast<size_t>(n)), im(static_cast<size_t>(n));
            for (auto& v : x) v = rng.bipolar();
            rf.forward(x.data(), re.data(), im.data());
            std::vector<float> shared(x), im2(static_cast<size_t>(n), 0.0f);
            rf.forward(shared.data(), shared.data(), im2.data());
            double err = 0.0;
            for (int k = 0; k < n; ++k) {
                err = std::max(err, static_cast<double>(std::fabs(shared[static_cast<size_t>(k)] - re[static_cast<size_t>(k)])));
                err = std::max(err, static_cast<double>(std::fabs(im2[static_cast<size_t>(k)] - im[static_cast<size_t>(k)])));
            }
            CHECK(err == 0.0, "the forward may write into the array it reads");
            // ...and so may the inverse, which is what lets an effect keep its one buffer.
            std::vector<float> out(static_cast<size_t>(n), 0.0f);
            rf.inverse(re.data(), im.data(), out.data());
            std::vector<float> reSame(re), imSame(im);
            rf.inverse(reSame.data(), imSame.data(), reSame.data());
            double ae = 0.0;
            for (int i = 0; i < n; ++i) ae = std::max(ae, static_cast<double>(std::fabs(reSame[static_cast<size_t>(i)] - out[static_cast<size_t>(i)])));
            CHECK(ae == 0.0, "and the inverse may write into the array it reads");
        }
    }
}

}   // namespace fftchecks

/** @brief The one call the selftest makes: runs fftchecks::fftChecks(). */
inline void fftChecks() { fftchecks::fftChecks(); }
