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
 * N samples late, which a loop that circles for seconds does not notice. Buffers are allocated in
 * prepare() only; process() may run in place.
 * @author Rene Weller
 */
#pragma once
#include "Dsp.h"
#include "Cosmos.h"
#include <vector>

namespace ambient
{

/**
 * @brief The phase-vocoder shifter: two channels, a N-point frame every Hop samples.
 *
 * Used by the Cosmos shimmer (Shimmer Mode = Spectral) and by the cloud's loop (Cloud Shift).
 * Hann analysis and synthesis at a quarter-frame hop; the overlap gain is folded in so a shift of
 * zero semitones comes out at the level it went in.
 */
class SpectralShifter
{
public:
    static constexpr int N = 4096,   ///< @brief frame length in samples (85 ms at 48 kHz), and the latency
        Hop = 1024;                  ///< hop in samples: a quarter frame, so four frames overlap
    /**
     * @brief Allocates the rings, spectra and window and clears the state.
     * @param[in] sampleRate  unused: the frame is fixed in samples, so the shift is exact at every rate
     */
    void prepare( double sampleRate );
    /** @brief Silences both channels' rings and spectra and restarts the hop counter. */
    void reset();
    /**
     * @brief The shift in semitones, from the next frame on.
     * @param[in] semitones  -36 .. +36 (clamped); 0 passes the signal through, N samples late
     */
    void setSemitones( float semitones );
    /**
     * @brief Shifts a block, stereo, N samples late.
     *
     * Every sample is pushed into the input ring and pulled from the overlap-add ring; every Hop
     * samples one frame per channel is analysed and resynthesised (frame()). In place is allowed:
     * each input sample is read before its output is written.
     * @param[in]     inL   left input
     * @param[in]     inR   right input
     * @param[in,out] outL  left output (may alias @p inL)
     * @param[in,out] outR  right output (may alias @p inR)
     * @param[in]     n     samples
     */
    void process( const float *inL, const float *inR, float *outL, float *outR, int n );
    /** @return the delay through the shifter in samples, always N */
    int latency() const { return N; }

private:
    /** @brief One channel's rings and spectra. */
    struct Channel
    {
        std::vector<float> m_in,          ///< @brief the input ring (N)
            m_out;                        ///< the input ring (N) and the overlap-add (2 N)
        std::vector<float> m_re,          ///< @brief real part of the frame being worked on (N)
            m_im;                         ///< the frame being worked on (N): its imaginary part
        std::vector<float> m_power;       ///< |X|^2 of the analysis, N / 2 + 1
        std::vector<float> m_prevRe,      ///< @brief real part of the previous frame's analysis spectrum
            m_prevIm;                     ///< the previous frame's analysis spectrum, imaginary part
        std::vector<float> m_accRe,       ///< @brief real part of this frame's shifted spectrum
            m_accIm;                      ///< this frame's shifted spectrum, imaginary part
        std::vector<float> m_prevOutRe,   ///< @brief real part of the previous frame's shifted spectrum
            m_prevOutIm;                  ///< and the previous frame's, imaginary part
        int m_inPos = 0,                  ///< @brief write position in the input ring
            m_outPos = 0;                 ///< read position in the overlap-add ring
    };
    /**
     * @brief One frame of one channel: window, transform, move every peak's region to its shifted
     *        bin with the phase that continues its own frequency there, transform back, overlap-add.
     *
     * Peaks are local maxima over two bins either side and no more than 70 dB under the loudest;
     * a peak's region runs to the trough before the next peak. Called from process() every Hop
     * samples.
     * @param[in,out] c  the channel
     */
    void frame( Channel &c );
    Channel            m_ch[2];            ///< left and right
    RealFft            m_fft{ N };         ///< the shared N-point real transform
    std::vector<float> m_window;           ///< the Hann window, N samples, for analysis and synthesis alike
    std::vector<int>   m_peaks;            ///< scratch: the bins of this frame's peaks, N / 2 entries
    double             m_ratio = 2.0;      ///< the frequency ratio of the current shift, 2^(semitones / 12)
    int                m_hopCounter = 0;   ///< samples since the last frame; a frame is due at Hop
};

}   // namespace ambient
