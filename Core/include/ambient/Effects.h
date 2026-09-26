/**
 * @file Effects.h
 * @brief Stereo effects.
 *
 * ```{.unparsed}
 *   Ensemble    modulated three-tap chorus
 *   StereoDelay asymmetric L/R delay with cross-feed and damping (time-based width)
 *   Reverb      8-line feedback delay network with diffusion, damping, freeze,
 *               L/R asymmetry and a tail high-cut (the dark "infinite background")
 *   MidSide     mono bass below a crossover, gentle side upper-mid lift, width
 * ```
 * Buffers are allocated in prepare() only.
 *
 * The list above is the file as it began; the classes that joined it later each carry their own
 * account: Unmask (the far bus ducked band by band under the near bus), Patina (the master's age),
 * Diffuser (the swell in front of the reverb), HaasBand (the frequency-selective Haas widener) and
 * EarlyRoom (the first reflections of a shoebox from its geometry). The Ensemble has grown two
 * more modes beside the chorus, the Reverb three beside the classic network. Every class here is
 * prepared once with the rate, set once a block from the parameters and run on the audio thread
 * with a stereo block; none allocates after prepare(). ringRead(), the fractional read behind a
 * write index that every delay line here uses, sits between them.
 *
 * The granular cloud of the far plane lives in Cloud.h.
 */
#pragma once
#include "Dsp.h"
#include "Oversample.h"
#include <vector>

