/**
 * @file Voice.cpp
 * @brief One voice of the instrument: its strand bank, its sources, its place in the room, and the
 *        two planes it renders into.
 *
 * A Voice (Voice.h) is one sounding note. Its own sound is a bank of up to MaxStrands strands,
 * each a phasor bank of MaxPartials partials (Simd.h) fed by the additive formula -- tilt,
 * brightness window, odd/even, inharmonic stretch, shimmer, presence -- and beside the bank stand
 * the Slots source slots (Sources.cpp), which render into the same filter, envelope and
 * placement. What this file adds to the sources is everything that makes the note a note in a
 * room: the envelope, the strike (a Karplus-Strong burst on the near plane), portamento with a
 * gravity towards the just ratios, the breathing distance and the depth law, Bloom and the slots'
 * own entrances, the slot roles, the filter and the z-plane, air and its Ghost mode, and the whole
 * of the binaural placement -- interaural time difference, the head shadow (a one-pole, or Brown
 * and Duda's spherical head), the near field's low shelf, the pinna notch and shoulder reflection
 * that externalise and give height, the phase field -- ending in the split between the near plane
 * and the far one that the Engine sends into its reverbs.
 *
 * Two entry points do the work. control() runs once per control block (ControlBlock samples) and
 * turns the parameters and the drifters into per-block targets: envelope times, the strands'
 * frequencies and partial amplitudes with their per-sample steps, filter coefficients, ITD
 * targets, shadow and shelf coefficients. render() then runs the samples: it calls control(),
 * renders the slots, steps the phasor banks, and takes each sample through the filters, the
 * wavefolder, the air, the delay and the head, adding the result into the four buffers it is
 * given. Both run on the audio thread; prepare() runs on the message thread once per sample rate
 * and seeds every random stream -- and the order of those seeds is load-bearing, because every
 * preset's random phases and drifts depend on it (the oracle test hears a changed draw).
 * @author Rene Weller
 */
