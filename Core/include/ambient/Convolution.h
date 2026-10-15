/**
 * @file Convolution.h
 * @brief Convolution reverb ("Room"): a third, optional reverb on the far plane that
 *        plays a real (or generated) impulse response of up to a minute.
 *
 * Partitioned convolution with partitions that grow along the impulse (Battenberg and Avizienis
 * 2011; Wefers 2015). The impulse is cut into three stretches -- its first 85 ms, up to 0.68 s, and
 * the rest -- and each stretch into partitions of its own size: 256, 2048 and 16384 samples at
 * 48 kHz. A stage cuts the input into blocks of its partition size; each block's spectrum enters
 * the stage's frequency-domain delay line, and every output block is the sum over the partitions
 * of (delayed input spectrum x partition spectrum), then one inverse transform. That is exact for
 * any size: a block and a partition of B samples convolve to 2B - 1, which the 2B transform holds.
 *
 * Latency is the first stage's block, 256 samples. A later stage's answer is due D + 256 - B
 * samples after its input block is complete (D is where its stretch starts), and every stage keeps
 * D >= 2B - 512, so its work can be spread over the B samples until its next block in equal slices,
 * one at every 256-sample step: the audio thread never pays for a 16384-sample partition at once.
 * Per second a stage costs (stretch length x rate / B) products plus four transforms per block --
 * the long partitions of the tail are the cheap ones, which is what makes a minute affordable.
 *
 * Per partition only the bins that matter are kept: the late partitions of a real room carry
 * almost nothing at the top, and all that is left out, together with the silence at the end, is
 * less than -100 dB of the impulse's energy. A mono impulse is read once for both channels.
 *
 * Two impulses, A and B, share the delay lines: the morph blends their spectra inside the sum,
 * which is exactly a crossfade between two convolutions at the cost of one; at 0 or 1 only one
 * impulse is read at all. Impulses are double-buffered like textures: the message thread writes
 * the copy the audio thread is not reading, then swaps. Without a file a generated hall is loaded
 * at prepare(), so the Room works out of the box (see makeDefaultImpulse).
 * @author Rene Weller
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

namespace ambient {

/**
 * @brief The Room: a stereo partitioned convolver with two morphable impulses and a scheduled tail.
 *
 * The engine owns one on the far plane. prepare() sizes the three stages for the rate and the
 * longest impulse allowed; setImpulse() / setImpulseB() load impulses from the message thread
 * (resampled, normalised, analysed into per-partition spectra and swapped in without a glitch);
 * process() runs on the audio thread and never allocates, doing the stages' work in equal slices
 * at every 256-sample step (step()). The private part is the machinery the file comment
 * describes: a resumable transform, the per-stage state machine and the products.
 */
class Convolver {
public:
    static constexpr int Stages = 3;   ///< the three stretches of the impulse, each with its own partition size

