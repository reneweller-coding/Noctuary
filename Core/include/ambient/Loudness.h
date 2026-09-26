/**
 * @file Loudness.h
 * @brief Loudness to ITU-R BS.1770-4 / EBU R 128, and the peak numbers that go with it.
 *
 * Ambient is the one genre where loudness is the enemy rather than the goal: a brickwall limiter
 * takes the finest amplitude movement out of a reverb tail and leaves it grainy and flat, and the
 * sense of an enormous room comes from the distance between the quietest texture and the loudest
 * swell, not from the average level. A master that has been squeezed to -9 LUFS has thrown that
 * distance away and cannot get it back. So this instrument, which can render a finished piece,
 * says what the piece measures: integrated and short-term loudness, the crest factor, and the
 * true peak between the samples.
 *
 * It runs in the engine rather than in the editor, because the integrated figure is a number
 * about the whole piece: a meter that only sees what the interface happened to ask for is a
 * meter with holes in it.
 *
 * LoudnessMeter is the one the engine owns (Engine::loudness()); it feeds the K-weighted mean
 * squares into the gated blocks of the standard, the unweighted signal into the true-peak
 * estimate, and both channels into a ZwickerLoudness for the sone figures. The audio thread
 * writes, the message thread reads a LoudnessReading snapshot; the histories are fixed rings
 * (LoudnessLog), so nothing here allocates after prepare().
 */
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <vector>

