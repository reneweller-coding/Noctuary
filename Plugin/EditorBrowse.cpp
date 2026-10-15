/**
 * @file EditorBrowse.cpp
 * @brief The browse page (columns, list, map) and the perform page.
 *
 * Two of the editor's three pages live here; the third, the panel itself, is PluginEditor.cpp, which
 * owns both of these as m_browse and m_perform and swaps them in with setPage(). Everything runs on
 * the message thread and reads the instrument through the processor: the parameters through apvts,
 * the engine's map cursor, route and morph state through engine() getters, and the presets through
 * the accessors of Presets.h and PresetMeta.h. Nothing here touches audio.
 *
 * The browse page is BrowseView, and it has two modes. "Columns" is the classic browser --
 * Family | Character | Motion | Features narrowing the list, the preset's own card (InfoPanel)
 * beside the results. "Map" is the measured plane of Tools/library/map_all.py: every preset a dot at
 * the place its measurements put it (MapView), the four macro range sliders thinning the cloud along
 * one descriptor each, the blend cursor and its radius, and the route strip that walks the cursor
 * from waypoint to waypoint. Both modes share the search box, the sort, the favourites, the "similar"
 * narrowing and the morph-on-select, and both end in applyFilter(), which builds the one list
 * (filtered) that the list box, the map and the info line all read.
 *
 * The map is the expensive picture: with a library loaded there are thousands of dots, so the cloud
 * is drawn once into an image and only what moves -- the cursor, the ring of what is playing, the
 * crossing while a preset change travels, the names once zoomed in -- is drawn live over it. The
 * colours come from the two helpers at the top of the file: familyColour() for where a preset came
 * from, clusterColour() for the measured group it sounds like.
 *
 * The perform page is PerformView: the eight macros as large knobs, the morph slider between the
 * A and B slots, and the set recorder (record every knob, gesture and note with its time; play a set
 * back; ambient_render --set-file renders it again offline).
 * @author Rene Weller
 */
#include "EditorCommon.h"

using namespace ambient;

// ---------------------------------------------------------------- browse page

namespace
{
/**
 * @brief The colours of the twelve built-in preset families, indexed by family.
 *
 * The twelve built-in families keep their colours; loaded packs get their own hues, spaced by
 * the golden angle so neighbouring packs never look alike.
 */
const juce::Colour FamilyColours[] = {
    juce::Colour( 0xff7fb3d5 ), juce::Colour( 0xff8ec9a8 ), juce::Colour( 0xffe0c070 ), juce::Colour( 0xffd08a8a ),
    juce::Colour( 0xffb094d8 ), juce::Colour( 0xff70c8c8 ), juce::Colour( 0xffe09a60 ), juce::Colour( 0xffa0b8e0 ),
    juce::Colour( 0xffc8d870 ), juce::Colour( 0xffd880b8 ), juce::Colour( 0xff90d0f0 ), juce::Colour( 0xffd0d0d0 ),
};
}   // namespace
namespace edt
{
/**
 * @brief A colour per measured group.
 *
 * The families are hues by index; the groups get their own ramp so
 * the two colourings cannot be confused with one another at a glance. The hue steps by the golden
 * angle from group to group; when there is more than one group the brightness is stepped in three
 * as well, so two groups that landed on nearby hues still tell apart.
 * @param[in] c  the group index as presetClusterOf() gives it, 0 .. numPresetClusters() - 1; negative means the preset
 *               is in no group
 * @return the group's colour, opaque; a neutral grey for a negative index
 */
juce::Colour clusterColour( int c )
{
    if ( c < 0 ) return juce::Colour( 0xff707880 );
    const int   n = juce::jmax( 1, numPresetClusters() );
    const float h = std::fmod( 0.08f + 0.61803398f * static_cast<float>( c ), 1.0f );
    return juce::Colour::fromHSV( h, 0.55f, 0.92f, 1.0f )
        .withMultipliedBrightness( 0.85f + 0.3f * ( static_cast<float>( c % 3 ) / 3.0f ) * ( n > 1 ? 1.0f : 0.0f ) );
}

/**
 * The first twelve families are the built-ins and take their colour from FamilyColours; a family
 * past them is a loaded pack, and is given a hue of its own, stepped by the golden angle from the
 * last built-in, so packs loaded one after another never share a colour with a neighbour. A
 * negative family reads as family 0.
 */
juce::Colour familyColour( int f )
{
    constexpr int BuiltIn = static_cast<int>( sizeof( FamilyColours ) / sizeof( FamilyColours[0] ) );
    if ( f < 0 ) f = 0;
    if ( f < BuiltIn ) return FamilyColours[static_cast<size_t>( f )];
    const float hue = std::fmod( 0.08f + 0.6180339f * static_cast<float>( f - BuiltIn + 1 ), 1.0f );
    return juce::Colour::fromHSV( hue, 0.42f, 0.82f, 1.0f );
}
}   // namespace edt

