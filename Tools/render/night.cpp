/**
 * @file night.cpp
 * @brief ambient_night: the night director's night as a table (plan 20.11.2026, D3) -- what Tools/night_plot.py prints
 *        and draws; no audio, no library needed (the director's table is compiled in).
 *
 *     ambient_night [--hours H (8) | --until HH:MM [--start HH:MM (23:00)]] [--seed N (1)] [--choice N|NAME]
 *                   [--prefer FILE] [--exclude FILE] [--journey] [--write FILE.journey] [--choices]
 *
 * Prints a line `# night <hours> <seed> <candidates> <scale> <choice>`, a line `# phases <asleep> <morning> <waves>`
 * (shares of the night), a header and one tab-separated line per step: its number, where it begins (seconds), how long
 * it stays (seconds, at the middle of its range), the preset, its pack, its stay class (0 short, 1 middle, 2 long), its
 * stillness, brightness and roughness as percentiles of the candidates, the curve's two targets at the middle of its
 * stay, whether it is preferred, and its distance in sound from the step before (the journeys' distance, in spreads).
 * Then `# journeys <mean neighbour distance> <count>`: the steps of the shipped " - Night" journeys of the library,
 * measured the same way (when the library's Journeys folder is found), and 201 lines `# curve <x> <stillness>
 * <brightness>`: the curve over the night (x 0 .. 1).
 *
 * `--journey` prints the journey file instead, `--write` writes it; `--choices` lists what --choice takes (a number,
 * "every", a family -- sleep, deep, ritual, cold, british, luminous, space -- or a pack's title). `--prefer` and
 * `--exclude` read preset names, one a line (the favourites file and the never-again file both read this way; a
 * favourites file's "first on/off" line names no preset and is passed over).
 * @author Rene Weller
 */
#include "ambient/Journey.h"
#include "ambient/NameList.h"
#include "ambient/NightDirector.h"
#include "ambient/Presets.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace ambient;

namespace
{

/**
 * @brief A clock time as hours.
 * @param[in]  s    "23:00", "7:30" or "7"
 * @param[out] out  receives the hours, 0 .. 24
 * @return false when it is none of those
 */
bool clockHours( const std::string &s, double &out )
{
    int       h = 0, m = 0;
    const int got = std::sscanf( s.c_str(), "%d:%d", &h, &m );
    if ( got < 1 || h < 0 || h > 24 || m < 0 || m > 59 ) return false;
    out = h + m / 60.0;
    return true;
}

/**
 * @brief Lower case, for comparing names.
 * @param[in] s  any text
 * @return it in lower case (ASCII)
 */
std::string lower( std::string s )
{
    for ( char &c : s ) c = static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );
    return s;
}

/**
 * @brief The mean distance in sound between neighbouring steps of the shipped " - Night" journeys.
 * @param[out] count  receives how many steps were measured
 * @return the mean (nightDistance), 0 when none were found
 */
double shippedNightsDistance( int &count )
{
    namespace fs = std::filesystem;
    double sum = 0.0;
    count = 0;
    char roots[4096] = {};
    libraryRoots( roots, static_cast<int>( sizeof( roots ) ) );
    std::string all = roots;
    size_t      from = 0;
    while ( from <= all.size() )
    {
        const size_t      semi = all.find( ';', from );
        const std::string root = all.substr( from, semi == std::string::npos ? std::string::npos : semi - from );
        from = semi == std::string::npos ? all.size() + 1 : semi + 1;
        std::error_code ec;
        const fs::path  dir = fs::path( root ) / "Journeys";
        if ( root.empty() || !fs::is_directory( dir, ec ) ) continue;
        for ( const auto &e : fs::directory_iterator( dir, ec ) )
        {
            const std::string file = e.path().filename().string();
            if ( file.size() < 16 || file.compare( file.size() - 16, 16, " - Night.journey" ) != 0 ) continue;
            Journey j;
            if ( !j.load( e.path().string().c_str() ) ) continue;
            for ( size_t k = 1; k < j.m_steps.size(); ++k )
            {
                const int a = nightRowOf( j.m_steps[k - 1].m_preset ), b = nightRowOf( j.m_steps[k].m_preset );
                if ( a < 0 || b < 0 ) continue;
                sum += nightDistance( a, b );
                ++count;
            }
        }
        if ( count > 0 ) break;   // the first library that has them
    }
    return count ? sum / count : 0.0;
}

}   // namespace

/**
 * @brief Reads the arguments, composes the night and prints it.
 * @param[in] argc  the argument count
 * @param[in] argv  the arguments
 * @return 0, 1 when the night came out empty or could not be written, 2 on a bad argument
 */
