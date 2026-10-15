/**
 * @file Memory.cpp
 * @brief The drifting sound memory of Memory.h: the pool, the lines, the exchange and the grains.
 *
 * Memory.h says what the device is and where each of its knobs comes from; this file is how it is
 * done, in the order the audio thread meets it. prepare() lays the 90-second pool out, designs the
 * reading interpolator (an eight-tap Kaiser-windowed sinc in 256 phases) and the fingerprint's window
 * and bin-to-pitch-class map, and allocates everything -- nothing is allocated after it. The setters
 * only clamp and store; the work of a changed setting is done in stepControl(), once per control
 * block of Sub samples, where the number of lines is re-laid, the lengths are found as primes, the
 * gains follow Hold and Age, the Lorenz attractor is integrated, the tape speed glides, and every
 * line's slide, pan and anti-alias filter are advanced. process() then runs the lines sample by
 * sample: the sinc read, the scattering all-pass, the absorption, the DC blocker, the butterfly
 * exchange, the per-line limiter and saturation, and the splatting write head (tapeWrite()). Recall
 * runs first in each control block, so its grains read tape this block has not yet written over.
 *
 * The fingerprints that Seek listens for are computed one per control block at most (analyse()),
 * from a queue of pool segments the write heads have just finished, so no block ever pays for more
 * than one chroma FFT. The anonymous namespace holds the constants that give the lines their
 * incommensurable periods and the small number-theoretic helpers the layout needs.
 * @author Rene Weller
 */
