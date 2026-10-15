/**
 * @file Cloud.cpp
 * @brief The granular cloud: onsets, grains, the feedback loop and the tuned resonators.
 *
 * Cloud.h says what the cloud is; this is how it runs. process() works in sub-blocks of Sub
 * samples: the input and what the loop handed back one sub-block ago are written into the history,
 * spawnSub() draws the onsets (the Poisson stream, or the Hawkes flocks of Swarm), spawn() places
 * each new grain behind the write head with its length, its rate, the interval drawRatio() gives it
 * and its pan, renderGrains() runs the live grains through the vector kernel of GrainRing.h onto one
 * bus per resonator, runResonators() rings the Band or Comb resonators on those buses, and mixSub()
 * blends dry grains and resonators at Level into the output and sends the same signal through Tone,
 * a DC blocker, the ADAA saturation and the throttle on its own mean level back into the loop's
 * ring -- shifted in the spectrum first when Shift is on. setHarmony() and the two build functions
 * turn the scale and the conductor's root into the interval list the scatter draws from and the
 * notes the resonators sit on; they are called every block but rebuild only when the harmony has
 * actually moved. The constants below are the loop's gain structure, with the measurements that
 * set them.
 * @author Rene Weller
 */
#include "ambient/Cloud.h"
#include "ambient/Adaa.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ambient
{

namespace
{

constexpr double Ln1000 = 6.907755278982137;   ///< ln 1000: the fall to -60 dB, for the resonators' ring times
/**
 * @brief What the loop multiplies by at Feedback 1.
 *
 * Each channel of the cloud comes out 11.7 dB below the
 * history it reads: 0.6 over the square root of the overlap, a Hann window's three eighths of the
 * power, and an equal-power pan that gives each side half of every grain. A gain of one would
 * never sustain -- measured, 2.9 lost 2.5 dB a pass and was gone within half a minute. 4.2 puts a
 * full turn of the loop 0.8 dB above unity, which the Tone filter's losses bring back towards one
 * and the ceiling holds.
 */
constexpr float LoopGain = 4.2f;
constexpr float LoopCeiling = 0.15f;   ///< the loop's mean level at which its feedback reaches zero
constexpr float LoopDrive = 1.5f;      ///< into the saturation, and out of it again by the same factor
constexpr float ResGain = 4.0f;        ///< the resonators' makeup against the dry grains

}   // namespace

void GrainCloud::prepare( double sampleRate, uint64_t seed )
{
    m_sr = sampleRate;
    m_rng.seed( seed );
    // Eight seconds. A grain transposed up reads its history faster than it is written, and an
    // 800 ms grain two octaves up at the far end of Spray needs more than six behind the write head.
    m_cap = static_cast<int>( 8.0 * m_sr ) + 4 * Sub;
    m_hist.assign( static_cast<size_t>( 2 * m_cap ), 0.0f );
    m_w = 0;
    m_t = 0;
    m_live = 0;
    m_started = 0;
    m_tail = 0;
    m_hazard = 0.0;
    m_nextHazard = 0.0;
    m_excite = 0.0;
    m_exciteDecay = std::exp( -1.0 / ( 0.25 * m_sr ) );
    std::memset( m_fbRingL, 0, sizeof( m_fbRingL ) );
    std::memset( m_fbRingR, 0, sizeof( m_fbRingR ) );
    m_toneL = m_toneR = m_dcxL = m_dcxR = m_dcyL = m_dcyR = 0.0f;
    m_satL.reset();
    m_satR.reset();
    m_loopEnv = 0.0f;
    m_fbCur = 0.0f;
    m_dcR = 1.0f - static_cast<float>( TwoPi * 20.0 / m_sr );
    m_envC = 1.0f - std::exp( -1.0f / ( 0.1f * static_cast<float>( m_sr ) ) );
    setLoop( m_feedback, m_toneHz );
    m_shift.prepare( m_sr );
    m_shift.setSemitones( m_shiftSemis );
    m_comb.assign( static_cast<size_t>( MaxRes * 2 * CombLen ), 0.0f );
    m_combW = 0;
    m_combA = 1.0f - std::exp( -TwoPi * 6000.0f / static_cast<float>( m_sr ) );
    m_combLag = ( 1.0f - m_combA ) / m_combA;   // the one-pole's delay at low frequencies, in samples
    std::memset( m_bandU, 0, sizeof( m_bandU ) );
    std::memset( m_bandV, 0, sizeof( m_bandV ) );
    std::memset( m_combLp, 0, sizeof( m_combLp ) );
    m_resCur = m_resAmount;
    m_levelCur = m_level;
    if ( m_haveHarmony )
    {   // the resonators' angles belong to the sample rate
        buildResonators();
        for ( int k = 0; k < m_numRes; ++k ) m_resTheta[k] = m_resThetaTarget[k];
    }
}

void GrainCloud::set( float densityPerSec, float sizeMs, float pitch, float spraySec, float level )
{
    m_density = clampv( densityPerSec, 0.1f, 200.0f );
    m_size = clampv( sizeMs, 5.0f, 2000.0f );
    m_pitch = clampv( pitch, 0.0f, 1.0f );
    m_spray = clampv( spraySec, 0.0f, 3.5f );
    m_level = clampv( level, 0.0f, 2.0f );
}

void GrainCloud::setLoop( float feedback, float toneHz )
{
    m_feedback = clampv( feedback, 0.0f, 1.0f );
    m_toneHz = clampv( toneHz, 100.0f, 20000.0f );
    const float hz = std::min( m_toneHz, 0.45f * static_cast<float>( m_sr ) );
    m_toneC = 1.0f - std::exp( -TwoPi * hz / static_cast<float>( m_sr ) );
}

void GrainCloud::setShift( float semitones )
{
    const float s = clampv( semitones, -24.0f, 24.0f );
    if ( s == m_shiftSemis ) return;
    // A shift switched on starts from silence, not from what it heard the last time it ran.
    if ( m_shiftSemis == 0.0f ) m_shift.reset();
    m_shiftSemis = s;
    if ( s != 0.0f ) m_shift.setSemitones( s );
}

void GrainCloud::setScatter( float transposeSemitones, float scatter )
{
    m_transposeRatio = std::pow( 2.0, static_cast<double>( clampv( transposeSemitones, -24.0f, 24.0f ) ) / 12.0 );
    m_scatter = clampv( scatter, 0.0f, 1.0f );
}

void GrainCloud::setSwarm( float swarm )
{
    m_swarm = clampv( swarm, 0.0f, 1.0f );
}

void GrainCloud::setResonators( float amount, int mode, int notes, float decaySeconds )
{
    m_resAmount = clampv( amount, 0.0f, 1.0f );
    mode = clampv( mode, 0, 1 );
    if ( mode != m_resMode )
    {   // the other kind starts from silence, not from the last one's state
        m_resMode = mode;
        std::memset( m_bandU, 0, sizeof( m_bandU ) );
        std::memset( m_bandV, 0, sizeof( m_bandV ) );
        std::memset( m_combLp, 0, sizeof( m_combLp ) );
        std::fill( m_comb.begin(), m_comb.end(), 0.0f );
    }
    m_resDecay = clampv( decaySeconds, 0.05f, 30.0f );
    notes = clampv( notes, 0, 3 );
    if ( notes != m_resNotes )
    {
        m_resNotes = notes;
        if ( m_haveHarmony ) buildResonators();
    }
}

void GrainCloud::setHarmony( const FixedScale &scale, double tonicHz, double rootHz )
{
    if ( !( tonicHz > 1.0 ) || !( rootHz > 1.0 ) || !std::isfinite( tonicHz ) || !std::isfinite( rootHz ) ) return;
    const int count = clampv( scale.m_count, 1, FixedScale::Max );
    double    sig = count * 1000.003 + scale.m_period;
    for ( int k = 0; k < count; ++k ) sig += scale.m_ratios[k] * ( k + 1.618 );
    if ( m_haveHarmony && sig == m_scaleSig && std::fabs( tonicHz / m_tonicHz - 1.0 ) < 1.0e-7 &&
         std::fabs( rootHz / m_rootHz - 1.0 ) < 1.0e-7 )
        return;
    m_scaleSig = sig;
    m_tonicHz = tonicHz;
    m_rootHz = rootHz;
    m_count = count;
    m_period = ( scale.m_period > 1.0001 && std::isfinite( scale.m_period ) ) ? scale.m_period : 2.0;
    // A Scala file can carry a degree of zero, which the tuning code answers with a frequency of
    // zero by design; here it would be a logarithm of zero. Such a degree is taken as the tonic.
    for ( int k = 0; k < count; ++k )
        m_ratios[k] = ( scale.m_ratios[k] > 0.0 && std::isfinite( scale.m_ratios[k] ) ) ? scale.m_ratios[k] : 1.0;
    const bool first = !m_haveHarmony;
    m_haveHarmony = true;
    buildIntervals();
    buildResonators();
    if ( first )
        for ( int k = 0; k < m_numRes; ++k ) m_resTheta[k] = m_resThetaTarget[k];
}

void GrainCloud::buildIntervals()
{
    const double P = m_period;
    // The conductor's root as a degree of the scale: its ratio to the tonic, folded into one
    // period and matched to the nearest degree -- the end of the period counted as the tonic again.
    double r = m_rootHz / m_tonicHz;
    for ( int guard = 0; guard < 64 && r >= P; ++guard ) r /= P;
    for ( int guard = 0; guard < 64 && r < 1.0; ++guard ) r *= P;
    int    d = 0;
    double best = 1.0e9;
    for ( int k = 0; k < m_count; ++k )
    {
        const double e =
            std::min( std::fabs( std::log( r / m_ratios[k] ) ), std::fabs( std::log( r / ( m_ratios[k] * P ) ) ) );
        if ( e < best )
        {
            best = e;
            d = k;
        }
    }
    // The scale rotated to begin on that degree: the intervals the root has above it within a period.
    for ( int j = 0; j < m_count; ++j )
    {
        const int k = ( d + j ) % m_count;
        double    q = m_ratios[k] / m_ratios[d];
        if ( d + j >= m_count ) q *= P;
        m_rot[j] = q;
    }
    // Every interval within an octave either way, the unison left out -- it is what a grain
    // without scatter plays -- ordered from the most consonant outwards, and the nearer of two
    // equally consonant intervals first.
    struct Cand
    {
        double m_v, m_c, m_dist;
    };
    Cand cand[MaxIv];
    int  nc = 0;
    for ( int o = -1; o <= 1; ++o )
    {
        const double po = o < 0 ? 1.0 / P : ( o > 0 ? P : 1.0 );
        for ( int j = 0; j < m_count && nc < MaxIv; ++j )
        {
            if ( o == 0 && j == 0 ) continue;
            const double v = m_rot[j] * po;
            if ( v < 0.499 || v > 2.001 ) continue;
            cand[nc++] = { v, intervalConsonance( v ), std::fabs( std::log2( v ) ) };
        }
    }
    std::stable_sort( cand, cand + nc, []( const Cand &a, const Cand &b )
                      { return a.m_c != b.m_c ? a.m_c > b.m_c : a.m_dist < b.m_dist; } );
    m_numIv = nc;
    for ( int k = 0; k < nc; ++k ) m_iv[k] = static_cast<float>( cand[k].m_v );
}

void GrainCloud::buildResonators()
{
    const double P = m_period;
    // The two octaves above C3: low enough to be a chord under the cloud, high enough that a grain
    // of a hundred milliseconds holds a dozen periods of the lowest note.
    double base = m_rootHz;
    for ( int guard = 0; guard < 64 && base >= 261.6255653; ++guard ) base *= 0.5;
    for ( int guard = 0; guard < 64 && base < 130.8127827; ++guard ) base *= 2.0;
    // The interval of the scale nearest to a or to b, measured from the root.
    const auto nearest = [&]( double a, double b )
    {
        if ( m_count < 2 ) return a;
        int    bj = 1;
        double be = 1.0e9;
        for ( int j = 1; j < m_count; ++j )
        {
            const double e = std::min( std::fabs( std::log( m_rot[j] / a ) ), std::fabs( std::log( m_rot[j] / b ) ) );
            if ( e < be )
            {
                be = e;
                bj = j;
            }
        }
        return m_rot[bj];
    };
    double hz[MaxRes];
    int    n = 0;
    switch ( m_resNotes )
    {
    case 0:
    {   // the scale: every degree in both octaves, the least consonant left out beyond twelve
        struct Note
        {
            double m_hz, m_c;
        };
        Note list[2 * FixedScale::Max];
        int  nl = 0;
        for ( int o = 0; o < 2; ++o )
            for ( int j = 0; j < m_count; ++j )
                list[nl++] = { base * m_rot[j] * ( o != 0 ? P : 1.0 ), intervalConsonance( m_rot[j] ) };
        std::stable_sort( list, list + nl, []( const Note &a, const Note &b ) { return a.m_c > b.m_c; } );
        n = std::min( nl, static_cast<int>( MaxRes ) );
        std::sort( list, list + n, []( const Note &a, const Note &b ) { return a.m_hz < b.m_hz; } );
        for ( int k = 0; k < n; ++k ) hz[k] = list[k].m_hz;
        break;
    }
    case 1:
    {   // the chord on the root: the third the scale has nearer (major or minor) and the fifth
        const double third = nearest( 1.25, 1.2 ), fifth = nearest( 1.5, 1.5 );
        const double c[6] = { 1.0, third, fifth, 2.0, 2.0 * third, 2.0 * fifth };
        for ( double v : c ) hz[n++] = base * v;
        break;
    }
    case 2:
    {
        const double fifth = nearest( 1.5, 1.5 );
        const double c[4] = { 1.0, fifth, 2.0, 2.0 * fifth };
        for ( double v : c ) hz[n++] = base * v;
        break;
    }
    default:
    {
        const double c[3] = { 1.0, 2.0, 4.0 };
        for ( double v : c ) hz[n++] = base * v;
        break;
    }
    }
    const double top = 0.4 * m_sr;
    for ( int k = 0; k < n; ++k )
    {
        const double f = std::min( hz[k], top );
        m_resHz[k] = static_cast<float>( f );
        m_resThetaTarget[k] = static_cast<float>( 2.0 * 3.141592653589793 * f / m_sr );
    }
    for ( int k = m_numRes; k < n; ++k ) m_resTheta[k] = m_resThetaTarget[k];   // a new resonator starts on its note
    m_numRes = n;
}

double GrainCloud::drawRatio()
{
    if ( m_scatter <= 0.0f || m_numIv == 0 ) return 1.0;
    const float s = m_scatter;
    double      ratio = 1.0;
    // Up to a quarter, more and more grains leave the unison; up to a half, the intervals they may
    // take open from the top of the list -- the most consonant -- until every one of them is in.
    if ( m_rng.uniform() < std::min( 1.0f, 4.0f * s ) )
    {
        const int want = static_cast<int>( std::ceil( std::min( 1.0f, 2.0f * s ) * static_cast<float>( m_numIv ) ) );
        const int open = std::clamp( want, std::min( 2, m_numIv ), m_numIv );
        ratio = static_cast<double>( m_iv[m_rng.below( open )] );
    }
    // Beyond a half the grains begin to leave the scale: a growing share of them is bent by up to
    // an octave either way, and at 1 every one of them is.
    if ( s > 0.5f )
    {
        const float q = 2.0f * ( s - 0.5f );
        if ( m_rng.uniform() < q ) ratio *= std::pow( 2.0, static_cast<double>( q * m_rng.bipolar() ) );
    }
    return ratio;
}

void GrainCloud::spawn( int w0, int offset )
{
    if ( m_live >= MaxGrains ) return;
    static const double Rates[4] = { 2.0, 0.5, 1.5, 4.0 };
    double len = static_cast<double>( m_size ) * ( 0.7 + 0.6 * static_cast<double>( m_rng.uniform() ) ) * m_sr / 1000.0;
    double rate = 1.0 + 0.02 * static_cast<double>( m_rng.bipolar() );
    if ( m_rng.uniform() < m_pitch ) rate = Rates[m_rng.below( m_rng.uniform() < 0.7f ? 2 : 4 )];
    rate *= m_transposeRatio * drawRatio();
    rate = std::clamp( rate, 0.125, 8.0 );
    const double spray = static_cast<double>( m_spray ) * static_cast<double>( m_rng.uniform() ) * m_sr;
    // A grain may never read what has not been written yet: it starts far enough behind the write
    // head that it is still behind it when it ends, even read faster than the head moves. Where the
    // history is not long enough for that, the grain is shortened rather than dropped.
    const double limit = static_cast<double>( m_cap ) - 2.0 * Sub - 16.0;
    const double speed = std::max( rate, 1.0 );
    double       behind = len * speed + spray + 4.0;
    if ( behind > limit )
    {
        len = ( limit - spray - 4.0 ) / speed;
        if ( len < 32.0 ) return;
        behind = len * speed + spray + 4.0;
    }
    const float overlap = std::max( m_density * m_size * 0.001f, 1.0f );
    const float gain = 0.6f / std::sqrt( overlap );   // roughly constant loudness
    const float pan = m_rng.bipolar();
    const float angle = ( pan + 1.0f ) * 0.25f * Pi;
    RingGrain  &g = m_grains[m_live++];
    double      pos = static_cast<double>( w0 + offset ) - behind;
    while ( pos < 0.0 ) pos += m_cap;
    while ( pos >= m_cap ) pos -= m_cap;
    g.m_pos = pos;
    g.m_rate = rate;
    g.m_len = static_cast<int>( len );
    g.m_age = 0;
    g.m_start = offset;
    g.m_gl = std::cos( angle ) * gain;
    g.m_gr = std::sin( angle ) * gain;
    g.m_wc = 1.0f;
    g.m_ws = 0.0f;
    phasorFrom( 1.0 / len, g.m_rc, g.m_rs );
    g.m_bus = ( m_routing && m_numRes > 1 ) ? m_rng.below( m_numRes ) : 0;
    ++m_started;
}

void GrainCloud::spawnSub( int w0, int m )
{
    // Onsets by time rescaling: the hazard -- the intensity integrated over time -- is compared
    // against an exponential draw, which at a constant intensity is exactly the Poisson stream the
    // cloud always had. The intensity is the base rate plus an excitation that every onset raises
    // and that decays with a quarter of a second (Hawkes 1971). The base rate gives up the share
    // of onsets the excitation makes, so the mean density stays what the knob says.
    const double nstar = 0.9 * static_cast<double>( m_swarm );   // the branching ratio: offspring per grain
    const double mu = static_cast<double>( m_density ) * ( 1.0 - nstar );
    const double jump = nstar / 0.25;   // so the kernel integrates to n*
    const double dt = 1.0 / m_sr;
    for ( int i = 0; i < m; ++i )
    {
        m_hazard += ( mu + m_excite ) * dt;
        m_excite *= m_exciteDecay;
        if ( m_hazard >= m_nextHazard )
        {
            m_hazard = 0.0;
            m_nextHazard = -std::log( 1.0 - static_cast<double>( m_rng.uniform() ) + 1.0e-12 );
            spawn( w0, i );
            m_excite += jump;
        }
    }
}

void GrainCloud::renderGrains( int m )
{
    const float *h = m_hist.data();
    for ( int c = 0; c < m_live; )
    {
        RingGrain &g = m_grains[c];
        {   // keep the window phasor on the unit circle (a grain can run for 96000 samples)
            const float r2 = g.m_wc * g.m_wc + g.m_ws * g.m_ws, fix = 1.5f - 0.5f * r2;
            g.m_wc *= fix;
            g.m_ws *= fix;
        }
        const int begin = g.m_start < m ? g.m_start : m;
        g.m_start = 0;
        const int count = std::min( m - begin, g.m_len - g.m_age );
        if ( count > 0 )
        {
            const int b = m_routing ? ( g.m_bus < m_numRes ? g.m_bus : g.m_bus % m_numRes ) : 0;
            renderRingGrain( h, m_cap, 2, g, m_busL[b] + begin, m_busR[b] + begin, count );
            g.m_age += count;
        }
        // Dead: the last live grain takes its place, and `c` stays where it is, because what now
        // sits there has not been rendered yet.
        if ( g.m_age >= g.m_len )
            m_grains[c] = m_grains[--m_live];
        else
            ++c;
    }
}

void GrainCloud::runResonators( int m, float *wetL, float *wetR )
{
    const float glide = 1.0f - std::exp( -static_cast<float>( m ) / ( 0.12f * static_cast<float>( m_sr ) ) );
    if ( m_resMode == 0 )
    {
        // Mathews and Smith's phasor filter: the state is a point in the plane, turned by the
        // note's angle and shrunk by the decay on every sample, with the input added to its real
        // part. A rotation keeps the point's distance whatever the angle is, so a resonator whose
        // note glides keeps its level. The imaginary part, scaled so the response peaks at one.
        const float rho = static_cast<float>( std::exp( -Ln1000 / ( static_cast<double>( m_resDecay ) * m_sr ) ) );
        const float norm = 2.0f * ( 1.0f - rho ) / rho;
        for ( int k = 0; k < m_numRes; ++k )
        {
            m_resTheta[k] += ( m_resThetaTarget[k] - m_resTheta[k] ) * glide;
            const float  c = rho * std::cos( m_resTheta[k] ), s = rho * std::sin( m_resTheta[k] );
            const float *xL = m_busL[k];
            const float *xR = m_busR[k];
            float        uL = m_bandU[k][0], vL = m_bandV[k][0], uR = m_bandU[k][1], vR = m_bandV[k][1];
            for ( int i = 0; i < m; ++i )
            {
                const float nuL = xL[i] + c * uL - s * vL;
                vL = s * uL + c * vL;
                uL = nuL;
                const float nuR = xR[i] + c * uR - s * vR;
                vR = s * uR + c * vR;
                uR = nuR;
                wetL[i] += vL * norm;
                wetR[i] += vR * norm;
            }
            m_bandU[k][0] = uL;
            m_bandV[k][0] = vL;
            m_bandU[k][1] = uR;
            m_bandV[k][1] = vR;
        }
    }
    else
    {
        // A feedback comb per note. The loop's delay is the note's period less the lag of the
        // one-pole that darkens it, read between two samples; its gain is what loses 60 dB in the
        // ring time; the output is scaled so the comb's teeth peak at one.
        constexpr int mask = CombLen - 1;
        for ( int k = 0; k < m_numRes; ++k )
        {
            m_resTheta[k] += ( m_resThetaTarget[k] - m_resTheta[k] ) * glide;
            const float period = 6.2831853f / std::max( m_resTheta[k], 1.0e-4f );
            const float delay = clampv( period - m_combLag, 2.0f, static_cast<float>( CombLen - 3 ) );
            const int   di = static_cast<int>( delay );
            const float fr = delay - static_cast<float>( di );
            const float gain = static_cast<float>(
                std::exp( -Ln1000 * static_cast<double>( delay ) / ( static_cast<double>( m_resDecay ) * m_sr ) ) );
            const float norm = 1.0f - gain;
            float      *bL = m_comb.data() + static_cast<size_t>( k ) * 2 * CombLen;
            float      *bR = bL + CombLen;
            float       lpL = m_combLp[k][0], lpR = m_combLp[k][1];
            int         w = m_combW;
            for ( int i = 0; i < m; ++i )
            {
                const int   r0 = ( w - di ) & mask, r1 = ( w - di - 1 ) & mask;
                const float dL = bL[r0] + fr * ( bL[r1] - bL[r0] );
                const float dR = bR[r0] + fr * ( bR[r1] - bR[r0] );
                lpL += m_combA * ( dL - lpL );
                lpR += m_combA * ( dR - lpR );
                const float yL = m_busL[k][i] + gain * lpL;
                const float yR = m_busR[k][i] + gain * lpR;
                bL[w] = yL;
                bR[w] = yR;
                wetL[i] += yL * norm;
                wetR[i] += yR * norm;
                w = ( w + 1 ) & mask;
            }
            m_combLp[k][0] = lpL;
            m_combLp[k][1] = lpR;
        }
        m_combW = ( m_combW + m ) & mask;
    }
}

void GrainCloud::mixSub( int m, float *outL, float *outR )
{
    float dryL[Sub], dryR[Sub];
    std::memcpy( dryL, m_busL[0], sizeof( float ) * static_cast<size_t>( m ) );
    std::memcpy( dryR, m_busR[0], sizeof( float ) * static_cast<size_t>( m ) );
    if ( m_routing )
        for ( int k = 1; k < m_numRes; ++k )
            for ( int i = 0; i < m; ++i )
            {
                dryL[i] += m_busL[k][i];
                dryR[i] += m_busR[k][i];
            }
    float      wetL[Sub] = {}, wetR[Sub] = {};
    const bool resOn = m_numRes > 0 && ( m_resAmount > 0.0f || m_resCur > 1.0e-4f );
    if ( resOn ) runResonators( m, wetL, wetR );

    // The three amounts glide across the sub-block rather than stepping at its edge.
    const float sm = 1.0f - std::exp( -static_cast<float>( m ) / ( 0.03f * static_cast<float>( m_sr ) ) );
    const float res0 = m_resCur, res1 = m_resCur + ( m_resAmount - m_resCur ) * sm;
    const float lev0 = m_levelCur, lev1 = m_levelCur + ( m_level - m_levelCur ) * sm;
    const float fbTarget = m_feedback * LoopGain;
    const float fb0 = m_fbCur, fb1 = m_fbCur + ( fbTarget - m_fbCur ) * sm;
    m_resCur = res1;
    m_levelCur = lev1;
    m_fbCur = fb1;
    const bool  loop = fb0 > 1.0e-6f || fb1 > 1.0e-6f;
    const float inv = 1.0f / static_cast<float>( m );
    float       peak = 0.0f;
    float       loopL[Sub], loopR[Sub], loopG[Sub];
    for ( int i = 0; i < m; ++i )
    {
        const float a = static_cast<float>( i + 1 ) * inv;
        const float res = res0 + ( res1 - res0 ) * a;
        const float lev = lev0 + ( lev1 - lev0 ) * a;
        const float yL = dryL[i] + ( wetL[i] * ResGain - dryL[i] ) * res;
        const float yR = dryR[i] + ( wetR[i] * ResGain - dryR[i] ) * res;
        outL[i] += yL * lev;
        outR[i] += yR * lev;
        peak = std::max( peak, std::fabs( yL ) + std::fabs( yR ) );
        if ( loop )
        {
            // Tone, a DC blocker (a transposed grain can carry an offset and a loop would stack
            // it), the saturation, and the throttle on the loop's own mean level.
            m_toneL += m_toneC * ( yL - m_toneL );
            m_toneR += m_toneC * ( yR - m_toneR );
            const float hL = m_toneL - m_dcxL + m_dcR * m_dcyL;
            m_dcxL = m_toneL;
            m_dcyL = hL;
            const float hR = m_toneR - m_dcxR + m_dcR * m_dcyR;
            m_dcxR = m_toneR;
            m_dcyR = hR;
            loopL[i] = m_satL( hL * LoopDrive ) / LoopDrive;
            loopR[i] = m_satR( hR * LoopDrive ) / LoopDrive;
            m_loopEnv += m_envC * ( 0.5f * ( std::fabs( loopL[i] ) + std::fabs( loopR[i] ) ) - m_loopEnv );
            const float reg = clampv( ( LoopCeiling - m_loopEnv ) / LoopCeiling, 0.0f, 1.0f );
            loopG[i] = ( fb0 + ( fb1 - fb0 ) * a ) * reg;
        }
    }
    if ( loop )
    {
        // Shifted on the way round: every pass of the loop an interval further up or down.
        if ( m_shiftSemis != 0.0f ) m_shift.process( loopL, loopR, loopL, loopR, m );
        for ( int i = 0; i < m; ++i )
        {
            const int fi = static_cast<int>( ( m_t + i ) & ( FbLen - 1 ) );
            m_fbRingL[fi] = loopL[i] * loopG[i];
            m_fbRingR[fi] = loopR[i] * loopG[i];
        }
    }
    else
    {
        for ( int i = 0; i < m; ++i )
        {
            const int fi = static_cast<int>( ( m_t + i ) & ( FbLen - 1 ) );
            m_fbRingL[fi] = 0.0f;
            m_fbRingR[fi] = 0.0f;
        }
        m_toneL = m_toneR = m_dcxL = m_dcxR = m_dcyL = m_dcyR = 0.0f;
        m_satL.reset();
        m_satR.reset();
        m_loopEnv = 0.0f;
    }
    m_tail = peak > 1.0e-7f ? static_cast<int>( 0.25 * m_sr ) : std::max( 0, m_tail - m );
}

void GrainCloud::process( const float *inL, const float *inR, float *outL, float *outR, int n )
{
    float *h = m_hist.data();
    for ( int p = 0; p < n; )
    {
        const int m = std::min( Sub, n - p );
        const int w0 = m_w;
        // The history: the input, and what the loop handed over one sub-block ago.
        const long long back = m_t - Sub;
        for ( int i = 0; i < m; ++i )
        {
            const int fi = static_cast<int>( ( back + i ) & ( FbLen - 1 ) );
            h[2 * m_w] = inL[p + i] + m_fbRingL[fi];
            h[2 * m_w + 1] = inR[p + i] + m_fbRingR[fi];
            if ( ++m_w == m_cap ) m_w = 0;
        }
        m_routing = m_numRes > 1 && ( m_resAmount > 0.0f || m_resCur > 1.0e-4f );
        spawnSub( w0, m );
        const int buses = m_routing ? m_numRes : 1;
        for ( int k = 0; k < buses; ++k )
        {
            std::memset( m_busL[k], 0, sizeof( float ) * static_cast<size_t>( m ) );
            std::memset( m_busR[k], 0, sizeof( float ) * static_cast<size_t>( m ) );
        }
        renderGrains( m );
        mixSub( m, outL + p, outR + p );
        m_t += m;
        p += m;
    }
}

}   // namespace ambient