#include "ambient/Voice.h"
#include "ambient/Simd.h"
#include "ambient/Params.h"   // StackRatios
#include "ambient/Tuning.h"   // intervalConsonance for the portamento gravity
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ambient
{

namespace
{
/**
 * @brief The level a source keeps at a distance: Range dB lost between the ear and the horizon.
 *
 * Until 25.09.2026 this was 1 - 0.5 d, six decibels at the horizon and linear in between, which
 * left the background a plane the ear could not place under the foreground: the production guide
 * (and Zahorik's -6 dB per doubling, taken over a log-distance plane) wants 20 to 36 dB between
 * the near and the far layer. Exponential in d, so every step of the plane costs the same number
 * of decibels: at Range 20 a note at Depth 0.7 is 14 dB under one at the ear.
 * @param[in] d        the perceived distance, 0 at the ear .. 1 on the horizon
 * @param[in] rangeDb  the dB lost over the whole plane (Depth Range)
 * @return the linear gain, 1 at the ear
 */
inline float depthLevel( float d, float rangeDb )
{
    return std::pow( 10.0f, -clampv( rangeDb, 0.0f, 60.0f ) * d / 20.0f );
}

/**
 * @brief The far send's gap at a distance, in samples: Gap ms at the ear, none on the horizon.
 * @param[in] d      the perceived distance, 0 .. 1
 * @param[in] gapMs  Depth Gap, the pre-delay a source at the ear gets before the far reverb
 * @param[in] sr     the sample rate in Hz
 * @return the delay in samples, at most what the ring holds (prepare() sized it for 80 ms)
 */
inline float depthGapSamples( float d, float gapMs, double sr )
{
    return clampv( gapMs, 0.0f, 80.0f ) * ( 1.0f - clampv( d, 0.0f, 1.0f ) ) * static_cast<float>( sr ) * 0.001f;
}

constexpr float FarDlyGlide =
    0.0005f;   ///< per-sample glide of the far send's gap (about 40 ms at 48 kHz), as the reverbs glide their pre-delay

/**
 * @brief A phasor (cosine, sine) at a phase given in cycles: the voice's name for Simd.h's phasorFrom().
 * @param[in]  phase01  the phase in cycles, 0 .. 1 (a rotation per sample is given the same way)
 * @param[out] c        receives the cosine
 * @param[out] s        receives the sine
 */
inline void phasorFromPhase( double phase01, float &c, float &s )
{
    phasorFrom( phase01, c, s );
}

/** @brief log2 of the harmonic numbers, so the presence bell needs one log2 per strand, not per partial. */
struct Log2Harmonics
{
    float m_v[MaxPartials + 1];   ///< v[h] = log2(h) for h = 1 .. MaxPartials; v[0] is 0 and unused
    /** @brief Fills the table once, at static initialisation. */
    Log2Harmonics()
    {
        m_v[0] = 0.0f;
        for ( int h = 1; h <= MaxPartials; ++h ) m_v[h] = std::log2( static_cast<float>( h ) );
    }
};
const Log2Harmonics Log2H;                            ///< the one table, read by control() for the presence bell
constexpr float     PresenceCentreLog2 = 11.64386f;   ///< log2(3200 Hz): the bell spans about 1.8-5.6 kHz
constexpr float     PresenceHalfWidth = 0.8f;         ///< octaves to the bell's zero

/** @brief Harmonic numbers as floats: phase modulation of the waveform by θ moves partial h by h·θ. */
struct HarmonicNumbers
{
    float m_v[MaxPartials];   ///< v[i] = i + 1, the harmonic number of partial i, as the FM bank step wants it
    /** @brief Fills the table once, at static initialisation. */
    HarmonicNumbers()
    {
        for ( int h = 0; h < MaxPartials; ++h ) m_v[h] = static_cast<float>( h + 1 );
    }
};
const HarmonicNumbers Hf;   ///< the one table, handed to phasorBankStepFm() every sample of a modulated voice
constexpr float       FmMaxStep =
    0.4f;   ///< per-sample deviation clamp (tan of the angle): keeps the two-step normalisation exact
constexpr float Sqrt2 =
    1.41421356237f;   ///< sqrt(2): a partial's whole weight on one ear when Partial Spread stops it there
constexpr float Sqrt2Half =
    0.70710678119f;   ///< 1 / sqrt(2): composes the spread's sum and difference with the strand's pan angle
}   // namespace

void Voice::prepare( double sampleRate, uint64_t seed )
{
    m_sr = sampleRate;
    {
        // The far send's gap ring: 80 ms (the Gap knob's top) plus room for the interpolation, a
        // power of two so the read wraps with a mask.
        int size = 16;
        while ( size < static_cast<int>( 0.08 * m_sr ) + 8 ) size <<= 1;
        m_farDlyL.assign( static_cast<size_t>( size ), 0.0f );
        m_farDlyR.assign( static_cast<size_t>( size ), 0.0f );
        m_farDlyMask = size - 1;
        m_farDlyW = 0;
        m_farDly = m_farDlyTarget = 0.0f;
    }
    // The modal bank derives every mode's angle and radius from its own copy of the rate, and
    // nothing ever set it: at 96 kHz the modes rang an octave high and half as long.
    m_zModal.prepare( static_cast<float>( m_sr ) );
    m_ildCoefConst = 1.0f - std::exp( -TwoPi * 1000.0f / static_cast<float>( m_sr ) );
    m_skyProto.setQ( 8000.0f, 1.5f, static_cast<float>( m_sr ) );   // Blauert's band: fixed frequency, fixed Q
    m_rng.seed( seed );
    m_env.setSampleRate( m_sr );
    for ( auto &s : m_strands )
    {
        for ( int h = 0; h < MaxPartials; ++h )
        {
            s.m_shimmer[h].init( m_rng );
            phasorFromPhase( m_rng.uniform(), s.m_pc[h], s.m_ps[h] );
            s.m_rc[h] = 1.0f;
            s.m_rs[h] = 0.0f;
            s.m_amp[h] = 0.0f;
            s.m_ampStep[h] = 0.0f;
        }
        s.m_pitch.init( m_rng );
        s.m_active = 0;
    }
    m_filterDrift.init( m_rng );
    m_airDrift.init( m_rng );
    m_panCenter.init( m_rng );
    m_breath.init( m_rng );
    m_rateWander.init( m_rng );
    m_zDriftX.init( m_rng );
    m_zDriftY.init( m_rng );
    for ( auto &r : m_zbL ) r.reset();
    for ( auto &r : m_zbR ) r.reset();
    // Slot 0 (Source 1) came later than the other two: it takes a seed derived from the voice's
    // rather than a fork from the stream, so the stream -- and with it every preset's random
    // phases and drifts -- stayed exactly where it was before the slot existed.
    m_slots[0].prepare( m_sr, seed ^ 0x9E3779B97F4A7C15ull );
    {   // the phase field's drifter seeds from a side stream, so the voice's stream is untouched
        Rng aux;
        aux.seed( seed ^ 0x51ED270B9E3779B9ull );
        m_phaseDrift.init( aux );
    }
    m_ksOn = false;
    m_ksLen = 0;
    std::memset( m_apX, 0, sizeof( m_apX ) );
    std::memset( m_apY, 0, sizeof( m_apY ) );
    // Slots 2 and 3 fork the voice's stream, as they always did. Slot 4 came later and seeds from
    // a side stream instead: one more fork here would advance the voice's stream by a draw and
    // move every random decision after it, and the oracle would report all 39 presets changed --
    // which is exactly what it did before this line was split.
    for ( int k = 1; k < 3; ++k ) m_slots[k].prepare( m_sr, m_rng.fork() );
    for ( int k = 3; k < Slots; ++k )
        m_slots[k].prepare( m_sr, seed ^ ( 0xC2B2AE3D27D4EB4Full + static_cast<uint64_t>( k ) ) );
    m_filt.prepare( m_sr );
    m_airL.reset();
    m_airR.reset();
    m_foldOsL.reset();
    m_foldOsR.reset();
    m_folding = false;
    std::memset( m_dryRingL, 0, sizeof( m_dryRingL ) );
    std::memset( m_dryRingR, 0, sizeof( m_dryRingR ) );
    m_dryW = 0;
    std::memset( m_itdBufL, 0, sizeof( m_itdBufL ) );
    std::memset( m_itdBufR, 0, sizeof( m_itdBufR ) );
    m_itdW = 0;
    m_itdL = m_itdR = m_itdLTarget = m_itdRTarget = 0.0f;
    m_cachedTilt = -1.0f;
    m_cachedOddEven = -9.0f;
    m_cachedPartials = -1;
    m_cachedB = -1.0f;
    m_env.kill();
    m_note = -1;
}

/**
 * @brief The plane as the ear would rank it.
 *
 * Heard distance grows with physical distance as roughly a
 * power of a half (Zahorik 2002, 2005: the pooled exponent is about 0.54), so a knob that is
 * linear in the model's distance spends most of its perceptual travel in its near half. Depth
 * Law bends it back: d^(1 + 0.85 law), which at 1 is d^1.85, the inverse of that exponent. The
 * ends stay where they are -- 0 is still the ear and 1 is still the horizon.
 * @param[in] d    the model's distance, 0 (the ear) .. 1 (the horizon)
 * @param[in] law  the Depth Law amount, 0 .. 1; at 0 the distance is returned as it is
 * @return the distance the placement works from, 0 .. 1
 */
static inline float perceivedDistance( float d, float law )
{
    return law > 0.0f ? std::pow( clampv( d, 0.0f, 1.0f ), 1.0f + 0.85f * clampv( law, 0.0f, 1.0f ) ) : d;
}

void Voice::noteOn( int note, double freqHz, float velocity, int owner, float distance, const VoiceParams &p,
                    bool allowStrike, float ageSeconds )
{
    const bool wasSounding = m_env.isActive();   // asked before anything below can change it
    m_note = note;
    m_freq = m_freqTarget = freqHz;
    m_rootComp = 1.0f;   // a note beginning now takes the ratios to the root as it is now
    m_velocity = 0.3f + 0.7f * clampv( velocity, 0.0f, 1.0f );
    m_owner = owner;
    m_distance = clampv( distance, 0.0f, 1.0f );
    m_distTarget = m_distance;
    m_distSeconds = 0.0f;
    m_distEff = perceivedDistance( m_distance, p.m_depthLaw );
    m_gNear = std::cos( m_distEff * 0.5f * Pi );
    m_gFar = std::sin( m_distEff * 0.5f * Pi );
    m_gLevel = depthLevel( m_distEff, p.m_depthRange );
    m_farDly = m_farDlyTarget =
        depthGapSamples( m_distEff, p.m_depthPreDelay, m_sr );   // no glide from zero: the note begins where it stands
    // Which of the five roles this note belongs to, for the matrix and for the plane it stands in.
    // The second conductor is the shadow whatever it plays; everything else is read off the
    // register, because in this music the register IS the role (Rene's table). A near event
    // (owner 3) is the colour, wherever it sits.
    m_layer = ( owner == 2 )   ? 1.0f   // 2 = the second conductor (Engine::OwnerBrain2)
              : ( owner == 3 ) ? 0.5f
                               : ( note < 43   ? 0.0f
                                   : note < 60 ? 0.25f
                                   : note < 84 ? 0.5f
                                               : 0.75f );
    // A near event's shape was set just before this; any other note starts with none of it, or a
    // voice that played an event a minute ago would keep its gate and its lift.
    if ( owner != 3 )
    {
        m_releaseMul = 1.0f;
        m_cutoffMul = 1.0f;
        m_proxDb = 0.0f;
        m_panOffset = 0.0f;
        m_nearGain = 1.0f;
    }
    // The near field's lift is a band, 120 to 300 Hz, and not a shelf: below it stand the
    // Foundation and Bass Mono, and a shelf there muddied the sub, which is where the depth lives.
    m_proxAmt = m_proxDb > 0.0f ? dbToGain( m_proxDb ) - 1.0f : 0.0f;
    m_proxLoCoef = 1.0f - std::exp( -TwoPi * 120.0f / static_cast<float>( m_sr ) );
    m_proxHiCoef = 1.0f - std::exp( -TwoPi * 300.0f / static_cast<float>( m_sr ) );
    {
        const float angle = ( m_panOffset + 1.0f ) * 0.25f * Pi;
        // The event's Gain rides on the pan pair: one multiplication that is already there, after
        // the sources and the filter, so it changes how loud the event is and nothing about it.
        m_panL = m_nearGain * 1.41421356f * std::cos( angle );
        m_panR = m_nearGain * 1.41421356f * std::sin( angle );
    }
    // The slot roles start where the note stands; control() fades them as the cluster changes.
    for ( int k = 0; k < Slots; ++k ) m_roleGain[k] = roleTarget( p.m_slot[k].m_role, m_placeLowest, m_placeHighest );
    // Velocity as an attack time rather than as a level (R7.1): the air floats in, the foundation
    // simply stands. Positive lets a quiet note enter slower and a loud one faster; at the extreme
    // the slowest is four times the written attack and the fastest a quarter of it. The factor is
    // fixed here, at the note's own velocity, because control() rewrites the times every block.
    m_attackMul = p.m_velAttack != 0.0f
                      ? std::pow( 4.0f, p.m_velAttack * ( 0.5f - clampv( velocity, 0.0f, 1.0f ) ) * 2.0f )
                      : 1.0f;
    m_env.setTimes( p.m_attack * m_attackMul, p.m_decay, p.m_sustain, p.m_release );
    for ( auto &s : m_slots ) s.noteOn( !m_env.isActive() );
    if ( !m_env.isActive() )
    {
        // Fresh start: random phases (no two voices share a waveform), silent partials.
        for ( auto &s : m_strands )
        {
            for ( int h = 0; h < MaxPartials; ++h )
            {
                phasorFromPhase( m_rng.uniform(), s.m_pc[h], s.m_ps[h] );
                s.m_amp[h] = 0.0f;
                s.m_ampStep[h] = 0.0f;
            }
            s.m_active = 0;
        }
        m_filt.reset();
        m_airL.reset();
        m_airR.reset();
        m_foldOsL.reset();
        m_foldOsR.reset();
        m_folding = false;
        std::memset( m_dryRingL, 0, sizeof( m_dryRingL ) );
        std::memset( m_dryRingR, 0, sizeof( m_dryRingR ) );
        std::memset( m_itdBufL, 0, sizeof( m_itdBufL ) );
        std::memset( m_itdBufR, 0, sizeof( m_itdBufR ) );
        std::fill( m_farDlyL.begin(), m_farDlyL.end(), 0.0f );
        std::fill( m_farDlyR.begin(), m_farDlyR.end(), 0.0f );
        m_proxLoL = m_proxLoR = m_proxHiL = m_proxHiR = 0.0f;
        // An inherited note has been sounding: its Bloom is open and its sources have entered.
        m_bloomT = ageSeconds;
        m_slotT = ageSeconds;
        for ( float &s : m_slotShift ) s = 0.0f;
        m_slotHeld = false;
        m_prevDist = m_distance;
    }
    m_press = m_pressTarget;
    m_slide = m_slideTarget;
    m_bend = m_bendTarget;   // a new note starts where its controller is
    // An inherited note enters at the level its age says it has reached; anything else climbs its
    // attack as it always did. Only a preset change passes an age at all.
    if ( ageSeconds > 0.0f && !wasSounding )
        m_env.noteOnAged( ageSeconds );
    else
        m_env.noteOn();
    if ( p.m_strikeLevel > 0.0f && allowStrike && ( owner == 0 || p.m_strikeBrain ) )
    {
        strikeStart( freqHz, p );
        ++m_strikes;
    }
}

/**
 * @brief The strike: a burst of noise into a delay loop with a damping low pass (Karplus-Strong).
 *
 * String
 * rings at the note; Wood is two octaves up, short and dull; Metal puts an all-pass in the loop,
 * which stretches the partials into something clangorous. It plays on the near plane whatever
 * the voice's distance -- the intimate impulse that makes the background behind it vast.
 */
void Voice::strikeStart( double hz, const VoiceParams &p )
{
    const int type = clampv( p.m_strikeType, 0, 2 );
    double    f = hz;
    if ( type == 1 ) f = clampv( hz * 4.0, 200.0, 2500.0 );
    m_ksLen = clampv( static_cast<int>( m_sr / std::max( f, 20.0 ) ), 8, StrikeMax - 1 );
    std::memset( m_ks, 0, sizeof( float ) * static_cast<size_t>( m_ksLen ) );
    m_ksPos = 0;
    m_ksLeft = m_ksLen;
    float decay = std::max( p.m_strikeDecay, 0.02f );
    if ( type == 1 ) decay *= 0.35f;
    m_ksG = std::pow( 10.0f, -3.0f * static_cast<float>( m_ksLen ) / ( decay * static_cast<float>( m_sr ) ) );
    m_ksLpC = 1.0f - 0.9f * clampv( p.m_strikeDamp, 0.0f, 1.0f );
    if ( type == 1 ) m_ksLpC = std::min( m_ksLpC, 0.25f );
    m_ksApK = type == 2 ? 0.55f : 0.0f;
    m_ksLp = m_ksApX = m_ksApY = 0.0f;
    m_ksAmp = 0.6f * p.m_strikeLevel * m_velocity;
    m_ksT = 0;
    m_ksTotal = static_cast<int>( decay * 1.5f * static_cast<float>( m_sr ) ) + m_ksLen;
    const float angle = ( clampv( m_centre, -1.0f, 1.0f ) + 1.0f ) * 0.25f * Pi;
    m_ksGainL = std::cos( angle );
    m_ksGainR = std::sin( angle );
    m_ksOn = true;
}

inline float Voice::strikeTick()
{
    float in = 0.0f;
    if ( m_ksLeft > 0 )
    {
        in = m_ksAmp * m_rng.bipolar();
        --m_ksLeft;
    }
    const float d = m_ks[m_ksPos];
    m_ksLp += m_ksLpC * ( d - m_ksLp );
    float v = m_ksLp;
    if ( m_ksApK != 0.0f )
    {
        const float y = -m_ksApK * v + m_ksApX + m_ksApK * m_ksApY;
        m_ksApX = v;
        m_ksApY = y;
        v = y;
    }
    m_ks[m_ksPos] = in + m_ksG * v;
    if ( ++m_ksPos >= m_ksLen ) m_ksPos = 0;
    if ( ++m_ksT > m_ksTotal ) m_ksOn = false;
    return d;
}

void Voice::noteOff()
{
    m_env.noteOff();
}
/**
 * @brief Silence at once, and that includes a strike still ringing on its own physics: the render loop
 *        now runs while EITHER the envelope or the strike is going, and a reset has to stop both.
 */
void Voice::kill()
{
    m_env.kill();
    m_note = -1;
    m_portaLeft = 0.0f;
    m_ksOn = false;
}

void Voice::glideFrom( double fromHz, float seconds, float gravity )
{
    if ( fromHz <= 0.0 || seconds <= 0.0f ) return;
    m_portaFrom = fromHz;
    m_freq = fromHz;
    m_portaSeconds = seconds;
    m_portaLeft = seconds;
    m_portaGravity = clampv( gravity, 0.0f, 1.0f );
}

void Voice::glideTo( int note, double hz, float seconds, float gravity )
{
    if ( hz <= 0.0 || !m_env.isActive() ) return;
    m_note = note;
    m_portaFrom = m_freq;
    m_freqTarget = hz;
    m_portaSeconds = std::max( seconds, 0.05f );
    m_portaLeft = m_portaSeconds;
    m_portaGravity = clampv( gravity, 0.0f, 1.0f );
}

void Voice::control( int blockLen, const VoiceParams &p )
{
    const float dtReal = static_cast<float>( blockLen / m_sr );
    // Freeze: the movement clock stops (spectrum, pitch drift, breath, bloom hold still); the
    // envelope, filter and effects keep their own time.
    const float dt = p.m_freeze ? 0.0f : dtReal;
    m_env.setTimes( p.m_attack * m_attackMul, p.m_decay, p.m_sustain, p.m_release * m_releaseMul );
    // A plane on its way somewhere (moveTo): an exponential approach that arrives, near enough,
    // in the seconds it was given.
    if ( m_distSeconds > 0.0f && dtReal > 0.0f )
    {
        m_distance += ( m_distTarget - m_distance ) * ( 1.0f - std::exp( -3.0f * dtReal / m_distSeconds ) );
        if ( std::fabs( m_distance - m_distTarget ) < 1.0e-3f )
        {
            m_distance = m_distTarget;
            m_distSeconds = 0.0f;
        }
    }

    // Portamento: slide in the log domain from m_portaFrom to the target; the speed drops near
    // consonant ratios to the root (gravity), so the slide dwells on the harmonic nodes and
    // hurries across the dissonant stretches. The nominal time is what an even slide takes.
    if ( m_portaLeft > 0.0f && dtReal > 0.0f )
    {
        const double total = std::log( m_freqTarget / m_portaFrom );
        if ( std::fabs( total ) < 1e-9 )
        {
            m_portaLeft = 0.0f;
            m_freq = m_freqTarget;
        }
        else
        {
            // Gravity as an attraction to the nearest node, not as a function of how consonant
            // the present ratio happens to be. The first version braked in proportion to
            // intervalConsonance(), which rises and falls smoothly across the whole slide, so the
            // pull was everywhere and nowhere -- more like wading than like a magnet. A magnet
            // has almost no reach and then all of it: 1/(1 + (d/d0)^2) with d the distance in
            // cents to the nearest just ratio. Thirty cents away it is at half strength; a
            // semitone away it is down to eight per cent and the slide runs free.
            const double cents = 1200.0 * std::log2( std::max( m_freq, 1.0 ) / std::max( p.m_rootHz, 1.0 ) );
            double       fold = std::fmod( cents, 1200.0 );
            if ( fold < 0.0 ) fold += 1200.0;
            static const double Nodes[] = {
                0.0, 203.91, 315.64, 386.31, 498.04, 701.96, 813.69, 884.36, 996.09, 1200.0
            };
            double dist = 1200.0;
            for ( double node : Nodes ) dist = std::min( dist, std::fabs( fold - node ) );
            const double pull = 1.0 / ( 1.0 + ( dist / 30.0 ) * ( dist / 30.0 ) );
            const double slow = 1.0 - 0.90 * m_portaGravity * pull;
            const double step = total / m_portaSeconds * dtReal * slow;
            double       lf = std::log( m_freq ) + step;
            const double lt = std::log( m_freqTarget );
            if ( ( total > 0 && lf >= lt ) || ( total < 0 && lf <= lt ) )
            {
                lf = lt;
                m_portaLeft = 0.0f;
            }
            m_freq = std::exp( lf );
            m_portaLeft =
                m_portaLeft > 0.0f ? std::max( m_portaLeft - dtReal * static_cast<float>( slow ), 1e-4f ) : 0.0f;
        }
    }
    // Retune glide (tuning purity / drift): log-domain one-pole toward the target, ~1 s.
    else if ( m_freqTarget != m_freq )
    {
        const double c = 1.0 - std::exp( -static_cast<double>( dtReal ) / 1.0 );
        m_freq = std::exp( std::log( m_freq ) + ( std::log( m_freqTarget ) - std::log( m_freq ) ) * c );
        if ( std::fabs( m_freq - m_freqTarget ) < 1e-5 * m_freqTarget ) m_freq = m_freqTarget;
    }

    // Breath: the voice's plane itself wanders slowly (Rich's "the room breathes"): everything
    // that hangs on the distance -- dry/wet balance, level, air absorption, presence -- moves
    // with it, continuously (Drifter = smoothstep curves, no steps).
    // Rate wander (nested modulation): one very slow curve per voice scales every movement
    // rate by up to +-1 octave, so five minutes never look like the five before.
    const float rw = m_rateWander.update( dt, 0.01f, m_rng );
    const float rateMul = p.m_rateWander > 0.0f ? std::pow( 2.0f, rw * p.m_rateWander ) : 1.0f;
    m_rateMul = rateMul;
    // Partial Spread: each partial of the bank gets its own place between the ears, on a
    // pattern that turns once every fifty seconds (the golden angle between neighbours, so no
    // two harmonically related partials sit together). Equal power per partial: the pair of
    // weights squares to two, which is what the mono path's identical left and right add up to.
    // Partial Spread follows the plane like the strands do (Near Width): a share at the ear, all
    // of it on the horizon.
    m_spreadAmt =
        clampv( p.m_partialSpread, 0.0f, 1.0f ) *
        ( clampv( p.m_depthWidth, 0.0f, 1.0f ) + ( 1.0f - clampv( p.m_depthWidth, 0.0f, 1.0f ) ) * m_distEff );
    if ( m_spreadAmt > 0.0f )
    {
        m_spreadPhase += static_cast<double>( dt ) * 0.02;
        if ( m_spreadPhase >= 1.0 ) m_spreadPhase -= 1.0;
        for ( int h = 0; h < MaxPartials; ++h )
        {
            const float w = m_spreadAmt * std::sin( static_cast<float>( h ) * 2.39996f +
                                                    static_cast<float>( m_spreadPhase ) * TwoPi );
            const float g = 1.0f / std::sqrt( 1.0f + w * w );
            const float wl = ( 1.0f + w ) * g, wr = ( 1.0f - w ) * g;   // equal power: wl^2 + wr^2 = 2
            // Kept as the pair's sum and difference, which is all a strand needs to carry its own
            // place into it -- they are the cosine and sine of the partial's angle less 45 degrees.
            m_spreadSum[h] = wl + wr;
            m_spreadDif[h] = wl - wr;
        }
    }
    const float driftRate = p.m_driftRate * rateMul, shimmerRate = p.m_shimmerRate * rateMul;

    // Expression, smoothed towards what the controller last said (about 30 ms).
    if ( !p.m_oneEuro )
    {
        const float c = clampv( dtReal / 0.03f, 0.0f, 1.0f );
        m_press += ( m_pressTarget - m_press ) * c;
        m_slide += ( m_slideTarget - m_slide ) * c;
        m_bend += ( m_bendTarget - m_bend ) * c;
    }
    else
    {
        // The one-euro filter (Casiez, Roussel and Vogel 2012): a one-pole whose cutoff rises
        // with the speed of what it is filtering, so a hand at rest is smoothed hard -- one
        // hertz or so, which takes the sensor's jitter out of a held note -- and a hand that moves
        // is followed at once. Controllers speak in steps, not streams, so the speed is taken
        // from how far the value still has to travel rather than from a derivative of the
        // input, which between two messages would be zero and leave a fast gesture crawling.
        auto euro = [dtReal]( float &y, float x, float unitsPerSecond )
        {
            const float fc = 0.6f + 30.0f * std::fabs( x - y ) / unitsPerSecond;
            const float a = 1.0f - std::exp( -TwoPi * fc * dtReal );
            y += a * ( x - y );
        };
        euro( m_press, m_pressTarget, 1.0f );
        euro( m_slide, m_slideTarget, 1.0f );
        euro( m_bend, m_bendTarget, 12.0f );
    }
    const float bd = m_breath.update( dt, p.m_breathRate * rateMul, m_rng );
    // Pressure pulls the note towards the listener: the plane already decides brightness, level,
    // dryness and presence, so one finger moves all of them the way leaning into a note does.
    const float pressPull = p.m_pressDistance * m_press;
    m_distEff = perceivedDistance( clampv( m_distance * ( 1.0f - pressPull ) + 0.35f * p.m_breath * bd, 0.0f, 1.0f ),
                                   p.m_depthLaw );
    // Doppler: the breathing distance has a velocity; a voice coming closer rises a little, one
    // receding falls. One plane unit is taken as about twenty metres.
    if ( p.m_doppler > 0.0f && dtReal > 0.0f )
    {
        const float vel = ( m_distEff - m_prevDist ) / dtReal;
        m_dopplerMul = clampv( 1.0 - 0.06 * static_cast<double>( p.m_doppler * vel ), 0.97, 1.03 );
    }
    else
        m_dopplerMul = 1.0;
    m_prevDist = m_distEff;
    m_gNear = std::cos( m_distEff * 0.5f * Pi );
    m_gFar = std::sin( m_distEff * 0.5f * Pi );
    m_gLevel = depthLevel( m_distEff, p.m_depthRange );
    m_farDlyTarget = depthGapSamples( m_distEff, p.m_depthPreDelay, m_sr );
    // Presence: a broad bell in the 2-5 kHz articulation band, only on the near plane. Applied
    // in the additive domain (per partial), so it costs nothing per sample.
    const float presLin =
        p.m_presence > 0.0f ? std::pow( 10.0f, p.m_presence * ( 1.0f - m_distEff ) / 20.0f ) - 1.0f : 0.0f;
    const float lowCut = p.m_lowCut;

    // Bloom: the spectrum opens over bloomTime seconds (smoothstep, so the start is gentle).
    m_bloomT += dt;
    const float bt = clampv( m_bloomT / std::max( p.m_bloomTime, 1.0f ), 0.0f, 1.0f );
    const float bloomOpen = bt * bt * ( 3.0f - 2.0f * bt );

    // Each slot's own entrance, from this note. A preset that says nothing about it -- no delay
    // and no shape -- gets a gain of exactly one and is not touched, which is what every preset
    // written before these parameters existed relies on.
    //
    // On real time, not the movement clock: an entrance is an envelope, and Freeze holds the
    // spectrum and the pitch still, not the envelopes (Help says so, and the amp envelope above
    // keeps its time). Read from m_bloomT, which Freeze stops, a slot with a delay or a shape of
    // its own never entered while Freeze was on -- and a preset that had Freeze on from the
    // start (the 2.0 library writes it into six per cent of them) with every slot delayed or
    // shaped was silent for good. Five of the fourteen thousand were, and four more had only
    // their undelayed slot left.
    m_slotT += dtReal;
    {
        const bool held = m_env.isActive() && !m_env.isReleasing();
        for ( int k = 0; k < Slots; ++k )
        {
            const SlotParams &sp = p.m_slot[k];
            m_slotEnvT[k] = -1.0f;
            if ( sp.m_delaySec <= 0.0f && sp.m_envIndex < 0 && !sp.m_ownEnv )
            {
                m_slotGain[k] = 1.0f;
                continue;
            }
            const float t = m_slotT - sp.m_delaySec;
            if ( t <= 0.0f )
            {
                m_slotGain[k] = 0.0f;
                continue;
            }
            // A shape: the slot's own, or one of the six borrowed. The own one may be a single
            // point -- a level -- where a borrowed one needs two to be a contour at all.
            const bool    borrowed = !sp.m_ownEnv && sp.m_envIndex >= 0 && sp.m_envIndex < NumModEnvs;
            const ModEnv *shape =
                sp.m_ownEnv ? p.m_srcEnvShape[k] : ( borrowed ? p.m_envShape[sp.m_envIndex] : nullptr );
            const ModEnvSpec *spec =
                sp.m_ownEnv ? p.m_srcEnvSpec[k] : ( borrowed ? p.m_envSpec[sp.m_envIndex] : nullptr );
            if ( shape != nullptr && shape->count() > ( sp.m_ownEnv ? 0 : 1 ) )
            {
                const float    scale = spec != nullptr ? std::max( spec->m_timeScale, 0.01f ) : 1.0f;
                const EnvModeE mode = spec != nullptr ? spec->m_mode : EnvModeE::ONE_SHOT;
                // Sustain Loop, the moment the note is let go: carry on from where the shape was
                // being read -- its sustain point, or wherever in its loop it had got to. The
                // note's clock ran on while the shape held, and reading the release from that
                // clock snapped the level to wherever the clock had got to in the tail.
                if ( mode == EnvModeE::SUSTAIN_LOOP && m_slotHeld && !held )
                    m_slotShift[k] = t - shape->readTime( ( t - m_slotShift[k] ) / scale, mode, true ) * scale;
                const float u = ( t - m_slotShift[k] ) / scale;
                const float v = shape->at( u, mode, held );
                m_slotEnvT[k] = shape->readTime( u, mode, held );   // where in the shape, for the editor's playhead
                if ( sp.m_ownEnv )
                {
                    // A slot's own shape is a level: 0 is silence, 1 the slot at its written
                    // level, and Depth 0 leaves it at that level whatever the shape says.
                    const float depth = spec != nullptr ? clampv( spec->m_depth, 0.0f, 1.0f ) : 1.0f;
                    m_slotGain[k] = 1.0f - depth * ( 1.0f - clampv( v, 0.0f, 1.0f ) );
                }
                else
                {
                    // The six are bipolar, as modulation shapes are; read as a level, -1 is
                    // silence and +1 is the slot at its written level.
                    m_slotGain[k] = clampv( 0.5f + 0.5f * v, 0.0f, 1.0f );
                }
            }
            else
            {
                const float r = clampv( t / std::max( sp.m_riseSec, 0.01f ), 0.0f, 1.0f );
                m_slotGain[k] = r * r * ( 3.0f - 2.0f * r );   // smoothstep, as Bloom above
            }
        }
        m_slotHeld = held;
    }
    {   // The slot roles: each slot's gain from where the note stands, faded over a second and a
        // half so a note that stops being the top hands its chime on instead of dropping it.
        const float c = 1.0f - std::exp( -dtReal / 1.5f );
        for ( int k = 0; k < Slots; ++k )
        {
            const float t = roleTarget( p.m_slot[k].m_role, m_placeLowest, m_placeHighest );
            m_roleGain[k] += ( t - m_roleGain[k] ) * c;
            if ( std::fabs( m_roleGain[k] - t ) < 1.0e-4f ) m_roleGain[k] = t;
        }
    }
    const float brightness = clampv( p.m_brightness * ( 1.0f - p.m_bloom * ( 1.0f - bloomOpen ) ) + p.m_cohBrightness +
                                         p.m_pressBright * m_press,
                                     0.0f, 1.0f );

    // Base spectrum shared by all strands of this voice. The tilt/odd-even shape is
    // cached (pow is expensive); only the brightness window is applied per block.
    const int partials = clampv( p.m_partials, 1, MaxPartials );
    if ( p.m_tilt != m_cachedTilt || p.m_oddEven != m_cachedOddEven || partials != m_cachedPartials )
    {
        for ( int h = 1; h <= partials; ++h )
        {
            float a = std::pow( static_cast<float>( h ), -p.m_tilt );
            if ( p.m_oddEven > 0.0f && ( h % 2 ) == 0 ) a *= 1.0f - p.m_oddEven;
            if ( p.m_oddEven < 0.0f && ( h % 2 ) == 1 && h > 1 ) a *= 1.0f + p.m_oddEven;
            m_tiltCache[h] = a;
        }
        m_cachedTilt = p.m_tilt;
        m_cachedOddEven = p.m_oddEven;
        m_cachedPartials = partials;
    }
    const float hc = 1.0f + brightness * brightness * 31.0f;   // brightness -> last full-level harmonic
    float       base[MaxPartials + 1];
    for ( int h = 1; h <= partials; ++h )
    {
        float a = m_tiltCache[h];
        if ( static_cast<float>( h ) > hc )
        {
            const float x = std::min( ( static_cast<float>( h ) - hc ) / 6.0f, 1.0f );
            a *= 0.5f * ( 1.0f + std::cos( Pi * x ) );
        }
        base[h] = a;
    }
    const float B = p.m_inharmonic * p.m_inharmonic * 0.02f;
    if ( B != m_cachedB )
    {
        for ( int h = 1; h <= MaxPartials; ++h )
            m_stretchCache[h - 1] = B > 0.0f ? std::sqrt( 1.0 + B * static_cast<double>( h * h ) ) : 1.0;
        m_cachedB = B;
    }
    const int    unison = clampv( p.m_unison, 1, MaxStrands );
    const float  norm = p.m_level * ( 1.0f + p.m_pressLevel * m_press ) / std::sqrt( static_cast<float>( unison ) );
    const double nyq = 0.45 * m_sr;
    const float  invLen = 1.0f / static_cast<float>( blockLen );

    // The voice's centre wanders slowly; strands fan out around it. Source 1's Pan shifts the
    // centre, its Octave and Ratio move the whole bank, so the three slots read alike.
    const SlotParams &s1 = p.m_slot[0];
    const float       centre =
        clampv( m_panCenter.update( dt, driftRate * 0.3f, m_rng ) * p.m_panDrift + p.m_cohPan + s1.m_pan + m_panOffset,
                -1.0f, 1.0f );
    m_centre = centre;
    const double bankMul =
        SlotRatios[clampv( s1.m_ratio, 0, NumSlotRatios - 1 )] * std::pow( 2.0, clampv( s1.m_octave, -2, 2 ) ) *
        ( static_cast<double>( p.m_pitchMul ) * m_dopplerMul )   // the tide and the doppler, 1.0 exactly when off
        * ( m_bend != 0.0f ? std::pow( 2.0, static_cast<double>( m_bend ) / 12.0 ) : 1.0 );
    const int stack = clampv( p.m_stack, 0, NumStacks - 1 );
    // Near Width (25.09.2026): the fan of strands is a share of Spread at the ear and all of it on
    // the horizon, so a near source is a place and a far one a surround -- the guide's width by
    // plane (near 20-40 %, far 90-100 %), read off the same distance as everything else.
    const float widthD =
        clampv( p.m_depthWidth, 0.0f, 1.0f ) + ( 1.0f - clampv( p.m_depthWidth, 0.0f, 1.0f ) ) * m_distEff;

    for ( int si = 0; si < unison; ++si )
    {
        Strand     &s = m_strands[si];
        const float pos =
            ( unison == 1 ) ? 0.0f : ( 2.0f * static_cast<float>( si ) / static_cast<float>( unison - 1 ) - 1.0f );
        float cents = pos * p.m_detune + s.m_pitch.update( dt, driftRate, m_rng ) * p.m_drift;
        // Two limits on how far a strand may stand from its note, and both are about the beat
        // rather than about the pitch. The beat between a strand and its note is f (2^(c/1200) - 1),
        // so the same cents beat faster the higher the note sits: Low Detune thins the detuning
        // out towards the bottom, where warmth turns into wobble, and Beat Ceiling is the flat
        // upper bound on the rate itself, halved under 150 Hz (R4.6).
        if ( p.m_lowDetune > 0.0f || p.m_beatCeiling > 0.0f )
        {
            const double fHz = m_freq * bankMul;
            if ( p.m_lowDetune > 0.0f )
            {
                const double t =
                    clampv( std::log2( std::max( fHz, 20.0 ) / 150.0 ), 0.0, 1.0 );   // 0 at 150 Hz, 1 at 300
                cents *= 1.0f - 0.5f * p.m_lowDetune * static_cast<float>( 1.0 - t );
            }
            if ( p.m_beatCeiling > 0.0f && fHz > 0.0 )
            {
                const double lim = static_cast<double>( p.m_beatCeiling ) * ( fHz < 150.0 ? 0.5 : 1.0 );
                const double maxCents = 1200.0 * std::log2( 1.0 + lim / fHz );
                if ( std::fabs( cents ) > maxCents ) cents = static_cast<float>( cents > 0.0f ? maxCents : -maxCents );
            }
        }
        // Stack: the strand sits at a pure ratio to the note (a just chord from one key);
        // detune and drift still apply on top, so Detune 0 makes it beat-free.
        const double f = m_freq * bankMul * StackRatios[stack][si] * std::pow( 2.0, cents / 1200.0 );
        const float  pan = clampv( centre + pos * p.m_spread * widthD, -1.0f, 1.0f );
        const float  angle = ( pan + 1.0f ) * 0.25f * Pi;
        s.m_gainL = std::cos( angle ) * norm;
        s.m_gainR = std::sin( angle ) * norm;
        // Partial Spread and the strand's own place, composed instead of multiplied. The spread
        // gives partial h a pair of weights, which is an angle in the field; the strand's pan is
        // another; and what the ear should get is their SUM. Multiplying the pair by the strand's
        // two gains one side at a time is not that: it attenuates every partial by how far it
        // leans away from the pan -- eleven decibels at the outer strand of a six-strand fan, and
        // since the pattern turns once every fifty seconds, coming and going. Which is why the
        // test for this passed: it placed one strand in the middle, and in the middle the two
        // agree exactly.
        //   Composing two angles needs no trigonometry of its own: cos and sin of (angle + phi -
        // 45 degrees) come out of the pair's own sum and difference. Past an ear the sum would
        // carry on to the far side and arrive inverted, which is a partial cancelling itself in
        // mono, so it is stopped AT the ear -- and a stop is still a place, so every partial
        // keeps cos^2 + sin^2 = 1 of its power wherever the pan puts it. A centred strand comes
        // out exactly as it did; a hard-panned one squeezes its field onto that ear.
        if ( m_spreadAmt > 0.0f )
        {
            const float ca = std::cos( angle ), sa = std::sin( angle );
            for ( int h = 0; h < MaxPartials; ++h )
            {
                float l = Sqrt2Half * ( ca * m_spreadSum[h] + sa * m_spreadDif[h] );
                float r = Sqrt2Half * ( sa * m_spreadSum[h] - ca * m_spreadDif[h] );
                if ( l < 0.0f )
                {
                    l = 0.0f;
                    r = Sqrt2;
                }
                else if ( r < 0.0f )
                {
                    l = Sqrt2;
                    r = 0.0f;
                }
                s.m_wL[h] = l;
                s.m_wR[h] = r;
            }
            s.m_spreadGain = norm * Sqrt2Half;
        }

        float       target[MaxPartials];
        float       sumSq = 0.0f;
        int         H = 0;
        const float lf0 = std::log2( static_cast<float>( f ) ) - PresenceCentreLog2;
        for ( int h = 1; h <= partials; ++h )
        {
            // The branch, not a blend: (f * h) * s and f * (h * s) are not the same double, and
            // the oracle would hear the last bit move on six thousand presets that never asked
            // for Match at all.
            const double fh =
                p.m_match > 0.0f ? f * static_cast<double>( p.m_partialRatio[h - 1] ) : f * h * m_stretchCache[h - 1];
            if ( fh >= nyq ) break;
            // Rotation per sample for this partial, and keep the phasor on the unit circle.
            const double inc = fh / m_sr;
            phasorFromPhase( inc, s.m_rc[h - 1], s.m_rs[h - 1] );
            const float r2 = s.m_pc[h - 1] * s.m_pc[h - 1] + s.m_ps[h - 1] * s.m_ps[h - 1];
            const float fix = 1.5f - 0.5f * r2;
            s.m_pc[h - 1] *= fix;
            s.m_ps[h - 1] *= fix;
            const float d = s.m_shimmer[h - 1].update( dt, shimmerRate, m_rng );
            float       a = base[h] * ( 1.0f + 0.9f * p.m_shimmer * d );
            if ( presLin > 0.0f )
            {
                const float x = ( lf0 + Log2H.m_v[h] ) / PresenceHalfWidth;
                if ( x > -1.0f && x < 1.0f ) a *= 1.0f + presLin * ( 1.0f - x * x );
            }
            if ( lowCut > 0.0f && fh < lowCut )
            {
                const float r = static_cast<float>( fh ) / lowCut;
                a *= r * r;   // 12 dB/oct below the cut
            }
            target[h - 1] = a;
            sumSq += a * a;
            H = h;
        }
        const float scale = sumSq > 0.0f ? 0.5f / std::sqrt( sumSq ) : 0.0f;
        for ( int h = 0; h < MaxPartials; ++h )
        {
            const float tgt = ( h < H ) ? target[h] * scale : 0.0f;
            s.m_ampStep[h] = ( tgt - s.m_amp[h] ) * invLen;
        }
        int act = H;
        for ( int h = H; h < s.m_active; ++h )
            if ( std::fabs( s.m_amp[h] ) > 1e-6f ) act = h + 1;
        s.m_active = act;
    }
    if ( unison < m_lastUnison )
        for ( int si = unison; si < m_lastUnison; ++si )
            for ( int h = 0; h < MaxPartials; ++h )
            {
                m_strands[si].m_amp[h] = 0.0f;
                m_strands[si].m_ampStep[h] = 0.0f;
            }
    m_lastUnison = unison;

    // Interaural time difference from the centre pan: the far ear hears it later.
    float lateral = centre;   // -1..1: where the source is, left to right
    bool  behind = false;
    float shadowOct = 2.7f * p.m_itd;
    if ( p.m_binaural )
    {
        // Headphones: the pan is an azimuth (90 degrees at full pan) and the head's yaw turns
        // the field the other way, so a source stays put in the room while the head moves.
        float az = centre * 90.0f - p.m_headYawDeg;
        while ( az > 180.0f ) az -= 360.0f;
        while ( az < -180.0f ) az += 360.0f;
        behind = std::fabs( az ) > 90.0f;
        const float folded = behind ? ( az > 0.0f ? 180.0f - az : -180.0f - az ) : az;   // -90..90
        const float th = folded * ( Pi / 180.0f );
        // Woodworth's spherical head: tau = (a/c)(theta + sin theta), a = 8.75 cm, which is
        // 0.656 ms at ninety degrees -- the figure the plain mode's 0.65 ms came from.
        const float tau = ( 0.0875f / 343.0f ) * ( std::fabs( th ) + std::sin( std::fabs( th ) ) );
        const float itdSamples = tau * static_cast<float>( m_sr );
        m_itdLTarget = th > 0.0f ? itdSamples : 0.0f;
        m_itdRTarget = th < 0.0f ? itdSamples : 0.0f;
        lateral = std::sin( th );
        shadowOct = 2.7f;   // the head is the head, whatever Time Width says
    }
    else
    {
        const float maxItd = 0.00065f * static_cast<float>( m_sr ) * p.m_itd;
        m_itdLTarget = centre > 0.0f ? centre * maxItd : 0.0f;
        m_itdRTarget = centre < 0.0f ? -centre * maxItd : 0.0f;
    }
    // Head shadow. In the plain mode a one-pole on the far ear, which is enough for a pan.
    // In the binaural mode the spherical-head filter of Brown and Duda (1998): a sphere of
    // radius a diffracts sound into a shadow whose transfer function is well approximated by
    //     H(s) = (1 + alpha beta s) / (1 + beta s),   beta = a / (2 c),
    // where alpha depends on the angle of incidence at that ear --
    //     alpha(t) = 1 + amin/2 + (1 - amin/2) cos(t / tmin * pi),  amin = 0.1, tmin = 150 deg --
    // so the ipsilateral ear is lifted (alpha near 2), the contralateral one shadowed (alpha
    // near 0.1) and the shadow is a shelf rather than a low pass: the far ear keeps a little of
    // its top, which is what a real head does and what a plain one-pole cannot.
    m_sphereShadow = p.m_binaural;
    if ( m_sphereShadow )
    {
        const float az = std::asin( clampv( lateral, -1.0f, 1.0f ) );        // radians, +right
        const float c = static_cast<float>( m_sr ) * ( 0.0875f / 343.0f );   // 1 / (w0 T), w0 = c/a
        for ( int ch = 0; ch < 2; ++ch )
        {
            // Angle of incidence at this ear: 0 when the source is at the ear itself.
            const float earSign = ( ch == 0 ) ? -1.0f : 1.0f;   // left ear points left
            float       theta = std::fabs( az - earSign * 0.5f * Pi ) * ( 180.0f / Pi );
            if ( theta > 180.0f ) theta = 360.0f - theta;
            const float amin = 0.1f, tmin = 150.0f;
            const float alpha = 1.0f + 0.5f * amin + ( 1.0f - 0.5f * amin ) * std::cos( theta / tmin * Pi );
            const float den = 1.0f + c;
            m_sphB0[ch] = ( 1.0f + alpha * c ) / den;
            m_sphB1[ch] = ( 1.0f - alpha * c ) / den;
            m_sphA1[ch] = ( 1.0f - c ) / den;
        }
    }
    const float fcL = 20000.0f * std::pow( 2.0f, -shadowOct * std::max( lateral, 0.0f ) );
    const float fcR = 20000.0f * std::pow( 2.0f, -shadowOct * std::max( -lateral, 0.0f ) );
    m_shadowCoefL = fcL >= 19000.0f ? 1.0f : 1.0f - std::exp( -TwoPi * fcL / static_cast<float>( m_sr ) );
    m_shadowCoefR = fcR >= 19000.0f ? 1.0f : 1.0f - std::exp( -TwoPi * fcR / static_cast<float>( m_sr ) );
    // The near field. The head model above is a far-field one: plane waves, and a level
    // difference that lives above a kilohertz or so where the head is big enough to shadow.
    // Within a metre the wavefront is curved, the two ears are at measurably different
    // distances from the source, and the level difference grows at EVERY frequency, most of
    // all at the low ones the far-field head does not touch -- twenty decibels and more at
    // two hundred hertz for a source near the ear (Brungart and Rabinowitz 1999). That growth
    // is the ear's own cue for "within reach", which no amount of presence or loudness gives
    // it. So: a shelf below a kilohertz, cut on the far ear by up to eighteen decibels and
    // lifted on the near one by up to four, scaled by how far to the side the source is and by
    // the square of its nearness, so it is gone by the middle of the room.
    m_ildAmt = clampv( p.m_nearIld, 0.0f, 1.0f );
    if ( m_ildAmt > 0.0f )
    {
        const float prox = 1.0f - clampv( m_distEff, 0.0f, 1.0f );
        const float lat = clampv( lateral, -1.0f, 1.0f );
        const float amount = m_ildAmt * prox * prox * std::fabs( lat );
        const float cut =
            1.0f - std::pow( 10.0f, -18.0f * amount / 20.0f );   // fraction of the lows taken from the far ear
        const float lift = std::pow( 10.0f, 4.0f * amount / 20.0f ) - 1.0f;   // fraction added to the near one
        m_ildL = lat > 0.0f ? cut : -lift;
        m_ildR = lat < 0.0f ? cut : -lift;
        m_ildCoef = m_ildCoefConst;
    }
    // Externalisation: the pinna's notch sits near 7 kHz for a source in front and climbs towards
    // 9 kHz as it moves to the side; the shoulder reflection arrives about a quarter of a
    // millisecond later. Together they are most of what tells the ear a sound is outside the head.
    m_extAmt = clampv( p.m_externalise, 0.0f, 1.0f );
    // Height, from where the voice stands between the two planes. It is heard through the same
    // pinna notch as externalisation -- the notch IS the elevation cue -- so a raised voice brings
    // that path in on its own, shoulder or no shoulder.
    const float elev = clampv( p.m_elevNear + ( p.m_elevFar - p.m_elevNear ) * m_distEff, -1.0f, 1.0f );
    m_pinnaAmt = std::max( m_extAmt, std::fabs( elev ) );
    m_skyGain = 0.0f;
    if ( m_pinnaAmt > 0.0f )
    {
        const float lat = std::fabs( lateral );
        // Behind the head the pinna's notch sits lower (Brown and Duda 1998), which is most of
        // what tells front from back with no visual cue; only the binaural mode knows about
        // behind, the plain mode's pan has no back. And it rises with height (Hebrank and
        // Wright 1974): about three kilohertz from the horizon to overhead.
        const float notch = 7000.0f + 2000.0f * lat - ( behind ? 1800.0f : 0.0f ) + 3000.0f * elev;
        m_pinnaL.setQ( clampv( notch, 3000.0f, 0.45f * static_cast<float>( m_sr ) ), 2.2f, static_cast<float>( m_sr ) );
        m_pinnaR.copyCoefficients( m_pinnaL );
        m_shoulder = std::max( 1, static_cast<int>( 0.00026 * m_sr ) );
        // Blauert's directional band for "above" sits near 8 kHz: lifted for a source overhead,
        // cut for one below. Guarded at zero so a flat field adds nothing, not even a zero.
        if ( elev != 0.0f )
        {
            m_skyL.copyCoefficients( m_skyProto );
            m_skyR.copyCoefficients( m_skyL );
            m_skyGain = 0.6f * elev;
        }
    }

    // Filter: cutoff follows key, envelope, a slow drift, and distance (air absorption).
    const float fd = m_filterDrift.update( dt, driftRate * 0.5f, m_rng );
    // m_note is given back the moment the envelope is done, and a strike may still be ringing out
    // after that: no key tracking in that case rather than tracking the filter down to note -1.
    const float octaves = p.m_keyTrack * static_cast<float>( ( m_note >= 0 ? m_note : 60 ) - 60 ) / 12.0f +
                          p.m_filterEnv * 4.0f * m_env.level() + p.m_filterDrift * 2.0f * fd +
                          p.m_slideCutoff * m_slide - 2.5f * m_distEff;
    const float cut = p.m_cutoff * m_cutoffMul * std::pow( 2.0f, octaves );
    m_filt.set( static_cast<FilterModelE>( clampv( p.m_filterModel, 0, NumFilterModels - 1 ) ), cut, p.m_resonance,
                p.m_filterDrive, p.m_filterMorph );
    // Binaural phase field: the two ears' all-pass corners drift apart and back on one slow curve;
    // the phase relation changes, not the level, so the room seems to breathe in size.
    m_phaseOn = p.m_phaseWidth > 0.0f;
    if ( m_phaseOn )
    {
        const float d =
            m_phaseDrift.update( dt, p.m_phaseRate * rateMul, m_rng ) * p.m_phaseWidth * 1.5f;   // octaves apart
        const float sr = static_cast<float>( m_sr );
        for ( int ch = 0; ch < 2; ++ch )
        {
            const float sgn = ch == 0 ? 1.0f : -1.0f;
            const float f1 = clampv( 300.0f * std::pow( 2.0f, sgn * d ), 40.0f, 0.4f * sr ),
                        f2 = clampv( 1500.0f * std::pow( 2.0f, sgn * d ), 40.0f, 0.4f * sr );
            const float t1 = std::tan( Pi * f1 / sr ), t2 = std::tan( Pi * f2 / sr );
            m_apC[ch][0] = ( t1 - 1.0f ) / ( t1 + 1.0f );
            m_apC[ch][1] = ( t2 - 1.0f ) / ( t2 + 1.0f );
        }
    }
    if ( p.m_filterOn != m_lastFilterOn )
    {
        if ( p.m_filterOn ) m_filt.reset();
        m_lastFilterOn = p.m_filterOn;
    }   // switched back in: from rest

    // Z-plane: the point wanders around (X, Y) on two Drifters, the frame is interpolated from
    // the shape's corners, resonance narrows the bandwidths, key tracking moves the frame with
    // the note. Mode changes ramp the wet/dry gains so switching never clicks.
    m_zModeCur = clampv( p.m_zMode, 0, 3 );   // 3 = Modal
    if ( m_zModeCur != 0 )
    {
        const float  dx = m_zDriftX.update( dt, p.m_zRate * rateMul, m_rng ),
                     dy = m_zDriftY.update( dt, p.m_zRate * 0.77f * rateMul, m_rng );
        const float  x = clampv( p.m_zX + 0.5f * p.m_zDepth * dx + p.m_cohZ + p.m_slideZ * m_slide, 0.0f, 1.0f ),
                     y = clampv( p.m_zY + 0.5f * p.m_zDepth * dy - p.m_cohZ, 0.0f, 1.0f );
        const ZFrame f = zInterpolate( p.m_zShape, x, y, p.m_zZ );
        const float  track = std::pow( static_cast<float>( m_freq ) / 261.6256f, p.m_zKeyTrack );
        const float  bwScale =
            std::pow( 2.0f, 2.0f * ( 0.5f - p.m_zRes ) );   // resonance 1 -> quarter bandwidth, 0 -> double
        const float sr = static_cast<float>( m_sr );
        ZFrame      scaled = f;   // key tracking and resonance move the whole frame
        for ( int i = 0; i < scaled.m_used; ++i )
        {
            ZSection &s = scaled.m_s[i];
            s.m_poleHz *= track;
            s.m_poleBw = std::max( s.m_poleBw * bwScale * track, 0.5f );
            if ( s.m_zeroHz > 0.0f )
            {
                s.m_zeroHz *= track;
                s.m_zeroBw = std::max( s.m_zeroBw * track, 0.5f );
            }
        }
        if ( m_zModeCur == 3 )
        {
            // Modal: the frame's frequencies are modes of a ringing object, not the corners of a
            // filter. Nothing is cascaded -- the resonators sit in parallel and add.
            m_zModal.set( scaled, p.m_zDecay, p.m_zDamp );
            m_zUsed = m_zModal.used();
            m_zNorm = 1.0f;   // the bank normalises itself on expected power
        }
        else
        {
            m_zUsed = scaled.m_used;
            m_zNorm = zBuildCascade( scaled, m_zbL, sr );
            for ( int i = 0; i < m_zUsed; ++i ) m_zbR[i].copyCoefficients( m_zbL[i] );
        }
        m_zWet = p.m_zMix;
        m_zDry = 1.0f - p.m_zMix;
    }
    else
    {
        m_zWet = 0.0f;
        m_zDry = 1.0f;
        m_zUsed = 0;
    }

    // Air: band-passed noise around a drifting multiple of the fundamental -- or, in Ghost mode,
    // noise through six sharp resonators on the note's harmonics 1 2 3 5 7 9: the harmony is
    // filtered out of the chaos (Rich's string and pipe resonances). Q from Air Q, x8.
    m_ghostGain = 0.0f;
    if ( p.m_air > 0.0f && p.m_airGhost )
    {
        static const float hs[6] = { 1.0f, 2.0f, 3.0f, 5.0f, 7.0f, 9.0f };
        const float        q = clampv( p.m_airQ, 1.0f, 40.0f ) * 8.0f;
        float              expected = 0.0f;
        for ( int i = 0; i < 6; ++i )
        {
            const float fc = clampv( static_cast<float>( m_freq ) * hs[i], 30.0f, static_cast<float>( nyq ) );
            const float bw = fc / q, gain = 1.0f / std::sqrt( hs[i] );
            m_ghostL[i].set( fc, bw, gain, static_cast<float>( m_sr ) );
            m_ghostR[i].m_b0 = m_ghostL[i].m_b0;
            m_ghostR[i].m_a1 = m_ghostL[i].m_a1;
            m_ghostR[i].m_a2 = m_ghostL[i].m_a2;
            expected += gain * gain * Pi * bw /
                        ( 2.0f * static_cast<float>( m_sr ) );   // noise power through a resonator of bandwidth bw
        }
        const float expectedRms = std::sqrt( expected / 3.0f );   // bipolar noise has power 1/3
        m_ghostGain = p.m_air * std::min( 0.05f / std::max( expectedRms, 1e-4f ), 60.0f );
        m_airGain = 0.0f;
    }
    else if ( p.m_air > 0.0f )
    {
        const float ad = m_airDrift.update( dt, driftRate * 0.7f, m_rng );
        const float fc = clampv( static_cast<float>( m_freq ) * p.m_airColor * std::pow( 2.0f, 0.5f * ad ), 40.0f,
                                 static_cast<float>( nyq ) );
        const float q = clampv( p.m_airQ, 1.0f, 40.0f );
        m_airL.setQ( fc, q, static_cast<float>( m_sr ) );
        m_airR.copyCoefficients( m_airL );
        // Normalise the expected band-passed noise level so `air` reads as a level.
        const float expectedRms = std::sqrt( ( 1.0f / 3.0f ) * Pi * fc / ( q * static_cast<float>( m_sr ) ) );
        m_airGain = p.m_air * std::min( 0.05f / std::max( expectedRms, 1e-4f ), 40.0f );
    }
    else
    {
        m_airGain = 0.0f;
    }
}

void Voice::render( float *nearL, float *nearR, float *farL, float *farR, int n, const VoiceParams &p, const float *fm,
                    const float *couple )
{
    // The coupling is capped low and enters before the filter, so what comes back is not the
    // other voices but this voice's answer to them.
    const float sympathy = couple != nullptr ? clampv( p.m_sympathy, 0.0f, 1.0f ) * 0.12f : 0.0f;
    const bool  doFm = fm != nullptr && p.m_fmAmount > 0.0f;
    const float fmScale = p.m_fmAmount * 3.0f;   // radians at the fundamental per unit of feedback signal
    m_fmHpCoef = 1.0f - TwoPi * 10.0f / static_cast<float>( m_sr );
    if ( !doFm )
    {
        m_fmHpXL = m_fmHpXR = m_fmHpYL = m_fmHpYR = 0.0f;
    }
    float *fdl = m_farDlyL.data();
    float *fdr = m_farDlyR.data();   // the far send's gap rings (prepare() sized them)
    int    pos = 0;
    while ( pos < n && ( m_env.isActive() || m_ksOn ) )
    {
        const int len = std::min( ControlBlock, n - pos );
        control( len, p );
        // Source 1 is the strand bank only while its type is Additive; any other type renders in
        // the slot below and the bank stays silent.
        const bool bank = p.m_slot[0].m_type == SourceTypeE::ADDITIVE;
        const int  unison = bank ? clampv( p.m_unison, 1, MaxStrands ) : 0;
        // The two filters: each on or off, and with both on either in series (the z-plane hears
        // the filter) or in parallel (both hear the dry sum, Mix balances them).
        const bool fOn = p.m_filterOn && m_zModeCur != 2;
        const bool zOn = m_zModeCur != 0;
        const bool parallel = p.m_filterParallel;
        const bool air = m_airGain > 0.0f;
        // The wavefolder: not a saturation but a mirror. Past its threshold the wave is turned
        // back on itself, so a sine grows a whole family of upper partials instead of shoulders,
        // which is the metallic, industrial edge the Drive knob could never reach. It sits after
        // both filters, where the material it is folding already has a shape.
        const float foldAmt = clampv( p.m_fold, 0.0f, 1.0f );
        const bool  ghost = m_ghostGain > 0.0f;
        // Extra sources render block-wise into their own buffers, then join the strands
        // before the filter (they share filter, envelope, distance and ITD with the bank).
        float slotL[ControlBlock], slotR[ControlBlock];
        bool  anySlot = false;
        for ( int k = 0; k < Slots; ++k )
        {
            const SlotParams &sp = p.m_slot[k];
            // A slot whose role has faded out is left idle like an Off one, not rendered at nothing.
            if ( sp.m_type == SourceTypeE::OFF || ( k == 0 && bank ) || m_roleGain[k] < 1.0e-3f )
            {
                m_slots[k].render( nullptr, nullptr, 0, m_freq, sp, nullptr, nullptr, 0.0f );
                continue;
            }
            if ( !anySlot )
            {
                std::memset( slotL, 0, sizeof( float ) * static_cast<size_t>( len ) );
                std::memset( slotR, 0, sizeof( float ) * static_cast<size_t>( len ) );
                anySlot = true;
            }
            const Wavetable  *table = sp.m_table >= NumTables - 1 ? p.m_userTable : &builtinTable( sp.m_table );
            const CycleTable *cycles = sp.m_type != SourceTypeE::WAVETABLE ? nullptr
                                       : sp.m_table >= NumTables - 1       ? p.m_userCycles
                                                                           : &builtinCycleTable( sp.m_table );
            // The slot's own entrance scales its level. A copy rather than a gain on the output,
            // because a grain that has already been given its gain keeps it until it dies: fading
            // the buffer would fade grains that are half over, fading the level lets the ones
            // already sounding finish and starts the new ones quieter, which is how an entrance
            // is heard. Costs one struct copy per slot per control block.
            SlotParams entered = sp;
            entered.m_level *= m_slotGain[k] * m_roleGain[k];
            m_slots[k].render( slotL, slotR, len, m_freq * ( static_cast<double>( p.m_pitchMul ) * m_dopplerMul ),
                               entered, table, p.m_texture[k], p.m_driftRate * m_rateMul, cycles );
        }
        for ( int i = 0; i < len; ++i )
        {
            const float e = m_env.process();
            const float th = doFm ? fm[pos + i] * fmScale : 0.0f;
            float       accL = anySlot ? slotL[i] : 0.0f, accR = anySlot ? slotR[i] : 0.0f;
            if ( sympathy > 0.0f )
            {
                const float c = couple[pos + i] * sympathy * e;
                accL += c;
                accR += c;
            }
            for ( int si = 0; si < unison; ++si )
            {
                Strand      &s = m_strands[si];
                float        sum = 0.0f;
                const int    act = s.m_active;
                float       *pc = s.m_pc;
                float       *ps = s.m_ps;
                const float *rc = s.m_rc;
                const float *rs = s.m_rs;
                float       *amp = s.m_amp;
                const float *step = s.m_ampStep;
                if ( doFm )
                {
                    // Rotate by the partial's own step, then by the modulation angle h·θ (small-angle
                    // rotation by tan = t, clamped, followed by two Newton steps of 1/sqrt so the
                    // phasor stays on the unit circle within 1e-4 per sample). The clamp makes deep
                    // modulation saturate softly on the high partials instead of tearing.
                    // Written out in Simd.h since 13.09.2026: it was the dearest of the bank's
                    // three loops and the only one still scalar on every platform.
                    sum = phasorBankStepFm( pc, ps, rc, rs, amp, step, act, Hf.m_v, th, FmMaxStep );
                }
                else if ( m_spreadAmt > 0.0f )
                {
                    float sumL, sumR;
                    phasorBankStepStereo( pc, ps, rc, rs, amp, step, act, s.m_wL, s.m_wR, sumL, sumR );
                    accL += sumL * s.m_spreadGain;
                    accR += sumR * s.m_spreadGain;
                    continue;
                }
                else
                {
                    sum = phasorBankStep( pc, ps, rc, rs, amp, step, act );
                }
                accL += sum * s.m_gainL;
                accR += sum * s.m_gainR;
            }
            float outL, outR;
            // The dry sum as a Parallel z-plane hears it: in step with the filter branch, whose drive
            // runs oversampled and so 29.5 samples late while it is on -- and a circuit model 25, always.
            m_dryRingL[m_dryW] = accL;
            m_dryRingR[m_dryW] = accR;
            m_dryW = ( m_dryW + 1 ) & ( DryRing - 1 );
            if ( fOn && zOn )
            {
                m_filt.tick( accL, accR, outL, outR );
                float zl = outL, zr = outR;
                if ( parallel )
                {
                    const float lat = m_filt.latency();
                    if ( lat > 0.0f )
                    {
                        // Delayed by the filter's latency: 29.5 samples between two taps of the
                        // ring for the Drive, 25 on one for a circuit model.
                        const int   whole = static_cast<int>( lat );
                        const float frac = lat - static_cast<float>( whole );
                        const int   a = ( m_dryW - 1 - whole ) & ( DryRing - 1 ),
                                    b = ( m_dryW - 2 - whole ) & ( DryRing - 1 );
                        zl = m_dryRingL[a] + frac * ( m_dryRingL[b] - m_dryRingL[a] );
                        zr = m_dryRingR[a] + frac * ( m_dryRingR[b] - m_dryRingR[a] );
                    }
                    else
                    {
                        zl = accL;
                        zr = accR;
                    }
                }
                zRun( zl, zr );
                outL = outL * m_zDry + zl * m_zNorm * m_zWet;
                outR = outR * m_zDry + zr * m_zNorm * m_zWet;
            }
            else if ( fOn )
            {
                m_filt.tick( accL, accR, outL, outR );
            }
            else if ( zOn )
            {   // the z-plane alone (Replace, or the filter switched off)
                float zl = accL, zr = accR;
                zRun( zl, zr );
                outL = accL * m_zDry + zl * m_zNorm * m_zWet;
                outR = accR * m_zDry + zr * m_zNorm * m_zWet;
            }
            else
            {
                outL = accL;
                outR = accR;
            }
            if ( foldAmt > 0.0f )
            {
                // At four times the rate (25.09.2026): what the fold makes above Nyquist is filtered
                // away instead of folding back down as tones in no relation to the note.
                if ( !m_folding )
                {
                    m_foldOsL.reset();
                    m_foldOsR.reset();
                    m_folding = true;
                }
                auto fold = [foldAmt]( float v ) { return wavefold( v, foldAmt ); };
                outL = m_foldOsL.process( outL, fold );
                outR = m_foldOsR.process( outR, fold );
            }
            else
                m_folding = false;
            if ( air )
            {
                float lp, bp, hp;
                m_airL.tick( m_rng.bipolar(), lp, bp, hp );
                outL += m_airGain * bp;
                m_airR.tick( m_rng.bipolar(), lp, bp, hp );
                outR += m_airGain * bp;
            }
            else if ( ghost )
            {
                const float nl2 = m_rng.bipolar(), nr2 = m_rng.bipolar();
                float       gl = 0.0f, gr = 0.0f;
                for ( int k = 0; k < 6; ++k )
                {
                    gl += m_ghostL[k].tick( nl2 );
                    gr += m_ghostR[k].tick( nr2 );
                }
                outL += m_ghostGain * gl;
                outR += m_ghostGain * gr;
            }
            const float g = e * m_velocity * m_gLevel;
            outL *= g * m_panL;   // a near event's place in the field; one and one for every other note
            outR *= g * m_panR;
            if ( doFm )
            {   // self-modulation of a partial by its own output carries a DC term (J1 of the index): block it
                const float yl = outL - m_fmHpXL + m_fmHpCoef * m_fmHpYL;
                m_fmHpXL = outL;
                m_fmHpYL = yl;
                outL = yl;
                const float yr = outR - m_fmHpXR + m_fmHpCoef * m_fmHpYR;
                m_fmHpXR = outR;
                m_fmHpYR = yr;
                outR = yr;
            }
            // Interaural time difference.
            m_itdL += ( m_itdLTarget - m_itdL ) * 0.002f;
            m_itdR += ( m_itdRTarget - m_itdR ) * 0.002f;
            m_itdBufL[m_itdW & ( ItdBuffer - 1 )] = outL;
            m_itdBufR[m_itdW & ( ItdBuffer - 1 )] = outR;
            {
                const int   di = static_cast<int>( m_itdL );
                const float f = m_itdL - static_cast<float>( di );
                const float a = m_itdBufL[( m_itdW - di ) & ( ItdBuffer - 1 )],
                            b = m_itdBufL[( m_itdW - di - 1 ) & ( ItdBuffer - 1 )];
                outL = a + f * ( b - a );
            }
            {
                const int   di = static_cast<int>( m_itdR );
                const float f = m_itdR - static_cast<float>( di );
                const float a = m_itdBufR[( m_itdW - di ) & ( ItdBuffer - 1 )],
                            b = m_itdBufR[( m_itdW - di - 1 ) & ( ItdBuffer - 1 )];
                outR = a + f * ( b - a );
            }
            ++m_itdW;
            if ( m_sphereShadow )
            {
                const float yl = m_sphB0[0] * outL + m_sphB1[0] * m_sphX1[0] - m_sphA1[0] * m_sphY1[0];
                m_sphX1[0] = outL;
                m_sphY1[0] = yl;
                outL = yl;
                const float yr = m_sphB0[1] * outR + m_sphB1[1] * m_sphX1[1] - m_sphA1[1] * m_sphY1[1];
                m_sphX1[1] = outR;
                m_sphY1[1] = yr;
                outR = yr;
            }
            else
            {
                m_shadowL += m_shadowCoefL * ( outL - m_shadowL );
                outL = m_shadowL;
                m_shadowR += m_shadowCoefR * ( outR - m_shadowR );
                outR = m_shadowR;
            }
            if ( m_ildAmt > 0.0f )
            {   // the near field's low shelf, per ear
                m_ildLpL += m_ildCoef * ( outL - m_ildLpL );
                outL -= m_ildL * m_ildLpL;
                m_ildLpR += m_ildCoef * ( outR - m_ildLpR );
                outR -= m_ildR * m_ildLpR;
            }
            if ( m_proxAmt != 0.0f )
            {   // a near event's lift between 120 and 300 Hz, a function of its plane: gone on the far one
                m_proxLoL += m_proxLoCoef * ( outL - m_proxLoL );
                m_proxHiL += m_proxHiCoef * ( outL - m_proxHiL );
                m_proxLoR += m_proxLoCoef * ( outR - m_proxLoR );
                m_proxHiR += m_proxHiCoef * ( outR - m_proxHiR );
                const float near = m_proxAmt * ( 1.0f - m_distEff );
                outL += near * ( m_proxHiL - m_proxLoL );
                outR += near * ( m_proxHiR - m_proxLoR );
            }
            if ( m_pinnaAmt > 0.0f )
            {
                // The shoulder's copy comes out of the same ring the interaural delay reads, one
                // more tap further back; the pinna's notch is the state-variable filter's low plus
                // high output, mixed in by the amount. The shoulder belongs to externalisation
                // alone; the notch is shared with height.
                if ( m_extAmt > 0.0f )
                {
                    const int sL = static_cast<int>( m_itdL ) + m_shoulder,
                              sR = static_cast<int>( m_itdR ) + m_shoulder;
                    outL += 0.32f * m_extAmt * m_itdBufL[( m_itdW - sL ) & ( ItdBuffer - 1 )];
                    outR += 0.32f * m_extAmt * m_itdBufR[( m_itdW - sR ) & ( ItdBuffer - 1 )];
                }
                float lp, bp, hp;
                m_pinnaL.tick( outL, lp, bp, hp );
                outL += m_pinnaAmt * ( ( lp + hp ) - outL ) * 0.8f;
                m_pinnaR.tick( outR, lp, bp, hp );
                outR += m_pinnaAmt * ( ( lp + hp ) - outR ) * 0.8f;
                if ( m_skyGain != 0.0f )
                {
                    m_skyL.tick( outL, lp, bp, hp );
                    outL += m_skyGain * bp;
                    m_skyR.tick( outR, lp, bp, hp );
                    outR += m_skyGain * bp;
                }
            }
            if ( m_phaseOn )
            {   // first-order all-passes: y = c x + x1 - c y1, two per ear
                float *c = m_apC[0];
                float *x1 = m_apX[0];
                float *y1 = m_apY[0];
                for ( int st = 0; st < 2; ++st )
                {
                    const float y = c[st] * outL + x1[st] - c[st] * y1[st];
                    x1[st] = outL;
                    y1[st] = y;
                    outL = y;
                }
                c = m_apC[1];
                x1 = m_apX[1];
                y1 = m_apY[1];
                for ( int st = 0; st < 2; ++st )
                {
                    const float y = c[st] * outR + x1[st] - c[st] * y1[st];
                    x1[st] = outR;
                    y1[st] = y;
                    outR = y;
                }
            }
            if ( m_ksOn )
            {
                const float sk = strikeTick();
                nearL[pos + i] += sk * m_ksGainL;
                nearR[pos + i] += sk * m_ksGainR;
            }
            nearL[pos + i] += outL * m_gNear;
            nearR[pos + i] += outR * m_gNear;
            // The far send through its gap (Depth Gap): written now, read Gap x (1 - d) later, so a
            // source at the ear reaches the hall after its direct sound and one on the horizon
            // with it. Linear interpolation, the delay glided per sample as the plane breathes.
            fdl[m_farDlyW & m_farDlyMask] = outL;
            fdr[m_farDlyW & m_farDlyMask] = outR;
            m_farDly += ( m_farDlyTarget - m_farDly ) * FarDlyGlide;
            const int   dI = static_cast<int>( m_farDly );
            const float dF = m_farDly - static_cast<float>( dI );
            const float fa = fdl[( m_farDlyW - dI ) & m_farDlyMask], fb = fdl[( m_farDlyW - dI - 1 ) & m_farDlyMask];
            const float ga = fdr[( m_farDlyW - dI ) & m_farDlyMask], gb = fdr[( m_farDlyW - dI - 1 ) & m_farDlyMask];
            ++m_farDlyW;
            farL[pos + i] += ( fa + dF * ( fb - fa ) ) * m_gFar;
            farR[pos + i] += ( ga + dF * ( gb - ga ) ) * m_gFar;
        }
        pos += len;
    }
    // The Z-plane filter is the one thing here whose poles sit close enough to the circle that
    // sweeping it -- which is what an LFO on Z X or Z Y does, and what half the library does --
    // can put more energy in than comes out. See ZBiquad::guard.
    for ( int k = 0; k < m_zUsed; ++k )
    {
        m_zbL[k].guard();
        m_zbR[k].guard();
    }
    m_zModal.guard();
    if ( !m_env.isActive() ) m_note = -1;
}

}   // namespace ambient