NoctuaryEditor::BrowseView::BrowseView( NoctuaryProcessor &p ) : m_proc( p ), m_map( *this )
{
    m_search.setTextToShowWhenEmpty( "search", Dim );
    m_search.onTextChange = [this] { applyFilter(); };
    addAndMakeVisible( m_search );
    m_family.addItem( "all families", 1 );
    for ( int f = 0; f < numPresetFamilies(); ++f ) m_family.addItem( presetFamilyName( f ), f + 2 );
    m_family.setSelectedId( 1, juce::dontSendNotification );
    m_family.onChange = [this] { applyFilter(); };
    addAndMakeVisible( m_family );
    m_sort.addItem( "by number", 1 );
    m_sort.addItem( "by name", 2 );
    m_sort.addItem( "dark -> bright", 3 );
    m_sort.addItem( "calm -> moving", 4 );
    m_sort.addItem( "narrow -> wide", 5 );
    m_sort.addItem( "tonal -> noisy", 6 );
    m_sort.addItem( "sparse -> dense", 7 );
    m_sort.addItem( "still -> evolving", 8 );
    m_sort.addItem( "smooth -> rough", 9 );
    m_sort.addItem( "near -> far", 10 );
    m_sort.setSelectedId( 1, juce::dontSendNotification );
    m_sort.onChange = [this] { applyFilter(); };
    addAndMakeVisible( m_sort );
    for ( int t = 0; t < NumPresetTags; ++t )
    {
        auto b = std::make_unique<juce::ToggleButton>( presetTagName( t ) );
        b->onClick = [this] { applyFilter(); };
        addAndMakeVisible( *b );
        m_tagButtons.push_back( std::move( b ) );
    }
    m_list.setModel( this );
    m_list.setRowHeight( 22 );
    m_list.setColour( juce::ListBox::backgroundColourId, Bg );
    addAndMakeVisible( m_list );
    addAndMakeVisible( m_map );
    m_mapActive.setButtonText( "Map blend (drag the cursor)" );
    addAndMakeVisible( m_mapActive );
    m_mapActiveAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        m_proc.m_apvts, paramDesc( ParamIdE::MAP_ACTIVE ).m_key, m_mapActive );
    m_radius.setSliderStyle( juce::Slider::LinearHorizontal );
    m_radius.setTextBoxStyle( juce::Slider::TextBoxRight, false, 50, 18 );
    addAndMakeVisible( m_radius );
    m_radiusAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        m_proc.m_apvts, paramDesc( ParamIdE::MAP_RADIUS ).m_key, m_radius );

    // The four macro sliders: one measured axis each, as a range rather than a point, so "show me
    // the dark half" and "show me the darkest tenth" are the same gesture. Wide open they filter
    // nothing. Narrow one and the cloud thins to what is left and the view closes in on it -- the
    // thing an even field could never show, that a hundred presets here are nearly one sound.
    static const char *const MacroNames[4] = { "dark - bright", "still - moving", "smooth - rough", "near - far" };
    float PresetMeta::*const MacroFields[4] = { &PresetMeta::m_bright, &PresetMeta::m_evolve, &PresetMeta::m_rough,
                                                &PresetMeta::m_wet };
    static const char *const MacroTips[4] = {
        "Where the spectrum's weight sits: left the dark presets, right the bright ones.",
        "How far the sound travels over a minute -- the drone's own axis, in place of the attack an ambient patch does not have.",
        "The roughness of its partials against each other (Plomp-Levelt): smooth and fused, or beating and grinding.",
        "How much of what you hear comes back from the far planes rather than standing in the near one."
    };
    for ( int i = 0; i < 4; ++i )
    {
        m_macros[i].m_field = MacroFields[i];
        m_macros[i].m_slider.setSliderStyle( juce::Slider::TwoValueHorizontal );
        m_macros[i].m_slider.setRange( 0.0, 1.0, 0.01 );
        m_macros[i].m_slider.setMinAndMaxValues( 0.0, 1.0, juce::dontSendNotification );
        m_macros[i].m_slider.setTextBoxStyle( juce::Slider::NoTextBox, false, 0, 0 );
        m_macros[i].m_slider.setTooltip( MacroTips[i] );
        m_macros[i].m_slider.onValueChange = [this]
        {
            m_fitPending = true;
            applyFilter();
        };
        m_macros[i].m_label.setText( MacroNames[i], juce::dontSendNotification );
        m_macros[i].m_label.setFont( juce::FontOptions( 10.5f ) );
        m_macros[i].m_label.setColour( juce::Label::textColourId, Dim );
        m_macros[i].m_label.setJustificationType( juce::Justification::centredRight );
        addAndMakeVisible( m_macros[i].m_slider );
        addAndMakeVisible( m_macros[i].m_label );
    }
    m_colourByGroup.setToggleState( true, juce::dontSendNotification );
    m_colourByGroup.setTooltip(
        "Colour the cloud by the measured groups -- presets that sound alike -- rather than by the pack a preset was written for." );
    m_colourByGroup.onClick = [this] { m_map.repaint(); };
    addAndMakeVisible( m_colourByGroup );
    m_info.setFont( juce::FontOptions( 12.0f ) );
    m_info.setColour( juce::Label::textColourId, Dim );
    addAndMakeVisible( m_info );
    m_load.onClick = [this]
    {
        if ( m_selected >= 0 ) m_proc.selectPreset( m_selected, m_morphOnSelect.getToggleState() );
    };
    m_toA.onClick = [this]
    {
        if ( m_selected >= 0 ) m_proc.setMorphSlotFromPreset( 0, m_selected );
    };
    m_toB.onClick = [this]
    {
        if ( m_selected >= 0 ) m_proc.setMorphSlotFromPreset( 1, m_selected );
    };
    m_star.onClick = [this]
    {
        if ( m_selected >= 0 )
        {
            m_proc.setFavourite( m_selected, !m_proc.isFavourite( m_selected ) );
            applyFilter();
        }
    };
    m_onlyFavourites.onClick = [this] { applyFilter(); };
    m_favouritesFirst.setToggleState( m_proc.favouritesFirst(), juce::dontSendNotification );
    m_favouritesFirst.setTooltip(
        "Puts the starred presets at the top of the list, whatever the sort. Remembered with the favourites (Documents\\Noctuary\\favourites.txt)." );
    m_favouritesFirst.onClick = [this]
    {
        m_proc.setFavouritesFirst( m_favouritesFirst.getToggleState() );
        applyFilter();
    };
    m_hideDull.onClick = [this] { applyFilter(); };
    m_hideDull.setTooltip(
        "Hides presets that measured as barely moving and barely wide -- the dull tail of a generated library" );
    for ( auto *b : { &m_load, &m_toA, &m_toB, &m_star } ) addAndMakeVisible( *b );
    // The map already knows which presets are alike: its neighbours are the ones that measured
    // alike. This narrows the list to the selected preset's six nearest and zooms the map onto
    // them; off again, the list is what the other filters make it.
    m_similar.onClick = [this]
    {
        m_similarSet.clear();
        m_similarOf = -1;
        if ( m_similar.getToggleState() && m_selected >= 0 && m_selected < numPresetMeta() )
        {
            const PresetMeta      &m = presetMeta( m_selected );
            const PresetMap::Blend b = PresetMap::neighbours( m.m_x, m.m_y, 0.12f );
            m_similarSet.push_back( m_selected );
            for ( int k = 0; k < b.m_count; ++k )
                if ( b.m_index[k] != m_selected ) m_similarSet.push_back( b.m_index[k] );
            m_similarOf = m_selected;
            m_map.zoomTo( m.m_x, m.m_y, 8.0f );
        }
        else if ( !m_similar.getToggleState() )
        {
            m_map.zoomTo( 0.5f, 0.5f, 1.0f );
        }
        applyFilter();
    };
    m_similar.setTooltip(
        "Narrow the list to the presets that measured most like the selected one, and zoom the map onto them" );
    addAndMakeVisible( m_similar );
    addAndMakeVisible( m_onlyFavourites );
    addAndMakeVisible( m_favouritesFirst );
    addAndMakeVisible( m_hideDull );
    addAndMakeVisible( m_info2 );
    // Choosing a preset as a journey. On by default: an instrument that plays for hours has no
    // business cutting from one world to another because somebody clicked a name.
    m_morphOnSelect.setToggleState( m_proc.morphOnSelect(), juce::dontSendNotification );
    m_morphOnSelect.setTooltip(
        "Choosing a preset morphs the whole instrument into it over the time beside this, instead of switching to it at once. What is playing becomes A, the chosen preset B; when it arrives, the preset is simply loaded." );
    m_morphOnSelect.onClick = [this] { m_proc.setMorphOnSelect( m_morphOnSelect.getToggleState() ); };
    addAndMakeVisible( m_morphOnSelect );
    m_morphSeconds.setSliderStyle( juce::Slider::LinearHorizontal );
    m_morphSeconds.setRange( 0.5, 600.0, 0.5 );
    m_morphSeconds.setSkewFactor( 0.4 );
    m_morphSeconds.setValue( m_proc.morphSelectSeconds(), juce::dontSendNotification );
    m_morphSeconds.setTextBoxStyle( juce::Slider::TextBoxRight, false, 56, 18 );
    m_morphSeconds.setTextValueSuffix( " s" );
    m_morphSeconds.setTooltip( "How long that journey takes: half a second to ten minutes." );
    m_morphSeconds.onValueChange = [this]
    { m_proc.setMorphSelectSeconds( static_cast<float>( m_morphSeconds.getValue() ) ); };
    addAndMakeVisible( m_morphSeconds );

    // Classic columns: Family | Character | Motion & density | Features. A click narrows,
    // several rows in one column combine with OR, columns combine with AND, "All" clears.
    struct Def
    {
        const char      *m_title;
        std::vector<int> m_tags;
        bool             m_families;
    };
    const Def defs[4] = {
        { "Family", {}, true },
        { "Character",
          { 0, 1, 4, 5, 6, 7, 21, 22, 23, 24 },
          false },                                     // Dark Bright Tonal Noisy Wide Bass Smooth Rough Near Far
        { "Motion", { 2, 3, 8, 9, 19, 20 }, false },   // Calm Moving Dense Sparse Still Evolving
        { "Features",
          { 10, 11, 12, 13, 14, 15, 16, 17, 18 },
          false },   // Keys Generative Cosmos Feedback Sources JI Sub Stack Air
    };
    for ( int c = 0; c < 4; ++c )
    {
        Column &col = m_columns[c];
        col.m_owner = this;
        col.m_title = defs[c].m_title;
        col.m_items.add( "All" );
        col.m_tagBits.push_back( 0 );
        col.m_familyIdx.push_back( -1 );
        if ( defs[c].m_families )
        {
            for ( int f = 0; f < numPresetFamilies(); ++f )
            {
                col.m_items.add( presetFamilyName( f ) );
                col.m_tagBits.push_back( 0 );
                col.m_familyIdx.push_back( f );
            }
        }
        else
        {
            for ( int t : defs[c].m_tags )
            {
                col.m_items.add( presetTagName( t ) );
                col.m_tagBits.push_back( 1u << t );
                col.m_familyIdx.push_back( -1 );
            }
        }
        col.m_box.setModel( &col );
        col.m_box.setRowHeight( 20 );
        col.m_box.setColour( juce::ListBox::backgroundColourId, Bg );
        addAndMakeVisible( col.m_box );
    }
    // Route strip
    m_routeBox.setTextWhenNothingSelected( "route preset" );
    for ( int r = 0; r < numRoutePresets(); ++r ) m_routeBox.addItem( routePreset( r ).m_name, r + 1 );
    m_routeBox.onChange = [this]
    {
        const int r = m_routeBox.getSelectedId() - 1;
        if ( r >= 0 && r < numRoutePresets() )
        {
            m_proc.setRouteText( routePreset( r ).m_points );
            m_map.repaint();
        }
    };
    addAndMakeVisible( m_routeBox );
    m_routePlay.setButtonText( "Play route" );
    addAndMakeVisible( m_routePlay );
    m_routePlayAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        m_proc.m_apvts, paramDesc( ParamIdE::ROUTE_ACTIVE ).m_key, m_routePlay );
    m_routeLoop.setButtonText( "loop" );
    addAndMakeVisible( m_routeLoop );
    m_routeLoopAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        m_proc.m_apvts, paramDesc( ParamIdE::ROUTE_LOOP ).m_key, m_routeLoop );
    m_routeSpeed.setSliderStyle( juce::Slider::LinearHorizontal );
    m_routeSpeed.setTextBoxStyle( juce::Slider::TextBoxRight, false, 44, 18 );
    m_routeSpeed.setTextValueSuffix( "x" );
    addAndMakeVisible( m_routeSpeed );
    m_routeSpeedAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        m_proc.m_apvts, paramDesc( ParamIdE::ROUTE_SPEED ).m_key, m_routeSpeed );
    m_routeAdd.setTooltip( "Append the current cursor (position and radius) as a waypoint: 60 s travel, 60 s hold" );
    m_routeAdd.onClick = [this]
    {
        Waypoint w;
        w.m_x = m_proc.engine().getParam( ParamIdE::MAP_X );
        w.m_y = m_proc.engine().getParam( ParamIdE::MAP_Y );
        w.m_radius = m_proc.engine().getParam( ParamIdE::MAP_RADIUS );
        w.m_travel = 60.0f;
        w.m_hold = 60.0f;
        const int near = m_map.nearestPreset( m_map.toScreen( w.m_x, w.m_y ), 8.0f );
        if ( near >= 0 )
        {
            w.m_preset = near;
            w.m_x = presetMeta( near ).m_x;
            w.m_y = presetMeta( near ).m_y;
        }
        m_proc.addRoutePoint( w );
        m_routeBox.setSelectedId( 0, juce::dontSendNotification );
        m_map.repaint();
    };
    m_routeClear.onClick = [this]
    {
        m_proc.clearRoute();
        m_routeBox.setSelectedId( 0, juce::dontSendNotification );
        m_map.repaint();
    };
    m_routeEdit.setTooltip(
        "Edit the route as text: preset|travel|hold[|radius] or x,y|travel|hold[|radius], separated by ;" );
    m_routeEdit.onClick = [this] { showRouteEditor(); };
    for ( auto *b : { &m_routeAdd, &m_routeClear, &m_routeEdit } ) addAndMakeVisible( *b );

    m_modeClassic.setClickingTogglesState( true );
    m_modeMap.setClickingTogglesState( true );
    m_modeClassic.setRadioGroupId( 77 );
    m_modeMap.setRadioGroupId( 77 );
    m_modeClassic.setColour( juce::TextButton::buttonOnColourId, Accent.withAlpha( 0.5f ) );
    m_modeMap.setColour( juce::TextButton::buttonOnColourId, Accent.withAlpha( 0.5f ) );
    m_modeClassic.onClick = [this] { setMode( 0 ); };
    m_modeMap.onClick = [this] { setMode( 1 ); };
    addAndMakeVisible( m_modeClassic );
    addAndMakeVisible( m_modeMap );
    setMode( 0 );
    applyFilter();
    startTimerHz( 15 );
}

