/**
 * @file Modulation.cpp
 * @brief The LFO shapes, the breakpoint envelope and the matrix behind Modulation.h.
 *
 * Modulation.h says what the section is and why it replaced the soldered drifters; this file is the
 * arithmetic and the parsers. The name tables the header declares extern are defined at the top, with
 * the ten envelope shape presets in their text form. The anonymous namespaces hold the LFO's shape
 * functions -- a wrap, a smoothstep, the ramped square and the wavetable reader -- the envelope's
 * segment curve, and the text names of the sources. Lfo::step() and ModEnv::at() run at control rate
 * (one step per 64 samples, Modulation.h) on the audio thread and allocate nothing; ModEnv::parse(),
 * ModMatrix::parse() and the two write() methods are the text forms that travel in the preset string
 * and the plugin state, and run on the message thread. ModMatrix::apply() is the one place where a
 * route's depth meets a parameter's range. The Wavetable of Sources.h is included for the Table
 * shape, which resynthesises a frame's spectrum rather than reading samples.
 * @author Rene Weller
 */
#include "ambient/Modulation.h"
#include "ambient/Sources.h"   // Wavetable, for the Table shape
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ambient
{

const char *const LfoShapeNames[NumLfoShapes] = {
    "Sine", "Triangle", "Ramp Up", "Ramp Down", "Square", "Random", "Steps", "Table",
};
const char *const LfoModeNames[NumLfoModes] = { "Global", "Per Voice", "Retrigger" };
const char *const EnvModeNames[NumEnvModes] = { "One Shot", "Loop", "Sustain Loop" };

/**
 * Written over about four time units, which the Time knob scales; at Time = 1 that is four
 * seconds. ADSR is first because it is what anybody looks for first.
 */
const char *const EnvShapePresetNames[NumEnvShapePresets] = {
    "ADSR",  "AD (percussive)", "AR (swell)", "Ramp up", "Ramp down",
    "Pulse", "Slow swell",      "Two peaks",  "Stepped", "Bipolar sweep",
};
const char *const EnvShapePresetTexts[NumEnvShapePresets] = {
    "0:0/0.4:1:-0.3/1.4:0.6/4:0:-0.3!s2",
    "0:0/0.15:1/2:0:-0.6",
    "0:0/1.5:1:0.2/4:0:-0.2!s1",
    "0:0/4:1",
    "0:1/4:0",
    "0:0/0.05:1/1:1/1.05:0!s2",
    "0:0/2.5:1:0.4/4:0.85/8:0:-0.4!s2",
    "0:0/0.6:1:-0.2/1.6:0.25/2.4:0.9:-0.2/4:0:-0.3",
    "0:0/0.8:0.35/0.81:0.35/1.8:0.7/1.81:0.7/2.8:1/4:0",
    "0:-1/2:1:0.3/4:-1:0.3",
};

namespace
{

/**
 * @brief The fractional part of a phase, so that any value lands in 0 .. 1.
 * @param[in] x  a phase in cycles, of any sign
 * @return x - floor(x)
 */
inline float wrap01( float x )
{
    return x - std::floor( x );
}

/**
 * @brief The cubic 3t^2 - 2t^3 with its input clamped: a ramp with no corner at either end.
 * @param[in] t  0 .. 1; anything outside is clamped
 * @return 0 at 0, 1 at 1, zero slope at both
 */
inline float smoothstep( float t )
{
    t = t < 0.0f ? 0.0f : ( t > 1.0f ? 1.0f : t );
    return t * t * ( 3.0f - 2.0f * t );
}

/**
 * @brief A square whose edges are ramped over 7 % of a cycle.
 *
 * The standing rule of this instrument is
 * that a modulator may not step -- a hard edge on a cutoff is a click -- and the width is chosen
 * so that even at the fastest rate the change per control block stays small.
 * @param[in] p  phase in 0 .. 1, already wrapped
 * @return -1 .. 1, high in the first half of the cycle
 */
inline float softSquare( float p )
{
    const float w = 0.035f;
    if ( p < w ) return -1.0f + 2.0f * smoothstep( ( p + w ) / ( 2.0f * w ) );   // rising edge, centred on 0
    if ( p < 0.5f - w ) return 1.0f;
    if ( p < 0.5f + w ) return 1.0f - 2.0f * smoothstep( ( p - ( 0.5f - w ) ) / ( 2.0f * w ) );
    if ( p < 1.0f - w ) return -1.0f;
    return -1.0f + 2.0f * smoothstep( ( p - ( 1.0f - w ) ) / ( 2.0f * w ) );   // other half of the rising edge
}

/**
 * @brief The Table shape: one frame of a wavetable read as an LFO cycle.
 *
 * Without a table, or with an empty one, it falls back to a sine, so a preset that names a table the
 * host has not loaded still moves.
 * @param[in] table    the user wavetable, or null
 * @param[in] frame    which frame of it, wrapped into the table's frame count
 * @param[in] phase01  phase in 0 .. 1
 * @return the shape at that phase, about -1 .. 1 after the normalisation in the body
 */
float tableAt( const Wavetable *table, int frame, float phase01 )
{
    if ( table == nullptr || table->m_frames <= 0 ) return std::sin( TwoPi * phase01 );
    float       spec[TablePartials];
    const float pos = table->m_frames > 1
                          ? static_cast<float>( frame % table->m_frames ) / static_cast<float>( table->m_frames - 1 )
                          : 0.0f;
    table->spectrumAt( pos, spec );
    // The table keeps a spectrum, not samples, so the shape is resynthesised. Thirty-two partials
    // at control rate is nothing, and it means any of the generated wavetables -- or any curve
    // drawn into one -- is an LFO shape without a second mechanism for drawable modulators.
    float v = 0.0f, norm = 0.0f;
    for ( int h = 0; h < TablePartials; ++h )
    {
        if ( spec[h] <= 1.0e-5f ) continue;
        v += spec[h] * std::sin( TwoPi * static_cast<float>( h + 1 ) * phase01 );
        norm += spec[h];
    }
    return norm > 1.0e-6f ? v / norm * 1.6f : 0.0f;
}

}   // namespace

// ---------------------------------------------------------------- LFO

void Lfo::reset( uint64_t seed, float phase01 )
{
    m_rng.seed( seed );
    m_phase = wrap01( phase01 );
    m_prev = m_rng.bipolar();
    m_next = m_rng.bipolar();
    for ( float &v : m_steps ) v = m_rng.bipolar();
    m_value = 0.0f;
}

float Lfo::shapeAt( const LfoSpec &spec, float phase01, const Wavetable *table, const Lfo *live )
{
    const float p = wrap01( phase01 );
    switch ( spec.m_shape )
    {
    case LfoShapeE::SINE: return std::sin( TwoPi * p );
    case LfoShapeE::TRIANGLE: return p < 0.25f ? 4.0f * p : ( p < 0.75f ? 2.0f - 4.0f * p : 4.0f * p - 4.0f );
    case LfoShapeE::RAMP_UP: return 2.0f * p - 1.0f;
    case LfoShapeE::RAMP_DOWN: return 1.0f - 2.0f * p;
    case LfoShapeE::SQUARE: return softSquare( p );
    case LfoShapeE::RANDOM:
    {   // smoothstep between two random values, one pair per cycle
        const float a = live ? live->m_prev : -0.5f, b = live ? live->m_next : 0.5f;
        return a + ( b - a ) * smoothstep( p );
    }
    case LfoShapeE::STEP_RANDOM:
    {   // four holds a cycle, each reached with a short ramp
        static const float Fallback[5] = { -0.5f, 0.5f, -0.2f, 0.8f, -0.5f };
        const float       *st = live ? live->m_steps : Fallback;
        const float        q = p * 4.0f;
        const int          k = std::min( 3, static_cast<int>( q ) );
        const float        f = q - static_cast<float>( k );
        return st[k] + ( st[k + 1] - st[k] ) * smoothstep( f / 0.3f );
    }
    case LfoShapeE::TABLE: return tableAt( table, spec.m_table, p );
    default: return 0.0f;
    }
}

float Lfo::step( float dt, const LfoSpec &spec, const Wavetable *table )
{
    const double inc = static_cast<double>( spec.m_rateHz ) * static_cast<double>( dt );
    const double before = m_phase;
    m_phase += inc;
    if ( m_phase >= 1.0 )
    {
        m_phase -= std::floor( m_phase );
        m_prev = m_next;
        m_next = m_rng.bipolar();
        m_steps[0] = m_steps[4];   // the last hold carries over, so the wrap is not a step
        for ( int i = 1; i < 5; ++i ) m_steps[i] = m_rng.bipolar();
    }
    (void)before;
    // A phase just under one rounds to exactly 1.0f on the cast to float, and wrapping that gives
    // zero -- the shape then jumps back to its start one control block before the cycle ends.
    // Measured on Random: a step of 1.994 out of a range of 2.
    float p = static_cast<float>( m_phase );
    if ( p >= 1.0f ) p = 0.99999994f;
    m_value = shapeAt( spec, p + spec.m_phase, table, this ) * spec.m_depth;
    return m_value;
}

// ---------------------------------------------------------------- envelope

bool ModEnv::set( const EnvPoint *points, int count )
{
    if ( points == nullptr || count < 0 || count > MaxEnvPoints ) return false;
    for ( int i = 1; i < count; ++i )
        if ( points[i].m_time < points[i - 1].m_time ) return false;
    for ( int i = 0; i < count; ++i ) m_points[i] = points[i];
    m_count = count;
    if ( m_sustain >= m_count ) m_sustain = -1;
    if ( m_loopFrom >= m_count || m_loopTo >= m_count )
    {
        m_loopFrom = m_loopTo = -1;
    }
    return true;
}

namespace
{
/**
 * @brief A segment with a curve: 0 is linear, positive dwells at the start, negative at the end.
 * @param[in] t      position in the segment, 0 .. 1; clamped
 * @param[in] curve  EnvPoint::curve of the segment's first point, -1 .. 1
 * @return the shaped position, 0 .. 1, a power of @p t
 */
float shape( float t, float curve )
{
    t = t < 0.0f ? 0.0f : ( t > 1.0f ? 1.0f : t );
    if ( std::fabs( curve ) < 1.0e-4f ) return t;
    const float k = curve > 0.0f ? 1.0f + 4.0f * curve : 1.0f / ( 1.0f - 4.0f * curve );
    return std::pow( t, k );
}
}   // namespace

float ModEnv::at( float seconds, EnvModeE mode, bool held ) const
{
    if ( m_count <= 0 ) return 0.0f;
    if ( m_count == 1 ) return m_points[0].m_value;
    const float end = m_points[m_count - 1].m_time;

    float      t = seconds < 0.0f ? 0.0f : seconds;
    const bool looping = ( mode == EnvModeE::LOOP ) || ( mode == EnvModeE::SUSTAIN_LOOP && held );
    if ( looping && m_loopFrom >= 0 && m_loopTo > m_loopFrom && m_loopTo < m_count )
    {
        const float a = m_points[m_loopFrom].m_time, b = m_points[m_loopTo].m_time;
        if ( b > a && t > b ) t = a + std::fmod( t - a, b - a );
    }
    else if ( mode == EnvModeE::LOOP && end > 0.0f && t > end )
    {
        t = std::fmod( t, end );
    }
    else if ( mode == EnvModeE::SUSTAIN_LOOP && held && m_sustain >= 0 && m_sustain < m_count )
    {
        t = std::min( t, m_points[m_sustain].m_time );
    }
    else if ( t > end )
    {
        return m_points[m_count - 1].m_value;
    }

    int i = 0;
    while ( i + 2 < m_count && m_points[i + 1].m_time <= t ) ++i;
    const EnvPoint &a = m_points[i];
    const EnvPoint &b = m_points[i + 1];
    const float     span = b.m_time - a.m_time;
    const float     u = span > 1.0e-6f ? ( t - a.m_time ) / span : 1.0f;
    return a.m_value + ( b.m_value - a.m_value ) * shape( u, a.m_curve );
}

float ModEnv::readTime( float seconds, EnvModeE mode, bool held ) const
{
    if ( m_count <= 1 ) return 0.0f;
    const float end = m_points[m_count - 1].m_time;
    float       t = seconds < 0.0f ? 0.0f : seconds;
    // The branches of at(), in the same order, so the two cannot disagree about which one applies.
    const bool looping = ( mode == EnvModeE::LOOP ) || ( mode == EnvModeE::SUSTAIN_LOOP && held );
    if ( looping && m_loopFrom >= 0 && m_loopTo > m_loopFrom && m_loopTo < m_count )
    {
        const float a = m_points[m_loopFrom].m_time, b = m_points[m_loopTo].m_time;
        if ( b > a && t > b ) t = a + std::fmod( t - a, b - a );
    }
    else if ( mode == EnvModeE::LOOP && end > 0.0f && t > end )
    {
        t = std::fmod( t, end );
    }
    else if ( mode == EnvModeE::SUSTAIN_LOOP && held && m_sustain >= 0 && m_sustain < m_count )
    {
        t = std::min( t, m_points[m_sustain].m_time );
    }
    return std::min( t, end );
}

bool ModEnv::parse( const char *text )
{
    if ( text == nullptr ) return false;
    EnvPoint    pts[MaxEnvPoints];
    int         n = 0, sus = -1, from = -1, to = -1;
    const char *s = text;
    while ( *s && *s != '!' )
    {
        if ( n >= MaxEnvPoints ) return false;
        char *endp = nullptr;
        pts[n].m_time = static_cast<float>( std::strtod( s, &endp ) );
        if ( endp == s || *endp != ':' || !std::isfinite( pts[n].m_time ) ) return false;
        s = endp + 1;
        pts[n].m_value = static_cast<float>( std::strtod( s, &endp ) );
        if ( endp == s || !std::isfinite( pts[n].m_value ) ) return false;
        s = endp;
        pts[n].m_curve = 0.0f;
        if ( *s == ':' )
        {
            ++s;
            pts[n].m_curve = static_cast<float>( std::strtod( s, &endp ) );
            if ( endp == s || !std::isfinite( pts[n].m_curve ) ) return false;
            s = endp;
        }
        ++n;
        if ( *s == '/' )
            ++s;
        else
            break;
    }
    while ( *s == '!' )
    {
        ++s;
        if ( *s == 's' )
            sus = std::atoi( s + 1 );
        else if ( *s == 'l' )
        {
            from = std::atoi( s + 1 );
            const char *dash = std::strchr( s, '-' );
            if ( dash != nullptr ) to = std::atoi( dash + 1 );
        }
        while ( *s && *s != '!' ) ++s;
    }
    if ( !set( pts, n ) ) return false;
    m_sustain = ( sus >= 0 && sus < m_count ) ? sus : -1;
    if ( from >= 0 && to > from && to < m_count )
    {
        m_loopFrom = from;
        m_loopTo = to;
    }
    else
    {
        m_loopFrom = m_loopTo = -1;
    }
    return true;
}

int ModEnv::write( char *buf, size_t cap ) const
{
    if ( buf == nullptr || cap == 0 ) return 0;
    int len = 0;
    for ( int i = 0; i < m_count; ++i )
    {
        const int w = std::snprintf( buf + len, cap - static_cast<size_t>( len ), "%s%.4g:%.4g:%.3g", i ? "/" : "",
                                     m_points[i].m_time, m_points[i].m_value, m_points[i].m_curve );
        if ( w < 0 || static_cast<size_t>( len + w ) >= cap ) return 0;
        len += w;
    }
    if ( m_sustain >= 0 )
    {
        const int w = std::snprintf( buf + len, cap - static_cast<size_t>( len ), "!s%d", m_sustain );
        if ( w < 0 || static_cast<size_t>( len + w ) >= cap ) return 0;
        len += w;
    }
    if ( m_loopFrom >= 0 && m_loopTo > m_loopFrom )
    {
        const int w = std::snprintf( buf + len, cap - static_cast<size_t>( len ), "!l%d-%d", m_loopFrom, m_loopTo );
        if ( w < 0 || static_cast<size_t>( len + w ) >= cap ) return 0;
        len += w;
    }
    return len;
}

// ---------------------------------------------------------------- matrix

namespace
{
/**
 * @brief The text-form names of the sources, in the order of ModSource: modSourceName() and
 * modSourceFromName() read this table both ways, so a row of the matrix survives in a preset string.
 */
const char *const SourceNames[NumModSources] = {
    "none",      "lfo1",    "lfo2",     "lfo3",     "lfo4",     "lfo5",     "lfo6",     "lfo7",      "lfo8",
    "env1",      "env2",    "env3",     "env4",     "env5",     "env6",     "amp",      "macro_a",   "macro_b",
    "macro_c",   "macro_d", "macro_e",  "macro_f",  "macro_g",  "macro_h",  "kura1",    "kura2",     "kura3",
    "kura4",     "note",    "velocity", "distance", "random",   "beat",     "pressure", "wheel",     "slide",
    "lenia1",    "lenia2",  "lenia3",   "lenia4",   "lorenz_x", "lorenz_y", "lorenz_z", "rossler_x", "rossler_y",
    "rossler_z", "cascade", "root_age", "layer",    "section",
};
}   // namespace

const char *modSourceName( ModSourceE s )
{
    const int i = static_cast<int>( s );
    return ( i >= 0 && i < NumModSources ) ? SourceNames[i] : "none";
}

bool modSourceFromName( const char *name, ModSourceE &out )
{
    if ( name == nullptr ) return false;
    for ( int i = 0; i < NumModSources; ++i )
        if ( std::strcmp( name, SourceNames[i] ) == 0 )
        {
            out = static_cast<ModSourceE>( i );
            return true;
        }
    return false;
}

bool ModMatrix::remove( int i )
{
    if ( i < 0 || i >= m_count ) return false;
    for ( int k = i; k + 1 < m_count; ++k ) m_routes[k] = m_routes[k + 1];
    --m_count;
    return true;
}

bool ModMatrix::parse( const char *text )
{
    if ( text == nullptr ) return false;
    ModRoute    rows[MaxModRoutes];
    int         n = 0;
    const char *s = text;
    while ( *s )
    {
        while ( *s == ' ' || *s == ';' ) ++s;
        if ( !*s ) break;
        if ( n >= MaxModRoutes ) return false;
        char src[24] = {}, tgt[48] = {};
        int  i = 0;
        while ( *s && *s != '>' && i < 23 ) src[i++] = *s++;
        if ( *s != '>' ) return false;
        ++s;
        i = 0;
        while ( *s && *s != ':' && *s != ';' && i < 47 ) tgt[i++] = *s++;
        ModRoute r;
        if ( !modSourceFromName( src, r.m_source ) ) return false;
        const ParamDesc *d = findParam( tgt );
        if ( d == nullptr ) return false;
        r.m_target = d->m_id;
        if ( *s == ':' )
        {
            char *endp = nullptr;
            r.m_depth = static_cast<float>( std::strtod( s + 1, &endp ) );
            if ( endp == s + 1 || !std::isfinite( r.m_depth ) ) return false;   // a NaN depth would poison every target
            s = endp;
        }
        while ( *s == ':' )
        {   // optional via source and the "u" flag, in any order
            ++s;
            char tok[24] = {};
            i = 0;
            while ( *s && *s != ':' && *s != ';' && i < 23 ) tok[i++] = *s++;
            if ( std::strcmp( tok, "u" ) == 0 )
                r.m_unipolar = true;
            else if ( !modSourceFromName( tok, r.m_via ) )
                return false;
        }
        rows[n++] = r;
        while ( *s && *s != ';' ) ++s;
    }
    for ( int k = 0; k < n; ++k ) m_routes[k] = rows[k];
    m_count = n;
    return true;
}

int ModMatrix::write( char *buf, size_t cap ) const
{
    if ( buf == nullptr || cap == 0 ) return 0;
    int len = 0;
    for ( int i = 0; i < m_count; ++i )
    {
        const ModRoute &r = m_routes[i];
        int             w = std::snprintf( buf + len, cap - static_cast<size_t>( len ), "%s%s>%s:%.4g", i ? ";" : "",
                                           modSourceName( r.m_source ), paramDesc( r.m_target ).m_key, r.m_depth );
        if ( w < 0 || static_cast<size_t>( len + w ) >= cap ) return 0;
        len += w;
        if ( r.m_via != ModSourceE::NONE )
        {
            w = std::snprintf( buf + len, cap - static_cast<size_t>( len ), ":%s", modSourceName( r.m_via ) );
            if ( w < 0 || static_cast<size_t>( len + w ) >= cap ) return 0;
            len += w;
        }
        if ( r.m_unipolar )
        {
            w = std::snprintf( buf + len, cap - static_cast<size_t>( len ), ":u" );
            if ( w < 0 || static_cast<size_t>( len + w ) >= cap ) return 0;
            len += w;
        }
    }
    return len;
}

void ModMatrix::apply( const float *sourceValues, float *out ) const
{
    if ( sourceValues == nullptr || out == nullptr ) return;
    for ( int i = 0; i < m_count; ++i )
    {
        const ModRoute &r = m_routes[i];
        if ( r.m_source == ModSourceE::NONE ) continue;
        float v = sourceValues[static_cast<int>( r.m_source )];
        if ( r.m_unipolar ) v = 0.5f * ( v + 1.0f );
        float depth = r.m_depth;
        if ( r.m_via != ModSourceE::NONE )
        {
            const float a = sourceValues[static_cast<int>( r.m_via )];
            depth *= 0.5f * ( a + 1.0f );   // the via source always acts as an amount, 0..1
        }
        // Depth is a fraction of the target's own range, so one number means the same thing on a
        // cutoff in hertz and on a mix in 0..1.
        const ParamDesc &d = paramDesc( r.m_target );
        out[static_cast<int>( r.m_target )] += v * depth * ( d.m_max - d.m_min );
    }
}

}   // namespace ambient