int main( int argc, char **argv )
{
    NightRequest r;
    bool         journey = false;
    std::string  write, until;
    double       start = 23.0;
    for ( int i = 1; i < argc; ++i )
    {
        const std::string a = argv[i];
        const bool        has = i + 1 < argc;
        if ( a == "--hours" && has )
            r.m_hours = std::atof( argv[++i] );
        else if ( a == "--until" && has )
            until = argv[++i];
        else if ( a == "--start" && has )
        {
            if ( !clockHours( argv[++i], start ) )
            {
                std::fprintf( stderr, "ambient_night: --start wants a clock time like 23:00\n" );
                return 2;
            }
        }
        else if ( a == "--seed" && has )
            r.m_seed = std::strtoull( argv[++i], nullptr, 10 );
        else if ( a == "--choice" && has )
        {
            const std::string c = lower( argv[++i] );
            int               found = -1;
            for ( int k = 0; k < numNightChoices() && found < 0; ++k )
            {
                const std::string n = lower( nightChoiceName( k ) );
                if ( c == std::to_string( k ) || c == n || "family: " + c == n || "pack: " + c == n ||
                     ( k == 0 && c == "every" ) )
                    found = k;
            }
            if ( found < 0 )
            {
                std::fprintf( stderr, "ambient_night: no choice '%s' (see --choices)\n", argv[i] );
                return 2;
            }
            r.m_choice = found;
        }
        else if ( ( a == "--prefer" || a == "--exclude" ) && has )
        {
            const std::string path = argv[++i];
            if ( !std::filesystem::exists( std::filesystem::path( path ) ) )
            {
                std::fprintf( stderr, "ambient_night: cannot read %s\n", path.c_str() );
                return 2;
            }
            std::vector<std::string> names = readNameList( path );
            names.erase( std::remove_if( names.begin(), names.end(),
                                         []( const std::string &n ) { return n.rfind( "first ", 0 ) == 0; } ),
                         names.end() );
            ( a == "--prefer" ? r.m_prefer : r.m_exclude ) = names;
        }
        else if ( a == "--journey" )
            journey = true;
        else if ( a == "--write" && has )
            write = argv[++i];
        else if ( a == "--choices" )
        {
            for ( int k = 0; k < numNightChoices(); ++k ) std::printf( "%d\t%s\n", k, nightChoiceName( k ).c_str() );
            return 0;
        }
        else
        {
            std::fprintf( stderr, "ambient_night [--hours H | --until HH:MM [--start HH:MM]] [--seed N] "
                                  "[--choice N|NAME] [--prefer FILE] [--exclude FILE] [--journey] [--write FILE] "
                                  "[--choices]\n" );
            return 2;
        }
    }
    if ( !until.empty() )
    {
        double wake = 0.0;
        if ( !clockHours( until, wake ) )
        {
            std::fprintf( stderr, "ambient_night: --until wants a clock time like 7:00\n" );
            return 2;
        }
        r.m_hours = wake - start + ( wake <= start ? 24.0 : 0.0 );
    }
    const NightPlan plan = composeNight( r );
    if ( plan.m_steps.empty() )
    {
        std::fprintf( stderr, "ambient_night: no preset to choose from\n" );
        return 1;
    }
    if ( !write.empty() && !plan.m_journey.save( write.c_str() ) )
    {
        std::fprintf( stderr, "ambient_night: cannot write %s\n", write.c_str() );
        return 1;
    }
    if ( journey )
    {
        std::printf( "%s", plan.m_journey.text().c_str() );
        return 0;
    }
    double asleep = 0.0, morning = 0.0;
    int    waves = 1;
    nightPhases( r.m_hours, asleep, morning, waves );
    std::printf( "# night %g %llu %d %.4f %s\n", r.m_hours, static_cast<unsigned long long>( r.m_seed ),
                 plan.m_candidates, plan.m_scale, nightChoiceName( r.m_choice ).c_str() );
    std::printf( "# phases %.4f %.4f %d\n", asleep, morning, waves );
    std::printf( "step\tat\tstay\tpreset\tpack\tclass\tstill\tbright\trough\ttarget_still\ttarget_bright\tpreferred\t"
                 "distance\n" );
    for ( size_t k = 0; k < plan.m_steps.size(); ++k )
    {
        const NightStep &s = plan.m_steps[k];
        std::printf( "%zu\t%.0f\t%.0f\t%s\t%s\t%d\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%d\t%.4f\n", k + 1, s.m_at, s.m_stay,
                     nightRowName( s.m_row ), nightRowPack( s.m_row ), s.m_class, s.m_still, s.m_bright, s.m_rough,
                     s.m_targetStill, s.m_targetBright, s.m_preferred ? 1 : 0, s.m_distance );
    }
    int          shipped = 0;
    const double mean = shippedNightsDistance( shipped );
    std::printf( "# journeys %.4f %d\n", mean, shipped );
    for ( int k = 0; k <= 200; ++k )   // the curve itself, for the plot
    {
        double still = 0.0, bright = 0.0;
        nightCurve( k / 200.0, r.m_hours, still, bright );
        std::printf( "# curve %.4f %.4f %.4f\n", k / 200.0, still, bright );
    }
    return 0;
}
