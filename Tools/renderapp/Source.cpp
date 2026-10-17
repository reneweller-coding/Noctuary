/**
 * @file Source.cpp
 * @brief Noctuary Journeys: the render app's source for Noctuary -- its journeys (the templates beside the packs and
 *        the player's own in Documents/Noctuary/Journeys), each rendered as the instrument plays it.
 *
 * As ambient_render plays a journey (Tools/render/main.cpp, its journey path) and the instrument does: the packs loaded
 * (loadDefaultPresetPacks), the first step's preset applied with its media (its clips, its wavetable, its rooms), the
 * engine prepared at 48 kHz with blocks of 256; every later step brought up on a fresh engine, as the instrument
 * builds one for every transition: everything the instrument stands at, what the engine leaving holds (its clips, its
 * wavetable, the near layer's clips, the room last read), the preset's own on top, the conductor's chord and the near
 * layer handed over, and crossfaded in on a sine/cosine pair once it is audible (at most eight seconds' wait), so the
 * sum keeps its power. The near layer as the instrument chooses it: Auto (fore_auto) from the pack's table, or the
 * step's own near preset ("keep" pins what plays). The arc of the presets that follow the clock (arc_clock) hears the
 * hour the rendering would be played at -- from now, or from 23:00 -- running with the file. The dwells and fades are
 * drawn from the journey's ranges with the dice, so the length is known before.
 * @note The media after the plugin's loadPresetFiles, the crossfade after its beginTransition and carryUserData, the
 *       near layer per step after NoctuaryProcessor::journeyTick (16.10.2026).
 * @author Rene Weller
 */
#include "RenderKit.h"
#include "ambient/ClusterBrain.h"
#include "ambient/Engine.h"
#include "ambient/Journey.h"
#include "ambient/Params.h"
#include "ambient/Presets.h"
#include "ambient/Sources.h"
#include "ambient/WavFile.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>

