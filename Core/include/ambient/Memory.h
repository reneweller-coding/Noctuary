// Noctuary -- Memory: a drifting sound memory.
//
// In the spirit of SOMA's Cosmos (Vlad Kreimer, 2021): a long memory in which what was played keeps
// being recombined -- delay lines of different prime lengths sliding against each other, their
// content exchanged and turned about the stereo field, fading, or held for ever. Not a copy of it:
// the parts come from the feedback-delay-network literature, which has better answers to the
// questions such a device has to settle.
//
//   Blur    The lines feed back through a lossless matrix -- a butterfly of Givens rotations, all
//           turned by one angle -- that runs from the identity (every line its own echo) to a dense,
//           Hadamard-like mix (every echo in every line) and is orthogonal at every angle between, so
//           turning it exchanges energy between the lines and never adds or removes any (Schlecht and
//           Habets, "On lossless feedback delay networks", IEEE TSP 2017; "Time-varying feedback
//           matrices in feedback delay networks", JASA 2015). Hold at 1 therefore holds exactly,
//           whatever Blur does. Beyond a half, an all-pass inside every line scatters each echo into
//           many (Schlecht and Habets, "Scattering in feedback delay networks", IEEE/ACM TASLP 2020):
//           the separate echoes run together into a space -- which the Cosmos offers as a separate
//           algorithm, and this reaches on the same knob.
//   Drift   The read heads slide on slow asynchronous sines whose rates a Lorenz attractor bends, and
//           every line wanders across the stereo field. The slide is bound in pitch -- at most 15
//           cents at 1 -- so the pattern of the echoes changes and the notes do not warble. A sliding
//           head reads between cells, through an eight-tap windowed sinc: a four-point interpolator
//           takes about 2 dB off the top octave of a noise on every pass, and a memory is passes.
//   Hold    How long a memory lasts (4 s to -60 dB at 0, twenty minutes near 1, for ever at 1); Age
//           how much darker it grows on the way: a first-order absorption filter in every line, its
//           loss scaled by the line's length so that every line ages at the same rate per second
//           (after Jot and Chaigne 1991).
//   Renew   The Cosmos's suppressor: what arrives pushes out what is kept, the harder the louder it is.
//   Drive   A saturation in the loop with antiderivative antialiasing (Adaa.h), so a loop that runs
//           for minutes does not fill up with aliases -- and, half a sample late, a little darker on
//           every pass, as tape is. Under it, always, a limiter on every line.
//   Recall  Grains played out of the whole memory. Where the Cosmos scrubs at random, Seek lets the
//           grains prefer the stretches whose pitch classes fit what is playing now -- the scale and
//           the last seconds of input -- the selection principle of corpus-based concatenative
//           synthesis (Schwarz et al., CataRT, DAFx 2006): the memory answers the harmony.
//   Tape    Reverse and Half Speed move the tape itself: the heads keep their distance and the tape
//           runs backwards or at half speed, so what was recorded plays backwards or an octave down,
//           while what is recorded now comes back as it went in -- as on a tape machine. A change of
//           direction runs down through a standstill and up again.
//
// The memory is one pool of 90 seconds, divided evenly between the lines, so changing the number of
// lines re-divides the same material rather than emptying it. Buffers are allocated in prepare() only.
#pragma once
#include "Dsp.h"
#include "Adaa.h"
#include "Cosmos.h"
#include "GrainRing.h"
#include "Tuning.h"
#include <vector>
#include <cstdint>

namespace ambient {

class Memory {
public:
    static constexpr int kMaxLines = 8;
    static constexpr int kMaxGrains = 32;
    static constexpr int kSegBits = 12;
    static constexpr int kSeg = 1 << kSegBits;   // the stretch of tape one pitch-class fingerprint covers
    static constexpr int kSub = 64;              // control block, in samples

    void prepare(double sampleRate, uint64_t seed);
    // lines 2, 4 or 8; size: the longest line's repeat in seconds; blur and drift 0..1.
    void setShape(int lines, float sizeSeconds, float blur, float drift);
    // all 0..1
    void setDecay(float hold, float age, float renew, float drive);
    // recall and seek 0..1, the grains' length in milliseconds
    void setRecall(float recall, float seek, float grainMs);
    void setTape(bool reverse, bool half, bool freeze, bool erase);
    // What Seek listens for: the scale and the frequency of its tonic.
    void setHarmony(const FixedScale& scale, double tonicHz);
    // In place: the send in, the memory's voice out.
    void process(float* L, float* R, int n);
    // Nothing in and nothing out for longer than two passes of the longest line.
    bool quiet() const { return quietFor_ > quietAfter_; }

