// Noctuary -- loudness to ITU-R BS.1770-4 / EBU R 128, and the peak numbers that go with it.
//
// Ambient is the one genre where loudness is the enemy rather than the goal: a brickwall limiter
// takes the finest amplitude movement out of a reverb tail and leaves it grainy and flat, and the
// sense of an enormous room comes from the distance between the quietest texture and the loudest
// swell, not from the average level. A master that has been squeezed to -9 LUFS has thrown that
// distance away and cannot get it back. So this instrument, which can render a finished piece,
// says what the piece measures: integrated and short-term loudness, the crest factor, and the
// true peak between the samples.
//
// It runs in the engine rather than in the editor, because the integrated figure is a number
// about the whole piece: a meter that only sees what the interface happened to ask for is a
// meter with holes in it.
#pragma once
#include <atomic>
#include <cstddef>
#include <vector>

namespace ambient {

struct LoudnessReading {
    // Loudness in sones, which is a different question from LUFS and the one an ambient bed is
    // actually asked. LUFS is energy through one fixed weighting curve; sones are how loud the
    // ear says it is, and the two part company as soon as the spectrum changes. Loudness grows
    // with BANDWIDTH once the sound is wider than a critical band, so a broadband bed and a
    // narrow low drone at the same LUFS can differ by a factor of two in sones -- and for a piece
    // that has to sit at a comfortable level for an hour, the sone figure is the one that says
    // whether it will.
    float sones      = 0.0f;      // now, after Zwicker
    float sonesN5    = 0.0f;      // exceeded five per cent of the time, since the last reset
    float sonesMax   = 0.0f;      // the loudest it has been
    float momentary  = -120.0f;   // LUFS over the last 400 ms
    float shortTerm  = -120.0f;   // LUFS over the last 3 s
    float integrated = -120.0f;   // LUFS, gated, since the last reset
    float range      = 0.0f;      // LU: the spread of the short-term values (10th to 95th centile)
    float truePeak   = -120.0f;   // dBTP, 4x oversampled, since the last reset
    float crest      = 0.0f;      // dB: true peak over the short-term loudness
    float seconds    = 0.0f;      // how long it has been measuring
};

// K-weighting: a high shelf and a high-pass, the two stages of BS.1770. The coefficients are
// derived from the analogue prototypes the standard names, so they are right at any sample rate
// rather than only at 48 kHz.
class KFilter {
public:
    void prepare(double sampleRate);
    void reset();
    float process(float x);
private:
    struct Biquad {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float z1 = 0, z2 = 0;
        float process(float x)
        {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void reset() { z1 = z2 = 0.0f; }
    };
    Biquad shelf_, hp_;
};

// Loudness in sones, after Zwicker (Zwicker and Fastl, Psychoacoustics, 3rd ed.; the model
// standardised as ISO 532-1).
//
// The chain is the one the standard describes. A third-octave analysis gives the level in each
// band; each band's energy is spread along the critical-band rate with the two slopes of a
// masking pattern -- steep towards lower frequencies, and towards higher ones a slope that gets
// shallower the louder the sound is, which is why a loud low tone masks upwards so far; the
// spread energies are summed to an excitation pattern; each point of that pattern is turned into
// a specific loudness by Zwicker's compressive law against the threshold in quiet; and the
// specific loudness is integrated over the whole twenty-four Bark.
//
// The threshold in quiet is computed rather than tabulated, from Terhardt's approximation, so it
// is right at every band centre instead of only at the ones a table happens to list.
//
// One convention has to be fixed and named, because sones are absolute and a digital signal is
// not: full scale here is 100 dB SPL. A piece measuring -23 LUFS is then heard at 77 dB, which is
// about a loud living room, and that is the level the figures below describe.
// A log of one value per frame that never grows: written to on the audio thread, read on the
// message thread. It used to be a std::vector that was reserved for a few thousand entries and
// then push_back'd for as long as the instrument played -- so after a few minutes, every time it
// doubled, the audio thread stopped to copy up to sixteen megabytes. An instrument meant to run
// all night cannot do that. What is kept now is a window of the recent past, which is also the
// more useful thing to report: a meter that describes the last hour rather than the whole night.
struct LoudnessLog {
    void prepare(size_t capacity) { buf_.assign(capacity, 0.0f); pos_.store(0); count_.store(0); }
    void clear() { pos_.store(0); count_.store(0); }
    void push(float v)                      // audio thread; never allocates
    {
        if (buf_.empty()) return;
        const size_t p = pos_.load(std::memory_order_relaxed);
        buf_[p] = v;
        pos_.store((p + 1) % buf_.size(), std::memory_order_release);
        const size_t c = count_.load(std::memory_order_relaxed);
        if (c < buf_.size()) count_.store(c + 1, std::memory_order_release);
    }
    size_t size() const { return count_.load(std::memory_order_acquire); }
    bool  empty() const { return size() == 0; }
    // The window in order, oldest first (message thread).
    std::vector<float> snapshot() const
    {
        // Both indices are read once and then held: they move under this function, and a count
        // that grew between the reserve and the loop would have walked past what was counted.
        const size_t n = count_.load(std::memory_order_acquire);
        const size_t p = pos_.load(std::memory_order_acquire);
        std::vector<float> out;
        out.reserve(n);
        const size_t from = (n < buf_.size()) ? 0 : p;
        for (size_t k = 0; k < n; ++k) out.push_back(buf_[(from + k) % buf_.size()]);
        return out;
    }
private:
    std::vector<float> buf_;
    // Written by the audio thread, read by whoever asks for a reading.
    std::atomic<size_t> pos_ { 0 }, count_ { 0 };
};

class ZwickerLoudness {
public:
    static constexpr int kBands = 28;      // third octaves, 25 Hz to 12.5 kHz
    static constexpr int kSteps = 240;     // 0.1 Bark apart, 0 to 24
    static constexpr float kFullScaleSpl = 100.0f;

