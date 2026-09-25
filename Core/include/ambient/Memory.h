/**
 * @file Memory.h
 * @brief Memory: a drifting sound memory.
 *
 * In the spirit of SOMA's Cosmos (Vlad Kreimer, 2021): a long memory in which what was played keeps
 * being recombined -- delay lines of different prime lengths sliding against each other, their
 * content exchanged and turned about the stereo field, fading, or held for ever. Not a copy of it:
 * the parts come from the feedback-delay-network literature, which has better answers to the
 * questions such a device has to settle.
 *
 * @code{.unparsed}
 *   Blur    The lines feed back through a lossless matrix -- a butterfly of Givens rotations, all
 *           turned by one angle -- that runs from the identity (every line its own echo) to a dense,
 *           Hadamard-like mix (every echo in every line) and is orthogonal at every angle between, so
 *           turning it exchanges energy between the lines and never adds or removes any (Schlecht and
 *           Habets, "On lossless feedback delay networks", IEEE TSP 2017; "Time-varying feedback
 *           matrices in feedback delay networks", JASA 2015). Hold at 1 therefore holds exactly,
 *           whatever Blur does. Beyond a half, an all-pass inside every line scatters each echo into
 *           many (Schlecht and Habets, "Scattering in feedback delay networks", IEEE/ACM TASLP 2020):
 *           the separate echoes run together into a space -- which the Cosmos offers as a separate
 *           algorithm, and this reaches on the same knob.
 *   Drift   The read heads slide on slow asynchronous sines whose rates a Lorenz attractor bends, and
 *           every line wanders across the stereo field. The slide is bound in pitch -- at most 15
 *           cents at 1 -- so the pattern of the echoes changes and the notes do not warble. A sliding
 *           head reads between cells, through an eight-tap windowed sinc: a four-point interpolator
 *           takes about 2 dB off the top octave of a noise on every pass, and a memory is passes.
 *   Hold    How long a memory lasts (4 s to -60 dB at 0, twenty minutes near 1, for ever at 1); Age
 *           how much darker it grows on the way: a first-order absorption filter in every line, its
 *           loss scaled by the line's length so that every line ages at the same rate per second
 *           (after Jot and Chaigne 1991).
 *   Renew   The Cosmos's suppressor: what arrives pushes out what is kept, the harder the louder it is.
 *   Drive   A saturation in the loop with antiderivative antialiasing (Adaa.h), so a loop that runs
 *           for minutes does not fill up with aliases -- and, half a sample late, a little darker on
 *           every pass, as tape is. Under it, always, a limiter on every line.
 *   Recall  Grains played out of the whole memory. Where the Cosmos scrubs at random, Seek lets the
 *           grains prefer the stretches whose pitch classes fit what is playing now -- the scale and
 *           the last seconds of input -- the selection principle of corpus-based concatenative
 *           synthesis (Schwarz et al., CataRT, DAFx 2006): the memory answers the harmony.
 *   Tape    Reverse and Half Speed move the tape itself: the heads keep their distance and the tape
 *           runs backwards or at half speed, so what was recorded plays backwards or an octave down,
 *           while what is recorded now comes back as it went in -- as on a tape machine. A change of
 *           direction runs down through a standstill and up again.
 * @endcode
 *
 * The memory is one pool of 90 seconds, divided evenly between the lines, so changing the number of
 * lines re-divides the same material rather than emptying it. Buffers are allocated in prepare() only.
 *
 * In the engine it is a parallel send off the near bus, returned to both planes like the Cosmos
 * (Engine.h: memory_ and its three sends); readParams() hands it the Mem* parameters once per block
 * and process() runs on the audio thread in control blocks of kSub samples.
 */
#pragma once
#include "Dsp.h"
#include "Adaa.h"
#include "Cosmos.h"
#include "GrainRing.h"
#include "Tuning.h"
#include <vector>
#include <cstdint>

namespace ambient {

/**
 * @brief The drifting sound memory of the file comment: 2, 4 or 8 prime-length lines over one pool
 *        of tape, exchanged by a lossless butterfly, sliding, ageing, saturating, and recalled as
 *        grains that prefer what fits the harmony.
 *
 * Setters are plain stores the engine makes once per block from its parameters; process() picks the
 * changes up at the start of every control block and glides towards them, so nothing steps. The
 * accessors under "For the self test" read state without synchronisation.
 */
class Memory {
public:
    static constexpr int kMaxLines = 8;          ///< lines the pool can be divided into at most (2, 4 or 8 are used)
    static constexpr int kMaxGrains = 32;        ///< recall grains alive at once
    static constexpr int kSegBits = 12;          ///< log2 of kSeg
    static constexpr int kSeg = 1 << kSegBits;   ///< the stretch of tape one pitch-class fingerprint covers
    static constexpr int kSub = 64;              ///< control block, in samples

