/**
 * @file brain_audit.cpp
 * @brief The conductor alone, for an hour, measured -- the rule book's own check (its section 11).
 *
 * @code
 *   ambient_brain_audit [--preset "name"] [--presets file] [--set key=value]... [--hours 1] [--dt 0.005] [--seed N]
 * @endcode
 *
 * Prints one line per event, E,\<seconds\>,\<conductor 1|2\>,\<on 1|0\>,\<note\>,\<velocity\>,\<pitch
 * class\>,\<Hz\> (the last two as the instrument's tuning sounds the note), and one per root change, R,\<seconds\>,\<root\>. Tools/library/brain_audit.py turns that into the table of
 * section 11, the anti-rules of section 12 and the roles of table 2. No audio is rendered: the
 * engine's conductors are stepped at the control rate with the parameters exactly as a render
 * would read them, which is why this is an engine method and not a copy of its wiring.
 *
 * `--presets file` audits every preset named in the file (one name per line) in turn, each on a fresh engine, each
 * introduced by its `# preset <name>` line (20.11.2026, Tools/library/music_audit.py): loading the library takes
 * longer than an hour of one preset's conductors, so a process per preset spent most of a full-library run loading.
 *
 * Under AMBIENT_BRAIN_OFFWHY each conductor NoteOff is preceded by a W,\<note\>,\<why\>,\<age\> line naming the path
 * that ended the note (ClusterBrain::M_OffWhy), for the study of notes shorter than their attack (20.11.2026).
 * @author Rene Weller
 */
#include "ambient/Engine.h"
#include "ambient/MainStack.h"
#include "ambient/Params.h"
#include "ambient/Presets.h"
#include "ambient/ClusterBrain.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace ambient;

/**
 * @brief Reads the options, sets the engine up as a render would, and runs Engine::auditConductor for the
 *        hours asked, printing the E and R lines (and the T lines under AMBIENT_BRAIN_TRACE) to stdout.
 * @param[in]     argc  argument count, as the runtime hands it over
 * @param[in,out] argv  the options listed in the file header; a bad or unknown one is reported on stderr
 * @return 0 when the audit ran, 2 for a command-line error (unknown option, preset or parameter, a non-positive --hours
 *         or --dt, a --presets file that cannot be read)
 */