namespace ambient {

/**
 * @brief One snapshot of the meter, as LoudnessMeter::read() fills it: the sone figures, the three
 *        LUFS figures and the range, the true peak and the crest, and how long it has been measuring.
 *
 * A plain value; the defaults are what an empty meter reports (-120 for every level, 0 for the rest).
 */
struct LoudnessReading {
    /**
     * @brief Loudness in sones, which is a different question from LUFS and the one an ambient bed is
     *        actually asked.
     *
     * LUFS is energy through one fixed weighting curve; sones are how loud the
     * ear says it is, and the two part company as soon as the spectrum changes. Loudness grows
     * with BANDWIDTH once the sound is wider than a critical band, so a broadband bed and a
     * narrow low drone at the same LUFS can differ by a factor of two in sones -- and for a piece
     * that has to sit at a comfortable level for an hour, the sone figure is the one that says
     * whether it will.
     *
     * now, after Zwicker
     */
    float sones      = 0.0f;
    float sonesN5    = 0.0f;      ///< exceeded five per cent of the time, since the last reset
    float sonesMax   = 0.0f;      ///< the loudest it has been
    float momentary  = -120.0f;   ///< LUFS over the last 400 ms
    float shortTerm  = -120.0f;   ///< LUFS over the last 3 s
    float integrated = -120.0f;   ///< LUFS, gated, since the last reset
    float range      = 0.0f;      ///< LU: the spread of the short-term values (10th to 95th centile)
    float truePeak   = -120.0f;   ///< dBTP, 4x oversampled, since the last reset
    float crest      = 0.0f;      ///< dB: true peak over the short-term loudness
    float seconds    = 0.0f;      ///< how long it has been measuring
    /// The correlation of left and right (25.09.2026, the production guide's correlation meter):
    /// +1 mono, 0 two unrelated signals, -1 one channel the other upside down. The guide wants the
    /// master's mean between 0.3 and 0.7 -- under it the image falls apart in mono, over it there
    /// is no width left.
    float correlation     = 0.0f;   ///< over the last 3 s, -1 .. 1; 0 while there is nothing to measure
    float correlationMean = 0.0f;   ///< since the last reset, energy-weighted, -1 .. 1
};

/**
 * @brief K-weighting: a high shelf and a high-pass, the two stages of BS.1770.
 *
 * The coefficients are
 * derived from the analogue prototypes the standard names, so they are right at any sample rate
 * rather than only at 48 kHz.
 *
 * One instance is one channel; LoudnessMeter owns a pair. Audio thread, per sample.
 */
class KFilter {
public:
    /**
     * @brief Computes both stages for a sample rate (the shelf from its analogue f0, gain and Q, the
     *        RLB high-pass with its unnormalised 1, -2, 1 numerator) and clears the state.
     * @param sampleRate  Hz
     */
    void prepare(double sampleRate);
    /** @brief Clears both stages' delay state; the coefficients stay. */
    void reset();
    /**
     * @brief One sample through the shelf and then the high-pass.
     * @param x  the input sample
     * @return   the K-weighted sample
     */
    float process(float x);
private:
    /** @brief One second-order section in transposed direct form II, with its two states. */
    struct Biquad {
        /** @var float b0
         *  @brief feed-forward coefficient of the current input */
        /** @var float b1
         *  @brief feed-forward coefficient of the input one sample back */
        /** @var float b2
         *  @brief feed-forward coefficient of the input two samples back */
        /** @var float a1
         *  @brief feedback coefficient of the output one sample back */
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;   ///< a2: feedback coefficient of the output two samples back (the section starts as a pass-through)
        /** @var float z1
         *  @brief the first state of the transposed form */
        float z1 = 0, z2 = 0;   ///< z2: the second state
        /**
         * @brief One sample through the section.
         * @param x  the input sample
         * @return   the filtered sample
         */
        float process(float x)
        {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        /** @brief Clears the two states. */
        void reset() { z1 = z2 = 0.0f; }
    };
    /** @var Biquad shelf_
     *  @brief stage one: the high shelf (+4 dB above about 1.7 kHz) */
    Biquad shelf_, hp_;   ///< hp_: stage two, the RLB high-pass at about 38 Hz
};

/**
 * @brief A log of one value per frame that never grows: written to on the audio thread, read on the
 *        message thread.
 *
 * It used to be a std::vector that was reserved for a few thousand entries and
 * then push_back'd for as long as the instrument played -- so after a few minutes, every time it
 * doubled, the audio thread stopped to copy up to sixteen megabytes. An instrument meant to run
 * all night cannot do that. What is kept now is a window of the recent past, which is also the
 * more useful thing to report: a meter that describes the last hour rather than the whole night.
 */
struct LoudnessLog {
    /**
     * @brief Sizes the ring (message thread, before use) and empties it.
     * @param capacity  values the window holds; the oldest is overwritten once it is full
     */
    void prepare(size_t capacity) { buf_.assign(capacity, 0.0f); pos_.store(0); count_.store(0); }
    /** @brief Empties the window without touching its storage. */
    void clear() { pos_.store(0); count_.store(0); }
    /**
     * @brief Appends one value, overwriting the oldest once the ring is full.
     *
     * audio thread; never allocates
     * @param v  the value (a loudness in LUFS or sones)
     */
    void push(float v)
    {
        if (buf_.empty()) return;
        const size_t p = pos_.load(std::memory_order_relaxed);
        buf_[p] = v;
        pos_.store((p + 1) % buf_.size(), std::memory_order_release);
        const size_t c = count_.load(std::memory_order_relaxed);
        if (c < buf_.size()) count_.store(c + 1, std::memory_order_release);
    }
    /** @brief How many values the window holds right now. @return 0 .. capacity */
    size_t size() const { return count_.load(std::memory_order_acquire); }
    /** @brief Whether nothing has been pushed since the last clear. @return true when size() is 0 */
    bool  empty() const { return size() == 0; }
    /**
     * @brief The window in order, oldest first (message thread).
     * @return a copy of the values; allocates, so not for the audio thread
     */
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
    std::vector<float> buf_;   ///< the ring's storage, sized once by prepare()
    /**
     * @brief Written by the audio thread, read by whoever asks for a reading.
     *
     * pos_ is the cell the next push() writes.
     */
    std::atomic<size_t> pos_ { 0 }, count_ { 0 };   ///< count_: how many cells hold a value, saturating at the capacity; the same threads
};

/**
 * @brief Loudness in sones, after Zwicker (Zwicker and Fastl, Psychoacoustics, 3rd ed.; the model
 *        standardised as ISO 532-1).
 *
 * The chain is the one the standard describes. A third-octave analysis gives the level in each
 * band; each band's energy is spread along the critical-band rate with the two slopes of a
 * masking pattern -- steep towards lower frequencies, and towards higher ones a slope that gets
 * shallower the louder the sound is, which is why a loud low tone masks upwards so far; the
 * spread energies are summed to an excitation pattern; each point of that pattern is turned into
 * a specific loudness by Zwicker's compressive law against the threshold in quiet; and the
 * specific loudness is integrated over the whole twenty-four Bark.
 *
 * The threshold in quiet is computed rather than tabulated, from Terhardt's approximation, so it
 * is right at every band centre instead of only at the ones a table happens to list.
 *
 * One convention has to be fixed and named, because sones are absolute and a digital signal is
 * not: full scale here is 100 dB SPL. A piece measuring -23 LUFS is then heard at 77 dB, which is
 * about a loud living room, and that is the level the figures below describe.
 *
 * The analysis is an 8192-point Hann-windowed FFT of the mono sum every 2048 samples; the band
 * levels of each frame go through fromBandLevels(), and the result is kept as the current value,
 * the maximum, and a LoudnessLog for the fifth centile.
 */
class ZwickerLoudness {
public:
    static constexpr int kBands = 28;      ///< third octaves, 25 Hz to 12.5 kHz
    static constexpr int kSteps = 240;     ///< 0.1 Bark apart, 0 to 24
    static constexpr float kFullScaleSpl = 100.0f;   ///< the convention: a full-scale sine is heard at this many dB SPL

