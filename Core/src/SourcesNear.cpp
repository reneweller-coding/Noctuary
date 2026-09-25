/**
 * @file SourcesNear.cpp
 * @brief The near sources (13.09.2026): what the instrument plays close to the ear.
 *
 * Every source the instrument had was a plane -- a bank, a table, a recording, a string, all of
 * them made to be sustained and to be sent back into the far reverb. Rich's foreground is
 * something else: a flute blown once, water falling into a bowl, a voice on a radio, a rim rubbed
 * until it sings. Four models, each a physical caricature small enough to run in every voice:
 *
 *   Flute   a blown pipe after the jet-drive waveguide of Cook (STK), with the loop closed the
 *           way an open pipe closes it -- the jet's travel time sets the register, so the same
 *           pipe overblows to its octave when the embouchure shortens the jet.
 *   Murmur  a voice that never says anything: a glottal pulse through three formants that walk
 *           between vowels at a syllable's pace, consonants as bursts of noise, phrases and pauses,
 *           and behind it a radio -- a band, a saturation, the hiss of the carrier, the squelch
 *           that closes after every transmission, the Quindar tones Apollo keyed its air with.
 *   Bowl    a set of modes rubbed by a stick, the bow's own friction curve (Sources.cpp) driving
 *           a modal body instead of a string. Every mode is a doublet a hair apart, which is
 *           where a real bowl's beating comes from (P1: the instrument wants beats).
 *   Ice     the same friction on a low, dense, short-lived set of modes, and a slip clock in
 *           front of it: ice or old wood creaking under a slow load, not struck, stretched.
 *   Drops   water falling into a vessel, after van den Doel (2005): a drop is a bubble, a sine
 *           whose pitch RISES as it decays, and the vessel it falls into rings after the click.
 *
 * Each is calibrated against the wavetable slot at the same Level, as the Bow is, and the selftest
 * holds them to it.
 *
 * The file also holds the Clip type (the recording played as it is, once, from Position) and the
 * second foreground round, the signals (Whistler, Shaker, Chime, Geiger, Tube, Krell, Beacon,
 * Morse, Dial), each a SourceSlot::render* method that SourceSlot::render() in Sources.cpp
 * dispatches to on the audio thread, one control block at a time, adding into its output. All of
 * them keep their state in the SourceSlot (Sources.h) and start it afresh at every fresh note and
 * at every change of type, which is what the `Ready_` flags (fluteReady_, rubReady_, murReady_, sigReady_) are for.
 */
#include "ambient/Sources.h"
#include "ambient/Voice.h"     // kControlBlock
#include <cmath>
#include <cstring>
#include <algorithm>

