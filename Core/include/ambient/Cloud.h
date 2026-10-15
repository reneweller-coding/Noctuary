/**
 * @file Cloud.h
 * @brief The granular cloud on the far plane.
 *
 * Grains of the recent foreground, scattered back in time, transposed and dropped into the far
 * reverb. Since 12.09.2026 it also does what Absynth's Aetherizer is known for -- grains that hear
 * their own echoes, tuned resonance, grains in groups -- built from the parts the literature does
 * better where it has them:
 * ```{.unparsed}
 *   Feedback   The cloud's output goes back into its own history: grains of grains, and the
 *              transpositions stack on every pass. The loop is low-passed at Tone, saturated with
 *              first-order antiderivative antialiasing (Parker, Zavalishin and Le Bihan, DAFx 2016;
 *              Bilbao, Esqueda, Parker and Valimaki, IEEE SPL 2017), so the partials a saturation
 *              makes are not folded back below Nyquist again on every pass, and throttled by its
 *              own level like the shimmer loop, so at 1 it holds instead of running away.
 *   Shift      The loop transposed on every pass -- an octave, a fifth or a fourth, up or down --
 *              in the spectrum (Shifter.h), so a spiral of grains climbs or sinks cleanly instead of
 *              growing grainier on every turn.
 *   Scatter    Transpositions drawn from the intervals of the scale that is playing, measured from
 *              the conductor's root and opened from the most consonant outwards -- octaves, then
 *              fifths and fourths, then the rest -- until at a half every interval of the scale
 *              within an octave is in; beyond a half the grains begin to leave the scale, and at 1
 *              they are free. The Aetherizer rasters to thirteen fixed twelve-tone menus; this
 *              follows the instrument's own tuning, just intonation and Scala files included.
 *   Resonance  Resonators on the scale's notes (the whole scale, the chord on the root, fifths or
 *              octaves). Each grain excites one of them, and the resonator rings on after its grain
 *              has ended -- the Aetherizer's filter lives and dies with its grain, which is why its
 *              manual has a low comb speak only with long grains. Band is the phasor filter of
 *              Mathews and Smith (SMAC 2003): a rotation, so its level does not move when its
 *              frequency does, and a new root glides in instead of clicking. Comb is a tuned
 *              feedback comb, every harmonic of the note ringing.
 *   Swarm      The onsets as a self-exciting (Hawkes) process: every grain raises the chance of the
 *              next for a quarter of a second, so the grains come in flocks at the same mean rate.
 *              At 0 they are the Poisson stream the cloud always had.
 * ```
 * The grain loop is the vector kernel of the texture sources (GrainRing.h). Buffers are allocated
 * in prepare() only.
 * @author Rene Weller
 */
#pragma once
#include "Dsp.h"
#include "Adaa.h"
#include "Shifter.h"
#include "Tuning.h"
#include "GrainRing.h"
#include <vector>
#include <cstdint>

namespace ambient {

/**
 * @brief The cloud: a history of the far bus, grains read out of it, the loop, the resonators.
 *
 * The engine owns one. prepare() allocates eight seconds of stereo history and the resonators'
 * rings; the set*() calls take the parameters once a block; setHarmony() tells it the scale and
 * the root the scatter and the resonators follow; process() runs on the audio thread in
 * sub-blocks of Sub samples -- the loop hands its output back one sub-block late, and the
 * resonators run per sub-block on the buses the grains were rendered into. quiet() lets the
 * engine skip process() when there is nothing to hear.
 */
class GrainCloud {
public:
    static constexpr int MaxGrains = 64;   ///< grains that may sound at once; a spawn beyond this is dropped
    static constexpr int MaxRes = 12;      ///< resonators at most, and buses for the grains to be routed into
    static constexpr int Sub = 64;          ///< the loop's delay and the resonators' block, in samples

