// Noctuary -- the Cosmos path: science-fiction / deep-space processing.
//   FreqShifter    single-sideband frequency shifter (Hilbert pair), inharmonic, alien
//   CombResonator  tuned comb filters with feedback (metallic, hull-like resonances)
//   VowelFilter    three formants morphing slowly between vowels (alien choir)
//   Nebula         STFT magnitude smearing with random phases (Paulstretch-like
//                  texture; smear = 1 freezes the spectrum)
//   PitchShifter   granular two-head shifter, used for the shimmer feedback loop
// Buffers are allocated in prepare() only.
#pragma once
#include "Dsp.h"
#include <vector>

namespace ambient {

class FreqShifter {
public:
    void prepare(double sampleRate);
    void set(float shiftHz, float mix) { shift_ = shiftHz; mix_ = clampv(mix, 0.0f, 1.0f); }
    void process(float* L, float* R, int n);
private:
    struct Hilbert {
        float x1[4] = {}, x2[4] = {}, y1[4] = {}, y2[4] = {};
        float x1b[4] = {}, x2b[4] = {}, y1b[4] = {}, y2b[4] = {};
        float delayed = 0.0f;
        void tick(float in, float& re, float& im);
    };
    Hilbert hL_, hR_;
    double sr_ = 48000.0, ph_ = 0.0;
    float  shift_ = 0.0f, mix_ = 0.0f;
};

class CombResonator {
public:
    void prepare(double sampleRate);
    void set(float freqHz, float feedback, float mix);
    void process(float* L, float* R, int n);
private:
    std::vector<float> bufL_, bufR_;
    int    mask_ = 0, w_ = 0;
    double sr_ = 48000.0;
    float  dL_ = 100.0f, dR_ = 100.0f, dLcur_ = 0.0f, dRcur_ = 0.0f;
    float  fb_ = 0.8f, mix_ = 0.0f, lpL_ = 0.0f, lpR_ = 0.0f;
};

class VowelFilter {
public:
    void prepare(double sampleRate, uint64_t seed);
    void set(float mix, float rateHz) { mix_ = clampv(mix, 0.0f, 1.0f); rate_ = rateHz; }
    void process(float* L, float* R, int n);
private:
    Svf     fL_[3], fR_[3];
    Drifter morph_;
    Rng     rng_;
    double  sr_ = 48000.0;
    float   mix_ = 0.0f, rate_ = 0.05f;
    int     counter_ = 0;
};

class PitchShifter {
public:
    void prepare(double sampleRate);
    void setSemitones(float st);
    void process(const float* in, float* out, int n);
private:
    std::vector<float> buf_;
    int    mask_ = 0, w_ = 0;
    double sr_ = 48000.0;
    float  window_ = 3840.0f, rate_ = 1.0f, pos_ = 0.0f;
};

class Fft {
public:
    explicit Fft(int n = 2048);
    void transform(float* re, float* im, bool inverse) const;
    int size() const { return n_; }
private:
    int n_;
    std::vector<float> cos_, sin_;
    std::vector<int>   rev_;
};

// The same spectrum for half the work, when the signal is real -- which in this instrument it
// always is (13.09.2026). VTune on the dearest preset of the library put Fft::transform at 2.16
// seconds of processor against 0.32 for the next thing on the list, and every one of its callers
// had just written a row of zeros into the imaginary half: the Nebula, the shifter, the Memory,
// the spectral source. Going back the other way they all build a spectrum that is conjugate
// symmetric on purpose, so that direction was paying twice as well.
//
// The method is the textbook one. N real samples are read as N/2 COMPLEX ones by taking them in
// pairs, z[k] = x[2k] + i x[2k+1]; one complex transform of half the length then holds the even
// and the odd samples' spectra folded together, and they come apart again with one rotation per
// bin: X[k] = (Z[k] + conj(Z[M-k]))/2 - i (Z[k] - conj(Z[M-k]))/2 * e^(-2 pi i k / N). The
// inverse undoes exactly that and hands the pairs back.
//
// `x` may be the same array as `re` -- every write lands on a bin that step has already read --
// which is what lets a caller keep the one buffer it had.
//
// The inverse keeps a scratch half, so an instance is one thread's (Fft itself is stateless and
// may be shared; this is not). It is there because the last step of the inverse interleaves two
// half-length arrays into one, and that shuffle cannot be done in place by walking it either way:
// writing the pair for index k lands on m + (2k - m), which is the imaginary part index 2k - m
// still has to read. Measured the hard way -- the round trip came back 1, 5, 3, 7, 5, 7, 7, 8 for
// 1 to 8, every odd sample taken from two places further on.
class RealFft {
public:
    explicit RealFft(int n = 2048);
    int size() const { return n_; }
    // x[0 .. n-1] real in; re/im[0 .. n/2] out. The bins above n/2 are filled with the conjugate
    // mirror as well, so a caller that walks the whole array reads what Fft would have left there.
    void forward(const float* x, float* re, float* im) const;
    // re/im[0 .. n/2] in (whatever is above n/2 is ignored); x[0 .. n-1] real out. The scaling is
    // Fft's: the round trip returns what went in.
    void inverse(const float* re, const float* im, float* x) const { inverse(re, im, x, work_.data()); }
    // The same with the scratch half handed in -- n/2 floats, which may be the `im` array itself,
    // since every write there lands on a bin the step has read. An instance shared between voices
    // (the spectral source keeps one per size for all of them) has to take this one: the other
    // writes into the instance, and a shared instance that writes is a race waiting for the day
    // somebody renders two voices at once.
    void inverse(const float* re, const float* im, float* x, float* scratch) const;
private:
    int n_;
    Fft half_;                     // the complex transform of n/2 that does the work
    std::vector<float> tc_, ts_;   // e^(-2 pi i k / n), k = 0 .. n/2
    mutable std::vector<float> work_;   // n/2, the inverse's imaginary half; sized in the constructor
};

class Nebula {
public:
    static constexpr int kN = 2048, kHop = 512;
    void prepare(double sampleRate, uint64_t seed);
    void set(float smear);              // 0 = follows the input, 1 = frozen
    // Wet output only (latency kN samples).
    void process(const float* inL, const float* inR, float* wetL, float* wetR, int n);
private:
    struct Channel {
        std::vector<float> in, out, mag, re, im;
        int inPos = 0, outPos = 0;
        Rng rng;
    };
    void frame(Channel& c);
    Channel ch_[2];
    RealFft fft_{ kN };
    std::vector<float> window_;
    int     hopCounter_ = 0;
    float   alpha_ = 1.0f;
};

} // namespace ambient
