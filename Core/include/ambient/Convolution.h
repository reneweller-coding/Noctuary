// Noctuary -- convolution reverb ("Room"): a third, optional reverb on the far plane that
// plays a real (or generated) impulse response of up to a minute.
//
// Partitioned convolution with partitions that grow along the impulse (Battenberg and Avizienis
// 2011; Wefers 2015). The impulse is cut into three stretches -- its first 85 ms, up to 0.68 s, and
// the rest -- and each stretch into partitions of its own size: 256, 2048 and 16384 samples at
// 48 kHz. A stage cuts the input into blocks of its partition size; each block's spectrum enters
// the stage's frequency-domain delay line, and every output block is the sum over the partitions
// of (delayed input spectrum x partition spectrum), then one inverse transform. That is exact for
// any size: a block and a partition of B samples convolve to 2B - 1, which the 2B transform holds.
//
// Latency is the first stage's block, 256 samples. A later stage's answer is due D + 256 - B
// samples after its input block is complete (D is where its stretch starts), and every stage keeps
// D >= 2B - 512, so its work can be spread over the B samples until its next block in equal slices,
// one at every 256-sample step: the audio thread never pays for a 16384-sample partition at once.
// Per second a stage costs (stretch length x rate / B) products plus four transforms per block --
// the long partitions of the tail are the cheap ones, which is what makes a minute affordable.
//
// Per partition only the bins that matter are kept: the late partitions of a real room carry
// almost nothing at the top, and all that is left out, together with the silence at the end, is
// less than -100 dB of the impulse's energy. A mono impulse is read once for both channels.
//
// Two impulses, A and B, share the delay lines: the morph blends their spectra inside the sum,
// which is exactly a crossfade between two convolutions at the cost of one; at 0 or 1 only one
// impulse is read at all. Impulses are double-buffered like textures: the message thread writes
// the copy the audio thread is not reading, then swaps. Without a file a generated hall is loaded
// at prepare(), so the Room works out of the box (see makeDefaultImpulse).
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

namespace ambient {

class Convolver {
public:
    static constexpr int kStages = 3;

    Convolver();
    // maxSeconds bounds the memory (and CPU) an impulse may take. A longer file is shortened, not
    // cut: an exponential window brings its tail to -60 dB at the limit and a 50 ms fade ends it,
    // so the room gets smaller instead of stopping like a gate. A minute holds about 46 MB of delay line.
    void prepare(double sampleRate, float maxSeconds = 60.0f);
    void reset();
    // Message thread. R may be nullptr (mono impulse). Resampled to the engine rate with a
    // windowed sinc, energy-normalised, the silence at the end trimmed.
    void setImpulse(const float* L, const float* R, int n, double impulseSampleRate)  { load(0, L, R, n, impulseSampleRate); }
    // The second impulse, the one the morph goes to.
    void setImpulseB(const float* L, const float* R, int n, double impulseSampleRate) { load(1, L, R, n, impulseSampleRate); }
    // Forget impulse B: the room is A alone again, and B's memory goes back. Message thread.
    void clearImpulseB();
    // The built-in hall (makeDefaultImpulse), for a Room without a file.
    void generateDefault(uint64_t seed, float seconds = 4.0f);
    // The built-in hall as samples, at any rate, for generateDefault and for whoever wants to
    // measure it: the statistical late field the generated library's halls are made of (Gaussian
    // noise, every frequency decaying at its own T60), with a colour and a decay solved offline.
    static void makeDefaultImpulse(double sampleRate, uint64_t seed, float seconds, std::vector<float>& L, std::vector<float>& R);
    bool  hasImpulse() const  { return active_[0].load(std::memory_order_acquire) >= 0; }
    bool  hasImpulseB() const { return active_[1].load(std::memory_order_acquire) >= 0; }
    float impulseSeconds() const;   // impulse A, as kept
    int   latency() const { return head_; }
    // How long the output can still carry sound after the input has fallen silent.
    long  tailSamples() const;
    // 0 plays impulse A, 1 impulse B. Audio thread, before process(); each stage takes the value
    // at the start of its own blocks.
    void  setMorph(float x) { morph_ = x; }
    // Wet only, replaces outL/outR (which may be the inputs). Any n. Audio thread; never allocates.
    void  process(const float* inL, const float* inR, float* outL, float* outR, int n);

    // For the tests.
    int   stageBlock(int s) const { return stage_[s].block; }
    int   stageStart(int s) const { return stage_[s].start; }
    long long keptBins() const;   // impulse A: spectrum bins stored after the band limits ...
    long long fullBins() const;   // ... and what storing all of them would have taken

private:
    // A radix-2 transform that can stop after any block of butterflies and carry on at the next
    // step. Level h (half the span) keeps its rotations together: cs[h + k] = cos(pi k / h).
    struct StepFft {
        int n = 0;
        long long work = 0;                  // what one whole transform counts in the schedule
        std::vector<int>   rev;
        std::vector<float> cs, snF, snI;     // sine negated for the forward transform, as is for the inverse
        void init(int size);
    };
    struct FftRun { int half = 0, pos = 0; };   // half 0: the bit reversal, at element pos
    static bool runFft(const StepFft& f, FftRun& r, float* re, float* im, bool inverse, long long& budget);

    struct Spectra {                          // one impulse's partitions in one stage
        std::vector<float> re[2], im[2];      // packed: partition p's first cnt[p] bins start at off[p]
        std::vector<int>   off, cnt;
        int parts = 0;
    };
    struct Impulse {
        Spectra   st[kStages];
        double    seconds = 0.0;
        long      samples = 0;
        bool      stereo = false;
        long long kept = 0, full = 0;
    };
    struct Stage {
        int  block = 0, start = 0, capParts = 0, slot = 0;   // slot: bins per delay-line entry, block + 8
        StepFft fft;
        std::vector<float> fdlRe[2], fdlIm[2];               // capParts x slot per channel
        int  fdlHead = 0, fdlFilled = 0;
        std::vector<float> re[2], im[2];     // 2 x block: the input block's transform, then the sum and its inverse
        // The block in progress.
        int  phase = 0, ch = 0, part = 0, bin = 0, parts = 0;
        FftRun run;
        long long write = 0, due = 0, work = 0;
        float wa = 1.0f, wb = 0.0f;
    };

    void load(int which, const float* L, const float* R, int n, double rate);
    void analyse(Impulse& imp, const std::vector<float>* ch, bool stereo, int len);
    void step();
    void begin(Stage& s);
    long long advance(Stage& s, int g, long long quota, const Impulse* A, const Impulse* B);
    void sum(Stage& s, int g, const Impulse* A, const Impulse* B, long long& quota);
    void waitForQuiet();

    Impulse imp_[2][2];                        // [A, B] x [the two copies]
    std::atomic<int> active_[2];
    // Begun and finished; see Engine::waitForQuiet for why it takes two of them.
    std::atomic<unsigned long long> stepsBegun_{ 0 }, stepsDone_{ 0 };
    Stage  stage_[kStages];
    int    stages_ = 0, head_ = 256;
    double sr_ = 48000.0;
    long   capSamples_ = 0;
    float  morph_ = 0.0f;
    long long t_ = 0;                          // samples taken in since reset()
    std::vector<float> hist_[2], ring_[2];     // the input's last blocks; the output still to come
    int    histMask_ = 0, ringMask_ = 0;
};

} // namespace ambient
