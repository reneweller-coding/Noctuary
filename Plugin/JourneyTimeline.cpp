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
                            " steps, about " + juce::String( ambient::Journey::timeText( j.meanLength() ) ) +
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
                juce::String( ambient::Journey::timeText( d.meanLength() ) ) +
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

// ---------------------------------------------------------------- the night director's controls

NightBar::NightBar( NoctuaryProcessor &proc ) : m_proc( proc )
{
    m_length.setTooltip( "How long the night is: until a wake time (counted from now), or so many hours" );
    for ( int c = 0; c < ambient::numNightChoices(); ++c ) m_choice.addItem( ambient::nightChoiceName( c ), c + 1 );
    m_choice.setSelectedId( 1, juce::dontSendNotification );
    m_choice.setTooltip(
        "Which presets the night draws from: every pack, the packs of one family of artists (Sleep, Deep, Ritual, Cold, British, Luminous, Space), or a single pack. The curve is in percentiles of what is chosen, so it fits any choice." );
    m_favourites.setToggleState( true, juce::dontSendNotification );
    m_favourites.setTooltip( "The starred presets (the browser's favourites) are chosen a little more readily" );
    m_dice.setTooltip( "Another night of the same kind: new dice, made at once" );
    m_make.setTooltip(
        "Writes the night as the journey being written (the timeline below shows it). Nothing starts by itself: Save ... keeps it, Play starts it." );
    m_save.setTooltip( "Writes the journey being written into Documents/Noctuary/Journeys under a name" );
    m_play.setTooltip( "Starts the night now (once through; at its end the morning's preset stays)" );
    m_dice.onClick = [this]
    {
        m_seed = static_cast<uint64_t>( juce::Random::getSystemRandom().nextInt64() ) | 1ull;
        make();
    };
    m_make.onClick = [this] { make(); };
    m_save.onClick = [this]
    {
        if ( m_onSave ) m_onSave();
    };
    m_play.onClick = [this]
    {
        if ( !m_proc.journey().m_steps.empty() ) m_proc.startJourney( m_proc.journey() );
    };
    m_summary.setJustificationType( juce::Justification::topLeft );
    m_summary.setColour( juce::Label::textColourId, ui::Text );
    m_summary.setFont( juce::FontOptions( 13.0f ) );
    m_summary.setText( "A whole night as a journey: falling asleep (darker, less motion), a deep middle in waves of "
                       "about ninety minutes, a morning (brighter, more motion). Choose the length and the presets, "
                       "then Make.",
                       juce::dontSendNotification );
    for ( juce::Component *c : { static_cast<juce::Component *>( &m_length ), static_cast<juce::Component *>( &m_choice ),
                                 static_cast<juce::Component *>( &m_favourites ), static_cast<juce::Component *>( &m_dice ),
                                 static_cast<juce::Component *>( &m_make ), static_cast<juce::Component *>( &m_save ),
                                 static_cast<juce::Component *>( &m_play ), static_cast<juce::Component *>( &m_summary ) } )
        addAndMakeVisible( c );
    refreshLengths();
}

void NightBar::refreshLengths()
{
    // The wake times a night may run to from now (one to fourteen hours away, to five minutes), then plain lengths.
    const juce::Time now = juce::Time::getCurrentTime();
    const double     hourNow = now.getHours() + now.getMinutes() / 60.0;
    const int        keep = m_length.getSelectedId();
    const juce::String kept = keep > 0 ? m_length.getItemText( keep - 1 ) : juce::String();
    m_length.clear( juce::dontSendNotification );
    m_lengthHours.clear();
    int preselect = 0;
    for ( int half = 10; half <= 18; ++half )   // 5:00 .. 9:00
    {
        const double wake = half * 0.5;
        double       h = wake - hourNow;
        if ( h <= 0.0 ) h += 24.0;
        h = std::round( h * 12.0 ) / 12.0;
        if ( h < 1.0 || h > 14.0 ) continue;
        m_lengthHours.push_back( h );
        const juce::String text = "Until " + juce::String( half / 2 ) + ( half % 2 ? ":30" : ":00" );
        m_length.addItem( text, static_cast<int>( m_lengthHours.size() ) );
        if ( half == 14 && h >= 3.0 && h <= 10.0 ) preselect = static_cast<int>( m_lengthHours.size() );
    }
    for ( int h = 3; h <= 10; ++h )
    {
        m_lengthHours.push_back( h );
        m_length.addItem( juce::String( h ) + " hours", static_cast<int>( m_lengthHours.size() ) );
        if ( h == 8 && preselect == 0 ) preselect = static_cast<int>( m_lengthHours.size() );
    }
    int again = 0;   // what was chosen, if it is still offered
    for ( int i = 0; i < m_length.getNumItems() && kept.isNotEmpty(); ++i )
        if ( m_length.getItemText( i ) == kept ) again = i + 1;
    m_length.setSelectedId( again > 0 ? again : preselect, juce::dontSendNotification );
}

