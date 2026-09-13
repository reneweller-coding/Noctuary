// Noctuary -- stereo effects.
//   Ensemble    modulated three-tap chorus
//   StereoDelay asymmetric L/R delay with cross-feed and damping (time-based width)
//   Reverb      8-line feedback delay network with diffusion, damping, freeze,
//               L/R asymmetry and a tail high-cut (the dark "infinite background")
//   MidSide     mono bass below a crossover, gentle side upper-mid lift, width
// Buffers are allocated in prepare() only.
#pragma once
#include "Dsp.h"
#include <vector>

namespace ambient {

class Ensemble {
public:
    void prepare(double sampleRate);
    void set(float mix, float depth, float rateHz) { mix_ = mix; depth_ = depth; rate_ = rateHz; }
    // Chorus (0) is the three modulated taps this has always been. Microshift (1) is the other
    // way a mix engineer widens a drone: the two channels are detuned by a few cents in opposite
    // directions and delayed by different amounts, with nothing modulated. It survives a mono
    // sum -- two channels a few cents apart are never at a fixed phase, so there is no comb to
    // cancel into -- where a deep chorus at 13 to 22 ms is exactly a comb filter waiting to be
    // summed. Depth becomes the detune in cents, Rate a very slow wander of it.
    // Velvet (2) is the third way, and the one the decorrelation literature settled on
    // (Valimaki, Alary and Politis 2017): each channel is convolved with its own sparse
    // random impulse response -- a few hundred impulses of +-1 a second, one per equal
    // interval at a random position inside it, which is "velvet noise". A sparse sequence of
    // signed unit impulses has a flat magnitude response, so the two channels come out
    // decorrelated without being coloured, and nothing is modulated, so nothing warbles.
    void setMode(int mode) { mode_ = mode; }
    void process(float* L, float* R, int n);
private:
    void processChorus(float* L, float* R, int n);
    void processShift(float* L, float* R, int n);
    void processVelvet(float* L, float* R, int n);
    void buildVelvet(uint64_t seed);
    static constexpr int kVelvetTaps = 96;      // impulses per channel
    int   velvetPos_[2][kVelvetTaps] = {};      // where each impulse sits, in samples
    float velvetSign_[2][kVelvetTaps] = {};     // +-1, with the 1/sqrt(N) gain folded in
    int   velvetLen_ = 0;                       // the sequence's length in samples
    std::vector<float> bufL_, bufR_;
    int    mask_ = 0, w_ = 0;
    double sr_ = 48000.0;
    double ph_[3] = { 0.0, 0.33, 0.66 };
    float  mix_ = 0.4f, depth_ = 0.4f, rate_ = 0.2f;
    int    mode_ = 0;
    // Microshift state: the two shifters' window phases and the slow wander of the detune.
    double shPh_[2] = { 0.0, 0.5 };
    double wanderPh_ = 0.0;
};

class StereoDelay {
public:
    void prepare(double sampleRate);
    // absorb: with it up, the loop also loses its low end and its high cut sinks as the feedback
    // rises, so long echoes drown into a fog instead of merely getting quieter.
    void set(float timeL, float timeR, float feedback, float cross, float damping, float absorb = 0.0f);
    // Duck: how far the loop's high cut is pulled down while the input is loud. The idea is the
    // one the far reverb's Unmask already uses -- get out of the way of the thing being played --
    // applied to the echoes: a fresh attack should not have to fight the brightness of the last
    // one's tail. At 0 the loop behaves exactly as it did.
    void setDuck(float amount) { duck_ = clampv(amount, 0.0f, 1.0f); }
    // Writes the wet signal only; the caller mixes it.
    void process(const float* inL, const float* inR, float* wetL, float* wetR, int n);
private:
    std::vector<float> bufL_, bufR_;
    int    mask_ = 0, w_ = 0;
    double sr_ = 48000.0;
    float  tL_ = 0, tR_ = 0, tLcur_ = 0, tRcur_ = 0;
    float  fb_ = 0.5f, cross_ = 0.3f, lpc_ = 0.5f;
    float  lpL_ = 0, lpR_ = 0;
    float  absorb_ = 0.0f, hpc_ = 0.0f, lpc2_ = 1.0f;   // the absorption band, from feedback and absorb
    float  loL_ = 0, loR_ = 0, hiL_ = 0, hiR_ = 0;
    float  duck_ = 0.0f, duckEnv_ = 0.0f, duckFast_ = 0.0f;   // how much, and the two envelopes driving it
    double modPh_[2] = { 0.0, 0.5 };
};

class Reverb {
public:
    void prepare(double sampleRate);
    void set(float size, float decaySeconds, float damping, float preDelayMs, bool freeze, float mix);
    // asymmetry 0..1: right-hand lines lengthened and the right output delayed by up to 10 ms
    // highcutHz: one-pole low-pass on the wet output (20000 = off)
    // lowcutHz:  two one-pole high-passes on the wet output, 12 dB/oct (20 = off). The other half
    //            of the filter funnel a mixing engineer puts on a reverb return: without it the
    //            tail piles up in the low mids, where it turns the whole picture to mud rather
    //            than sitting behind it.
    void setSpace(float asymmetry, float highcutHz, float lowcutHz = 20.0f);
    // Classic (0) is the network as it always was. Scattering (1) puts a Schroeder all-pass
    // inside every delay line's loop, after Schlecht and Habets (2020): each pass round the
    // network then scatters every echo into many, so the echo density grows much faster
    // (measured: 0.60 -> 0.76 of Gaussian after 50 ms) while the late tail stays at least as
    // smooth. Same decay, same level, same lines; a denser texture of tail.
    //
    // Colourless (2) is Scattering with a second change: the eight line lengths are not the
    // hand-picked primes of the classic network but a set searched offline for the flattest
    // magnitude response (Tools/optimise_fdn.py, after the colourless-FDN work of Dal Santo,
    // Prawda, Schlecht and Valimaki). A delay network's tail is a sum of modes at the lines'
    // own frequencies, and where those pile up the tail rings; lengths chosen against that
    // measure ring less -- 0.31 dB of third-octave spread against the classic set's 0.47.
    // Rotating (3) is Colourless with the feedback changed: the line lengths stop moving (the
    // classic network wobbles each by a sample and a half, which is how it keeps its modes from
    // ringing) and the feedback matrix turns instead -- eight Givens rotations on pairs of
    // lines, their angles advancing at a few hundredths of a hertz, in front of the Householder
    // reflection the network always had. A product of rotations and a reflection is orthogonal
    // whatever the angles are, so the loop is lossless at every instant and the tail loses
    // energy only through the gains and the damping, as designed; and because the modes are
    // being re-mixed rather than re-tuned, nothing in the tail is pitch-modulated (Schlecht and
    // Habets 2015, time-varying feedback matrices). Measured: the band pattern of the tail
    // drifts from one moment to the next where the fixed network's stays put.
    void setMode(int mode) { mode_ = mode; }
    void process(float* L, float* R, int n);
private:
    static constexpr int kLines = 8;
    static constexpr int kRotations = 8;
    float  rot_ = 0.0f;                                // 0 fixed matrix .. 1 turning, glided
    float  rotC_[kRotations] = {}, rotS_[kRotations] = {};     // the angles, as cos and sin
    float  rotDc_[kRotations] = {}, rotDs_[kRotations] = {};   // their per-sample advance
    int    rotNorm_ = 0;
    static constexpr int kAllpasses = 4;
    std::vector<float> line_[kLines];
    std::vector<float> ap_[kAllpasses], apR_[kAllpasses];   // the input diffusion, a chain per channel
    std::vector<float> pre_, preR_, outR_;                  // the pre-delay, a ring per channel
    int    mask_ = 0, w_ = 0, outMask_ = 0;
    double sr_ = 48000.0;
    // A one-pole DC blocker on the input, 5 Hz. A network whose loop gain is a hair under one
    // -- a forty-second tail -- passes an offset with a gain of hundreds: measured, a texture
    // with -0.1 of offset stood at -2.1 on the far bus after thirty seconds, the Patina's
    // clipper railed on it and the output DC blocker turned the rail into silence.
    float  dcInX_[2] = {}, dcInY_[2] = {}, dcR_ = 0.999f;
    float  lenTarget_[kLines] = {}, lenCur_[kLines] = {};
    float  gain_[kLines] = {};
    float  lp_[kLines] = {};
    double modPh_[kLines] = {};
    float  modRate_[kLines] = {};
    int    apLen_[kAllpasses] = {};
    float  preTarget_ = 0.0f, preCur_ = 0.0f;
    float  outDelayTarget_ = 0.0f, outDelayCur_ = 0.0f;
    float  damp_ = 0.4f, mix_ = 0.45f, decay_ = 12.0f, size_ = 1.6f, asym_ = 0.0f;
    float  hcCoef_ = 1.0f, hcL_ = 0.0f, hcR_ = 0.0f;
    float  lcCoef_ = 0.0f;                                        // 0 = off
    float  lcL1_ = 0.0f, lcR1_ = 0.0f, lcL2_ = 0.0f, lcR2_ = 0.0f;
    bool   freeze_ = false;
    int    mode_ = 0;
    std::vector<float> sc_[kLines];   // the scattering all-passes, one per line
    int    scLen_[kLines] = {};
};

// The background steps aside for the foreground, band by band. A mixing engineer rides the
// reverb return down while a line is sounding and lets it back up in the gaps; done per band it
// is what keeps a dense pad from swallowing its own notes. Three bands (below 300 Hz, 300 Hz to
// 2.5 kHz, above), the near bus as the side chain, fast to duck and slow to return -- the return
// is the part the ear hears as the room breathing back in.
class Unmask {
public:
    void prepare(double sampleRate);
    // spread: how far a loud low band also ducks the bands above it. Masking in the ear is
    // asymmetric -- a low tone masks the frequencies above it far more than those below
    // (Zwicker and Fastl 1999, the upward spread of masking) -- and at 0 the three bands
    // are independent, as they always were.
    // returnSeconds: how long the far bus takes to come back after a duck (the duck itself is
    // 50 ms). 1.2 s is what it always was; longer, and the horizon's return is a gesture.
    void set(float amount, float spread = 0.0f, float returnSeconds = 1.2f);   // 0 = off (and then not computed at all)
    // Ducks far[] where near[] has energy, in place.
    void process(const float* nearL, const float* nearR, float* farL, float* farR, int n);
    void reset();
private:
    struct Split { float lo = 0.0f, mid = 0.0f; };   // one-pole state per crossover per channel
    Split  sNear_[2], sFar_[2];
    float  env_[3] = {};                              // the near bus's band envelopes
    float  gain_[3] = { 1.0f, 1.0f, 1.0f };           // what the far bus is multiplied by, smoothed
    float  aCoef_ = 0.01f, rCoef_ = 0.0005f;
    float  c1_ = 0.02f, c2_ = 0.2f;                   // crossover coefficients (300 Hz, 2.5 kHz)
    float  amount_ = 0.0f, spread_ = 0.0f, returnSec_ = 1.2f;
    double sr_ = 48000.0;
};

// The master's age: tape wow, the highs a worn machine loses, a noise floor that lives under the
// music, and a gentle saturation. Every one of them is a defect, and together they are most of
// what separates a recording from a render. Off at amount 0, and then bypassed entirely.
class Patina {
public:
    void prepare(double sampleRate, uint64_t seed);
    void set(float amount, float wow, float hiss, float age);
    void process(float* L, float* R, int n);
    void reset();
    bool active() const { return amount_ > 0.0f; }   // off at 0, and then not computed at all
private:
    std::vector<float> bufL_, bufR_;
    int     mask_ = 0, w_ = 0;
    double  sr_ = 48000.0;
    float   amount_ = 0.0f, wow_ = 0.3f, hiss_ = 0.2f, age_ = 0.3f;
    float   lpC_ = 1.0f, lpL_ = 0.0f, lpR_ = 0.0f;
    float   env_ = 0.0f;
    Drifter wowDrift_;
    double  flutterPh_ = 0.0;
    Rng     rng_;
};

class MidSide {
public:
    void prepare(double sampleRate);
    void set(float bassMonoHz, float sideAirDb, float width);
    // Tilt: a see-saw around the pivot -- one first-order low pass and its complement, weighted
    // against each other. Flat at 0 dB and then not computed at all.
    void setTilt(float dB, float pivotHz);
    void process(float* L, float* R, int n);
    // Mono safety. Everything upstream is built to widen -- all-pass phase width, asymmetric
    // delays, a reverb whose two sides are deliberately different -- and a drone that is
    // gigantic in stereo can vanish when it is summed to mono. The side channel is measured
    // against the mid over a long window and the width is eased back only when the side actually
    // dominates: a slow, small correction that a listener cannot hear and a mono sum can.
    void setMonoGuard(bool on) { guardOn_ = on; }
    float widthTrim() const { return guard_; }   // 1 = untouched, for the meter

private:
    Svf    hp_, air_;
    float  airGain_ = 0.0f, width_ = 1.0f;
    float  midPow_ = 0.0f, sidePow_ = 0.0f, guard_ = 1.0f;
    bool   guardOn_ = true;
    float  tiltLo_ = 1.0f, tiltHi_ = 1.0f, tiltC_ = 0.1f, tiltState_[2] = {};
    bool   tiltOn_ = false;
    double sr_ = 48000.0;
};

// A diffusion field: four modulated all-passes that turn an impulse into a swell before the
// reverb ever sees it. A feedback network answers immediately by construction; this is what
// gives a tail the slow arrival a large room has.
class Diffuser {
public:
    void prepare(double sampleRate);
    void set(float amount);
    void process(float* L, float* R, int n);
    void reset();
private:
    static constexpr int kStages = 4;
    std::vector<float> buf_[2][kStages];
    int    len_[kStages] = {};
    int    mask_ = 0, w_ = 0;
    double modPh_[kStages] = { 0.0, 0.3, 0.6, 0.85 };
    float  amount_ = 0.0f;
    double sr_ = 48000.0;
};

// The granular cloud of the far plane lives in Cloud.h.

// Read `delay` samples (>= 1, fractional) behind write index `w` from a power-of-two ring.
inline float ringRead(const float* buf, int mask, int w, float delay)
{
    const int di = static_cast<int>(delay);
    const float f = delay - static_cast<float>(di);
    const float a = buf[(w - di) & mask];
    const float b = buf[(w - di - 1) & mask];
    return a + f * (b - a);
}

// The frequency-selective Haas effect. Delaying a whole channel by ten to thirty milliseconds
// widens it and destroys it in mono: the two channels comb, and the comb's first notch lands in
// the bass. Done to one band only -- roughly 1.2 to 4 kHz, where the ear takes its direction from
// level rather than from time -- and cross-fed, so each side hears the other's delayed band six
// decibels down, the width appears at the edges and the low end and the top stay exactly where
// they were. Off at amount 0, and then not computed.
class HaasBand {
public:
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
    void set(float amount, float timeMs) { amount_ = clampv(amount, 0.0f, 1.0f); timeMs_ = clampv(timeMs, 1.0f, 45.0f); }
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
    std::vector<float> bufL_;                       // one band, one line: it goes to the sides
    int    mask_ = 0, w_ = 0;
    double sr_ = 48000.0;
    Svf    hp_[1], lp_[1];
    float  amount_ = 0.0f, smAmount_ = 0.0f, timeMs_ = 15.0f, smDelay_ = 0.0f;
};



// The early reflections of a room, from its geometry rather than from a reverb's statistics.
//
// A feedback delay network makes a plausible tail out of numbers that mean nothing in particular.
// The first reflections are not statistics: they arrive from the six surfaces of the room at times
// and from directions that follow from where the source is standing, and the ear reads those times
// and directions as the size of the room and the distance of the source (Blauert 1997 for the
// direction, Bronkhorst and Houtgast 1999 for the distance). A reverb whose early part does not
// move when the source moves is a room the source is not in.
//
// This is the scattering delay network of De Sena, Hacihabiboglu and Cvetkovic (2015) in its
// simplest useful form: one node at the centre of each of the six walls of a shoebox, a delay from
// the source to each node and from each node to the listener, an absorption filter at each node,
// and a single scattering stage that feeds every node from all the others so the reflections go on
// reflecting. The first arrivals are then at the geometrically right times, from the right
// directions, and they move when the source does -- which is the whole point -- while the later
// energy is handed to the far reverb, whose job it already is.
//
// One simplification is worth naming. The source position is one position for the whole near bus,
// taken from where the voices actually are (the level-weighted mean of their pan and distance),
// not one per voice: six delay lines per voice would cost more than the rest of the instrument.
// What that buys is a room whose early pattern follows the music; what it gives up is a different
// early pattern for two voices sounding at once from different places.
class EarlyRoom {
public:
    static constexpr int kWalls = 6;

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

