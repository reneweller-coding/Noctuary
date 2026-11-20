/**
 * @file NightDirector.cpp
 * @brief The night director (NightDirector.h); its table of the library is generated (NightDirector.inc).
 * @author Rene Weller
 */
#include "ambient/NightDirector.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace ambient
{

namespace
{
/** @brief A pack of the table: its title and the family of artists it belongs to (make_journeys.py). */
struct NightPackS
{
    const char *m_title;    ///< the pack's title
    const char *m_family;   ///< sleep, deep, ritual, cold, brit, luminous or space
};
/** @brief A measured preset of the table (Tools/make_night.py). */
struct NightRowS
{
    const char *m_name;          ///< the preset's name
    short       m_pack;          ///< index into NightPacks
    short       m_still;         ///< stillness, z-score x 1000 (lower is stiller)
    short       m_bright;        ///< brightness, z-score x 1000
    short       m_rough;         ///< roughness, z-score x 1000
    short       m_moving;        ///< its own motion, z-score x 1000
    signed char m_feature[18];   ///< the journeys' descriptors, z-scores x 20
};
#include "NightDirector.inc"
constexpr int Rows = static_cast<int>( sizeof( NightRows ) / sizeof( NightRows[0] ) );     ///< presets in the table
constexpr int Packs = static_cast<int>( sizeof( NightPacks ) / sizeof( NightPacks[0] ) );   ///< packs in the table
constexpr double FeatureScale = 20.0;   ///< a feature byte's step is 1/20 of a spread

/** @brief The families of packs, in make_journeys.py's order: the key in the table and the name shown. */
constexpr const char *Families[][2] = { { "sleep", "Sleep" },     { "deep", "Deep" },         { "ritual", "Ritual" },
                                        { "cold", "Cold" },       { "brit", "British" },      { "luminous", "Luminous" },
                                        { "space", "Space" } };
constexpr int         NumFamilies = static_cast<int>( sizeof( Families ) / sizeof( Families[0] ) );   ///< 7

constexpr double Pi = 3.14159265358979323846;   ///< pi
// The curve (NightDirector.h), percentiles of the candidates.
constexpr double StillEvening = 0.55;    ///< the stillness the night begins at
constexpr double StillDeep = 0.06;       ///< the deep
constexpr double StillWave = 0.30;       ///< a wave's crest above the deep (the last wave's; the first 0.6 of it)
constexpr double StillMorning = 0.65;    ///< the stillness the night ends at
constexpr double BrightEvening = 0.50;   ///< the brightness the night begins at
constexpr double BrightDeep = 0.12;      ///< the deep's
constexpr double BrightWave = 0.15;      ///< a wave's crest above it
constexpr double BrightMorning = 0.85;   ///< the brightness the night ends at
constexpr double PhaseShare = 0.08;      ///< falling asleep and the morning, each a share of the night ...
constexpr double PhaseLeast = 30.0;      ///< ... 30 minutes at the least ...
constexpr double PhaseMost = 45.0;       ///< ... and 45 at the most (and a quarter of the night at the most)
constexpr double WaveMinutes = 90.0;     ///< a wave of the middle, about
// The choice.
constexpr double BrightWeight = 0.7;   ///< the brightness's distance from the curve counts this much of the stillness's
constexpr double SoundWeight = 0.3;    ///< the distance in sound from the step before, against the curve's
constexpr double SoundScale = 5.7;     ///< the distance in sound divided by this (a random pair's median, 5.69)
constexpr double PreferBonus = 0.1;    ///< a preferred preset's cost lowered by this
constexpr double Dice = 0.04;          ///< a die of up to this on every candidate's cost
constexpr double RoughGate = 0.6;      ///< roughness percentile above which a preset is kept out of the falling asleep
constexpr double DeepGate = 0.15;      ///< ... and out of the middle where the curve's stillness is below this
// The stays (make_journeys.py's nights): short, middle, long -- dwell and fade ranges in seconds.
constexpr double DwellLo[3] = { 180.0, 360.0, 600.0 };   ///< the shortest dwell of each class
constexpr double DwellHi[3] = { 360.0, 600.0, 900.0 };   ///< the longest
constexpr double FadeLo[3] = { 45.0, 60.0, 90.0 };       ///< the shortest crossfade of each class
constexpr double FadeHi[3] = { 90.0, 120.0, 150.0 };     ///< the longest

/**
 * @brief Half a cosine from a to b.
 * @param[in] a  the value at u = 0
 * @param[in] b  the value at u = 1
 * @param[in] u  0 .. 1 (clamped)
 * @return the eased value
 */
double ease( double a, double b, double u )
{
    u = std::clamp( u, 0.0, 1.0 );
    return a + ( b - a ) * ( 0.5 - 0.5 * std::cos( Pi * u ) );
}

/**
 * @brief Percentiles of values: (rank + 0.5) / n, ties broken by the index.
 * @param[in] v  the values
 * @return one percentile per value
 */
std::vector<double> percentiles( const std::vector<double> &v )
{
    std::vector<size_t> order( v.size() );
    std::iota( order.begin(), order.end(), size_t{ 0 } );
    std::stable_sort( order.begin(), order.end(), [&]( size_t a, size_t b ) { return v[a] < v[b]; } );
    std::vector<double> out( v.size() );
    for ( size_t r = 0; r < order.size(); ++r )
        out[order[r]] = ( static_cast<double>( r ) + 0.5 ) / static_cast<double>( order.size() );
    return out;
}

/**
 * @brief Whether a row belongs to a choice.
 * @param[in] row     the row
 * @param[in] choice  0 every pack, 1 .. NumFamilies a family, then a pack each
 * @return true then
 */
bool inChoice( int row, int choice )
{
    if ( choice <= 0 || choice >= numNightChoices() ) return true;
    const NightPackS &p = NightPacks[NightRows[row].m_pack];
    if ( choice <= NumFamilies ) return std::strcmp( p.m_family, Families[choice - 1][0] ) == 0;
    return NightRows[row].m_pack == choice - 1 - NumFamilies;
}
}   // namespace

int numNightChoices()
{
    return 1 + NumFamilies + Packs;
}

std::string nightChoiceName( int choice )
{
    if ( choice == 0 ) return "Every pack";
    if ( choice >= 1 && choice <= NumFamilies ) return std::string( "Family: " ) + Families[choice - 1][1];
    if ( choice > NumFamilies && choice < numNightChoices() )
        return std::string( "Pack: " ) + NightPacks[choice - 1 - NumFamilies].m_title;
    return {};
}

void nightPhases( double hours, double &asleep, double &morning, int &waves )
{
    const double minutes = std::max( hours, 0.25 ) * 60.0;
    const double phase = std::min( std::clamp( PhaseShare * minutes, PhaseLeast, PhaseMost ), 0.25 * minutes );
    asleep = morning = phase / minutes;
    waves = std::max( 1, static_cast<int>( std::lround( ( minutes - 2.0 * phase ) / WaveMinutes ) ) );
}

void nightCurve( double x, double hours, double &still, double &bright )
{
    double asleep = 0.0, morning = 0.0;
    int    waves = 1;
    nightPhases( hours, asleep, morning, waves );
    x = std::clamp( x, 0.0, 1.0 );
    if ( x < asleep )
    {
        still = ease( StillEvening, StillDeep, x / asleep );
        bright = ease( BrightEvening, BrightDeep, x / asleep );
        return;
    }
    const double end = 1.0 - morning;
    if ( x > end )
    {
        still = ease( StillDeep, StillMorning, ( x - end ) / morning );
        bright = ease( BrightDeep, BrightMorning, ( x - end ) / morning );
        return;
    }
    // The middle: waves from the deep and back, mostly near the deep, the later ones a little higher.
    const double u = ( x - asleep ) / std::max( end - asleep, 1e-9 ) * waves;
    const double f = u - std::floor( u );
    const double w = std::pow( 0.5 - 0.5 * std::cos( 2.0 * Pi * f ), 2.0 );
    const double grow = 0.6 + 0.4 * std::clamp( u / waves, 0.0, 1.0 );
    still = StillDeep + grow * StillWave * w;
    bright = BrightDeep + grow * BrightWave * w;
}

int nightRowCount()
{
    return Rows;
}

const char *nightRowName( int row )
{
    return row >= 0 && row < Rows ? NightRows[row].m_name : "";
}

const char *nightRowPack( int row )
{
    return row >= 0 && row < Rows ? NightPacks[NightRows[row].m_pack].m_title : "";
}

int nightRowOf( const std::string &name )
{
    static std::once_flag                       once;
    static std::unordered_map<std::string, int> index;
    std::call_once( once,
                    []
                    {
                        index.reserve( static_cast<size_t>( Rows ) );
                        for ( int r = 0; r < Rows; ++r ) index.emplace( NightRows[r].m_name, r );
                    } );
    const auto it = index.find( name );
    return it == index.end() ? -1 : it->second;
}

bool nightMeasures( int row, float &still, float &bright, float &rough, float &moving )
{
    if ( row < 0 || row >= Rows ) return false;
    still = NightRows[row].m_still * 0.001f;
    bright = NightRows[row].m_bright * 0.001f;
    rough = NightRows[row].m_rough * 0.001f;
    moving = NightRows[row].m_moving * 0.001f;
    return true;
}

float nightDistance( int a, int b )
{
    if ( a < 0 || b < 0 || a >= Rows || b >= Rows ) return 0.0f;
    int s = 0;
    for ( int k = 0; k < 18; ++k )
    {
        const int d = NightRows[a].m_feature[k] - NightRows[b].m_feature[k];
        s += d * d;
    }
    return static_cast<float>( std::sqrt( static_cast<double>( s ) ) / FeatureScale );
}

NightPlan composeNight( const NightRequest &request )
{
    NightPlan    plan;
    const double hours = std::clamp( request.m_hours, 1.0, 14.0 );
    const double total = hours * 3600.0;
    double       asleep = 0.0, morning = 0.0;
    int          waves = 1;
    nightPhases( hours, asleep, morning, waves );

    // The candidates: the choice's presets that can be played, none excluded.
    const std::unordered_set<std::string> excluded( request.m_exclude.begin(), request.m_exclude.end() );
    const std::unordered_set<std::string> preferred( request.m_prefer.begin(), request.m_prefer.end() );
    struct Candidate
    {
        int    m_row;              // the table's row
        double m_still = 0.0;      // stillness, then its percentile
        double m_bright = 0.0;     // brightness, then its percentile
        double m_rough = 0.0;      // roughness, then its percentile
        double m_moving = 0.0;     // its own motion (the table's z-score, thousandths)
        double m_dice = 0.0;       // the seed's die
        int    m_class = 1;        // its stay
        bool   m_preferred = false;
        bool   m_used = false;
    };
    std::vector<Candidate> cand;
    for ( int r = 0; r < Rows; ++r )
    {
        if ( !inChoice( r, request.m_choice ) ) continue;
        const std::string name = NightRows[r].m_name;
        if ( excluded.count( name ) ) continue;
        if ( request.m_available && !request.m_available( name ) ) continue;
        Candidate c{ r };
        c.m_still = NightRows[r].m_still;
        c.m_bright = NightRows[r].m_bright;
        c.m_rough = NightRows[r].m_rough;
        c.m_moving = NightRows[r].m_moving;
        c.m_preferred = preferred.count( name ) > 0;
        cand.push_back( c );
    }
    plan.m_candidates = static_cast<int>( cand.size() );
    if ( cand.empty() ) return plan;
    {   // percentiles over the candidates; the stays by the thirds of their own motion; the dice
        std::vector<double> s( cand.size() ), b( cand.size() ), g( cand.size() ), m( cand.size() );
        for ( size_t k = 0; k < cand.size(); ++k )
        {
            s[k] = cand[k].m_still;
            b[k] = cand[k].m_bright;
            g[k] = cand[k].m_rough;
            m[k] = cand[k].m_moving;
        }
        const std::vector<double> ps = percentiles( s ), pb = percentiles( b ), pg = percentiles( g ),
                                  pm = percentiles( m );
        std::uint64_t rng = request.m_seed ? request.m_seed : 0x9E3779B97F4A7C15ull;
        for ( size_t k = 0; k < cand.size(); ++k )
        {
            cand[k].m_still = ps[k];
            cand[k].m_bright = pb[k];
            cand[k].m_rough = pg[k];
            cand[k].m_class = pm[k] < 1.0 / 3.0 ? 0 : ( pm[k] >= 2.0 / 3.0 ? 2 : 1 );
            rng ^= rng << 13;   // xorshift64, as the journey player's
            rng ^= rng >> 7;
            rng ^= rng << 17;
            cand[k].m_dice = Dice * static_cast<double>( rng >> 11 ) * ( 1.0 / 9007199254740992.0 );
        }
    }

    // The walk: at each step the candidate whose cost is lowest.
    auto walk = [&]( std::vector<NightStep> &steps )
    {
        steps.clear();
        for ( Candidate &c : cand ) c.m_used = false;
        double t = 0.0;
        int    prevClass = -1, prevRow = -1;
        while ( total - t > 0.5 * 0.5 * ( DwellLo[1] + DwellHi[1] ) )
        {
            int    best = -1, bestClass = 1;
            double bestCost = 1e30;
            // First with the roughness kept out of the falling asleep and the deep; should nothing be left there (a
            // single pack, late in a long night), without.
            for ( int pass = 0; pass < 2 && best < 0; ++pass )
                for ( size_t k = 0; k < cand.size(); ++k )
                {
                    const Candidate &c = cand[k];
                    if ( c.m_used ) continue;
                    int cls = c.m_class;
                    if ( cls == prevClass && cls != 1 ) cls = 1;   // never two short or two long ones in a row
                    const double stay = 0.5 * ( DwellLo[cls] + DwellHi[cls] );
                    const double x = std::min( 1.0, ( t + 0.5 * stay ) / total );
                    double       ts = 0.0, tb = 0.0;
                    nightCurve( x, hours, ts, tb );
                    if ( pass == 0 && c.m_rough > RoughGate && x < 1.0 - morning && ( x < asleep || ts < DeepGate ) )
                        continue;
                    double cost = std::hypot( c.m_still - ts, BrightWeight * ( c.m_bright - tb ) ) + c.m_dice;
                    if ( prevRow >= 0 ) cost += SoundWeight * nightDistance( prevRow, c.m_row ) / SoundScale;
                    if ( c.m_preferred ) cost -= PreferBonus;
                    if ( cost < bestCost )
                    {
                        bestCost = cost;
                        best = static_cast<int>( k );
                        bestClass = cls;
                    }
                }
            if ( best < 0 ) break;   // every candidate played
            Candidate &c = cand[static_cast<size_t>( best )];
            c.m_used = true;
            NightStep st;
            st.m_row = c.m_row;
            st.m_at = t;
            st.m_stay = 0.5 * ( DwellLo[bestClass] + DwellHi[bestClass] );
            nightCurve( std::min( 1.0, ( t + 0.5 * st.m_stay ) / total ), hours, st.m_targetStill,
                        st.m_targetBright );
            st.m_still = c.m_still;
            st.m_bright = c.m_bright;
            st.m_rough = c.m_rough;
            st.m_distance = prevRow >= 0 ? nightDistance( prevRow, c.m_row ) : 0.0;
            st.m_class = bestClass;
            st.m_preferred = c.m_preferred;
            steps.push_back( st );
            t += st.m_stay;
            prevRow = c.m_row;
            prevClass = bestClass;
        }
        return t;
    };
    // The stays. A night wants still presets, and still presets move little by themselves: by the thirds of every
    // candidate's motion almost none of the night's would stay long. The journeys' rule is the thirds of the presets
    // the journey itself plays (make_journeys.py's timings()), and which those are is known only once the night is
    // chosen -- so the walk lays every step out at a middle stay, then the night's own thirds of motion give each step
    // its class (the third that moves least short, the third that moves most long, never two short or two long ones
    // in a row), and the steps are laid out again at their stays, the curve read anew at the middle of each.
    for ( Candidate &c : cand ) c.m_class = 1;
    walk( plan.m_steps );
    if ( plan.m_steps.size() >= 3 )
    {
        std::vector<double> m;
        for ( const NightStep &st : plan.m_steps ) m.push_back( NightRows[st.m_row].m_moving );
        std::sort( m.begin(), m.end() );
        const double lo = m[m.size() / 3], hi = m[m.size() - m.size() / 3];
        for ( Candidate &c : cand ) c.m_class = c.m_moving < lo ? 0 : ( c.m_moving >= hi ? 2 : 1 );
    }
    const double t = walk( plan.m_steps );
    if ( plan.m_steps.empty() ) return plan;

    // Every dwell scaled alike, so the night lasts what was asked.
    plan.m_scale = total / t;
    Journey &j = plan.m_journey;
    char     name[64];
    std::snprintf( name, sizeof( name ), "Night %g h", std::round( hours * 4.0 ) / 4.0 );
    j.m_name = request.m_name.empty() ? std::string( name ) : request.m_name;
    j.m_cyclic = false;
    for ( NightStep &st : plan.m_steps )
    {
        st.m_at *= plan.m_scale;
        st.m_stay *= plan.m_scale;
        JourneyStep js;
        js.m_preset = NightRows[st.m_row].m_name;
        js.m_dwellLo = std::round( DwellLo[st.m_class] * plan.m_scale );
        js.m_dwellHi = std::round( DwellHi[st.m_class] * plan.m_scale );
        js.m_fadeLo = FadeLo[st.m_class];
        js.m_fadeHi = FadeHi[st.m_class];
        j.m_steps.push_back( js );
    }
    return plan;
}

}   // namespace ambient