int main( int argc, char **argv )
{
    ambient::ensureMainStack( argv, 64ull << 20 );   // Linux: the stack Windows and macOS link in (MainStack.h)
    double                                           hours = 1.0, dt = 0.005;
    long long                                        seed = -1;
    std::vector<std::string>                         presetNames;
    std::vector<std::pair<std::string, std::string>> sets;
    for ( int i = 1; i < argc; ++i )
    {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? std::string( argv[++i] ) : std::string(); };
        if ( a == "--preset" )
            presetNames.push_back( next() );
        else if ( a == "--presets" )
        {
            const std::string file = next();
            std::ifstream     in( file );
            if ( !in )
            {
                std::fprintf( stderr, "cannot read %s\n", file.c_str() );
                return 2;
            }
            for ( std::string line; std::getline( in, line ); )
            {
                while ( !line.empty() && ( line.back() == '\r' || line.back() == '\n' ) ) line.pop_back();
                if ( !line.empty() ) presetNames.push_back( line );
            }
        }
        else if ( a == "--hours" )
            hours = std::atof( next().c_str() );
        else if ( a == "--dt" )
            dt = std::atof( next().c_str() );
        else if ( a == "--seed" )
            seed = std::atoll( next().c_str() );
        else if ( a == "--set" )
        {
            const std::string kv = next();
            const size_t      eq = kv.find( '=' );
            if ( eq == std::string::npos )
            {
                std::fprintf( stderr, "bad --set %s\n", kv.c_str() );
                return 2;
            }
            sets.emplace_back( kv.substr( 0, eq ), kv.substr( eq + 1 ) );
        }
        else
        {
            std::fprintf( stderr, "unknown option %s\n", a.c_str() );
            return 2;
        }
    }
    if ( dt <= 0.0 || hours <= 0.0 )
    {
        std::fprintf( stderr, "hours and dt must be positive\n" );
        return 2;
    }
    loadDefaultPresetPacks();   // $AMBIENT_PACKS, as the render tool
    std::vector<int> presetIndices;
    for ( const auto &name : presetNames )
    {
        int presetIndex = -1;
        for ( int p = 0; p < numPresets(); ++p )
            if ( name == preset( p ).m_name ) presetIndex = p;
        if ( presetIndex < 0 )
        {
            std::fprintf( stderr, "unknown preset '%s'\n", name.c_str() );
            return 2;
        }
        presetIndices.push_back( presetIndex );
    }
    if ( presetIndices.empty() ) presetIndices.push_back( -1 );   // no preset: the defaults
    // With AMBIENT_BRAIN_TRACE set, every stamp of the constellation memory and every verdict it
    // gives is printed as T,\<seconds\>,\<what\>,\<note\>,\<key\>: how a repeat is traced to its cause.
    static double now = 0.0;
    if ( std::getenv( "AMBIENT_BRAIN_TRACE" ) )
        ClusterBrain::M_Trace = []( const char *what, int note, uint32_t key )
        { std::printf( "T,%.4f,%s,%d,%u\n", now, what, note, key ); };
    // With AMBIENT_BRAIN_OFFWHY set, every NoteOff of a conductor is preceded by W,\<note\>,\<why\>,\<age\>: the path
    // that ended it and, in Chords mode, the seconds it had sounded when it left its slot (ClusterBrain::M_OffWhy;
    // 20.11.2026, plan F1). The E line that follows carries the time and the conductor.
    if ( std::getenv( "AMBIENT_BRAIN_OFFWHY" ) )
        ClusterBrain::M_OffWhy = []( int note, const char *why, double age )
        { std::printf( "W,%d,%s,%.3f\n", note, why, age ); };
    for ( const int presetIndex : presetIndices )
    {
        auto engine = std::make_unique<Engine>();   // an Engine is large; the heap, not the stack -- and a fresh one each
        engine->prepare( ambient::DefaultSampleRate, 256 );
        if ( presetIndex >= 0 ) engine->applyPreset( presetIndex );
        for ( const auto &kv : sets )
        {
            const ParamDesc *d = findParam( kv.first.c_str() );
            if ( !d )
            {
                std::fprintf( stderr, "unknown parameter '%s'\n", kv.first.c_str() );
                return 2;
            }
            engine->setParam( d->m_id, paramValueFromText( *d, kv.second.c_str() ) );
        }
        if ( seed >= 0 ) engine->setParam( ParamIdE::SEED, static_cast<float>( seed ) );
        std::printf( "# preset %s\n", presetIndex >= 0 ? preset( presetIndex ).m_name : "(defaults)" );
        now = 0.0;
        // The pitch class is the one the conductor itself hears -- from the note's frequency in the
        // instrument's own tuning, which is what the constellation memory keys -- not the MIDI number's.
        // With a just scale on D the two differ for some keys, and an audit that counted MIDI numbers
        // reported constellations coming back that never did.
        Engine *eng = engine.get();
        engine->auditConductor(
            hours * 3600.0, dt,
            [eng]( double t, int which, const BrainEvent &e )
            {
                now = t;
                const double hz = eng->frequencyOf( e.m_note );
                const int    pc = pitchClassOf( hz );
                std::printf( "E,%.4f,%d,%d,%d,%.3f,%d,%.3f\n", t, which, e.m_type == BrainEvent::TypeE::NOTE_ON ? 1 : 0,
                             e.m_note, static_cast<double>( e.m_velocity ), pc, hz );
            },
            []( double t, int root )
            {
                now = t;
                std::printf( "R,%.4f,%d\n", t, root );
            } );
    }
    return 0;
}