    // size: the room's longest dimension in metres (the shoebox is size x 0.8 size x 0.45 size,
    // proportions with no simple ratio between them, so its own modes do not pile up).
    // absorb: how much each surface takes out per reflection, and how dark what comes back is.
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

    // Where the source stands, from the near bus's own voices: pan -1..1 across the room, and
    // distance 0..1 from the listener towards the far wall.
    void setSource(float pan, float distance)
    {
        const float p = clampv(pan, -1.0f, 1.0f), d = clampv(distance, 0.0f, 1.0f);
        src_[0] = p * (half_[0] - 0.5f);
        src_[1] = 0.8f + d * (half_[2] - 1.0f);   // in front of the listener
        src_[2] = 0.0f;
        sourceGeometry();
    }

    void setLevel(float level) { level_ = clampv(level, 0.0f, 1.0f); }
    bool active() const { return level_ > 0.0f || smLevel_ > 1.0e-5f; }

    // Adds the room's early reflections to L/R, from the near bus in inL/inR (which must not be
    // the same buffers). Every wall hears one channel: the left wall the left and the right wall
    // the right, and of the four walls straight ahead, behind, above and below, two each -- those
    // come back from the middle whichever channel they heard, so the split only changes their
    // delays. Identical channels excite the room exactly as the mono sum used to. Halves of both
    // channels on the middle walls were tried first and cancel an anti-phase pair there: it
    // reached the room 10.7 dB down, from the side walls alone.
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
    // Wall to wall and wall to listener: these change only when the room changes.
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