void NoctuaryEditor::BrowseView::setMode( int m )
{
    m_mode = m;
    m_modeClassic.setToggleState( m == 0, juce::dontSendNotification );
    m_modeMap.setToggleState( m == 1, juce::dontSendNotification );
    for ( auto &c : m_columns ) c.m_box.setVisible( m == 0 );
    for ( auto &b : m_tagButtons ) b->setVisible( m == 1 );
    m_family.setVisible( m == 1 );
    m_map.setVisible( m == 1 );
    m_mapActive.setVisible( m == 1 );
    m_radius.setVisible( m == 1 );
    m_colourByGroup.setVisible( m == 1 );
    for ( auto &mc : m_macros )
    {
        mc.m_slider.setVisible( m == 1 );
        mc.m_label.setVisible( m == 1 );
    }
    for ( juce::Component *c :
          { static_cast<juce::Component *>( &m_routeBox ), static_cast<juce::Component *>( &m_routePlay ),
            static_cast<juce::Component *>( &m_routeLoop ), static_cast<juce::Component *>( &m_routeSpeed ),
            static_cast<juce::Component *>( &m_routeAdd ), static_cast<juce::Component *>( &m_routeClear ),
            static_cast<juce::Component *>( &m_routeEdit ) } )
        c->setVisible( m == 1 );
    resized();
    applyFilter();
}

void NoctuaryEditor::BrowseView::Column::paintListBoxItem( int row, juce::Graphics &g, int w, int h, bool )
{
    const bool on = ( row == 0 ) ? m_chosen.empty() : m_chosen.count( row ) > 0;
    if ( on ) g.fillAll( Accent.withAlpha( 0.18f ) );
    g.setColour( on ? Text : Dim );
    g.setFont( juce::FontOptions( 12.5f ) );
    g.drawText( m_items[row], 8, 0, w - 40, h, juce::Justification::centredLeft );
    // count of presets this row would leave -- cached, because counting five thousand presets
    // per painted row per repaint is not something a list box should be doing
    if ( row > 0 && m_owner != nullptr )
    {
        updateCounts();
        if ( static_cast<size_t>( row ) < m_counts.size() )
        {
            g.setColour( Dim.withAlpha( 0.7f ) );
            g.setFont( juce::FontOptions( 10.5f ) );
            g.drawText( juce::String( m_counts[static_cast<size_t>( row )] ), w - 34, 0, 28, h,
                        juce::Justification::centredRight );
        }
    }
}

void NoctuaryEditor::BrowseView::Column::updateCounts()
{
    if ( m_countsFor == numPresets() && m_counts.size() == static_cast<size_t>( m_items.size() ) ) return;
    m_countsFor = numPresets();
    m_counts.assign( static_cast<size_t>( m_items.size() ), 0 );
    for ( int i = 0; i < numPresets(); ++i )
    {
        const PresetMeta &m = presetMeta( i );
        for ( size_t r = 1; r < m_counts.size(); ++r )
            if ( ( m_familyIdx[r] >= 0 && m.m_family == m_familyIdx[r] ) ||
                 ( m_tagBits[r] && ( m.m_tags & m_tagBits[r] ) ) )
                ++m_counts[r];
    }
}

void NoctuaryEditor::BrowseView::Column::listBoxItemClicked( int row, const juce::MouseEvent &e )
{
    if ( row == 0 )
        m_chosen.clear();
    else if ( e.mods.isCommandDown() || e.mods.isCtrlDown() )
    {
        if ( m_chosen.count( row ) )
            m_chosen.erase( row );
        else
            m_chosen.insert( row );
    }
    else if ( m_chosen.size() == 1 && m_chosen.count( row ) )
        m_chosen.clear();
    else
    {
        m_chosen.clear();
        m_chosen.insert( row );
    }
    m_box.repaint();
    if ( m_owner ) m_owner->applyFilter();
}

bool NoctuaryEditor::BrowseView::Column::passes( int preset ) const
{
    if ( m_chosen.empty() ) return true;
    const PresetMeta &m = presetMeta( preset );
    for ( int row : m_chosen )
    {
        const size_t r = static_cast<size_t>( row );
        if ( m_familyIdx[r] >= 0 && m.m_family == m_familyIdx[r] ) return true;
        if ( m_tagBits[r] && ( m.m_tags & m_tagBits[r] ) ) return true;
    }
    return false;
}

void NoctuaryEditor::BrowseView::applyFilter()
{
    ++m_filterGen;   // the map's cached cloud is drawn from this list
    m_filtered.clear();
    const juce::String needle = m_search.getText().trim().toLowerCase();
    const int          fam = m_family.getSelectedId() - 2;
    uint32_t           need = 0;
    for ( int t = 0; t < NumPresetTags; ++t )
        if ( m_tagButtons[static_cast<size_t>( t )]->getToggleState() ) need |= ( 1u << t );
    for ( int i = 0; i < numPresets(); ++i )
    {
        const PresetMeta &m = presetMeta( i );
        if ( needle.isNotEmpty() && !juce::String( preset( i ).m_name ).toLowerCase().contains( needle ) ) continue;
        if ( m_onlyFavourites.getToggleState() && !m_proc.isFavourite( i ) ) continue;
        if ( !m_similarSet.empty() && std::find( m_similarSet.begin(), m_similarSet.end(), i ) == m_similarSet.end() )
            continue;
        // Dull, measured rather than judged: in the bottom fifth for movement and for width, and
        // not carrying the sparseness that would make that a deliberate character.
        if ( m_hideDull.getToggleState() && m.m_motion < 0.2f && m.m_width < 0.2f && m.m_density < 0.5f ) continue;
        if ( m_mode == 1 )
        {   // the macro ranges
            bool inRange = true;
            for ( const auto &mc : m_macros )
            {
                const float v = m.*( mc.m_field );
                const float lo = static_cast<float>( mc.m_slider.getMinValue() ),
                            hi = static_cast<float>( mc.m_slider.getMaxValue() );
                if ( lo > 0.0f || hi < 1.0f )
                    if ( v < lo - 1.0e-4f || v > hi + 1.0e-4f )
                    {
                        inRange = false;
                        break;
                    }
            }
            if ( !inRange ) continue;
        }
        if ( m_mode == 1 )
        {
            if ( fam >= 0 && m.m_family != fam ) continue;
            if ( ( m.m_tags & need ) != need ) continue;
        }
        else
        {
            bool ok = true;
            for ( const auto &c : m_columns )
                if ( !c.passes( i ) )
                {
                    ok = false;
                    break;
                }
            if ( !ok ) continue;
        }
        m_filtered.push_back( i );
    }
    const int s = m_sort.getSelectedId();
    auto      key = [s]( int i ) -> float
    {
        const PresetMeta &m = presetMeta( i );
        switch ( s )
        {
        case 3: return m.m_bright;
        case 4: return m.m_motion;
        case 5: return m.m_width;
        case 6: return m.m_noisy;
        case 7: return m.m_density;
        case 8: return m.m_evolve;
        case 9: return m.m_rough;
        case 10: return m.m_wet;
        default: return 0.0f;
        }
    };
    if ( s == 2 )
        std::sort( m_filtered.begin(), m_filtered.end(), []( int a, int b )
                   { return juce::String( preset( a ).m_name ).compareIgnoreCase( preset( b ).m_name ) < 0; } );
    else if ( s >= 3 )
        std::stable_sort( m_filtered.begin(), m_filtered.end(), [&]( int a, int b ) { return key( a ) < key( b ); } );
    // The stars to the top, in the order the sort left them (Rene, 13.09.2026).
    if ( m_favouritesFirst.getToggleState() )
        std::stable_partition( m_filtered.begin(), m_filtered.end(),
                               [this]( int i ) { return m_proc.isFavourite( i ); } );
    m_list.updateContent();
    m_info.setText( juce::String( static_cast<int>( m_filtered.size() ) ) + " of " + juce::String( numPresets() ) +
                        " presets" +
                        ( numPresetMeta() == 0 ? "   (map not measured yet: run Tools/library/map_all.py)" : "" ),
                    juce::dontSendNotification );
    if ( m_fitPending ) fitToFilter();
    m_map.repaint();
}

/**
 * The info panel. Headings in the accent colour, the prose under them, wrapped -- the same shape
 * u-he uses, because it is the right one: name at the top, then what it is, then what your hands
 * do, then where it is filed.
 */