namespace
{

using ambient::Engine;
using ambient::ParamIdE;

constexpr double Rate = 48000.0;      ///< the files' rate
constexpr int    EngineBlock = 256;   ///< the engine's block (ambient_render's)

/**
 * @brief The player's own journeys and the templates wherever the library lies, as the instrument lists them
 *        (NoctuaryProcessor::journeyFiles): each folder once, sorted by name.
 * @param[out] own  receives how many of them are the player's (they come first)
 * @return the files
 */
juce::Array<juce::File> journeyFiles( int &own )
{
    juce::Array<juce::File> out;
    juce::StringArray       seen;
    auto                    add = [&]( const juce::File &dir )
    {
        if ( !dir.isDirectory() || seen.contains( dir.getFullPathName() ) ) return;
        seen.add( dir.getFullPathName() );
        juce::Array<juce::File> files = dir.findChildFiles( juce::File::findFiles, false, "*.journey" );
        files.sort();
        out.addArray( files );
    };
    add( juce::File::getSpecialLocation( juce::File::userDocumentsDirectory )
             .getChildFile( "Noctuary" )
             .getChildFile( "Journeys" ) );
    own = out.size();
    char roots[4096] = {};
    ambient::libraryRoots( roots, static_cast<int>( sizeof( roots ) ) );
    for ( const juce::String &r :
          juce::StringArray::fromTokens( juce::String( juce::CharPointer_UTF8( roots ) ), ";", "" ) )
        if ( r.isNotEmpty() )
            add( juce::File::isAbsolutePath( r )
                     ? juce::File( r ).getChildFile( "Journeys" )
                     : juce::File::getCurrentWorkingDirectory().getChildFile( r ).getChildFile( "Journeys" ) );
    return out;
}

/**
 * @brief A preset of the library by its name.
 * @param[in] name  the name
 * @return its index, -1 when the library has none of that name
 */
int presetIndex( const std::string &name )
{
    for ( int p = 0; p < ambient::numPresets(); ++p )
        if ( name == ambient::preset( p ).m_name ) return p;
    return -1;
}

/**
 * @brief How long a rendering of a journey lasts: its dwells as the seed draws them, for so many passes (one for a
 *        journey that does not cycle) -- exactly what the rendering then writes.
 * @param[in] journey  the journey
 * @param[in] seed     the dice (JourneyPlayer's)
 * @param[in] passes   passes of a cyclic journey (at least one)
 * @return seconds
 */
double lengthOf( const ambient::Journey &journey, std::uint64_t seed, int passes )
{
    const int steps = static_cast<int>( journey.m_steps.size() ) * ( journey.m_cyclic ? std::max( passes, 1 ) : 1 );
    ambient::JourneyPlayer p;
    p.start( journey, seed );
    ambient::JourneyStep st;
    double               fade = 0.0, total = 0.0;
    if ( !p.advance( 0.0, st, fade ) ) return 0.0;
    for ( int k = 0; k < steps; ++k )
    {   // each step's dwell as the player draws it, the next begun when it is over
        total += p.remaining();
        if ( k + 1 < steps && !p.advance( p.remaining(), st, fade ) ) break;
    }
    return total;
}

/**
 * @brief The near layer's clip from a file, or a pool of them from a folder (sorted, up to 48 spread over it, each
 *        kept to twenty seconds), as ambient_render's loadNearClips and the plugin load them.
 * @param[in,out] engine  the engine
 * @param[in]     path    a WAV/FLAC/AIFF file, or a folder of them
 * @return whether a clip was loaded
 */
bool loadNearClips( Engine &engine, const std::string &path )
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if ( fs::is_directory( path, ec ) )
    {
        std::vector<std::string> files;
        for ( const auto &entry : fs::directory_iterator( path, ec ) )
        {
            if ( !entry.is_regular_file() ) continue;
            std::string ext = entry.path().extension().string();
            for ( char &c : ext ) c = static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );
            if ( ext == ".flac" || ext == ".wav" || ext == ".aif" || ext == ".aiff" )
                files.push_back( entry.path().string() );
        }
        std::sort( files.begin(), files.end() );
        const size_t Max = 48;
        if ( files.size() > Max )
        {
            std::vector<std::string> some;
            for ( size_t i = 0; i < Max; ++i )
                some.push_back(
                    files[static_cast<size_t>( std::lround( i * ( files.size() - 1.0 ) / ( Max - 1.0 ) ) )] );
            files.swap( some );
        }
        std::vector<ambient::Texture> pool;
        for ( const std::string &f : files )
        {
            std::vector<std::vector<float>> ch;
            int                             rate = 0;
            if ( !ambient::readWavChannels( f.c_str(), ch, rate ) || ch.empty() ) continue;
            ambient::Texture t = Engine::makeTexture( ch[0].data(), ch.size() > 1 ? ch[1].data() : nullptr,
                                                      static_cast<int>( ch[0].size() ), rate, 261.6256,
                                                      ambient::loopFromName( f.c_str() ), 20.0 );
            if ( !t.empty() ) pool.push_back( std::move( t ) );
        }
        if ( pool.empty() ) return false;
        engine.setNearTextures( std::move( pool ) );
        return true;
    }
    std::vector<std::vector<float>> ch;
    int                             rate = 0;
    if ( !ambient::readWavChannels( path.c_str(), ch, rate ) || ch.empty() ) return false;
    engine.setNearTexture( ch[0].data(), ch.size() > 1 ? ch[1].data() : nullptr, static_cast<int>( ch[0].size() ), rate,
                           261.6256, ambient::loopFromName( path.c_str() ) );
    return true;
}

/**
 * @brief A near preset applied with its clips, as the instrument applies one (applyNearPreset): a preset that names no
 *        clip, or one not found, takes the clip that was loaded away, so the flute after the radio finds no voice in
 *        its slot.
 * @param[in,out] e     the engine
 * @param[in]     pick  the near preset (0: Near Off)
 */
void applyNear( Engine &e, int pick )
{
    e.applyNearPreset( pick );
    const ambient::Preset &np = ambient::nearPreset( pick );
    const std::string      got =
        np.m_texture != nullptr && *np.m_texture != 0 ? ambient::resolveLibraryFile( np.m_texture ) : std::string();
    if ( got.empty() || !loadNearClips( e, got ) ) e.clearNearTexture();
}

