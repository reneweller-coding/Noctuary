/**
 * @file JourneyTimeline.cpp
 * @brief The journeys over time (JourneyTimeline.h).
 * @author Rene Weller
 */
#include "JourneyTimeline.h"

#include "NoctuaryLookAndFeel.h"

#include <algorithm>
#include <cmath>

namespace
{

/**
 * @brief A step's average length, its crossfade and its dwell.
 * @param[in] s  the step
 * @return seconds
 */
double stepSeconds( const ambient::JourneyStep &s )
{
    return 0.5 * ( s.m_fadeLo + s.m_fadeHi ) + 0.5 * ( s.m_dwellLo + s.m_dwellHi );
}

/**
 * @brief A journey's average length.
 * @param[in] j  the journey
 * @return seconds
 */
double journeySeconds( const ambient::Journey &j )
{
    double t = 0.0;
    for ( const ambient::JourneyStep &s : j.m_steps ) t += stepSeconds( s );
    return t;
}

/**
 * @brief Whether two journeys are the same row of the same presets.
 * @param[in] a  one
 * @param[in] b  the other
 * @return true then
 */
bool sameJourney( const ambient::Journey &a, const ambient::Journey &b )
{
    if ( a.m_name != b.m_name || a.m_steps.size() != b.m_steps.size() ) return false;
    for ( size_t i = 0; i < a.m_steps.size(); ++i )
        if ( a.m_steps[i].m_preset != b.m_steps[i].m_preset ) return false;
    return true;
}

}   // namespace

JourneyTimeline::JourneyTimeline( NoctuaryProcessor &proc, std::function<bool( ambient::Journey & )> chosen,
                                  std::function<void( int )> startAt )
    : m_proc( proc ), m_chosen( std::move( chosen ) ), m_startAt( std::move( startAt ) )
{
    m_copy.setTooltip(
        "Takes the journey above into the one being written, to change here and save from the Morph section's journey row" );
    m_copy.onClick = [this]
    {
        ambient::Journey j;
        if ( !upper( j ) ) return;
        j.m_name = j.m_name.empty() ? std::string( "My Journey" ) : j.m_name + " (mine)";
        m_proc.setJourney( j );
        repaint();
    };
    addAndMakeVisible( m_copy );
    startTimerHz( 2 );
}

void JourneyTimeline::timerCallback()
{
    if ( isShowing() ) repaint();
}

bool JourneyTimeline::upper( ambient::Journey &out ) const
{
    if ( m_proc.journeyPlayer().running() )
    {
        out = m_proc.journeyPlayer().journey();
        return true;
    }
    return m_chosen && m_chosen( out );
}

bool JourneyTimeline::hasDraft() const
{
    const ambient::Journey &d = m_proc.journey();
    if ( d.m_steps.empty() ) return false;
    ambient::Journey u;
    return !upper( u ) || !sameJourney( u, d );
}

juce::Rectangle<float> JourneyTimeline::band( int which ) const
{
    auto r = getLocalBounds().toFloat().reduced( 18.0f, 0.0f ).withTrimmedTop( 28.0f ).withTrimmedBottom( 6.0f );
    if ( !hasDraft() ) return which == 0 ? r.withTrimmedBottom( 20.0f ) : juce::Rectangle<float>();
    const float h =
        ( r.getHeight() - 20.0f - 22.0f ) / 2.0f;   // two bands, the hours under the first, a title over the second
    return which == 0 ? r.withHeight( h ) : r.withTrimmedTop( h + 20.0f + 22.0f );
}

std::vector<JourneyTimeline::Block> JourneyTimeline::blocks( const ambient::Journey &j, juce::Rectangle<float> r )
{
    std::vector<Block> out;
    const double       total = journeySeconds( j );
    if ( total <= 0.0 ) return out;
    double t = 0.0;
    for ( const ambient::JourneyStep &s : j.m_steps )
    {
        Block b;
        b.m_x0 = r.getX() + r.getWidth() * static_cast<float>( t / total );
        t += 0.5 * ( s.m_fadeLo + s.m_fadeHi );
        b.m_xg = r.getX() + r.getWidth() * static_cast<float>( t / total );
        t += 0.5 * ( s.m_dwellLo + s.m_dwellHi );
        b.m_x1 = r.getX() + r.getWidth() * static_cast<float>( t / total );
        out.push_back( b );
    }
    return out;
}