    /**
     * @brief Allocates the pool (90 s, capped at kPoolMaxCells), the all-pass rings, the reading
     *        interpolator, the fingerprint tables and the FFT scratch; lays the lines out and puts every
     *        state at rest. Message thread, before any process().
     * @param sampleRate  Hz
     * @param seed        the memory's own random stream (the engine gives it a constant, so a fork
     *                    of the voices' dice is never spent here)
     */
    void prepare(double sampleRate, uint64_t seed);
    /**
     * @brief lines 2, 4 or 8; size: the longest line's repeat in seconds; blur and drift 0..1.
     *
     * A new line count is taken up at the next control block after the output has faded; size moves
     * the read heads as a tape delay does; blur is the butterfly's angle and, past a half, the
     * scattering; drift the slides and the pans.
     *
     * @param lines        2, 4 or 8 (anything else rounds up to the next of them)
     * @param sizeSeconds  the longest line's repeat, clamped to 0.2 .. 60 s (and to the pool's share)
     * @param blur         0..1
     * @param drift        0..1
     */
    void setShape(int lines, float sizeSeconds, float blur, float drift);
    /**
     * @brief all 0..1
     * @param hold   how long a memory lasts: 4 s to -60 dB at 0, twenty minutes near 1, for ever at 1
     * @param age    how much darker it grows per second of line, up to 2 dB per second at Nyquist
     * @param renew  how hard what arrives pushes out what is kept
     * @param drive  the saturation in the loop
     */
    void setDecay(float hold, float age, float renew, float drive);
    /**
     * @brief recall and seek 0..1, the grains' length in milliseconds
     * @param recall   how much of the output is grains rather than the lines (0 switches them off)
     * @param seek     how much a grain's harmony counts against chance when its place is chosen
     * @param grainMs  the grains' length, 20 .. 2000 ms (each drawn within a fifth of it)
     */
    void setRecall(float recall, float seek, float grainMs);
    /**
     * @brief The tape's four switches.
     * @param reverse  the tape runs backwards (through a standstill on the way)
     * @param half     the tape runs at half speed, an octave down
     * @param freeze   nothing is written, nothing decays: the memory is held as it is
     * @param erase    the output fades and the pool is emptied a stretch at a time
     */
    void setTape(bool reverse, bool half, bool freeze, bool erase);
    /**
     * @brief What Seek listens for: the scale and the frequency of its tonic.
     *
     * Turned into a twelve-bin pitch-class profile (the tonic weighted up) that the recall grains
     * score the tape's fingerprints against; recomputed only when the pair changes.
     *
     * @param scale    the current scale
     * @param tonicHz  the key's tonic in Hz (anything not above 1 Hz is ignored)
     */
    void setHarmony(const FixedScale& scale, double tonicHz);
    /**
     * @brief In place: the send in, the memory's voice out.
     *
     * Audio thread. In control blocks of kSub samples: the control step, the recall grains, then
     * every sample through the lines -- read through the sinc, all-pass, absorption and DC blocker
     * in the loop, the butterfly, gain, limiter and drive, the input added, and the tape written.
     *
     * @param L  left channel, replaced
     * @param R  right channel, replaced
     * @param n  samples in the block
     */
    void process(float* L, float* R, int n);
    /**
     * @brief Nothing in and nothing out for longer than two passes of the longest line.
     * @return true when the engine may skip the memory without losing a tail
     */
    bool quiet() const { return quietFor_ > quietAfter_; }