    /** @brief Marks both impulse slots empty; nothing is allocated until prepare(). */
    Convolver();
    /**
     * @brief Sizes the stages, the delay lines and the rings for a rate and a longest impulse.
     *
     * maxSeconds bounds the memory (and CPU) an impulse may take. A longer file is shortened, not
     * cut: an exponential window brings its tail to -60 dB at the limit and a 50 ms fade ends it,
     * so the room gets smaller instead of stopping like a gate. A minute holds about 46 MB of delay line.
     *
     * The block sizes scale with the rate (256 x 1, 2 or 4 samples up to 64, 128 kHz and beyond),
     * so the latency stays about 5 ms. Stages whose stretch begins past the cap are left out. Both
     * impulses are forgotten. Not for the audio thread.
     * @param[in] sampleRate  the engine's rate in Hz
     * @param[in] maxSeconds  the longest impulse kept, in seconds (60 by default; 4 on the Quest)
     */
    void prepare(double sampleRate, float maxSeconds = 60.0f);
    /**
     * @brief Empties the delay lines and the rings; the impulses stay.
     *
     * Nothing old is read after this: a delay line counts as empty until it is written again.
     * Cheap enough for the audio thread whatever the impulse length.
     */
    void reset();
    /**
     * @brief Loads impulse A.
     *
     * Message thread. R may be nullptr (mono impulse). Resampled to the engine rate with a
     * windowed sinc, energy-normalised, the silence at the end trimmed.
     * @param[in] L                  the left (or only) channel, n samples
     * @param[in] R                  the right channel, or nullptr for a mono impulse read once for both
     * @param[in] n                  samples per channel
     * @param[in] impulseSampleRate  the rate the impulse was recorded at, in Hz; 0 or less means the engine's
     */
    void setImpulse(const float* L, const float* R, int n, double impulseSampleRate)  { load(0, L, R, n, impulseSampleRate); }
    /**
     * @brief The second impulse, the one the morph goes to.
     *
     * Message thread, otherwise as setImpulse().
     * @param[in] L                  the left (or only) channel, n samples
     * @param[in] R                  the right channel, or nullptr for mono
     * @param[in] n                  samples per channel
     * @param[in] impulseSampleRate  the rate the impulse was recorded at, in Hz
     */
    void setImpulseB(const float* L, const float* R, int n, double impulseSampleRate) { load(1, L, R, n, impulseSampleRate); }
    /**
     * @brief Forget impulse B: the room is A alone again, and B's memory goes back. Message thread.
     * @pre called on the message thread
     */
    void clearImpulseB();
    /**
     * @brief The built-in hall (makeDefaultImpulse), for a Room without a file.
     *
     * Renders the hall at the engine's rate and loads it as impulse A. Message thread.
     * @param[in] seed     seeds the hall's noise
     * @param[in] seconds  the hall's length, 4 s by default
     * @pre called on the message thread
     */
    void generateDefault(uint64_t seed, float seconds = 4.0f);
    /**
     * @brief The built-in hall as samples.
     *
     * The built-in hall as samples, at any rate, for generateDefault and for whoever wants to
     * measure it: the statistical late field the generated library's halls are made of (Gaussian
     * noise, every frequency decaying at its own T60), with a colour and a decay solved offline.
     * @param[in]  sampleRate  the rate to render at, in Hz
     * @param[in]  seed        seeds the two noise streams
     * @param[in]  seconds     the hall's length
     * @param[out] L           receives the left channel, seconds x sampleRate samples
     * @param[out] R           receives the right channel, half-coherent with the left in the bass
     */
    static void makeDefaultImpulse(double sampleRate, uint64_t seed, float seconds, std::vector<float>& L, std::vector<float>& R);
    /**
     * @brief Whether impulse A is loaded.
     * @return  true once setImpulse() or generateDefault() has published an impulse
     */
    bool  hasImpulse() const  { return m_active[0].load(std::memory_order_acquire) >= 0; }
    /**
     * @brief Whether impulse B is loaded.
     * @return  true between setImpulseB() and clearImpulseB()
     */
    bool  hasImpulseB() const { return m_active[1].load(std::memory_order_acquire) >= 0; }
    /**
     * @brief Length of impulse A, as kept.
     * @return  seconds after resampling, shortening and trimming; 0 without an impulse
     */
    float impulseSeconds() const;   // impulse A, as kept
    /**
     * @brief The Room's latency.
     * @return  the first stage's block in samples: 256 at rates under 64 kHz, 512 and 1024 above
     */
    int   latency() const { return m_head; }
    /**
     * @brief How long the output can still carry sound after the input has fallen silent.
     * @return  the longer impulse plus the latency and two of the last stage's blocks, in samples
     */
    long  tailSamples() const;
    /**
     * @brief The morph between the two impulses.
     *
     * 0 plays impulse A, 1 impulse B. Audio thread, before process(); each stage takes the value
     * at the start of its own blocks.
     * @param[in] x  0 .. 1 (clamped when a block begins)
     * @pre called on the audio thread
     */
    void  setMorph(float x) { m_morph = x; }
    /**
     * @brief Runs a block through the Room.
     *
     * Wet only, replaces outL/outR (which may be the inputs). Any n. Audio thread; never allocates.
     * @param[in]  inL   left input, n samples
     * @param[in]  inR   right input
     * @param[out] outL  receives the left wet signal; may be inL
     * @param[out] outR  receives the right wet signal; may be inR
     * @param[in]  n     samples in the block
     * @pre called on the audio thread
     */
    void  process(const float* inL, const float* inR, float* outL, float* outR, int n);