#include "ambient/Memory.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace ambient
{

namespace
{

constexpr double Ln1000 =
    6.907755278982137;   ///< ln 1000, the fall to -60 dB: turns a T60 into a gain per pass in gains()
constexpr double Tau = 6.283185307179586;   ///< two pi in double, for the slide, pan and stage phases
constexpr float  Limit = 0.7f;              ///< each line's loop is held under this level
constexpr int    PoolMaxCells = 8640000;    ///< 90 s at 96 kHz, 45 s at 192 kHz
constexpr int    QueueMask = 511;           ///< the fingerprint queue holds 512 segment indices and wraps by masking
/**
 * @brief The scattering all-passes, in milliseconds: mutually prime, so at full Blur an echo is spread over a
 * tenth to a sixth of a second on every pass, and no two lines spread it alike.
 */
constexpr float ApMs[Memory::MaxLines] = { 41.1f, 53.3f, 67.9f, 79.3f, 97.1f, 113.3f, 131.9f, 149.3f };
/**
 * @brief The slides and the pans: periods between 19 and 83 seconds, none a multiple of another.
 *
 * This table is the slides: the rate, in Hz, of the sine each line's read head rides on before the
 * attractor bends it (stepControl()). PanHz and StageHz complete the set of periods.
 */
constexpr double DriftHz[Memory::MaxLines] = { 1.0 / 23.0, 1.0 / 29.0, 1.0 / 31.0, 1.0 / 37.0,
                                               1.0 / 41.0, 1.0 / 47.0, 1.0 / 53.0, 1.0 / 61.0 };
/** @brief The pans: the rate, in Hz, of each line's wander across the stereo field (see DriftHz). */
constexpr double PanHz[Memory::MaxLines] = { 1.0 / 67.0, 1.0 / 59.0, 1.0 / 43.0, 1.0 / 71.0,
                                             1.0 / 49.0, 1.0 / 79.0, 1.0 / 57.0, 1.0 / 83.0 };
/** @brief The rates, in Hz, of the sines on which the three stages of the exchange lean off the common angle. */
constexpr double StageHz[3] = { 1.0 / 19.0, 1.0 / 27.0, 1.0 / 35.0 };

/**
 * @brief The modified Bessel function of the first kind, order zero: the Kaiser window's shape.
 *
 * The power series, summed until a term falls below 1e-16 of the sum; for the arguments a window of
 * beta 5 asks for that is well within the forty terms allowed.
 * @param[in] x  the argument, non-negative where it is used
 * @return I0(x), which is 1 at x = 0 and grows like exp(x)
 */
double besselI0( double x )
{
    double sum = 1.0, term = 1.0;
    for ( int k = 1; k < 40; ++k )
    {
        const double h = x / ( 2.0 * k );
        term *= h * h;
        sum += term;
        if ( term < 1.0e-16 * sum ) break;
    }
    return sum;
}

/**
 * @brief The smallest power of two not below @p n: the all-pass rings are sized so that a mask can wrap them.
 * @param[in] n  the length that has to fit; 1 or less gives 1
 * @return the power of two, at least @p n
 */
int pow2At( int n )
{
    int p = 1;
    while ( p < n ) p <<= 1;
    return p;
}

/**
 * @brief Whether @p n is prime, by trial division up to its square root.
 * @param[in] n  any integer; anything below 2 is not prime
 * @return true for a prime
 */
bool isPrime( int n )
{
    if ( n < 2 ) return false;
    if ( n % 2 == 0 ) return n == 2;
    for ( int d = 3; d * d <= n; d += 2 )
        if ( n % d == 0 ) return false;
    return true;
}

/**
 * @brief The largest prime not above @p n: every line's length is one, so no two lines share a period.
 *
 * Counts down from @p n testing each candidate, which near two million is a few thousand divisions
 * per line; lengths() therefore asks only when the size has really moved.
 * @param[in] n  the upper bound, in cells
 * @return the prime, or 2 when @p n is below 3
 */
int primeAtMost( int n )
{
    for ( int x = n; x > 2; --x )
        if ( isPrime( x ) ) return x;
    return 2;
}

/**
 * @brief Each line's place in the stereo field before it drifts: the left input's lines on the left, the
 * right's on the right, the longest furthest out.
 * @param[in] k      the line, 0 .. lines - 1; even lines take the left input, odd ones the right
 * @param[in] lines  how many lines are laid out, 2, 4 or 8
 * @return the pan, -1 (left) .. 1 (right), between +-0.15 and +-0.85
 */
float panBase( int k, int lines )
{
    const float side = ( k & 1 ) ? 1.0f : -1.0f;
    const float half = static_cast<float>( std::max( 1, lines / 2 ) );
    return side * ( 0.85f - 0.7f * static_cast<float>( k / 2 ) / half );
}

}   // namespace

void Memory::prepare( double sampleRate, uint64_t seed )
{
    m_sr = sampleRate;
    m_rng.seed( seed );
    m_poolCells = std::min( PoolMaxCells, static_cast<int>( 90.0 * m_sr ) );
    m_pool.assign( static_cast<size_t>( m_poolCells ), 0.0f );
    m_apSize = pow2At( static_cast<int>( 0.2 * m_sr ) + 16 );
    m_apMask = m_apSize - 1;
    m_ap.assign( static_cast<size_t>( MaxLines ) * static_cast<size_t>( m_apSize ), 0.0f );
    m_apW = 0;
    for ( int k = 0; k < MaxLines; ++k ) m_apLen[k] = std::max( 1, static_cast<int>( ApMs[k] * m_sr / 1000.0 ) );
    // The reading interpolator: an eight-tap sinc under a Kaiser window (beta 5), in 256 phases with
    // a straight line between two, every row summing to one. At a whole cell it is exactly that cell,
    // so a head that does not slide reads its line without touching it.
    m_sinc.assign( static_cast<size_t>( ( Phases + 1 ) * Taps ), 0.0f );
    {
        const double beta = 5.0, norm = besselI0( beta );
        for ( int ph = 0; ph <= Phases; ++ph )
        {
            const double frac = static_cast<double>( ph ) / Phases;
            double       row[Taps], sum = 0.0;
            for ( int j = 0; j < Taps; ++j )
            {
                const double t = static_cast<double>( j - 3 ) - frac;   // tap j reads cell i1 - 3 + j
                const double nearest = std::round( t );
                double       sinc;
                if ( std::fabs( t - nearest ) < 1.0e-9 )
                    sinc = nearest == 0.0 ? 1.0 : 0.0;
                else
                    sinc = std::sin( 3.141592653589793 * t ) / ( 3.141592653589793 * t );
                const double u = t / 4.0;
                const double win = std::fabs( u ) >= 1.0 ? 0.0 : besselI0( beta * std::sqrt( 1.0 - u * u ) ) / norm;
                row[j] = sinc * win;
                sum += row[j];
            }
            for ( int j = 0; j < Taps; ++j )
                m_sinc[static_cast<size_t>( ph * Taps + j )] = static_cast<float>( row[j] / sum );
        }
    }
    const int segs = m_poolCells / Seg + 2;
    m_chroma.assign( static_cast<size_t>( segs ) * 12, 0.0f );
    m_segRms.assign( static_cast<size_t>( segs ), 0.0f );
    m_queue.assign( static_cast<size_t>( QueueMask + 1 ), 0 );
    m_qHead = m_qTail = 0;
    m_fftRe.assign( Seg, 0.0f );
    m_fftIm.assign( Seg, 0.0f );
    m_window.resize( Seg );
    for ( int i = 0; i < Seg; ++i )
        m_window[static_cast<size_t>( i )] = static_cast<float>( 0.5 - 0.5 * std::cos( Tau * i / Seg ) );
    // Which pitch class each bin of the fingerprint's spectrum belongs to, from 55 Hz (below it a
    // bin is wider than a semitone) to 5 kHz (above it there are partials, not notes).
    m_binPc.assign( Seg / 2, -1 );
    for ( int b = 1; b < Seg / 2; ++b )
    {
        const double hz = b * m_sr / Seg;
        if ( hz < 55.0 || hz > 5000.0 ) continue;
        const long pc = std::lround( 12.0 * std::log2( hz / 16.351597831287414 ) );
        m_binPc[static_cast<size_t>( b )] = static_cast<signed char>( ( ( pc % 12 ) + 12 ) % 12 );
    }
    m_inBuf.assign( Seg, 0.0f );
    m_inPos = 0;
    std::memset( m_inputChroma, 0, sizeof( m_inputChroma ) );
    m_inputLevel = 0.0f;
    m_envAtk = 1.0f - std::exp( -1.0f / ( 0.01f * static_cast<float>( m_sr ) ) );
    m_envRel = 1.0f - std::exp( -1.0f / ( 0.3f * static_cast<float>( m_sr ) ) );
    m_limAtk = 1.0f - std::exp( -1.0f / ( 0.001f * static_cast<float>( m_sr ) ) );
    m_limRel = 1.0f - std::exp( -1.0f / ( 0.4f * static_cast<float>( m_sr ) ) );
    m_dcR = 1.0f - static_cast<float>( Tau * 3.0 / m_sr );   // the loop's DC blocker: 3 Hz, below anything a line holds
    m_live = 0;
    m_started = 0;
    m_hazard = 0.0;
    m_nextHazard = 0.0;
    m_lorenz[0] = 0.1;
    m_lorenz[1] = 0.0;
    m_lorenz[2] = 20.0;
    for ( int s = 0; s < 3; ++s ) m_stagePh[s] = 0.37 * s;
    for ( int k = 0; k < MaxLines; ++k )
    {
        m_line[k] = Line{};
        m_line[k].m_driftPh = 0.61 * k;
        m_line[k].m_panPh = 1.7 * k;
    }
    m_pendingLines = false;
    layout( m_linesWant );
    m_v = m_vPrev = ( m_reverse ? -1.0 : 1.0 ) * ( m_half ? 0.5 : 1.0 );
    m_gliding = false;
    m_angle = m_blur * 0.78539816f;
    {
        const float x = clampv( ( m_blur - 0.45f ) / 0.55f, 0.0f, 1.0f );
        m_apCoef = 0.6f * x * x * ( 3.0f - 2.0f * x );
    }
    for ( int s = 0; s < 3; ++s )
    {
        m_stageC[s] = std::cos( m_angle );
        m_stageS[s] = std::sin( m_angle );
    }
    m_recallCur = m_recall;
    m_driveCur = m_drive;
    m_inGainCur = ( m_freeze || m_erase ) ? 0.0f : 1.0f;
    m_outGainCur = m_erase ? 0.0f : 1.0f;
    m_clearPos = -1;
    m_cleared = false;
    lengths();
    for ( int k = 0; k < MaxLines; ++k )
        m_line[k].m_lenBase = m_line[k].m_lenPrev = m_line[k].m_lenNext = m_line[k].m_lenTarget;
    m_quietFor = LLONG_MAX / 2;   // a memory that has heard nothing is quiet
    m_harmonySig = -1.0;
}

void Memory::setShape( int lines, float sizeSeconds, float blur, float drift )
{
    m_linesWant = lines <= 2 ? 2 : ( lines <= 4 ? 4 : 8 );
    m_size = clampv( sizeSeconds, 0.2f, 60.0f );
    m_blur = clampv( blur, 0.0f, 1.0f );
    m_drift = clampv( drift, 0.0f, 1.0f );
}

void Memory::setDecay( float hold, float age, float renew, float drive )
{
    m_hold = clampv( hold, 0.0f, 1.0f );
    m_age = clampv( age, 0.0f, 1.0f );
    m_renew = clampv( renew, 0.0f, 1.0f );
    m_drive = clampv( drive, 0.0f, 1.0f );
}

void Memory::setRecall( float recall, float seek, float grainMs )
{
    m_recall = clampv( recall, 0.0f, 1.0f );
    m_seek = clampv( seek, 0.0f, 1.0f );
    m_grainMs = clampv( grainMs, 20.0f, 2000.0f );
}

void Memory::setTape( bool reverse, bool half, bool freeze, bool erase )
{
    m_reverse = reverse;
    m_half = half;
    m_freeze = freeze;
    m_erase = erase;
}

void Memory::setHarmony( const FixedScale &scale, double tonicHz )
{
    if ( !( tonicHz > 1.0 ) || !std::isfinite( tonicHz ) ) return;
    const int count = clampv( scale.m_count, 1, FixedScale::Max );
    double    sig = tonicHz + count * 1000.003;
    for ( int k = 0; k < count; ++k ) sig += scale.m_ratios[k] * ( k + 1.618 );
    if ( sig == m_harmonySig ) return;
    m_harmonySig = sig;
    // The scale as pitch classes: every degree where its frequency falls, the tonic weighted up.
    float c[12] = {};
    for ( int k = 0; k < count; ++k )
    {
        const double ratio =
            ( scale.m_ratios[k] > 0.0 && std::isfinite( scale.m_ratios[k] ) ) ? scale.m_ratios[k] : 1.0;
        const long pc = std::lround( 12.0 * std::log2( tonicHz * ratio / 16.351597831287414 ) );
        c[( ( pc % 12 ) + 12 ) % 12] += ( k == 0 ? 1.5f : 1.0f );
    }
    double norm = 0.0;
    for ( float v : c ) norm += static_cast<double>( v ) * v;
    norm = std::sqrt( norm );
    for ( int j = 0; j < 12; ++j ) m_scaleChroma[j] = norm > 0.0 ? static_cast<float>( c[j] / norm ) : 0.0f;
}

void Memory::layout( int lines )
{
    m_lines = lines;
    m_cap = ( m_poolCells / m_lines ) / Seg * Seg;
    for ( int k = 0; k < MaxLines; ++k )
    {
        Line &ln = m_line[k];
        ln.m_w = std::fmod( ln.m_w, static_cast<double>( m_cap ) );
        if ( ln.m_w < 0.0 ) ln.m_w += m_cap;
        ln.m_base = static_cast<int>( ln.m_w );
        ln.m_acc0 = ln.m_wt0 = ln.m_acc1 = ln.m_wt1 = 0.0f;
        ln.m_seg = -1;
    }
    m_live = 0;   // a grain's place belongs to the old division
    m_sizeDone = -1.0f;
}

void Memory::lengths()
{
    // The longest line is the size, as far as its share of the pool allows: short of it by the
    // slide's reach and the heads' margins. The others step down from it geometrically to six
    // tenths, every length a prime number of cells, so no two lines ever meet in a common period.
    const double guard = 0.25 * m_sr + 4.0 * Sub;
    const double longest =
        std::clamp( static_cast<double>( m_size ) * m_sr, 0.2 * m_sr, static_cast<double>( m_cap ) - guard );
    for ( int k = 0; k < MaxLines; ++k )
    {
        Line        &ln = m_line[k];
        const double ratio =
            m_lines > 1 ? std::pow( 0.6, static_cast<double>( std::min( k, m_lines - 1 ) ) / ( m_lines - 1 ) ) : 1.0;
        const double target = std::max( 0.05 * m_sr, longest * ratio - m_apLen[k] );
        ln.m_lenTarget = static_cast<double>( primeAtMost( static_cast<int>( target ) ) );
        if ( ln.m_lenBase <= 0.0 ) ln.m_lenBase = ln.m_lenPrev = ln.m_lenNext = ln.m_lenTarget;
    }
    m_sizeDone = m_size;
    m_quietAfter = static_cast<long long>( 2.0 * ( longest + m_apLen[0] ) + m_sr );
    gains();
}

void Memory::gains()
{
    const bool   forever = m_hold >= 0.999f;
    const double t60 = 4.0 * std::pow( 300.0, static_cast<double>( m_hold ) );
    for ( int k = 0; k < MaxLines; ++k )
    {
        Line        &ln = m_line[k];
        const double pass = ( ln.m_lenTarget + m_apLen[k] ) / m_sr;   // seconds per pass at normal speed
        ln.m_gain = forever ? 1.0f : static_cast<float>( std::exp( -Ln1000 * pass / t60 ) );
        // Age: what a pass costs at Nyquist -- two decibels per second of line at full Age -- as a
        // one-pole whose gain at DC is one, so the lows keep what Hold gives them.
        const double r = std::pow( 10.0, 2.0 * static_cast<double>( m_age ) * pass / 20.0 );
        ln.m_absA = static_cast<float>( ( r - 1.0 ) / ( r + 1.0 ) );
    }
    m_holdDone = m_hold;
    m_ageDone = m_age;
}

void Memory::resetLines()
{
    for ( int k = 0; k < MaxLines; ++k )
    {
        Line &ln = m_line[k];
        ln.m_acc0 = ln.m_wt0 = ln.m_acc1 = ln.m_wt1 = 0.0f;
        ln.m_absZ = ln.m_dcX = ln.m_dcY = ln.m_env = 0.0f;
        ln.m_sat.reset();
        ln.m_aa.m_ic1 = ln.m_aa.m_ic2 = 0.0f;
    }
}

void Memory::stepControl( int m )
{
    const double dt = static_cast<double>( m ) / m_sr;
    const float  fm = static_cast<float>( m );
    const float  srf = static_cast<float>( m_sr );

    // A new number of lines: the output fades, the pool is re-divided, the output comes back.
    if ( m_linesWant != m_lines ) m_pendingLines = true;
    const bool  mute = m_pendingLines || m_erase;
    const float fade = fm / ( 0.06f * srf );
    m_outGainCur = mute ? std::max( 0.0f, m_outGainCur - fade ) : std::min( 1.0f, m_outGainCur + fade );
    if ( m_pendingLines && m_outGainCur <= 0.0f )
    {
        layout( m_linesWant );
        lengths();
        for ( int k = 0; k < MaxLines; ++k )
            m_line[k].m_lenBase = m_line[k].m_lenPrev = m_line[k].m_lenNext = m_line[k].m_lenTarget;
        m_pendingLines = false;
    }
    // Erase: once the output is down, the pool is emptied a little at a time -- all of it at once
    // would be one very long block -- and everything the lines hold with it.
    if ( m_erase )
    {
        if ( !m_cleared && m_outGainCur <= 0.0f )
        {
            if ( m_clearPos < 0 ) m_clearPos = 0;
            const int end = std::min( m_poolCells, m_clearPos + ( 1 << 17 ) );
            std::fill( m_pool.begin() + m_clearPos, m_pool.begin() + end, 0.0f );
            m_clearPos = end;
            if ( m_clearPos >= m_poolCells )
            {
                std::fill( m_ap.begin(), m_ap.end(), 0.0f );
                std::fill( m_segRms.begin(), m_segRms.end(), 0.0f );
                resetLines();
                m_live = 0;
                m_clearPos = -1;
                m_cleared = true;
            }
        }
    }
    else
    {
        m_cleared = false;
        m_clearPos = -1;
    }
    // The lengths only when the size has moved by more than half a millisecond: finding eight primes
    // near two million is a few thousand divisions each, which a modulated size must not ask for on
    // every block.
    if ( std::fabs( m_size - m_sizeDone ) > 0.0005f )
        lengths();
    else if ( m_hold != m_holdDone || m_age != m_ageDone )
        gains();

    // The attractor that bends the slides and the pans: one of its time units to eight seconds.
    {
        const double h = dt / 8.0;
        const auto   f = []( const double *s, double *d )
        {
            d[0] = 10.0 * ( s[1] - s[0] );
            d[1] = s[0] * ( 28.0 - s[2] ) - s[1];
            d[2] = s[0] * s[1] - ( 8.0 / 3.0 ) * s[2];
        };
        double k1[3], k2[3], k3[3], k4[3], t[3];
        f( m_lorenz, k1 );
        for ( int i = 0; i < 3; ++i ) t[i] = m_lorenz[i] + 0.5 * h * k1[i];
        f( t, k2 );
        for ( int i = 0; i < 3; ++i ) t[i] = m_lorenz[i] + 0.5 * h * k2[i];
        f( t, k3 );
        for ( int i = 0; i < 3; ++i ) t[i] = m_lorenz[i] + h * k3[i];
        f( t, k4 );
        for ( int i = 0; i < 3; ++i ) m_lorenz[i] += h / 6.0 * ( k1[i] + 2.0 * k2[i] + 2.0 * k3[i] + k4[i] );
        if ( !std::isfinite( m_lorenz[0] + m_lorenz[1] + m_lorenz[2] ) )
        {
            m_lorenz[0] = 0.1;
            m_lorenz[1] = 0.0;
            m_lorenz[2] = 20.0;
        }
        m_chaos[0] = clampv( static_cast<float>( m_lorenz[0] / 20.0 ), -1.0f, 1.0f );
        m_chaos[1] = clampv( static_cast<float>( m_lorenz[1] / 27.0 ), -1.0f, 1.0f );
        m_chaos[2] = clampv( static_cast<float>( ( m_lorenz[2] - 25.0 ) / 22.0 ), -1.0f, 1.0f );
    }

    // The tape: a change of speed glides, a change of direction runs through a standstill. A full
    // turn from forwards to backwards takes three tenths of a second.
    const double vt = ( m_reverse ? -1.0 : 1.0 ) * ( m_half ? 0.5 : 1.0 );
    const double step = static_cast<double>( m ) / ( 0.15 * m_sr );
    m_vPrev = m_v;
    if ( m_v != vt )
    {
        m_v += std::clamp( vt - m_v, -step, step );
        m_gliding = true;
    }

    // The exchange: the matrix's angle and the scattering, glided; each of the butterfly's stages
    // leans a little off the common angle on its own slow sine, so the mix itself drifts.
    const float sm = 1.0f - std::exp( -fm / ( 0.08f * srf ) );
    m_angle += ( m_blur * 0.78539816f - m_angle ) * sm;
    {
        const float x = clampv( ( m_blur - 0.45f ) / 0.55f, 0.0f, 1.0f );
        m_apCoef += ( 0.6f * x * x * ( 3.0f - 2.0f * x ) - m_apCoef ) * sm;
    }
    for ( int s = 0; s < 3; ++s )
    {
        m_stagePh[s] += Tau * StageHz[s] * dt;
        if ( m_stagePh[s] > Tau ) m_stagePh[s] -= Tau;
        const float a = m_angle * ( 1.0f + 0.08f * m_drift * static_cast<float>( std::sin( m_stagePh[s] ) ) );
        m_stageC[s] = std::cos( a );
        m_stageS[s] = std::sin( a );
    }
    m_recallCur += ( m_recall - m_recallCur ) * sm;
    m_driveCur += ( m_drive - m_driveCur ) * sm;
    const float inTarget = ( m_freeze || m_erase ) ? 0.0f : 1.0f;
    const float inStep = fm / ( 0.02f * srf );
    m_inGainCur = m_inGainCur < inTarget ? std::min( inTarget, m_inGainCur + inStep )
                                         : std::max( inTarget, m_inGainCur - inStep );

    // The lines: their slides, their places in the field, and a slow tape's anti-alias filter.
    const double slope =
        std::pow( 2.0, 15.0 * static_cast<double>( m_drift ) / 1200.0 ) - 1.0;   // cells of slide per cell, at most
    const float aaHz = std::min( static_cast<float>( 0.45 * std::fabs( m_v ) * m_sr ), 0.45f * srf );
    for ( int k = 0; k < m_lines; ++k )
    {
        Line        &ln = m_line[k];
        const double bend = 1.0 + 0.35 * ( std::cos( 1.3 * k ) * m_chaos[0] + std::sin( 1.3 * k ) * m_chaos[1] );
        ln.m_driftPh += Tau * DriftHz[k] * bend * dt;
        if ( ln.m_driftPh > Tau ) ln.m_driftPh -= Tau;
        // The amplitude that keeps the slope of the slide under the bound even with the rate bent
        // up by a third, and never more than two fifths of the line.
        const double ampWant = std::min( slope * m_sr / ( Tau * DriftHz[k] * 1.35 ), 0.4 * ln.m_lenBase );
        ln.m_driftAmp += ( static_cast<float>( ampWant ) - ln.m_driftAmp ) * std::min( 1.0f, fm / ( 2.0f * srf ) );
        const double reach = 0.25 * m;   // a quarter of a cell a sample: a new size slides in, as on a tape delay
        ln.m_lenBase += std::clamp( ln.m_lenTarget - ln.m_lenBase, -reach, reach );
        ln.m_lenPrev = ln.m_lenNext;
        ln.m_lenNext =
            std::max( 4.0 * Sub, ln.m_lenBase + static_cast<double>( ln.m_driftAmp ) * std::sin( ln.m_driftPh ) );
        const double bendPan = 1.0 + 0.35 * ( std::sin( 0.7 * k ) * m_chaos[0] - std::cos( 0.7 * k ) * m_chaos[2] );
        ln.m_panPh += Tau * PanHz[k] * bendPan * dt;
        if ( ln.m_panPh > Tau ) ln.m_panPh -= Tau;
        const float pan = clampv(
            panBase( k, m_lines ) + 0.45f * m_drift * static_cast<float>( std::sin( ln.m_panPh ) ), -1.0f, 1.0f );
        const float th = ( pan + 1.0f ) * 0.78539816f;
        ln.m_glPrev = ln.m_glNext;
        ln.m_grPrev = ln.m_grNext;
        ln.m_glNext = std::cos( th );
        ln.m_grNext = std::sin( th );
        if ( std::fabs( m_v ) < 0.999 ) ln.m_aa.setQ( aaHz, 0.7071f, srf );
    }
}

void Memory::tapeWrite( int k, float x, double v )
{
    // The write head spreads each sample over the two cells it falls between, weighted by where it
    // is, and keeps the running sums until it has left a cell for good. At full speed on a whole
    // cell that is exactly the sample; at half speed each cell is the average of the samples that
    // crossed it; at a standstill the sums just grow.
    Line     &ln = m_line[k];
    float    *ring = m_pool.data() + static_cast<size_t>( k ) * static_cast<size_t>( m_cap );
    const int fl = static_cast<int>( ln.m_w );
    if ( fl != ln.m_base )
    {
        const int next = ln.m_base + 1 == m_cap ? 0 : ln.m_base + 1;
        const int prev = ln.m_base == 0 ? m_cap - 1 : ln.m_base - 1;
        if ( fl == next )
        {   // on by a cell: the one left behind is finished
            if ( ln.m_wt0 > 0.0f ) ring[ln.m_base] = ln.m_acc0 / ln.m_wt0;
            ln.m_acc0 = ln.m_acc1;
            ln.m_wt0 = ln.m_wt1;
            ln.m_acc1 = 0.0f;
            ln.m_wt1 = 0.0f;
        }
        else if ( fl == prev )
        {   // back by a cell: the one above it is finished
            if ( ln.m_wt1 > 0.0f ) ring[next] = ln.m_acc1 / ln.m_wt1;
            ln.m_acc1 = ln.m_acc0;
            ln.m_wt1 = ln.m_wt0;
            ln.m_acc0 = 0.0f;
            ln.m_wt0 = 0.0f;
        }
        else
        {   // moved by more: the heads were placed anew
            ln.m_acc0 = ln.m_wt0 = ln.m_acc1 = ln.m_wt1 = 0.0f;
        }
        ln.m_base = fl;
        const int seg = static_cast<int>( ( static_cast<long long>( k ) * m_cap + fl ) >> SegBits );
        if ( seg != ln.m_seg )
        {
            if ( ln.m_seg >= 0 )
            {   // the stretch it has just finished is fingerprinted
                const int nt = ( m_qTail + 1 ) & QueueMask;
                if ( nt != m_qHead )
                {
                    m_queue[static_cast<size_t>( m_qTail )] = ln.m_seg;
                    m_qTail = nt;
                }
            }
            ln.m_seg = seg;
        }
    }
    const float f = static_cast<float>( ln.m_w - fl );
    ln.m_acc0 += ( 1.0f - f ) * x;
    ln.m_wt0 += 1.0f - f;
    ln.m_acc1 += f * x;
    ln.m_wt1 += f;
    ln.m_w += v;
    if ( ln.m_w >= m_cap )
        ln.m_w -= m_cap;
    else if ( ln.m_w < 0.0 )
        ln.m_w += m_cap;
}

void Memory::fingerprint( const float *x, float *chroma, float &rms )
{
    double sq = 0.0;
    for ( int i = 0; i < Seg; ++i )
    {
        m_fftRe[static_cast<size_t>( i )] = x[i] * m_window[static_cast<size_t>( i )];
        sq += static_cast<double>( x[i] ) * x[i];
    }
    rms = static_cast<float>( std::sqrt( sq / Seg ) );
    for ( int j = 0; j < 12; ++j ) chroma[j] = 0.0f;
    if ( rms < 1.0e-6f ) return;
    m_fft.forward( m_fftRe.data(), m_fftRe.data(), m_fftIm.data() );
    for ( int b = 1; b < Seg / 2; ++b )
    {
        const int pc = m_binPc[static_cast<size_t>( b )];
        if ( pc < 0 ) continue;
        const float re = m_fftRe[static_cast<size_t>( b )], im = m_fftIm[static_cast<size_t>( b )];
        chroma[pc] += std::sqrt( re * re + im * im );
    }
    double norm = 0.0;
    for ( int j = 0; j < 12; ++j ) norm += static_cast<double>( chroma[j] ) * chroma[j];
    norm = std::sqrt( norm );
    if ( norm > 0.0 )
        for ( int j = 0; j < 12; ++j ) chroma[j] = static_cast<float>( chroma[j] / norm );
}

void Memory::analyse()
{
    // One fingerprint per control block at most: the last seconds of input when a window of it is
    // full, otherwise the next stretch of tape a write head has finished.
    float c[12];
    float rms = 0.0f;
    if ( m_inPos >= Seg )
    {
        fingerprint( m_inBuf.data(), c, rms );
        m_inPos = 0;
        const float wgt = rms > 1.0e-4f ? 0.07f : 0.0f;   // about four seconds of memory
        for ( int j = 0; j < 12; ++j ) m_inputChroma[j] += wgt * ( c[j] - m_inputChroma[j] );
        m_inputLevel += 0.07f * ( rms - m_inputLevel );
        return;
    }
    if ( m_qHead == m_qTail ) return;
    const int seg = m_queue[static_cast<size_t>( m_qHead )];
    m_qHead = ( m_qHead + 1 ) & QueueMask;
    const long long start = static_cast<long long>( seg ) << SegBits;
    if ( start + Seg > m_poolCells ) return;
    fingerprint( m_pool.data() + start, c, rms );
    std::memcpy( m_chroma.data() + static_cast<size_t>( seg ) * 12, c, sizeof( c ) );
    m_segRms[static_cast<size_t>( seg )] = rms;
}

void Memory::spawnGrain( int offset )
{
    if ( m_live >= MaxGrains || m_cap <= 0 ) return;
    const int glen =
        std::max( 64, static_cast<int>( static_cast<double>( m_grainMs ) *
                                        ( 0.8 + 0.4 * static_cast<double>( m_rng.uniform() ) ) * m_sr / 1000.0 ) );
    // A grain reads forwards at the speed it was recorded, and it must not meet a write head while
    // it plays: it starts far enough behind one that the head, moving at the tape's speed, cannot
    // reach it, and far enough ahead of it the other way round the ring.
    const double margin = 2.0 * Sub + 16.0;
    const double behind = std::max( 0.0, 1.0 - m_v ) * glen + margin;
    const double room = static_cast<double>( m_cap ) - behind - margin - glen;
    if ( room < Seg ) return;
    // What the grains listen for: the scale, and as much of the last seconds of input as there was.
    float target[12];
    {
        const float inW = clampv( m_inputLevel / 0.02f, 0.0f, 1.0f );
        double      norm = 0.0;
        for ( int j = 0; j < 12; ++j )
        {
            target[j] = m_scaleChroma[j] + inW * m_inputChroma[j];
            norm += static_cast<double>( target[j] ) * target[j];
        }
        norm = std::sqrt( norm );
        for ( int j = 0; j < 12; ++j ) target[j] = norm > 0.0 ? static_cast<float>( target[j] / norm ) : 0.0f;
    }
    // A dozen places drawn at random; Seek decides how much their harmony counts against chance.
    int    bestK = -1;
    double bestPos = 0.0;
    float  bestScore = -1.0e9f;
    for ( int c = 0; c < 12; ++c )
    {
        const int    k = m_rng.below( m_lines );
        const double delta = behind + static_cast<double>( m_rng.uniform() ) * room;
        double       pos = m_line[k].m_w - delta;
        while ( pos < 0.0 ) pos += m_cap;
        const int seg =
            static_cast<int>( ( static_cast<long long>( k ) * m_cap + static_cast<long long>( pos ) ) >> SegBits );
        const float u = m_rng.uniform();
        float       score = -1.0f + 0.01f * u;   // silence, which is taken only if there is nothing else
        if ( m_segRms[static_cast<size_t>( seg )] >= 1.0e-4f )
        {
            const float *ch = m_chroma.data() + static_cast<size_t>( seg ) * 12;
            float        dot = 0.0f;
            for ( int j = 0; j < 12; ++j ) dot += ch[j] * target[j];
            score = m_seek * dot + ( 1.0f - m_seek ) * u;
        }
        if ( score > bestScore )
        {
            bestScore = score;
            bestK = k;
            bestPos = pos;
        }
    }
    if ( bestK < 0 || bestScore < 0.0f ) return;   // an empty memory has nothing to recall
    RingGrain &g = m_grains[m_live++];
    g.m_pos = bestPos;
    g.m_rate = 1.0;
    g.m_len = glen;
    g.m_age = 0;
    g.m_start = offset;
    g.m_ring = bestK;
    g.m_bus = 0;
    const float side = ( bestK & 1 ) ? 1.0f : -1.0f;
    const float pan = clampv( 0.45f * side + 0.35f * m_rng.bipolar(), -1.0f, 1.0f );
    const float th = ( pan + 1.0f ) * 0.78539816f;
    g.m_gl = std::cos( th );
    g.m_gr = std::sin( th );
    g.m_wc = 1.0f;
    g.m_ws = 0.0f;
    phasorFrom( 1.0 / glen, g.m_rc, g.m_rs );
    ++m_started;
}

void Memory::process( float *L, float *R, int n )
{
    float peakIn = 0.0f, peakOut = 0.0f;
    for ( int p = 0; p < n; )
    {
        const int m = std::min( Sub, n - p );
        stepControl( m );
        const int N = m_lines;

        // Recall: the grains are rendered before the lines write this block. They read no closer
        // than two blocks to a write head, so nothing they read changes under them.
        float      gL[Sub] = {}, gR[Sub] = {};
        const bool recallOn = ( m_recall > 0.0f || m_recallCur > 1.0e-4f ) && !m_pendingLines && !m_erase;
        if ( recallOn )
        {
            const double density = 4000.0 / static_cast<double>( m_grainMs );   // an overlap of four
            for ( int i = 0; i < m; ++i )
            {
                m_hazard += density / m_sr;
                if ( m_hazard >= m_nextHazard )
                {
                    m_hazard = 0.0;
                    m_nextHazard = -std::log( 1.0 - static_cast<double>( m_rng.uniform() ) + 1.0e-12 );
                    spawnGrain( i );
                }
            }
            for ( int c = 0; c < m_live; )
            {
                RingGrain &g = m_grains[c];
                {
                    const float r2 = g.m_wc * g.m_wc + g.m_ws * g.m_ws, fix = 1.5f - 0.5f * r2;
                    g.m_wc *= fix;
                    g.m_ws *= fix;
                }
                const int begin = g.m_start < m ? g.m_start : m;
                g.m_start = 0;
                const int count = std::min( m - begin, g.m_len - g.m_age );
                if ( count > 0 && g.m_ring < N )
                {
                    renderRingGrain( m_pool.data() + static_cast<size_t>( g.m_ring ) * static_cast<size_t>( m_cap ),
                                     m_cap, 1, g, gL + begin, gR + begin, count );
                    g.m_age += count;
                }
                if ( g.m_age >= g.m_len || g.m_ring >= N )
                    m_grains[c] = m_grains[--m_live];
                else
                    ++c;
            }
        }
        else
        {
            m_live = 0;
        }
        if ( m_qHead != m_qTail || m_inPos >= Seg ) analyse();

        // The lines, sample by sample.
        const float  inv = 1.0f / static_cast<float>( m );
        const double v0 = m_vPrev, v1 = m_v;
        const bool   slow = std::fabs( v0 ) < 0.999 || std::fabs( v1 ) < 0.999;
        const float  inGain = m_inGainCur * std::sqrt( 2.0f / static_cast<float>( N ) );
        const float  apc = m_apCoef;
        const float  drv = m_driveCur;
        const float  rc = m_recallCur;
        const float  outG = m_outGainCur;
        const float  grainGain = 0.815f * std::sqrt( 0.5f * static_cast<float>( N ) );
        for ( int i = 0; i < m; ++i )
        {
            const float  a = static_cast<float>( i + 1 ) * inv;
            const double v = v0 + ( v1 - v0 ) * static_cast<double>( a );
            const float  xl = L[p + i], xr = R[p + i];
            const float  lev = std::max( std::fabs( xl ), std::fabs( xr ) );
            peakIn = std::max( peakIn, lev );
            m_inEnv += ( lev > m_inEnv ? m_envAtk : m_envRel ) * ( lev - m_inEnv );
            const float keep = m_freeze ? 1.0f : 1.0f - m_renew * clampv( m_inEnv / 0.08f, 0.0f, 1.0f );
            float       y[MaxLines];
            float       outL = 0.0f, outR = 0.0f;
            for ( int k = 0; k < N; ++k )
            {
                Line        &ln = m_line[k];
                const float *ring = m_pool.data() + static_cast<size_t>( k ) * static_cast<size_t>( m_cap );
                const double len = ln.m_lenPrev + ( ln.m_lenNext - ln.m_lenPrev ) * static_cast<double>( a );
                double       r = ln.m_w - len;
                if ( r < 0.0 ) r += m_cap;
                const int    i1 = static_cast<int>( r );
                const float  pf = static_cast<float>( r - i1 ) * static_cast<float>( Phases );
                const int    ph = std::min( static_cast<int>( pf ), Phases - 1 );
                const float  pw = pf - static_cast<float>( ph );
                const float *h0 = m_sinc.data() + static_cast<size_t>( ph * Taps );
                const float *h1 = h0 + Taps;
                float        s = 0.0f;
                int          c = i1 - 3;
                if ( c >= 0 && c + Taps <= m_cap )
                {
                    for ( int j = 0; j < Taps; ++j ) s += ring[c + j] * ( h0[j] + pw * ( h1[j] - h0[j] ) );
                }
                else
                {
                    if ( c < 0 ) c += m_cap;
                    for ( int j = 0; j < Taps; ++j )
                    {
                        s += ring[c] * ( h0[j] + pw * ( h1[j] - h0[j] ) );
                        if ( ++c == m_cap ) c = 0;
                    }
                }
                outL += s * ( ln.m_glPrev + ( ln.m_glNext - ln.m_glPrev ) * a );
                outR += s * ( ln.m_grPrev + ( ln.m_grNext - ln.m_grPrev ) * a );
                // In the loop: the scattering all-pass, the absorption, a DC blocker.
                float      *ab = m_ap.data() + static_cast<size_t>( k ) * static_cast<size_t>( m_apSize );
                const float d = ab[( m_apW - m_apLen[k] ) & m_apMask];
                const float sy = d - apc * s;
                ab[m_apW & m_apMask] = s + apc * sy;
                const float absA = m_freeze ? 0.0f : ln.m_absA;
                ln.m_absZ = sy + absA * ( ln.m_absZ - sy );
                // Scaled by (1 + R) / 2, so its gain is one at Nyquist and under one everywhere else:
                // unscaled it lifted the highs by a thousandth of a decibel a pass, and a memory held
                // for ever is nothing but passes.
                const float hp = 0.5f * ( 1.0f + m_dcR ) * ( ln.m_absZ - ln.m_dcX ) + m_dcR * ln.m_dcY;
                ln.m_dcX = ln.m_absZ;
                ln.m_dcY = std::fabs( hp ) < 1.0e-20f ? 0.0f : hp;
                y[k] = ln.m_dcY;
            }
            m_apW = ( m_apW + 1 ) & m_apMask;
            // The exchange: every stage turns pairs of lines a span apart, the spans doubling.
            {
                int stage = 0;
                for ( int span = 1; span < N; span <<= 1, ++stage )
                {
                    const float cs = m_stageC[stage], sn = m_stageS[stage];
                    for ( int b = 0; b < N; b += 2 * span )
                        for ( int j = b; j < b + span; ++j )
                        {
                            const float u = y[j], w = y[j + span];
                            y[j] = cs * u - sn * w;
                            y[j + span] = sn * u + cs * w;
                        }
                }
            }
            for ( int k = 0; k < N; ++k )
            {
                Line       &ln = m_line[k];
                float       fb = y[k] * ( m_freeze ? 1.0f : ln.m_gain * keep );
                const float mag = std::fabs( fb );
                ln.m_env += ( mag > ln.m_env ? m_limAtk : m_limRel ) * ( mag - ln.m_env );
                if ( ln.m_env > Limit ) fb *= Limit / ln.m_env;
                if ( drv > 1.0e-4f && !m_freeze )
                {
                    const float gd = 1.0f + 5.0f * drv;
                    const float sat = ln.m_sat( fb * gd ) / gd;
                    fb += std::min( 1.0f, 20.0f * drv ) * ( sat - fb );
                }
                float x = m_erase ? 0.0f : fb + inGain * ( ( k & 1 ) ? xr : xl );
                if ( slow )
                {
                    float lp, bp, hp;
                    ln.m_aa.tick( x, lp, bp, hp );
                    x = lp;
                }
                tapeWrite( k, x, v );
            }
            const float oL = ( outL + ( gL[i] * grainGain - outL ) * rc ) * outG;
            const float oR = ( outR + ( gR[i] * grainGain - outR ) * rc ) * outG;
            L[p + i] = oL;
            R[p + i] = oR;
            peakOut = std::max( peakOut, std::max( std::fabs( oL ), std::fabs( oR ) ) );
            if ( m_inPos < Seg ) m_inBuf[static_cast<size_t>( m_inPos++ )] = 0.5f * ( xl + xr );
        }
        // A glide that has arrived at full speed puts the heads back on whole cells, where a splat
        // is exact: half a cell of offset would low-pass every pass, and passes add up.
        if ( m_gliding && m_v == ( m_reverse ? -1.0 : 1.0 ) * ( m_half ? 0.5 : 1.0 ) )
        {
            m_gliding = false;
            if ( std::fabs( m_v ) == 1.0 )
                for ( int k = 0; k < N; ++k )
                {
                    double w = std::round( m_line[k].m_w );
                    if ( w >= m_cap ) w -= m_cap;
                    m_line[k].m_w = w;
                }
        }
        p += m;
    }
    m_quietFor = ( peakIn < 1.0e-6f && peakOut < 1.0e-6f ) ? m_quietFor + n : 0;
}

double Memory::energy() const
{
    double e = 0.0;
    for ( int k = 0; k < m_lines; ++k )
    {
        const float *ring = m_pool.data() + static_cast<size_t>( k ) * static_cast<size_t>( m_cap );
        const int    len = static_cast<int>( m_line[k].m_lenTarget );
        int          c = static_cast<int>( std::floor( m_line[k].m_w ) ) - len;
        while ( c < 0 ) c += m_cap;
        for ( int j = 0; j < len; ++j )
        {
            e += static_cast<double>( ring[c] ) * ring[c];
            if ( ++c == m_cap ) c = 0;
        }
        const float *ab = m_ap.data() + static_cast<size_t>( k ) * static_cast<size_t>( m_apSize );
        for ( int j = 1; j <= m_apLen[k]; ++j )
        {
            const float v = ab[( m_apW - j ) & m_apMask];
            e += static_cast<double>( v ) * v;
        }
    }
    return e;
}

}   // namespace ambient