    /**
     * @name For the self test.
     * @{ */
    /**
     * @brief The energy in the loop.
     * @return what circulates: the lines' spans and their all-passes
     */
    double    energy() const;                    ///< what circulates: the lines' spans and their all-passes
    /** @brief How many lines the pool is divided into right now. @return 2, 4 or 8 */
    int       lineCount() const { return lines_; }
    /** @brief One line's repeat at normal speed. @param k the line @return seconds, the all-pass included */
    double    lineSeconds(int k) const { return (line_[k].lenTarget + apLen_[k]) / sr_; }
    /** @brief Recall grains started since prepare(). @return the count */
    long long grainsStarted() const { return started_; }
    /** @brief The tape's speed. @return 1 forwards, -1 backwards, 0.5 at half speed, in between while it glides */
    double    velocity() const { return v_; }
    /** @} */
private:
    /** @var static constexpr int kTaps
     *  @brief taps of the reading interpolator */
    static constexpr int kTaps = 8, kPhases = 256;   ///< kPhases: fractional phases the interpolator is tabulated at
    /**
     * @brief One delay line: its write head with the two cells it is splatting into, its read
     *        distance and slide, its pan, and the state of the filters and the saturation in its loop.
     */
    struct Line {
        double   w = 0.0;                    ///< the write head, in cells, in [0, cap)
        int      base = 0;                   ///< the cell it was in when the two pending cells were set up
        /** @var float acc0
         *  @brief the weighted sum splatted into cell base so far */
        float    acc0 = 0.0f, wt0 = 0.0f;    ///< what the head has splatted into that cell ...
        /** @var float acc1
         *  @brief the weighted sum splatted into the cell after base */
        float    acc1 = 0.0f, wt1 = 0.0f;    ///< ... and into the one after it
        double   lenTarget = 0.0;            ///< the read head's distance behind the write head, in cells
        double   lenBase = 0.0;              ///< the same, glided towards the target
        /** @var double lenPrev
         *  @brief the read distance at the start of the sub-block, slide included */
        double   lenPrev = 0.0, lenNext = 0.0;   ///< at the start and at the end of the sub-block
        /** @var double driftPh
         *  @brief phase of the slide's sine, radians */
        double   driftPh = 0.0, panPh = 0.0;   ///< panPh: phase of the pan's sine, radians
        float    driftAmp = 0.0f;            ///< cells, glided
        /** @var float gain
         *  @brief the loop gain per pass that gives Hold its decay */
        /** @var float absA
         *  @brief the absorption one-pole's coefficient for Age */
        float    gain = 1.0f, absA = 0.0f, absZ = 0.0f;   ///< absZ: the absorption filter's state
        /** @var float dcX
         *  @brief the loop's DC blocker: the previous input */
        float    dcX = 0.0f, dcY = 0.0f;   ///< dcY: the previous output
        float    env = 0.0f;                 ///< the limiter's envelope follower
        TanhAdaa sat;                        ///< the drive's saturation, antialiased (Adaa.h)
        Svf      aa;                         ///< the slow tape's anti-alias low-pass
        /** @var float glPrev
         *  @brief left gain at the start of the sub-block */
        /** @var float grPrev
         *  @brief right gain at the start of the sub-block */
        /** @var float glNext
         *  @brief left gain at its end */
        float    glPrev = 0.7f, grPrev = 0.7f, glNext = 0.7f, grNext = 0.7f;   ///< grNext: right gain at the end of the sub-block (an equal-power pan)
        int      seg = -1;                   ///< the pool segment the write head is in
    };
    /**
     * @brief Divides the pool between `lines` lines and folds every write head into the new capacity;
     *        the grains are dropped, since their places belonged to the old division.
     * @param lines  2, 4 or 8
     */
    void   layout(int lines);
    /** @brief Sets every line's target length from Size: the longest as far as its share allows, the rest stepping down geometrically to six tenths, each a prime number of cells; then gains(). */
    void   lengths();
    /** @brief Sets every line's loop gain from Hold and its absorption coefficient from Age, both scaled by the line's length so all lines decay and age at the same rate per second. */
    void   gains();
    /**
     * @brief The control step at the head of every sub-block: line-count and erase fades, the Lorenz
     *        step, the tape speed glide, the butterfly angle and scattering, the recall, drive and
     *        input gains, and every line's slide, pan and anti-alias filter.
     * @param m  samples in this sub-block (kSub, or the remainder)
     */
    void   stepControl(int m);
    /**
     * @brief Writes one sample onto line k's tape at the write head, spread over the two cells it falls
     *        between, and advances the head by v; queues a finished segment for fingerprinting.
     * @param k  the line
     * @param x  the sample
     * @param v  cells per sample: the tape's speed, negative backwards
     */
    void   tapeWrite(int k, float x, double v);
    /**
     * @brief Starts a recall grain if there is room: draws a dozen places behind the write heads, scores
     *        their fingerprints against the harmony as Seek says, and takes the best.
     * @param offset  the sample of the current sub-block the grain begins in
     */
    void   spawnGrain(int offset);
    /** @brief One fingerprint per control block at most: the last seconds of input when a window is full, otherwise the next finished stretch of tape from the queue. */
    void   analyse();
    /**
     * @brief The pitch-class profile of kSeg samples: a Hann-windowed FFT, the bins between 55 Hz and
     *        5 kHz summed into their pitch classes, the twelve normalised.
     * @param x       kSeg samples
     * @param chroma  receives the twelve values (all zero for silence)
     * @param rms     receives the stretch's RMS
     */
    void   fingerprint(const float* x, float* chroma, float& rms);
    /** @brief Clears every line's splat sums, filter states and saturation (after an erase). */
    void   resetLines();