/**
 * @brief The foreground a pack preset brings while Auto is on (fore_auto): the artist's table by the preset's name,
 *        and the near preset's Every scaled by its class's factor (NoctuaryProcessor::applyNearAuto).
 * @param[in,out] e    the engine
 * @param[in]     idx  the sound preset
 */
void nearAuto( Engine &e, int idx )
{
    if ( idx < 0 || e.getParam( ParamIdE::FORE_AUTO ) < 0.5f ) return;
    const int pack = ambient::presetPack( idx );
    if ( pack < 0 ) return;
    float     factor = 1.0f;
    const int pick = ambient::nearAutoPick( ambient::presetPackName( pack ), ambient::preset( idx ).m_name, factor );
    if ( pick < 0 ) return;
    applyNear( e, pick );
    if ( pick > 0 )
        e.setParam( ParamIdE::FORE_RATE, std::clamp( e.getParam( ParamIdE::FORE_RATE ) * factor, 10.0f, 900.0f ) );
}

/**
 * @brief A journey's step's sound preset and near layer, in the instrument's order (NoctuaryProcessor::journeyTick,
 *        setCurrentProgram): "keep" turns Auto off before the preset could bring its own foreground; the preset applied
 *        whole; Auto's foreground (fore_auto); a near preset's name over it, Auto off ("auto" or nothing leaves it to
 *        Auto).
 * @param[in,out] e     the engine the step is on
 * @param[in]     idx   the sound preset
 * @param[in]     want  the step's near field
 */
void applyStep( Engine &e, int idx, const std::string &want )
{
    if ( want == "keep" ) e.setParam( ParamIdE::FORE_AUTO, 0.0f );
    e.applyPreset( idx );
    nearAuto( e, idx );
    if ( want.empty() || want == "auto" || want == "keep" ) return;
    for ( int i = 0; i < ambient::numNearPresets(); ++i )
        if ( want == ambient::nearPreset( i ).m_name )
        {
            e.setParam( ParamIdE::FORE_AUTO, 0.0f );
            applyNear( e, i );
            return;
        }
}

/**
 * @brief The hour a rendering begins at, for the presets whose arc follows the clock (arc_clock): 23:00 when the night
 *        is asked for, else the local time now -- what the instrument would read if it were played from this moment.
 * @param[in] night  as from 23:00
 * @return 0 .. 24
 */
double startHour( bool night )
{
    if ( night ) return 23.0;
    const std::time_t t = std::time( nullptr );
    std::tm           lt{};
#if defined( _WIN32 )
    localtime_s( &lt, &t );
#else
    localtime_r( &t, &lt );
#endif
    return lt.tm_hour + lt.tm_min / 60.0 + lt.tm_sec / 3600.0;
}