void NoctuaryEditor::BrowseView::InfoPanel::paint( juce::Graphics &g )
{
    auto r = getLocalBounds();
    g.setColour( ui::CardColour.withAlpha( 0.55f ) );
    g.fillRoundedRectangle( r.toFloat(), 6.0f );
    r = r.reduced( 12, 10 );
    if ( m_title.isEmpty() )
    {
        g.setColour( Dim );
        g.setFont( juce::FontOptions( 11.5f ) );
        g.drawText( "PRESET INFO", r.removeFromTop( 16 ), juce::Justification::topLeft );
        g.setColour( ui::Faint );
        g.setFont( juce::FontOptions( 11.0f ) );
        g.drawText( "choose a preset to read what it is", r, juce::Justification::topLeft );
        return;
    }
    g.setColour( Text );
    g.setFont( juce::FontOptions( 13.0f, juce::Font::bold ) );
    g.drawText( m_title, r.removeFromTop( 18 ), juce::Justification::topLeft );
    r.removeFromTop( 4 );
    g.setFont( juce::FontOptions( 11.5f ) );
    juce::StringArray lines;
    lines.addLines( m_body );
    // The phrases under SOUNDS LIKE are the only text here nobody wrote for this preset: a model
    // picked them out of our vocabulary. They are set in italic so the eye can tell the measured
    // sentence from the chosen one without being told.
    bool quoted = false;
    for ( const auto &line : lines )
    {
        if ( r.getHeight() <= 0 ) break;
        const juce::String t = line.trim();
        if ( t.isEmpty() )
        {
            r.removeFromTop( 5 );
            continue;
        }
        // A heading is a line in capitals: draw it as one, with a rule under it.
        const bool heading = t == t.toUpperCase() && t.containsOnly( "ABCDEFGHIJKLMNOPQRSTUVWXYZ -" );
        if ( heading )
        {
            quoted = ( t == "SOUNDS LIKE" );
            r.removeFromTop( 5 );
            auto h = r.removeFromTop( 14 );
            g.setColour( Accent.withAlpha( 0.85f ) );
            g.setFont( juce::FontOptions( 10.0f, juce::Font::bold ) );
            g.drawText( t, h, juce::Justification::topLeft );
            g.setColour( Accent.withAlpha( 0.25f ) );
            g.drawLine( static_cast<float>( h.getX() ), static_cast<float>( h.getBottom() ),
                        static_cast<float>( h.getRight() ), static_cast<float>( h.getBottom() ), 1.0f );
            g.setFont( juce::FontOptions( 11.5f ) );
            continue;
        }
        // Prose wraps; the arrow rows do not need to.
        juce::GlyphArrangement  ga;
        const juce::FontOptions bodyFont =
            quoted ? juce::FontOptions( 11.5f ).withStyle( "Italic" ) : juce::FontOptions( 11.5f );
        ga.addFittedText( bodyFont, t, static_cast<float>( r.getX() ), static_cast<float>( r.getY() ),
                          static_cast<float>( r.getWidth() ), static_cast<float>( juce::jmin( r.getHeight(), 44 ) ),
                          juce::Justification::topLeft, 3, 1.0f );
        g.setColour( t.contains( "->" ) ? Dim : Text.withAlpha( 0.92f ) );
        ga.draw( g );
        const auto bb = ga.getBoundingBox( 0, -1, true );
        r.removeFromTop( juce::jmax( 14, static_cast<int>( bb.getHeight() ) + 2 ) );
    }
}

void NoctuaryEditor::BrowseView::updateInfo()
{
    const int want = m_selected >= 0 ? m_selected : m_proc.getCurrentProgram();
    if ( want == m_infoFor ) return;
    m_infoFor = want;
    if ( want < 0 || want >= numPresets() )
    {
        m_info2.m_title = {};
        m_info2.m_body = {};
    }
    else
    {
        m_info2.m_title = preset( want ).m_name;
        m_info2.m_body = juce::String( ambient::presetInfoText( want ) );
    }
    m_info2.repaint();
}

bool NoctuaryEditor::BrowseView::macroActive() const
{
    for ( const auto &mc : m_macros )
        if ( mc.m_slider.getMinValue() > 0.0 || mc.m_slider.getMaxValue() < 1.0 ) return true;
    return false;
}

/**
 * What Absynth's browser does when a tag is chosen: the cloud condenses. Ours cannot move the
 * points -- their places are what they mean -- so the view closes in on what is left instead,
 * which is the same gesture from the other side. Only on the filter's own action, never while
 * the mouse is panning or zooming, so the view never fights the hand.
 */
void NoctuaryEditor::BrowseView::fitToFilter()
{
    m_fitPending = false;
    if ( m_mode != 1 ) return;
    const int shown = juce::jmin( numPresetMeta(), numPresets() );
    if ( !macroActive() )
    {
        m_map.zoomTo( 0.5f, 0.5f, 1.0f );
        return;
    }
    float x0 = 1.0f, x1 = 0.0f, y0 = 1.0f, y1 = 0.0f;
    int   n = 0;
    for ( int i : m_filtered )
    {
        if ( i >= shown ) continue;
        const PresetMeta &m = presetMeta( i );
        x0 = juce::jmin( x0, m.m_x );
        x1 = juce::jmax( x1, m.m_x );
        y0 = juce::jmin( y0, m.m_y );
        y1 = juce::jmax( y1, m.m_y );
        ++n;
    }
    if ( n < 2 ) return;
    const float w = juce::jmax( 0.02f, x1 - x0 ), h = juce::jmax( 0.02f, y1 - y0 );
    const float z = juce::jlimit( 1.0f, 40.0f, 0.85f / juce::jmax( w, h ) );
    m_map.zoomTo( 0.5f * ( x0 + x1 ), 0.5f * ( y0 + y1 ), z );
}

void NoctuaryEditor::BrowseView::paintListBoxItem( int row, juce::Graphics &g, int w, int h, bool sel )
{
    if ( row < 0 || row >= static_cast<int>( m_filtered.size() ) ) return;
    const int         i = m_filtered[static_cast<size_t>( row )];
    const PresetMeta &m = presetMeta( i );
    if ( sel )
        g.fillAll( Accent.withAlpha( 0.18f ) );
    else if ( i == m_proc.getCurrentProgram() )
        g.fillAll( Accent.withAlpha( 0.08f ) );
    g.setColour( familyColour( m.m_family ) );
    g.fillEllipse( 6.0f, h * 0.5f - 4.0f, 8.0f, 8.0f );
    g.setColour( m_proc.isFavourite( i ) ? juce::Colour( 0xffe0c070 ) : SectionFill );
    g.setFont( juce::FontOptions( 13.0f ) );
    g.drawText( juce::String( juce::CharPointer_UTF8( "\xe2\x98\x85" ) ), 18, 0, 16, h, juce::Justification::centred );
    g.setColour( Text );
    g.setFont( juce::FontOptions( 13.0f ) );
    g.drawText( preset( i ).m_name, 38, 0, w - 320, h, juce::Justification::centredLeft );
    if ( m_mode == 0 )
    {
        g.setColour( Dim );
        g.setFont( juce::FontOptions( 11.0f ) );
        g.drawText( presetFamilyName( m.m_family ), w - 320, 0, 140, h, juce::Justification::centredLeft );
    }
    // small bars: bright, motion, width, density
    const float        vals[4] = { m.m_bright, m.m_motion, m.m_width, m.m_density };
    const juce::Colour cols[4] = { juce::Colour( 0xffe0c070 ), juce::Colour( 0xff8ec9a8 ), juce::Colour( 0xff7fb3d5 ),
                                   juce::Colour( 0xffd08a8a ) };
    for ( int k = 0; k < 4; ++k )
    {
        const int x = w - 176 + k * 42;
        g.setColour( SectionFill );
        g.fillRect( x, h / 2 - 3, 36, 6 );
        g.setColour( cols[k] );
        g.fillRect( x, h / 2 - 3, static_cast<int>( 36 * vals[k] ), 6 );
    }
}

void NoctuaryEditor::BrowseView::listBoxItemClicked( int row, const juce::MouseEvent &e )
{
    if ( row < 0 || row >= static_cast<int>( m_filtered.size() ) ) return;
    m_selected = m_filtered[static_cast<size_t>( row )];
    if ( e.x >= 16 && e.x < 36 )
    {   // the star
        m_proc.setFavourite( m_selected, !m_proc.isFavourite( m_selected ) );
        if ( m_favouritesFirst.getToggleState() )
            applyFilter();
        else
        {
            m_list.repaint();
            m_map.repaint();
        }
        return;
    }
    if ( e.getNumberOfClicks() >= 2 || e.mods.isLeftButtonDown() )
        m_proc.selectPreset( m_selected, m_morphOnSelect.getToggleState() );
    if ( m_mapActive.getToggleState() )
    {   // the cursor jumps to the preset's point
        const PresetMeta &m = presetMeta( m_selected );
        if ( auto *px = m_proc.m_apvts.getParameter( paramDesc( ParamIdE::MAP_X ).m_key ) )
            px->setValueNotifyingHost( m.m_x );
        if ( auto *py = m_proc.m_apvts.getParameter( paramDesc( ParamIdE::MAP_Y ).m_key ) )
            py->setValueNotifyingHost( m.m_y );
    }
    m_map.repaint();
}

void NoctuaryEditor::BrowseView::timerCallback()
{
    m_map.repaint();
    m_list.repaint();
    updateInfo();
    if ( m_routeEditOpen )
    {   // same pattern as the gesture editor: mirror while open, apply when gone
        if ( m_routeEditor != nullptr )
            m_routeEditText = m_routeEditor->getText();
        else
        {
            m_routeEditOpen = false;
            if ( m_routeEditText.trim().isEmpty() )
                m_proc.clearRoute();
            else if ( !m_proc.setRouteText( m_routeEditText ) )
                juce::AlertWindow::showMessageBoxAsync(
                    juce::MessageBoxIconType::WarningIcon, "Route",
                    "A point could not be parsed (unknown preset name or malformed); the previous route is kept." );
            m_routeBox.setSelectedId( 0, juce::dontSendNotification );
        }
    }
}

void NoctuaryEditor::BrowseView::showRouteEditor()
{
    auto *editor = new juce::TextEditor();
    editor->setMultiLine( true, true );
    editor->setReturnKeyStartsNewLine( true );
    editor->setFont( juce::FontOptions( juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain ) );
    editor->setText( m_proc.routeText().replace( ";", ";\n" ), false );
    editor->setSize( 560, 320 );
    auto *content = new juce::Component();
    content->setSize( 560, 360 );
    content->addAndMakeVisible( editor );
    editor->setTopLeftPosition( 0, 0 );
    auto *hint = new juce::Label(
        juce::String(), "one point per line:  Preset Name|travel s|hold s[|radius]   or   x,y|travel|hold[|radius]" );
    hint->setFont( juce::FontOptions( 11.0f ) );
    hint->setColour( juce::Label::textColourId, Dim );
    hint->setBounds( 0, 324, 560, 30 );
    content->addAndMakeVisible( hint );
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned( content );
    o.dialogTitle = "Route over the map";
    o.componentToCentreAround = this;
    o.dialogBackgroundColour = GroupFill;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
    m_routeEditor = editor;
    m_routeEditText = editor->getText();
    m_routeEditOpen = true;
}