    /**
     * @brief Allocates the history and the rings, seeds the generator and clears everything.
     *
     * The history is eight seconds plus slack (a transposed grain reads faster than the head
     * writes). The loop and resonator coefficients that depend on the rate are recomputed, and if
     * a harmony is already known the resonators are rebuilt for the new rate. Not for the audio thread.
     * @param[in] sampleRate  the engine's rate in Hz
     * @param[in] seed        seeds the cloud's own generator: onsets, grain lengths, pans and transpositions
     */
    void prepare(double sampleRate, uint64_t seed);
    /**
     * @brief The five grain parameters, once a block; each is clamped.
     * @param[in] densityPerSec  mean grains per second, 0.1 .. 200
     * @param[in] sizeMs         mean grain length in ms, 5 .. 2000 (each grain draws 0.7 .. 1.3 of it)
     * @param[in] pitch          0 .. 1: the chance a grain takes a fixed transposition (mostly an octave up or down,
     *                           sometimes a fifth or two octaves up) instead of a two per cent detune
     * @param[in] spraySec       how far back in the history a grain may start, 0 .. 3.5 s
     * @param[in] level          the cloud's output gain, 0 .. 2, glided across the sub-block
     */
    void set(float densityPerSec, float sizeMs, float pitch, float spraySec, float level);
    /**
     * @brief The feedback loop, once a block.
     *
     * Feedback 0..1 (1 holds at the loop's ceiling) and the loop's low-pass in hertz.
     * @param[in] feedback  0 .. 1, scaled by the loop gain inside and throttled by the loop's own level
     * @param[in] toneHz    the loop's one-pole low pass, 100 .. 20000 Hz (and under 0.45 of the rate)
     */
    void setLoop(float feedback, float toneHz);
    /**
     * @brief The loop's transposition on every pass, in semitones; 0 is off.
     *
     * Switching the shift on from 0 resets the spectral shifter, so it starts from silence.
     * @param[in] semitones  -24 .. 24; 0 bypasses the shifter
     */
    void setShift(float semitones);
    /**
     * @brief Transposition of every grain in semitones, and the scatter over the scale's intervals, 0..1.
     * @param[in] transposeSemitones  -24 .. 24, applied to every grain as a ratio
     * @param[in] scatter             0 .. 1; see the file comment and drawRatio()
     */
    void setScatter(float transposeSemitones, float scatter);
    /**
     * @brief 0: Poisson onsets, as always; towards 1 the onsets excite each other and come in flocks.
     * @param[in] swarm  0 .. 1, the Hawkes branching ratio being 0.9 of it
     */
    void setSwarm(float swarm);
    /**
     * @brief Mix 0..1; mode 0 Band, 1 Comb; notes 0 Scale, 1 Chord, 2 Fifths, 3 Octaves; ring time to -60 dB.
     *
     * A change of mode clears the other kind's state; a change of notes rebuilds the resonators if
     * the harmony is known.
     * @param[in] amount        the resonators' mix against the dry grains, 0 .. 1, glided
     * @param[in] mode          0 the phasor band pass, 1 the feedback comb
     * @param[in] notes         which notes get a resonator: 0 the scale, 1 the chord on the root, 2 root and fifth, 3
     *                          octaves
     * @param[in] decaySeconds  time to -60 dB, 0.05 .. 30 s
     */
    void setResonators(float amount, int mode, int notes, float decaySeconds);
    /**
     * @brief The music the scatter and the resonators follow.
     *
     * The music the scatter and the resonators follow: the scale, the frequency of its tonic and of
     * the conductor's root. Cheap to call on every block -- the tables are rebuilt only when one of
     * the three has moved.
     * @param[in] scale    the scale that is playing (Tuning.h); a degree of zero is taken as the tonic
     * @param[in] tonicHz  the frequency of the scale's first degree in Hz; ignored if not above 1 or not finite
     * @param[in] rootHz   the conductor's root in Hz, likewise; the scatter's intervals are measured from it
     */
    void setHarmony(const FixedScale& scale, double tonicHz, double rootHz);
    /**
     * @brief One block: writes the history, spawns and renders grains, runs the resonators and the loop.
     *
     * Feeds the history with both channels and ADDS the cloud to outL/outR. A grain reads the left
     * history into its left gain and the right into its right, so a wide field keeps its sides in
     * the cloud; identical channels give exactly the cloud the mono sum would.
     *
     * Audio thread, any n; the work runs in sub-blocks of Sub samples.
     * @param[in]     inL   left input, n samples (the far bus)
     * @param[in]     inR   right input
     * @param[in,out] outL  left output; the cloud is added to what is there
     * @param[in,out] outR  right output, likewise
     * @param[in]     n     samples in the block
     */
    void process(const float* inL, const float* inR, float* outL, float* outR, int n);
    /**
     * @brief No grain sounding and nothing left ringing: the engine may stop calling process().
     * @return  true when m_live is 0 and the quarter-second tail after the last sound has run out
     */
    bool quiet() const { return m_live == 0 && m_tail <= 0; }