/** @brief A journey being rendered: two engines at most, each step crossfaded in on a fresh one. */
class JourneyRendering : public renderkit::Rendering
{
public:
    /**
     * @brief Sets the engine up on the journey's first step.
     * @param[in]  journey  the journey
     * @param[in]  r        the request (passes, fade, night)
     * @param[in]  seed     the dice for this journey (renderkit::seedFor)
     * @param[out] why      receives why it cannot be rendered
     */
    JourneyRendering( const ambient::Journey &journey, const renderkit::Request &r, std::uint64_t seed,
                      juce::String &why )
        : m_journey( journey ), m_fadeL( EngineBlock ), m_fadeR( EngineBlock )
    {
        const std::uint64_t s = seed | 1ull;   // as ambient_render's --journey-seed
        m_seed = s;
        m_passes = r.option( "passes" ) + 1;
        m_seconds = lengthOf( journey, s, m_passes );
        m_fade = r.option( "fade", 1 ) ? 20.0 : 0.0;
        m_hour = startHour( r.option( "night" ) != 0 );
        m_frames = static_cast<std::int64_t>( std::llround( m_seconds * Rate ) );
        m_live = std::make_unique<Engine>();   // on the heap: it is large
        m_player.start( journey, s, 0 );
        ambient::JourneyStep st;
        double               fade = 0.0;
        if ( !m_player.advance( 0.0, st, fade ) )
        {
            why = "the journey has no steps";
            m_live.reset();
            return;
        }
        const int idx = presetIndex( st.m_preset );
        if ( idx < 0 )
        {
            why = "its first preset is not in the library: " + juce::String( st.m_preset );
            m_live.reset();
            return;
        }
        // The first step like --preset: applied, its media read, its rooms set once the engine is prepared.
        applyStep( *m_live, idx, st.m_nearPreset );
        loadMedia( *m_live, idx, false );
        m_live->prepare( Rate, EngineBlock );
        setRooms( *m_live );
        m_step = st.m_preset;
    }
    /** @brief Whether it could be set up. @return true when it renders */
    bool ok() const { return m_live != nullptr; }
    double seconds() const override { return m_seconds; }
    double fadeSeconds() const override { return m_fade; }
    void render( float *left, float *right, int frames ) override
    {
        for ( int at = 0; at < frames; )
        {
            const int            n = std::min( EngineBlock, frames - at );
            ambient::JourneyStep st;
            double               fade = 0.0;
            if ( m_player.advance( n / Rate, st, fade ) )
            {   // the next step, when its time has come, crossfaded in as in the instrument
                const int idx = presetIndex( st.m_preset );
                if ( idx >= 0 )
                {
                    // never longer than what is left to render: the file ends on the step, not on the crossfade
                    const double left_ = static_cast<double>( m_frames - m_done ) / Rate;
                    beginStep( idx, std::min( fade, std::max( 0.0, left_ - 1.0 ) ), st.m_nearPreset );
                    const std::lock_guard<std::mutex> hold( m_lock );
                    m_step = st.m_preset;
                }
            }
            // The arc's clock runs with the file: the engine would read the wall clock, which an offline rendering
            // races past (and which is no hour of the piece).
            const double hour = std::fmod( m_hour + static_cast<double>( m_done ) / Rate / 3600.0, 24.0 );
            m_live->setClockHourOverride( hour );
            if ( m_fading != nullptr ) m_fading->setClockHourOverride( hour );
            float *L = left + at, *R = right + at;
            m_live->process( L, R, n );
            if ( m_fading != nullptr ) crossfade( L, R, n );
            at += n;
            m_done += n;
        }
    }
    juce::String now() const override
    {
        const std::lock_guard<std::mutex> hold( m_lock );
        return "step " + juce::String( m_player.step() + 1 ) + " of " +
               juce::String( static_cast<int>( m_journey.m_steps.size() ) ) + ": " + juce::String( m_step );
    }
    /**
     * @brief The journey's time line: every step a new section for Kaleidoscope -- the first an intro, the fade over the
     *        end an outro -- and a chapter with its preset's name, the pass named when a cyclic journey goes round more
     *        than once; drawn as the rendering draws them (the same dice).
     * @param[out] sheet  receives it
     */
    void timeline( renderkit::CueSheet &sheet ) const override
    {
        const int              per = static_cast<int>( m_journey.m_steps.size() );
        const int              passes = m_journey.m_cyclic ? std::max( m_passes, 1 ) : 1;
        ambient::JourneyPlayer p;
        p.start( m_journey, m_seed, 0 );
        ambient::JourneyStep st;
        double               fade = 0.0, at = 0.0;
        if ( per == 0 || !p.advance( 0.0, st, fade ) ) return;
        for ( int k = 0; k < per * passes; ++k )
        {
            const juce::String pass = passes > 1 ? "Pass " + juce::String( k / per + 1 ) + ", " : juce::String();
            sheet.section( at, k == 0 ? "Intro" : "Groove", k == 0 ? 0.25 : 0.4 );
            sheet.chapter( at, pass + "Step " + juce::String( k % per + 1 ) + ": " + juce::String( st.m_preset ) );
            at += p.remaining();
            if ( k + 1 < per * passes && !p.advance( p.remaining(), st, fade ) ) break;
        }
        if ( m_fade > 0.0 ) sheet.section( std::max( 0.0, m_seconds - m_fade ), "Outro", 0.2 );
    }

private:
    /**
     * @brief A pack preset's media as the instrument reads them (loadPresetFiles): its clips (one file for every slot,
     *        or up to four separated by ';', a slot the per-slot form leaves empty cleared), its wavetable, its rooms (a
     *        room named without a second takes the second away) -- kept as the room the next engine takes over, and
     *        into the engine at once when it is prepared.
     * @param[in,out] e         the engine
     * @param[in]     idx       the preset
     * @param[in]     roomsNow  the engine is prepared: the rooms go in now
     */
    void loadMedia( Engine &e, int idx, bool roomsNow )
    {
        const char        *tex = ambient::presetFilePath( idx, 0 );
        const char        *tab = ambient::presetFilePath( idx, 1 );
        std::vector<float> mono;
        int                rate = 0;
        if ( tex && *tex )
        {
            std::vector<std::string> parts;   // one file for every slot, or one for each
            const std::string        all( tex );
            for ( size_t start = 0;; )
            {
                const size_t semi = all.find( ';', start );
                std::string  one = all.substr( start, semi == std::string::npos ? std::string::npos : semi - start );
                while ( !one.empty() && one.front() == ' ' ) one.erase( one.begin() );
                while ( !one.empty() && one.back() == ' ' ) one.pop_back();
                parts.push_back( one );
                if ( semi == std::string::npos ) break;
                start = semi + 1;
            }
            const bool perSlot = parts.size() > 1;
            for ( int slot = 0; slot < ( perSlot ? ambient::Slots : 1 ); ++slot )
            {
                const std::string one =
                    static_cast<size_t>( slot ) < parts.size() ? parts[static_cast<size_t>( slot )] : std::string();
                std::vector<float> right;
                if ( !one.empty() && ambient::readWavStereo( one.c_str(), mono, right, rate ) )
                {
                    double base = ambient::baseHzFromName( one.c_str() );
                    if ( base <= 0.0 ) base = 261.6256;
                    const bool   seamless = ambient::loopFromName( one.c_str() );
                    const float *rp = right.empty() ? nullptr : right.data();
                    if ( perSlot )
                        e.setTexture( slot, mono.data(), rp, static_cast<int>( mono.size() ), rate, base, seamless );
                    else
                        e.setTexture( mono.data(), rp, static_cast<int>( mono.size() ), rate, base, seamless );
                }
                else if ( perSlot )
                    e.clearTexture( slot );   // the per-slot form names every slot: one it leaves empty has nothing
            }
        }
        if ( tab && *tab )
        {
            int cycle = 0;
            if ( ambient::readWavetableFile( tab, mono, cycle ) )
                e.loadUserWavetable( mono.data(), static_cast<int>( mono.size() ), cycle );
        }
        const char                     *imp = ambient::presetFilePath( idx, 2 );
        const char                     *impB = ambient::presetFilePath( idx, 3 );
        std::vector<std::vector<float>> ch, chB;
        int                             r2 = 0;
        if ( imp && *imp && ambient::readWavChannels( imp, ch, r2 ) && !ch.empty() )
        {
            m_ir = std::move( ch );
            m_irRate = r2;
            if ( roomsNow ) setRoom( e, false );
        }
        if ( impB && *impB && ambient::readWavChannels( impB, chB, r2 ) && !chB.empty() )
        {
            m_irB = std::move( chB );
            m_irBRate = r2;
            if ( roomsNow ) setRoom( e, true );
        }
        else if ( ( impB && *impB ) || ( imp && *imp ) )
        {   // a room named without its second (or a second not found): Room Morph has nothing left over to go to
            m_irB.clear();
            if ( roomsNow ) e.clearImpulseB();
        }
    }
    /**
     * @brief The room last read from a preset, or its second (Room Morph's), into an engine that is prepared.
     * @param[in,out] e       the engine
     * @param[in]     second  the second room
     */
    void setRoom( Engine &e, bool second ) const
    {
        const std::vector<std::vector<float>> &ch = second ? m_irB : m_ir;
        if ( ch.empty() ) return;
        if ( second )
            e.setImpulseB( ch[0].data(), ch.size() > 1 ? ch[1].data() : nullptr, static_cast<int>( ch[0].size() ),
                           m_irBRate );
        else
            e.setImpulse( ch[0].data(), ch.size() > 1 ? ch[1].data() : nullptr, static_cast<int>( ch[0].size() ),
                          m_irRate );
    }
    /**
     * @brief Both rooms last read from presets into an engine that is prepared.
     * @param[in,out] e  the engine
     */
    void setRooms( Engine &e ) const
    {
        setRoom( e, false );
        setRoom( e, true );
    }
    /**
     * @brief The next step: a cut when its fade is nothing, else the preset brought up on a fresh engine, as the
     *        instrument builds one for every transition (ensureEngine: the idle one goes back between steps), with
     *        everything the instrument stands at and what the engine leaving holds (carryUserData), the preset's own on
     *        top, the conductor's chord and the near layer handed over.
     * @param[in] idx   the preset
     * @param[in] fade  seconds of the crossfade
     * @param[in] near  the step's near field
     */
    void beginStep( int idx, double fade, const std::string &near )
    {
        if ( fade < 0.05 )
        {   // a cut: the preset on the engine that plays
            applyStep( *m_live, idx, near );
            loadMedia( *m_live, idx, true );
            return;
        }
        m_fading.reset();   // only ever two engines: a step whose fade is still running is finished here
        auto    held = std::make_unique<Engine>();
        Engine &out = *m_live, &in = *held;
        for ( int i = 0; i < ambient::NumParams; ++i )
            in.setParam( static_cast<ParamIdE>( i ), out.getParam( static_cast<ParamIdE>( i ) ) );
        in.prepare( Rate, EngineBlock );
        // What the instrument carries into a new engine (carryUserData): the clips, the wavetable, the near layer's
        // clips, the room -- the engine keeps rooms as spectra, so the last one read comes from the samples kept here.
        // The preset's own come over them.
        for ( int k = 0; k < ambient::Slots; ++k )
            if ( const ambient::Texture *t = out.displayTexture( k ) )
                if ( !t->empty() ) in.setTexture( k, *t );
        if ( const ambient::Wavetable *wt = out.userWavetable() ) in.setUserWavetable( *wt );
        if ( const ambient::CycleTable *ct = out.userCycles() ) in.setUserCycles( *ct );
        if ( const std::vector<ambient::Texture> *pool = out.displayNearPool() ) in.setNearTextures( *pool );
        setRooms( in );
        applyStep( in, idx, near );
        loadMedia( in, idx, true );
        {   // the conductor's chord taken over (a crossfade changes the instrument, not the music)
            int   cn[ambient::ClusterBrain::Slots];
            float cv[ambient::ClusterBrain::Slots];
            for ( int which = 0; which < 2; ++which )
            {
                const bool second = which == 1;
                const int  n = out.soundingCluster( cn, cv, second );
                if ( n > 0 )
                    in.adoptCluster( cn, cv, n, second );
                else if ( !second )
                    in.requestBrainFill();
            }
            in.adoptNear( out.nearState() );
        }
        m_fading = std::move( m_live );
        m_live = std::move( held );
        m_fadePos = 0.0;
        m_fadeHead = 0.0;
        m_fadeSeconds = fade;
    }
    /**
     * @brief The step coming in over the one going out: the ramp waits for the arriving preset to be audible (up to
     *        eight seconds), then a sine/cosine pair so the sum keeps its power.
     * @param[in,out] L  the arriving engine's left block; receives the mix
     * @param[in,out] R  the arriving engine's right block; receives the mix
     * @param[in]     n  samples
     */
    void crossfade( float *L, float *R, int n )
    {
        const double t0 = m_fadePos;
        bool         ramping = t0 > 0.0;
        if ( !ramping )
        {
            double e = 0.0;
            for ( int i = 0; i < n; ++i ) e += static_cast<double>( L[i] ) * L[i] + static_cast<double>( R[i] ) * R[i];
            m_fadeHead += n / Rate;
            ramping = std::sqrt( e / ( 2.0 * n ) ) > 1.0e-3 || m_fadeHead >= 8.0;
        }
        const double t1 = ramping ? std::min( 1.0, t0 + n / ( Rate * std::max( m_fadeSeconds, 0.25 ) ) ) : 0.0;
        m_fading->process( m_fadeL.data(), m_fadeR.data(), n );
        for ( int i = 0; i < n; ++i )
        {
            const double t = t0 + ( t1 - t0 ) * ( i + 1.0 ) / n;
            const float  gIn = static_cast<float>( std::sin( 1.5707963267948966 * t ) );
            const float  gOut = static_cast<float>( std::cos( 1.5707963267948966 * t ) );
            L[i] = L[i] * gIn + m_fadeL[static_cast<size_t>( i )] * gOut;
            R[i] = R[i] * gIn + m_fadeR[static_cast<size_t>( i )] * gOut;
        }
        m_fadePos = t1;
        if ( t1 >= 1.0 ) m_fading.reset();
    }