namespace ambient {

/**
 * @brief The widener of a plane: a three-tap chorus, a microshift or a velvet-noise decorrelator.
 *
 * Both channels go through one of three ways of widening (setMode), wet against dry by Mix; Depth
 * and Rate mean something different in each. At Mix 0 the delay lines are still fed, so the effect
 * has its history when it comes back, but nothing else is computed.
 */
class Ensemble {
public:
    /**
     * @brief Sizes the delay lines (0.45 s: the chorus taps and the microshifter's longest read) and clears them.
     * @param sampleRate  the engine's rate in Hz
     */
    void prepare(double sampleRate);
    /**
     * @brief The three knobs, once a block; what they mean depends on the mode.
     * @param mix     0 .. 1 wet against dry; 0 skips everything but feeding the lines
     * @param depth   Chorus: the sweep, 6 ms at 1. Microshift: the detune, 12 cents at 1. Velvet: how much of the 30 ms sequence is used, 0.1 .. 1
     * @param rateHz  Chorus: the LFO rate in Hz (each tap at its own multiple). Microshift: a very slow wander of the detune. Velvet: unused
     */
    void set(float mix, float depth, float rateHz) { mix_ = mix; depth_ = depth; rate_ = rateHz; }
    /**
     * @brief Which of the three ways of widening runs.
     *
     * Chorus (0) is the three modulated taps this has always been. Microshift (1) is the other
     * way a mix engineer widens a drone: the two channels are detuned by a few cents in opposite
     * directions and delayed by different amounts, with nothing modulated. It survives a mono
     * sum -- two channels a few cents apart are never at a fixed phase, so there is no comb to
     * cancel into -- where a deep chorus at 13 to 22 ms is exactly a comb filter waiting to be
     * summed. Depth becomes the detune in cents, Rate a very slow wander of it.
     * Velvet (2) is the third way, and the one the decorrelation literature settled on
     * (Valimaki, Alary and Politis 2017): each channel is convolved with its own sparse
     * random impulse response -- a few hundred impulses of +-1 a second, one per equal
     * interval at a random position inside it, which is "velvet noise". A sparse sequence of
     * signed unit impulses has a flat magnitude response, so the two channels come out
     * decorrelated without being coloured, and nothing is modulated, so nothing warbles.
     * @param mode  0 Chorus, 1 Microshift, 2 Velvet
     */
    void setMode(int mode) { mode_ = mode; }
    /**
     * @brief Runs a block in place through the current mode.
     * @param L  left channel, n samples, replaced by the mix
     * @param R  right channel, likewise
     * @param n  samples in the block
     */
    void process(float* L, float* R, int n);
private:
    /**
     * @brief The chorus: three taps at 13, 17 and 22 ms, each swept by its own LFO, the right channel a quarter cycle on.
     * @param L  left channel, in place
     * @param R  right channel
     * @param n  samples in the block
     */
    void processChorus(float* L, float* R, int n);
    /**
     * @brief The microshift: two delay-line pitch shifters, +c cents on the left and -c on the right, at 11 and 19 ms.
     *
     * The read pointer travels 200 ms and hands over to a second tap in a 25 ms cross-fade, so the
     * shifter is a plain delay line at a slowly changing delay rather than a two-tap comb (see
     * Effects.cpp for the measurement behind that).
     * @param L  left channel, in place
     * @param R  right channel
     * @param n  samples in the block
     */
    void processShift(float* L, float* R, int n);
    /**
     * @brief The velvet-noise decorrelator: each channel convolved with its own sparse sequence of +-1 impulses.
     *
     * Builds the sequences on first use. Depth shortens the sequence; the gain is renormalised for
     * the taps in use.
     * @param L  left channel, in place
     * @param R  right channel
     * @param n  samples in the block
     */
    void processVelvet(float* L, float* R, int n);
    /**
     * @brief Draws the two velvet sequences: kVelvetTaps impulses over 30 ms, one per equal interval at a random position, signs at random.
     * @param seed  seeds the draw (a fixed constant, so the sequences are the same on every machine)
     */
    void buildVelvet(uint64_t seed);
    static constexpr int kVelvetTaps = 96;      ///< impulses per channel
    int   velvetPos_[2][kVelvetTaps] = {};      ///< where each impulse sits, in samples
    float velvetSign_[2][kVelvetTaps] = {};     ///< +-1, with the 1/sqrt(N) gain folded in
    int   velvetLen_ = 0;                       ///< the sequence's length in samples
    std::vector<float> bufL_,   ///< the left delay line, a power-of-two ring
                       bufR_;   ///< the right delay line
    int    mask_ = 0,   ///< ring size - 1
           w_ = 0;      ///< the write index, shared by both lines
    double sr_ = 48000.0;   ///< sample rate in Hz
    double ph_[3] = { 0.0, 0.33, 0.66 };   ///< the chorus LFOs' phases in cycles, one per tap, staggered
    float  mix_ = 0.4f,     ///< wet against dry
           depth_ = 0.4f,   ///< the Depth knob
           rate_ = 0.2f;    ///< the Rate knob in Hz
    int    mode_ = 0;   ///< 0 chorus, 1 microshift, 2 velvet
    /**
     * @name Microshift state: the two shifters' window phases and the slow wander of the detune.
     * @{ */
    double shPh_[2] = { 0.0, 0.5 };   ///< where each channel's read pointer is in its 200 ms ramp, 0 .. 1
    double wanderPh_ = 0.0;   ///< the phase of the detune's wander, in cycles
    /** @} */
};

/**
 * @brief Asymmetric stereo delay with cross-feed, damping, an absorption band and a duck on the loop's brightness.
 *
 * Two delay lines whose times glide to their targets and wander by 0.3 ms; the feedback of each
 * side is a blend of its own and the other's output (cross), low-passed (damping) and, with
 * absorb up, band-limited harder the higher the feedback. Wet only: the caller mixes.
 */
class StereoDelay {
public:
    /**
     * @brief Sizes the delay lines for 4 s and clears them.
     * @param sampleRate  the engine's rate in Hz
     */
    void prepare(double sampleRate);
    /**
     * @brief The delay's settings, once a block.
     *
     * absorb: with it up, the loop also loses its low end and its high cut sinks as the feedback
     * rises, so long echoes drown into a fog instead of merely getting quieter.
     * @param timeL     left delay in seconds, 0.001 .. 4
     * @param timeR     right delay in seconds, 0.001 .. 4
     * @param feedback  loop gain, 0 .. 0.98
     * @param cross     0 .. 1: how much of each side's feedback comes from the other side
     * @param damping   0 .. 1: the one-pole low pass in the loop, from open to a coefficient of 0.1
     * @param absorb    0 .. 1: at full feedback and full absorb the loop keeps 320 Hz .. 1 kHz
     */
    void set(float timeL, float timeR, float feedback, float cross, float damping, float absorb = 0.0f);
    /**
     * @brief How far the loop's brightness is pulled down while the input is loud.
     *
     * Duck: how far the loop's high cut is pulled down while the input is loud. The idea is the
     * one the far reverb's Unmask already uses -- get out of the way of the thing being played --
     * applied to the echoes: a fresh attack should not have to fight the brightness of the last
     * one's tail. At 0 the loop behaves exactly as it did.
     * @param amount  0 .. 1; the duck is driven by the input's level over its own slow average, so a drone ducks nothing
     */
    void setDuck(float amount) { duck_ = clampv(amount, 0.0f, 1.0f); }
    /**
     * @brief Writes the wet signal only; the caller mixes it.
     * @param inL   left input, n samples
     * @param inR   right input
     * @param wetL  receives the left echoes
     * @param wetR  receives the right echoes
     * @param n     samples in the block
     */
    void process(const float* inL, const float* inR, float* wetL, float* wetR, int n);
private:
    std::vector<float> bufL_,   ///< the left delay line, a power-of-two ring
                       bufR_;   ///< the right delay line
    int    mask_ = 0,   ///< ring size - 1
           w_ = 0;      ///< the write index, shared by both lines
    double sr_ = 48000.0;   ///< sample rate in Hz
    float  tL_ = 0,      ///< the left delay asked for, in samples
           tR_ = 0,      ///< the right delay asked for
           tLcur_ = 0,   ///< the left delay as it glides (0 before the first set())
           tRcur_ = 0;   ///< the right delay as it glides
    float  fb_ = 0.5f,      ///< loop gain
           cross_ = 0.3f,   ///< the cross-feed, 0 .. 1
           lpc_ = 0.5f;     ///< the damping one-pole's coefficient, 1 - 0.9 damping
    float  lpL_ = 0,   ///< the damping low pass's state, left
           lpR_ = 0;   ///< and right
    float  absorb_ = 0.0f,   ///< the Absorb knob
           hpc_ = 0.0f,      ///< the absorption band's low-cut coefficient
           lpc2_ = 1.0f;     ///< the absorption band, from feedback and absorb
    float  loL_ = 0,   ///< the absorption low cut's state, left
           loR_ = 0,   ///< and right
           hiL_ = 0,   ///< the absorption high cut's state, left
           hiR_ = 0;   ///< and right
    float  duck_ = 0.0f,       ///< the Duck knob
           duckEnv_ = 0.0f,    ///< the input's slow average, about 3.5 s
           duckFast_ = 0.0f;   ///< how much, and the two envelopes driving it
    double modPh_[2] = { 0.0, 0.5 };   ///< the phases of the two delays' slow wander (0.07 and 0.053 Hz), in cycles
};

/**
 * @brief The 8-line feedback delay network: the dark infinite background of the far plane.
 *
 * Each channel goes through its own pre-delay and four-stage all-pass diffusion into its own half
 * of the network (the left into lines 0-3, whose taps are the left output, the right into 4-7);
 * a Householder reflection mixes all eight lines back into themselves with per-line gains set for
 * the decay, a one-pole damping in every loop, and a DC blocker on the input. Four modes (setMode)
 * change how the lines scatter, what lengths they have and whether the matrix turns. The tail is
 * high-cut and low-cut on the way out, the right output a little late for asymmetry; freeze holds
 * the loop lossless with the input shut.
 */
class Reverb {
public:
    /**
     * @brief Allocates the lines, the diffusers, the pre-delays and the scattering all-passes for the rate, and sets the defaults.
     * @param sampleRate  the engine's rate in Hz
     */
    void prepare(double sampleRate);
    /**
     * @brief The main settings, once a block.
     * @param size          scales the line lengths, 0.5 .. 3 (1 is 30 .. 80 ms)
     * @param decaySeconds  time to -60 dB, at least 0.1 s; the per-line gains follow from it
     * @param damping       0 .. 1: the one-pole low pass in every loop, from open to a coefficient of 0.08
     * @param preDelayMs    pre-delay before the diffusion, 0 .. 500 ms, glided
     * @param freeze        true holds the loop at unity gain, without damping and with the input shut
     * @param mix           0 .. 1 wet against dry
     */
    void set(float size, float decaySeconds, float damping, float preDelayMs, bool freeze, float mix);
    /**
     * @brief The stereo asymmetry and the tail's filter funnel, once a block.
     *
     * asymmetry 0..1: right-hand lines lengthened and the right output delayed by up to 10 ms
     * highcutHz: one-pole low-pass on the wet output (20000 = off)
     * lowcutHz:  two one-pole high-passes on the wet output, 12 dB/oct (20 = off). The other half
     *            of the filter funnel a mixing engineer puts on a reverb return: without it the
     *            tail piles up in the low mids, where it turns the whole picture to mud rather
     *            than sitting behind it.
     * @param asymmetry  0 .. 1: lines 4-7 up to 8 % longer and the right output up to 10 ms late
     * @param highcutHz  200 .. 20000 Hz; 19000 and above turns the filter off
     * @param lowcutHz   20 .. 1000 Hz, the -3 dB point of the pair; 21 and below turns it off
     */
    void setSpace(float asymmetry, float highcutHz, float lowcutHz = 20.0f);
    /**
     * @brief Which network runs: classic, scattering, colourless or rotating.
     *
     * Classic (0) is the network as it always was. Scattering (1) puts a Schroeder all-pass
     * inside every delay line's loop, after Schlecht and Habets (2020): each pass round the
     * network then scatters every echo into many, so the echo density grows much faster
     * (measured: 0.60 -> 0.76 of Gaussian after 50 ms) while the late tail stays at least as
     * smooth. Same decay, same level, same lines; a denser texture of tail.
     *
     * Colourless (2) is Scattering with a second change: the eight line lengths are not the
     * hand-picked primes of the classic network but a set searched offline for the flattest
     * magnitude response (Tools/optimise_fdn.py, after the colourless-FDN work of Dal Santo,
     * Prawda, Schlecht and Valimaki). A delay network's tail is a sum of modes at the lines'
     * own frequencies, and where those pile up the tail rings; lengths chosen against that
     * measure ring less -- 0.31 dB of third-octave spread against the classic set's 0.47.
     * Rotating (3) is Colourless with the feedback changed: the line lengths stop moving (the
     * classic network wobbles each by a sample and a half, which is how it keeps its modes from
     * ringing) and the feedback matrix turns instead -- eight Givens rotations on pairs of
     * lines, their angles advancing at a few hundredths of a hertz, in front of the Householder
     * reflection the network always had. A product of rotations and a reflection is orthogonal
     * whatever the angles are, so the loop is lossless at every instant and the tail loses
     * energy only through the gains and the damping, as designed; and because the modes are
     * being re-mixed rather than re-tuned, nothing in the tail is pitch-modulated (Schlecht and
     * Habets 2015, time-varying feedback matrices). Measured: the band pattern of the tail
     * drifts from one moment to the next where the fixed network's stays put.
     * @param mode  0 Classic, 1 Scattering, 2 Colourless, 3 Rotating; the line lengths take effect at the next set()
     */
    void setMode(int mode) { mode_ = mode; }
    /**
     * @brief The high-pass in front of the input (Send Low Cut, 25.09.2026): nothing under it is
     *        thrown into the tail.
     *
     * A second-order Butterworth on the wet path only -- the dry share of an insert passes as it
     * was -- after the DC blocker and before the pre-delay. The production guide's first rule of
     * reverb is a filter before every send, 150 to 300 Hz: a hall fed with the fundamentals of a
     * drone answers with a low-frequency mass that no return filter takes out again, because by
     * then it has recirculated for the length of the tail. The Low Cut of setSpace() stays what it
     * was, a filter on the tail. Off at 20 Hz and below.
     * @param hz  the -3 dB point in Hz; 20 or less switches it off
     */
    void setSendLowcut(float hz);
    /**
     * @brief Runs a block in place, mixing the tail against the dry signal.
     * @param L  left channel, n samples
     * @param R  right channel
     * @param n  samples in the block
     */
    void process(float* L, float* R, int n);
private:
    static constexpr int kLines = 8;   ///< delay lines in the network
    static constexpr int kRotations = 8;   ///< Givens rotations of the turning matrix: four on neighbouring pairs, four across
    float  rot_ = 0.0f;                                ///< 0 fixed matrix .. 1 turning, glided
    float  rotC_[kRotations] = {},   ///< the cosines of the rotation angles
           rotS_[kRotations] = {};   ///< the angles, as cos and sin
    float  rotDc_[kRotations] = {},   ///< cos of each angle's per-sample step
           rotDs_[kRotations] = {};   ///< their per-sample advance
    int    rotNorm_ = 0;   ///< samples since the rotation pairs were last pulled back onto the unit circle (every 4096)
    static constexpr int kAllpasses = 4;   ///< stages of input diffusion per channel
    std::vector<float> line_[kLines];   ///< the eight delay lines, power-of-two rings
    std::vector<float> ap_[kAllpasses],    ///< the left channel's input diffusion
                       apR_[kAllpasses];   ///< the input diffusion, a chain per channel
    std::vector<float> pre_,    ///< the left pre-delay ring
                       preR_,   ///< the right pre-delay ring
                       outR_;   ///< the pre-delay, a ring per channel
    int    mask_ = 0,      ///< ring size - 1 for the lines, diffusers and pre-delays
           w_ = 0,         ///< the write index, shared
           outMask_ = 0;   ///< outR_ size - 1
    double sr_ = 48000.0;   ///< sample rate in Hz
    /**
     * A one-pole DC blocker on the input, 5 Hz. A network whose loop gain is a hair under one
     * -- a forty-second tail -- passes an offset with a gain of hundreds: measured, a texture
     * with -0.1 of offset stood at -2.1 on the far bus after thirty seconds, the Patina's
     * clipper railed on it and the output DC blocker turned the rail into silence.
     */
    float  dcInX_[2] = {},   ///< the blocker's last input per channel
           dcInY_[2] = {},   ///< its last output per channel
           dcR_ = 0.999f;    ///< its pole, 5 Hz at the rate
    float  lenTarget_[kLines] = {},   ///< each line's length asked for, in samples
           lenCur_[kLines] = {};      ///< each line's length as it glides
    float  gain_[kLines] = {};   ///< each line's loop gain, from its length and the decay; 1 when frozen
    float  lp_[kLines] = {};   ///< the damping one-pole's state per line
    double modPh_[kLines] = {};   ///< the phase of each line's length wobble, in cycles
    float  modRate_[kLines] = {};   ///< each wobble's rate in Hz, 0.11 .. 0.37
    int    apLen_[kAllpasses] = {};   ///< the input diffusers' lengths in samples (5.1 .. 13.7 ms)
    float  preTarget_ = 0.0f,   ///< the pre-delay asked for, in samples
           preCur_ = 0.0f;      ///< the pre-delay as it glides
    float  outDelayTarget_ = 0.0f,   ///< the right output's delay asked for, in samples
           outDelayCur_ = 0.0f;      ///< that delay as it glides
    float  damp_ = 0.4f,     ///< the Damping knob
           mix_ = 0.45f,     ///< wet against dry
           decay_ = 12.0f,   ///< time to -60 dB in seconds
           size_ = 1.6f,     ///< the Size knob
           asym_ = 0.0f;     ///< the asymmetry, 0 .. 1
    float  hcCoef_ = 1.0f,   ///< the tail high cut's one-pole coefficient; 1 is off
           hcL_ = 0.0f,      ///< the high cut's state, left
           hcR_ = 0.0f;      ///< and right
    float  lcCoef_ = 0.0f;                                        ///< 0 = off
    float  lcL1_ = 0.0f,   ///< the tail low cut's first one-pole, left
           lcR1_ = 0.0f,   ///< first one-pole, right
           lcL2_ = 0.0f,   ///< second one-pole, left
           lcR2_ = 0.0f;   ///< second one-pole, right
    bool   freeze_ = false;   ///< whether the loop is held lossless with the input shut
    Svf    sendHpL_,   ///< @brief the input high-pass (Send Low Cut), left
           sendHpR_;   ///< the input high-pass, right
    bool   sendHpOn_ = false;   ///< whether the input high-pass runs (Send Low Cut above 20 Hz)
    int    mode_ = 0;   ///< 0 classic, 1 scattering, 2 colourless, 3 rotating
    std::vector<float> sc_[kLines];   ///< the scattering all-passes, one per line
    int    scLen_[kLines] = {};   ///< their lengths in samples (1.9 .. 7.1 ms, mutually prime)
};

/**
 * @brief The background ducked under the near bus, band by band.
 *
 * The background steps aside for the foreground, band by band. A mixing engineer rides the
 * reverb return down while a line is sounding and lets it back up in the gaps; done per band it
 * is what keeps a dense pad from swallowing its own notes. The near bus is the side chain, fast
 * to duck and slow to return -- the return is the part the ear hears as the room breathing back in.
 *
 * Seven bands since 25.09.2026, an octave apart from 150 Hz to 4.8 kHz: the production guide's
 * spectral ducking wants six to eight, so that the background gives way only where the
 * foreground actually is (three bands let a flute at 2 kHz take out everything from 300 Hz up).
 * The split is a cascade of one-poles whose bands sum back to the input exactly, so at no duck
 * the signal is untouched. The engine runs two: one on the far hall, one on the convolution room
 * (the guide's "near ducks the main room"), both on the Unmask knob.
 */
class Unmask {
public:
    static constexpr int kBands = 7;   ///< bands of the split: under 150 Hz, five octaves to 4.8 kHz, above
    /**
     * @brief Sets the crossovers, the duck (20 ms) and return times for the rate, and resets.
     * @param sampleRate  the engine's rate in Hz
     */
    void prepare(double sampleRate);
    /**
     * @brief The amount, the spread and the return time, once a block.
     *
     * spread: how far a loud low band also ducks the bands above it. Masking in the ear is
     * asymmetric -- a low tone masks the frequencies above it far more than those below
     * (Zwicker and Fastl 1999, the upward spread of masking) -- and at 0 the bands are
     * independent.
     * returnSeconds: how long the background takes to come back after a duck (the duck itself is
     * 20 ms, the guide's attack). The guide's release is half a second, which is the parameter's
     * default since 25.09.2026; longer, and the horizon's return is a gesture.
     * @param amount         0 = off (and then not computed at all); 1 ducks by up to about 16 dB
     * @param spread         0 .. 1, see above
     * @param returnSeconds  0.05 .. 30 s; the release coefficient is recomputed only when it changes
     */
    void set(float amount, float spread = 0.0f, float returnSeconds = 0.5f);
    /**
     * @brief Ducks the background where the near bus has energy, in place.
     * @param nearL  the near bus, left, n samples (read only)
     * @param nearR  the near bus, right
     * @param farL   the background (the far hall, or the room's return), left, ducked in place
     * @param farR   the background, right
     * @param n      samples in the block
     */
    void process(const float* nearL, const float* nearR, float* farL, float* farR, int n);
    /** @brief Clears the crossover states, the envelopes and the gains (back to 1). */
    void reset();
private:
    /**
     * @brief Splits one sample into the kBands bands: a second-order low-pass at every crossover,
     *        each band the difference of two neighbours, so the bands add back to the input exactly.
     * @param lp    the crossovers' filters, kBands - 1 of them, advanced here
     * @param x     the input sample
     * @param band  receives the bands, lowest first; they sum to x
     */
    static void split(Svf* lp, float x, float* band);
    Svf    xNear_[kBands - 1];      ///< the near bus's crossovers (the near bus is summed to mono)
    Svf    xBg_[2][kBands - 1];     ///< the background's crossovers, per channel
    float  env_[kBands] = {};          ///< the near bus's band envelopes
    float  gain_[kBands] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };   ///< what the background is multiplied by, smoothed
    float  aCoef_ = 0.01f,     ///< the attack coefficient of the envelopes and gains: the duck, 20 ms
           rCoef_ = 0.0005f;   ///< the release coefficient: the return, returnSec_
    float  amount_ = 0.0f,      ///< the Unmask knob, 0 .. 1
           spread_ = 0.0f,      ///< the Spread knob, 0 .. 1
           returnSec_ = 0.5f;   ///< the return time in seconds
    double sr_ = 48000.0;   ///< sample rate in Hz
};