    void prepare(double sampleRate);
    void reset();
    void process(const float* L, const float* R, int n);
    float sones() const { return now_; }
    float sonesMax() const { return max_; }
    float sonesN5() const;                 // exceeded five per cent of the time

    // The model on its own, for tests and for offline use: third-octave levels in dB SPL in,
    // loudness in sones out.
    static float fromBandLevels(const float* levelsDb);
    static float thresholdInQuiet(float hz);
    // Before the scale is pinned to the sone's own definition. Public only so that the pinning
    // below can be read and checked rather than believed.
    static float rawLoudness(const float* levelsDb);

private:
    void frame();

    double sr_ = 48000.0;
    static constexpr int kN = 8192, kHop = 2048;
    std::vector<float> ring_, re_, im_, window_;
    int   pos_ = 0, filled_ = 0;
    int   binLo_[kBands] = {}, binHi_[kBands] = {};
    float now_ = 0.0f, max_ = 0.0f;
    // A frame every 2048 samples, so this holds about an hour and a half at 48 kHz.
    static constexpr size_t kHistory = 1u << 17;
    LoudnessLog history_;                  // one value per frame, for the fifth centile
};

class LoudnessMeter {
public:
    void prepare(double sampleRate);
    void reset();
    // The finished output, after the master stage: what would be written to the file.
    void process(const float* L, const float* R, int n);
    LoudnessReading read() const;
private:
    void pushBlock();

    double sr_ = 48000.0;
    KFilter kL_, kR_;
    ZwickerLoudness zwicker_;
    // One 400 ms block, taken every 100 ms: the 75 % overlap the standard's gating is defined on.
    int    blockLen_ = 19200, hopLen_ = 4800, hopPos_ = 0;
    std::vector<double> sumL_, sumR_;   // ring of per-hop mean squares, four hops to a block
    int    ringPos_ = 0, ringFilled_ = 0;
    static constexpr int kHopsPerBlock = 4;
    static constexpr int kHopsPerShort = 30;      // 3 s
    double hopL_ = 0.0, hopR_ = 0.0;
    long   hopSamples_ = 0;
    // Gating needs every block's loudness, not a running mean: the relative gate is a threshold
    // computed from all of them and then applied to all of them.
    // Ten a second, so these hold about an hour and three quarters.
    static constexpr size_t kBlockLog = 1u << 16;
    LoudnessLog blocks_;                          // block loudness in LUFS
    LoudnessLog shortBlocks_;                     // short-term values, for the range
    double truePeak_ = 0.0;
    double seconds_ = 0.0;
    float  lastShort_ = -120.0f;
    // True peak: four-times oversampling with a short windowed-sinc, which is what "inter-sample"
    // means in practice -- a signal at 0 dBFS on every sample can reach well above it between them.
    float  tpHistL_[4] = {}, tpHistR_[4] = {};
};

}   // namespace ambient