    /**
     * @name For the self test.
     * @{ */
    /**
     * @brief How many scatter intervals the current harmony gives.
     * @return  the length of the interval list, at most MaxIv
     */
    int       intervalCount() const { return m_numIv; }
    /**
     * @brief One scatter interval, most consonant first.
     * @param[in] k  0 .. intervalCount() - 1
     * @return the interval as a frequency ratio, within an octave either way
     */
    float     interval(int k) const { return m_iv[k]; }
    /**
     * @brief How many resonators the current harmony and notes setting give.
     * @return  0 .. MaxRes
     */
    int       resonatorCount() const { return m_numRes; }
    /**
     * @brief One resonator's note.
     * @param[in] k  0 .. resonatorCount() - 1
     * @return its frequency in Hz
     */
    float     resonatorHz(int k) const { return m_resHz[k]; }
    /**
     * @brief How many grains have been started since prepare().
     * @return  the count, for checking the onset statistics
     */
    long long grainsStarted() const { return m_started; }
    /** @} */
private:
    static constexpr int MaxIv = 3 * FixedScale::Max;   ///< the most scatter intervals: every degree in three octaves
    static constexpr int FbLen = 256;       ///< the loop's ring: a power of two, at least two sub-blocks
    static constexpr int CombLen = 1024;    ///< a comb resonator's ring (down to 47 Hz)
    /**
     * @brief Draws the onsets of one sub-block and spawns a grain at each.
     *
     * The hazard -- the intensity integrated over time -- against an exponential draw: Poisson at
     * Swarm 0, a Hawkes process with a quarter-second kernel above it, the base rate reduced so
     * the mean density stays what the knob says.
     * @param[in] w0  the history's write frame at the start of the sub-block
     * @param[in] m   samples in the sub-block
     */
    void   spawnSub(int w0, int m);
    /**
     * @brief Starts one grain: its length, rate, start position, pan and bus.
     *
     * The grain starts far enough behind the write head that it is still behind it when it ends,
     * read faster than the head moves; where the history is too short for that the grain is
     * shortened, and dropped if that leaves under 32 samples. Nothing happens at MaxGrains.
     * @param[in] w0      the history's write frame at the start of the sub-block
     * @param[in] offset  the sample within the sub-block the grain begins at
     */
    void   spawn(int w0, int offset);
    /**
     * @brief The scatter's transposition for one grain.
     *
     * Up to a quarter more and more grains leave the unison; up to a half the intervals open from
     * the most consonant down; beyond a half a growing share is bent by up to an octave either way.
     * @return  a frequency ratio; 1 without scatter or without a harmony
     */
    double drawRatio();
    /**
     * @brief Renders every live grain into its bus for the sub-block and retires the ones that ended.
     * @param[in] m  samples in the sub-block
     */
    void   renderGrains(int m);
    /**
     * @brief Sums the buses, runs the resonators, mixes the cloud into the output and feeds the loop.
     *
     * The resonator mix, the level and the loop gain glide across the sub-block. The loop path is
     * Tone, a DC blocker, the ADAA saturation, the throttle on the loop's own mean level and, when
     * a shift is set, the spectral shifter; its output goes into the loop ring for the next
     * sub-block. The tail counter is refreshed while anything is audible.
     * @param[in]     m     samples in the sub-block
     * @param[in,out] outL  left output for the sub-block; the cloud is added
     * @param[in,out] outR  right output, likewise
     */
    void   mixSub(int m, float* outL, float* outR);
    /**
     * @brief Runs the resonators over their buses and adds them to the wet sum.
     *
     * Band: the phasor filter of Mathews and Smith, one rotation per sample, the angle gliding to
     * its target. Comb: one feedback comb per note, its delay the period less the one-pole's lag,
     * its gain set for -60 dB in the ring time.
     * @param[in]     m     samples in the sub-block
     * @param[in,out] wetL  left wet sum, m samples, added to
     * @param[in,out] wetR  right wet sum, likewise
     */
    void   runResonators(int m, float* wetL, float* wetR);
    /**
     * @brief The scatter intervals from the scale and the root.
     *
     * The root is matched to the nearest degree, the scale rotated to begin there, and every
     * interval within an octave either way (the unison left out) is ordered by consonance, the
     * nearer of two equally consonant ones first.
     */
    void   buildIntervals();
    /**
     * @brief The resonators' notes from the root, the scale and the notes setting.
     *
     * The root is folded into the two octaves above C3; the scale mode takes every degree in both
     * octaves, the least consonant dropped beyond twelve, the chord mode the root, the scale's
     * nearer third and its fifth in two octaves. A resonator that is new starts on its note; an
     * existing one glides to it.
     */
    void   buildResonators();