/**
 * @brief The master's age: wow and flutter, lost highs, a noise floor and a gentle saturation.
 *
 * The master's age: tape wow, the highs a worn machine loses, a noise floor that lives under the
 * music, and a gentle saturation. Every one of them is a defect, and together they are most of
 * what separates a recording from a render. Off at amount 0, and then bypassed entirely.
 */
class Patina {
public:
    /**
     * @brief Sizes the wow's delay rings, seeds the generator and resets.
     * @param sampleRate  the engine's rate in Hz
     * @param seed        seeds the hiss and the wow's drift
     */
    void prepare(double sampleRate, uint64_t seed);
    /**
     * @brief The four knobs, once a block; all clamped to 0 .. 1.
     * @param amount  the whole effect's depth; 0 bypasses it
     * @param wow     the depth of the pitch waver (up to 2.2 ms of swing at amount 1)
     * @param hiss    the noise floor's level
     * @param age     the lost top end: the high cut falls from 20 kHz towards 4 kHz with age x amount
     */
    void set(float amount, float wow, float hiss, float age);
    /**
     * @brief Runs a block in place: wow and flutter, the high cut, the hiss, the saturation.
     * @param L  left channel, n samples
     * @param R  right channel
     * @param n  samples in the block
     */
    void process(float* L, float* R, int n);
    /** @brief Clears the delay rings, the high-cut states and the level envelope. */
    void reset();
    /**
     * @brief Whether the patina does anything.
     * @return  true when amount is above 0 -- off at 0, and then not computed at all
     */
    bool active() const { return amount_ > 0.0f; }
private:
    std::vector<float> bufL_,   ///< the left wow delay ring, 4096 samples
                       bufR_;   ///< the right wow delay ring
    int     mask_ = 0,   ///< ring size - 1
            w_ = 0;      ///< the write index, kept inside the ring
    double  sr_ = 48000.0;   ///< sample rate in Hz
    float   amount_ = 0.0f,   ///< the Amount knob
            wow_ = 0.3f,      ///< the Wow knob
            hiss_ = 0.2f,     ///< the Hiss knob
            age_ = 0.3f;      ///< the Age knob
    float   lpC_ = 1.0f,   ///< the age high cut's one-pole coefficient; 1 is off
            lpL_ = 0.0f,   ///< its state, left
            lpR_ = 0.0f;   ///< its state, right
    float   env_ = 0.0f;   ///< the signal's level, fast up and slow down, which raises the hiss (modulation noise)
    Drifter wowDrift_;   ///< the slow, irregular wow
    double  flutterPh_ = 0.0;   ///< the phase of the steady 6 Hz flutter, in cycles
    Rng     rng_;   ///< the patina's own generator: the hiss and the wow's knots
    Oversampler4 osL_,   ///< @brief the saturation at four times the rate, left
                 osR_;   ///< and right
};