    // Source to wall: recomputed whenever the source moves, and glided into place per sample.
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

    // Which channel of the near bus each wall hears (left, right, front, back, ceiling, floor).
    static constexpr float kFromLeft[kWalls]  = { 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f };
    static constexpr float kFromRight[kWalls] = { 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f };
    std::vector<float> inL_, inR_;
    std::vector<float> press_[kWalls];
    std::vector<float> pair_[kWalls][kWalls];
    int    mask_ = 0, w_ = 0;
    double sr_ = 48000.0;
    float  node_[kWalls][3] = {}, half_[kWalls] = {};
    float  src_[3] = { 0.0f, 2.0f, 0.0f };
    float  dPair_[kWalls][kWalls] = {}, dPairT_[kWalls][kWalls] = {};
    float  dSrc_[kWalls] = {}, dSrcT_[kWalls] = {}, gSrc_[kWalls] = {}, gSrcT_[kWalls] = {};
    float  dEar_[kWalls] = {}, dEarT_[kWalls] = {}, gEar_[kWalls] = {};
    float  panL_[kWalls] = {}, panR_[kWalls] = {};
    float  lp_[kWalls] = {};
    float  lpCoef_ = 1.0f;
    float  room_ = 8.0f, absorb_ = 0.35f, width_ = 1.0f, level_ = 0.0f, smLevel_ = 0.0f;
};

} // namespace ambient