void NoctuaryEditor::BrowseView::paint( juce::Graphics &g )
{
    g.fillAll( Bg );
    g.setColour( GroupFill );
    g.fillRoundedRectangle( getLocalBounds().reduced( 8 ).toFloat(), 8.0f );
    g.setColour( Dim );
    g.setFont( juce::FontOptions( 11.0f ) );
    g.drawText( "bright  motion  width  density", m_list.getRight() - 176, m_list.getY() - 16, 176, 14,
                juce::Justification::centredLeft );
    if ( m_mode == 0 )
    {
        g.drawText( "preset", m_list.getX() + 38, m_list.getY() - 16, 200, 14, juce::Justification::centredLeft );
        g.drawText( "family", m_list.getRight() - 320, m_list.getY() - 16, 140, 14, juce::Justification::centredLeft );
        for ( auto &c : m_columns )
        {
            g.setColour( Text );
            g.setFont( juce::FontOptions( 12.0f, juce::Font::bold ) );
            g.drawText( c.m_title.toUpperCase(), c.m_box.getX(), c.m_box.getY() - 18, c.m_box.getWidth(), 16,
                        juce::Justification::centredLeft );
        }
    }
}

void NoctuaryEditor::BrowseView::resized()
{
    auto area = getLocalBounds().reduced( 16 );
    auto top = area.removeFromTop( 26 );
    m_modeClassic.setBounds( top.removeFromLeft( 80 ) );
    m_modeMap.setBounds( top.removeFromLeft( 60 ) );
    top.removeFromLeft( 12 );
    m_search.setBounds( top.removeFromLeft( 180 ) );
    top.removeFromLeft( 6 );
    if ( m_mode == 1 )
    {
        m_family.setBounds( top.removeFromLeft( 150 ) );
        top.removeFromLeft( 6 );
    }
    m_sort.setBounds( top.removeFromLeft( 150 ) );
    top.removeFromLeft( 12 );
    m_onlyFavourites.setBounds( top.removeFromLeft( 130 ) );
    m_favouritesFirst.setBounds( top.removeFromLeft( 130 ) );
    m_similar.setBounds( top.removeFromLeft( 150 ) );
    m_hideDull.setBounds( top.removeFromLeft( 160 ) );
    area.removeFromTop( 8 );
    auto buttons = area.removeFromBottom( 26 );
    m_load.setBounds( buttons.removeFromLeft( 80 ) );
    buttons.removeFromLeft( 6 );
    m_toA.setBounds( buttons.removeFromLeft( 60 ) );
    buttons.removeFromLeft( 6 );
    m_toB.setBounds( buttons.removeFromLeft( 60 ) );
    buttons.removeFromLeft( 6 );
    m_star.setBounds( buttons.removeFromLeft( 90 ) );
    buttons.removeFromLeft( 12 );
    m_morphOnSelect.setBounds( buttons.removeFromLeft( 130 ) );
    m_morphSeconds.setBounds( buttons.removeFromLeft( juce::jmin( 190, juce::jmax( 110, buttons.getWidth() / 3 ) ) ) );
    buttons.removeFromLeft( 12 );
    if ( m_mode == 1 )
    {
        auto mapControls = buttons.removeFromRight( juce::jmax( 300, buttons.getWidth() * 3 / 5 ) );
        m_mapActive.setBounds( mapControls.removeFromLeft( 220 ) );
        mapControls.removeFromLeft( 12 );
        m_radius.setBounds( mapControls );
    }
    m_info.setBounds( buttons );
    area.removeFromBottom( 6 );
    if ( m_mode == 1 )
    {   // the macro sliders: two pairs across the bottom of the map side
        auto strip = area.removeFromBottom( 24 );
        strip.removeFromLeft( juce::jmax( 420, ( getLocalBounds().reduced( 16 ).getWidth() ) * 2 / 5 ) + 12 );
        m_colourByGroup.setBounds( strip.removeFromRight( 90 ) );
        const int each = juce::jmax( 90, strip.getWidth() / 4 );
        for ( int i = 0; i < 4; ++i )
        {
            auto cell = strip.removeFromLeft( each );
            m_macros[i].m_label.setBounds( cell.removeFromLeft( juce::jmin( 86, each / 2 ) ) );
            m_macros[i].m_slider.setBounds( cell );
        }
        area.removeFromBottom( 4 );
    }
    if ( m_mode == 1 )
    {   // route strip above the bottom row, on the map side
        auto strip = area.removeFromBottom( 26 );
        strip.removeFromLeft( juce::jmax( 420, ( getLocalBounds().reduced( 16 ).getWidth() ) * 2 / 5 ) + 12 );
        m_routeBox.setBounds( strip.removeFromLeft( 170 ) );
        strip.removeFromLeft( 6 );
        m_routePlay.setBounds( strip.removeFromLeft( 96 ) );
        strip.removeFromLeft( 2 );
        m_routeLoop.setBounds( strip.removeFromLeft( 56 ) );
        strip.removeFromLeft( 6 );
        m_routeSpeed.setBounds( strip.removeFromLeft( juce::jmax( 120, strip.getWidth() - 250 ) ) );
        strip.removeFromLeft( 6 );
        m_routeAdd.setBounds( strip.removeFromLeft( 70 ) );
        strip.removeFromLeft( 4 );
        m_routeClear.setBounds( strip.removeFromLeft( 56 ) );
        strip.removeFromLeft( 4 );
        m_routeEdit.setBounds( strip.removeFromLeft( 70 ) );
        area.removeFromBottom( 6 );
    }

    if ( m_mode == 0 )
    {
        // Omnisphere-style: four columns on top, the results below, and the preset's own card on
        // the right of them -- Zebra puts it exactly there, and it is where the eye goes next.
        auto      cols = area.removeFromTop( juce::jmax( 160, area.getHeight() * 2 / 5 ) );
        const int colW = cols.getWidth() / 4;
        for ( int c = 0; c < 4; ++c )
        {
            auto r = cols.removeFromLeft( colW ).reduced( 0, 0 );
            r.removeFromTop( 18 );   // title painted above
            m_columns[c].m_box.setBounds( r.reduced( c == 0 ? 0 : 4, 0 ).withTrimmedRight( 4 ) );
        }
        area.removeFromTop( 24 );   // column header of the result list
        {                           // the list, and the preset's card beside it
            auto card = area.removeFromRight( juce::jlimit( 240, 420, area.getWidth() / 3 ) );
            m_info2.setBounds( card.withTrimmedLeft( 12 ) );
        }
        m_list.setBounds( area );
    }
    else
    {
        auto left = area.removeFromLeft( juce::jmax( 420, area.getWidth() * 2 / 5 ) );
        area.removeFromLeft( 12 );
        const int perRow = 5, tagH = 22;
        const int rows = ( NumPresetTags + perRow - 1 ) / perRow;
        auto      tags = left.removeFromTop( rows * tagH );
        // The card paints until it runs out of room, so on the map side it gets as much as half
        // the column: the description, the phrases and the filing all have to fit above the fold.
        m_info2.setBounds( left.removeFromBottom( juce::jmin( 240, left.getHeight() / 2 ) ).withTrimmedTop( 8 ) );
        for ( int t = 0; t < NumPresetTags; ++t )
        {
            const int r = t / perRow, c = t % perRow;
            m_tagButtons[static_cast<size_t>( t )]->setBounds( tags.getX() + c * ( tags.getWidth() / perRow ),
                                                               tags.getY() + r * tagH, tags.getWidth() / perRow, tagH );
        }
        left.removeFromTop( 18 );
        m_list.setBounds( left );
        m_map.setBounds( area );
    }
}

/** The window onto the plane: at zoom 1 and centre (0.5, 0.5) this is exactly the old fixed view. */
juce::Point<float> NoctuaryEditor::BrowseView::MapView::toScreen( float x, float y ) const
{
    const auto r = getLocalBounds().toFloat().reduced( 18.0f );
    return { r.getCentreX() + ( x - m_centre.x ) * m_zoom * r.getWidth(),
             r.getCentreY() - ( y - m_centre.y ) * m_zoom * r.getHeight() };
}

juce::Point<float> NoctuaryEditor::BrowseView::MapView::toMapRaw( juce::Point<float> s ) const
{
    const auto r = getLocalBounds().toFloat().reduced( 18.0f );
    return { m_centre.x + ( s.x - r.getCentreX() ) / juce::jmax( 1.0f, m_zoom * r.getWidth() ),
             m_centre.y + ( r.getCentreY() - s.y ) / juce::jmax( 1.0f, m_zoom * r.getHeight() ) };
}

juce::Point<float> NoctuaryEditor::BrowseView::MapView::toMap( juce::Point<float> s ) const
{
    const auto m = toMapRaw( s );
    return { juce::jlimit( 0.0f, 1.0f, m.x ), juce::jlimit( 0.0f, 1.0f, m.y ) };
}

void NoctuaryEditor::BrowseView::MapView::zoomTo( float x, float y, float z )
{
    m_zoom = juce::jlimit( 1.0f, 40.0f, z );
    m_centre = { x, y };
    if ( m_zoom <= 1.0f ) m_centre = { 0.5f, 0.5f };
    repaint();
}

void NoctuaryEditor::BrowseView::MapView::mouseWheelMove( const juce::MouseEvent &e, const juce::MouseWheelDetails &w )
{
    // Zoom about the mouse: the point of the plane under the cursor stays under the cursor, so
    // the eye can follow a cluster in while it opens up.
    const auto  pivot = toMapRaw( e.position );
    const float factor = std::pow( 1.25f, w.deltaY * 4.0f );
    const float newZoom = juce::jlimit( 1.0f, 40.0f, m_zoom * factor );
    if ( newZoom == m_zoom ) return;
    m_zoom = newZoom;
    const auto r = getLocalBounds().toFloat().reduced( 18.0f );
    m_centre = { pivot.x - ( e.position.x - r.getCentreX() ) / ( m_zoom * r.getWidth() ),
                 pivot.y - ( r.getCentreY() - e.position.y ) / ( m_zoom * r.getHeight() ) };
    if ( m_zoom <= 1.0f ) m_centre = { 0.5f, 0.5f };
    repaint();
}

