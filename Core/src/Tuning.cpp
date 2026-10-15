/**
 * @file Tuning.cpp
 * @brief The tuning tables and the arithmetic over them: built-in scales, the Scala reader,
 *        note-to-frequency and interval consonance.
 *
 * The built-in scales are just-intonation tables written as ratios over the root, one row per
 * scale, in the order of ScaleNames (Params.h) -- a static_assert keeps the two in step. 12-TET is
 * the one exception and is computed rather than tabulated. makeBuiltinScale() copies a row into a
 * FixedScale, the fixed-size form that can be swapped into the audio thread without allocating;
 * parseScala() fills the same form from the text of a .scl file, deciding cents against ratio by
 * the value alone and tolerating the empty description line the format allows.
 *
 * scaleFrequency() is the one function the voices call: MIDI note in, hertz out, with the root key
 * sounding ratios[0] at its equal-tempered pitch and the keyboard either snapped to the nearest
 * degree (twelve keys an octave, for octave-periodic scales) or walked degree by degree.
 * intervalConsonance() ranks a ratio by its harmonic entropy, read from a table of one value per
 * cent (HarmonicEntropy.inc, since 25.09.2026; before, the Tenney height of the nearest simple
 * fraction within ten cents); the cluster brain uses it to prefer the intervals a just scale makes
 * consonant.
 * @author Rene Weller
 */
#include "ambient/Tuning.h"
#include "ambient/Params.h"
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>

