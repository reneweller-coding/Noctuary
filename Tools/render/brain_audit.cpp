/**
 * @file brain_audit.cpp
 * @brief The conductor alone, for an hour, measured -- the rule book's own check (its section 11).
 *
 * @code
 *   ambient_brain_audit [--preset "name"] [--presets file] [--set key=value]... [--hours 1] [--dt 0.005] [--seed N]
 *                       [--arrive N]
 *   ambient_brain_audit --ear-calibrate | --ear-bench
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
 *
 * `--arrive N` (or AMBIENT_BRAIN_ARRIVE=N; the option wins) plays the conductors with the arrival repairs of plan F1b
 * switched on: a sum of BrainParams::Arrive* bits, 0 for none; without either the engine's default (none). README.md, "Arrival repairs", lists them
 * and says how they would become the default.
 *
 * Under AMBIENT_BRAIN_EAR the ear's log of plan B1 (ClusterBrain::M_Ear, 20.11.2026) is printed beside the events: per
 * decision a line D,\<seq\>,\<seconds\>,\<conductor\>,\<kind\>,\<from\>,\<chosen\>,\<best\>,\<root\>,\<consonance\>,
 * \<mode\>,\<layers\>,\<smooth\>,\<staying Hz\>,\<leaving Hz\>,\<other Hz\> (the sets ';'-separated; kind 0 Free
 * draw, 1 chooseNote, 2 Smooth's trial, 3 pivot, 4 deja vu), then per candidate C,\<note\>,\<Hz\>,\<struck bits\>,
 * \<weight\>,\<would\>,\<tension staying\>,\<leaving\>,\<other\>,\<roughness\>,\<disharmony\>,\<entropy\>,\<order\>,
 * \<staying partner Hz\>,\<other partner Hz\> (the notes the worst pairs are with, 0 for none), and for stepRubs()
 * S,\<seconds\>,\<conductor\>,\<arriving\>,\<leaving\>,\<rubs\>,\<roughness\>,\<disharmony\>,\<entropy\>,\<tension\>,
 * \<arriving Hz\>,\<leaving Hz\> (a repeat of the conductor's last S line is left out). A line "# ear scale \<name\>" follows
 * each preset line. Tools/library/ear_audit.py reads it. `--ear-calibrate` prints the standard deviations the tension
 * is scaled by and the tension of every interval from the unison to two octaves in five registers; `--ear-bench`
 * times noteTension().
 * @author Rene Weller
 */
#include "ambient/Engine.h"
#include "ambient/MainStack.h"
#include "ambient/Params.h"
#include "ambient/Presets.h"
#include "ambient/ClusterBrain.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace ambient;

/**
 * @brief The default voice's partial template, as the engine builds it before any preset (BrainParams::m_spectrum).
 * @return a copy of Engine::brainSpectrum() after the parameters have been read once
 */
static BrainSpectrum defaultSpectrum()
{
    auto engine = std::make_unique<Engine>();
    engine->prepare( ambient::DefaultSampleRate, 256 );
    engine->auditConductor( 0.01, 0.01, []( double, int, const BrainEvent & ) {}, nullptr );   // reads the parameters
    return engine->brainSpectrum();
}

/**
 * @brief --ear-calibrate: the three parts of pairTension() over the eleven intervals m2 .. M7 at middle C, 12-TET, with
 *        the default template, their standard deviations (the constants pairTension() divides by), and the tension of
 *        every interval up to two octaves at C2, C3, C4, C5 and C6.
 */
static void earCalibrate()
{
    const BrainSpectrum sp = defaultSpectrum();
    std::printf( "template:" );
    for ( int i = 0; i < sp.m_count; ++i ) std::printf( " %.4g@%.4g", sp.m_amp[i], sp.m_ratio[i] );
    std::printf( "\n" );
    const double c4 = 261.6255653005986;
    double       sum[3] = {}, sq[3] = {};
    for ( int k = 1; k <= 11; ++k )
    {
        TensionParts t;
        pairTension( c4, c4 * std::pow( 2.0, k / 12.0 ), sp, &t );
        const double v[3] = { t.m_rough, t.m_disharm, t.m_entropy };
        for ( int i = 0; i < 3; ++i )
        {
            sum[i] += v[i];
            sq[i] += v[i] * v[i];
        }
    }
    const char *names[3] = { "roughness", "disharmony", "entropy" };
    for ( int i = 0; i < 3; ++i )
        std::printf( "sd %s %.4f\n", names[i],
                     std::sqrt( std::max( 0.0, sq[i] / 11.0 - ( sum[i] / 11.0 ) * ( sum[i] / 11.0 ) ) ) );
    std::printf( "semitones,register,hz_low,roughness,disharmony,entropy,tension\n" );
    for ( int oct = 2; oct <= 6; ++oct )
    {
        const double lo = c4 * std::pow( 2.0, oct - 4 );
        for ( int k = 0; k <= 24; ++k )
        {
            TensionParts t;
            pairTension( lo, lo * std::pow( 2.0, k / 12.0 ), sp, &t );
            std::printf( "%d,C%d,%.2f,%.4f,%.4f,%.4f,%.4f\n", k, oct, lo, t.m_rough, t.m_disharm, t.m_entropy, t.m_total );
        }
    }
}

/**
 * @brief --ear-bench: nanoseconds per noteTension() call against sets of one, three, six and twelve notes, with the
 *        default template and with a pure tone, each the median of five rounds.
 */
