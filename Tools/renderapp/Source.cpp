/**
 * @file Source.cpp
 * @brief Noctuary Journeys: the render app's source for Noctuary -- its journeys (the templates beside the packs and
 *        the player's own in Documents/Noctuary/Journeys), each rendered as the instrument plays it.
 *
 * As ambient_render plays a journey (Tools/render/main.cpp, its journey path) and the instrument does: the packs loaded
 * (loadDefaultPresetPacks), the first step's preset applied with its media (its clips, its wavetable, its rooms), the
 * engine prepared at 48 kHz with blocks of 256; every later step brought up on a second engine with everything the
 * instrument stands at, the preset's own on top, the conductor's chord and the near layer handed over, and crossfaded
 * in on a sine/cosine pair once it is audible (at most eight seconds' wait), so the sum keeps its power. The near layer
 * as the instrument chooses it: Auto (fore_auto) from the pack's table, or the step's own near preset ("keep" pins what
 * plays). The dwells and fades are drawn from the journey's ranges with the dice, so the length is known before.
 * @note The media and the crossfade after ambient_render's runOnce (16.10.2026), which matches the plugin's
 *       beginTransition; the near layer per step after NoctuaryProcessor::journeyTick.
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
 */
void loadNearClips( Engine &engine, const std::string &path )
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
        if ( !pool.empty() ) engine.setNearTextures( std::move( pool ) );
        return;
    }
    std::vector<std::vector<float>> ch;
    int                             rate = 0;
    if ( !ambient::readWavChannels( path.c_str(), ch, rate ) || ch.empty() ) return;
    engine.setNearTexture( ch[0].data(), ch.size() > 1 ? ch[1].data() : nullptr, static_cast<int>( ch[0].size() ), rate,
                           261.6256, ambient::loopFromName( path.c_str() ) );
}

/**
 * @brief A near preset applied with its clips, as the instrument applies one.
 * @param[in,out] e     the engine
 * @param[in]     pick  the near preset (0: Near Off)
 */