/**
 * @brief The master's stereo stage: mono bass, a lift on the side's upper mids, width, a tilt and the mono guard.
 *
 * The block is split into mid and side; the side is high-passed at the bass-mono frequency,
 * given a broad 3 kHz bell, scaled by the width (and the guard's trim), and put back. A tilt EQ
 * runs ahead of that when it is not flat.
 */
class MidSide {
public:
    /**
     * @brief Takes the rate, resets the filters and sets the defaults (150 Hz, +2 dB, 1.2).
     * @param sampleRate  the engine's rate in Hz
     */
    void prepare(double sampleRate);
    /**
     * @brief The three main settings, once a block.
     * @param bassMonoHz  below this the side is removed, 20 .. 400 Hz (a Butterworth high pass on the side)
     * @param sideAirDb   the side's 3 kHz bell, 0 .. 12 dB
     * @param width       the side's gain, 0 .. 2; 1 leaves it as it is
     */
    void set(float bassMonoHz, float sideAirDb, float width);
    /**
     * @brief A spectral tilt on both channels.
     *
     * Tilt: a see-saw around the pivot -- one first-order low pass and its complement, weighted
     * against each other. Flat at 0 dB and then not computed at all.
     * @param dB       the difference between the top and the bottom; positive brightens, half of it each way
     * @param pivotHz  where the see-saw turns, 20 Hz .. 0.45 of the rate
     */
    void setTilt(float dB, float pivotHz);
    /**
     * @brief Runs a block in place.
     * @param L  left channel, n samples
     * @param R  right channel
     * @param n  samples in the block
     */
    void process(float* L, float* R, int n);
    /**
     * @brief Switches the mono guard.
     *
     * Mono safety. Everything upstream is built to widen -- all-pass phase width, asymmetric
     * delays, a reverb whose two sides are deliberately different -- and a drone that is
     * gigantic in stereo can vanish when it is summed to mono. The side channel is measured
     * against the mid over a long window and the width is eased back only when the side actually
     * dominates: a slow, small correction that a listener cannot hear and a mono sum can.
     * @param on  false puts the trim back to 1 and leaves the width alone
     */
    void setMonoGuard(bool on) { guardOn_ = on; }
    /**
     * @brief The guard's current trim on the width.
     * @return  1 = untouched, for the meter; down to 0.75 when the side has been dominating
     */
    float widthTrim() const { return guard_; }   // 1 = untouched, for the meter

private:
    Svf    hp_,    ///< the side's high pass at the bass-mono frequency
           air_;   ///< the side's 3 kHz bell (its band pass, added)
    float  airGain_ = 0.0f,   ///< the bell's gain as a linear amount added, 10^(dB / 20) - 1
           width_ = 1.0f;     ///< the side's gain
    float  midPow_ = 0.0f,    ///< the mid's power on a slow average (about 1.5 s)
           sidePow_ = 0.0f,   ///< the side's power before the trim, likewise
           guard_ = 1.0f;     ///< the guard's trim on the width, 0.75 .. 1
    bool   guardOn_ = true;   ///< whether the guard measures and trims
    float  tiltLo_ = 1.0f,       ///< the tilt's gain below the pivot
           tiltHi_ = 1.0f,       ///< its gain above
           tiltC_ = 0.1f,        ///< the pivot one-pole's coefficient
           tiltState_[2] = {};   ///< the pivot one-pole's state per channel
    bool   tiltOn_ = false;   ///< whether the tilt is more than 0.05 dB from flat
    double sr_ = 48000.0;   ///< sample rate in Hz
};