    std::vector<float> m_hist;                ///< the history, left and right interleaved, m_cap frames
    int       m_cap = 0,   ///< frames in the history: eight seconds and slack
              m_w = 0;     ///< the write frame
    long long m_t = 0;                        ///< samples processed: the loop's ring is indexed by it
    RingGrain m_grains[MaxGrains];   ///< the grains; the first m_live of them are sounding
    int       m_live = 0;   ///< how many grains are sounding
    long long m_started = 0;   ///< grains started since prepare(), for the self test
    Rng       m_rng;   ///< the cloud's own generator
    double    m_sr = 48000.0;   ///< sample rate in Hz
    float     m_density = 12.0f,   ///< mean grains per second
              m_size = 250.0f,     ///< mean grain length in ms
              m_pitch = 0.3f,      ///< chance of a fixed transposition
              m_spray = 0.8f,      ///< how far back a grain may start, in seconds
              m_level = 0.7f;      ///< the output gain the knob asks for
    float     m_levelCur = 0.7f;   ///< the output gain as it glides towards m_level
    int       m_tail = 0;   ///< samples of ring-out left after the last audible sub-block (a quarter of a second)
    bool      m_routing = false;              ///< grains go to their resonators' buses this sub-block
    /**
     * @name onsets
     * @{ */
    float     m_swarm = 0.0f;   ///< the Swarm knob, 0 .. 1
    double    m_hazard = 0.0,        ///< the intensity integrated since the last onset
              m_nextHazard = 0.0,    ///< the exponential draw the hazard is compared against
              m_excite = 0.0,        ///< the Hawkes excitation every onset raises
              m_exciteDecay = 1.0;   ///< the excitation's per-sample decay, a quarter of a second
    /** @} */
    /**
     * @name the loop
     * @{ */
    float     m_feedback = 0.0f,    ///< the Feedback knob, 0 .. 1
              m_toneHz = 5000.0f,   ///< the loop's low-pass frequency in Hz
              m_toneC = 0.5f,       ///< its one-pole coefficient
              m_fbCur = 0.0f;       ///< the loop gain (feedback x the loop gain constant) as it glides
    float     m_fbRingL[FbLen] = {},   ///< the loop's output, left, indexed by m_t; read a sub-block later into the history
              m_fbRingR[FbLen] = {};   ///< the loop's output, right
    float     m_toneL = 0.0f,   ///< the loop low pass's state, left
              m_toneR = 0.0f;   ///< and right
    float     m_dcxL = 0.0f,    ///< the DC blocker's last input, left
              m_dcxR = 0.0f,    ///< and right
              m_dcyL = 0.0f,    ///< the DC blocker's last output, left
              m_dcyR = 0.0f,    ///< and right
              m_dcR = 0.997f;   ///< the blocker's pole, 20 Hz at the rate
    TanhAdaa  m_satL,   ///< the loop's antialiased saturation, left
              m_satR;   ///< and right
    float     m_loopEnv = 0.0f,   ///< the loop's mean level, which throttles the feedback towards the ceiling
              m_envC = 0.0002f;   ///< the envelope's one-pole coefficient, 100 ms
    SpectralShifter m_shift;   ///< the transposition on every pass, when m_shiftSemis is not 0
    float     m_shiftSemis = 0.0f;   ///< the Shift knob in semitones; 0 bypasses m_shift
    /** @} */
    /**
     * @name the scatter
     * @{ */
    double    m_transposeRatio = 1.0;   ///< the Transpose knob as a ratio, 2^(semitones / 12)
    float     m_scatter = 0.0f;   ///< the Scatter knob, 0 .. 1
    float     m_iv[MaxIv] = {};   ///< the scale's intervals as ratios, most consonant first (buildIntervals)
    int       m_numIv = 0;   ///< how many of m_iv are filled
    /** @} */
    /**
     * @name the resonators
     * @{ */
    float     m_resAmount = 0.0f,   ///< the resonator mix the knob asks for
              m_resCur = 0.0f,      ///< the mix as it glides
              m_resDecay = 2.5f;    ///< ring time to -60 dB in seconds
    int       m_resMode = 0,    ///< 0 band, 1 comb
              m_resNotes = 1,   ///< 0 scale, 1 chord, 2 fifths, 3 octaves
              m_numRes = 0;     ///< how many resonators are built
    float     m_resHz[MaxRes] = {},            ///< each resonator's note in Hz
              m_resTheta[MaxRes] = {},         ///< its angle per sample as it glides
              m_resThetaTarget[MaxRes] = {};   ///< the angle its note asks for
    float     m_bandU[MaxRes][2] = {},   ///< the phasor filter's real part per resonator and channel
              m_bandV[MaxRes][2] = {};   ///< its imaginary part, which is the output
    float     m_combLp[MaxRes][2] = {};   ///< the comb loops' one-pole state per resonator and channel
    std::vector<float> m_comb;                ///< MaxRes x 2 x CombLen
    int       m_combW = 0;   ///< the combs' write index, shared
    float     m_combA = 0.5f,     ///< the comb loops' one-pole coefficient, 6 kHz
              m_combLag = 1.0f;   ///< that one-pole's delay at low frequencies in samples, taken off the comb's delay
    float     m_busL[MaxRes][Sub] = {},   ///< the grains of one sub-block, left, per resonator (bus 0 alone when not routing)
              m_busR[MaxRes][Sub] = {};   ///< and right
    /** @} */
    /**
     * @name the harmony
     * @{ */
    bool      m_haveHarmony = false;   ///< whether setHarmony() has been given a scale yet
    double    m_scaleSig = -1.0,   ///< a signature of the scale's degrees, so a repeat call is recognised
              m_tonicHz = 0.0,     ///< the tonic in Hz
              m_rootHz = 0.0,      ///< the conductor's root in Hz
              m_period = 2.0;      ///< the scale's repeating interval as a ratio (2 for an octave)
    int       m_count = 12;   ///< degrees in the scale
    double    m_ratios[FixedScale::Max] = {};   ///< the degrees as ratios over the tonic, a zero degree taken as 1
    double    m_rot[FixedScale::Max] = {};   ///< the scale's intervals above the conductor's root
    /** @} */
};

} // namespace ambient