    /**
     * @brief Allocates the FFT buffers, builds the window and the bin ranges of the 28 bands for this
     *        sample rate, sizes the history, and resets.
     * @param sampleRate  Hz (anything not positive reads as 48 kHz)
     */
    void prepare(double sampleRate);
    /** @brief Clears the ring, the current and maximum values and the history. */
    void reset();
    /**
     * @brief Feeds a block of the finished output: the mono sum goes into the analysis ring, and
     *        every 2048 samples a frame is measured (audio thread).
     * @param L  left channel
     * @param R  right channel
     * @param n  samples in the block
     */
    void process(const float* L, const float* R, int n);
    /** @brief The loudness of the last frame. @return sones */
    float sones() const { return now_; }
    /** @brief The loudest frame since the last reset. @return sones */
    float sonesMax() const { return max_; }
    /**
     * @brief The level the loudness stays under ninety-five per cent of the time (message thread; it
     *        copies the history and picks the centile).
     * @return exceeded five per cent of the time
     */
    float sonesN5() const;                 ///< exceeded five per cent of the time

    /**
     * @brief The model on its own, for tests and for offline use: third-octave levels in dB SPL in,
     *        loudness in sones out.
     *
     * rawLoudness() pinned to the sone's definition: the constant is the model's own answer for a
     * 1 kHz third octave at 40 dB SPL, computed once, so one sone IS that tone whatever the model
     * does otherwise.
     *
     * @param levelsDb  kBands levels in dB SPL, one per third octave from 25 Hz up
     * @return          sones
     */
    static float fromBandLevels(const float* levelsDb);
    /**
     * @brief Terhardt's approximation of the absolute threshold of hearing.
     * @param hz  frequency
     * @return    the threshold in dB SPL, clamped to -5 .. 80
     */
    static float thresholdInQuiet(float hz);
    /**
     * @brief Before the scale is pinned to the sone's own definition.
     *
     * Public only so that the pinning
     * below can be read and checked rather than believed.
     *
     * @param levelsDb  kBands levels in dB SPL
     * @return          the integral of the specific loudness over 24 Bark, in the model's own units
     */
    static float rawLoudness(const float* levelsDb);

private:
    /** @brief One analysis frame: window and transform the ring, sum the bins of each band into a level in dB SPL, and turn the levels into sones (audio thread, every kHop samples). */
    void frame();

    double sr_ = 48000.0;   ///< the sample rate prepare() was given
    /** @var static constexpr int kN
     *  @brief the FFT length: 8192 samples, 5.9 Hz per bin at 48 kHz */
    static constexpr int kN = 8192, kHop = 2048;   ///< kHop: samples between frames
    /** @var std::vector<float> ring_
     *  @brief the last kN mono samples */
    /** @var std::vector<float> re_
     *  @brief the transform's real part, windowed input in */
    /** @var std::vector<float> im_
     *  @brief the transform's imaginary part */
    std::vector<float> ring_, re_, im_, window_;   ///< window_: the Hann window of kN
    /** @var int pos_
     *  @brief write position in the ring */
    int   pos_ = 0, filled_ = 0;   ///< filled_: samples since the last frame; a frame runs at kHop
    /** @var int binLo_[kBands]
     *  @brief first FFT bin of each third octave */
    int   binLo_[kBands] = {}, binHi_[kBands] = {};   ///< binHi_: last bin of each third octave
    /** @var float now_
     *  @brief the last frame's loudness in sones */
    float now_ = 0.0f, max_ = 0.0f;   ///< max_: the loudest frame since the reset
    /** @brief A frame every 2048 samples, so this holds about an hour and a half at 48 kHz. */
    static constexpr size_t kHistory = 1u << 17;
    LoudnessLog history_;                  ///< one value per frame, for the fifth centile
};

/**
 * @brief The meter the engine runs on its output: BS.1770 momentary, short-term and gated integrated
 *        loudness, the loudness range, a true-peak estimate, the crest, and the sone figures through a
 *        ZwickerLoudness.
 *
 * process() is the audio thread's, per block; read() is anyone's and computes the gated and
 * centile figures from the logs on the spot, so it allocates and belongs on the message thread.
 */
class LoudnessMeter {
public:
    /**
     * @brief Sizes everything for a sample rate: the 100 ms hop, the K filters, the Zwicker model
     *        and the two block logs; then resets.
     * @param sampleRate  Hz (anything not positive reads as 48 kHz)
     */
    void prepare(double sampleRate);
    /** @brief Starts the measurement over: filters, hop sums, logs, true peak and the clock. */
    void reset();
    /**
     * @brief The finished output, after the master stage: what would be written to the file.
     *
     * Audio thread. The true peak is taken first, on the unweighted signal; then both channels are
     * K-weighted and their squares summed into the current hop, and every hop pushBlock() closes
     * one 100 ms slice.
     *
     * @param L  left channel
     * @param R  right channel
     * @param n  samples in the block
     */
    void process(const float* L, const float* R, int n);
    /**
     * @brief A snapshot of every figure: momentary from the last four hops, integrated gated at -70
     *        LUFS and then 10 LU under the mean, the range as the 10th to 95th centile of the
     *        short-term values above a -20 LU gate, the true peak, the crest, and the sones.
     * @return the reading; -120 for a level that has nothing to measure yet
     */
    LoudnessReading read() const;
private:
    /** @brief Closes one hop: its mean squares join the ring, and the last four hops make a 400 ms block for the log, the last thirty a 3 s short-term value. */
    void pushBlock();