void JourneyTimeline::drawBand( juce::Graphics &g, const ambient::Journey &j, juce::Rectangle<float> r, int lit,
                                double done, bool faint ) const
{
    const juce::Colour hue = ui::MorphCol;
    const auto         bs = blocks( j, r );
    g.setFont( juce::FontOptions( 11.0f ) );
    for ( size_t i = 0; i < bs.size(); ++i )
    {
        const Block       &b = bs[i];
        const bool         now = static_cast<int>( i ) == lit;
        const juce::Colour c = now ? hue : ( faint ? ui::CardColour : ui::CardColour.brighter( 0.25f ) );
        juce::Path         ramp;   // the crossfade: rising into the step
        ramp.startNewSubPath( b.m_x0, r.getBottom() );
        ramp.lineTo( b.m_xg, r.getY() );
        ramp.lineTo( b.m_xg, r.getBottom() );
        ramp.closeSubPath();
        g.setColour( c.withAlpha( now ? 0.35f : 0.6f ) );
        g.fillPath( ramp );
        g.setColour( c.withAlpha( now ? 0.35f : 0.85f ) );
        g.fillRect( juce::Rectangle<float>::leftTopRightBottom( b.m_xg, r.getY(), b.m_x1 - 1.0f, r.getBottom() ) );
        if ( now )
        {
            g.setColour( hue.withAlpha( 0.85f ) );
            g.fillRect( juce::Rectangle<float>::leftTopRightBottom(
                b.m_xg, r.getY(), b.m_xg + ( b.m_x1 - 1.0f - b.m_xg ) * static_cast<float>( done ), r.getBottom() ) );
        }
        if ( static_cast<int>( i ) == m_dropAt && r == band( 1 ) )
        {   // where a moved block would land
            g.setColour( ui::Accent );
            g.fillRect( b.m_x0 - 1.5f, r.getY() - 2.0f, 3.0f, r.getHeight() + 4.0f );
        }
        g.setColour( now ? ui::Bg0 : ui::Text.withAlpha( faint ? 0.55f : 0.85f ) );
        g.drawFittedText( j.m_steps[i].m_preset,
                          juce::Rectangle<float>( b.m_xg + 4.0f, r.getY() + 2.0f,
                                                  std::max( 0.0f, b.m_x1 - b.m_xg - 8.0f ), r.getHeight() - 4.0f )
                              .toNearestInt(),
                          juce::Justification::centredLeft, 2, 0.7f );
    }
}

void JourneyTimeline::paint( juce::Graphics &g )
{
    const auto b = getLocalBounds().toFloat();
    g.setColour( ui::GroupColour.withAlpha( 0.96f ) );
    g.fillRoundedRectangle( b, 6.0f );
    g.setColour( ui::CardEdge );
    g.drawRoundedRectangle( b.reduced( 0.5f ), 6.0f, 1.0f );
    const ambient::JourneyPlayer &player = m_proc.journeyPlayer();
    const bool                    running = player.running();
    ambient::Journey              j;
    const bool                    any = upper( j );
    const double                  total = journeySeconds( j );
    g.setFont( juce::FontOptions( 13.0f, juce::Font::bold ) );
    g.setColour( running ? ui::MorphCol : ui::Dim );
    if ( !any )
    {
        g.drawText(
            "Choose a journey in the Morph section's journey row: its steps are drawn here over time, the one under way lit while it runs.",
            b.reduced( 18.0f, 4.0f ).withHeight( 20.0f ), juce::Justification::centredLeft );
    }
    else
    {
        juce::String head = juce::String( j.m_name ) + "   " + juce::String( static_cast<int>( j.m_steps.size() ) ) +
                            " steps, about " + juce::String( ambient::Journey::timeText( total ) ) +
                            ( j.m_cyclic ? ", then again" : "" );
        if ( running )
            head << "   -- step " << ( player.step() + 1 ) << ", "
                 << juce::String( ambient::Journey::timeText( player.remaining() ) )
                 << " left in it (click a step: there)";
        else
            head << "   (not under way: a click on a step starts it there)";
        g.drawText( head, b.reduced( 18.0f, 4.0f ).withHeight( 20.0f ).withTrimmedRight( 120.0f ),
                    juce::Justification::centredLeft );
        const auto r0 = band( 0 );
        double     done = 0.0;
        if ( running && player.step() >= 0 && player.step() < static_cast<int>( j.m_steps.size() ) )
        {
            const ambient::JourneyStep &s = j.m_steps[static_cast<size_t>( player.step() )];
            done = juce::jlimit( 0.0, 1.0,
                                 1.0 - player.remaining() / std::max( 0.5 * ( s.m_dwellLo + s.m_dwellHi ), 1.0 ) );
        }
        drawBand( g, j, r0, running ? player.step() : -1, done, !running );
        g.setColour( ui::Dim );   // the hours under it
        g.setFont( juce::FontOptions( 11.0f ) );
        const double every = total > 4.0 * 3600.0 ? 3600.0 : ( total > 3600.0 ? 1800.0 : 600.0 );
        for ( double h = 0.0; total > 0.0 && h <= total; h += every )
        {
            const float x = r0.getX() + r0.getWidth() * static_cast<float>( h / total );
            g.drawVerticalLine( juce::roundToInt( x ), r0.getBottom(), r0.getBottom() + 4.0f );
            g.drawText( juce::String( ambient::Journey::timeText( h ) ),
                        juce::Rectangle<float>( x - 30.0f, r0.getBottom() + 4.0f, 60.0f, 14.0f ),
                        juce::Justification::centred );
        }
    }
    // The one being written, to change.
    if ( hasDraft() )
    {
        const ambient::Journey &d = m_proc.journey();
        const auto              r1 = band( 1 );
        g.setColour( ui::Accent );
        g.setFont( juce::FontOptions( 12.5f, juce::Font::bold ) );
        g.drawText(
            "Being written: " + juce::String( d.m_name.empty() ? "your journey" : d.m_name ) + ", " +
                juce::String( static_cast<int>( d.m_steps.size() ) ) + " steps, about " +
                juce::String( ambient::Journey::timeText( journeySeconds( d ) ) ) +
                "   -- drag an edge: how long it stays; the ramp: how long it fades in; the block: where it goes; right-click: remove",
            juce::Rectangle<float>( r1.getX(), r1.getY() - 21.0f, r1.getWidth(), 18.0f ),
            juce::Justification::centredLeft );
        drawBand( g, d, r1, -1, 0.0, false );
    }
}