void NoctuaryEditor::BrowseView::MapView::mouseDoubleClick( const juce::MouseEvent & )
{
    zoomTo( 0.5f, 0.5f, 1.0f );
}

int NoctuaryEditor::BrowseView::MapView::nearestPreset( juce::Point<float> p, float maxDist ) const
{
    int   best = -1;
    float bd = maxDist * maxDist;
    for ( int i = 0; i < std::min( numPresetMeta(), numPresets() ); ++i )
    {
        const auto  s = toScreen( presetMeta( i ).m_x, presetMeta( i ).m_y );
        const float d = ( s.x - p.x ) * ( s.x - p.x ) + ( s.y - p.y ) * ( s.y - p.y );
        if ( d < bd )
        {
            bd = d;
            best = i;
        }
    }
    return best;
}

void NoctuaryEditor::BrowseView::MapView::paint( juce::Graphics &g )
{
    g.setColour( Bg.brighter( 0.03f ) );
    g.fillRoundedRectangle( getLocalBounds().toFloat(), 6.0f );
    g.setColour( SectionFill );
    g.drawRoundedRectangle( getLocalBounds().toFloat().reduced( 1.0f ), 6.0f, 1.0f );
    if ( numPresetMeta() == 0 )
    {
        g.setColour( Dim );
        g.setFont( juce::FontOptions( 13.0f ) );
        g.drawText( "The map is not measured yet: run Tools/preset_map.py and rebuild.", getLocalBounds(),
                    juce::Justification::centred );
        return;
    }
    const bool  blend = m_owner.m_mapActive.getToggleState();
    const float x = m_owner.m_proc.engine().getParam( ParamIdE::MAP_X ),
                y = m_owner.m_proc.engine().getParam( ParamIdE::MAP_Y );
    const float rad = m_owner.m_proc.engine().getParam( ParamIdE::MAP_RADIUS );
    const auto  cursor = toScreen( x, y );
    const auto  r = getLocalBounds().toFloat().reduced( 18.0f );
    if ( blend )
    {   // the blend radius as a soft disc
        const float rr = rad * r.getWidth() * m_zoom;
        g.setColour( Accent.withAlpha( 0.10f ) );
        g.fillEllipse( cursor.x - rr, cursor.y - rr, 2 * rr, 2 * rr );
        g.setColour( Accent.withAlpha( 0.25f ) );
        g.drawEllipse( cursor.x - rr, cursor.y - rr, 2 * rr, 2 * rr, 1.0f );
    }
    // dimmed points first, then the filtered ones, then the current program
    const bool groups = m_owner.m_colourByGroup.getToggleState() && numPresetClusters() > 0;
    const int  current = m_owner.m_proc.getCurrentProgram();
    // With a preset library loaded there can be thousands of points: shrink the dots so the
    // plane stays readable, and draw the dimmed ones as squares, which is much cheaper to fill.
    const int shown = std::min( numPresetMeta(), numPresets() );
    // The dots grow a little with the zoom -- not with it, or a cluster opened up would be
    // blobs -- and everything outside the window is skipped, which at forty times is most of it.
    const float dotScale =
        juce::jlimit( 0.34f, 1.0f, std::sqrt( 200.0f / juce::jmax( 1, shown ) ) ) * std::pow( m_zoom, 0.35f );
    const bool many = shown > 800;
    const auto screen = getLocalBounds().toFloat().expanded( 12.0f );
    // The cloud is the same picture until the window, the zoom, the pan, the filter or the
    // colouring changes, so it is drawn into an image and blitted. Live at fifteen frames a
    // second it was eight and a half thousand filled ellipses per frame.
    if ( m_cloud.isNull() || m_cloudW != getWidth() || m_cloudH != getHeight() || m_cloudZoom != m_zoom ||
         m_cloudCentre != m_centre || m_cloudFilter != m_owner.m_filterGen || m_cloudGroups != groups ||
         m_cloudCount != shown )
    {
        m_cloudW = juce::jmax( 1, getWidth() );
        m_cloudH = juce::jmax( 1, getHeight() );
        m_cloudZoom = m_zoom;
        m_cloudCentre = m_centre;
        m_cloudFilter = m_owner.m_filterGen;
        m_cloudGroups = groups;
        m_cloudCount = shown;
        m_cloud = juce::Image( juce::Image::ARGB, m_cloudW, m_cloudH, true );
        juce::Graphics    cg( m_cloud );
        std::vector<bool> inFilter( static_cast<size_t>( numPresets() ), false );
        for ( int i : m_owner.m_filtered ) inFilter[static_cast<size_t>( i )] = true;
        for ( int pass = 0; pass < 2; ++pass )
        {
            for ( int i = 0; i < shown; ++i )
            {
                if ( inFilter[static_cast<size_t>( i )] != ( pass == 1 ) ) continue;
                const PresetMeta &m = presetMeta( i );
                const auto        s = toScreen( m.m_x, m.m_y );
                if ( !screen.contains( s ) ) continue;
                const float  size = juce::jmax( 2.0f, ( 6.0f + 6.0f * m.m_density ) * dotScale );
                juce::Colour c = groups ? clusterColour( presetClusterOf( m ) ) : familyColour( m.m_family );
                if ( pass == 0 ) c = c.withAlpha( 0.18f );
                cg.setColour( c );
                if ( pass == 0 && many )
                    cg.fillRect( s.x - size / 2, s.y - size / 2, size, size );
                else
                    cg.fillEllipse( s.x - size / 2, s.y - size / 2, size, size );
            }
        }
    }
    g.drawImageAt( m_cloud, 0, 0 );
    // The favourites, ringed in the star's gold, live over the cloud: a star is set with a click
    // and the cloud must not be drawn again for that. Only those the filter lets through, so the
    // map and the list agree.
    if ( m_owner.m_proc.favouriteCount() > 0 )
    {
        g.setColour( juce::Colour( 0xffe0c070 ).withAlpha( 0.9f ) );
        for ( int i : m_owner.m_filtered )
        {
            if ( i >= shown || !m_owner.m_proc.isFavourite( i ) ) continue;
            const PresetMeta &m = presetMeta( i );
            const auto        s = toScreen( m.m_x, m.m_y );
            if ( !screen.contains( s ) ) continue;
            const float size = juce::jmax( 2.0f, ( 6.0f + 6.0f * m.m_density ) * dotScale );
            g.drawEllipse( s.x - size / 2 - 2.0f, s.y - size / 2 - 2.0f, size + 4.0f, size + 4.0f, 1.2f );
        }
    }
    // The chosen preset's own ring is live: picking one in the list must not redraw the cloud.
    if ( m_owner.m_selected >= 0 && m_owner.m_selected < shown && m_owner.m_selected != current )
    {
        const PresetMeta &m = presetMeta( m_owner.m_selected );
        const auto        s = toScreen( m.m_x, m.m_y );
        const float       size = juce::jmax( 2.0f, ( 6.0f + 6.0f * m.m_density ) * dotScale );
        g.setColour( Text );
        g.drawEllipse( s.x - size / 2 - 3, s.y - size / 2 - 3, size + 6, size + 6, 1.5f );
    }
    // What is playing, marked so it can be found at a glance: everything else on this plane is a
    // dot of a few pixels, and "where am I" is the first question a map has to answer. A ring, a
    // crosshair, and the name -- at every zoom, whether or not it survived the filter. While a
    // preset change is travelling (the browser's "morph into it"), a line runs from where the
    // sound started to where it is going, filled in as far as it has come.
    // The ring is on the preset the instrument has been given; while a transition is in flight the
    // sound is still on its way there from the one that is leaving, so the line is drawn from THAT
    // one to here. It used to be drawn from the program to the morph target, which were the same
    // preset from the moment the change was made once a preset change became a crossfade -- a line
    // of no length, and its label printed over the ring's own name.
    const int  from = m_owner.m_proc.morphingFrom();
    const bool travelling = from >= 0 && from < shown && from != current;
    if ( current >= 0 && current < shown )
    {
        const PresetMeta &m = presetMeta( current );
        const auto        s = toScreen( m.m_x, m.m_y );
        if ( travelling )
        {
            const PresetMeta &mf = presetMeta( from );
            const auto        d = toScreen( mf.m_x, mf.m_y );
            const float       t = juce::jlimit( 0.0f, 1.0f, m_owner.m_proc.morphProgress() );
            g.setColour( Accent.withAlpha( 0.35f ) );
            g.drawLine( juce::Line<float>( d, s ), 1.4f );     // the whole way
            const juce::Point<float> at = d + ( s - d ) * t;   // how far it has come
            g.setColour( Accent );
            g.drawLine( juce::Line<float>( d, at ), 2.2f );
            g.fillEllipse( at.x - 4.0f, at.y - 4.0f, 8.0f, 8.0f );
            // Both labels stay at the departure, where nothing else writes: the moving point
            // carries the eye, and a number that travels with it would end up on the ring's name.
            g.setColour( Text.withAlpha( 0.75f ) );
            g.setFont( juce::FontOptions( 10.5f ) );
            g.drawText( preset( from ).m_name, static_cast<int>( d.x ) + 10, static_cast<int>( d.y ) - 7, 240, 14,
                        juce::Justification::centredLeft );
            g.setColour( Accent.withAlpha( 0.8f ) );
            g.setFont( juce::FontOptions( 9.5f ) );
            g.drawText( juce::String( juce::roundToInt( t * 100.0f ) ) + " % across", static_cast<int>( d.x ) + 10,
                        static_cast<int>( d.y ) + 7, 160, 12, juce::Justification::centredLeft );
        }
        const float r = 13.0f;
        g.setColour( Accent.withAlpha( 0.16f ) );
        g.fillEllipse( s.x - r, s.y - r, 2 * r, 2 * r );
        g.setColour( Accent );
        g.drawEllipse( s.x - r, s.y - r, 2 * r, 2 * r, 2.0f );
        g.setColour( Accent.withAlpha( 0.55f ) );
        g.drawEllipse( s.x - r - 4.0f, s.y - r - 4.0f, 2 * r + 8.0f, 2 * r + 8.0f, 1.0f );
        for ( int k = 0; k < 4; ++k )
        {   // a crosshair, so it is found even inside a dense ball
            const float a = k * 1.57079633f;
            const float c1 = std::cos( a ), s1 = std::sin( a );
            g.drawLine( s.x + c1 * ( r + 3.0f ), s.y + s1 * ( r + 3.0f ), s.x + c1 * ( r + 9.0f ),
                        s.y + s1 * ( r + 9.0f ), 1.6f );
        }
        g.setColour( Text );
        g.setFont( juce::FontOptions( 11.0f, juce::Font::bold ) );
        g.drawText( preset( current ).m_name, static_cast<int>( s.x ) + 18, static_cast<int>( s.y ) - 8, 240, 15,
                    juce::Justification::centredLeft );
        g.setColour( Accent.withAlpha( 0.8f ) );
        g.setFont( juce::FontOptions( 9.5f ) );
        // "arriving" while the crossfade runs: the ring is already on the new preset, but what is
        // mostly being heard at that moment is still the old one.
        g.drawText( travelling ? "arriving" : "playing", static_cast<int>( s.x ) + 18, static_cast<int>( s.y ) + 6, 120,
                    12, juce::Justification::centredLeft );
    }
    // Names, once there is room for them. Zoomed in past four, every preset in the filter whose
    // label would not sit on another's gets its name; the check is a coarse grid of the label's
    // own size, so a cluster shows the few names that fit and not a smear of all of them.
    if ( m_zoom >= 4.0f )
    {
        g.setFont( juce::FontOptions( 10.5f ) );
        std::set<std::pair<int, int>> taken;
        const int                     cw = 130, ch = 13;   // a label's width, so two names never share a line
        // The two labels that were already written -- what is playing, and what is being left --
        // take their cells first, so no other preset's name is printed across them. The grid only
        // ever kept THESE labels apart from each other; the ring's own name was invisible to it.
        auto reserve = [&]( int i )
        {
            if ( i < 0 || i >= shown ) return;
            const auto p = toScreen( presetMeta( i ).m_x, presetMeta( i ).m_y );
            for ( int dx = 0; dx <= 1; ++dx )
                for ( int dy = -1; dy <= 1; ++dy )
                    taken.insert( { static_cast<int>( p.x ) / cw + dx, static_cast<int>( p.y ) / ch + dy } );
        };
        reserve( current );
        if ( travelling ) reserve( from );
        for ( int i : m_owner.m_filtered )
        {
            if ( i >= shown ) continue;
            // Not the ones that have a label of their own already: what is playing carries its
            // name in bold beside the ring, and a preset being left carries it at the line's
            // start. Drawing them here as well put the same word twice over itself, eleven
            // pixels apart -- one smeared line of glyphs that read as neither name.
            if ( i == current || ( travelling && i == from ) ) continue;
            const PresetMeta &m = presetMeta( i );
            const auto        s = toScreen( m.m_x, m.m_y );
            if ( !screen.contains( s ) ) continue;
            const std::pair<int, int> cell{ static_cast<int>( s.x ) / cw, static_cast<int>( s.y ) / ch };
            if ( !taken.insert( cell ).second ) continue;
            g.setColour( i == m_owner.m_selected ? Text : Dim );
            g.drawText( preset( i ).m_name, static_cast<int>( s.x ) + 7, static_cast<int>( s.y ) - 7, cw + 40, ch,
                        juce::Justification::centredLeft );
        }
    }
    if ( m_zoom > 1.0f )
    {   // say where we are, and how to get back
        g.setColour( Dim );
        g.setFont( juce::FontOptions( 10.5f ) );
        g.drawText( juce::String( "zoom x" ) + juce::String( m_zoom, 1 ) + "   drag to pan, double-click to reset",
                    getLocalBounds().reduced( 10 ).removeFromTop( 14 ), juce::Justification::topRight );
    }
    // the route: numbered points joined by a line, the segment being walked highlighted
    {
        const Route &rt = m_owner.m_proc.engine().route();
        if ( rt.count() > 0 )
        {
            const bool running = rt.running();
            const int  seg = rt.segment();
            for ( int i = 0; i < rt.count(); ++i )
            {
                const Waypoint &w = rt.point( i );
                const auto      s = toScreen( w.m_x, w.m_y );
                if ( i > 0 )
                {
                    const Waypoint &pw = rt.point( i - 1 );
                    const auto      ps = toScreen( pw.m_x, pw.m_y );
                    g.setColour( juce::Colour( 0xffe0c070 ).withAlpha( running && i == seg ? 0.9f : 0.35f ) );
                    g.drawLine( juce::Line<float>( ps, s ), running && i == seg ? 2.0f : 1.0f );
                }
                g.setColour( juce::Colour( 0xffe0c070 ).withAlpha( 0.8f ) );
                g.drawEllipse( s.x - 9, s.y - 9, 18, 18, 1.2f );
                g.setFont( juce::FontOptions( 10.0f, juce::Font::bold ) );
                g.drawText( juce::String( i + 1 ), static_cast<int>( s.x ) - 9, static_cast<int>( s.y ) - 9, 18, 18,
                            juce::Justification::centred );
            }
            if ( m_owner.m_proc.engine().getParam( ParamIdE::ROUTE_LOOP ) >= 0.5f && rt.count() > 1 )
            {
                g.setColour( juce::Colour( 0xffe0c070 ).withAlpha( 0.2f ) );
                g.drawLine(
                    juce::Line<float>( toScreen( rt.point( rt.count() - 1 ).m_x, rt.point( rt.count() - 1 ).m_y ),
                                       toScreen( rt.point( 0 ).m_x, rt.point( 0 ).m_y ) ),
                    1.0f );
            }
        }
    }
    // neighbour lines while blending
    if ( blend )
    {
        const PresetMap::Blend b = PresetMap::neighbours( x, y, rad );
        for ( int k = 0; k < b.m_count; ++k )
        {
            if ( b.m_weight[k] < 0.02f ) continue;
            const PresetMeta &m = presetMeta( b.m_index[k] );
            const auto        s = toScreen( m.m_x, m.m_y );
            g.setColour( Accent.withAlpha( 0.2f + 0.7f * b.m_weight[k] ) );
            g.drawLine( juce::Line<float>( cursor, s ), 1.0f + 2.0f * b.m_weight[k] );
            g.setFont( juce::FontOptions( 11.0f ) );
            g.drawText( juce::String( preset( b.m_index[k] ).m_name ) + " " +
                            juce::String( static_cast<int>( 100 * b.m_weight[k] ) ) + "%",
                        static_cast<int>( s.x ) + 8, static_cast<int>( s.y ) - 7, 220, 14,
                        juce::Justification::centredLeft );
        }
        g.setColour( Text );
        g.drawLine( cursor.x - 8, cursor.y, cursor.x + 8, cursor.y, 1.2f );
        g.drawLine( cursor.x, cursor.y - 8, cursor.x, cursor.y + 8, 1.2f );
    }
    if ( m_hover >= 0 && m_hover < numPresets() )
    {
        const PresetMeta &m = presetMeta( m_hover );
        const auto        s = toScreen( m.m_x, m.m_y );
        juce::String      tags;
        for ( int t = 0; t < NumPresetTags; ++t )
            if ( m.m_tags & ( 1u << t ) ) tags += juce::String( presetTagName( t ) ) + " ";
        g.setColour( Text );
        g.setFont( juce::FontOptions( 12.0f, juce::Font::bold ) );
        g.drawText( preset( m_hover ).m_name, static_cast<int>( s.x ) + 10, static_cast<int>( s.y ) - 20, 260, 14,
                    juce::Justification::centredLeft );
        g.setColour( Dim );
        g.setFont( juce::FontOptions( 10.5f ) );
        g.drawText( juce::String( presetFamilyName( m.m_family ) ) + "  -  " + tags.trim(),
                    static_cast<int>( s.x ) + 10, static_cast<int>( s.y ) - 6, 360, 14,
                    juce::Justification::centredLeft );
    }
    // legend: the groups if the cloud is coloured by them, otherwise where the presets came from
    g.setFont( juce::FontOptions( 10.5f ) );
    int       lx = 10, ly = getHeight() - 16;
    const int legendCount = groups ? numPresetClusters() : numPresetFamilies();
    for ( int f = 0; f < legendCount; ++f )
    {
        const char *nm = groups ? presetClusterName( f ) : presetFamilyName( f );
        g.setColour( groups ? clusterColour( f ) : familyColour( f ) );
        g.fillEllipse( static_cast<float>( lx ), static_cast<float>( ly ) + 3.0f, 7.0f, 7.0f );
        g.setColour( Dim );
        g.drawText( nm, lx + 10, ly, 120, 13, juce::Justification::centredLeft );
        lx += 14 + 7 * juce::jmin( 16, static_cast<int>( std::strlen( nm ) ) );
        if ( lx > getWidth() - 120 ) break;
    }
}