    double sr_ = 48000.0;   ///< the sample rate prepare() was given
    /** @var KFilter kL_
     *  @brief the left channel's K-weighting */
    KFilter kL_, kR_;   ///< kR_: the right channel's
    ZwickerLoudness zwicker_;   ///< the sone model, fed the same blocks
    /** @var int blockLen_
     *  @brief samples in a 400 ms block (four hops)
     *  One 400 ms block, taken every 100 ms: the 75 % overlap the standard's gating is defined on. */
    /** @var int hopLen_
     *  @brief samples in a 100 ms hop */
    int    blockLen_ = 19200, hopLen_ = 4800, hopPos_ = 0;   ///< hopPos_: samples into the current hop
    /** @var std::vector<double> sumL_
     *  @brief the left channel's ring of per-hop mean squares */
    std::vector<double> sumL_, sumR_;   ///< ring of per-hop mean squares, four hops to a block
    /** @var int ringPos_
     *  @brief the next cell of the hop rings */
    int    ringPos_ = 0, ringFilled_ = 0;   ///< ringFilled_: hops in the ring so far, up to kHopsPerShort
    static constexpr int kHopsPerBlock = 4;       ///< hops in a 400 ms block
    static constexpr int kHopsPerShort = 30;      ///< 3 s
    /** @var double hopL_
     *  @brief the left channel's K-weighted squares summed over the current hop */
    double hopL_ = 0.0, hopR_ = 0.0;   ///< hopR_: the right channel's
    long   hopSamples_ = 0;   ///< samples summed into the current hop
    /**
     * @name The correlation meter (25.09.2026)
     * Unweighted sums of L x R, L x L and R x R: per hop, in a ring of the last three seconds, and
     * since the reset.
     * @{ */
    double hopLR_ = 0.0,   ///< L x R summed over the current hop
           hopLL_ = 0.0,   ///< L x L summed over the current hop
           hopRR_ = 0.0;   ///< R x R summed over the current hop
    std::array<double, kHopsPerShort> corrLR_{},   ///< @brief the ring of per-hop L x R sums
                           corrLL_{},   ///< the ring of per-hop L x L sums
                           corrRR_{};   ///< the ring of per-hop R x R sums
    double totLR_ = 0.0,   ///< L x R since the reset
           totLL_ = 0.0,   ///< L x L since the reset
           totRR_ = 0.0;   ///< R x R since the reset
    /** @} */
    /**
     * @brief Gating needs every block's loudness, not a running mean: the relative gate is a threshold
     *        computed from all of them and then applied to all of them.
     *
     * Ten a second, so these hold about an hour and three quarters.
     */
    static constexpr size_t kBlockLog = 1u << 16;
    LoudnessLog blocks_;                          ///< block loudness in LUFS
    LoudnessLog shortBlocks_;                     ///< short-term values, for the range
    double truePeak_ = 0.0;      ///< the largest inter-sample peak so far, linear
    double seconds_ = 0.0;       ///< how long the meter has been fed
    float  lastShort_ = -120.0f; ///< the last 3 s value in LUFS, what read() reports as shortTerm
    /**
     * @brief True peak: four-times oversampling with a short windowed-sinc, which is what "inter-sample"
     *        means in practice -- a signal at 0 dBFS on every sample can reach well above it between them.
     *
     * The last four samples of the left channel, for the four-point interpolation between the two
     * middle ones.
     */
    float  tpHistL_[4] = {}, tpHistR_[4] = {};   ///< tpHistR_: the same for the right channel
};

}   // namespace ambient
