/**
 * @file CycleTable.h
 * @brief The classic wavetable: a stack of single cycles, played as samples.
 *
 * The Harmonic type reads a table as a sequence of spectra, thirty-two partial amplitudes a frame,
 * and builds the sound from a phasor bank. That is alias-free and gives every partial a life of its
 * own, but it throws away what makes a table from Serum, Vital or Hive what it is: the phases -- so
 * the shape of the wave -- and everything above the thirty-second partial. A saw of thirty-two
 * partials is a soft saw, and a formant sweep drawn into a table is a different sweep once its
 * phases are gone.
 *
 * This is the other kind. A table is up to 256 single cycles (2048 samples each in the common file
 * layout), read with a phase accumulator and a four-point interpolation, one frame blended into the
 * next by Position. What keeps a high note from aliasing is the classic remedy: every frame is stored
 * at ten resolutions, each an octave poorer than the one before (512 harmonics in 4096 samples, 256
 * in 2048, down to a single sine in 32), and a note reads the richest copy whose highest harmonic
 * still lies below Nyquist.
 *
 * Every copy holds eight samples per cycle of its highest harmonic. Four was tried first, and at four
 * the interpolation's own images are what is heard: a saw of 512 harmonics played at 2 kHz put
 * everything between its harmonics at -41 dB, all of it the images of the top few partials folded
 * back from above Nyquist. Doubling the samples costs memory -- about four megabytes for a table of
 * 256 frames -- and nothing at all in the loop.
 *
 * The copies are made from each frame's Fourier coefficients rather than by filtering samples, so a
 * level is the same waveform with its upper octave of harmonics taken away -- exactly, and with the
 * phases of everything that remains. A frame's DC is dropped on the way.
 */
#pragma once
#include <complex>
#include <cstddef>
#include <vector>

namespace ambient {

/**
 * @brief A wavetable of single cycles at ten band-limited resolutions, with the reader's arithmetic.
 *
 * Built once (build() from samples, buildFromHarmonics() from spectra) on the message thread or at
 * prepare; read on the audio thread with sample(), which never allocates. Every frame is stored at
 * every level, one stored cycle being levelLength(level) samples plus kGuard guard samples, and all
 * of them lie in `data` in level-major order, the levels starting at `offset`.
 */
struct CycleTable {
    static constexpr int kLen       = 2048;   ///< samples per cycle in the common file layout
    static constexpr int kStoreLen  = 4096;   ///< samples per stored cycle at the finest level
    static constexpr int kMaxFrames = 256;    ///< the most frames a table keeps; a longer file is thinned to this
    static constexpr int kLevels    = 10;     ///< 4096 .. 32 samples, 512 .. 1 harmonics
    static constexpr int kGuard     = 3;      ///< one sample stored before each cycle, two after it
    /**
     * @brief The RMS the loudest frame of a table is scaled to.
     *
     * It is what the Harmonic type gives every
     * frame (its bank is normalised to half amplitude per unit of spectrum energy), so one file
     * played by either type enters the mix at the same level.
     */
    static constexpr float kTargetRms = 0.35355339f;

    int frames = 0;   ///< how many frames the table holds; 0 for an empty table
    int offset[kLevels] = {};   ///< where each level's cycles begin in `data`
    std::vector<float> data;   ///< every frame at every level, guards included, level-major

    /**
     * @brief Samples per stored cycle at a level: 4096 halved per level, never under 32.
     * @param level  0 (finest) .. kLevels - 1
     * @return       the cycle length in samples, a power of two
     */
    static int levelLength(int level)    { const int n = kStoreLen >> level; return n < 32 ? 32 : n; }
    /**
     * @brief Highest harmonic a level keeps: 512 halved per level, never under 1 -- an eighth of the level's length.
     * @param level  0 (finest) .. kLevels - 1
     * @return       the harmonic count
     */
    static int levelHarmonics(int level) { const int h = (kStoreLen / 8) >> level; return h < 1 ? 1 : h; }