void NightBar::make( double hours, uint64_t seed )
{
    if ( seed != 0 ) m_seed = seed;
    const int id = m_length.getSelectedId();
    if ( hours <= 0.0 )
        hours = id > 0 && id <= static_cast<int>( m_lengthHours.size() ) ? m_lengthHours[static_cast<size_t>( id - 1 )]
                                                                          : 8.0;
    const juce::Time now = juce::Time::getCurrentTime();
    m_startHour = now.getHours() + now.getMinutes() / 60.0;
    // The name, which is also the file's when it is saved: no colon.
    const juce::String name = "Night " + juce::String( std::round( hours * 4.0 ) / 4.0 ) + " h (" +
                              now.formatted( "%d.%m." ) + ")";
    m_hours = hours;
    m_plan = m_proc.makeNight( hours, std::max( 0, m_choice.getSelectedId() - 1 ), m_seed,
                               m_favourites.getToggleState(), name.toStdString() );
    if ( m_plan.m_journey.m_steps.empty() )
    {
        m_summary.setText( "No preset to choose from: this library has none of the director's presets in that choice "
                           "(or every one is marked never again).",
                           juce::dontSendNotification );
        repaint();
        return;
    }
    double asleep = 0.0, morning = 0.0;
    int    waves = 1;
    ambient::nightPhases( hours, asleep, morning, waves );
    const double middle = ( 1.0 - asleep - morning ) * hours * 60.0;
    int          preferred = 0;
    for ( const ambient::NightStep &s : m_plan.m_steps ) preferred += s.m_preferred ? 1 : 0;
    juce::String text;
    text << name << ": " << static_cast<int>( m_plan.m_steps.size() ) << " presets of " << m_plan.m_candidates
         << " -- falling asleep " << juce::roundToInt( asleep * hours * 60.0 ) << " min, " << waves
         << ( waves == 1 ? " wave of " : " waves of " ) << juce::roundToInt( middle / waves ) << " min, morning "
         << juce::roundToInt( morning * hours * 60.0 ) << " min" << ( preferred > 0 ? ", " : "" )
         << ( preferred > 0 ? juce::String( preferred ) + " of your favourites" : juce::String() ) << ".\n"
         << "It is the journey being written now (below). Save ... keeps it, Play starts it; New dice makes another.";
    m_summary.setText( text, juce::dontSendNotification );
    repaint();
    if ( auto *p = getParentComponent() ) p->repaint();
}

juce::Rectangle<float> NightBar::curveArea() const
{
    return getLocalBounds().toFloat().withTrimmedTop( 34.0f + 44.0f ).reduced( 40.0f, 4.0f ).withTrimmedBottom( 16.0f );
}

void NightBar::resized()
{
    auto r = getLocalBounds();
    auto row = r.removeFromTop( 28 );
    m_length.setBounds( row.removeFromLeft( 150 ) );
    row.removeFromLeft( 8 );
    m_choice.setBounds( row.removeFromLeft( 230 ) );
    row.removeFromLeft( 8 );
    m_favourites.setBounds( row.removeFromLeft( 240 ) );
    row.removeFromLeft( 8 );
    m_dice.setBounds( row.removeFromLeft( 90 ) );
    row.removeFromLeft( 6 );
    m_make.setBounds( row.removeFromLeft( 80 ) );
    m_play.setBounds( row.removeFromRight( 70 ) );
    row.removeFromRight( 6 );
    m_save.setBounds( row.removeFromRight( 90 ) );
    r.removeFromTop( 6 );
    m_summary.setBounds( r.removeFromTop( 42 ) );
}