void NoctuaryEditor::BrowseView::MapView::mouseMove( const juce::MouseEvent &e )
{
    const int h = nearestPreset( e.position, 10.0f );
    if ( h != m_hover )
    {
        m_hover = h;
        repaint();
    }
}

void NoctuaryEditor::BrowseView::MapView::mouseDown( const juce::MouseEvent &e )
{
    m_dragging = false;
    m_panning = false;
    // The right button, the middle button, or a drag on empty plane while the blend cursor is
    // off: all of those pan. With the blend on, the left button is the cursor, as it always was.
    if ( e.mods.isRightButtonDown() || e.mods.isMiddleButtonDown() )
    {
        m_panning = true;
        m_panFrom = e.position;
        m_centreFrom = m_centre;
        return;
    }
    const int h = nearestPreset( e.position, 10.0f );
    if ( h >= 0 && !m_owner.m_mapActive.getToggleState() )
    {   // plain click on a point loads it
        m_owner.m_selected = h;
        m_owner.m_proc.selectPreset( h, m_owner.m_morphOnSelect.getToggleState() );
        m_owner.m_list.repaint();
        repaint();
        return;
    }
    if ( !m_owner.m_mapActive.getToggleState() )
    {
        m_panning = true;
        m_panFrom = e.position;
        m_centreFrom = m_centre;
        return;
    }
    mouseDrag( e );
}

