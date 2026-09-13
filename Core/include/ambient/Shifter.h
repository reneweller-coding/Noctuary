// Noctuary -- a pitch shifter in the spectrum, for loops.
//
// A pitch shifter inside a feedback loop hears its own output again on every pass, and whatever it
// does to a sound once it does a hundred times. The granular shifter the shimmer used -- two read
// heads crossfading every 80 ms -- lays an amplitude modulation and a comb on every pass, and a
// shimmer grew grainier and hollower the higher it rose. This one shifts in the spectrum instead,
// after Laroche and Dolson ("New phase-vocoder techniques for pitch-shifting, harmonizing and other
// exotic effects", IEEE WASPAA 1999): every spectral peak is moved to its shifted frequency together
// with the bins around it, its phase advanced from one frame to the next at the shifted frequency,
// and the bins of its region turned with it (the identity phase locking of Laroche and Dolson,
// "Improved phase vocoder time-scale modification of audio", IEEE TSAP 1999). A steady partial comes
// out as a steady partial -- no crossfade, no modulation -- and so does its hundredth pass.
//
// kN samples late, which a loop that circles for seconds does not notice. Buffers are allocated in
// prepare() only; process() may run in place.
#pragma once
#include "Dsp.h"
#include "Cosmos.h"
#include <vector>

namespace ambient {

class SpectralShifter {
public:
    static constexpr int kN = 4096, kHop = 1024;
    void prepare(double sampleRate);
    void reset();
    // The shift in semitones, from the next frame on.
    void setSemitones(float semitones);
    void process(const float* inL, const float* inR, float* outL, float* outR, int n);
    int  latency() const { return kN; }
private:
    struct Channel {
        std::vector<float> in, out;                    // the input ring (kN) and the overlap-add (2 kN)
        std::vector<float> re, im;                     // the frame being worked on (kN)
        std::vector<float> power;                      // |X|^2 of the analysis, kN / 2 + 1
        std::vector<float> prevRe, prevIm;             // the previous frame's analysis spectrum
        std::vector<float> accRe, accIm;               // this frame's shifted spectrum
        std::vector<float> prevOutRe, prevOutIm;       // and the previous frame's
        int inPos = 0, outPos = 0;
    };
    void frame(Channel& c);
    Channel ch_[2];
    RealFft fft_{ kN };
    std::vector<float> window_;
    std::vector<int>   peaks_;
    double  ratio_ = 2.0;
    int     hopCounter_ = 0;
};

} // namespace ambient