void NightBar::paint( juce::Graphics &g )
{
    const auto area = curveArea();
    if ( area.getHeight() < 30.0f ) return;
    const float  laneH = ( area.getHeight() - 8.0f ) / 2.0f;
    const double hours = m_hours > 0.0 ? m_hours : 8.0;
    const double total = hours * 3600.0;
    g.setFont( juce::FontOptions( 11.0f ) );
    for ( int lane = 0; lane < 2; ++lane )
    {
        const juce::Rectangle<float> r = area.withHeight( laneH ).translated( 0.0f, lane * ( laneH + 8.0f ) );
        g.setColour( ui::CardColour );
        g.fillRoundedRectangle( r, 4.0f );
        g.setColour( ui::Dim );
        // Up is more in both lanes: the first is the motion the night wants (its percentile, 0 the stillest), so the
        // deep of the night lies low in it as it lies dark in the second.
        g.drawText( lane == 0 ? "motion" : "bright",juce::Rectangle<float>( 0.0f, r.getY(), 38.0f, r.getHeight() ),
                    juce::Justification::centred );
        // the curve, dashed
        juce::Path curve;
        for ( int k = 0; k <= 200; ++k )
        {
            double s = 0.0, b = 0.0;
            ambient::nightCurve( k / 200.0, hours, s, b );
            const float x = r.getX() + r.getWidth() * static_cast<float>( k / 200.0 );
            const float y = r.getBottom() - r.getHeight() * static_cast<float>( lane == 0 ? s : b );
            if ( k == 0 )
                curve.startNewSubPath( x, y );
            else
                curve.lineTo( x, y );
        }
        juce::Path      dashed;
        const float     dash[2] = { 5.0f, 4.0f };
        juce::PathStrokeType( 1.5f ).createDashedStroke( dashed, curve, dash, 2 );
        g.setColour( ui::Live.withAlpha( 0.85f ) );
        g.fillPath( dashed );
        // the presets chosen
        g.setColour( lane == 0 ? ui::Accent : ui::BackCol );
        for ( const ambient::NightStep &s : m_plan.m_steps )
        {
            const float x0 = r.getX() + r.getWidth() * static_cast<float>( s.m_at / total );
            const float x1 = r.getX() + r.getWidth() * static_cast<float>( std::min( 1.0, ( s.m_at + s.m_stay ) / total ) );
            const float y = r.getBottom() - r.getHeight() * static_cast<float>( lane == 0 ? s.m_still : s.m_bright );
            g.fillRect( juce::Rectangle<float>( x0, y - 1.5f, std::max( 1.0f, x1 - x0 - 1.0f ), 3.0f ) );
        }
    }
    // the hours under it, on the clock from the night's start
    g.setColour( ui::Dim );
    for ( int h = 0; h <= static_cast<int>( hours ); ++h )
    {
        const float x = area.getX() + area.getWidth() * static_cast<float>( h * 3600.0 / total );
        const int   m = juce::roundToInt( ( m_startHour + h ) * 60.0 ) % ( 24 * 60 );
        g.drawText( juce::String( m / 60 ) + ":" + juce::String( m % 60 ).paddedLeft( '0', 2 ),
                    juce::Rectangle<float>( x - 20.0f, area.getBottom() + 2.0f, 40.0f, 13.0f ),
                    juce::Justification::centred );
    }
}

// ---------------------------------------------------------------- the overlay

JourneyPanel::JourneyPanel( NoctuaryProcessor &proc, std::function<bool( ambient::Journey & )> chosen,
                            std::function<void( int )> startAt )
    : m_timeline( proc, std::move( chosen ), std::move( startAt ) ), m_night( proc )
{
    m_close.onClick = [this] { setVisible( false ); };
    m_nightButton.setClickingTogglesState( true );
    m_nightButton.setTooltip(
        "The night director: a whole night as a journey with a form -- falling asleep, a deep middle in waves, a morning" );
    m_nightButton.onClick = [this] { showNight( m_nightButton.getToggleState() ); };
    addAndMakeVisible( m_timeline );
    addChildComponent( m_night );
    addAndMakeVisible( m_nightButton );
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

void JourneyPanel::openNight( double makeHours )
{
    open();
    showNight( true );
    if ( makeHours > 0.0 ) m_night.make( makeHours, 1 );
}

void JourneyPanel::showNight( bool on )
{
    m_nightButton.setToggleState( on, juce::dontSendNotification );
    if ( on ) m_night.refreshLengths();
    m_night.setVisible( on );
    resized();
    repaint();
}

juce::Rectangle<int> JourneyPanel::panel() const
{
    const int w = std::min( 1100, getWidth() - 60 );
    const int h = std::min( m_night.isVisible() ? 560 : 300, getHeight() - 160 );
    return { ( getWidth() - w ) / 2, m_night.isVisible() ? 90 : 140, w, h };
}

void JourneyPanel::resized()
{
    auto r = panel().reduced( 10 );
    auto top = r.removeFromTop( 28 );
    m_close.setBounds( top.removeFromRight( 80 ).reduced( 2 ) );
    top.removeFromRight( 6 );
    m_nightButton.setBounds( top.removeFromRight( 96 ).reduced( 2 ) );
    r.removeFromTop( 6 );
    if ( m_night.isVisible() )
    {
        m_night.setBounds( r.removeFromTop( 250 ).reduced( 8, 0 ) );
        r.removeFromTop( 6 );
    }
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