    /**
     * @name For the tests.
     * @{ */
    /**
     * @brief A stage's partition and block size.
     * @param[in] s  0 .. Stages - 1
     * @return B in samples
     */
    int   stageBlock(int s) const { return m_stage[s].m_block; }
    /**
     * @brief Where a stage's stretch of the impulse begins.
     * @param[in] s  0 .. Stages - 1
     * @return D in samples
     */
    int   stageStart(int s) const { return m_stage[s].m_start; }
    /**
     * @brief impulse A: spectrum bins stored after the band limits ...
     * @return  the bins kept over all partitions and stages; 0 without an impulse
     */
    long long keptBins() const;
    /**
     * @brief ... and what storing all of them would have taken
     * @return  the bins a full spectrum per partition would have held
     */
    long long fullBins() const;
    /** @} */

private:
    /**
     * @brief A radix-2 transform that can stop after any block of butterflies and carry on at the next
     *        step. Level h (half the span) keeps its rotations together: cs[h + k] = cos(pi k / h).
     */
    struct StepFft {
        int m_n = 0;   ///< the transform length, a power of two
        long long m_work = 0;                  ///< what one whole transform counts in the schedule
        std::vector<int>   m_rev;   ///< the bit-reversal permutation of 0 .. n - 1
        std::vector<float> m_cs,    ///< the cosines of every level, cs[h + k]
                           m_snF,   ///< the sines for the forward transform, negated
                           m_snI;   ///< sine negated for the forward transform, as is for the inverse
        /**
         * @brief Builds the tables for a size and its cost estimate.
         * @param[in] size  the transform length, a power of two
         */
        void init(int size);
    };
    /** @brief Where a stepwise transform has got to: the level (half the span) and the position in it. */
    struct FftRun {
        int m_half = 0,   ///< 0 during the bit reversal, then the level's half-span
        m_pos = 0;        ///< the element the run continues at
    };   ///< half 0: the bit reversal, at element pos
    /**
     * @brief Runs a transform as far as a budget allows and remembers where it stopped.
     *
     * The same radix-2 transform as Fft (Cosmos.h): forward turns by e^-i, inverse by e^+i and is
     * left unscaled -- the caller divides by n when it delivers. Stops at the end of a run of
     * butterflies once the budget is spent.
     * @param[in]     f        the tables for this size
     * @param[in,out] r        the run's position, FftRun{} to start; advanced by the call
     * @param[in,out] re       n real parts, transformed in place
     * @param[in,out] im       n imaginary parts
     * @param[in]     inverse  false forward, true inverse (unscaled)
     * @param[in,out] budget   work units left in this slice; reduced by what was done, may go negative
     * @return true once the whole transform is done
     */
    static bool runFft(const StepFft& f, FftRun& r, float* re, float* im, bool inverse, long long& budget);

    /** @brief One impulse's partitions in one stage. */
    struct Spectra {
        std::vector<float> m_re[2],   ///< the kept bins' real parts per channel, packed
                           m_im[2];   ///< packed: partition p's first cnt[p] bins start at off[p]
        std::vector<int>   m_off,   ///< where each partition's bins start in re/im
                           m_cnt;   ///< how many bins each partition keeps, a multiple of eight
        int m_parts = 0;   ///< partitions of this stretch the impulse reaches into
    };
    /** @brief One impulse as the audio thread reads it: its spectra per stage and what it was. */
    struct Impulse {
        Spectra   m_st[Stages];   ///< the partitions of each stage
        double    m_seconds = 0.0;   ///< length as kept, in seconds
        long      m_samples = 0;   ///< length as kept, in samples
        bool      m_stereo = false;   ///< whether the second channel holds its own spectra (false: read channel 0 for both)
        long long m_kept = 0,   ///< bins stored over all partitions
                  m_full = 0;   ///< bins a full spectrum per partition would have taken
    };
    /** @brief One stretch of the impulse: its partition size, its delay line and the block in progress. */
    struct Stage {
        int  m_block = 0,      ///< B: partition and input block size in samples
             m_start = 0,      ///< D: where the stretch begins in the impulse, in samples
             m_capParts = 0,   ///< partitions the stretch can hold up to the cap
             m_slot = 0;       ///< slot: bins per delay-line entry, block + 8
        StepFft m_fft;   ///< the 2B transform
        std::vector<float> m_fdlRe[2],   ///< the frequency-domain delay line's real parts per channel
                           m_fdlIm[2];   ///< capParts x slot per channel
        int  m_fdlHead = 0,     ///< the entry the next input spectrum goes into
             m_fdlFilled = 0;   ///< entries holding a spectrum, up to capParts
        std::vector<float> m_re[2],   ///< real parts of the working spectrum per channel
                           m_im[2];   ///< 2 x block: the input block's transform, then the sum and its inverse
        /**
         * @name The block in progress.
         * @{ */
        int  m_phase = 0,   ///< the state machine's phase (idle, forward transform, push, sum, inverse, deliver)
             m_ch = 0,      ///< the channel being transformed
             m_part = 0,    ///< the partition the sum is at
             m_bin = 0,     ///< the bin within it
             m_parts = 0;   ///< partitions this block sums, the delay line's fill when it was pushed
        FftRun m_run;   ///< where the current transform stopped
        long long m_write = 0,   ///< the absolute sample the answer lands at in the output ring
                  m_due = 0,     ///< the step (in latencies) the block must be finished by
                  m_work = 0;    ///< the work estimated to remain, for the equal slices
        float m_wa = 1.0f,   ///< impulse A's weight for this block, 1 - morph
              m_wb = 0.0f;   ///< impulse B's weight, the morph
        /** @} */
    };