void NoctuaryEditor::BrowseView::MapView::mouseDrag( const juce::MouseEvent &e )
{
    if ( m_panning )
    {
        const auto r = getLocalBounds().toFloat().reduced( 18.0f );
        m_centre = { m_centreFrom.x - ( e.position.x - m_panFrom.x ) / ( m_zoom * juce::jmax( 1.0f, r.getWidth() ) ),
                     m_centreFrom.y + ( e.position.y - m_panFrom.y ) / ( m_zoom * juce::jmax( 1.0f, r.getHeight() ) ) };
        // Keep the plane on screen: the centre may not leave the unit square by more than the
        // half-window, so the last row of presets stays reachable and nothing is lost off the edge.
        const float half = 0.5f / m_zoom;
        m_centre = { juce::jlimit( half, 1.0f - half, m_centre.x ), juce::jlimit( half, 1.0f - half, m_centre.y ) };
        repaint();
        return;
    }
    m_dragging = true;
    if ( !m_owner.m_mapActive.getToggleState() ) return;
    const auto m = toMap( e.position );
    if ( auto *px = m_owner.m_proc.m_apvts.getParameter( paramDesc( ParamIdE::MAP_X ).m_key ) )
        px->setValueNotifyingHost( m.x );
    if ( auto *py = m_owner.m_proc.m_apvts.getParameter( paramDesc( ParamIdE::MAP_Y ).m_key ) )
        py->setValueNotifyingHost( m.y );
    repaint();
}

void NoctuaryEditor::BrowseView::MapView::mouseUp( const juce::MouseEvent & )
{
    m_dragging = false;
    m_panning = false;
}

NoctuaryEditor::PerformView::PerformView( NoctuaryProcessor &p ) : m_proc( p )
{
    for ( int m = 0; m < 8; ++m )
    {
        const ParamDesc &d = paramDesc( static_cast<ParamIdE>( static_cast<int>( ParamIdE::MACRO_A ) + m ) );
        auto             s =
            std::make_unique<juce::Slider>( juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow );
        s->setTextBoxStyle( juce::Slider::TextBoxBelow, false, 70, 20 );
        addAndMakeVisible( *s );
        auto l = std::make_unique<juce::Label>( juce::String(), d.m_name );
        l->setJustificationType( juce::Justification::centred );
        l->setFont( juce::FontOptions( 20.0f, juce::Font::bold ) );
        l->setColour( juce::Label::textColourId, Text );
        addAndMakeVisible( *l );
        m_attachments.push_back(
            std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>( m_proc.m_apvts, d.m_key, *s ) );
        m_knobs.push_back( std::move( s ) );
        m_labels.push_back( std::move( l ) );
    }
    m_morph.setSliderStyle( juce::Slider::LinearHorizontal );
    m_morph.setTextBoxStyle( juce::Slider::TextBoxRight, false, 60, 22 );
    addAndMakeVisible( m_morph );
    m_morphAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        m_proc.m_apvts, paramDesc( ParamIdE::MORPH_POS ).m_key, m_morph );
    m_morphActive.setButtonText( "Morph" );
    addAndMakeVisible( m_morphActive );
    m_morphActiveAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        m_proc.m_apvts, paramDesc( ParamIdE::MORPH_ACTIVE ).m_key, m_morphActive );
    for ( auto *l : { &m_morphLabel, &m_aLabel, &m_bLabel } )
    {
        l->setFont( juce::FontOptions( 16.0f ) );
        l->setColour( juce::Label::textColourId, Dim );
        addAndMakeVisible( *l );
    }
    m_morphLabel.setText( "A  <  position  >  B", juce::dontSendNotification );
    m_morphLabel.setJustificationType( juce::Justification::centred );
    m_aLabel.setJustificationType( juce::Justification::centredRight );
    m_bLabel.setJustificationType( juce::Justification::centredLeft );

    // Set timeline
    m_setRec.setColour( juce::TextButton::buttonOnColourId, juce::Colour( 0xffb03030 ) );
    m_setRec.onClick = [this]
    {
        if ( m_proc.isRecordingSet() )
        {
            m_chooser = std::make_unique<juce::FileChooser>( "Save the set", juce::File(), "*.ambientset" );
            m_chooser->launchAsync( juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                                        juce::FileBrowserComponent::warnAboutOverwriting,
                                    [this]( const juce::FileChooser &fc )
                                    {
                                        auto file = fc.getResult();
                                        if ( file != juce::File() && !file.hasFileExtension( "ambientset" ) )
                                            file = file.withFileExtension( "ambientset" );
                                        if ( !m_proc.stopSetRecording( file ) && file != juce::File() )
                                            juce::AlertWindow::showMessageBoxAsync(
                                                juce::MessageBoxIconType::WarningIcon, "Set",
                                                "Could not write the set file." );
                                        updateSetInfo();
                                    } );
        }
        else
        {
            m_proc.startSetRecording();
            updateSetInfo();
        }
    };
    m_setPlay.onClick = [this]
    {
        m_chooser = std::make_unique<juce::FileChooser>( "Play a set", juce::File(), "*.ambientset" );
        m_chooser->launchAsync( juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                [this]( const juce::FileChooser &fc )
                                {
                                    const auto file = fc.getResult();
                                    if ( !file.existsAsFile() ) return;
                                    if ( !m_proc.playSetFile( file ) )
                                        juce::AlertWindow::showMessageBoxAsync( juce::MessageBoxIconType::WarningIcon,
                                                                                "Set",
                                                                                "This is not a readable set file." );
                                    updateSetInfo();
                                } );
    };
    m_setStop.onClick = [this]
    {
        m_proc.stopSetPlayback();
        if ( m_proc.isRecordingSet() ) m_proc.stopSetRecording( juce::File() );
        updateSetInfo();
    };
    for ( auto *b : { &m_setRec, &m_setPlay, &m_setStop } ) addAndMakeVisible( *b );
    m_setInfo.setFont( juce::FontOptions( 12.0f ) );
    m_setInfo.setColour( juce::Label::textColourId, Dim );
    addAndMakeVisible( m_setInfo );
    updateSetInfo();
}

void NoctuaryEditor::PerformView::updateSetInfo()
{
    m_setRec.setToggleState( m_proc.isRecordingSet(), juce::dontSendNotification );
    const int    t = static_cast<int>( m_proc.setTime() );
    juce::String text;
    if ( m_proc.isRecordingSet() )
        text = juce::String::formatted( "recording set  %d:%02d", t / 60, t % 60 );
    else if ( m_proc.isPlayingSet() )
        text = juce::String::formatted( "playing set  %d:%02d", t / 60, t % 60 );
    else
        text =
            "a set = every knob, gesture and note with its time; Record, play a while, stop to save; render it again with ambient_render --set-file";
    m_setInfo.setText( text, juce::dontSendNotification );
}

void NoctuaryEditor::PerformView::paint( juce::Graphics &g )
{
    g.fillAll( Bg );
    g.setColour( GroupFill );
    g.fillRoundedRectangle( getLocalBounds().reduced( 12 ).toFloat(), 10.0f );
    g.setColour( Dim );
    g.setFont( juce::FontOptions( 13.0f ) );
    g.drawText(
        "PERFORM  -  eight macros, one morph. Each knob moves several parameters; a knob acts once it has been moved.",
        getLocalBounds().reduced( 24 ).removeFromTop( 24 ), juce::Justification::centredLeft );
    m_aLabel.setText( "A: " + m_proc.morphSlotName( 0 ), juce::dontSendNotification );
    m_bLabel.setText( "B: " + m_proc.morphSlotName( 1 ), juce::dontSendNotification );
    updateSetInfo();
}

void NoctuaryEditor::PerformView::resized()
{
    auto area = getLocalBounds().reduced( 24 );
    auto top = area.removeFromTop( 32 );
    {   // set controls on the right of the title line
        auto s = top.removeFromRight( juce::jmin( 720, top.getWidth() - 620 ) );
        m_setStop.setBounds( s.removeFromRight( 90 ).reduced( 0, 4 ) );
        s.removeFromRight( 6 );
        m_setPlay.setBounds( s.removeFromRight( 100 ).reduced( 0, 4 ) );
        s.removeFromRight( 6 );
        m_setRec.setBounds( s.removeFromRight( 100 ).reduced( 0, 4 ) );
        s.removeFromRight( 10 );
        m_setInfo.setBounds( s );
    }
    auto      morphArea = area.removeFromBottom( 90 );
    const int cols = 4, rows = 2;
    const int cellW = area.getWidth() / cols, cellH = area.getHeight() / rows;
    for ( int m = 0; m < 8; ++m )
    {
        juce::Rectangle<int> cell( area.getX() + ( m % cols ) * cellW, area.getY() + ( m / cols ) * cellH, cellW,
                                   cellH );
        cell = cell.reduced( 16 );
        m_labels[static_cast<size_t>( m )]->setBounds( cell.removeFromBottom( 30 ) );
        const int side = std::min( cell.getWidth(), cell.getHeight() );
        m_knobs[static_cast<size_t>( m )]->setBounds( cell.withSizeKeepingCentre( side, side ) );
    }
    m_morphActive.setBounds( morphArea.removeFromLeft( 90 ).withSizeKeepingCentre( 90, 28 ) );
    m_morphLabel.setBounds( morphArea.removeFromTop( 22 ) );
    m_aLabel.setBounds( morphArea.removeFromLeft( 220 ) );
    m_bLabel.setBounds( morphArea.removeFromRight( 220 ) );
    m_morph.setBounds( morphArea.withSizeKeepingCentre( morphArea.getWidth(), 40 ) );
}