    ambient::Journey                m_journey;             ///< the journey
    ambient::JourneyPlayer          m_player;              ///< its clock
    std::uint64_t                   m_seed = 1;            ///< its dice
    int                             m_passes = 1;          ///< passes of a cyclic journey
    std::unique_ptr<Engine>         m_live;                ///< the engine the instrument is
    std::unique_ptr<Engine>         m_fading;              ///< the one on its way out, or none
    double                          m_fadePos = 0.0;       ///< 0 .. 1 across the crossfade
    double                          m_fadeHead = 0.0;      ///< s waited for the arriving preset to be audible
    double                          m_fadeSeconds = 0.0;   ///< the crossfade's length
    std::vector<std::vector<float>> m_ir;                  ///< the room last read from a preset
    std::vector<std::vector<float>> m_irB;                 ///< ... and its second (Room Morph's), or none
    int                             m_irRate = 0;          ///< the room's rate
    int                             m_irBRate = 0;         ///< the second room's rate
    std::vector<float>              m_fadeL;               ///< the leaving engine's block, left
    std::vector<float>              m_fadeR;               ///< ... right
    double                          m_seconds = 0.0;       ///< the length
    double                          m_fade = 0.0;          ///< the fade over the end, s
    double                          m_hour = 0.0;          ///< the arc clock's hour at the file's start
    std::int64_t                    m_frames = 0;          ///< the length in samples
    std::int64_t                    m_done = 0;            ///< samples rendered
    mutable std::mutex              m_lock;                ///< guards m_step
    std::string                     m_step;                ///< the step's preset
};