/**
 * @brief The swell in front of the reverb: four modulated all-passes per channel.
 *
 * A diffusion field: four modulated all-passes that turn an impulse into a swell before the
 * reverb ever sees it. A feedback network answers immediately by construction; this is what
 * gives a tail the slow arrival a large room has.
 */
class Diffuser {
public:
    /**
     * @brief Sizes the rings for the rate and sets the four lengths (13.7, 21.3, 33.1, 47.9 ms).
     * @param sampleRate  the engine's rate in Hz
     */
    void prepare(double sampleRate);
    /**
     * @brief The amount, once a block.
     * @param amount  0 .. 1: the all-pass coefficient (0.5 .. 0.85), the modulation depth and the wet mix at once; 0 bypasses
     */
    void set(float amount);
    /**
     * @brief Runs a block in place.
     * @param L  left channel, n samples
     * @param R  right channel
     * @param n  samples in the block
     */
    void process(float* L, float* R, int n);
    /** @brief Clears the rings. */
    void reset();
private:
    static constexpr int kStages = 4;   ///< all-passes in the chain
    std::vector<float> buf_[2][kStages];   ///< one ring per channel per stage
    int    len_[kStages] = {};   ///< the stages' nominal delays in samples
    int    mask_ = 0,   ///< ring size - 1
           w_ = 0;      ///< the write index, shared
    double modPh_[kStages] = { 0.0, 0.3, 0.6, 0.85 };   ///< the phase of each stage's read-point modulation, in cycles (the right channel takes the opposite side)
    float  amount_ = 0.0f;   ///< the Diffusion knob
    double sr_ = 48000.0;   ///< sample rate in Hz
};

/**
 * @brief Read `delay` samples (>= 1, fractional) behind write index `w` from a power-of-two ring.
 * @param buf    the ring
 * @param mask   its size - 1
 * @param w      the write index (the sample at w is not yet written for this step)
 * @param delay  how far behind w to read, at least 1, linearly interpolated between two samples
 * @return       the interpolated sample
 */
inline float ringRead(const float* buf, int mask, int w, float delay)
{
    const int di = static_cast<int>(delay);
    const float f = delay - static_cast<float>(di);
    const float a = buf[(w - di) & mask];
    const float b = buf[(w - di - 1) & mask];
    return a + f * (b - a);
}