namespace ambient {

namespace {

/**
 * @brief A one-pole delay's share of a loop, in samples at the fundamental: (1 - g) / g.
 * @param g  the one-pole's coefficient, 0 .. 1 (floored at 0.001 so a closed filter does not divide by zero)
 * @return   the samples to take off the loop's delay line so the loop stays one period long
 */
inline double onePoleDelay(float g) { return (1.0 - static_cast<double>(g)) / std::max(static_cast<double>(g), 1.0e-3); }

/**
 * @brief The bow's friction, as one function (Sources.cpp keeps its own copy inside the string loop):
 *        the Stribeck curve of the waveguide literature, sticking while the relative velocity is small,
 *        slipping once it is not.
 * @param dv     the relative velocity between the stick (or bow) and the body under it
 * @param slope  how steep the curve falls with |dv|: 5 at no Force, 1 at full, so a heavier hand sticks wider
 * @return       the friction force, dv times the curve, clamped to -1 .. 1
 */
inline float friction(float dv, float slope)
{
    float rho = std::pow(std::fabs((dv - 0.001f) * slope) + 0.75f, -4.0f);
    if (rho > 1.0f) rho = 1.0f;
    return clampv(dv * rho, -1.0f, 1.0f);
}

/**
 * @brief The vowels the murmur walks between: F1, F2, F3 in hertz (Peterson and Barney's men, rounded),
 *        and a schwa in the middle where every unstressed syllable goes.
 */
constexpr float kVowels[6][3] = {
    { 730.0f, 1090.0f, 2440.0f },   // a
    { 530.0f, 1840.0f, 2480.0f },   // e
    { 270.0f, 2290.0f, 3010.0f },   // i
    { 570.0f,  840.0f, 2410.0f },   // o
    { 300.0f,  870.0f, 2240.0f },   // u
    { 500.0f, 1500.0f, 2500.0f },   // schwa
};

/**
 * @brief The modes of the two rubbed bodies, as ratios to the lowest, with each mode's weight at the
 *        rim and how long it rings against the lowest.
 *
 * A thin-walled bowl's (n,0) modes go roughly as
 * (n^2 - 1), which is the 1 : 2.7 : 5 : 7.8 ladder every singing bowl has; ice and old wood are
 * plate-like, dense and inharmonic, and their modes die in a second.
 */
struct RubBody {
    float ratio[SourceSlot::kRubModes];    ///< each mode's frequency as a ratio to the lowest
    float weight[SourceSlot::kRubModes];   ///< each mode's amplitude at the rim, the lowest at 1
    float ring[SourceSlot::kRubModes];     ///< each mode's ring time as a fraction of t60
    float t60;                             ///< how long the lowest mode takes to fall 60 dB, in seconds
};
constexpr RubBody kBowlBody = { { 1.0f, 2.71f, 4.98f, 7.78f, 11.0f, 14.6f }, { 1.0f, 0.55f, 0.32f, 0.18f, 0.10f, 0.06f }, { 1.0f, 0.7f, 0.5f, 0.35f, 0.25f, 0.18f }, 14.0f };   ///< the singing bowl: the (n^2 - 1) ladder, ringing for fourteen seconds
constexpr RubBody kIceBody  = { { 1.0f, 1.58f, 2.24f, 3.02f, 3.98f, 5.1f },  { 1.0f, 0.8f, 0.6f, 0.45f, 0.3f, 0.2f },     { 1.0f, 0.8f, 0.6f, 0.45f, 0.35f, 0.25f }, 1.4f };   ///< ice and old wood: dense, inharmonic, dead within a second and a half

} // namespace

// ---------------------------------------------------------------- Flute

/**
 * @brief The pipe is a loop of one period: a delay for the round trip, a one-pole at the far end for the
 *        losses (radiation and the walls take the highs first), and the air jet at the embouchure.
 *
 * The
 * jet is deflected by the acoustic velocity at the hole, travels to the edge in a time of its own
 * -- the jet delay -- and there it is switched into or out of the pipe by a soft cubic (Cook's
 * jet table, x - x^3). The sign is what makes it a flute: the jet works AGAINST the wave that
 * deflected it, and with a travel time of half a period that inversion arrives back in phase, so
 * the pipe speaks its fundamental. Shorten the travel to a quarter period and the octave is in
 * phase instead: that is overblowing, and Position is the embouchure that does it. Breath
 * pressure (Force) sets how hard the jet is driven, and the loop's small-signal gain with it,
 * so a light breath does not speak at all and a heavy one saturates towards the cubic's limit;
 * Speed is the air's own noise, the turbulence a real jet always carries, part of it into the
 * pipe (where it is filtered into breathiness) and part straight out.
 *
 * The loop's length is the period less what the filter and the feedback sample already delay,
 * as the bow's is, and the selftest measures the pitch across the register against the
 * instrument's own tuning.
 */
void SourceSlot::renderFlute(float* out, int n, double hz, const SlotParams& p, float dt)
{
    (void)dt;
    // A flute has a bottom: asked for a note below it, it plays the note an octave up, as a
    // player would. (Measured: at 110 Hz the loop, with the jet's low pass down at 165 Hz, no
    // longer found its fundamental at all.)
    double f0 = hz > 25.0 ? hz : 25.0;
    while (f0 < 180.0) f0 *= 2.0;
    const double period = sr_ / f0;
    if (period < 6.0 || period >= static_cast<double>(kBowMax - 4)) { std::memset(out, 0, sizeof(float) * static_cast<size_t>(n)); return; }
    if (!fluteReady_) {
        std::fill(bowNut_.begin(), bowNut_.end(), 0.0f);      // the bore
        std::fill(bowBridge_.begin(), bowBridge_.end(), 0.0f); // the jet's travel
        bowW_ = 0; fluteRefl_ = 0.0f; fluteBreath_ = 0.0f; fluteHpX_ = fluteHpY_ = 0.0f; fluteJetLp_ = 0.0f;
        fluteNoiseBp1_ = fluteNoiseBp2_ = 0.0f;
        flutePhase_ = rng_.uniform();
        fluteVibHz_ = 4.4 + 1.4 * static_cast<double>(rng_.uniform());   // a player's vibrato, one per note
        fluteReady_ = true;
    }
    const float force = clampv(p.bowForce, 0.0f, 1.0f);
    const float air   = clampv(p.bowSpeed, 0.0f, 1.0f);
    // The breath rises over a quarter of a second, and the air comes before the tone: below the
    // pressure at which the jet's gain beats the pipe's losses only the turbulence is heard, the
    // pipe filtering it into breath, and the note grows out of that once the breath is there.
    // Rich's flutes begin exactly so. (A one-pole per SAMPLE: the first version took the block's
    // seconds for a per-sample coefficient and was at full pressure in four milliseconds.)
    const float pressure = 0.22f + 0.78f * force;
    const float rise = 1.0f - std::exp(-1.0f / (0.1f * static_cast<float>(sr_)));
    // Losses: Bright is the pipe's end filter, as it is the string's loop filter; and the pipe
    // loses seven per cent of its wave per round trip whatever Bright says -- a flute's Q is
    // about thirty. That loss is what the jet's saturation is balanced against: with a nearly
    // lossless pipe (0.985) and the same jet the amplitude had no limit at all, grew to the
    // guard, reset, and the pitch that was measured was the pitch of a loop restarting itself.
    const float lpCoef = 0.35f + 0.6f * clampv(p.bright, 0.0f, 1.0f);
    const float loopGain = 0.93f;
    // The jet's travel as a fraction of the period: half a period speaks the fundamental, a quarter
    // the octave. Position is the embouchure: the lower half of the knob the one, the upper half
    // the other. Not a slide between them -- a jet standing anywhere between pulls the pitch
    // (simulated: +5 cents at 0.475, +17 at 0.4), which is what a player's embouchure does and
    // what a source that is asked for a note must not.
    const double jetRatio = p.position < 0.5f ? 0.5 : 0.25;
    // The jet is a slow thing: its deflection cannot follow the pipe's upper modes, and without
    // that the loop at A3 chose its twelfth over its fundamental (both are in phase for a jet at
    // half a period). A one-pole at one and a half times the register's frequency on what the jet
    // reads; its own phase delay at that frequency is taken off the jet's line, so the register
    // stays exactly where it was.
    const double freg = 0.5 * f0 / jetRatio;
    const double fc = 1.5 * freg;
    const float  gj = 1.0f - std::exp(static_cast<float>(-kTwoPi * fc / sr_));
    // The one-pole's phase delay at the register's frequency, in samples, for the DIGITAL
    // one-pole and not its continuous cousin: the two differ by four tenths of a sample at C6,
    // which the loop answered with a constant two cents sharp across the whole register.
    const double wr = kTwoPi * freg / sr_;
    const double jetLpDelay = std::atan2((1.0 - gj) * std::sin(wr), 1.0 - (1.0 - gj) * std::cos(wr)) / wr;
    // Vibrato: Pos Drift is its depth, a third of a percent of pitch at 1 -- a breath, not a wobble.
    const double vibDepth = 0.0035 * static_cast<double>(clampv(p.positionDrift, 0.0f, 1.0f));
    const double vibInc = fluteVibHz_ / sr_;
    // The noise the air carries: a band around 3 kHz (a Chamberlin band pass, well below its limit).
    const float nf = 2.0f * std::sin(static_cast<float>(kPi * std::min(3000.0, 0.15 * sr_) / sr_));
    const float hpCoef = 1.0f - static_cast<float>(kTwoPi * 20.0 / sr_);
    for (int i = 0; i < n; ++i) {
        fluteBreath_ += (pressure - fluteBreath_) * rise;
        flutePhase_ += vibInc; if (flutePhase_ >= 1.0) flutePhase_ -= 1.0;
        const double vib = 1.0 + vibDepth * static_cast<double>(sin01(flutePhase_));
        // The bore's line plus the end filter's own delay is one period: what is written at the
        // embouchure at time n is read back, filtered, exactly a period later. The jet line holds
        // the FILTERED wave, so the filter's delay is already inside what it reads, and its line
        // is the jet's share of the period and nothing less. Measured: a spare sample in the bore
        // line made C6 +54 cents; the filter's delay subtracted a second time from the jet made it
        // +18; with both right the register is within a few cents.
        const double lpD   = onePoleDelay(lpCoef);
        const double dBore = std::max(2.0, period / vib - lpD);
        const double dJet  = std::max(1.0, period / vib * jetRatio - jetLpDelay);
        const int bI = static_cast<int>(dBore), jI = static_cast<int>(dJet);
        const float bF = static_cast<float>(dBore - bI), jF = static_cast<float>(dJet - jI);
        // What returns from the far end of the pipe, filtered and inverted at the open end.
        const float b0 = bowNut_[(bowW_ - bI + kBowMax) & (kBowMax - 1)];
        const float b1 = bowNut_[(bowW_ - bI - 1 + kBowMax) & (kBowMax - 1)];
        fluteRefl_ += lpCoef * ((b0 + bF * (b1 - b0)) - fluteRefl_);
        const float v = fluteRefl_ * loopGain;
        // The air's noise, coloured, part of it into the jet.
        const float w = rng_.bipolar();
        fluteNoiseBp2_ += nf * fluteNoiseBp1_;
        const float hp = w - fluteNoiseBp2_ - 0.5f * fluteNoiseBp1_;
        fluteNoiseBp1_ += nf * hp;
        const float turb = fluteNoiseBp1_ * (0.02f + 0.12f * air) * fluteBreath_;
        // The jet: deflected by the velocity at the hole, delayed by its travel, then switched
        // by the cubic, and working against what deflected it.
        const float j0 = bowBridge_[(bowW_ - jI + kBowMax) & (kBowMax - 1)];
        const float j1 = bowBridge_[(bowW_ - jI - 1 + kBowMax) & (kBowMax - 1)];
        // The jet: a thin thing fully deflected by a small velocity (the gain of three into the
        // cubic) whose saturated push is small against the wave in the pipe (a tenth) -- so the
        // loop's small-signal gain through it is well above the pipe's losses and the note
        // speaks, and its saturated gain is what the losses balance, at an amplitude of about a
        // half. Simulated before it was built: within two cents across the register, no reset.
        fluteJetLp_ += gj * ((j0 + jF * (j1 - j0)) - fluteJetLp_);
        float x = 3.0f * fluteJetLp_ + turb;
        if (x > 1.0f) x = 1.0f; else if (x < -1.0f) x = -1.0f;
        const float jet = -0.1f * (x - x * x * x * (1.0f / 3.0f)) * fluteBreath_;
        float in = v + jet;
        if (!(in > -4.0f && in < 4.0f)) {   // guarded, not trusted: a pipe that runs away is reset
            std::fill(bowNut_.begin(), bowNut_.end(), 0.0f);
            std::fill(bowBridge_.begin(), bowBridge_.end(), 0.0f);
            fluteRefl_ = 0.0f; in = 0.0f;
        }
        bowNut_[bowW_ & (kBowMax - 1)] = in;
        bowBridge_[bowW_ & (kBowMax - 1)] = v;
        ++bowW_;
        // What leaves the pipe: the wave at the open end, with the breath's own noise beside it,
        // through a DC blocker (the cubic is odd, but the breath's rise is not).
        const float raw = in * 0.55f + fluteNoiseBp1_ * 0.05f * air * fluteBreath_;
        const float y = raw - fluteHpX_ + hpCoef * fluteHpY_;
        fluteHpX_ = raw; fluteHpY_ = y;
        out[i] += y;
    }
}

// ---------------------------------------------------------------- Bowl and Ice

/**
 * @brief A modal body under a stick.
 *
 * Each mode is a two-pole resonator on the mode's velocity, driven
 * by the friction between the stick and the sum of the modes at the contact point -- the bow's
 * curve (friction() above), with the bow's Force as the pressing and its Speed as the rubbing.
 * A slow stick with a heavy hand sticks and slips once per period of the lowest mode, which is
 * the Helmholtz motion of a rubbed rim, and because the modes' decay is long the tone takes
 * seconds to build, as a bowl does. Every mode is two resonators a hair apart, split by Pos
 * Drift: the doublet an asymmetric bowl always has, and its beating -- a real bowl warbles,
 * and so does this one. Position is where the stick sits, from the rim (every mode) to the
 * belly (the lowest alone). Below a twentieth of Speed the stick is lifted and the body only
 * rings, so a note that lets go of the stick decays on its own physics.
 *
 * Ice is the same body with low, dense, short modes and a slip clock in front of the friction:
 * under a slow load the stick does not glide, it creeps -- holds, gives, holds -- and every
 * give is a pulse of stick velocity into the modes. The rate of the creeping follows Speed.
 */
void SourceSlot::renderRub(float* out, int n, double hz, const SlotParams& p, float dt, bool ice)
{
    (void)dt;
    const RubBody& body = ice ? kIceBody : kBowlBody;
    const double f0 = clampv(hz, 20.0, 0.2 * sr_);
    if (!rubReady_) {
        std::memset(rubY1_, 0, sizeof(rubY1_)); std::memset(rubY2_, 0, sizeof(rubY2_));
        rubSlipLeft_ = 0.0; rubSlipOn_ = 0.0; rubF1_ = rubF2_ = 0.0f;
        rubContact_ = static_cast<double>(rng_.uniform());
        rubReady_ = true;
    }
    const float force = clampv(p.bowForce, 0.0f, 1.0f);
    const float speed = clampv(p.bowSpeed, 0.0f, 1.0f);
    const float slope = 5.0f - 4.0f * force;
    const bool lifted = speed < 0.05f;
    const float vStick = 0.6f * speed;
    const float split = 0.0008f + 0.012f * clampv(p.positionDrift, 0.0f, 1.0f);   // the doublet's width
    const float bright = clampv(p.bright, 0.0f, 1.0f);
    const float contact = clampv(p.position, 0.0f, 1.0f);
    // Per mode: the two poles' angle and radius, and the mode's weight at the contact point.
    float a1[kRubModes][2], a2[kRubModes][2], w[kRubModes], drive[kRubModes];
    int used = 0;
    for (int m = 0; m < kRubModes; ++m) {
        const double fm = f0 * static_cast<double>(body.ratio[m]);
        if (fm >= 0.45 * sr_) break;
        // How long this mode rings: the body's own T60, scaled by Bright for the upper modes.
        const float t60 = body.t60 * (m == 0 ? 1.0f : body.ring[m] * (0.3f + 1.4f * bright));
        const float r = std::exp(-6.908f / (t60 * static_cast<float>(sr_)));   // -60 dB after t60
        for (int d = 0; d < 2; ++d) {
            const double fd = fm * (1.0 + (d == 0 ? -0.5 : 0.5) * static_cast<double>(split) * (m + 1));
            const float th = static_cast<float>(kTwoPi * std::min(fd, 0.45 * sr_) / sr_);
            a1[m][d] = 2.0f * r * std::cos(th);
            a2[m][d] = -r * r;
        }
        // The stick at the rim touches every mode, at the belly mostly the lowest.
        w[m] = body.weight[m] * (m == 0 ? 1.0f : (1.0f - 0.85f * contact));
        // The mode is a VELOCITY resonator: two poles and a zero at zero frequency (the numerator
        // 1 - z^-2), so that at its own frequency the velocity is in phase with the force, as a
        // mass on a spring has it at resonance. The plain two-pole was tried first and measured
        // as silence: its response at resonance stands a quarter turn behind the force, so the
        // friction's negative slope only detuned the mode instead of feeding it. With the phase
        // right, the slope (about -0.2 at the stick's speed) times the mode's gain -- 1/(1-r),
        // scaled here to sixty -- is a loop that grows, and the friction's own saturation is what
        // stops it, at the amplitude where the body's swing reaches the stick's speed. The ice,
        // with its short modes, needs a tenth of the gain.
        drive[m] = (1.0f - r) * 60.0f;
        used = m + 1;
    }
    // The stick travels round the rim as it rubs -- a turn every few seconds at the rubbing
    // speed -- and the two halves of every doublet stand at right angles on the rim, so the
    // stick drives first the one and then the other. Driven together and held still, the two
    // lock to one frequency under the friction's nonlinearity and the beating is gone
    // (measured: an envelope flat to the third decimal); driven in turn they keep their own
    // pitches, and the bowl warbles the way a rubbed bowl does.
    const double turn = (0.04 + 0.15 * static_cast<double>(speed)) / sr_;   // revolutions per sample
    // The creep: pulses of stick velocity, 4 to 40 a second with Speed, each ten to forty
    // milliseconds -- long enough for the lowest mode to swing a few times in each.
    const double slipRate = 4.0 + 36.0 * static_cast<double>(speed);
    for (int i = 0; i < n; ++i) {
        if (!lifted) { rubContact_ += turn; if (rubContact_ >= 1.0) rubContact_ -= 1.0; }
        // The doublet's two halves under the stick: |cos| and |sin| of the contact angle, so the
        // two weights' squares always sum to one and the drive's power does not dip between them
        // (with 1 +- cos it fell to a half at forty-five degrees, an eighteen-decibel wah).
        const float wa = std::fabs(sin01(rubContact_ + 0.25)), wb = std::fabs(sin01(rubContact_));
        float vBody = 0.0f;
        for (int m = 0; m < used; ++m) vBody += w[m] * (wa * rubY1_[m][0] + wb * rubY1_[m][1]);
        float f = 0.0f;
        if (!lifted) {
            float vs = vStick;
            if (ice) {
                rubSlipLeft_ -= 1.0;
                if (rubSlipLeft_ <= 0.0) {
                    rubSlipLeft_ = -std::log(1.0 - static_cast<double>(rng_.uniform()) + 1e-9) * sr_ / slipRate;
                    rubSlipOn_ = (0.01 + 0.03 * static_cast<double>(rng_.uniform())) * sr_;
                }
                if (rubSlipOn_ > 0.0) { rubSlipOn_ -= 1.0; vs = vStick * 3.0f; } else vs = vStick * 0.15f;
            }
            f = friction(vs - vBody, slope) * (0.3f + 0.7f * force);
        }
        const float fd = f - rubF2_;          // the numerator's 1 - z^-2, shared by every mode
        rubF2_ = rubF1_; rubF1_ = f;
        float y = 0.0f;
        for (int m = 0; m < used; ++m) {
            const float in = fd * w[m] * drive[m];
            for (int d = 0; d < 2; ++d) {
                const float v = in * (d == 0 ? wa : wb) + a1[m][d] * rubY1_[m][d] + a2[m][d] * rubY2_[m][d];
                rubY2_[m][d] = rubY1_[m][d]; rubY1_[m][d] = v;
                y += v * body.weight[m];
            }
        }
        if (!(y > -50.0f && y < 50.0f)) {   // a body that runs away is a body reset
            std::memset(rubY1_, 0, sizeof(rubY1_)); std::memset(rubY2_, 0, sizeof(rubY2_));
            y = 0.0f;
        }
        // Calibrated at a middling Force and Speed against the table (the amplitude of a rubbed
        // body follows both knobs over twenty decibels, measured), and softly held from above,
        // so the heavy-and-fast corner is loud rather than a wall.
        const float o = y * (ice ? 12.0f : 5.0f);
        out[i] += o / (1.0f + 1.2f * std::fabs(o));
    }
}

// ---------------------------------------------------------------- Murmur

/**
 * @brief Speech without words.
 *
 * The source is a glottal pulse train at the slot's pitch -- an impulse
 * per period through two one-poles, which is the -12 dB per octave of a glottal flow -- with a
 * jitter per period so it is a voice and not an oscillator, and a pitch that moves the way
 * speech moves: it falls over a phrase (declination) and rises and falls a little on every
 * syllable. Three formant filters in parallel walk between the vowels of the table above at a
 * syllable's pace (Speed: three to six a second), some syllables are fricatives (noise through
 * the formants and above them), some begin with a stop (a gap, then a burst), and the syllables
 * come in phrases of a few seconds with pauses between -- so it is heard as someone talking, and
 * never as what they say.
 *
 * Position is the medium. At 0 the voice is in the room, close; towards 1 it goes through a
 * radio: a band from 300 to 3000 Hz, a saturation, the hiss of the carrier under every
 * transmission, the burst of squelch noise that closes the channel after each phrase, and from
 * 0.7 up the Quindar tones -- 2525 Hz to key the transmitter, 2475 Hz to release it, a quarter of
 * a second each, which is the sound every Apollo air-to-ground loop opened and closed with.
 * Force is effort: level, openness (the first formant rises), and the tilt of the pulse.
 */
void SourceSlot::renderMurmur(float* out, int n, double hz, const SlotParams& p, float dt)
{
    const float sr = static_cast<float>(sr_);
    if (!murReady_) {
        murPhase_ = 0.0; murSylLeft_ = 0.0; murPauseLeft_ = 0.3 * sr_; murInPhrase_ = false; murSyllables_ = 0;
        murBeepLeft_ = 0.0; murSquelchLeft_ = 0.0; murBeepPhase_ = 0.0;
        for (int k = 0; k < 3; ++k) { murF_[k] = murTo_[k] = kVowels[5][k]; murForm_[k].reset(); }
        murGlot1_ = murGlot2_ = 0.0f; murPitchSt_ = murPitchTo_ = 0.0f; murDecl_ = 0.0f;
        murVoice_ = murVoiceTo_ = 1.0f; murGap_ = murGapTo_ = 1.0f; murJitter_ = 1.0f;
        murHpX_ = murHpY_ = 0.0f; murHiss_ = 0.0f;
        murRadioHp_.reset(); murRadioLp_.reset(); murFric_.reset();
        murRadioHp_.setQ(300.0f, 0.7f, sr);
        murRadioLp_.setQ(std::min(3000.0f, 0.4f * sr), 0.7f, sr);
        murFric_.setQ(std::min(4500.0f, 0.4f * sr), 1.2f, sr);
        murReady_ = true;
    }
    const float effort = clampv(p.bowForce, 0.0f, 1.0f);
    const float rate   = 2.5f + 4.0f * clampv(p.bowSpeed, 0.0f, 1.0f);     // syllables a second
    const float medium = clampv(p.position, 0.0f, 1.0f);
    const float range  = 1.0f + 4.0f * clampv(p.positionDrift, 0.0f, 1.0f); // semitones of intonation
    const float clarity = clampv(p.bright, 0.0f, 1.0f);
    // The syllable and phrase machine, stepped once per block: it moves at a few hertz.
    const double block = static_cast<double>(n);
    if (murInPhrase_) {
        murSylLeft_ -= block;
        if (murSylLeft_ <= 0.0) {
            if (murSyllables_ <= 0) {
                // The phrase ends: a pause, and the radio closes its channel.
                murInPhrase_ = false;
                murPauseLeft_ = (0.5 + 2.2 * static_cast<double>(rng_.uniform())) * sr_ * (4.0 / rate);
                murVoiceTo_ = 0.0f; murGapTo_ = 0.0f;
                if (medium > 0.15f) murSquelchLeft_ = (0.04 + 0.05 * static_cast<double>(rng_.uniform())) * sr_;
                if (medium > 0.7f) { murBeepLeft_ = 0.25 * sr_; murBeepHz_ = 2475.0f; murBeepPhase_ = 0.0; }
            } else {
                // The next syllable: a vowel (the schwa when unstressed), voiced or a fricative,
                // sometimes after a stop; its length, its accent.
                --murSyllables_;
                const bool stressed = rng_.uniform() < 0.35f;
                const int vowel = stressed ? rng_.below(5) : (rng_.uniform() < 0.6f ? 5 : rng_.below(5));
                for (int k = 0; k < 3; ++k) murTo_[k] = kVowels[vowel][k];
                murTo_[0] *= 1.0f + 0.35f * effort;                     // effort opens the mouth
                const bool fric = rng_.uniform() < 0.22f;
                murVoiceTo_ = fric ? 0.0f : (stressed ? 1.0f : 0.7f);
                murGapTo_ = 1.0f;
                if (rng_.uniform() < 0.18f) murGap_ = 0.0f;            // a stop: silence, then the burst
                murSylLeft_ = (0.6 + 0.8 * static_cast<double>(rng_.uniform())) * sr_ / rate;
                murPitchTo_ = (stressed ? 1.0f : -0.3f) * range * (0.5f + 0.5f * rng_.uniform()) - murDecl_;
                murDecl_ += 0.35f * range / 8.0f;                       // the phrase falls as it goes
                murJitter_ = 1.0f;
            }
        }
    } else {
        murPauseLeft_ -= block;
        if (murPauseLeft_ <= 0.0) {
            murInPhrase_ = true;
            murSyllables_ = 3 + rng_.below(9);
            murSylLeft_ = 0.0;
            murDecl_ = 0.0f;
            if (medium > 0.7f) { murBeepLeft_ = 0.25 * sr_; murBeepHz_ = 2525.0f; murBeepPhase_ = 0.0; }
        }
    }
    // Glides: formants in sixty milliseconds, voicing in twenty, pitch in eighty, the stop's gap in ten.
    const float cF = 1.0f - std::exp(-dt / 0.06f), cV = 1.0f - std::exp(-dt / 0.02f);
    const float cP = 1.0f - std::exp(-dt / 0.08f), cG = 1.0f - std::exp(-dt / 0.01f);
    for (int k = 0; k < 3; ++k) {
        murF_[k] += (murTo_[k] - murF_[k]) * cF;
        murForm_[k].setQ(clampv(murF_[k], 100.0f, 0.4f * sr), k == 0 ? 9.0f : 11.0f, sr);
    }
    murVoice_ += (murVoiceTo_ - murVoice_) * cV;
    murPitchSt_ += (murPitchTo_ - murPitchSt_) * cP;
    murGap_ += (murGapTo_ - murGap_) * cG;
    const double f0 = clampv(hz, 40.0, 1000.0) * std::pow(2.0, static_cast<double>(murPitchSt_) / 12.0);
    const double inc = f0 / sr_;
    // The glottal pulse's two poles: effort and Bright open the tilt (a brighter, harder voice).
    const float tilt = 1.0f - std::exp(-kTwoPi * (900.0f + 2500.0f * (0.5f * effort + 0.5f * clarity)) / sr);
    const float hpCoef = 1.0f - static_cast<float>(kTwoPi * 40.0 / sr_);
    const float level = 0.45f + 0.55f * effort;
    const float hissLevel = 0.012f * medium;
    const float drive = 1.0f + 3.0f * medium;
    const float bandMix = medium;                       // how much of the voice goes through the radio
    const float fricGain = 0.35f + 0.3f * clarity;
    const double beepInc = static_cast<double>(murBeepHz_) / sr_;
    for (int i = 0; i < n; ++i) {
        // The pulse train, jittered per period.
        murPhase_ += inc * static_cast<double>(murJitter_);
        float pulse = 0.0f;
        if (murPhase_ >= 1.0) { murPhase_ -= 1.0; pulse = 1.0f; murJitter_ = 1.0f + 0.012f * rng_.bipolar(); }
        murGlot1_ += tilt * (pulse - murGlot1_);
        murGlot2_ += tilt * (murGlot1_ - murGlot2_);
        const float w = rng_.bipolar();
        // Voiced: the pulse; unvoiced: noise, through the same formants and a band above them.
        const float src = (murGlot2_ * 18.0f * murVoice_ + w * 0.5f * (1.0f - murVoice_)) * murGap_;
        float lp, bp, hp, voice = 0.0f;
        for (int k = 0; k < 3; ++k) { murForm_[k].tick(src, lp, bp, hp); voice += bp * (k == 0 ? 1.0f : (k == 1 ? 0.7f : 0.35f)); }
        murFric_.tick(w, lp, bp, hp);
        voice += bp * fricGain * (1.0f - murVoice_) * murGap_ * (murInPhrase_ ? 1.0f : 0.0f);
        voice *= level;
        // The radio: the band, the saturation, the carrier's hiss while the channel is open,
        // the squelch when it closes, the beeps that key it.
        float radio = voice;
        if (bandMix > 0.0f) {
            murRadioHp_.tick(radio, lp, bp, hp); radio = hp;
            murRadioLp_.tick(radio, lp, bp, hp); radio = lp;
            radio = softClip(radio * drive) / drive * 1.6f;
            murHiss_ += 0.002f * ((murInPhrase_ ? 1.0f : 0.0f) - murHiss_);
            radio += w * hissLevel * murHiss_;
            if (murSquelchLeft_ > 0.0) { murSquelchLeft_ -= 1.0; radio += w * 0.08f * medium; }
            if (murBeepLeft_ > 0.0) {
                murBeepLeft_ -= 1.0;
                murBeepPhase_ += beepInc; if (murBeepPhase_ >= 1.0) murBeepPhase_ -= 1.0;
                radio += 0.06f * sin01(murBeepPhase_);
            }
        }
        const float mixed = voice + (radio - voice) * bandMix;
        const float y = mixed - murHpX_ + hpCoef * murHpY_;
        murHpX_ = mixed; murHpY_ = y;
        out[i] += y;
    }
    for (int k = 0; k < 3; ++k) if (!std::isfinite(murForm_[k].ic1) || !std::isfinite(murForm_[k].ic2)) murForm_[k].reset();
}

// ---------------------------------------------------------------- Drops

/**
 * @brief A drop of water falling into a vessel makes two sounds: the click of the impact, and the
 *        bubble the impact pulls under the surface, which rings like a bell whose pitch RISES as the
 *        bubble rises towards the surface.
 *
 * Van den Doel (2005) gives the bubble as a sine at f0 with a
 * damping d = 0.043 f0 + 0.0014 f0^1.5 and a frequency f(t) = f0 (1 + s d t): the "bloop" of
 * every drip. The bubble's size sets f0 -- a millimetre is three kilohertz, and a drop's bubbles
 * are one to seven -- and Bright is that size: small and high, or large and low. The click goes
 * into the vessel, two resonators whose pitch is Position (a cup at the top, a cistern at the
 * bottom), and the bubble is heard dry beside it. Drops fall at Density a second, on a Poisson
 * clock, so the pattern never repeats; with Pitch = Note the bubbles sit on the note's own
 * partials instead of on a random size, which is the wet marimba of a cave.
 */
void SourceSlot::renderDrops(float* out, int n, double hz, const SlotParams& p, float dt)
{
    (void)dt;
    const double density = clampv(static_cast<double>(p.density), 0.05, 60.0);
    const float bright = clampv(p.bright, 0.0f, 1.0f);
    const float wander = clampv(p.positionDrift, 0.0f, 1.0f);
    // The vessel: two modes, the second a little over twice the first, from Position. Each mode's
    // input is scaled by (1 - r), so its gain at its own frequency is the number written here
    // and not 1/(1 - r) -- unscaled, a two-millisecond click rang the vessel to full scale and
    // beyond (measured: peak 1.0, the clipper, and a reset every few drops).
    const double vessel = 150.0 * std::pow(10.0, static_cast<double>(clampv(p.position, 0.0f, 1.0f)));   // 150 .. 1500 Hz
    float va1[2], va2[2], vdrive[2];
    for (int m = 0; m < 2; ++m) {
        const double fm = std::min(vessel * (m == 0 ? 1.0 : 2.32), 0.4 * sr_);
        const float r = std::exp(-6.908f / ((m == 0 ? 0.9f : 0.5f) * static_cast<float>(sr_)));
        const float th = static_cast<float>(kTwoPi * fm / sr_);
        va1[m] = 2.0f * r * std::cos(th); va2[m] = -r * r;
        vdrive[m] = (1.0f - r) * 300.0f;
    }
    for (int i = 0; i < n; ++i) {
        dropNext_ -= 1.0;
        if (dropNext_ <= 0.0) {
            dropNext_ = -std::log(1.0 - static_cast<double>(rng_.uniform()) + 1e-9) * sr_ / density;
            for (auto& d : drops_) {
                if (d.on) continue;
                // The bubble: its size from Bright with a spread from Pos Drift, or on the note.
                double f0;
                if (p.follow) {
                    f0 = hz;
                    while (f0 < 400.0) f0 *= 2.0;
                    while (f0 > 3200.0) f0 *= 0.5;
                    f0 *= std::pow(2.0, 0.08 * static_cast<double>(wander * rng_.bipolar()));
                } else {
                    const double lo = 2600.0 - 2000.0 * static_cast<double>(bright) * 0.5;   // dull: large bubbles
                    const double centre = 500.0 + 2200.0 * static_cast<double>(bright);
                    f0 = centre * std::pow(2.0, (0.3 + 0.9 * static_cast<double>(wander)) * static_cast<double>(rng_.bipolar()));
                    f0 = clampv(f0, 300.0, std::max(lo, 3400.0));
                }
                f0 = std::min(f0, 0.3 * sr_);
                const double damp = 0.043 * f0 + 0.0014 * std::pow(f0, 1.5);   // van den Doel's damping, per second
                d.hz = f0;
                d.rise = 0.1 * damp;                                            // s d: the pitch's rise per second
                d.decay = std::exp(-static_cast<float>(damp / sr_));
                d.amp = (0.35f + 0.65f * rng_.uniform()) * 0.4f;
                d.phase = 0.0;
                d.left = static_cast<int>(std::min(8.0 / damp, 1.5) * sr_);
                d.on = true;
                dropClick_ = 0.5f + 0.5f * rng_.uniform();
                dropClickLeft_ = static_cast<int>(0.0015 * sr_);
                break;
            }
        }
        float y = 0.0f;
        for (auto& d : drops_) {
            if (!d.on) continue;
            const double t = 1.0 - static_cast<double>(d.left) / std::max(1.0, 1.5 * sr_);
            (void)t;
            d.phase += d.hz / sr_; if (d.phase >= 1.0) d.phase -= 1.0;
            d.hz = std::min(d.hz + d.rise * d.hz / sr_, 0.4 * sr_);
            y += d.amp * sin01(d.phase);
            d.amp *= d.decay;
            if (--d.left <= 0 || d.amp < 1.0e-4f) d.on = false;
        }
        // The click into the vessel.
        float click = 0.0f;
        if (dropClickLeft_ > 0) { --dropClickLeft_; click = dropClick_ * rng_.bipolar(); }
        for (int m = 0; m < 2; ++m) {
            const float v = click * vdrive[m] + va1[m] * dropVesselY1_[m] + va2[m] * dropVesselY2_[m];
            dropVesselY2_[m] = dropVesselY1_[m]; dropVesselY1_[m] = v;
            y += v * (m == 0 ? 0.5f : 0.25f);
        }
        if (!(y > -50.0f && y < 50.0f)) { dropVesselY1_[0] = dropVesselY1_[1] = dropVesselY2_[0] = dropVesselY2_[1] = 0.0f; y = 0.0f; }
        out[i] += y * 0.3f;
    }
}

// ---------------------------------------------------------------- Clip

/**
 * @brief The recording as it is.
 *
 * Every other way this instrument has of playing a clip takes it apart --
 * grains, a spectral stretch, a band model -- because a drone wants a texture and not a document.
 * The foreground wants the document: "Houston, we've had a problem" is a sentence, and a sentence
 * in grains is not one. So: the clip from Position, once, at its own speed (Pitch = Free, with
 * the slot's octave and ratio as a speed) or pitched to the note (Pitch = Note, against the pitch
 * its name carries), the last twenty milliseconds faded, and silence after -- unless the file's
 * name marks it seamless, in which case it wraps. A new note starts it again.
 */
void SourceSlot::renderClip(float* outL, int n, double hz, double speed, const SlotParams& p, const Texture* tex, float dt)
{
    (void)dt;
    std::memset(scratch_, 0, sizeof(float) * static_cast<size_t>(n));
    if (tex == nullptr || tex->empty() || clipDone_) return;
    const int len = static_cast<int>(tex->mono.size());
    const bool wide = tex->stereo();
    if (clipPos_ < 0.0) clipPos_ = static_cast<double>(clampv(p.position, 0.0f, 1.0f)) * static_cast<double>(len - 2);
    const double rate = (tex->sampleRate / sr_) * (p.follow ? hz / std::max(tex->baseHz, 1.0) : speed);
    const float angle = (clampv(p.pan, -1.0f, 1.0f) + 1.0f) * 0.25f * kPi;
    const float gain = tex->gain * clampv(p.level, 0.0f, 2.0f);
    const float gl = gain * std::cos(angle), gr = gain * std::sin(angle);
    const double fadeLen = 0.02 * sr_;
    const float* s = wide ? tex->lr.data() : tex->mono.data();
    for (int i = 0; i < n; ++i) {
        if (clipPos_ >= static_cast<double>(len - 2)) {
            if (tex->seamless) clipPos_ -= static_cast<double>(len - 2);
            else { clipDone_ = true; break; }
        }
        const int    i0 = static_cast<int>(clipPos_);
        const float  fr = static_cast<float>(clipPos_ - i0);
        float l, r;
        if (wide) {
            const float* a = s + 2 * i0;
            l = a[0] + fr * (a[2] - a[0]);
            r = a[1] + fr * (a[3] - a[1]);
        } else {
            l = r = s[i0] + fr * (s[i0 + 1] - s[i0]);
        }
        const double left = static_cast<double>(len - 2) - clipPos_;
        const float fade = left < fadeLen ? static_cast<float>(left / fadeLen) : 1.0f;
        outL[i] += l * gl * fade;
        scratch_[i] += r * gr * fade;
        clipPos_ += rate;
    }
}

// ================================================================ the signals (13.09.2026)

/**
 * @brief The second foreground round, Rene's list and a few more: not instruments but the sounds of
 *        things -- a whistler, a rattle, a bell, a Geiger tube, a fluorescent tube, a chaotic circuit, a
 *        beacon, a number station, a shortwave set.
 *
 * Each is a caricature small enough to run in a
 * voice, each calibrated to the same level as the others (about 0.1 RMS at Level 1), and each
 * starts its clocks again at every note. One set of memories serves all nine (sigT_ and the
 * rest in Sources.h): a slot is one type at a time.
 *
 * This namespace holds what the nine share: the resonator every one of them rings its clicks and
 * noise in, the sanity guard, and the Morse alphabet of the number station.
 */
namespace {

/**
 * @brief A two-pole resonance at f with a bandwidth of f/q, as the drops' vessel: the input is scaled by
 *        (1 - r) so the gain at resonance is what is written and not 1/(1 - r).
 *
 * The coefficients alone; the two samples of state live in the slot (sigY1_, sigY2_ and their
 * kin) and resoStep() runs one sample through them.
 */
struct Reso2 { float a1, a2, drive; };
/** @var float Reso2::a1
 *  @brief the feedback on the previous output, 2 r cos(theta)
 */
/** @var float Reso2::a2
 *  @brief the feedback on the output before that, -r^2
 */
/** @var float Reso2::drive
 *  @brief the input scale, (1 - r) times the written gain, so the peak at resonance is that gain
 */

/**
 * @brief Designs a Reso2: the pole radius from the bandwidth, the angle from the frequency.
 * @param hz    the resonance, clamped to 20 Hz .. 0.45 of the sample rate
 * @param q     the quality factor, bandwidth = hz / q (floored at 0.05)
 * @param sr    the sample rate in Hz
 * @param gain  the gain at resonance, applied through the input scale
 * @return      the three coefficients
 */
inline Reso2 reso2(double hz, double q, double sr, float gain)
{
    const double f = std::min(std::max(hz, 20.0), 0.45 * sr);
    const float r = std::exp(static_cast<float>(-3.14159265358979 * f / (std::max(q, 0.05) * sr)));
    Reso2 c;
    c.a1 = 2.0f * r * std::cos(static_cast<float>(kTwoPi * f / sr));
    c.a2 = -r * r;
    c.drive = (1.0f - r) * gain;
    return c;
}
/**
 * @brief One sample through a Reso2.
 * @param c   the coefficients
 * @param in  the input sample
 * @param y1  the previous output, updated to this one
 * @param y2  the output before that, updated to the previous one
 * @return    the resonator's output sample
 */
inline float resoStep(const Reso2& c, float in, float& y1, float& y2)
{
    const float v = in * c.drive + c.a1 * y1 + c.a2 * y2;
    y2 = y1; y1 = v;
    return v;
}
/**
 * @brief Every note is two seconds at most of state; a resonator that has run away is put back.
 * @param v  a resonator's state
 * @return   whether it is still within +-50, the bound past which the caller resets it
 */
inline bool sane(float v) { return v > -50.0f && v < 50.0f; }

/** @brief The Morse alphabet, figures first (a number station reads figures), then the letters. */
constexpr const char* kMorse[36] = {
    "-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----.",
    ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..", "--",
    "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--..",
};

} // namespace

// ---------------------------------------------------------------- Whistler

/**
 * @brief A lightning stroke's pulse travelling along a field line through the magnetosphere's plasma
 *        arrives dispersed: the higher frequencies first, the lower ones later, and what a VLF receiver
 *        hears is a whistle falling.
 *
 * Eckersley's law gives the delay as D / sqrt(f), so the frequency
 * falls as one over the square of time: f(t) = fEnd + (fStart - fEnd) / (1 + t / tau)^2. The note
 * is where the whistle ends, Bright how far above it begins (two to six octaves), Speed the tau
 * (0.2 to 1.7 s, the slow ones the long field lines), and under the tone a thread of noise in a
 * narrow band that follows it, the carrier's own fluctuation, at -32 dB.
 */
void SourceSlot::renderWhistler(float* out, int n, double hz, const SlotParams& p, float dt)
{
    (void)dt;
    if (!sigReady_) { sigT_ = 0.0; sigPhase_ = 0.0; sigY1_ = sigY2_ = 0.0f; sigReady_ = true; }
    const double fEnd = std::min(std::max(hz, 40.0), 0.2 * sr_);
    const double fStart = std::min(fEnd * std::pow(2.0, 2.0 + 4.0 * static_cast<double>(clampv(p.bright, 0.0f, 1.0f))), 0.4 * sr_);
    const double tau = 0.2 + 1.5 * static_cast<double>(clampv(p.bowSpeed, 0.0f, 1.0f));
    for (int i = 0; i < n; ++i) {
        sigT_ += 1.0 / sr_;
        const double u = 1.0 + sigT_ / tau;
        const double f = fEnd + (fStart - fEnd) / (u * u);
        sigPhase_ += f / sr_; if (sigPhase_ >= 1.0) sigPhase_ -= 1.0;
        const Reso2 c = reso2(f, 40.0, sr_, 6.0f);   // recomputed per sample: the tone moves every sample
        const float noise = resoStep(c, rng_.bipolar(), sigY1_, sigY2_);
        if (!sane(sigY1_)) { sigY1_ = sigY2_ = 0.0f; }
        out[i] += 0.28f * sin01(sigPhase_) + 0.28f * 0.025f * noise;
    }
}

// ---------------------------------------------------------------- Shaker

/**
 * @brief Cook's PhISEM (Physically Informed Stochastic Event Modeling, 1997): a shaker is a system
 *        energy that a shake tops up and that decays, and while it lasts, beans that hit the shell at
 *        random, each hit a grain of noise at the energy's level, the shell a resonance.
 *
 * Density is the
 * shakes a second while the note is held (the first at the note), Force how long the energy
 * lasts (a bean pod at 0 to a big gourd at 1), Position the shell's pitch (1.5 to 5 kHz, or the
 * note with Pitch = Note), Noise Q how much it rings.
 */
void SourceSlot::renderShaker(float* out, int n, double hz, const SlotParams& p, float dt)
{
    (void)dt;
    if (!sigReady_) { sigEnergy_ = 0.0f; sigNext_ = 0.0; sigY1_ = sigY2_ = 0.0f; sigReady_ = true; }
    const double shakes = clampv(static_cast<double>(p.density), 0.05, 20.0);
    const float tc = 0.03f + 0.25f * clampv(p.bowForce, 0.0f, 1.0f);
    const float decay = std::exp(-1.0f / (tc * static_cast<float>(sr_)));
    const double fc = p.follow ? std::min(std::max(hz, 200.0), 0.4 * sr_)
                               : 1500.0 * std::pow(10.0, 0.52 * static_cast<double>(clampv(p.position, 0.0f, 1.0f)));
    const Reso2 c = reso2(fc, 1.5 + 8.0 * static_cast<double>(clampv(p.noiseQ, 0.0f, 1.0f)), sr_, 1.0f);
    const float hitsPerSample = 900.0f / static_cast<float>(sr_);   // at an energy of 1
    for (int i = 0; i < n; ++i) {
        sigNext_ -= 1.0;
        if (sigNext_ <= 0.0) {
            sigEnergy_ = std::min(sigEnergy_ + 1.0f, 2.0f);
            sigNext_ = sr_ / shakes * (0.7 + 0.6 * static_cast<double>(rng_.uniform()));
        }
        sigEnergy_ *= decay;
        float in = 0.0f;
        if (sigEnergy_ > 1.0e-3f && rng_.uniform() < sigEnergy_ * hitsPerSample) in = rng_.bipolar() * sigEnergy_ * 8.0f;
        const float v = resoStep(c, in, sigY1_, sigY2_);
        if (!sane(sigY1_)) { sigY1_ = sigY2_ = 0.0f; }
        out[i] += 0.8f * v;
    }
}

// ---------------------------------------------------------------- Chime

/**
 * @brief Struck bronze: a ting-sha, a ship's bell, a church bell far off.
 *
 * A modal bank of five, and the
 * thing that makes cast bronze beat is that its modes come in doublets a hair apart (an
 * asymmetry of the casting): the prime's twin sits at 1 + split, and the two of them make the
 * beating a pair of cymbals has (3.5 Hz at 2.4 kHz). The hum an octave under the prime (Tilt is
 * how much of it: a church bell has it, a cymbal none), the tierce a minor third over, a high
 * partial at 2.5 to 3.5 times (Bright). Force is how long it rings (2 to 18 s for the prime, the
 * high one a fifth of that), Pos Drift the split. One hit of 0.8 ms at the note, all modes at once.
 */
void SourceSlot::renderChime(float* out, int n, double hz, const SlotParams& p, float dt)
{
    (void)dt;
    if (!sigReady_) { for (int m = 0; m < 6; ++m) chY1_[m] = chY2_[m] = 0.0f; sigCount_ = 1; sigState_ = static_cast<int>(0.0008 * sr_); sigReady_ = true; }
    const double split = 0.0004 + 0.004 * static_cast<double>(clampv(p.positionDrift, 0.0f, 1.0f));
    const double ratios[5] = { 0.5, 1.0, 1.0 + split, 1.2, 2.5 + 1.0 * static_cast<double>(clampv(p.bright, 0.0f, 1.0f)) };
    const float  amps[5]   = { 0.5f * clampv(p.tilt, 0.0f, 1.0f), 1.0f, 1.0f, 0.35f * clampv(p.bright, 0.0f, 1.0f), 0.25f };
    const float  T = 2.0f + 16.0f * clampv(p.bowForce, 0.0f, 1.0f);
    const float  t60[5] = { T * 1.2f, T, T * 0.95f, T * 0.5f, T * 0.2f };
    // One Dirac into every mode. A two-pole's answer to a unit impulse rings at 1 / sin(theta),
    // so a drive of amp * sin(theta) leaves every mode ringing at its written amplitude whatever
    // its decay -- the earlier (1 - r) scaling, right for a driven vessel, left an 18-second
    // bronze thirty decibels under the others. The 0.8 ms of noise beside it is the click of
    // the striker, heard dry.
    float a1[5], a2[5], drive[5];
    for (int m = 0; m < 5; ++m) {
        const double f = std::min(hz * ratios[m], 0.45 * sr_);
        const float r = std::exp(-6.908f / (t60[m] * static_cast<float>(sr_)));
        const float th = static_cast<float>(kTwoPi * f / sr_);
        a1[m] = 2.0f * r * std::cos(th);
        a2[m] = -r * r;
        drive[m] = 0.24f * amps[m] * std::sin(th);
    }
    for (int i = 0; i < n; ++i) {
        float hit = 0.0f, click = 0.0f;
        if (sigCount_ > 0) { --sigCount_; hit = 1.0f; }
        if (sigState_ > 0) { --sigState_; click = 0.1f * rng_.bipolar(); }
        float y = 0.0f;
        for (int m = 0; m < 5; ++m) {
            const float v = hit * drive[m] + a1[m] * chY1_[m] + a2[m] * chY2_[m];
            chY2_[m] = chY1_[m]; chY1_[m] = v;
            y += v;
        }
        if (!sane(y)) { for (int m = 0; m < 5; ++m) chY1_[m] = chY2_[m] = 0.0f; y = 0.0f; }
        out[i] += y + click;
    }
}

// ---------------------------------------------------------------- Geiger

/**
 * @brief A Geiger-Mueller tube: discharges on a Poisson clock at Density a second, and now and then a
 *        cluster, the rate leaping to 45 a second for 120 ms and falling back over 60 -- the burst a
 *        particle shower makes.
 *
 * Force is the chance of a cluster (per second, up to two thirds). Every
 * discharge is a Dirac step into the counter's piezo, a heavily damped resonance (Position: 800
 * to 4000 Hz, the classic 1850 near the middle; Noise Q: 0.7 to 2.2) that makes the click last
 * about two milliseconds and no longer.
 */
void SourceSlot::renderGeiger(float* out, int n, double hz, const SlotParams& p, float dt)
{
    (void)hz; (void)dt;
    if (!sigReady_) { sigNext_ = 0.0; sigBurst_ = 0.0; sigState_ = 0; sigY1_ = sigY2_ = 0.0f; sigReady_ = true; }
    const double base = clampv(static_cast<double>(p.density), 0.05, 20.0);
    const double clusterPerSec = 0.05 + 0.6 * static_cast<double>(clampv(p.bowForce, 0.0f, 1.0f));
    const double fc = 800.0 * std::pow(5.0, static_cast<double>(clampv(p.position, 0.0f, 1.0f)));
    const Reso2 c = reso2(fc, 0.7 + 1.5 * static_cast<double>(clampv(p.noiseQ, 0.0f, 1.0f)), sr_, 1.0f);
    const float burstDecay = std::exp(-1.0f / (0.06f * static_cast<float>(sr_)));
    for (int i = 0; i < n; ++i) {
        if (sigState_ > 0) --sigState_; else sigBurst_ *= burstDecay;
        if (rng_.uniform() < clusterPerSec / sr_) { sigBurst_ = 45.0; sigState_ = static_cast<int>(0.12 * sr_); }
        const double lambda = base + sigBurst_;
        sigNext_ -= 1.0;
        float in = 0.0f;
        if (sigNext_ <= 0.0) {
            in = (0.5f + 0.5f * rng_.uniform()) * (rng_.uniform() < 0.5f ? 1.0f : -1.0f) * 7.0f;
            sigNext_ = -std::log(1.0 - static_cast<double>(rng_.uniform()) + 1e-9) * sr_ / lambda;
        }
        const float v = resoStep(c, in, sigY1_, sigY2_);
        if (!sane(sigY1_)) { sigY1_ = sigY2_ = 0.0f; }
        out[i] += 0.7f * v;
    }
}

// ---------------------------------------------------------------- Tube

/**
 * @brief A fluorescent tube in a bunker corridor, in three stages: the bimetal starter's two or three
 *        clicks 150 to 350 ms apart (a Dirac through a low-pass at 380 Hz, the thunk of the switch); the
 *        choke's hum, the mains rectified to twice its frequency -- 100 Hz on 50, 120 on 60, Position
 *        chooses -- with harmonics falling as k^-1.6, coloured by the coil's resonance at 850 Hz; and
 *        the plasma's hiss, noise between 3.5 and 6.5 kHz chopped by the same half-waves (Bright is
 *        how much).
 *
 * The hum and the hiss come up over 400 ms after the last click, as the tube strikes.
 */
void SourceSlot::renderTube(float* out, int n, double hz, const SlotParams& p, float dt)
{
    (void)hz; (void)dt;
    if (!sigReady_) {
        sigT_ = 0.0; sigCount_ = 2 + (rng_.uniform() < 0.5f ? 1 : 0); sigNext_ = 0.02 * sr_;
        sigLp_ = 0.0f; sigEnergy_ = 0.0f; sigPhase_ = 0.0; sigY1_ = sigY2_ = sigZ1_ = sigZ2_ = 0.0f; sigReady_ = true;
    }
    const double mains = clampv(p.position, 0.0f, 1.0f) < 0.5f ? 100.0 : 120.0;
    const Reso2 coil = reso2(850.0, 1.2, sr_, 1.0f);
    const Reso2 plasma = reso2(5000.0, 1.7, sr_, 1.0f);
    const float hiss = 0.04f * (0.2f + 0.8f * clampv(p.bright, 0.0f, 1.0f));
    const float lpc = 1.0f - std::exp(static_cast<float>(-kTwoPi * 380.0 / sr_));
    const float rise = 1.0f - std::exp(-1.0f / (0.4f * static_cast<float>(sr_)));
    for (int i = 0; i < n; ++i) {
        // The starter.
        float click = 0.0f;
        if (sigCount_ > 0) {
            sigNext_ -= 1.0;
            if (sigNext_ <= 0.0) {
                click = 6.0f * (rng_.uniform() < 0.5f ? 1.0f : -1.0f);
                --sigCount_;
                sigNext_ = (0.15 + 0.2 * static_cast<double>(rng_.uniform())) * sr_;
            }
        } else {
            sigEnergy_ += (1.0f - sigEnergy_) * rise;   // the tube has struck: hum and hiss come up
        }
        sigLp_ += lpc * (click - sigLp_);
        // The choke.
        sigPhase_ += mains / sr_; if (sigPhase_ >= 1.0) sigPhase_ -= 1.0;
        float hum = 0.0f;
        for (int k = 1; k <= 5; ++k) {
            double ph = sigPhase_ * k; ph -= std::floor(ph);
            hum += std::pow(static_cast<float>(k), -1.6f) * sin01(ph);
        }
        const float coloured = resoStep(coil, hum, sigY1_, sigY2_);
        // The plasma.
        const float half = std::fabs(sin01(0.5 * sigPhase_));
        const float pl = resoStep(plasma, rng_.bipolar(), sigZ1_, sigZ2_) * half;
        if (!sane(sigY1_) || !sane(sigZ1_)) { sigY1_ = sigY2_ = sigZ1_ = sigZ2_ = 0.0f; }
        out[i] += 0.5f * sigLp_ + sigEnergy_ * (0.09f * hum + 0.06f * coloured + hiss * pl);
    }
}

// ---------------------------------------------------------------- Krell

/**
 * @brief The Krell's machines (Forbidden Planet, 1956; Louis and Bebe Barron's circuits): FM whose
 *        carrier and index are steered by a Roessler attractor, the one strange attractor with a single
 *        fold, so the pitch wanders through its band and never repeats and never quite loses the thread.
 *
 * dx = -y - z, dy = x + a y, dz = b + z (x - c) with a = b = 0.2, c = 5.7; x steers the carrier
 * over 2.6 octaves about the note (Position past the middle snaps it to semitones), y the index
 * (FM Index is its ceiling), Speed the attractor's pace. FM Ratio is the modulator's.
 */
void SourceSlot::renderKrell(float* out, int n, double hz, const SlotParams& p, float dt)
{
    (void)dt;
    if (!sigReady_) {
        krX_ = 0.1 + 4.0 * static_cast<double>(rng_.bipolar()); krY_ = 3.0 * static_cast<double>(rng_.bipolar()); krZ_ = 0.05;
        sigPhase_ = sigPhase2_ = 0.0; sigReady_ = true;
    }
    const double pace = (0.5 + 6.0 * static_cast<double>(clampv(p.bowSpeed, 0.0f, 1.0f))) / sr_;
    const bool quantise = clampv(p.position, 0.0f, 1.0f) > 0.5f;
    const double ratio = std::max(static_cast<double>(p.fmRatio), 0.1);
    const double indexMax = 6.0 * std::max(static_cast<double>(p.fmIndex), 0.0);
    for (int i = 0; i < n; ++i) {
        // Euler at a small step: the Roessler system is tame enough for it at this pace.
        const double dx = -krY_ - krZ_, dy = krX_ + 0.2 * krY_, dz = 0.2 + krZ_ * (krX_ - 5.7);
        krX_ += pace * dx; krY_ += pace * dy; krZ_ += pace * dz;
        if (!(krX_ > -50.0 && krX_ < 50.0 && krZ_ > -50.0 && krZ_ < 200.0)) { krX_ = 0.1; krY_ = 0.0; krZ_ = 0.05; }
        double oct = clampv(krX_ / 6.0, -1.3, 1.3) * 1.4;
        if (quantise) oct = std::round(oct * 12.0) / 12.0;
        const double fc = std::min(hz * std::pow(2.0, oct), 0.4 * sr_);
        const double index = clampv((krY_ + 6.0) / 12.0, 0.0, 1.0) * indexMax;
        sigPhase_ += fc / sr_; if (sigPhase_ >= 1.0) sigPhase_ -= 1.0;
        sigPhase2_ += fc * ratio / sr_; if (sigPhase2_ >= 1.0) sigPhase2_ -= 1.0;
        double ph = sigPhase_ + index * static_cast<double>(sin01(sigPhase2_)) / kTwoPi;
        ph -= std::floor(ph);
        out[i] += 0.28f * sin01(ph);
    }
}

// ---------------------------------------------------------------- Beacon

/**
 * @brief A deep-space beacon's packet: a preamble chirp falling from 2.17 to 1.5 times the note over
 *        35 ms, then eight bits of frequency-shift keying, 25 ms each, space at the note and mark a
 *        major third over it (1200 and 1500 Hz on a note of 1200), every bit windowed with 5 ms Tukey
 *        edges so the keying does not click.
 *
 * The bits are drawn anew for every packet; a packet every
 * 1/Density seconds while the note lasts, the first at once.
 */
void SourceSlot::renderBeacon(float* out, int n, double hz, const SlotParams& p, float dt)
{
    (void)dt;
    if (!sigReady_) { sigT_ = 0.0; sigNext_ = 0.0; sigPhase_ = 0.0; sigBits_ = 0u; sigState_ = 0; sigReady_ = true; }
    const double every = 1.0 / clampv(static_cast<double>(p.density), 0.05, 20.0);
    const double f0 = std::min(std::max(hz, 60.0), 0.2 * sr_);
    const double chirpLen = 0.035, bitLen = 0.025, edge = 0.005;
    const double packetLen = chirpLen + 8.0 * bitLen;
    for (int i = 0; i < n; ++i) {
        if (sigState_ == 0) {   // waiting for the next packet
            sigNext_ -= 1.0 / sr_;
            if (sigNext_ <= 0.0) { sigState_ = 1; sigT_ = 0.0; sigBits_ = static_cast<unsigned>(rng_.uniform() * 256.0f) & 255u; }
            else continue;
        }
        double f, env;
        if (sigT_ < chirpLen) {
            const double q = sigT_ / chirpLen;
            f = f0 * (2.17 - 0.67 * q);
            env = std::min(1.0, std::min(sigT_, chirpLen - sigT_) / edge);
        } else {
            const double tb = sigT_ - chirpLen;
            const int bit = std::min(7, static_cast<int>(tb / bitLen));
            const double inBit = tb - bit * bitLen;
            f = ((sigBits_ >> bit) & 1u) ? f0 * 1.25 : f0;
            const double e = std::min(inBit, bitLen - inBit) / edge;
            env = e >= 1.0 ? 1.0 : 0.5 - 0.5 * std::cos(3.14159265358979 * std::max(e, 0.0));
        }
        sigPhase_ += f / sr_; if (sigPhase_ >= 1.0) sigPhase_ -= 1.0;
        out[i] += 0.28f * static_cast<float>(env) * sin01(sigPhase_);
        sigT_ += 1.0 / sr_;
        if (sigT_ >= packetLen) { sigState_ = 0; sigNext_ = std::max(every - packetLen, 0.05); }
    }
}

// ---------------------------------------------------------------- Morse

/**
 * @brief A number station: five-figure groups (Position past the middle: letters) in Morse at Speed
 *        words a minute (8 to 30; a dit is 1.2 / wpm seconds, a dah three, the gaps one, three and seven
 *        dits), the tone at the note, keyed with 5 ms edges, and over it the ionosphere's flutter, a
 *        slow random tremolo (Bright is its depth) as the signal comes and goes over the horizon.
 */
void SourceSlot::renderMorse(float* out, int n, double hz, const SlotParams& p, float dt)
{
    (void)dt;
    if (!sigReady_) {
        sigPhase_ = 0.0; sigState_ = 1; sigNext_ = 0.0; sigPos_ = 0; sigCount_ = 0; sigBits_ = 0u;
        sigLp_ = 0.0f; sigFlutter_ = 0.5; sigFlutterHz_ = 0.3 + 2.5 * static_cast<double>(rng_.uniform()); sigPhase2_ = 0.0;
        sigReady_ = true;
    }
    const double wpm = 8.0 + 22.0 * static_cast<double>(clampv(p.bowSpeed, 0.0f, 1.0f));
    const double dit = 1.2 / wpm;
    const bool letters = clampv(p.position, 0.0f, 1.0f) > 0.5f;
    const float depth = clampv(p.bright, 0.0f, 1.0f);
    const float keyC = 1.0f - std::exp(-1.0f / (0.0025f * static_cast<float>(sr_)));
    const double f = std::min(std::max(hz, 60.0), 0.2 * sr_);
    // sigBits_ holds the current character's index into kMorse, sigPos_ the element within it,
    // sigCount_ the figure within the group; sigState_: 1 = a gap runs, 2 = an element sounds.
    for (int i = 0; i < n; ++i) {
        sigNext_ -= 1.0 / sr_;
        if (sigNext_ <= 0.0) {
            if (sigState_ == 2) {                 // an element ended: the gap after it
                sigState_ = 1;
                ++sigPos_;
                const char* code = kMorse[sigBits_ % 36u];
                if (code[sigPos_] == 0) {           // the character ended
                    sigPos_ = -1;
                    ++sigCount_;
                    sigNext_ = (sigCount_ % 5 == 0 ? 7.0 : 3.0) * dit;
                } else sigNext_ = dit;
            } else {                              // a gap ended: the next element
                if (sigPos_ < 0 || sigPos_ == 0) {  // a new character
                    if (sigPos_ < 0 || sigCount_ == 0) sigBits_ = letters ? 10u + static_cast<unsigned>(rng_.uniform() * 26.0f) % 26u
                                                                          : static_cast<unsigned>(rng_.uniform() * 10.0f) % 10u;
                    sigPos_ = 0;
                }
                const char* code = kMorse[sigBits_ % 36u];
                sigState_ = 2;
                sigNext_ = (code[sigPos_] == '-' ? 3.0 : 1.0) * dit;
            }
        }
        const float key = sigState_ == 2 ? 1.0f : 0.0f;
        sigLp_ += keyC * (key - sigLp_);
        sigPhase_ += f / sr_; if (sigPhase_ >= 1.0) sigPhase_ -= 1.0;
        // The flutter: a slow sine whose rate wanders, the depth Bright.
        sigPhase2_ += sigFlutterHz_ / sr_;
        if (sigPhase2_ >= 1.0) { sigPhase2_ -= 1.0; sigFlutterHz_ = 0.3 + 2.5 * static_cast<double>(rng_.uniform()); }
        const float flutter = 1.0f - depth * 0.5f * (1.0f + sin01(sigPhase2_));
        out[i] += 0.28f * sigLp_ * flutter * sin01(sigPhase_);
    }
}

// ---------------------------------------------------------------- Dial

/**
 * @brief A shortwave set with its dial turned: heterodyne whistles -- a carrier beating against the
 *        local oscillator -- that slide as the tuning moves, one to three of them (Position), each
 *        drifting to a new pitch every second or two between 300 Hz and 3 kHz with the note as the
 *        centre, and under them the band's own noise (Bright), through the same 350-3200 Hz window the
 *        Murmur's radio has, with a squelch burst now and then when a carrier drops.
 */
void SourceSlot::renderDial(float* out, int n, double hz, const SlotParams& p, float dt)
{
    (void)dt;
    const int count = 1 + static_cast<int>(clampv(p.position, 0.0f, 1.0f) * 2.999f);
    const double centre = std::min(std::max(hz, 200.0), 3000.0);
    if (!sigReady_) {
        for (int k = 0; k < 3; ++k) { dlF_[k] = centre * std::pow(2.0, 1.5 * static_cast<double>(rng_.bipolar())); dlTo_[k] = dlF_[k]; dlPh_[k] = 0.0; }
        sigNext_ = 0.3 * sr_; sigBurst_ = 0.0; sigY1_ = sigY2_ = sigZ1_ = sigZ2_ = 0.0f; sigReady_ = true;
    }
    const Reso2 band = reso2(1200.0, 0.6, sr_, 1.0f);
    const Reso2 squelch = reso2(2500.0, 0.8, sr_, 1.0f);
    const float noise = 0.02f + 0.06f * clampv(p.bright, 0.0f, 1.0f);
    const double glide = 1.0 - std::exp(-1.0 / (0.25 * sr_));
    const float burstDecay = std::exp(-1.0f / (0.08f * static_cast<float>(sr_)));
    for (int i = 0; i < n; ++i) {
        sigNext_ -= 1.0;
        if (sigNext_ <= 0.0) {   // the dial moves: one whistle gets a new target, now and then a carrier drops
            const int k = static_cast<int>(rng_.uniform() * 2.999f);
            dlTo_[k] = clampv(centre * std::pow(2.0, 1.8 * static_cast<double>(rng_.bipolar())), 300.0, 3000.0);
            if (rng_.uniform() < 0.3f) sigBurst_ = 1.0;
            sigNext_ = (0.4 + 1.4 * static_cast<double>(rng_.uniform())) * sr_;
        }
        float tones = 0.0f;
        for (int k = 0; k < count; ++k) {
            dlF_[k] += (dlTo_[k] - dlF_[k]) * glide;
            dlPh_[k] += dlF_[k] / sr_; if (dlPh_[k] >= 1.0) dlPh_[k] -= 1.0;
            tones += sin01(dlPh_[k]) * (k == 0 ? 1.0f : 0.5f);
        }
        const float bed = resoStep(band, rng_.bipolar(), sigY1_, sigY2_);
        const float sq = resoStep(squelch, rng_.bipolar(), sigZ1_, sigZ2_) * static_cast<float>(sigBurst_);
        sigBurst_ *= burstDecay;
        if (!sane(sigY1_) || !sane(sigZ1_)) { sigY1_ = sigY2_ = sigZ1_ = sigZ2_ = 0.0f; }
        out[i] += 0.2f * tones + 2.0f * noise * bed + 0.24f * sq;
    }
}

} // namespace ambient