    std::vector<float> pool_;                ///< the tape: poolCells_ mono cells, cap_ per line
    std::vector<float> sinc_;                ///< the reading interpolator: (kPhases + 1) rows of kTaps
    /** @var int poolCells_
     *  @brief cells in the pool: 90 s of sample rate, capped */
    /** @var int lines_
     *  @brief lines the pool is divided into right now */
    int       poolCells_ = 0, lines_ = 4, cap_ = 0;   ///< cap_: cells per line, a multiple of kSeg
    Line      line_[kMaxLines];              ///< the lines; the first lines_ are in use
    std::vector<float> ap_;                  ///< the scattering all-passes' rings, apSize_ cells per line
    /** @var int apSize_
     *  @brief cells per all-pass ring, a power of two */
    /** @var int apMask_
     *  @brief apSize_ - 1 */
    int       apSize_ = 0, apMask_ = 0, apW_ = 0;   ///< apW_: the all-passes' shared write position
    int       apLen_[kMaxLines] = {};        ///< each all-pass's delay in cells (kApMs in Memory.cpp)
    double    sr_ = 48000.0;                 ///< the sample rate prepare() was given
    Rng       rng_;                          ///< the memory's own stream: grain places, lengths and pans
    /**
     * @name settings
     * What the setters last stored; process() glides towards them.
     * @{ */
    int       linesWant_ = 4;                ///< the line count asked for; taken up once the output has faded
    /** @var float size_
     *  @brief the longest line's repeat in seconds */
    /** @var float blur_
     *  @brief 0..1, the butterfly's angle and the scattering */
    float     size_ = 9.0f, blur_ = 0.25f, drift_ = 0.3f;   ///< drift_: 0..1, the slides and the pans
    /** @var float hold_
     *  @brief 0..1, how long a memory lasts */
    /** @var float age_
     *  @brief 0..1, how much darker it grows */
    /** @var float renew_
     *  @brief 0..1, the suppressor */
    float     hold_ = 0.8f, age_ = 0.3f, renew_ = 0.0f, drive_ = 0.0f;   ///< drive_: 0..1, the saturation in the loop
    /** @var float recall_
     *  @brief 0..1, the grains' share of the output */
    /** @var float seek_
     *  @brief 0..1, harmony against chance in the grains' choice */
    float     recall_ = 0.0f, seek_ = 0.5f, grainMs_ = 220.0f;   ///< grainMs_: the grains' length in milliseconds
    /** @var bool reverse_
     *  @brief the tape runs backwards */
    /** @var bool half_
     *  @brief the tape runs at half speed */
    /** @var bool freeze_
     *  @brief nothing written, nothing decays */
    bool      reverse_ = false, half_ = false, freeze_ = false, erase_ = false;   ///< erase_: the pool is being emptied
    /** @} */
    /**
     * @name state
     * What process() carries from one control block to the next.
     * @{ */
    /** @var double v_
     *  @brief the tape's speed in cells per sample, glided towards the switches' target */
    double    v_ = 1.0, vPrev_ = 1.0;        ///< vPrev_: the speed at the previous control block, for the ramp across the sub-block
    bool      gliding_ = false;              ///< the speed is on its way; when it arrives at full speed the heads are put back on whole cells
    /** @var float angle_
     *  @brief the butterfly's angle in radians, 0 .. pi/4, glided */
    float     angle_ = 0.0f, apCoef_ = 0.0f;   ///< apCoef_: the scattering all-passes' coefficient, 0 below Blur 0.45, up to 0.6
    double    stagePh_[3] = {};              ///< each butterfly stage's slow sine, radians, leaning its angle off the common one
    /** @var float stageC_[3]
     *  @brief cosine of each stage's angle this block */
    float     stageC_[3] = { 1.0f, 1.0f, 1.0f }, stageS_[3] = {};   ///< stageS_: its sine
    /** @var float recallCur_
     *  @brief the recall share, glided */
    /** @var float driveCur_
     *  @brief the drive, glided */
    /** @var float inGainCur_
     *  @brief the input gain, 1 or (frozen, erasing) 0, ramped over 20 ms */
    float     recallCur_ = 0.0f, driveCur_ = 0.0f, inGainCur_ = 1.0f, outGainCur_ = 1.0f;   ///< outGainCur_: the output gain, faded out over 60 ms for a line-count change or an erase
    /** @var float inEnv_
     *  @brief the input's envelope for Renew: 10 ms attack, 300 ms release */
    /** @var float envAtk_
     *  @brief that envelope's attack coefficient */
    /** @var float envRel_
     *  @brief its release coefficient */
    /** @var float limAtk_
     *  @brief the per-line limiter's attack coefficient (1 ms) */
    /** @var float limRel_
     *  @brief its release coefficient (400 ms) */
    float     inEnv_ = 0.0f, envAtk_ = 0.0f, envRel_ = 0.0f, limAtk_ = 0.0f, limRel_ = 0.0f, dcR_ = 0.999f;   ///< dcR_: the loop DC blocker's pole, 3 Hz
    double    lorenz_[3] = { 0.1, 0.0, 20.0 };   ///< the Lorenz attractor's state, one time unit to eight seconds
    float     chaos_[3] = {};                ///< its three coordinates scaled to -1..1, bending the slides and the pans
    bool      pendingLines_ = false;         ///< a new line count is waiting for the output to fade
    int       clearPos_ = -1;                ///< how far the erase has emptied the pool, -1 when not erasing
    bool      cleared_ = false;              ///< the erase has finished; nothing more to empty until Erase is released
    /** @var float sizeDone_
     *  @brief the size the lengths were last computed for */
    /** @var float holdDone_
     *  @brief the hold the gains were last computed for */
    float     sizeDone_ = -1.0f, holdDone_ = -1.0f, ageDone_ = -1.0f;   ///< ageDone_: the age the gains were last computed for
    /** @var long long quietFor_
     *  @brief samples with nothing in and nothing out */
    long long quietFor_ = 0, quietAfter_ = 0;   ///< quietAfter_: two passes of the longest line plus a second, in samples; quiet() past it
    /** @} */
    /**
     * @name recall
     * The grains and the fingerprints they choose their places by.
     * @{ */
    RingGrain grains_[kMaxGrains];           ///< the live grains, the first live_ of them
    int       live_ = 0;                     ///< grains playing
    long long started_ = 0;                  ///< grains started since prepare()
    /** @var double hazard_
     *  @brief the Poisson clock's accumulated hazard */
    double    hazard_ = 0.0, nextHazard_ = 0.0;   ///< nextHazard_: the exponential draw the next grain starts at
    std::vector<float> chroma_;             ///< twelve per segment of the pool
    std::vector<float> segRms_;             ///< each segment's RMS, so silence is never chosen while anything else is there
    std::vector<int>   queue_;              ///< segments the write heads have finished, waiting to be fingerprinted (a ring of kQueueMask + 1)
    /** @var int qHead_
     *  @brief the queue's read position */
    int       qHead_ = 0, qTail_ = 0;       ///< qTail_: its write position
    /** @var std::vector<float> fftRe_
     *  @brief the fingerprint transform's real part */
    /** @var std::vector<float> fftIm_
     *  @brief its imaginary part */
    std::vector<float> fftRe_, fftIm_, window_;   ///< window_: the Hann window of kSeg
    std::vector<signed char> binPc_;        ///< each FFT bin's pitch class, -1 outside what is listened to
    RealFft   fft_{ kSeg };                 ///< the transform of one segment
    /** @var float scaleChroma_[12]
     *  @brief the scale's pitch-class profile from setHarmony(), normalised */
    float     scaleChroma_[12] = {}, inputChroma_[12] = {};   ///< inputChroma_: the last seconds of input as a profile, a few seconds of memory
    float     inputLevel_ = 0.0f;           ///< the input's RMS, smoothed like inputChroma_; weights the input into what the grains listen for
    std::vector<float> inBuf_;              ///< kSeg samples of the mono input being gathered for the next fingerprint
    int       inPos_ = 0;                   ///< samples gathered so far
    double    harmonySig_ = -1.0;           ///< a signature of the scale and tonic the profile was built for, so setHarmony() only rebuilds on a change
    /** @} */
};

} // namespace ambient