/**
 * @brief The frequency-selective Haas effect.
 *
 * Delaying a whole channel by ten to thirty milliseconds
 * widens it and destroys it in mono: the two channels comb, and the comb's first notch lands in
 * the bass. Done to one band only -- roughly 1.2 to 4 kHz, where the ear takes its direction from
 * level rather than from time -- and cross-fed, so each side hears the other's delayed band six
 * decibels down, the width appears at the edges and the low end and the top stay exactly where
 * they were. Off at amount 0, and then not computed.
 */
class HaasBand {
public:
    /**
     * @brief Sizes the one delay line (50 ms), sets the band's two filters and snaps the delay to the knob.
     * @param sampleRate  the engine's rate in Hz
     */
    void prepare(double sampleRate)
    {
        sr_ = sampleRate;
        int size = 1; while (size < static_cast<int>(0.05 * sr_) + 8) size <<= 1;
        bufL_.assign(static_cast<size_t>(size), 0.0f);
        mask_ = size - 1; w_ = 0;
        smDelay_ = timeMs_ * static_cast<float>(sr_ / 1000.0) + 1.0f;
        // Two poles at the ends of the band: a high-pass at 1.2 kHz and a low-pass at 4 kHz, which
        // between them is the band the paper isolates.
        hp_[0].setQ(1200.0f, 0.7f, static_cast<float>(sr_));
        lp_[0].setQ(4000.0f, 0.7f, static_cast<float>(sr_));
    }
    /**
     * @brief The amount and the delay, once a block; both glide inside process().
     * @param amount  0 .. 1; the delayed band enters the side channel at -6 dB at 1
     * @param timeMs  the delay, 1 .. 45 ms
     */
    void set(float amount, float timeMs) { amount_ = clampv(amount, 0.0f, 1.0f); timeMs_ = clampv(timeMs, 1.0f, 45.0f); }
    /**
     * @brief Runs a block in place: the mid's band, delayed, added on the left and taken off on the right.
     * @param L  left channel, n samples
     * @param R  right channel
     * @param n  samples in the block
     */
    void process(float* L, float* R, int n)
    {
        if (amount_ <= 0.0f && smAmount_ <= 1.0e-5f) { w_ = (w_ + n) & 0x3FFFFFFF; return; }
        // The delay follows the knob at the same pace as the amount: read straight from the knob
        // it would jump the read pointer, which is a click, not a change of width.
        const float delayTarget = timeMs_ * static_cast<float>(sr_ / 1000.0) + 1.0f;
        const float c = 1.0f - std::exp(-1.0f / (0.05f * static_cast<float>(sr_)));   // 50 ms
        float* bl = bufL_.data();
        for (int i = 0; i < n; ++i) {
            smAmount_ += c * (amount_ - smAmount_);
            smDelay_ += c * (delayTarget - smDelay_);
            const float delay = smDelay_;
            // The band of what is in the middle -- the part of the picture that has no width yet.
            float lp, bp, hp;
            const float mid = 0.5f * (L[i] + R[i]);
            hp_[0].tick(mid, lp, bp, hp); float band = hp;
            lp_[0].tick(band, lp, bp, hp); band = lp;
            bl[w_ & mask_] = band;
            // Into the side channel: added on the left, taken off on the right. That is what "hard
            // to the opposite side" comes to once it is made symmetrical, and it is the only form
            // of it that is exactly mono-safe -- summed to mono the two cancel to the sample, so
            // the centre of the mix is the same picture it was before the widening.
            const float d = smAmount_ * 0.5f * ringRead(bl, mask_, w_, delay);   // -6 dB at full amount
            L[i] += d;
            R[i] -= d;
            ++w_;
        }
    }
private:
    std::vector<float> bufL_;                       ///< one band, one line: it goes to the sides
    int    mask_ = 0,   ///< ring size - 1
           w_ = 0;      ///< the write index
    double sr_ = 48000.0;   ///< sample rate in Hz
    Svf    hp_[1],   ///< the band's high pass at 1.2 kHz
           lp_[1];   ///< the band's low pass at 4 kHz
    float  amount_ = 0.0f,     ///< the Haas knob
           smAmount_ = 0.0f,   ///< the amount as it glides (50 ms)
           timeMs_ = 15.0f,    ///< the delay asked for, in ms
           smDelay_ = 0.0f;    ///< the delay as it glides, in samples plus one
};



/**
 * @brief The early reflections of a room, from its geometry rather than from a reverb's statistics.
 *
 * A feedback delay network makes a plausible tail out of numbers that mean nothing in particular.
 * The first reflections are not statistics: they arrive from the six surfaces of the room at times
 * and from directions that follow from where the source is standing, and the ear reads those times
 * and directions as the size of the room and the distance of the source (Blauert 1997 for the
 * direction, Bronkhorst and Houtgast 1999 for the distance). A reverb whose early part does not
 * move when the source moves is a room the source is not in.
 *
 * This is the scattering delay network of De Sena, Hacihabiboglu and Cvetkovic (2015) in its
 * simplest useful form: one node at the centre of each of the six walls of a shoebox, a delay from
 * the source to each node and from each node to the listener, an absorption filter at each node,
 * and a single scattering stage that feeds every node from all the others so the reflections go on
 * reflecting. The first arrivals are then at the geometrically right times, from the right
 * directions, and they move when the source does -- which is the whole point -- while the later
 * energy is handed to the far reverb, whose job it already is.
 *
 * One simplification is worth naming. The source position is one position for the whole near bus,
 * taken from where the voices actually are (the level-weighted mean of their pan and distance),
 * not one per voice: six delay lines per voice would cost more than the rest of the instrument.
 * What that buys is a room whose early pattern follows the music; what it gives up is a different
 * early pattern for two voices sounding at once from different places.
 */
class EarlyRoom {
public:
    static constexpr int kWalls = 6;   ///< the shoebox's surfaces: left, right, front, back, ceiling, floor

