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
 * at every change of type, which is what the `Ready_` flags (m_fluteReady, m_rubReady, m_murReady, m_sigReady) are for.
 */
#include "ambient/Sources.h"
#include "ambient/Voice.h"     // ControlBlock
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
constexpr float Vowels[6][3] = {
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
    float m_ratio[SourceSlot::RubModes];    ///< each mode's frequency as a ratio to the lowest
    float m_weight[SourceSlot::RubModes];   ///< each mode's amplitude at the rim, the lowest at 1
    float m_ring[SourceSlot::RubModes];     ///< each mode's ring time as a fraction of t60
    float m_t60;                             ///< how long the lowest mode takes to fall 60 dB, in seconds
};
constexpr RubBody BowlBody = { { 1.0f, 2.71f, 4.98f, 7.78f, 11.0f, 14.6f }, { 1.0f, 0.55f, 0.32f, 0.18f, 0.10f, 0.06f }, { 1.0f, 0.7f, 0.5f, 0.35f, 0.25f, 0.18f }, 14.0f };   ///< the singing bowl: the (n^2 - 1) ladder, ringing for fourteen seconds
constexpr RubBody IceBody  = { { 1.0f, 1.58f, 2.24f, 3.02f, 3.98f, 5.1f },  { 1.0f, 0.8f, 0.6f, 0.45f, 0.3f, 0.2f },     { 1.0f, 0.8f, 0.6f, 0.45f, 0.35f, 0.25f }, 1.4f };   ///< ice and old wood: dense, inharmonic, dead within a second and a half

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
    const double period = m_sr / f0;
    if (period < 6.0 || period >= static_cast<double>(BowMax - 4)) { std::memset(out, 0, sizeof(float) * static_cast<size_t>(n)); return; }
    if (!m_fluteReady) {
        std::fill(m_bowNut.begin(), m_bowNut.end(), 0.0f);      // the bore
        std::fill(m_bowBridge.begin(), m_bowBridge.end(), 0.0f); // the jet's travel
        m_bowW = 0; m_fluteRefl = 0.0f; m_fluteBreath = 0.0f; m_fluteHpX = m_fluteHpY = 0.0f; m_fluteJetLp = 0.0f;
        m_fluteNoiseBp1 = m_fluteNoiseBp2 = 0.0f;
        m_flutePhase = m_rng.uniform();
        m_fluteVibHz = 4.4 + 1.4 * static_cast<double>(m_rng.uniform());   // a player's vibrato, one per note
        m_fluteReady = true;
    }
    const float force = clampv(p.m_bowForce, 0.0f, 1.0f);
    const float air   = clampv(p.m_bowSpeed, 0.0f, 1.0f);
    // The breath rises over a quarter of a second, and the air comes before the tone: below the
    // pressure at which the jet's gain beats the pipe's losses only the turbulence is heard, the
    // pipe filtering it into breath, and the note grows out of that once the breath is there.
    // Rich's flutes begin exactly so. (A one-pole per SAMPLE: the first version took the block's
    // seconds for a per-sample coefficient and was at full pressure in four milliseconds.)
    const float pressure = 0.22f + 0.78f * force;
    const float rise = 1.0f - std::exp(-1.0f / (0.1f * static_cast<float>(m_sr)));
    // Losses: Bright is the pipe's end filter, as it is the string's loop filter; and the pipe
    // loses seven per cent of its wave per round trip whatever Bright says -- a flute's Q is
    // about thirty. That loss is what the jet's saturation is balanced against: with a nearly
    // lossless pipe (0.985) and the same jet the amplitude had no limit at all, grew to the
    // guard, reset, and the pitch that was measured was the pitch of a loop restarting itself.
    const float lpCoef = 0.35f + 0.6f * clampv(p.m_bright, 0.0f, 1.0f);
    const float loopGain = 0.93f;
    // The jet's travel as a fraction of the period: half a period speaks the fundamental, a quarter
    // the octave. Position is the embouchure: the lower half of the knob the one, the upper half
    // the other. Not a slide between them -- a jet standing anywhere between pulls the pitch
    // (simulated: +5 cents at 0.475, +17 at 0.4), which is what a player's embouchure does and
    // what a source that is asked for a note must not.
    const double jetRatio = p.m_position < 0.5f ? 0.5 : 0.25;
    // The jet is a slow thing: its deflection cannot follow the pipe's upper modes, and without
    // that the loop at A3 chose its twelfth over its fundamental (both are in phase for a jet at
    // half a period). A one-pole at one and a half times the register's frequency on what the jet
    // reads; its own phase delay at that frequency is taken off the jet's line, so the register
    // stays exactly where it was.
    const double freg = 0.5 * f0 / jetRatio;
    const double fc = 1.5 * freg;
    const float  gj = 1.0f - std::exp(static_cast<float>(-TwoPi * fc / m_sr));
    // The one-pole's phase delay at the register's frequency, in samples, for the DIGITAL
    // one-pole and not its continuous cousin: the two differ by four tenths of a sample at C6,
    // which the loop answered with a constant two cents sharp across the whole register.
    const double wr = TwoPi * freg / m_sr;
    const double jetLpDelay = std::atan2((1.0 - gj) * std::sin(wr), 1.0 - (1.0 - gj) * std::cos(wr)) / wr;
    // Vibrato: Pos Drift is its depth, a third of a percent of pitch at 1 -- a breath, not a wobble.
    const double vibDepth = 0.0035 * static_cast<double>(clampv(p.m_positionDrift, 0.0f, 1.0f));
    const double vibInc = m_fluteVibHz / m_sr;
    // The noise the air carries: a band around 3 kHz (a Chamberlin band pass, well below its limit).
    const float nf = 2.0f * std::sin(static_cast<float>(Pi * std::min(3000.0, 0.15 * m_sr) / m_sr));
    const float hpCoef = 1.0f - static_cast<float>(TwoPi * 20.0 / m_sr);
    for (int i = 0; i < n; ++i) {
        m_fluteBreath += (pressure - m_fluteBreath) * rise;
        m_flutePhase += vibInc; if (m_flutePhase >= 1.0) m_flutePhase -= 1.0;
        const double vib = 1.0 + vibDepth * static_cast<double>(sin01(m_flutePhase));
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
        const float b0 = m_bowNut[(m_bowW - bI + BowMax) & (BowMax - 1)];
        const float b1 = m_bowNut[(m_bowW - bI - 1 + BowMax) & (BowMax - 1)];
        m_fluteRefl += lpCoef * ((b0 + bF * (b1 - b0)) - m_fluteRefl);
        const float v = m_fluteRefl * loopGain;
        // The air's noise, coloured, part of it into the jet.
        const float w = m_rng.bipolar();
        m_fluteNoiseBp2 += nf * m_fluteNoiseBp1;
        const float hp = w - m_fluteNoiseBp2 - 0.5f * m_fluteNoiseBp1;
        m_fluteNoiseBp1 += nf * hp;
        const float turb = m_fluteNoiseBp1 * (0.02f + 0.12f * air) * m_fluteBreath;
        // The jet: deflected by the velocity at the hole, delayed by its travel, then switched
        // by the cubic, and working against what deflected it.
        const float j0 = m_bowBridge[(m_bowW - jI + BowMax) & (BowMax - 1)];
        const float j1 = m_bowBridge[(m_bowW - jI - 1 + BowMax) & (BowMax - 1)];
        // The jet: a thin thing fully deflected by a small velocity (the gain of three into the
        // cubic) whose saturated push is small against the wave in the pipe (a tenth) -- so the
        // loop's small-signal gain through it is well above the pipe's losses and the note
        // speaks, and its saturated gain is what the losses balance, at an amplitude of about a
        // half. Simulated before it was built: within two cents across the register, no reset.
        m_fluteJetLp += gj * ((j0 + jF * (j1 - j0)) - m_fluteJetLp);
        float x = 3.0f * m_fluteJetLp + turb;
        if (x > 1.0f) x = 1.0f; else if (x < -1.0f) x = -1.0f;
        const float jet = -0.1f * (x - x * x * x * (1.0f / 3.0f)) * m_fluteBreath;
        float in = v + jet;
        if (!(in > -4.0f && in < 4.0f)) {   // guarded, not trusted: a pipe that runs away is reset
            std::fill(m_bowNut.begin(), m_bowNut.end(), 0.0f);
            std::fill(m_bowBridge.begin(), m_bowBridge.end(), 0.0f);
            m_fluteRefl = 0.0f; in = 0.0f;
        }
        m_bowNut[m_bowW & (BowMax - 1)] = in;
        m_bowBridge[m_bowW & (BowMax - 1)] = v;
        ++m_bowW;
        // What leaves the pipe: the wave at the open end, with the breath's own noise beside it,
        // through a DC blocker (the cubic is odd, but the breath's rise is not).
        const float raw = in * 0.55f + m_fluteNoiseBp1 * 0.05f * air * m_fluteBreath;
        const float y = raw - m_fluteHpX + hpCoef * m_fluteHpY;
        m_fluteHpX = raw; m_fluteHpY = y;
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
    const RubBody& body = ice ? IceBody : BowlBody;
    const double f0 = clampv(hz, 20.0, 0.2 * m_sr);
    if (!m_rubReady) {
        std::memset(m_rubY1, 0, sizeof(m_rubY1)); std::memset(m_rubY2, 0, sizeof(m_rubY2));
        m_rubSlipLeft = 0.0; m_rubSlipOn = 0.0; m_rubF1 = m_rubF2 = 0.0f;
        m_rubContact = static_cast<double>(m_rng.uniform());
        m_rubReady = true;
    }
    const float force = clampv(p.m_bowForce, 0.0f, 1.0f);
    const float speed = clampv(p.m_bowSpeed, 0.0f, 1.0f);
    const float slope = 5.0f - 4.0f * force;
    const bool lifted = speed < 0.05f;
    const float vStick = 0.6f * speed;
    const float split = 0.0008f + 0.012f * clampv(p.m_positionDrift, 0.0f, 1.0f);   // the doublet's width
    const float bright = clampv(p.m_bright, 0.0f, 1.0f);
    const float contact = clampv(p.m_position, 0.0f, 1.0f);
    // Per mode: the two poles' angle and radius, and the mode's weight at the contact point.
    float a1[RubModes][2], a2[RubModes][2], w[RubModes], drive[RubModes];
    int used = 0;
    for (int m = 0; m < RubModes; ++m) {
        const double fm = f0 * static_cast<double>(body.m_ratio[m]);
        if (fm >= 0.45 * m_sr) break;
        // How long this mode rings: the body's own T60, scaled by Bright for the upper modes.
        const float t60 = body.m_t60 * (m == 0 ? 1.0f : body.m_ring[m] * (0.3f + 1.4f * bright));
        const float r = std::exp(-6.908f / (t60 * static_cast<float>(m_sr)));   // -60 dB after t60
        for (int d = 0; d < 2; ++d) {
            const double fd = fm * (1.0 + (d == 0 ? -0.5 : 0.5) * static_cast<double>(split) * (m + 1));
            const float th = static_cast<float>(TwoPi * std::min(fd, 0.45 * m_sr) / m_sr);
            a1[m][d] = 2.0f * r * std::cos(th);
            a2[m][d] = -r * r;
        }
        // The stick at the rim touches every mode, at the belly mostly the lowest.
        w[m] = body.m_weight[m] * (m == 0 ? 1.0f : (1.0f - 0.85f * contact));
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
    const double turn = (0.04 + 0.15 * static_cast<double>(speed)) / m_sr;   // revolutions per sample
    // The creep: pulses of stick velocity, 4 to 40 a second with Speed, each ten to forty
    // milliseconds -- long enough for the lowest mode to swing a few times in each.
    const double slipRate = 4.0 + 36.0 * static_cast<double>(speed);
    for (int i = 0; i < n; ++i) {
        if (!lifted) { m_rubContact += turn; if (m_rubContact >= 1.0) m_rubContact -= 1.0; }
        // The doublet's two halves under the stick: |cos| and |sin| of the contact angle, so the
        // two weights' squares always sum to one and the drive's power does not dip between them
        // (with 1 +- cos it fell to a half at forty-five degrees, an eighteen-decibel wah).
        const float wa = std::fabs(sin01(m_rubContact + 0.25)), wb = std::fabs(sin01(m_rubContact));
        float vBody = 0.0f;
        for (int m = 0; m < used; ++m) vBody += w[m] * (wa * m_rubY1[m][0] + wb * m_rubY1[m][1]);
        float f = 0.0f;
        if (!lifted) {
            float vs = vStick;
            if (ice) {
                m_rubSlipLeft -= 1.0;
                if (m_rubSlipLeft <= 0.0) {
                    m_rubSlipLeft = -std::log(1.0 - static_cast<double>(m_rng.uniform()) + 1e-9) * m_sr / slipRate;
                    m_rubSlipOn = (0.01 + 0.03 * static_cast<double>(m_rng.uniform())) * m_sr;
                }
                if (m_rubSlipOn > 0.0) { m_rubSlipOn -= 1.0; vs = vStick * 3.0f; } else vs = vStick * 0.15f;
            }
            f = friction(vs - vBody, slope) * (0.3f + 0.7f * force);
        }
        const float fd = f - m_rubF2;          // the numerator's 1 - z^-2, shared by every mode
        m_rubF2 = m_rubF1; m_rubF1 = f;
        float y = 0.0f;
        for (int m = 0; m < used; ++m) {
            const float in = fd * w[m] * drive[m];
            for (int d = 0; d < 2; ++d) {
                const float v = in * (d == 0 ? wa : wb) + a1[m][d] * m_rubY1[m][d] + a2[m][d] * m_rubY2[m][d];
                m_rubY2[m][d] = m_rubY1[m][d]; m_rubY1[m][d] = v;
                y += v * body.m_weight[m];
            }
        }
        if (!(y > -50.0f && y < 50.0f)) {   // a body that runs away is a body reset
            std::memset(m_rubY1, 0, sizeof(m_rubY1)); std::memset(m_rubY2, 0, sizeof(m_rubY2));
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
    const float sr = static_cast<float>(m_sr);
    if (!m_murReady) {
        m_murPhase = 0.0; m_murSylLeft = 0.0; m_murPauseLeft = 0.3 * m_sr; m_murInPhrase = false; m_murSyllables = 0;
        m_murBeepLeft = 0.0; m_murSquelchLeft = 0.0; m_murBeepPhase = 0.0;
        for (int k = 0; k < 3; ++k) { m_murF[k] = m_murTo[k] = Vowels[5][k]; m_murForm[k].reset(); }
        m_murGlot1 = m_murGlot2 = 0.0f; m_murPitchSt = m_murPitchTo = 0.0f; m_murDecl = 0.0f;
        m_murVoice = m_murVoiceTo = 1.0f; m_murGap = m_murGapTo = 1.0f; m_murJitter = 1.0f;
        m_murHpX = m_murHpY = 0.0f; m_murHiss = 0.0f;
        m_murRadioHp.reset(); m_murRadioLp.reset(); m_murFric.reset();
        m_murRadioHp.setQ(300.0f, 0.7f, sr);
        m_murRadioLp.setQ(std::min(3000.0f, 0.4f * sr), 0.7f, sr);
        m_murFric.setQ(std::min(4500.0f, 0.4f * sr), 1.2f, sr);
        m_murReady = true;
    }
    const float effort = clampv(p.m_bowForce, 0.0f, 1.0f);
    const float rate   = 2.5f + 4.0f * clampv(p.m_bowSpeed, 0.0f, 1.0f);     // syllables a second
    const float medium = clampv(p.m_position, 0.0f, 1.0f);
    const float range  = 1.0f + 4.0f * clampv(p.m_positionDrift, 0.0f, 1.0f); // semitones of intonation
    const float clarity = clampv(p.m_bright, 0.0f, 1.0f);
    // The syllable and phrase machine, stepped once per block: it moves at a few hertz.
    const double block = static_cast<double>(n);
    if (m_murInPhrase) {
        m_murSylLeft -= block;
        if (m_murSylLeft <= 0.0) {
            if (m_murSyllables <= 0) {
                // The phrase ends: a pause, and the radio closes its channel.
                m_murInPhrase = false;
                m_murPauseLeft = (0.5 + 2.2 * static_cast<double>(m_rng.uniform())) * m_sr * (4.0 / rate);
                m_murVoiceTo = 0.0f; m_murGapTo = 0.0f;
                if (medium > 0.15f) m_murSquelchLeft = (0.04 + 0.05 * static_cast<double>(m_rng.uniform())) * m_sr;
                if (medium > 0.7f) { m_murBeepLeft = 0.25 * m_sr; m_murBeepHz = 2475.0f; m_murBeepPhase = 0.0; }
            } else {
                // The next syllable: a vowel (the schwa when unstressed), voiced or a fricative,
                // sometimes after a stop; its length, its accent.
                --m_murSyllables;
                const bool stressed = m_rng.uniform() < 0.35f;
                const int vowel = stressed ? m_rng.below(5) : (m_rng.uniform() < 0.6f ? 5 : m_rng.below(5));
                for (int k = 0; k < 3; ++k) m_murTo[k] = Vowels[vowel][k];
                m_murTo[0] *= 1.0f + 0.35f * effort;                     // effort opens the mouth
                const bool fric = m_rng.uniform() < 0.22f;
                m_murVoiceTo = fric ? 0.0f : (stressed ? 1.0f : 0.7f);
                m_murGapTo = 1.0f;
                if (m_rng.uniform() < 0.18f) m_murGap = 0.0f;            // a stop: silence, then the burst
                m_murSylLeft = (0.6 + 0.8 * static_cast<double>(m_rng.uniform())) * m_sr / rate;
                m_murPitchTo = (stressed ? 1.0f : -0.3f) * range * (0.5f + 0.5f * m_rng.uniform()) - m_murDecl;
                m_murDecl += 0.35f * range / 8.0f;                       // the phrase falls as it goes
                m_murJitter = 1.0f;
            }
        }
    } else {
        m_murPauseLeft -= block;
        if (m_murPauseLeft <= 0.0) {
            m_murInPhrase = true;
            m_murSyllables = 3 + m_rng.below(9);
            m_murSylLeft = 0.0;
            m_murDecl = 0.0f;
            if (medium > 0.7f) { m_murBeepLeft = 0.25 * m_sr; m_murBeepHz = 2525.0f; m_murBeepPhase = 0.0; }
        }
    }
    // Glides: formants in sixty milliseconds, voicing in twenty, pitch in eighty, the stop's gap in ten.
    const float cF = 1.0f - std::exp(-dt / 0.06f), cV = 1.0f - std::exp(-dt / 0.02f);
    const float cP = 1.0f - std::exp(-dt / 0.08f), cG = 1.0f - std::exp(-dt / 0.01f);
    for (int k = 0; k < 3; ++k) {
        m_murF[k] += (m_murTo[k] - m_murF[k]) * cF;
        m_murForm[k].setQ(clampv(m_murF[k], 100.0f, 0.4f * sr), k == 0 ? 9.0f : 11.0f, sr);
    }
    m_murVoice += (m_murVoiceTo - m_murVoice) * cV;
    m_murPitchSt += (m_murPitchTo - m_murPitchSt) * cP;
    m_murGap += (m_murGapTo - m_murGap) * cG;
    const double f0 = clampv(hz, 40.0, 1000.0) * std::pow(2.0, static_cast<double>(m_murPitchSt) / 12.0);
    const double inc = f0 / m_sr;
    // The glottal pulse's two poles: effort and Bright open the tilt (a brighter, harder voice).
    const float tilt = 1.0f - std::exp(-TwoPi * (900.0f + 2500.0f * (0.5f * effort + 0.5f * clarity)) / sr);
    const float hpCoef = 1.0f - static_cast<float>(TwoPi * 40.0 / m_sr);
    const float level = 0.45f + 0.55f * effort;
    const float hissLevel = 0.012f * medium;
    const float drive = 1.0f + 3.0f * medium;
    const float bandMix = medium;                       // how much of the voice goes through the radio
    const float fricGain = 0.35f + 0.3f * clarity;
    const double beepInc = static_cast<double>(m_murBeepHz) / m_sr;
    for (int i = 0; i < n; ++i) {
        // The pulse train, jittered per period.
        m_murPhase += inc * static_cast<double>(m_murJitter);
        float pulse = 0.0f;
        if (m_murPhase >= 1.0) { m_murPhase -= 1.0; pulse = 1.0f; m_murJitter = 1.0f + 0.012f * m_rng.bipolar(); }
        m_murGlot1 += tilt * (pulse - m_murGlot1);
        m_murGlot2 += tilt * (m_murGlot1 - m_murGlot2);
        const float w = m_rng.bipolar();
        // Voiced: the pulse; unvoiced: noise, through the same formants and a band above them.
        const float src = (m_murGlot2 * 18.0f * m_murVoice + w * 0.5f * (1.0f - m_murVoice)) * m_murGap;
        float lp, bp, hp, voice = 0.0f;
        for (int k = 0; k < 3; ++k) { m_murForm[k].tick(src, lp, bp, hp); voice += bp * (k == 0 ? 1.0f : (k == 1 ? 0.7f : 0.35f)); }
        m_murFric.tick(w, lp, bp, hp);
        voice += bp * fricGain * (1.0f - m_murVoice) * m_murGap * (m_murInPhrase ? 1.0f : 0.0f);
        voice *= level;
        // The radio: the band, the saturation, the carrier's hiss while the channel is open,
        // the squelch when it closes, the beeps that key it.
        float radio = voice;
        if (bandMix > 0.0f) {
            m_murRadioHp.tick(radio, lp, bp, hp); radio = hp;
            m_murRadioLp.tick(radio, lp, bp, hp); radio = lp;
            radio = softClip(radio * drive) / drive * 1.6f;
            m_murHiss += 0.002f * ((m_murInPhrase ? 1.0f : 0.0f) - m_murHiss);
            radio += w * hissLevel * m_murHiss;
            if (m_murSquelchLeft > 0.0) { m_murSquelchLeft -= 1.0; radio += w * 0.08f * medium; }
            if (m_murBeepLeft > 0.0) {
                m_murBeepLeft -= 1.0;
                m_murBeepPhase += beepInc; if (m_murBeepPhase >= 1.0) m_murBeepPhase -= 1.0;
                radio += 0.06f * sin01(m_murBeepPhase);
            }
        }
        const float mixed = voice + (radio - voice) * bandMix;
        const float y = mixed - m_murHpX + hpCoef * m_murHpY;
        m_murHpX = mixed; m_murHpY = y;
        out[i] += y;
    }
    for (int k = 0; k < 3; ++k) if (!std::isfinite(m_murForm[k].m_ic1) || !std::isfinite(m_murForm[k].m_ic2)) m_murForm[k].reset();
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
    const double density = clampv(static_cast<double>(p.m_density), 0.05, 60.0);
    const float bright = clampv(p.m_bright, 0.0f, 1.0f);
    const float wander = clampv(p.m_positionDrift, 0.0f, 1.0f);
    // The vessel: two modes, the second a little over twice the first, from Position. Each mode's
    // input is scaled by (1 - r), so its gain at its own frequency is the number written here
    // and not 1/(1 - r) -- unscaled, a two-millisecond click rang the vessel to full scale and
    // beyond (measured: peak 1.0, the clipper, and a reset every few drops).
    const double vessel = 150.0 * std::pow(10.0, static_cast<double>(clampv(p.m_position, 0.0f, 1.0f)));   // 150 .. 1500 Hz
    float va1[2], va2[2], vdrive[2];
    for (int m = 0; m < 2; ++m) {
        const double fm = std::min(vessel * (m == 0 ? 1.0 : 2.32), 0.4 * m_sr);
        const float r = std::exp(-6.908f / ((m == 0 ? 0.9f : 0.5f) * static_cast<float>(m_sr)));
        const float th = static_cast<float>(TwoPi * fm / m_sr);
        va1[m] = 2.0f * r * std::cos(th); va2[m] = -r * r;
        vdrive[m] = (1.0f - r) * 300.0f;
    }
    for (int i = 0; i < n; ++i) {
        m_dropNext -= 1.0;
        if (m_dropNext <= 0.0) {
            m_dropNext = -std::log(1.0 - static_cast<double>(m_rng.uniform()) + 1e-9) * m_sr / density;
            for (auto& d : m_drops) {
                if (d.m_on) continue;
                // The bubble: its size from Bright with a spread from Pos Drift, or on the note.
                double f0;
                if (p.m_follow) {
                    f0 = hz;
                    while (f0 < 400.0) f0 *= 2.0;
                    while (f0 > 3200.0) f0 *= 0.5;
                    f0 *= std::pow(2.0, 0.08 * static_cast<double>(wander * m_rng.bipolar()));
                } else {
                    const double lo = 2600.0 - 2000.0 * static_cast<double>(bright) * 0.5;   // dull: large bubbles
                    const double centre = 500.0 + 2200.0 * static_cast<double>(bright);
                    f0 = centre * std::pow(2.0, (0.3 + 0.9 * static_cast<double>(wander)) * static_cast<double>(m_rng.bipolar()));
                    f0 = clampv(f0, 300.0, std::max(lo, 3400.0));
                }
                f0 = std::min(f0, 0.3 * m_sr);
                const double damp = 0.043 * f0 + 0.0014 * std::pow(f0, 1.5);   // van den Doel's damping, per second
                d.m_hz = f0;
                d.m_rise = 0.1 * damp;                                            // s d: the pitch's rise per second
                d.m_decay = std::exp(-static_cast<float>(damp / m_sr));
                d.m_amp = (0.35f + 0.65f * m_rng.uniform()) * 0.4f;
                d.m_phase = 0.0;
                d.m_left = static_cast<int>(std::min(8.0 / damp, 1.5) * m_sr);
                d.m_on = true;
                m_dropClick = 0.5f + 0.5f * m_rng.uniform();
                m_dropClickLeft = static_cast<int>(0.0015 * m_sr);
                break;
            }
        }
        float y = 0.0f;
        for (auto& d : m_drops) {
            if (!d.m_on) continue;
            const double t = 1.0 - static_cast<double>(d.m_left) / std::max(1.0, 1.5 * m_sr);
            (void)t;
            d.m_phase += d.m_hz / m_sr; if (d.m_phase >= 1.0) d.m_phase -= 1.0;
            d.m_hz = std::min(d.m_hz + d.m_rise * d.m_hz / m_sr, 0.4 * m_sr);
            y += d.m_amp * sin01(d.m_phase);
            d.m_amp *= d.m_decay;
            if (--d.m_left <= 0 || d.m_amp < 1.0e-4f) d.m_on = false;
        }
        // The click into the vessel.
        float click = 0.0f;
        if (m_dropClickLeft > 0) { --m_dropClickLeft; click = m_dropClick * m_rng.bipolar(); }
        for (int m = 0; m < 2; ++m) {
            const float v = click * vdrive[m] + va1[m] * m_dropVesselY1[m] + va2[m] * m_dropVesselY2[m];
            m_dropVesselY2[m] = m_dropVesselY1[m]; m_dropVesselY1[m] = v;
            y += v * (m == 0 ? 0.5f : 0.25f);
        }
        if (!(y > -50.0f && y < 50.0f)) { m_dropVesselY1[0] = m_dropVesselY1[1] = m_dropVesselY2[0] = m_dropVesselY2[1] = 0.0f; y = 0.0f; }
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
    std::memset(m_scratch, 0, sizeof(float) * static_cast<size_t>(n));
    if (tex == nullptr || tex->empty() || m_clipDone) return;
    const int len = static_cast<int>(tex->m_mono.size());
    const bool wide = tex->stereo();
    if (m_clipPos < 0.0) m_clipPos = static_cast<double>(clampv(p.m_position, 0.0f, 1.0f)) * static_cast<double>(len - 2);
    const double rate = (tex->m_sampleRate / m_sr) * (p.m_follow ? hz / std::max(tex->m_baseHz, 1.0) : speed);
    const float angle = (clampv(p.m_pan, -1.0f, 1.0f) + 1.0f) * 0.25f * Pi;
    const float gain = tex->m_gain * clampv(p.m_level, 0.0f, 2.0f);
    const float gl = gain * std::cos(angle), gr = gain * std::sin(angle);
    const double fadeLen = 0.02 * m_sr;
    const float* s = wide ? tex->m_lr.data() : tex->m_mono.data();
    for (int i = 0; i < n; ++i) {
        if (m_clipPos >= static_cast<double>(len - 2)) {
            if (tex->m_seamless) m_clipPos -= static_cast<double>(len - 2);
            else { m_clipDone = true; break; }
        }
        const int    i0 = static_cast<int>(m_clipPos);
        const float  fr = static_cast<float>(m_clipPos - i0);
        float l, r;
        if (wide) {
            const float* a = s + 2 * i0;
            l = a[0] + fr * (a[2] - a[0]);
            r = a[1] + fr * (a[3] - a[1]);
        } else {
            l = r = s[i0] + fr * (s[i0 + 1] - s[i0]);
        }
        const double left = static_cast<double>(len - 2) - m_clipPos;
        const float fade = left < fadeLen ? static_cast<float>(left / fadeLen) : 1.0f;
        outL[i] += l * gl * fade;
        m_scratch[i] += r * gr * fade;
        m_clipPos += rate;
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
 * starts its clocks again at every note. One set of memories serves all nine (m_sigT and the
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
 * The coefficients alone; the two samples of state live in the slot (m_sigY1, m_sigY2 and their
 * kin) and resoStep() runs one sample through them.
 */
struct Reso2 { float m_a1, m_a2, m_drive; };
/** @var float Reso2::m_a1
 *  @brief the feedback on the previous output, 2 r cos(theta)
 */
/** @var float Reso2::m_a2
 *  @brief the feedback on the output before that, -r^2
 */
/** @var float Reso2::m_drive
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
    c.m_a1 = 2.0f * r * std::cos(static_cast<float>(TwoPi * f / sr));
    c.m_a2 = -r * r;
    c.m_drive = (1.0f - r) * gain;
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
    const float v = in * c.m_drive + c.m_a1 * y1 + c.m_a2 * y2;
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
constexpr const char* Morse[36] = {
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
    if (!m_sigReady) { m_sigT = 0.0; m_sigPhase = 0.0; m_sigY1 = m_sigY2 = 0.0f; m_sigReady = true; }
    const double fEnd = std::min(std::max(hz, 40.0), 0.2 * m_sr);
    const double fStart = std::min(fEnd * std::pow(2.0, 2.0 + 4.0 * static_cast<double>(clampv(p.m_bright, 0.0f, 1.0f))), 0.4 * m_sr);
    const double tau = 0.2 + 1.5 * static_cast<double>(clampv(p.m_bowSpeed, 0.0f, 1.0f));
    for (int i = 0; i < n; ++i) {
        m_sigT += 1.0 / m_sr;
        const double u = 1.0 + m_sigT / tau;
        const double f = fEnd + (fStart - fEnd) / (u * u);
        m_sigPhase += f / m_sr; if (m_sigPhase >= 1.0) m_sigPhase -= 1.0;
        const Reso2 c = reso2(f, 40.0, m_sr, 6.0f);   // recomputed per sample: the tone moves every sample
        const float noise = resoStep(c, m_rng.bipolar(), m_sigY1, m_sigY2);
        if (!sane(m_sigY1)) { m_sigY1 = m_sigY2 = 0.0f; }
        out[i] += 0.28f * sin01(m_sigPhase) + 0.28f * 0.025f * noise;
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
    if (!m_sigReady) { m_sigEnergy = 0.0f; m_sigNext = 0.0; m_sigY1 = m_sigY2 = 0.0f; m_sigReady = true; }
    const double shakes = clampv(static_cast<double>(p.m_density), 0.05, 20.0);
    const float tc = 0.03f + 0.25f * clampv(p.m_bowForce, 0.0f, 1.0f);
    const float decay = std::exp(-1.0f / (tc * static_cast<float>(m_sr)));
    const double fc = p.m_follow ? std::min(std::max(hz, 200.0), 0.4 * m_sr)
                               : 1500.0 * std::pow(10.0, 0.52 * static_cast<double>(clampv(p.m_position, 0.0f, 1.0f)));
    const Reso2 c = reso2(fc, 1.5 + 8.0 * static_cast<double>(clampv(p.m_noiseQ, 0.0f, 1.0f)), m_sr, 1.0f);
    const float hitsPerSample = 900.0f / static_cast<float>(m_sr);   // at an energy of 1
    for (int i = 0; i < n; ++i) {
        m_sigNext -= 1.0;
        if (m_sigNext <= 0.0) {
            m_sigEnergy = std::min(m_sigEnergy + 1.0f, 2.0f);
            m_sigNext = m_sr / shakes * (0.7 + 0.6 * static_cast<double>(m_rng.uniform()));
        }
        m_sigEnergy *= decay;
        float in = 0.0f;
        if (m_sigEnergy > 1.0e-3f && m_rng.uniform() < m_sigEnergy * hitsPerSample) in = m_rng.bipolar() * m_sigEnergy * 8.0f;
        const float v = resoStep(c, in, m_sigY1, m_sigY2);
        if (!sane(m_sigY1)) { m_sigY1 = m_sigY2 = 0.0f; }
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
    if (!m_sigReady) { for (int m = 0; m < 6; ++m) m_chY1[m] = m_chY2[m] = 0.0f; m_sigCount = 1; m_sigState = static_cast<int>(0.0008 * m_sr); m_sigReady = true; }
    const double split = 0.0004 + 0.004 * static_cast<double>(clampv(p.m_positionDrift, 0.0f, 1.0f));
    const double ratios[5] = { 0.5, 1.0, 1.0 + split, 1.2, 2.5 + 1.0 * static_cast<double>(clampv(p.m_bright, 0.0f, 1.0f)) };
    const float  amps[5]   = { 0.5f * clampv(p.m_tilt, 0.0f, 1.0f), 1.0f, 1.0f, 0.35f * clampv(p.m_bright, 0.0f, 1.0f), 0.25f };
    const float  T = 2.0f + 16.0f * clampv(p.m_bowForce, 0.0f, 1.0f);
    const float  t60[5] = { T * 1.2f, T, T * 0.95f, T * 0.5f, T * 0.2f };
    // One Dirac into every mode. A two-pole's answer to a unit impulse rings at 1 / sin(theta),
    // so a drive of amp * sin(theta) leaves every mode ringing at its written amplitude whatever
    // its decay -- the earlier (1 - r) scaling, right for a driven vessel, left an 18-second
    // bronze thirty decibels under the others. The 0.8 ms of noise beside it is the click of
    // the striker, heard dry.
    float a1[5], a2[5], drive[5];
    for (int m = 0; m < 5; ++m) {
        const double f = std::min(hz * ratios[m], 0.45 * m_sr);
        const float r = std::exp(-6.908f / (t60[m] * static_cast<float>(m_sr)));
        const float th = static_cast<float>(TwoPi * f / m_sr);
        a1[m] = 2.0f * r * std::cos(th);
        a2[m] = -r * r;
        drive[m] = 0.24f * amps[m] * std::sin(th);
    }
    for (int i = 0; i < n; ++i) {
        float hit = 0.0f, click = 0.0f;
        if (m_sigCount > 0) { --m_sigCount; hit = 1.0f; }
        if (m_sigState > 0) { --m_sigState; click = 0.1f * m_rng.bipolar(); }
        float y = 0.0f;
        for (int m = 0; m < 5; ++m) {
            const float v = hit * drive[m] + a1[m] * m_chY1[m] + a2[m] * m_chY2[m];
            m_chY2[m] = m_chY1[m]; m_chY1[m] = v;
            y += v;
        }
        if (!sane(y)) { for (int m = 0; m < 5; ++m) m_chY1[m] = m_chY2[m] = 0.0f; y = 0.0f; }
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
    if (!m_sigReady) { m_sigNext = 0.0; m_sigBurst = 0.0; m_sigState = 0; m_sigY1 = m_sigY2 = 0.0f; m_sigReady = true; }
    const double base = clampv(static_cast<double>(p.m_density), 0.05, 20.0);
    const double clusterPerSec = 0.05 + 0.6 * static_cast<double>(clampv(p.m_bowForce, 0.0f, 1.0f));
    const double fc = 800.0 * std::pow(5.0, static_cast<double>(clampv(p.m_position, 0.0f, 1.0f)));
    const Reso2 c = reso2(fc, 0.7 + 1.5 * static_cast<double>(clampv(p.m_noiseQ, 0.0f, 1.0f)), m_sr, 1.0f);
    const float burstDecay = std::exp(-1.0f / (0.06f * static_cast<float>(m_sr)));
    for (int i = 0; i < n; ++i) {
        if (m_sigState > 0) --m_sigState; else m_sigBurst *= burstDecay;
        if (m_rng.uniform() < clusterPerSec / m_sr) { m_sigBurst = 45.0; m_sigState = static_cast<int>(0.12 * m_sr); }
        const double lambda = base + m_sigBurst;
        m_sigNext -= 1.0;
        float in = 0.0f;
        if (m_sigNext <= 0.0) {
            in = (0.5f + 0.5f * m_rng.uniform()) * (m_rng.uniform() < 0.5f ? 1.0f : -1.0f) * 7.0f;
            m_sigNext = -std::log(1.0 - static_cast<double>(m_rng.uniform()) + 1e-9) * m_sr / lambda;
        }
        const float v = resoStep(c, in, m_sigY1, m_sigY2);
        if (!sane(m_sigY1)) { m_sigY1 = m_sigY2 = 0.0f; }
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
    if (!m_sigReady) {
        m_sigT = 0.0; m_sigCount = 2 + (m_rng.uniform() < 0.5f ? 1 : 0); m_sigNext = 0.02 * m_sr;
        m_sigLp = 0.0f; m_sigEnergy = 0.0f; m_sigPhase = 0.0; m_sigY1 = m_sigY2 = m_sigZ1 = m_sigZ2 = 0.0f; m_sigReady = true;
    }
    const double mains = clampv(p.m_position, 0.0f, 1.0f) < 0.5f ? 100.0 : 120.0;
    const Reso2 coil = reso2(850.0, 1.2, m_sr, 1.0f);
    const Reso2 plasma = reso2(5000.0, 1.7, m_sr, 1.0f);
    const float hiss = 0.04f * (0.2f + 0.8f * clampv(p.m_bright, 0.0f, 1.0f));
    const float lpc = 1.0f - std::exp(static_cast<float>(-TwoPi * 380.0 / m_sr));
    const float rise = 1.0f - std::exp(-1.0f / (0.4f * static_cast<float>(m_sr)));
    for (int i = 0; i < n; ++i) {
        // The starter.
        float click = 0.0f;
        if (m_sigCount > 0) {
            m_sigNext -= 1.0;
            if (m_sigNext <= 0.0) {
                click = 6.0f * (m_rng.uniform() < 0.5f ? 1.0f : -1.0f);
                --m_sigCount;
                m_sigNext = (0.15 + 0.2 * static_cast<double>(m_rng.uniform())) * m_sr;
            }
        } else {
            m_sigEnergy += (1.0f - m_sigEnergy) * rise;   // the tube has struck: hum and hiss come up
        }
        m_sigLp += lpc * (click - m_sigLp);
        // The choke.
        m_sigPhase += mains / m_sr; if (m_sigPhase >= 1.0) m_sigPhase -= 1.0;
        float hum = 0.0f;
        for (int k = 1; k <= 5; ++k) {
            double ph = m_sigPhase * k; ph -= std::floor(ph);
            hum += std::pow(static_cast<float>(k), -1.6f) * sin01(ph);
        }
        const float coloured = resoStep(coil, hum, m_sigY1, m_sigY2);
        // The plasma.
        const float half = std::fabs(sin01(0.5 * m_sigPhase));
        const float pl = resoStep(plasma, m_rng.bipolar(), m_sigZ1, m_sigZ2) * half;
        if (!sane(m_sigY1) || !sane(m_sigZ1)) { m_sigY1 = m_sigY2 = m_sigZ1 = m_sigZ2 = 0.0f; }
        out[i] += 0.5f * m_sigLp + m_sigEnergy * (0.09f * hum + 0.06f * coloured + hiss * pl);
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
    if (!m_sigReady) {
        m_krX = 0.1 + 4.0 * static_cast<double>(m_rng.bipolar()); m_krY = 3.0 * static_cast<double>(m_rng.bipolar()); m_krZ = 0.05;
        m_sigPhase = m_sigPhase2 = 0.0; m_sigReady = true;
    }
    const double pace = (0.5 + 6.0 * static_cast<double>(clampv(p.m_bowSpeed, 0.0f, 1.0f))) / m_sr;
    const bool quantise = clampv(p.m_position, 0.0f, 1.0f) > 0.5f;
    const double ratio = std::max(static_cast<double>(p.m_fmRatio), 0.1);
    const double indexMax = 6.0 * std::max(static_cast<double>(p.m_fmIndex), 0.0);
    for (int i = 0; i < n; ++i) {
        // Euler at a small step: the Roessler system is tame enough for it at this pace.
        const double dx = -m_krY - m_krZ, dy = m_krX + 0.2 * m_krY, dz = 0.2 + m_krZ * (m_krX - 5.7);
        m_krX += pace * dx; m_krY += pace * dy; m_krZ += pace * dz;
        if (!(m_krX > -50.0 && m_krX < 50.0 && m_krZ > -50.0 && m_krZ < 200.0)) { m_krX = 0.1; m_krY = 0.0; m_krZ = 0.05; }
        double oct = clampv(m_krX / 6.0, -1.3, 1.3) * 1.4;
        if (quantise) oct = std::round(oct * 12.0) / 12.0;
        const double fc = std::min(hz * std::pow(2.0, oct), 0.4 * m_sr);
        const double index = clampv((m_krY + 6.0) / 12.0, 0.0, 1.0) * indexMax;
        m_sigPhase += fc / m_sr; if (m_sigPhase >= 1.0) m_sigPhase -= 1.0;
        m_sigPhase2 += fc * ratio / m_sr; if (m_sigPhase2 >= 1.0) m_sigPhase2 -= 1.0;
        double ph = m_sigPhase + index * static_cast<double>(sin01(m_sigPhase2)) / TwoPi;
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
    if (!m_sigReady) { m_sigT = 0.0; m_sigNext = 0.0; m_sigPhase = 0.0; m_sigBits = 0u; m_sigState = 0; m_sigReady = true; }
    const double every = 1.0 / clampv(static_cast<double>(p.m_density), 0.05, 20.0);
    const double f0 = std::min(std::max(hz, 60.0), 0.2 * m_sr);
    const double chirpLen = 0.035, bitLen = 0.025, edge = 0.005;
    const double packetLen = chirpLen + 8.0 * bitLen;
    for (int i = 0; i < n; ++i) {
        if (m_sigState == 0) {   // waiting for the next packet
            m_sigNext -= 1.0 / m_sr;
            if (m_sigNext <= 0.0) { m_sigState = 1; m_sigT = 0.0; m_sigBits = static_cast<unsigned>(m_rng.uniform() * 256.0f) & 255u; }
            else continue;
        }
        double f, env;
        if (m_sigT < chirpLen) {
            const double q = m_sigT / chirpLen;
            f = f0 * (2.17 - 0.67 * q);
            env = std::min(1.0, std::min(m_sigT, chirpLen - m_sigT) / edge);
        } else {
            const double tb = m_sigT - chirpLen;
            const int bit = std::min(7, static_cast<int>(tb / bitLen));
            const double inBit = tb - bit * bitLen;
            f = ((m_sigBits >> bit) & 1u) ? f0 * 1.25 : f0;
            const double e = std::min(inBit, bitLen - inBit) / edge;
            env = e >= 1.0 ? 1.0 : 0.5 - 0.5 * std::cos(3.14159265358979 * std::max(e, 0.0));
        }
        m_sigPhase += f / m_sr; if (m_sigPhase >= 1.0) m_sigPhase -= 1.0;
        out[i] += 0.28f * static_cast<float>(env) * sin01(m_sigPhase);
        m_sigT += 1.0 / m_sr;
        if (m_sigT >= packetLen) { m_sigState = 0; m_sigNext = std::max(every - packetLen, 0.05); }
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
    if (!m_sigReady) {
        m_sigPhase = 0.0; m_sigState = 1; m_sigNext = 0.0; m_sigPos = 0; m_sigCount = 0; m_sigBits = 0u;
        m_sigLp = 0.0f; m_sigFlutter = 0.5; m_sigFlutterHz = 0.3 + 2.5 * static_cast<double>(m_rng.uniform()); m_sigPhase2 = 0.0;
        m_sigReady = true;
    }
    const double wpm = 8.0 + 22.0 * static_cast<double>(clampv(p.m_bowSpeed, 0.0f, 1.0f));
    const double dit = 1.2 / wpm;
    const bool letters = clampv(p.m_position, 0.0f, 1.0f) > 0.5f;
    const float depth = clampv(p.m_bright, 0.0f, 1.0f);
    const float keyC = 1.0f - std::exp(-1.0f / (0.0025f * static_cast<float>(m_sr)));
    const double f = std::min(std::max(hz, 60.0), 0.2 * m_sr);
    // m_sigBits holds the current character's index into Morse, m_sigPos the element within it,
    // m_sigCount the figure within the group; m_sigState: 1 = a gap runs, 2 = an element sounds.
    for (int i = 0; i < n; ++i) {
        m_sigNext -= 1.0 / m_sr;
        if (m_sigNext <= 0.0) {
            if (m_sigState == 2) {                 // an element ended: the gap after it
                m_sigState = 1;
                ++m_sigPos;
                const char* code = Morse[m_sigBits % 36u];
                if (code[m_sigPos] == 0) {           // the character ended
                    m_sigPos = -1;
                    ++m_sigCount;
                    m_sigNext = (m_sigCount % 5 == 0 ? 7.0 : 3.0) * dit;
                } else m_sigNext = dit;
            } else {                              // a gap ended: the next element
                if (m_sigPos < 0 || m_sigPos == 0) {  // a new character
                    if (m_sigPos < 0 || m_sigCount == 0) m_sigBits = letters ? 10u + static_cast<unsigned>(m_rng.uniform() * 26.0f) % 26u
                                                                          : static_cast<unsigned>(m_rng.uniform() * 10.0f) % 10u;
                    m_sigPos = 0;
                }
                const char* code = Morse[m_sigBits % 36u];
                m_sigState = 2;
                m_sigNext = (code[m_sigPos] == '-' ? 3.0 : 1.0) * dit;
            }
        }
        const float key = m_sigState == 2 ? 1.0f : 0.0f;
        m_sigLp += keyC * (key - m_sigLp);
        m_sigPhase += f / m_sr; if (m_sigPhase >= 1.0) m_sigPhase -= 1.0;
        // The flutter: a slow sine whose rate wanders, the depth Bright.
        m_sigPhase2 += m_sigFlutterHz / m_sr;
        if (m_sigPhase2 >= 1.0) { m_sigPhase2 -= 1.0; m_sigFlutterHz = 0.3 + 2.5 * static_cast<double>(m_rng.uniform()); }
        const float flutter = 1.0f - depth * 0.5f * (1.0f + sin01(m_sigPhase2));
        out[i] += 0.28f * m_sigLp * flutter * sin01(m_sigPhase);
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
    const int count = 1 + static_cast<int>(clampv(p.m_position, 0.0f, 1.0f) * 2.999f);
    const double centre = std::min(std::max(hz, 200.0), 3000.0);
    if (!m_sigReady) {
        for (int k = 0; k < 3; ++k) { m_dlF[k] = centre * std::pow(2.0, 1.5 * static_cast<double>(m_rng.bipolar())); m_dlTo[k] = m_dlF[k]; m_dlPh[k] = 0.0; }
        m_sigNext = 0.3 * m_sr; m_sigBurst = 0.0; m_sigY1 = m_sigY2 = m_sigZ1 = m_sigZ2 = 0.0f; m_sigReady = true;
    }
    const Reso2 band = reso2(1200.0, 0.6, m_sr, 1.0f);
    const Reso2 squelch = reso2(2500.0, 0.8, m_sr, 1.0f);
    const float noise = 0.02f + 0.06f * clampv(p.m_bright, 0.0f, 1.0f);
    const double glide = 1.0 - std::exp(-1.0 / (0.25 * m_sr));
    const float burstDecay = std::exp(-1.0f / (0.08f * static_cast<float>(m_sr)));
    for (int i = 0; i < n; ++i) {
        m_sigNext -= 1.0;
        if (m_sigNext <= 0.0) {   // the dial moves: one whistle gets a new target, now and then a carrier drops
            const int k = static_cast<int>(m_rng.uniform() * 2.999f);
            m_dlTo[k] = clampv(centre * std::pow(2.0, 1.8 * static_cast<double>(m_rng.bipolar())), 300.0, 3000.0);
            if (m_rng.uniform() < 0.3f) m_sigBurst = 1.0;
            m_sigNext = (0.4 + 1.4 * static_cast<double>(m_rng.uniform())) * m_sr;
        }
        float tones = 0.0f;
        for (int k = 0; k < count; ++k) {
            m_dlF[k] += (m_dlTo[k] - m_dlF[k]) * glide;
            m_dlPh[k] += m_dlF[k] / m_sr; if (m_dlPh[k] >= 1.0) m_dlPh[k] -= 1.0;
            tones += sin01(m_dlPh[k]) * (k == 0 ? 1.0f : 0.5f);
        }
        const float bed = resoStep(band, m_rng.bipolar(), m_sigY1, m_sigY2);
        const float sq = resoStep(squelch, m_rng.bipolar(), m_sigZ1, m_sigZ2) * static_cast<float>(m_sigBurst);
        m_sigBurst *= burstDecay;
        if (!sane(m_sigY1) || !sane(m_sigZ1)) { m_sigY1 = m_sigY2 = m_sigZ1 = m_sigZ2 = 0.0f; }
        out[i] += 0.2f * tones + 2.0f * noise * bed + 0.24f * sq;
    }
}

} // namespace ambient