void applyNear( Engine &e, int pick )
{
    e.applyNearPreset( pick );
    const ambient::Preset &np = ambient::nearPreset( pick );
    if ( np.m_texture != nullptr && *np.m_texture != 0 )
    {
        const std::string got = ambient::resolveLibraryFile( np.m_texture );
        if ( !got.empty() ) loadNearClips( e, got );
    }
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
 * @brief The near layer a journey's step asks for (NoctuaryProcessor::journeyTick): "keep" pins what plays (Auto off),
 *        a near preset's name pins that one, nothing or "auto" leaves it to the sound preset's Auto.
 * @param[in,out] e     the engine the step is on
 * @param[in]     want  the step's near field
 */
void stepNear( Engine &e, const std::string &want )
{
    if ( want.empty() || want == "auto" ) return;
    e.setParam( ParamIdE::FORE_AUTO, 0.0f );
    if ( want == "keep" ) return;
    for ( int i = 0; i < ambient::numNearPresets(); ++i )
        if ( want == ambient::nearPreset( i ).m_name )
        {
            applyNear( e, i );
            return;
        }
}

/** @brief A journey being rendered: two engines at most, a step crossfaded in on the second. */
class JourneyRendering : public renderkit::Rendering
{
public:
    /**
     * @brief Sets the engine up on the journey's first step.
     * @param[in]  journey  the journey
     * @param[in]  r        the request (passes, fade)
     * @param[in]  seed     the dice for this journey (renderkit::seedFor)
     * @param[out] why      receives why it cannot be rendered
     */
    JourneyRendering( const ambient::Journey &journey, const renderkit::Request &r, std::uint64_t seed,
                      juce::String &why )
        : m_journey( journey ), m_fadeL( EngineBlock ), m_fadeR( EngineBlock )
    {
        const std::uint64_t s = seed | 1ull;   // as ambient_render's --journey-seed
        m_seconds = lengthOf( journey, s, r.option( "passes" ) + 1 );
        m_fade = r.option( "fade", 1 ) ? 20.0 : 0.0;
        m_frames = static_cast<std::int64_t>( std::llround( m_seconds * Rate ) );
        m_a = std::make_unique<Engine>();   // on the heap: it is large
        m_live = m_a.get();
        m_player.start( journey, s, 0 );
        ambient::JourneyStep st;
        double               fade = 0.0;
        if ( !m_player.advance( 0.0, st, fade ) )
        {
            why = "the journey has no steps";
            m_a.reset();
            return;
        }
        const int idx = presetIndex( st.m_preset );
        if ( idx < 0 )
        {
            why = "its first preset is not in the library: " + juce::String( st.m_preset );
            m_a.reset();
            return;
        }
        // The first step like --preset: applied, its media read, its rooms set once the engine is prepared.
        m_a->applyPreset( idx );
        loadMedia( *m_a, idx, false );
        nearAuto( *m_a, idx );
        stepNear( *m_a, st.m_nearPreset );
        m_a->prepare( Rate, EngineBlock );
        if ( !m_ir.empty() )
            m_a->setImpulse( m_ir[0].data(), m_ir.size() > 1 ? m_ir[1].data() : nullptr,
                             static_cast<int>( m_ir[0].size() ), m_irRate );
        if ( !m_irB.empty() )
            m_a->setImpulseB( m_irB[0].data(), m_irB.size() > 1 ? m_irB[1].data() : nullptr,
                              static_cast<int>( m_irB[0].size() ), m_irBRate );
        m_step = st.m_preset;
    }
    /** @brief Whether it could be set up. @return true when it renders */
    bool ok() const { return m_a != nullptr; }
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

private:
    /**
     * @brief A pack preset's media: its clips (one file for every slot, or up to four separated by ';'), its
     *        wavetable, its rooms -- into the engine at once when it is prepared, else the rooms kept for after.
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
            const std::string all( tex );
            const bool        perSlot = all.find( ';' ) != std::string::npos;
            size_t            start = 0;
            for ( int slot = 0; slot < ambient::Slots; ++slot )
            {
                const size_t semi = all.find( ';', start );
                std::string  one = all.substr( start, semi == std::string::npos ? std::string::npos : semi - start );
                while ( !one.empty() && one.front() == ' ' ) one.erase( one.begin() );
                while ( !one.empty() && one.back() == ' ' ) one.pop_back();
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
                if ( semi == std::string::npos ) break;
                start = semi + 1;
            }
        }
        if ( tab && *tab )
        {
            int cycle = 0;
            if ( ambient::readWavetableFile( tab, mono, cycle ) )
                e.loadUserWavetable( mono.data(), static_cast<int>( mono.size() ), cycle );
        }
        for ( int which = 2; which <= 3; ++which )
        {
            const char *imp = ambient::presetFilePath( idx, which );
            if ( !imp || !*imp ) continue;
            std::vector<std::vector<float>> ch;
            int                             r2 = 0;
            if ( !ambient::readWavChannels( imp, ch, r2 ) || ch.empty() ) continue;
            if ( roomsNow )
            {
                if ( which == 2 )
                    e.setImpulse( ch[0].data(), ch.size() > 1 ? ch[1].data() : nullptr,
                                  static_cast<int>( ch[0].size() ), r2 );
                else
                    e.setImpulseB( ch[0].data(), ch.size() > 1 ? ch[1].data() : nullptr,
                                   static_cast<int>( ch[0].size() ), r2 );
            }
            else if ( which == 2 )
            {
                m_ir = ch;
                m_irRate = r2;
            }
            else
            {
                m_irB = ch;
                m_irBRate = r2;
            }
        }
    }
    /**
     * @brief The next step: a cut when its fade is nothing, else the preset brought up on the other engine with
     *        everything the instrument stands at, the conductor's chord and the near layer handed over.
     * @param[in] idx   the preset
     * @param[in] fade  seconds of the crossfade
     * @param[in] near  the step's near field
     */
    void beginStep( int idx, double fade, const std::string &near )
    {
        Engine &out = *m_live;
        if ( fade < 0.05 )
        {
            out.applyPreset( idx );
            loadMedia( out, idx, true );
            nearAuto( out, idx );
            stepNear( out, near );
            return;
        }
        // only ever two engines: a step whose fade is still running is finished here
        if ( m_fading != nullptr )
        {
            m_fading->allNotesOff();
            m_fading->reset();
            m_fading = nullptr;
        }
        if ( m_b == nullptr ) m_b = std::make_unique<Engine>();
        Engine &in = m_live == m_a.get() ? *m_b : *m_a;
        in.allNotesOff();
        in.reset();
        for ( int i = 0; i < ambient::NumParams; ++i )
            in.setParam( static_cast<ParamIdE>( i ), out.getParam( static_cast<ParamIdE>( i ) ) );
        in.applyPreset( idx );
        nearAuto( in, idx );
        stepNear( in, near );
        in.prepare( Rate, EngineBlock );
        // the first preset's rooms, then the preset's own over them (ambient_render)
        if ( !m_ir.empty() )
            in.setImpulse( m_ir[0].data(), m_ir.size() > 1 ? m_ir[1].data() : nullptr,
                           static_cast<int>( m_ir[0].size() ), m_irRate );
        if ( !m_irB.empty() )
            in.setImpulseB( m_irB[0].data(), m_irB.size() > 1 ? m_irB[1].data() : nullptr,
                            static_cast<int>( m_irB[0].size() ), m_irBRate );
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
        m_live = &in;
        m_fading = &out;
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
        if ( t1 >= 1.0 )
        {
            m_fading->allNotesOff();
            m_fading->reset();
            m_fading = nullptr;
        }
    }

    ambient::Journey                m_journey;             ///< the journey
    ambient::JourneyPlayer          m_player;              ///< its clock
    std::unique_ptr<Engine>         m_a;                   ///< the first engine
    std::unique_ptr<Engine>         m_b;                   ///< the second, built at the first crossfade
    Engine                         *m_live = nullptr;      ///< the one the instrument is
    Engine                         *m_fading = nullptr;    ///< the one on its way out, or none
    double                          m_fadePos = 0.0;       ///< 0 .. 1 across the crossfade
    double                          m_fadeHead = 0.0;      ///< s waited for the arriving preset to be audible
    double                          m_fadeSeconds = 0.0;   ///< the crossfade's length
    std::vector<std::vector<float>> m_ir;                  ///< the first preset's room
    std::vector<std::vector<float>> m_irB;                 ///< ... and its second (Room Morph's)
    int                             m_irRate = 0;          ///< the room's rate
    int                             m_irBRate = 0;         ///< the second room's rate
    std::vector<float>              m_fadeL;               ///< the leaving engine's block, left
    std::vector<float>              m_fadeR;               ///< ... right
    double                          m_seconds = 0.0;       ///< the length
    double                          m_fade = 0.0;          ///< the fade over the end, s
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