    /**
     * @brief Allocates the lines for the largest room, sets a default room and source, and snaps every delay to its target.
     * @param sampleRate  the engine's rate in Hz
     */
    void prepare(double sampleRate)
    {
        sr_ = sampleRate;
        // Long enough for the longest path in the largest room: forty metres across is a hundred
        // and seventeen milliseconds, and every delay in here is shorter than that.
        int size = 1;
        while (size < static_cast<int>(0.14 * sr_) + 8) size <<= 1;
        inL_.assign(static_cast<size_t>(size), 0.0f);
        inR_.assign(static_cast<size_t>(size), 0.0f);
        for (auto& v : press_) v.assign(static_cast<size_t>(size), 0.0f);
        for (auto& row : pair_) for (auto& v : row) v.assign(static_cast<size_t>(size), 0.0f);
        mask_ = size - 1;
        w_ = 0;
        for (int k = 0; k < kWalls; ++k) { lp_[k] = 0.0f; dSrc_[k] = 1.0f; gSrc_[k] = 0.0f; }
        setRoom(8.0f, 0.35f, 1.0f);
        setSource(0.0f, 0.4f);
        for (int k = 0; k < kWalls; ++k) {
            dSrc_[k] = dSrcT_[k]; gSrc_[k] = gSrcT_[k];
            dEar_[k] = dEarT_[k];
            for (int m = 0; m < kWalls; ++m) dPair_[k][m] = dPairT_[k][m];
        }
    }

    /**
     * @brief The room's size, absorption and width, once a block; the geometry is recomputed and glided into.
     *
     * size: the room's longest dimension in metres (the shoebox is size x 0.8 size x 0.45 size,
     * proportions with no simple ratio between them, so its own modes do not pile up).
     * absorb: how much each surface takes out per reflection, and how dark what comes back is.
     * @param sizeMetres  2 .. 40 m
     * @param absorb      0 .. 1; the wall filter's corner falls from 16 kHz by five octaves at 1
     * @param width       0 .. 1.5: how far each wall's direction is spread across the field; 0 puts every reflection in the middle
     */
    void setRoom(float sizeMetres, float absorb, float width)
    {
        room_ = clampv(sizeMetres, 2.0f, 40.0f);
        absorb_ = clampv(absorb, 0.0f, 1.0f);
        width_ = clampv(width, 0.0f, 1.5f);
        const float d = room_, w = room_ * 0.8f, h = room_ * 0.45f;
        half_[0] = w * 0.5f; half_[1] = w * 0.5f;    // left, right
        half_[2] = d * 0.5f; half_[3] = d * 0.5f;    // front, back
        half_[4] = h * 0.5f; half_[5] = h * 0.5f;    // ceiling, floor
        // Each wall's node sits at the centre of that wall; the listener is at the origin.
        for (int k = 0; k < kWalls; ++k) {
            for (int a = 0; a < 3; ++a) node_[k][a] = 0.0f;
            node_[k][k / 2] = (k & 1) ? half_[k] : -half_[k];
        }
        // How dark a reflection comes back: a one-pole whose corner falls with the absorption.
        const float fc = 16000.0f * std::pow(2.0f, -5.0f * absorb_);
        lpCoef_ = 1.0f - std::exp(-kTwoPi * fc / static_cast<float>(sr_));
        roomGeometry();
        sourceGeometry();
    }

    /**
     * @brief Where the source stands.
     *
     * Where the source stands, from the near bus's own voices: pan -1..1 across the room, and
     * distance 0..1 from the listener towards the far wall.
     * @param pan       -1 (left wall) .. 1 (right wall)
     * @param distance  0 (just in front of the listener) .. 1 (a metre short of the front wall)
     */
    void setSource(float pan, float distance)
    {
        const float p = clampv(pan, -1.0f, 1.0f), d = clampv(distance, 0.0f, 1.0f);
        src_[0] = p * (half_[0] - 0.5f);
        src_[1] = 0.8f + d * (half_[2] - 1.0f);   // in front of the listener
        src_[2] = 0.0f;
        sourceGeometry();
    }

    /**
     * @brief The level of the reflections added to the output.
     * @param level  0 .. 1, glided across the block inside process(); 0 lets the room fall silent and then skip
     */
    void setLevel(float level) { level_ = clampv(level, 0.0f, 1.0f); }
    /**
     * @brief Whether process() has anything to do.
     * @return  true while the level asked for or the glided level is above nothing
     */
    bool active() const { return level_ > 0.0f || smLevel_ > 1.0e-5f; }