    /**
     * @brief Loads an impulse into the copy the audio thread is not reading, then swaps.
     *
     * Message thread. Resamples both channels to the engine rate, shortens an impulse beyond the
     * cap (shrinkToCap), normalises the energy to one, trims the silence at the end, waits for the
     * steps that had begun, analyses into the free copy, publishes it and frees the old copy once
     * every step that could still read it has ended.
     * @param[in] which  0 impulse A, 1 impulse B
     * @param[in] L      the left (or only) channel
     * @param[in] R      the right channel or nullptr
     * @param[in] n      samples per channel
     * @param[in] rate   the impulse's own sample rate in Hz
     */
    void load(int which, const float* L, const float* R, int n, double rate);
    /**
     * @brief Cuts an impulse into the stages' partitions and keeps each partition's spectrum up to
     *        the last bin that matters.
     *
     * Each partition is zero-padded to 2B and transformed; from the top down, bins are dropped
     * while the energy they carry stays inside the partition's share of the budget, and the rest
     * are stored packed, rounded up to eights.
     * @param[in,out] imp     the copy to fill
     * @param[in]     ch      the channels, resampled and normalised
     * @param[in]     stereo  whether ch[1] is to be analysed as well
     * @param[in]     len     samples to analyse (the trimmed length)
     */
    void analyse(Impulse& imp, const std::vector<float>* ch, bool stereo, int len);
    /**
     * @brief One scheduling step, every latency's worth of samples on the audio thread.
     *
     * Counts itself as begun and, on every way out, as done. Begins a stage's new block where its
     * block boundary falls (finishing a leftover first), then gives every busy stage an equal share
     * of its remaining work for the steps left until it is due, the last taking the rest.
     */
    void step();
    /**
     * @brief Begins a stage's block: copies the last B input samples into the work buffers, zero-padded,
     *        and sets the phase, the deadline, the morph weights and the work estimate.
     * @param[in,out] s  the stage
     */
    void begin(Stage& s);
    /**
     * @brief Runs a stage's state machine until its block is done or the quota is spent.
     *
     * Forward transform of both channels, push into the delay line, the products (sum()), inverse
     * transform of both channels, delivery into the output ring scaled by 1 / N.
     * @param[in,out] s      the stage
     * @param[in]     g      its index, for the impulses' spectra
     * @param[in]     quota  work units this slice may spend
     * @param[in]     A      impulse A as the step saw it, or nullptr
     * @param[in]     B      impulse B, or nullptr
     * @return the work units used
     */
    long long advance(Stage& s, int g, long long quota, const Impulse* A, const Impulse* B);
    /**
     * @brief The products: accumulates delayed input spectra times partition spectra, in chunks of bins.
     *
     * Where both impulses have a bin they are blended with the block's weights; above the shorter
     * one only the longer is summed. A mono impulse is loaded once for both channels. When every
     * partition is done the upper half of the spectrum is mirrored and the phase moves to the inverse.
     * @param[in,out] s      the stage
     * @param[in]     g      its index
     * @param[in]     A      impulse A or nullptr
     * @param[in]     B      impulse B or nullptr
     * @param[in,out] quota  work units left; reduced by what was done
     */
    void sum(Stage& s, int g, const Impulse* A, const Impulse* B, long long& quota);
    /**
     * @brief Message thread: waits until every step that had begun when the call was made has ended.
     *
     * Spins on m_stepsDone against m_stepsBegun with 50 microsecond sleeps, giving up after 400 ms;
     * see Engine::waitForQuiet.
     * @pre called on the message thread
     */
    void waitForQuiet();

    Impulse m_imp[2][2];                        ///< [A, B] x [the two copies]
    std::atomic<int> m_active[2];   ///< per impulse, the copy the audio thread reads (0 or 1), or -1 for none
    /** Begun and finished; see Engine::waitForQuiet for why it takes two of them. */
    std::atomic<unsigned long long> m_stepsBegun{ 0 },   ///< steps begun
                                    m_stepsDone{ 0 };    ///< steps finished
    Stage  m_stage[Stages];   ///< the stages, m_stages of them in use
    int    m_stages = 0,   ///< stages the cap needs, up to Stages
           m_head = 256;   ///< the latency and the step, in samples: 256 x the rate scale
    double m_sr = 48000.0;   ///< sample rate in Hz
    long   m_capSamples = 0;   ///< the longest impulse kept, in samples at the engine rate
    float  m_morph = 0.0f;   ///< the morph as last set; each stage reads it when a block begins
    long long m_t = 0;                          ///< samples taken in since reset()
    std::vector<float> m_hist[2],   ///< the input's last blocks, a ring per channel
                       m_ring[2];   ///< the input's last blocks; the output still to come
    int    m_histMask = 0,   ///< m_hist size - 1
           m_ringMask = 0;   ///< m_ring size - 1
};

} // namespace ambient