namespace ambient
{

namespace
{

/** @brief One built-in scale as the table describes it: a name, the repeating interval and the ratios of its degrees. */
struct BuiltinDef
{
    const char   *m_name;     ///< the display name, the same text as in ScaleNames
    double        m_period;   ///< ratio of the repeating interval: 2 for an octave, 3 for the Bohlen-Pierce tritave
    int           m_n;        ///< degrees per period, the period itself excluded
    const double *m_ratios;   ///< the n ratios over the root, ascending from 1; nullptr for 12-TET, which is computed
};

/**
 * @name The ratio tables of the built-in scales
 * Ratios exclude the period; ratios[0] must be 1.
 * @{ */
const double Major[] = {
    1, 9.0 / 8, 5.0 / 4, 4.0 / 3, 3.0 / 2, 5.0 / 3, 15.0 / 8
};   ///< JI Major (Ptolemy's intense diatonic), 5-limit
const double Minor[] = {
    1, 9.0 / 8, 6.0 / 5, 4.0 / 3, 3.0 / 2, 8.0 / 5, 9.0 / 5
};   ///< JI Minor, the natural minor in 5-limit ratios
const double SevenLimit[] = {
    1, 9.0 / 8, 7.0 / 6, 5.0 / 4, 4.0 / 3, 7.0 / 5, 3.0 / 2, 8.0 / 5, 5.0 / 3, 7.0 / 4, 15.0 / 8
};   ///< JI 7-limit: eleven degrees with the septimal third, tritone and seventh
const double Pythag[] = {
    1, 9.0 / 8, 81.0 / 64, 4.0 / 3, 3.0 / 2, 27.0 / 16, 243.0 / 128
};   ///< Pythagorean: every degree a stack of pure fifths
const double Penta[] = { 1, 9.0 / 8, 5.0 / 4, 3.0 / 2,
                         5.0 / 3 };   ///< JI Pentatonic, the major pentatonic in 5-limit ratios
const double Harm[] = {
    1, 9.0 / 8, 5.0 / 4, 11.0 / 8, 3.0 / 2, 13.0 / 8, 7.0 / 4, 15.0 / 8
};   ///< Harmonic 8-16: harmonics 8 to 15 over the eighth
const double Subharm[] = {
    1, 16.0 / 15, 8.0 / 7, 16.0 / 13, 4.0 / 3, 16.0 / 11, 8.0 / 5, 16.0 / 9
};   ///< Subharmonic 16-8: the harmonic series turned upside down
const double Slendro[] = { 1, 8.0 / 7, 21.0 / 16, 3.0 / 2,
                           7.0 / 4 };   ///< Slendro (JI): a just reading of the five-tone gamelan scale
const double BP[] = {
    1,       27.0 / 25, 25.0 / 21, 9.0 / 7, 7.0 / 5,   75.0 / 49, 5.0 / 3,
    9.0 / 5, 49.0 / 25, 15.0 / 7,  7.0 / 3, 63.0 / 25, 25.0 / 9
};   ///< Bohlen-Pierce (JI): thirteen degrees repeating at the tritave (period 3)
const double Otonal[] = {
    1, 9.0 / 8, 5.0 / 4, 11.0 / 8, 3.0 / 2, 7.0 / 4
};   ///< Otonality 1-11: harmonics 8, 9, 10, 11, 12 and 14 over the eighth
/** @} */

/**
 * @brief Builds a BuiltinDef from a ratio table, taking the degree count from the array's size so
 *        that it cannot be miscounted.
 * @param[in] name    the display name, the same text as in ScaleNames
 * @param[in] period  ratio of the repeating interval (2 = octave, 3 = tritave)
 * @param[in] r       the ratio table, ratios[0] == 1, ascending, all below the period
 * @return the definition Builtins holds
 * @tparam N      the number of ratios in the table, deduced
 */
template <size_t N>
constexpr BuiltinDef def( const char *name, double period, const double ( &r )[N] )
{
    return { name, period, static_cast<int>( N ), r };
}

/**
 * @brief The built-in scales in the order of ScaleNames (Params.h): 12-TET first, computed, then
 *        the ten just tables above.
 */
const BuiltinDef Builtins[] = {
    { "12-TET", 2.0, 12, nullptr },       def( "JI Major (Ptolemy)", 2.0, Major ), def( "JI Minor", 2.0, Minor ),
    def( "JI 7-limit", 2.0, SevenLimit ), def( "Pythagorean", 2.0, Pythag ),       def( "JI Pentatonic", 2.0, Penta ),
    def( "Harmonic 8-16", 2.0, Harm ),    def( "Subharmonic 16-8", 2.0, Subharm ), def( "Slendro (JI)", 2.0, Slendro ),
    def( "Bohlen-Pierce (JI)", 3.0, BP ), def( "Otonality 1-11", 2.0, Otonal ),
};
constexpr int NumBuiltins = static_cast<int>(
    sizeof( Builtins ) /
    sizeof(
        Builtins
            [0] ) );   ///< how many scales Builtins holds; must equal UserScaleIndex, the first choice that is not a table
static_assert( NumBuiltins == UserScaleIndex, "scale name table and built-in table out of sync" );

/**
 * @brief Copies a name into the scale's fixed buffer, truncated to fit and always NUL-terminated.
 * @param[in,out] s     the scale whose name is set
 * @param[in]     name  the text to copy; longer than FixedScale::name holds, it is cut
 */
void copyName( FixedScale &s, const char *name )
{
    std::strncpy( s.m_name, name, sizeof( s.m_name ) - 1 );
    s.m_name[sizeof( s.m_name ) - 1] = 0;
}

}   // namespace

bool makeBuiltinScale( int index, FixedScale &out )
{
    if ( index < 0 || index >= NumBuiltins ) return false;
    const BuiltinDef &d = Builtins[index];
    out.m_period = d.m_period;
    out.m_count = d.m_n;
    if ( d.m_ratios == nullptr )
    {
        for ( int i = 0; i < 12; ++i ) out.m_ratios[i] = std::pow( 2.0, i / 12.0 );
    }
    else
    {
        for ( int i = 0; i < d.m_n; ++i ) out.m_ratios[i] = d.m_ratios[i];
    }
    copyName( out, d.m_name );
    return true;
}

bool parseScala( const char *text, FixedScale &out )
{
    if ( text == nullptr ) return false;
    FixedScale s;
    int        lineState = 0;   // 0 = description, 1 = count, 2+ = pitches
    int        expected = 0, got = 0;
    double     values[FixedScale::Max + 1];

    const char *p = text;
    while ( *p )
    {
        const char *end = p;
        while ( *end && *end != '\n' ) ++end;
        // trim
        const char *a = p;
        while ( a < end && ( *a == ' ' || *a == '\t' || *a == '\r' ) ) ++a;
        // The description may be an empty line -- the format allows it and many files have one.
        // Skipping blank lines in every state turned the count line into the name and the first
        // pitch into the count, and a valid file was refused.
        if ( a >= end && lineState == 0 )
        {
            copyName( s, "" );
            lineState = 1;
        }
        if ( a < end && *a != '!' )
        {
            char   line[256];
            size_t len = static_cast<size_t>( end - a );
            if ( len >= sizeof( line ) ) len = sizeof( line ) - 1;
            std::memcpy( line, a, len );
            line[len] = 0;
            if ( lineState == 0 )
            {
                copyName( s, line );
                lineState = 1;
            }
            else if ( lineState == 1 )
            {
                expected = std::atoi( line );
                if ( expected < 1 || expected > FixedScale::Max ) return false;
                lineState = 2;
            }
            else if ( got < expected )
            {
                double v = 0.0;
                char  *endp = nullptr;
                // Cents or ratio is decided by the value alone, not by the comment that may
                // follow it on the same line: "3/2 pure fifth (approx. 702c)" is a ratio.
                size_t vend = 0;
                while ( line[vend] != 0 && line[vend] != ' ' && line[vend] != '\t' ) ++vend;
                if ( std::memchr( line, '.', vend ) != nullptr )
                {
                    double cents = std::strtod( line, &endp );
                    v = std::pow( 2.0, cents / 1200.0 );
                }
                else
                {
                    long num = std::strtol( line, &endp, 10 );
                    long den = 1;
                    if ( endp && *endp == '/' ) den = std::strtol( endp + 1, nullptr, 10 );
                    if ( num <= 0 || den <= 0 ) return false;
                    v = static_cast<double>( num ) / static_cast<double>( den );
                }
                if ( !( v > 0.0 ) ) return false;
                values[got++] = v;
            }
        }
        p = ( *end ) ? end + 1 : end;
    }
    if ( lineState < 2 || got != expected ) return false;

    // Scala lists pitches above 1/1 and ends with the period.
    s.m_period = values[expected - 1];
    s.m_count = expected;
    s.m_ratios[0] = 1.0;
    for ( int i = 1; i < expected; ++i ) s.m_ratios[i] = values[i - 1];
    if ( s.m_period <= 1.0 ) return false;
    if ( s.m_name[0] == 0 ) copyName( s, "Scala" );
    out = s;
    return true;
}

double scaleFrequency( const FixedScale &s, int midiNote, int rootNote, double refA4, bool snapToKeys )
{
    const double rootFreq = refA4 * std::pow( 2.0, ( rootNote - 69 ) / 12.0 );
    const int    n = s.m_count > 0 ? s.m_count : 1;
    const int    k = midiNote - rootNote;
    const bool   octavePeriodic = std::fabs( s.m_period - 2.0 ) < 1e-9;
    if ( snapToKeys && octavePeriodic )
    {
        const int    oct = ( k >= 0 ) ? k / 12 : -( ( -k + 11 ) / 12 );
        const int    semis = k - oct * 12;
        const double target = std::pow( 2.0, semis / 12.0 );
        double       best = s.m_ratios[0], bestErr = 1e9;
        for ( int d = 0; d <= n; ++d )
        {
            const double r = ( d < n ) ? s.m_ratios[d] : s.m_period;   // degree 0 of the next octave is a candidate too
            const double err = std::fabs( std::log2( r / target ) );
            if ( err < bestErr )
            {
                bestErr = err;
                best = r;
            }
        }
        return rootFreq * std::pow( 2.0, oct ) * best;
    }
    const int oct = ( k >= 0 ) ? k / n : -( ( -k + n - 1 ) / n );
    const int deg = k - oct * n;
    return rootFreq * std::pow( s.m_period, oct ) * s.m_ratios[deg];
}

namespace
{
#include "HarmonicEntropy.inc"
}

double intervalConsonance( double ratio )
{
    if ( !( ratio > 0.0 ) || !( ratio < 1.0e30 ) ) return 0.0;
    while ( ratio >= 2.0 ) ratio *= 0.5;
    while ( ratio < 1.0 ) ratio *= 2.0;
    // Harmonic entropy, cent by cent (25.09.2026; the table and how it was made are in
    // Tools/make_harmonic_entropy.py). It used to be the Tenney height of the simplest ratio within
    // ten cents, which is a step function: a tempered major third, fourteen cents from 5/4, fell
    // from 0.19 to 0.11 and was judged as rough as a semitone, and a chord under Purity Drift
    // jumped between neighbouring ratios. The entropy agrees with the old score at the just
    // intervals within a few hundredths and moves smoothly in between.
    const double cents = 1200.0 * std::log2( ratio );
    const int    i = static_cast<int>( cents );
    if ( i >= 1200 ) return HarmonicEntropy[1200];
    const double f = cents - static_cast<double>( i );
    return static_cast<double>( HarmonicEntropy[i] ) +
           f * static_cast<double>( HarmonicEntropy[i + 1] - HarmonicEntropy[i] );
}

}   // namespace ambient
