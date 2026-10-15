/**
 * @file Cosmos.h
 * @brief The Cosmos path: science-fiction / deep-space processing.
 *
 * ```{.unparsed}
 *   FreqShifter    single-sideband frequency shifter (Hilbert pair), inharmonic, alien
 *   CombResonator  tuned comb filters with feedback (metallic, hull-like resonances)
 *   VowelFilter    three formants morphing slowly between vowels (alien choir)
 *   Nebula         STFT magnitude smearing with random phases (Paulstretch-like
 *                  texture; smear = 1 freezes the spectrum)
 *   PitchShifter   granular two-head shifter, used for the shimmer feedback loop
 * ```
 * Buffers are allocated in prepare() only.
 *
 * The Cosmos is one of the engine's processing paths: a stereo chain the far plane can be sent
 * through, each stage with its own mix so a preset can take one of them alone. The two transforms
 * at the end of the file -- Fft, and the RealFft built on it -- are the engine's only Fourier
 * transforms outside the Room's own stepwise one (Convolution.cpp): the Nebula, the spectral
 * shifter, the Memory, the spectral source and the CycleTable builder all use them.
 */
#pragma once
#include "Dsp.h"
#include <vector>

namespace ambient {

/**
 * @brief Single-sideband frequency shifter: every partial moved by the same number of hertz, so
 *        harmonic becomes inharmonic.
 *
 * The input is split into a 90-degree pair by two chains of all-passes (Niemitalo's design), which
 * is multiplied by a quadrature oscillator at the shift frequency: re x cos + im x sin keeps the one
 * sideband and cancels the other. The right channel's oscillator runs three per cent slower than the
 * left's, so the two sides beat slowly against each other. Wet and dry are mixed inside; at mix 0
 * nothing is computed.
 */
class FreqShifter {
public:
    /**
     * @brief Takes the sample rate and clears the all-pass chains and the oscillator.
     * @param sampleRate  the engine's rate in Hz
     */
    void prepare(double sampleRate);
    /**
     * @brief The shift and the mix, once a block.
     * @param shiftHz  how far every partial moves, in Hz; negative moves the spectrum down
     * @param mix      0 .. 1 wet against dry
     */
    void set(float shiftHz, float mix) { m_shift = shiftHz; m_mix = clampv(mix, 0.0f, 1.0f); }
    /**
     * @brief Shifts a block in place.
     * @param L  left channel, n samples, replaced by the mix
     * @param R  right channel, likewise
     * @param n  samples in the block
     */
    void process(float* L, float* R, int n);
private:
    /**
     * @brief One channel's 90-degree phase splitter: two chains of four second-order all-passes
     *        whose outputs are a quarter cycle apart across the band (Olli Niemitalo).
     */
    struct Hilbert {
        float m_x1[4] = {},   ///< chain A: each section's input one sample ago
              m_x2[4] = {},   ///< chain A: input two samples ago
              m_y1[4] = {},   ///< chain A: output one sample ago
              m_y2[4] = {};   ///< chain A: output two samples ago
        float m_x1b[4] = {},   ///< chain B: input one sample ago
              m_x2b[4] = {},   ///< chain B: input two samples ago
              m_y1b[4] = {},   ///< chain B: output one sample ago
              m_y2b[4] = {};   ///< chain B: output two samples ago
        float m_delayed = 0.0f;   ///< chain A's last output: A lags B by one sample, so A is handed out a sample late to line them up
        /**
         * @brief One sample through both chains.
         * @param in  the input sample
         * @param re  receives the in-phase output (chain A, delayed a sample)
         * @param im  receives the quadrature output (chain B)
         */
        void tick(float in, float& re, float& im);
    };
    Hilbert m_hL,   ///< the left channel's splitter
            m_hR;   ///< the right channel's splitter
    double m_sr = 48000.0,   ///< sample rate in Hz
           m_ph = 0.0;       ///< the oscillator's phase in cycles, advanced by shift / sr per sample and reset when it runs far
    float  m_shift = 0.0f,   ///< the shift in Hz
           m_mix = 0.0f;     ///< wet against dry, 0 .. 1
};

/**
 * @brief A pair of tuned feedback combs: the metallic, hull-like resonance of the Cosmos.
 *
 * One delay line per channel, its length the period of the frequency asked for (the right one 0.3 %
 * longer, for width), with a one-pole low pass in the loop and the output scaled so the resonant
 * peak stays near unity gain whatever the feedback. The delay glides towards its target, so a
 * moving frequency bends rather than clicks. At mix 0 the lines are still fed, so the ring is there
 * the moment the mix comes up.
 */
class CombResonator {
public:
    /**
     * @brief Sizes the delay lines for 20 Hz at this rate and clears them.
     * @param sampleRate  the engine's rate in Hz
     */
    void prepare(double sampleRate);
    /**
     * @brief The comb's tuning, feedback and mix, once a block.
     * @param freqHz    the fundamental in Hz, clamped 20 .. 4000
     * @param feedback  the loop gain, clamped 0 .. 0.97
     * @param mix       0 .. 1 wet against dry
     */
    void set(float freqHz, float feedback, float mix);
    /**
     * @brief Runs a block in place.
     * @param L  left channel, n samples, replaced by the mix
     * @param R  right channel, likewise
     * @param n  samples in the block
     */
    void process(float* L, float* R, int n);
private:
    std::vector<float> m_bufL,   ///< the left delay line, a power-of-two ring
                       m_bufR;   ///< the right delay line
    int    m_mask = 0,   ///< ring size - 1
           m_w = 0;      ///< the write index, shared by both lines
    double m_sr = 48000.0;   ///< sample rate in Hz
    float  m_dL = 100.0f,    ///< target delay of the left line in samples, sr / freq
           m_dR = 100.0f,    ///< target delay of the right line, 0.3 % longer
           m_dLcur = 0.0f,   ///< the left delay as it glides towards m_dL (0 before the first set())
           m_dRcur = 0.0f;   ///< the right delay as it glides
    float  m_fb = 0.8f,    ///< the loop gain
           m_mix = 0.0f,   ///< wet against dry
           m_lpL = 0.0f,   ///< the left loop's one-pole low-pass state
           m_lpR = 0.0f;   ///< the right loop's low-pass state
};

/**
 * @brief Three formants morphing slowly between vowels: the alien choir of the Cosmos.
 *
 * Three band passes per channel sit on the formants of a vowel, and a Drifter wanders the vowel
 * through a-e-i-o-u at the rate given, the frequencies interpolated geometrically between the
 * neighbouring vowels and recomputed every 64 samples. The right channel's formants sit one per
 * cent higher for width. At mix 0 nothing is computed.
 */
class VowelFilter {
public:
    /**
     * @brief Takes the rate, seeds the drift and resets the filters.
     * @param sampleRate  the engine's rate in Hz
     * @param seed        seeds the filter's own generator, which drives the vowel drift
     */
    void prepare(double sampleRate, uint64_t seed);
    /**
     * @brief The mix and the morph rate, once a block.
     * @param mix     0 .. 1 wet against dry
     * @param rateHz  how fast the vowel wanders, in new targets per second (a Drifter's rate)
     */
    void set(float mix, float rateHz) { m_mix = clampv(mix, 0.0f, 1.0f); m_rate = rateHz; }
    /**
     * @brief Runs a block in place.
     * @param L  left channel, n samples, replaced by the mix
     * @param R  right channel, likewise
     * @param n  samples in the block
     */
    void process(float* L, float* R, int n);
private:
    Svf     m_fL[3],   ///< the three formant band passes, left
            m_fR[3];   ///< the three formant band passes, right (one per cent higher)
    Drifter m_morph;   ///< the vowel position, -1 .. 1 mapped onto the five vowels
    Rng     m_rng;     ///< the filter's own generator, for the drift
    double  m_sr = 48000.0;   ///< sample rate in Hz
    float   m_mix = 0.0f,     ///< wet against dry
            m_rate = 0.05f;   ///< the drift's rate in Hz
    int     m_counter = 0;   ///< samples until the formant coefficients are recomputed (every 64)
};

/**
 * @brief Granular two-head pitch shifter: the transposition in the shimmer feedback loop.
 *
 * One delay line read by two heads half a cycle apart under complementary sine windows; the heads
 * run towards or away from the write head at the rate the interval asks for and wrap every 80 ms,
 * so the pitch changes and the delay does not. Mono: the shimmer loop runs one per channel.
 */
class PitchShifter {
public:
    /**
     * @brief Sizes the delay line (0.2 s) and sets the 80 ms window.
     * @param sampleRate  the engine's rate in Hz
     */
    void prepare(double sampleRate);
    /**
     * @brief The interval, once a block.
     * @param st  semitones, positive up; the read rate becomes 2^(st / 12)
     */
    void setSemitones(float st);
    /**
     * @brief Shifts a block; wet only.
     * @param in   n input samples
     * @param out  n shifted samples (may not be the input)
     * @param n    samples in the block
     */
    void process(const float* in, float* out, int n);
private:
    std::vector<float> m_buf;   ///< the delay line, a power-of-two ring
    int    m_mask = 0,   ///< ring size - 1
           m_w = 0;      ///< the write index
    double m_sr = 48000.0;   ///< sample rate in Hz
    float  m_window = 3840.0f,   ///< the heads' travel in samples, 80 ms
           m_rate = 1.0f,        ///< the read rate, 2^(semitones / 12)
           m_pos = 0.0f;         ///< the first head's position in its window, 0 .. 1; the second is half a window on
};

/**
 * @brief A complex radix-2 FFT of a fixed power-of-two size, in place, with its tables built once.
 *
 * Stateless after construction, so one instance may be shared by any number of callers and
 * threads. The forward transform turns by e^-i, the inverse by e^+i and divides by n, so the round
 * trip returns what went in. For a real signal use RealFft, which does the same for half the work.
 */
class Fft {
public:
    /**
     * @brief Builds the twiddle and bit-reversal tables for a size.
     * @param n  the transform length, a power of two (2048 by default)
     */
    explicit Fft(int n = 2048);
    /**
     * @brief Transforms in place.
     * @param re       n real parts, replaced by the spectrum's (or the signal's, on the way back)
     * @param im       n imaginary parts, likewise
     * @param inverse  false for the forward transform, true for the inverse scaled by 1 / n
     */
    void transform(float* re, float* im, bool inverse) const;
    /**
     * @brief The transform length.
     * @return  n as given to the constructor
     */
    int size() const { return m_n; }
private:
    int m_n;   ///< the transform length
    std::vector<float> m_cos,   ///< cos(2 pi k / n), k = 0 .. n/2 - 1
                       m_sin;   ///< sin(2 pi k / n), the same range
    std::vector<int>   m_rev;   ///< the bit-reversal permutation of 0 .. n - 1
};

/**
 * @brief The same spectrum for half the work, when the signal is real -- which in this instrument it
 *        always is (13.09.2026).
 *
 * VTune on the dearest preset of the library put Fft::transform at 2.16
 * seconds of processor against 0.32 for the next thing on the list, and every one of its callers
 * had just written a row of zeros into the imaginary half: the Nebula, the shifter, the Memory,
 * the spectral source. Going back the other way they all build a spectrum that is conjugate
 * symmetric on purpose, so that direction was paying twice as well.
 *
 * The method is the textbook one. N real samples are read as N/2 COMPLEX ones by taking them in
 * pairs, z[k] = x[2k] + i x[2k+1]; one complex transform of half the length then holds the even
 * and the odd samples' spectra folded together, and they come apart again with one rotation per
 * bin: X[k] = (Z[k] + conj(Z[M-k]))/2 - i (Z[k] - conj(Z[M-k]))/2 * e^(-2 pi i k / N). The
 * inverse undoes exactly that and hands the pairs back.
 *
 * `x` may be the same array as `re` -- every write lands on a bin that step has already read --
 * which is what lets a caller keep the one buffer it had.
 *
 * The inverse keeps a scratch half, so an instance is one thread's (Fft itself is stateless and
 * may be shared; this is not). It is there because the last step of the inverse interleaves two
 * half-length arrays into one, and that shuffle cannot be done in place by walking it either way:
 * writing the pair for index k lands on m + (2k - m), which is the imaginary part index 2k - m
 * still has to read. Measured the hard way -- the round trip came back 1, 5, 3, 7, 5, 7, 7, 8 for
 * 1 to 8, every odd sample taken from two places further on.
 */
class RealFft {
public:
    /**
     * @brief Builds the half-length complex transform, the rotation table and the scratch half.
     * @param n  the real transform length, a power of two (2048 by default)
     */
    explicit RealFft(int n = 2048);
    /**
     * @brief The transform length.
     * @return  n as given to the constructor
     */
    int size() const { return m_n; }
    /**
     * @brief The forward transform of a real signal.
     *
     * x[0 .. n-1] real in; re/im[0 .. n/2] out. The bins above n/2 are filled with the conjugate
     * mirror as well, so a caller that walks the whole array reads what Fft would have left there.
     * @param x   n real samples; may be the same array as re
     * @param re  n floats, receives the real parts of the spectrum
     * @param im  n floats, receives the imaginary parts
     */
    void forward(const float* x, float* re, float* im) const;
    /**
     * @brief The inverse transform back to a real signal, using the instance's own scratch half.
     *
     * re/im[0 .. n/2] in (whatever is above n/2 is ignored); x[0 .. n-1] real out. The scaling is
     * Fft's: the round trip returns what went in.
     * @param re  the real parts of the spectrum
     * @param im  the imaginary parts
     * @param x   n floats, receives the signal; may be the same array as re
     */
    void inverse(const float* re, const float* im, float* x) const { inverse(re, im, x, m_work.data()); }
    /**
     * @brief The inverse transform with the scratch half supplied by the caller.
     *
     * The same with the scratch half handed in -- n/2 floats, which may be the `im` array itself,
     * since every write there lands on a bin the step has read. An instance shared between voices
     * (the spectral source keeps one per size for all of them) has to take this one: the other
     * writes into the instance, and a shared instance that writes is a race waiting for the day
     * somebody renders two voices at once.
     * @param re       the real parts of the spectrum
     * @param im       the imaginary parts
     * @param x        n floats, receives the signal; may be the same array as re
     * @param scratch  n/2 floats of working space; may be im itself
     */
    void inverse(const float* re, const float* im, float* x, float* scratch) const;
private:
    int m_n;   ///< the real transform length
    Fft m_half;                     ///< the complex transform of n/2 that does the work
    std::vector<float> m_tc,   ///< cos(2 pi k / n), k = 0 .. n/2
                       m_ts;   ///< e^(-2 pi i k / n), k = 0 .. n/2 -- its sine
    mutable std::vector<float> m_work;   ///< n/2, the inverse's imaginary half; sized in the constructor
};

/**
 * @brief STFT magnitude smearing with random phases: a Paulstretch-like texture of the input.
 *
 * Every Hop samples a Hann frame of N is transformed; each bin's magnitude is smoothed over time
 * with a coefficient set by Smear, the phases are drawn fresh at random, and the frame is
 * resynthesised and overlap-added. At Smear 0 the magnitudes follow the input, at 1 the smoother's
 * time constant is minutes and the spectrum is frozen. Wet only, N samples late.
 */
class Nebula {
public:
    static constexpr int N = 2048,    ///< the frame length and the latency in samples
                         Hop = 512;   ///< samples between frames: a quarter of the frame
    /**
     * @brief Allocates the two channels' buffers, builds the window and seeds the phase generators.
     * @param sampleRate  the engine's rate (unused: the frame is fixed in samples)
     * @param seed        seeds the two channels' phase generators
     */
    void prepare(double sampleRate, uint64_t seed);
    /**
     * @brief The smear, once a block.
     * @param smear  0 = follows the input, 1 = frozen
     */
    void set(float smear);
    /**
     * @brief Wet output only (latency N samples).
     * @param inL   left input, n samples
     * @param inR   right input
     * @param wetL  receives n samples of the left texture
     * @param wetR  receives the right
     * @param n     samples in the block
     */
    void process(const float* inL, const float* inR, float* wetL, float* wetR, int n);
private:
    /** @brief One channel's rings, spectrum and smoothed magnitudes. */
    struct Channel {
        std::vector<float> m_in,    ///< the input ring, N samples
                           m_out,   ///< the overlap-add output ring, 2 N samples
                           m_mag,   ///< the smoothed magnitudes, N / 2 + 1 bins
                           m_re,    ///< the frame and then its spectrum's real parts, N
                           m_im;    ///< the spectrum's imaginary parts, N
        int m_inPos = 0,    ///< samples written into `in` so far
            m_outPos = 0;   ///< samples read out of `out` so far
        Rng m_rng;   ///< the channel's own generator for the random phases
    };
    /**
     * @brief One frame of one channel: window, transform, smooth the magnitudes, random phases, back, overlap-add.
     * @param c  the channel
     */
    void frame(Channel& c);
    Channel m_ch[2];   ///< left and right
    RealFft m_fft{ N };   ///< the real transform of N
    std::vector<float> m_window;   ///< the Hann window, N samples, used on the way in and out
    int     m_hopCounter = 0;   ///< samples since the last frame; a frame runs when it reaches Hop
    float   m_alpha = 1.0f;   ///< the magnitude smoother's coefficient, (1 - smear)^2 floored at 2e-4
};

} // namespace ambient