/** @brief Noctuary's journeys for the render app. */
class NoctuarySource : public renderkit::Source
{
public:
    /** @brief Loads the preset packs (the library) once, as the instrument and ambient_render do. */
    NoctuarySource() { ambient::loadDefaultPresetPacks(); }
    juce::String product() const override { return "Noctuary"; }
    juce::String title() const override { return "Noctuary Journeys"; }
    juce::String subtitle() const override { return "Render journeys into files, faster than real time"; }
    juce::String noun() const override { return "journey"; }
    renderkit::Palette palette() const override
    {
        renderkit::Palette c;
        c.m_back = juce::Colour( 0xff080b12 );
        c.m_panel = juce::Colour( 0xff141a29 );
        c.m_row = juce::Colour( 0xff10151f );
        c.m_raised = juce::Colour( 0xff1a2133 );
        c.m_edge = juce::Colour( 0xff1f2739 );
        c.m_chosen = juce::Colour( 0xff21404a );
        c.m_text = juce::Colour( 0xffcfd7e6 );
        c.m_dim = juce::Colour( 0xff6d7a92 );
        c.m_accent = juce::Colour( 0xff6fd8d2 );
        return c;
    }
    std::vector<renderkit::Column> columns() const override
    {
        return { { "Steps", 56, true }, { "Shortest .. longest", 150, true }, { "Kind", 90, false } };
    }
    std::vector<renderkit::Item> list() override
    {
        std::vector<renderkit::Item> out;
        juce::StringArray names;   // a template in two library folders (the installed one, the source tree's) once
        int               own = 0;
        const juce::Array<juce::File> files = journeyFiles( own );
        for ( int k = 0; k < files.size(); ++k )
        {
            ambient::Journey j;
            if ( !j.load( files[k].getFullPathName().toRawUTF8() ) || j.m_steps.empty() ) continue;
            renderkit::Item item;
            item.m_name = j.m_name.empty() ? files[k].getFileNameWithoutExtension() : juce::String( j.m_name );
            if ( names.contains( item.m_name ) ) continue;
            names.add( item.m_name );
            item.m_kind = k < own ? "Yours" : "Template";
            double shortest = 0.0, longest = 0.0;
            for ( const ambient::JourneyStep &st : j.m_steps )
            {
                shortest += st.m_dwellLo;
                longest += st.m_dwellHi;
                item.m_detail << st.m_preset << " ";
            }
            item.m_cells.add( juce::String( static_cast<int>( j.m_steps.size() ) ) );
            item.m_cells.add( renderkit::clockText( shortest ) + " .. " + renderkit::clockText( longest ) );
            item.m_cells.add( item.m_kind );
            item.m_keys = { static_cast<double>( j.m_steps.size() ), shortest, std::nan( "" ) };
            const std::lock_guard<std::mutex> hold( m_lock );
            item.m_index = static_cast<int>( m_store.size() );
            m_store.push_back( std::make_shared<const ambient::Journey>( std::move( j ) ) );
            out.push_back( std::move( item ) );
        }
        return out;
    }
    std::vector<renderkit::Option> options() const override
    {
        renderkit::Option passes{ "passes",
                                  "Passes",
                                  "How many times a cyclic journey goes round (one that does not cycle "
                                  "plays once).",
                                  {},
                                  0,
                                  true };
        for ( int p = 1; p <= 8; ++p ) passes.m_choices.add( juce::String( p ) );
        return { passes,
                 { "fade",
                   "Fade out over the last 20 seconds",
                   "The end faded out, so the file does not stop dead.",
                   {},
                   1,
                   false },
                 { "night",
                   "As at night (the clock arc from 23:00)",
                   "The presets whose arc follows the clock play as from 23:00 on, the clock running with the file; "
                   "otherwise as from the moment the rendering starts.",
                   {},
                   0,
                   false } };
    }
    bool applies( const renderkit::Item &item, const juce::String &key ) const override
    {
        return key != "passes" || journey( item )->m_cyclic;
    }
    double length( const renderkit::Item &item, const renderkit::Request &r ) override
    {
        return lengthOf( *journey( item ), renderkit::seedFor( item, r ) | 1ull, r.option( "passes" ) + 1 );
    }
    juce::String describe( const renderkit::Item &item, const renderkit::Request &r ) override
    {
        const std::shared_ptr<const ambient::Journey> j = journey( item );
        double                                        shortest = 0.0, longest = 0.0;
        int                                           missing = 0;
        for ( const ambient::JourneyStep &st : j->m_steps )
        {
            shortest += st.m_dwellLo;
            longest += st.m_dwellHi;
            missing += presetIndex( st.m_preset ) < 0 ? 1 : 0;
        }
        juce::String s;
        s << juce::String( static_cast<int>( j->m_steps.size() ) ) << " steps"
          << ( j->m_cyclic ? ", cyclic" : ", once through" ) << "\n"
          << "A pass: " << renderkit::clockText( shortest ) << " .. " << renderkit::clockText( longest ) << " (about "
          << renderkit::clockText( j->meanLength() ) << ")";
        const int passes = r.option( "passes" ) + 1;
        if ( j->m_cyclic && passes > 1 ) s << "\n" << juce::String( passes ) << " passes";
        if ( missing > 0 )
            s << "\n"
              << juce::String( missing ) << ( missing == 1 ? " preset is" : " presets are" )
              << " not in the library (the one before plays on)";
        return s;
    }
    std::unique_ptr<renderkit::Rendering> open( const renderkit::Item &item, const renderkit::Request &r,
                                                juce::String &why ) override
    {
        auto rendering = std::make_unique<JourneyRendering>( *journey( item ), r, renderkit::seedFor( item, r ), why );
        if ( !rendering->ok() ) return nullptr;
        return rendering;
    }

private:
    /**
     * @brief A row's journey.
     * @param[in] item  the row
     * @return it (shared: a rendering may outlive a Refresh)
     */
    std::shared_ptr<const ambient::Journey> journey( const renderkit::Item &item ) const
    {
        const std::lock_guard<std::mutex> hold( m_lock );
        return m_store[static_cast<size_t>( item.m_index )];
    }
    mutable std::mutex                                   m_lock;    ///< guards m_store
    std::vector<std::shared_ptr<const ambient::Journey>> m_store;   ///< every journey listed (only ever added to)
};

}   // namespace

std::unique_ptr<renderkit::Source> renderkit::makeSource()
{
    return std::make_unique<NoctuarySource>();
}