    /**
     * @brief Runs a block: the near bus into the walls, the walls into each other and into the ears.
     *
     * Adds the room's early reflections to L/R, from the near bus in inL/inR (which must not be
     * the same buffers). Every wall hears one channel: the left wall the left and the right wall
     * the right, and of the four walls straight ahead, behind, above and below, two each -- those
     * come back from the middle whichever channel they heard, so the split only changes their
     * delays. Identical channels excite the room exactly as the mono sum used to. Halves of both
     * channels on the middle walls were tried first and cancel an anti-phase pair there: it
     * reached the room 10.7 dB down, from the side walls alone.
     * @param inL  the near bus, left, n samples
     * @param inR  the near bus, right
     * @param L    left output; the reflections are added
     * @param R    right output, likewise
     * @param n    samples in the block
     */
    void process(const float* inL, const float* inR, float* L, float* R, int n)
    {
        if (!active()) { w_ = (w_ + n) & 0x3FFFFFFF; return; }
        const float lvlStep = 1.0f / static_cast<float>(std::max(1, n));
        // Every reflection loses this much on top of what the wall filter takes: the matrix below
        // conserves energy exactly, so this factor alone decides how long the early field runs.
        const float g = (1.0f - absorb_) * 0.8f;
        for (int i = 0; i < n; ++i) {
            smLevel_ += (level_ - smLevel_) * lvlStep;
            inL_[static_cast<size_t>(w_ & mask_)] = inL[i];
            inR_[static_cast<size_t>(w_ & mask_)] = inR[i];
            // The source's own path glides rather than jumps: a delay that steps is a click, and
            // the near bus's centre of gravity moves whenever a voice starts or stops.
            float inc[kWalls][kWalls];
            float sum[kWalls], inject[kWalls];
            for (int k = 0; k < kWalls; ++k) {
                dSrc_[k] += 0.0005f * (dSrcT_[k] - dSrc_[k]);
                gSrc_[k] += 0.0005f * (gSrcT_[k] - gSrc_[k]);
                // The walls' distances glide too. setRoom() runs every block from the parameters,
                // and while the source's distance already glided, the ear and wall-pair delays
                // were set outright: automating Early Size moved six read heads by hundreds of
                // samples at once, a click per block.
                dEar_[k] += 0.0005f * (dEarT_[k] - dEar_[k]);
                inject[k] = (kFromLeft[k] * ringRead(inL_.data(), mask_, w_, dSrc_[k])
                           + kFromRight[k] * ringRead(inR_.data(), mask_, w_, dSrc_[k])) * gSrc_[k];
                float s = inject[k];
                for (int j = 0; j < kWalls; ++j) {
                    if (j == k) { inc[j][k] = 0.0f; continue; }
                    dPair_[j][k] += 0.0005f * (dPairT_[j][k] - dPair_[j][k]);
                    inc[j][k] = ringRead(pair_[j][k].data(), mask_, w_, dPair_[j][k]);
                    s += inc[j][k];
                }
                sum[k] = s;
            }
            float wetL = 0.0f, wetR = 0.0f;
            for (int k = 0; k < kWalls; ++k) {
                // The wall takes its bite out of everything that reaches it.
                lp_[k] += lpCoef_ * (sum[k] - lp_[k]);
                // Isotropic scattering: what leaves towards one neighbour is the average of
                // everything that arrived, less what that neighbour itself sent. Written out over
                // five incoming waves the factor is two fifths, and the matrix it forms is its own
                // inverse -- it moves energy between the walls without creating or losing any.
                const float c = 0.4f * lp_[k];
                for (int m = 0; m < kWalls; ++m) {
                    if (m == k) continue;
                    pair_[k][m][static_cast<size_t>(w_ & mask_)] = g * (c - inc[m][k]);
                }
                // What the listener hears of this wall, delayed by its own distance and placed by
                // its direction.
                press_[k][static_cast<size_t>(w_ & mask_)] = c;
                const float toEar = ringRead(press_[k].data(), mask_, w_, dEar_[k]) * gEar_[k];
                wetL += toEar * panL_[k];
                wetR += toEar * panR_[k];
            }
            L[i] += wetL * smLevel_;
            R[i] += wetR * smLevel_;
            ++w_;
        }
    }

private:
    /**
     * @brief Wall to wall and wall to listener: these change only when the room changes.
     *
     * From each node's position: its delay and gain to the ear (spherical spreading and the
     * absorption), its pan from its axis and the width, and the delay to every other node. A
     * delay that has never been set is snapped to its target, the others glide in process().
     */
    void roomGeometry()
    {
        const float c = 343.0f;
        const float maxD = static_cast<float>(mask_ - 8);
        for (int k = 0; k < kWalls; ++k) {
            float dnl = 0.0f;
            for (int a = 0; a < 3; ++a) dnl += node_[k][a] * node_[k][a];
            dnl = std::sqrt(dnl);
            dEarT_[k] = std::min(maxD, dnl / c * static_cast<float>(sr_) + 1.0f);
            if (dEar_[k] <= 0.0f) dEar_[k] = dEarT_[k];   // first time: nothing to glide from
            gEar_[k] = (1.0f - absorb_) / (1.0f + dnl);
            // Direction: the wall's own axis, widened or narrowed by Width.
            const float x = node_[k][0] / std::max(0.001f, dnl);
            const float pan = clampv(x * width_, -1.0f, 1.0f);
            panL_[k] = std::sqrt(0.5f * (1.0f - pan));
            panR_[k] = std::sqrt(0.5f * (1.0f + pan));
            for (int m = 0; m < kWalls; ++m) {
                if (m == k) { dPairT_[k][m] = 1.0f; dPair_[k][m] = 1.0f; continue; }
                float d2 = 0.0f;
                for (int a = 0; a < 3; ++a) { const float u = node_[m][a] - node_[k][a]; d2 += u * u; }
                dPairT_[k][m] = std::min(maxD, std::sqrt(d2) / c * static_cast<float>(sr_) + 1.0f);
                if (dPair_[k][m] <= 0.0f) dPair_[k][m] = dPairT_[k][m];
            }
        }
    }

    /** @brief Source to wall: recomputed whenever the source moves, and glided into place per sample. */
    void sourceGeometry()
    {
        const float c = 343.0f;
        const float maxD = static_cast<float>(mask_ - 8);
        for (int k = 0; k < kWalls; ++k) {
            float dsn = 0.0f;
            for (int a = 0; a < 3; ++a) { const float u = node_[k][a] - src_[a]; dsn += u * u; }
            dsn = std::sqrt(dsn);
            dSrcT_[k] = std::min(maxD, dsn / c * static_cast<float>(sr_) + 1.0f);
            gSrcT_[k] = 1.0f / (1.0f + dsn);          // spherical spreading over the first leg
        }
    }

    /**
     * @name Which channel of the near bus each wall hears (left, right, front, back, ceiling, floor).
     * @{ */
    static constexpr float kFromLeft[kWalls]  = { 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f };   ///< 1 where the wall hears the left channel: left, front, ceiling
    static constexpr float kFromRight[kWalls] = { 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f };   ///< 1 where the wall hears the right channel: right, back, floor
    /** @} */
    std::vector<float> inL_,   ///< the near bus's left channel, a ring the source paths read
                       inR_;   ///< the near bus's right channel
    std::vector<float> press_[kWalls];   ///< what leaves each wall, a ring per wall, read by the ear path
    std::vector<float> pair_[kWalls][kWalls];   ///< what wall k sends towards wall m, a ring per pair
    int    mask_ = 0,   ///< ring size - 1
           w_ = 0;      ///< the write index, shared by every ring
    double sr_ = 48000.0;   ///< sample rate in Hz
    float  node_[kWalls][3] = {},   ///< each wall's node in metres, the listener at the origin
           half_[kWalls] = {};      ///< half the room's extent along each wall's axis, in metres
    float  src_[3] = { 0.0f, 2.0f, 0.0f };   ///< the source's position in metres
    float  dPair_[kWalls][kWalls] = {},    ///< wall-to-wall delays in samples as they glide
           dPairT_[kWalls][kWalls] = {};   ///< the delays the geometry asks for
    float  dSrc_[kWalls] = {},    ///< source-to-wall delays as they glide
           dSrcT_[kWalls] = {},   ///< the delays the source position asks for
           gSrc_[kWalls] = {},    ///< source-to-wall gains as they glide
           gSrcT_[kWalls] = {};   ///< the gains the source position asks for, 1 / (1 + distance)
    float  dEar_[kWalls] = {},    ///< wall-to-ear delays as they glide
           dEarT_[kWalls] = {},   ///< the delays the room asks for
           gEar_[kWalls] = {};    ///< wall-to-ear gains, (1 - absorb) / (1 + distance)
    float  panL_[kWalls] = {},   ///< each wall's gain into the left output
           panR_[kWalls] = {};   ///< and into the right, equal power from its direction and the width
    float  lp_[kWalls] = {};   ///< each wall's absorption one-pole state
    float  lpCoef_ = 1.0f;   ///< the absorption one-pole's coefficient, from absorb_
    float  room_ = 8.0f,      ///< the room's longest dimension in metres
           absorb_ = 0.35f,   ///< the Absorb knob
           width_ = 1.0f,     ///< the Width knob
           level_ = 0.0f,     ///< the level asked for
           smLevel_ = 0.0f;   ///< the level as it glides across the block
};

} // namespace ambient
