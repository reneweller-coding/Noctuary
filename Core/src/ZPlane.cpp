/**
 * @file ZPlane.cpp
 * @brief The z-plane filter's arithmetic: the modal bank, the corner frames and their
 *        interpolation, and the normalised biquad cascade.
 *
 * ZPlane.h describes the idea and declares the pieces; this file builds them. The generated bank
 * -- a hundred and fifty-five shapes of eight corner descriptors each, with their names and
 * families -- is compiled in from ZPlaneBank.inc, which Tools/make_zplane_bank.py writes.
 * zFrameFromSpec() turns one compact corner descriptor into a frame of up to six sections;
 * zInterpolate() blends the eight corners of a shape trilinearly in the log-frequency /
 * log-bandwidth domain, over logarithms that are taken once per shape and cached (ShapeCorners),
 * because the interpolation runs at control rate for every sounding voice and the corners never
 * change.
 *
 * zBuildCascade() is the cascade's normalisation: every section is scaled to its geometric mean
 * over a fixed 16-point logarithmic grid plus its own pole and zero angles, and the finished
 * cascade is measured over the same points so that its loudest point comes back to unity -- the
 * reason a shape can be extremely resonant without becoming loud. ZModal is the same frames read
 * as a parallel bank of ringing resonators, each normalised to unity at its own frequency and the
 * bank as a whole on expected power. The two warm-up functions build the static tables from the
 * message thread, so that no voice ever builds them under the lock a function-local static carries.
 * @author Rene Weller
 */
#include <vector>
#include "ambient/ZPlane.h"
#include <algorithm>