void JourneyTimeline::resized()
{
    m_copy.setBounds( getWidth() - 118, 5, 104, 20 );
}

int JourneyTimeline::draftAt( juce::Point<float> p, DragE &drag ) const
{
    drag = DragE::NONE;
    const auto r = band( 1 );
    if ( r.isEmpty() || !r.expanded( 4.0f ).contains( p ) ) return -1;
    const auto bs = blocks( m_proc.journey(), r );
    for ( size_t i = 0; i < bs.size(); ++i )
    {
        const Block &b = bs[i];
        if ( p.x < b.m_x0 || p.x > b.m_x1 + 4.0f ) continue;
        if ( p.x >= b.m_x1 - 6.0f )
            drag = DragE::DWELL;
        else if ( p.x < b.m_xg )
            drag = DragE::FADE;
        else
            drag = DragE::MOVE;
        return static_cast<int>( i );
    }
    return -1;
}

void JourneyTimeline::mouseMove( const juce::MouseEvent &e )
{
    DragE d = DragE::NONE;
    draftAt( e.position, d );
    setMouseCursor( d == DragE::DWELL || d == DragE::FADE ? juce::MouseCursor::LeftRightResizeCursor
                    : d == DragE::MOVE                    ? juce::MouseCursor::DraggingHandCursor
                                                          : juce::MouseCursor::NormalCursor );
}

void JourneyTimeline::mouseDown( const juce::MouseEvent &e )
{
    // The journey above: a click on a step goes there (starting the one chosen when none runs).
    const auto       r0 = band( 0 );
    ambient::Journey j;
    if ( r0.contains( e.position ) && upper( j ) )
    {
        const auto bs = blocks( j, r0 );
        for ( size_t i = 0; i < bs.size(); ++i )
            if ( e.position.x >= bs[i].m_x0 && e.position.x < bs[i].m_x1 )
            {
                if ( !m_proc.journeyPlayer().running() )
                {
                    if ( m_startAt ) m_startAt( static_cast<int>( i ) );
                }
                else
                {
                    m_proc.skipJourneyTo( static_cast<int>( i ) );
                }
                repaint();
                return;
            }
        return;
    }
    // The one being written.
    DragE     d = DragE::NONE;
    const int i = draftAt( e.position, d );
    if ( i < 0 ) return;
    if ( e.mods.isPopupMenu() )
    {
        juce::PopupMenu m;
        m.addItem( "Remove " + juce::String( m_proc.journey().m_steps[static_cast<size_t>( i )].m_preset ),
                   [this, i]
                   {
                       ambient::Journey dj = m_proc.journey();
                       dj.m_steps.erase( dj.m_steps.begin() + i );
                       m_proc.setJourney( dj );
                       repaint();
                   } );
        m.showMenuAsync( juce::PopupMenu::Options().withTargetComponent( this ).withTargetScreenArea(
            juce::Rectangle<int>( e.getScreenX(), e.getScreenY(), 1, 1 ) ) );
        return;
    }
    m_drag = d;
    m_dragStep = i;
    m_dragX = e.position.x;
    m_before = m_proc.journey().m_steps[static_cast<size_t>( i )];
    m_perPixel = journeySeconds( m_proc.journey() ) / std::max( 1.0f, band( 1 ).getWidth() );
    startTimerHz( 10 );
}