    // For the self test.
    double    energy() const;                    // what circulates: the lines' spans and their all-passes
    int       lineCount() const { return lines_; }
    double    lineSeconds(int k) const { return (line_[k].lenTarget + apLen_[k]) / sr_; }
    long long grainsStarted() const { return started_; }
    double    velocity() const { return v_; }
private:
    static constexpr int kTaps = 8, kPhases = 256;
    struct Line {
        double   w = 0.0;                    // the write head, in cells, in [0, cap)
        int      base = 0;                   // the cell it was in when the two pending cells were set up
        float    acc0 = 0.0f, wt0 = 0.0f;    // what the head has splatted into that cell ...
        float    acc1 = 0.0f, wt1 = 0.0f;    // ... and into the one after it
        double   lenTarget = 0.0;            // the read head's distance behind the write head, in cells
        double   lenBase = 0.0;              // the same, glided towards the target
        double   lenPrev = 0.0, lenNext = 0.0;   // at the start and at the end of the sub-block
        double   driftPh = 0.0, panPh = 0.0;
        float    driftAmp = 0.0f;            // cells, glided
        float    gain = 1.0f, absA = 0.0f, absZ = 0.0f;
        float    dcX = 0.0f, dcY = 0.0f;
        float    env = 0.0f;
        TanhAdaa sat;
        Svf      aa;                         // the slow tape's anti-alias low-pass
        float    glPrev = 0.7f, grPrev = 0.7f, glNext = 0.7f, grNext = 0.7f;
        int      seg = -1;                   // the pool segment the write head is in
    };
    void   layout(int lines);
    void   lengths();
    void   gains();
    void   stepControl(int m);
    void   tapeWrite(int k, float x, double v);
    void   spawnGrain(int offset);
    void   analyse();
    void   fingerprint(const float* x, float* chroma, float& rms);
    void   resetLines();

    std::vector<float> pool_;
    std::vector<float> sinc_;                // the reading interpolator: (kPhases + 1) rows of kTaps
    int       poolCells_ = 0, lines_ = 4, cap_ = 0;
    Line      line_[kMaxLines];
    std::vector<float> ap_;
    int       apSize_ = 0, apMask_ = 0, apW_ = 0;
    int       apLen_[kMaxLines] = {};
    double    sr_ = 48000.0;
    Rng       rng_;
    // settings
    int       linesWant_ = 4;
    float     size_ = 9.0f, blur_ = 0.25f, drift_ = 0.3f;
    float     hold_ = 0.8f, age_ = 0.3f, renew_ = 0.0f, drive_ = 0.0f;
    float     recall_ = 0.0f, seek_ = 0.5f, grainMs_ = 220.0f;
    bool      reverse_ = false, half_ = false, freeze_ = false, erase_ = false;
    // state
    double    v_ = 1.0, vPrev_ = 1.0;
    bool      gliding_ = false;
    float     angle_ = 0.0f, apCoef_ = 0.0f;
    double    stagePh_[3] = {};
    float     stageC_[3] = { 1.0f, 1.0f, 1.0f }, stageS_[3] = {};
    float     recallCur_ = 0.0f, driveCur_ = 0.0f, inGainCur_ = 1.0f, outGainCur_ = 1.0f;
    float     inEnv_ = 0.0f, envAtk_ = 0.0f, envRel_ = 0.0f, limAtk_ = 0.0f, limRel_ = 0.0f, dcR_ = 0.999f;
    double    lorenz_[3] = { 0.1, 0.0, 20.0 };
    float     chaos_[3] = {};
    bool      pendingLines_ = false;
    int       clearPos_ = -1;
    bool      cleared_ = false;
    float     sizeDone_ = -1.0f, holdDone_ = -1.0f, ageDone_ = -1.0f;
    long long quietFor_ = 0, quietAfter_ = 0;
    // recall
    RingGrain grains_[kMaxGrains];
    int       live_ = 0;
    long long started_ = 0;
    double    hazard_ = 0.0, nextHazard_ = 0.0;
    std::vector<float> chroma_;             // twelve per segment of the pool
    std::vector<float> segRms_;
    std::vector<int>   queue_;
    int       qHead_ = 0, qTail_ = 0;
    std::vector<float> fftRe_, fftIm_, window_;
    std::vector<signed char> binPc_;        // each FFT bin's pitch class, -1 outside what is listened to
    RealFft   fft_{ kSeg };
    float     scaleChroma_[12] = {}, inputChroma_[12] = {};
    float     inputLevel_ = 0.0f;
    std::vector<float> inBuf_;
    int       inPos_ = 0;
    double    harmonySig_ = -1.0;
};

} // namespace ambient