    /**
     * @brief Whether there is nothing to play.
     * @return  true when no frame was built
     */
    bool empty() const { return frames <= 0; }
    /**
     * @brief The stored cycle of frame f at a level; readable from index -1 to levelLength(level) + 1.
     * @param level  0 .. kLevels - 1
     * @param f      the frame, 0 .. frames - 1
     * @return       a pointer at the cycle's sample 0, with the guards either side of it
     */
    const float* cycle(int level, int f) const
    {
        return data.data() + static_cast<size_t>(offset[level])
             + static_cast<size_t>(f) * static_cast<size_t>(levelLength(level) + kGuard) + 1;
    }
    /**
     * @brief One sample of frame f at a level, at `phase` in [0, 1): Catmull-Rom through four samples.
     *
     * Audio thread; the guards make the four-point read safe at both ends of the cycle.
     * @param level  the level cycleLevelFor() chose for the note
     * @param f      the frame, 0 .. frames - 1
     * @param phase  the phase accumulator's value in cycles, [0, 1)
     * @return       the interpolated sample
     */
    float sample(int level, int f, double phase) const
    {
        const int len = levelLength(level);
        const double x = phase * static_cast<double>(len);
        int i = static_cast<int>(x);
        if (i >= len) i = len - 1;
        if (i < 0) i = 0;
        const float t = static_cast<float>(x - static_cast<double>(i));
        const float* c = cycle(level, f);
        const float y0 = c[i - 1], y1 = c[i], y2 = c[i + 1], y3 = c[i + 2];
        const float a = 0.5f * (y2 - y0);
        const float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float d = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((d * t + b) * t + a) * t + y1;
    }

    /**
     * @brief Builds the table from a mono file of cycles laid end to end.
     *
     * Frames of `cycleLen` samples laid end to end. 2048 is the layout of Serum, Vital and Hive, but
     * any length from 8 samples is read (600 is an Adventure Kid single cycle, 256 a WaveEdit bank).
     * More than kMaxFrames frames are thinned evenly. False if there is not one whole frame, or if
     * there is nothing in the frames but silence.
     *
     * Each cycle is analysed into its harmonics (an FFT for a power-of-two length, the plain sum
     * otherwise) and the table is then made by buildFromHarmonics(). Not for the audio thread.
     * @param mono      the samples, n of them
     * @param n         how many samples there are
     * @param cycleLen  samples per cycle, at least 8; see detectCycleLength()
     * @return          true when a table was built; on false the table is left empty
     */
    bool build(const float* mono, int n, int cycleLen);
    /**
     * @brief Builds the table from spectra.
     *
     * From Fourier coefficients, one vector per frame: element h-1 is harmonic h as the complex
     * amplitude of a cosine (|c| cos(2 pi h t + arg c)). What the built-in tables are written in.
     *
     * The loudest frame (by Parseval) is brought to kTargetRms and every frame scaled by the same
     * gain; each level's copy is synthesised by an inverse FFT of the harmonics it keeps. At most
     * kMaxFrames frames are taken. Not for the audio thread.
     * @param frameCoefficients  one vector per frame; harmonics beyond the 512th are ignored
     * @return                   true when a table was built; false for no frames or all-silent ones, the table then empty
     */
    bool buildFromHarmonics(const std::vector<std::vector<std::complex<double>>>& frameCoefficients);
    /** @brief Empties the table and frees its memory. */
    void clear() { frames = 0; data.clear(); }
};

/**
 * @brief The level a cycle at `hz` reads: the richest whose highest harmonic lies below Nyquist.
 *
 * It moves
 * toward a richer level only with ten per cent to spare, so a pitch that hovers on a boundary does
 * not flip between two levels. `current` < 0 means there is no previous level.
 * @param hz          the cycle's frequency in Hz
 * @param sampleRate  the engine's rate in Hz
 * @param current     the level the voice read last time, or a negative number for none
 * @return            0 .. CycleTable::kLevels - 1
 */
int cycleLevelFor(double hz, double sampleRate, int current);

/**
 * @brief How long one cycle is in a file that does not say so itself.
 *
 * A multiple of 2048 is frames of 2048,
 * except at WaveEdit's size (64 x 256 samples), where the wave's continuity decides between 64
 * cycles of 256 and eight frames of 2048. A file of at most 4096 samples that is no multiple of 2048
 * is one cycle of its own length; a longer one is cut at 1024, 512 or 256 if one of them divides it.
 * @param mono  the file's samples
 * @param n     how many there are
 * @return      the cycle length in samples to hand to CycleTable::build(); 0 for no samples at all
 */
int detectCycleLength(const float* mono, int n);

/**
 * @brief The five tables of the Wavetable type that need no file.
 *
 * The tables the Wavetable type offers built in, index 0 .. 4: Classic (sine, triangle, saw, square,
 * pulse, band-limited to the full 512 harmonics), and Organ, Vocal, Glass and Metal written from the
 * Harmonic type's spectra. Built on first use -- which Source slots do in prepare(), never on the
 * audio thread.
 * @param index  0 Classic, 1 Organ, 2 Vocal, 3 Glass, 4 Metal; anything outside is clamped
 * @return       the table, which lives for the process
 */
const CycleTable& builtinCycleTable(int index);

} // namespace ambient