namespace ambient
{

const char *const ZModeNames[4] = { "Off", "Series", "Replace", "Modal" };

// ---------------------------------------------------------------- modal bank

void ZModal::reset()
{
    for ( Mode &m : m_modes )
    {
        m.m_z1[0] = m.m_z1[1] = 0.0f;
        m.m_z2[0] = m.m_z2[1] = 0.0f;
    }
}

void ZModal::set( const ZFrame &f, float decaySeconds, float damp )
{
    m_used = 0;
    const float nyq = m_sr * 0.49f;
    const float d = clampv( decaySeconds, 0.02f, 40.0f );
    const float dp = clampv( damp, 0.0f, 1.0f );
    // The lowest mode sets the reference: with damp = 1 every other mode's decay is shortened in
    // proportion to how far above it sits, which is what makes a bright object sound short and a
    // heavy one long without touching two knobs.
    float lowest = 1.0e9f;
    for ( int i = 0; i < f.m_used; ++i )
        if ( f.m_s[i].m_poleHz > 0.0f ) lowest = std::min( lowest, f.m_s[i].m_poleHz );
    if ( lowest > 1.0e8f ) lowest = 100.0f;

    float power = 0.0f;
    for ( int i = 0; i < f.m_used && m_used < ZSections; ++i )
    {
        const float hz = f.m_s[i].m_poleHz;
        if ( hz <= 5.0f || hz >= nyq ) continue;   // nothing above Nyquist can ring
        Mode       &m = m_modes[m_used];
        const float t60 = std::max( 0.01f, d * std::pow( lowest / hz, dp ) );
        // T60 to pole radius: 60 dB is a factor of 1000, so r^(t60*sr) = 1e-3.
        const float r = std::exp( -6.907755f / ( t60 * m_sr ) );
        const float w = 2.0f * Pi * hz / m_sr;
        m.m_hz = hz;
        m.m_r = std::min( r, 0.9999995f );   // strictly inside the unit circle
        m.m_a1 = 2.0f * m.m_r * std::cos( w );
        m.m_a2 = -( m.m_r * m.m_r );
        // Unity gain at the mode's own frequency: |H| = b0 / |1 - a1 e^-jw - a2 e^-2jw|.
        const float c1 = std::cos( w ), s1 = std::sin( w ), c2 = std::cos( 2.0f * w ), s2 = std::sin( 2.0f * w );
        const float dr = 1.0f - m.m_a1 * c1 - m.m_a2 * c2, di = m.m_a1 * s1 + m.m_a2 * s2;
        m.m_b0 = std::sqrt( dr * dr + di * di );
        m.m_gain = f.m_s[i].m_gain;
        power += m.m_gain * m.m_gain;
        ++m_used;
    }
    // Normalised on expected power rather than on the sum of the peaks. The modes are at
    // different frequencies and almost never in phase, so adding their peaks would leave a bank
    // of six far quieter than a bank of two; adding their powers keeps the level even.
    m_norm = power > 0.0f ? 1.0f / std::sqrt( power ) : 0.0f;
}

float ZModal::magnitudeAt( float hz ) const
{
    // The modes add. |H| of the bank is the magnitude of the sum, and the phases matter, so this
    // sums the complex responses rather than the magnitudes.
    const float w = 2.0f * Pi * clampv( hz, 1.0f, m_sr * 0.5f ) / m_sr;
    const float c1 = std::cos( w ), s1 = std::sin( w ), c2 = std::cos( 2.0f * w ), s2 = std::sin( 2.0f * w );
    float       re = 0.0f, im = 0.0f;
    for ( int i = 0; i < m_used; ++i )
    {
        const Mode &m = m_modes[i];
        const float dr = 1.0f - m.m_a1 * c1 - m.m_a2 * c2, di = m.m_a1 * s1 + m.m_a2 * s2;
        const float den = std::max( dr * dr + di * di, 1e-20f );
        // b0 / (dr + j di) = b0 (dr - j di) / |den|
        re += m.m_gain * m.m_b0 * dr / den;
        im -= m.m_gain * m.m_b0 * di / den;
    }
    return std::sqrt( re * re + im * im ) * m_norm;
}

#include "ZPlaneBank.inc"

ZFrame zFrameFromSpec( const ZCornerSpec &c )
{
    ZFrame f;
    for ( int i = 0; i < ZSections; ++i )
    {
        if ( c.m_hz[i] <= 0.0f ) break;
        ZSection &s = f.m_s[i];
        s.m_poleHz = c.m_hz[i];
        s.m_poleBw = std::max( c.m_hz[i] * c.m_bwRatio, 0.5f );
        s.m_zeroHz = c.m_zeroRatio > 0.0f ? c.m_hz[i] * c.m_zeroRatio : 0.0f;
        s.m_zeroBw = s.m_zeroHz * c.m_zeroBwRatio;
        s.m_gain = std::pow( c.m_tilt, static_cast<float>( i ) );
        f.m_used = i + 1;
    }
    return f;
}

namespace
{
/**
 * @brief The eight corners of a shape, and the logarithms the interpolation takes of them.
 *
 * Both depend
 * on the shape alone -- on a number that changes when somebody turns a knob, and not otherwise --
 * and both used to be computed afresh on every call: eight corner frames rebuilt, and then
 * thirty-two logarithms per section taken of the values that had just been rebuilt. This runs at
 * control rate for every sounding voice, so at eleven voices that was eight thousand times a
 * second of audio, always with the same answer. VTune put it at nine per cent of an entire
 * render, which is roughly what the whole reverb costs.
 *
 * Held here instead, worked out once for each shape the moment it is first asked for. The
 * interpolation itself is untouched -- the same weights over the same logarithms, so the same
 * numbers to the last bit; only the arithmetic that had no reason to be repeated is gone.
 */
struct ShapeCorners
{
    int m_used = 0;   ///< sections every one of the eight corners has; the interpolated frame has as many
    /**
     * @brief One section as seen from all eight corners: the values the interpolation blends,
     *        corner by corner.
     *
     * One value per corner in each array; the logarithms are log2 of the value floored at 1e-3,
     * exactly as zInterpolate used to take them on every call.
     */
    struct Sec
    {
        float m_lPole[8] = {},   ///< log2 of the pole frequency in Hz
            m_lBw[8] = {},       ///< log2 of the pole bandwidth in Hz
            m_lZeroHz[8] = {},   ///< log2 of the zero frequency in Hz (meaningful only when `zeros`)
            m_lZeroBw[8] = {},   ///< log2 of the zero bandwidth in Hz
            m_gain[8] = {};      ///< the section gain, blended linearly
        bool m_zeros = false;    ///< a zero in every corner, or none at all
    } m_s[ZSections];            ///< the sections, the first `used` of them meaningful
};

/**
 * @brief The cached corners of a shape, built for every shape on the first call.
 *
 * The whole table is a function-local static, so the first caller builds it -- all shapes at once,
 * from ZCorners through zFrameFromSpec -- under the guard the language puts around the static.
 * zWarmShapes() makes sure that first caller is the message thread.
 * @param[in] shape  a valid shape index, 0 .. ZShapes - 1 (zInterpolate clamps before asking)
 * @return the corners and their logarithms for that shape
 */
const ShapeCorners &shapeCorners( int shape )
{
    static const std::vector<ShapeCorners> all = []
    {
        std::vector<ShapeCorners> v( static_cast<size_t>( ZShapes ) );
        for ( int sh = 0; sh < ZShapes; ++sh )
        {
            ZFrame c[8];
            for ( int k = 0; k < 8; ++k ) c[k] = zFrameFromSpec( ZCorners[sh][k] );
            ShapeCorners &out = v[static_cast<size_t>( sh )];
            out.m_used = c[0].m_used;
            for ( int k = 1; k < 8; ++k ) out.m_used = std::min( out.m_used, c[k].m_used );
            for ( int i = 0; i < out.m_used; ++i )
            {
                ShapeCorners::Sec &d = out.m_s[i];
                d.m_zeros = true;
                for ( int k = 0; k < 8; ++k )
                    if ( c[k].m_s[i].m_zeroHz <= 0.0f ) d.m_zeros = false;
                for ( int k = 0; k < 8; ++k )
                {
                    d.m_lPole[k] = std::log2( std::max( c[k].m_s[i].m_poleHz, 1e-3f ) );
                    d.m_lBw[k] = std::log2( std::max( c[k].m_s[i].m_poleBw, 1e-3f ) );
                    d.m_lZeroHz[k] = std::log2( std::max( c[k].m_s[i].m_zeroHz, 1e-3f ) );
                    d.m_lZeroBw[k] = std::log2( std::max( c[k].m_s[i].m_zeroBw, 1e-3f ) );
                    d.m_gain[k] = c[k].m_s[i].m_gain;
                }
            }
        }
        return v;
    }();
    return all[static_cast<size_t>( shape )];
}
}   // namespace

/**
 * Builds the shape table now, off the audio thread. Engine::prepare calls it; without that the
 * first voice to reach a filter would build it under the lock the language puts around a static.
 */
void zWarmShapes()
{
    (void)shapeCorners( 0 );
}

ZFrame zInterpolate( int shape, float x, float y, float z )
{
    shape = clampv( shape, 0, ZShapes - 1 );
    x = clampv( x, 0.0f, 1.0f );
    y = clampv( y, 0.0f, 1.0f );
    z = clampv( z, 0.0f, 1.0f );
    const ShapeCorners &c = shapeCorners( shape );
    ZFrame              out;
    out.m_used = c.m_used;
    // Trilinear. At z = 0 only the first four terms have any weight, so this is exactly the
    // bilinear interpolation it replaces -- which is what makes the third axis free to add.
    auto lerp3 = [&]( const float *v )
    {
        const float f0 = ( v[0] * ( 1 - x ) + v[1] * x ) * ( 1 - y ) + ( v[2] * ( 1 - x ) + v[3] * x ) * y;
        const float f1 = ( v[4] * ( 1 - x ) + v[5] * x ) * ( 1 - y ) + ( v[6] * ( 1 - x ) + v[7] * x ) * y;
        return f0 * ( 1 - z ) + f1 * z;
    };
    for ( int i = 0; i < out.m_used; ++i )
    {
        ZSection                &s = out.m_s[i];
        const ShapeCorners::Sec &d = c.m_s[i];
        s.m_poleHz = std::exp2( lerp3( d.m_lPole ) );
        s.m_poleBw = std::exp2( lerp3( d.m_lBw ) );
        // A zero that exists in every corner is interpolated; if any corner has none, there is none.
        s.m_zeroHz = d.m_zeros ? std::exp2( lerp3( d.m_lZeroHz ) ) : 0.0f;
        s.m_zeroBw = d.m_zeros ? std::exp2( lerp3( d.m_lZeroBw ) ) : 0.0f;
        s.m_gain = lerp3( d.m_gain );
    }
    return out;
}

namespace
{
/**
 * @brief The normalisation grid: 16 logarithmically spaced points in normalised frequency (about
 *        24 Hz to 21 kHz at 48 kHz), with cos/sin of w and 2w precomputed once.
 */
constexpr int Grid = 16;
/**
 * @brief The precomputed trigonometry of the normalisation grid, one set per grid point.
 *
 * The angular frequencies are w = pi * 0.001 * 900^(k / 15), so the grid runs from 0.001 pi to
 * 0.9 pi -- a fixed fraction of the sample rate, not a fixed frequency -- and the cos/sin pairs are
 * exactly what ZBiquad::magSqAt() takes, so a magnitude on the grid costs no transcendental call.
 */
struct NormGrid
{
    float m_c1[Grid],   ///< cos(w) at each grid point
        m_s1[Grid],     ///< sin(w)
        m_c2[Grid],     ///< cos(2w)
        m_s2[Grid];     ///< sin(2w)
    /** @brief Fills the four tables; run once, when grid() is first called. */
    NormGrid()
    {
        for ( int k = 0; k < Grid; ++k )
        {
            const float w = Pi * 0.001f * std::pow( 900.0f, static_cast<float>( k ) / ( Grid - 1 ) );
            m_c1[k] = std::cos( w );
            m_s1[k] = std::sin( w );
            m_c2[k] = std::cos( 2.0f * w );
            m_s2[k] = std::sin( 2.0f * w );
        }
    }
};
/**
 * @brief The one normalisation grid, built on the first call.
 * @return  the grid's cos/sin tables; the same object for the life of the process
 */
const NormGrid &grid()
{
    static const NormGrid g;
    return g;
}
}   // namespace

/**
 * Builds the normalisation grid now, on whoever calls -- prepare() does, from the message thread.
 * Left alone, the first voice to build a cascade did it on the audio thread, inside the guard
 * the language puts around a function-local static: a lock, where no lock belongs.
 */
void zWarmTables()
{
    (void)grid();
}

namespace
{
}

float zBuildCascade( const ZFrame &f, ZBiquad *ch, float sr )
{
    const NormGrid &g = grid();
    const int       n = clampv( f.m_used, 0, ZSections );
    if ( n == 0 ) return 1.0f;
    // The probe points: the fixed grid plus every pole and zero angle of this frame. A needle-
    // sharp resonance has its maximum at its own pole angle, which no fixed grid would catch.
    constexpr int MaxPoints = Grid + 2 * ZSections;
    float         c1[MaxPoints], s1[MaxPoints], c2[MaxPoints], s2[MaxPoints];
    for ( int k = 0; k < Grid; ++k )
    {
        c1[k] = g.m_c1[k];
        s1[k] = g.m_s1[k];
        c2[k] = g.m_c2[k];
        s2[k] = g.m_s2[k];
    }
    int  points = Grid;
    auto addAngle = [&]( float hz )
    {
        if ( hz <= 0.0f || points >= MaxPoints ) return;
        const float w = TwoPi * clampv( hz, 10.0f, sr * 0.48f ) / sr;
        c1[points] = std::cos( w );
        s1[points] = std::sin( w );
        c2[points] = std::cos( 2.0f * w );
        s2[points] = std::sin( 2.0f * w );
        ++points;
    };
    for ( int i = 0; i < n; ++i )
    {
        addAngle( f.m_s[i].m_poleHz );
        addAngle( f.m_s[i].m_zeroHz );
    }
    // One pass gives both normalisations: the row maximum per section, and the product down
    // each column for the finished cascade.
    float mag[ZSections][MaxPoints];
    for ( int i = 0; i < n; ++i )
    {
        ch[i].set( f.m_s[i], sr );
        float rowMax = 1e-20f, logSum = 0.0f;
        for ( int k = 0; k < points; ++k )
        {
            mag[i][k] = std::max( ch[i].magSqAt( c1[k], s1[k], c2[k], s2[k] ), 1e-20f );
            rowMax = std::max( rowMax, mag[i][k] );
            logSum += std::log( mag[i][k] );
        }
        // Normalise the section to its geometric mean over the grid, not to its peak: a bell
        // (zero on the pole, wider) then keeps unity background and boosts only its peak, so
        // six of them in series shape a spectrum instead of cancelling each other out. The
        // boost is capped at 30 dB per section so the cascade cannot overflow.
        const float gmean = std::exp( 0.5f * logSum / static_cast<float>( points ) );
        const float scale = std::min( f.m_s[i].m_gain / gmean, 32.0f / std::sqrt( rowMax ) );
        ch[i].m_b0 *= scale;
        ch[i].m_b1 *= scale;
        ch[i].m_b2 *= scale;
        const float sq = scale * scale;
        for ( int k = 0; k < points; ++k ) mag[i][k] *= sq;
    }
    float peakSq = 1e-20f;
    for ( int k = 0; k < points; ++k )
    {
        float prod = 1.0f;
        for ( int i = 0; i < n; ++i ) prod *= mag[i][k];
        peakSq = std::max( peakSq, prod );
    }
    return clampv( 1.0f / std::sqrt( peakSq ), 1e-4f, 16.0f );
}

void ZBiquad::set( const ZSection &s, float sr )
{
    // Poles: radius from the bandwidth, angle from the centre frequency. The radius is capped
    // well inside the unit circle (about 0.4 s of ringing), so no interpolated point can blow up.
    const float f = clampv( s.m_poleHz, 20.0f, sr * 0.45f );
    const float r = std::min( std::exp( -Pi * std::max( s.m_poleBw, 0.5f ) / sr ), 0.99995f );
    const float th = TwoPi * f / sr;
    m_a1 = -2.0f * r * std::cos( th );
    m_a2 = r * r;
    if ( s.m_zeroHz > 0.0f )
    {
        const float fz = clampv( s.m_zeroHz, 20.0f, sr * 0.48f );
        const float rz = std::min( std::exp( -Pi * std::max( s.m_zeroBw, 0.5f ) / sr ), 0.9999f );
        const float tz = TwoPi * fz / sr;
        m_b0 = 1.0f;
        m_b1 = -2.0f * rz * std::cos( tz );
        m_b2 = rz * rz;
    }
    else
    {
        m_b0 = 1.0f;
        m_b1 = 0.0f;
        m_b2 = 0.0f;
    }
    // The gain is applied by zBuildCascade, which normalises the section over the whole grid.
}

float ZBiquad::magnitudeAt( float w ) const
{
    const float c1 = std::cos( w ), s1 = std::sin( w );
    const float c2 = std::cos( 2.0f * w ), s2 = std::sin( 2.0f * w );
    const float nr = m_b0 + m_b1 * c1 + m_b2 * c2, ni = -( m_b1 * s1 + m_b2 * s2 );
    const float dr = 1.0f + m_a1 * c1 + m_a2 * c2, di = -( m_a1 * s1 + m_a2 * s2 );
    const float den = dr * dr + di * di;
    return std::sqrt( ( nr * nr + ni * ni ) / std::max( den, 1e-20f ) );
}

}   // namespace ambient
