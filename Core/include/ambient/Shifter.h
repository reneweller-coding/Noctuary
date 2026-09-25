/**
 * @file Shifter.h
 * @brief A pitch shifter in the spectrum, for loops.
 *
 * A pitch shifter inside a feedback loop hears its own output again on every pass, and whatever it
 * does to a sound once it does a hundred times. The granular shifter the shimmer used -- two read
 * heads crossfading every 80 ms -- lays an amplitude modulation and a comb on every pass, and a
 * shimmer grew grainier and hollower the higher it rose. This one shifts in the spectrum instead,
 * after Laroche and Dolson ("New phase-vocoder techniques for pitch-shifting, harmonizing and other
 * exotic effects", IEEE WASPAA 1999): every spectral peak is moved to its shifted frequency together
 * with the bins around it, its phase advanced from one frame to the next at the shifted frequency,
 * and the bins of its region turned with it (the identity phase locking of Laroche and Dolson,
 * "Improved phase vocoder time-scale modification of audio", IEEE TSAP 1999). A steady partial comes
 * out as a steady partial -- no crossfade, no modulation -- and so does its hundredth pass.
 *
 * kN samples late, which a loop that circles for seconds does not notice. Buffers are allocated in
 * prepare() only; process() may run in place.
 */
#pragma once
#include "Dsp.h"
#include "Cosmos.h"
#include <vector>

namespace ambient {

/**
 * @brief The phase-vocoder shifter: two channels, a kN-point frame every kHop samples.
 *
 * Used by the Cosmos shimmer (Shimmer Mode = Spectral) and by the cloud's loop (Cloud Shift).
 * Hann analysis and synthesis at a quarter-frame hop; the overlap gain is folded in so a shift of
 * zero semitones comes out at the level it went in.
 */
class SpectralShifter {
public:
    static constexpr int kN = 4096,     ///< @brief frame length in samples (85 ms at 48 kHz), and the latency
                         kHop = 1024;   ///< hop in samples: a quarter frame, so four frames overlap
    /**
     * @brief Allocates the rings, spectra and window and clears the state.
     * @param sampleRate  unused: the frame is fixed in samples, so the shift is exact at every rate
     */
    void prepare(double sampleRate);
    /** @brief Silences both channels' rings and spectra and restarts the hop counter. */
    void reset();
    /**
     * @brief The shift in semitones, from the next frame on.
     * @param semitones  -36 .. +36 (clamped); 0 passes the signal through, kN samples late
     */
    void setSemitones(float semitones);
    /**
     * @brief Shifts a block, stereo, kN samples late.
     *
     * Every sample is pushed into the input ring and pulled from the overlap-add ring; every kHop
     * samples one frame per channel is analysed and resynthesised (frame()). In place is allowed:
     * each input sample is read before its output is written.
     *
     * @param inL   left input
     * @param inR   right input
     * @param outL  left output (may alias @p inL)
     * @param outR  right output (may alias @p inR)
     * @param n     samples
     */
    void process(const float* inL, const float* inR, float* outL, float* outR, int n);
    /** @return the delay through the shifter in samples, always kN */
    int  latency() const { return kN; }
private:
    /** @brief One channel's rings and spectra. */
    struct Channel {
        std::vector<float> in,    ///< @brief the input ring (kN)
                           out;   ///< the input ring (kN) and the overlap-add (2 kN)
        std::vector<float> re,   ///< @brief real part of the frame being worked on (kN)
                           im;   ///< the frame being worked on (kN): its imaginary part
        std::vector<float> power;                      ///< |X|^2 of the analysis, kN / 2 + 1
        std::vector<float> prevRe,   ///< @brief real part of the previous frame's analysis spectrum
                           prevIm;   ///< the previous frame's analysis spectrum, imaginary part
        std::vector<float> accRe,   ///< @brief real part of this frame's shifted spectrum
                           accIm;   ///< this frame's shifted spectrum, imaginary part
        std::vector<float> prevOutRe,   ///< @brief real part of the previous frame's shifted spectrum
                           prevOutIm;   ///< and the previous frame's, imaginary part
        int inPos = 0,    ///< @brief write position in the input ring
            outPos = 0;   ///< read position in the overlap-add ring
    };
    /**
     * @brief One frame of one channel: window, transform, move every peak's region to its shifted
     *        bin with the phase that continues its own frequency there, transform back, overlap-add.
     *
     * Peaks are local maxima over two bins either side and no more than 70 dB under the loudest;
     * a peak's region runs to the trough before the next peak. Called from process() every kHop
     * samples.
     *
     * @param c  the channel
     */
    void frame(Channel& c);
    Channel ch_[2];               ///< left and right
    RealFft fft_{ kN };           ///< the shared kN-point real transform
    std::vector<float> window_;   ///< the Hann window, kN samples, for analysis and synthesis alike
    std::vector<int>   peaks_;    ///< scratch: the bins of this frame's peaks, kN / 2 entries
    double  ratio_ = 2.0;         ///< the frequency ratio of the current shift, 2^(semitones / 12)
    int     hopCounter_ = 0;      ///< samples since the last frame; a frame is due at kHop
};

} // namespace ambient