void JourneyTimeline::mouseDrag( const juce::MouseEvent &e )
{
    if ( m_drag == DragE::NONE || m_dragStep < 0 || m_dragStep >= static_cast<int>( m_proc.journey().m_steps.size() ) )
        return;
    ambient::Journey      dj = m_proc.journey();
    ambient::JourneyStep &s = dj.m_steps[static_cast<size_t>( m_dragStep )];
    const double moved = ( e.position.x - m_dragX ) * m_perPixel;   // seconds, on the scale the drag began with
    if ( m_drag == DragE::DWELL || m_drag == DragE::FADE )
    {
        const double lo = m_drag == DragE::DWELL ? m_before.m_dwellLo : m_before.m_fadeLo,
                     hi = m_drag == DragE::DWELL ? m_before.m_dwellHi : m_before.m_fadeHi;
        const double mean = 0.5 * ( lo + hi ), least = m_drag == DragE::DWELL ? 30.0 : 0.0;
        const double now = std::max(
            least, mean + ( m_drag == DragE::FADE ? -moved : moved ) );   // the ramp: dragged left, a longer fade
        const double k = mean > 0.0 ? now / mean : 1.0;                   // the span kept in proportion
        const double newLo = mean > 0.0 ? lo * k : now, newHi = mean > 0.0 ? hi * k : now;
        if ( m_drag == DragE::DWELL )
        {
            s.m_dwellLo = newLo;
            s.m_dwellHi = newHi;
        }
        else
        {
            s.m_fadeLo = newLo;
            s.m_fadeHi = newHi;
        }
        m_proc.setJourney( dj );
    }
    else
    {   // the block: where it would land
        DragE ignored = DragE::NONE;
        m_dropAt = draftAt( { e.position.x, band( 1 ).getCentreY() }, ignored );
    }
    repaint();
}

void JourneyTimeline::mouseUp( const juce::MouseEvent & )
{
    if ( m_drag == DragE::MOVE && m_dropAt >= 0 && m_dropAt != m_dragStep &&
         m_dragStep < static_cast<int>( m_proc.journey().m_steps.size() ) )
    {
        ambient::Journey           dj = m_proc.journey();
        const ambient::JourneyStep s = dj.m_steps[static_cast<size_t>( m_dragStep )];
        dj.m_steps.erase( dj.m_steps.begin() + m_dragStep );
        dj.m_steps.insert( dj.m_steps.begin() + std::min( m_dropAt, static_cast<int>( dj.m_steps.size() ) ), s );
        m_proc.setJourney( dj );
    }
    m_drag = DragE::NONE;
    m_dragStep = -1;
    m_dropAt = -1;
    startTimerHz( 2 );
    repaint();
}

// ---------------------------------------------------------------- the overlay

JourneyPanel::JourneyPanel( NoctuaryProcessor &proc, std::function<bool( ambient::Journey & )> chosen,
                            std::function<void( int )> startAt )
    : m_timeline( proc, std::move( chosen ), std::move( startAt ) )
{
    m_close.onClick = [this] { setVisible( false ); };
    addAndMakeVisible( m_timeline );
    addAndMakeVisible( m_close );
    setWantsKeyboardFocus( true );
    setVisible( false );
}

void JourneyPanel::open()
{
    if ( auto *p = getParentComponent() ) setBounds( p->getLocalBounds() );
    setVisible( true );
    toFront( true );
    resized();
    grabKeyboardFocus();
}

juce::Rectangle<int> JourneyPanel::panel() const
{
    const int w = std::min( 1100, getWidth() - 60 ), h = std::min( 300, getHeight() - 160 );
    return { ( getWidth() - w ) / 2, 140, w, h };
}

void JourneyPanel::resized()
{
    auto r = panel().reduced( 10 );
    auto top = r.removeFromTop( 28 );
    m_close.setBounds( top.removeFromRight( 80 ).reduced( 2 ) );
    r.removeFromTop( 6 );
    m_timeline.setBounds( r );
}

void JourneyPanel::paint( juce::Graphics &g )
{
    g.fillAll( ui::Bg0.withAlpha( 0.55f ) );
    const auto p = panel().toFloat();
    g.setColour( ui::GroupColour );
    g.fillRoundedRectangle( p, 8.0f );
    g.setColour( ui::MorphCol.withAlpha( 0.6f ) );
    g.drawRoundedRectangle( p.reduced( 0.5f ), 8.0f, 1.2f );
    g.setColour( ui::MorphCol );
    g.setFont( juce::FontOptions( 15.0f, juce::Font::bold ) );
    g.drawText( "Journeys over time", panel().reduced( 14, 10 ).withHeight( 24 ), juce::Justification::centredLeft );
}

void JourneyPanel::mouseDown( const juce::MouseEvent &e )
{
    if ( !panel().contains( e.getPosition() ) ) setVisible( false );
}

bool JourneyPanel::keyPressed( const juce::KeyPress &key )
{
    if ( key.isKeyCode( juce::KeyPress::escapeKey ) )
    {
        setVisible( false );
        return true;
    }
    return false;
}