static void earBench()
{
    const BrainSpectrum sp = defaultSpectrum(), pure;
    const double        c4 = 261.6255653005986;
    double              set[12];
    for ( int i = 0; i < 12; ++i ) set[i] = c4 * std::pow( 2.0, ( 3 * i - 12 ) / 12.0 );   // minor thirds from C3
    volatile double sink = 0.0;
    for ( const BrainSpectrum *s : { &sp, &pure } )
        for ( int n : { 1, 3, 6, 12 } )
        {
            double rounds[5];
            for ( double &r : rounds )
            {
                const int  calls = 2000;
                const auto t0 = std::chrono::steady_clock::now();
                for ( int k = 0; k < calls; ++k )
                    sink = sink + noteTension( c4 * std::pow( 2.0, ( k % 36 - 12 ) / 12.0 ), set, n, *s );
                r = std::chrono::duration<double, std::nano>( std::chrono::steady_clock::now() - t0 ).count() / calls;
            }
            std::sort( rounds, rounds + 5 );
            std::printf( "noteTension %s, %2d notes: %.0f ns per call\n", s == &pure ? "pure tone" : "template", n,
                         rounds[2] );
        }
}

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
    int arrive = std::getenv( "AMBIENT_BRAIN_ARRIVE" ) ? std::atoi( std::getenv( "AMBIENT_BRAIN_ARRIVE" ) )
                                                       : -1;   // -1: the engine's own default
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
        else if ( a == "--arrive" )
            arrive = std::atoi( next().c_str() );
        else if ( a == "--ear-calibrate" )
        {
            earCalibrate();
            return 0;
        }
        else if ( a == "--ear-bench" )
        {
            earBench();
            return 0;
        }
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
    // With AMBIENT_BRAIN_EAR set, the ear's log of plan B1 (see the file header). The conductor's address tells which of
    // the two a report is from.
    static Engine *ear = nullptr;
    static int     earSeq = 0;
    const bool     earOn = std::getenv( "AMBIENT_BRAIN_EAR" ) != nullptr;
    if ( earOn )
    {
        ClusterBrain::M_Ear = []( const ClusterBrain::EarDecision &d, const ClusterBrain::EarCandidate *c )
        {
            if ( c == nullptr )
            {
                const BrainParams &p = *d.m_params;
                std::printf( "D,%d,%.3f,%d,%d,%d,%d,%.6g,%d,%.3f,%d,%.3f,%.3f,", ++earSeq, now,
                             ear ? ear->conductorIndex( d.m_who ) : 0, static_cast<int>( d.m_kind ), d.m_from, d.m_chosen,
                             d.m_best, d.m_root, static_cast<double>( p.m_consonance ), static_cast<int>( p.m_mode ),
                             static_cast<double>( p.m_layers ), static_cast<double>( p.m_smooth ) );
                auto list = []( const double *v, int n, const char *end )
                {
                    for ( int i = 0; i < n; ++i ) std::printf( i ? ";%.3f" : "%.3f", v[i] );
                    std::printf( "%s", end );
                };
                list( d.m_own, d.m_ownCount, "," );
                list( d.m_left, d.m_leftCount, "," );
                list( d.m_other, d.m_otherCount, "\n" );
                return;
            }
            std::printf( "C,%d,%.3f,%u,%.6g,%.6g,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%.3f,%.3f\n", c->m_note, c->m_hz,
                         c->m_struck, c->m_weight, c->m_would, c->m_own, c->m_left, c->m_other, c->m_worst.m_rough,
                         c->m_worst.m_disharm, c->m_worst.m_entropy, c->m_order, c->m_ownAt, c->m_otherAt );
        };
        ClusterBrain::M_EarStep = []( const ClusterBrain *who, int arriving, int leaving, bool rubs, const TensionParts &t )
        {
            static int last[3][3] = {};   // per conductor: arriving + 1, leaving + 1, rubs + 1 of the last line printed
            const int  w = ear ? ear->conductorIndex( who ) : 0;
            if ( last[w][0] == arriving + 1 && last[w][1] == leaving + 1 && last[w][2] == ( rubs ? 2 : 1 ) ) return;
            last[w][0] = arriving + 1;
            last[w][1] = leaving + 1;
            last[w][2] = rubs ? 2 : 1;
            std::printf( "S,%.3f,%d,%d,%d,%d,%.4f,%.4f,%.4f,%.4f,%.3f,%.3f\n", now, w, arriving, leaving, rubs ? 1 : 0,
                         t.m_rough, t.m_disharm, t.m_entropy, t.m_total, ear ? ear->frequencyOf( arriving ) : 0.0,
                         ear ? ear->frequencyOf( leaving ) : 0.0 );
        };
    }
    for ( const int presetIndex : presetIndices )
    {
        auto engine = std::make_unique<Engine>();   // an Engine is large; the heap, not the stack -- and a fresh one each
        engine->prepare( ambient::DefaultSampleRate, 256 );
        if ( arrive >= 0 ) engine->setArriveMask( arrive );
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
        if ( earOn )
        {
            const int sc = static_cast<int>( std::lround( engine->getParam( ParamIdE::SCALE ) ) );
            std::printf( "# ear scale %s\n", sc >= 0 && sc < NumScaleChoices ? ScaleNames[sc] : "?" );
            ear = engine.get();
        }
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
