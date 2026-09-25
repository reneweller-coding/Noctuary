/**
 * @file NoctuaryLookAndFeel.h
 * @brief The editor's look.
 *
 * A dark, cool ground with one warm accent for anything that
 * is actually sounding; knobs are a thin value arc rather than a chrome cap, and the value lives
 * inside the knob so a cell is a knob and a name, not a knob, a box and a name.
 *
 * The palette is deliberately not the flat grey of a mixing tool: the instrument is about
 * distance and slow motion, so the ground is a deep blue-black with a little vertical lift, and
 * each signal group keeps its own hue on the arc and on the card's edge.
 */
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

/** @brief The palette and the two typefaces of the editor; EditorCommon.h gives them shorter names. */
namespace ui {

/** @name Ground and surfaces
 *  @{ */
const juce::Colour bg0        { 0xff080b12 };   ///< window, top
const juce::Colour bg1        { 0xff0e1220 };   ///< window, bottom
const juce::Colour card       { 0xff141a29 };   ///< section card
const juce::Colour cardEdge   { 0xff1f2739 };   ///< the one-pixel edge of a card, a box or a button
const juce::Colour group      { 0xff10151f };   ///< group panel behind the cards
const juce::Colour track      { 0xff232c41 };   ///< unfilled part of an arc
/** @} */

/** @name Type
 *  @{ */
const juce::Colour text       { 0xffcfd7e6 };   ///< primary text: values, names, button labels
const juce::Colour dim        { 0xff6d7a92 };   ///< secondary text and the box chevron
const juce::Colour faint      { 0xff3d4658 };   ///< the faintest: axis labels, disabled controls, scrollbar thumbs
/** @} */

/** @name Accents
 *  @{ */
const juce::Colour accent     { 0xff6fd8d2 };   ///< the instrument's own colour: cold, wide
const juce::Colour live       { 0xffe8a45c };   ///< warm: something is sounding or moving now
const juce::Colour voiceCol   { 0xff74b6e0 };   ///< the Voice group
const juce::Colour foreCol    { 0xff86d3a6 };   ///< the near (foreground) layer
const juce::Colour backCol    { 0xffb197e8 };   ///< the background layer
const juce::Colour cosmosCol  { 0xffe8a45c };   ///< the Cosmos chain (the same warm hue as live)
const juce::Colour condCol    { 0xff6fd8d2 };   ///< the conductor (the same hue as accent)
const juce::Colour morphCol   { 0xffe07fb0 };   ///< Morph
const juce::Colour masterCol  { 0xff9aa7bd };   ///< the master section
/** @} */

/**
 * @brief letter-spaced small caps for headings
 * @param height  the font height in points
 * @return a bold face with 0.14 extra kerning
 */
juce::Font title(float height);
/**
 * @brief The plain face for everything that is not a heading.
 * @param height  the font height in points
 * @return the default face at that height
 */
juce::Font body(float height);

} // namespace ui

/**
 * @brief JUCE's LookAndFeel_V4 with the palette above and the instrument's own knobs, boxes,
 *        buttons, menus and scrollbars.
 *
 * Slider properties the look reads:
 *   "bipolar" (bool)  arc grows from the centre instead of from the left
 *   "suffix"  (String) unit drawn after the value inside the knob
 *
 * The editor sets more of them (PluginEditor.cpp): "modColour" and "modOffset" for a knob the
 * matrix moves, "liveOffset" for one the morph, the map's blend or a route moves, "clockHour" for
 * the arc's dial round the clock, and "halo" for a slow process's own state; drawRotarySlider()
 * says what each one draws.
 */
class NoctuaryLookAndFeel : public juce::LookAndFeel_V4
{
public:
    /** @brief Sets every JUCE colour id the editor's components read to the palette. */
    NoctuaryLookAndFeel();

    /**
     * @brief The knob: an inner disc, the track, the value arc (from the centre when bipolar), the
     *        modulation and live arcs, the clock dial or halo dot, a short pointer, and the value
     *        text inside when the knob is big enough.
     * @param g           the graphics context
     * @param x           the bounds the slider gives
     * @param y           the bounds the slider gives
     * @param width       the bounds the slider gives
     * @param height      the bounds the slider gives
     * @param pos         the value, 0 .. 1 along the arc
     * @param startAngle  the arc's start in radians
     * @param endAngle    the arc's end in radians
     * @param s           the slider, for its colours, properties and text
     */
    void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height, float pos,
                          float startAngle, float endAngle, juce::Slider&) override;
    /**
     * @brief The whole component is the knob; there is no text box, the value is drawn inside.
     * @param s  the slider
     * @return its bounds as the slider area and an empty text box
     */
    juce::Slider::SliderLayout getSliderLayout(juce::Slider&) override;
    /**
     * @brief JUCE's text box made invisible (no background, no outline), for the sliders that keep one.
     * @param s  the slider
     * @return the label, owned by the slider
     */
    juce::Label* createSliderTextBox(juce::Slider&) override;

    /**
     * @brief A linear slider as a thin track with a filled part and a round thumb, horizontal or vertical.
     * @param g       the graphics context
     * @param x       the bounds the slider gives
     * @param y       the bounds the slider gives
     * @param width   the bounds the slider gives
     * @param height  the bounds the slider gives
     * @param pos     the thumb's position in pixels along the track
     * @param min     unused
     * @param max     unused
     * @param style   horizontal (LinearHorizontal, LinearBar) or anything else, taken as vertical
     * @param s       the slider, for its fill colour
     */
    void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height, float pos,
                          float min, float max, juce::Slider::SliderStyle, juce::Slider&) override;

    /**
     * @brief A rounded box with a chevron, brighter and accent-edged while hot.
     * @param g        the graphics context
     * @param width    the box's width
     * @param height   the box's height
     * @param down     unused
     * @param buttonX  unused
     * @param buttonY  unused
     * @param buttonW  unused
     * @param buttonH  unused
     * @param box      the box, for its colours and hover state
     */
    void drawComboBox(juce::Graphics&, int width, int height, bool down, int buttonX, int buttonY,
                      int buttonW, int buttonH, juce::ComboBox&) override;
    /**
     * @brief Places the box's label left of the chevron and gives it the box font and colour.
     * @param box    the box
     * @param label  its text label
     */
    void positionComboBoxText(juce::ComboBox&, juce::Label&) override;
    /**
     * @param box  the box
     * @return the body face, 60 \% of the box height and at most 13 pt
     */
    juce::Font getComboBoxFont(juce::ComboBox&) override;

    /**
     * @brief A pill switch with a sliding knob and the text beside it (see the body).
     * @param g            the graphics context
     * @param b            the button, for its state, colour and text
     * @param highlighted  the mouse is over it: a faint outline
     * @param down         unused
     */
    void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;

    /**
     * @brief A rounded button: brighter when highlighted or down, accent-edged when on.
     * @param g            the graphics context
     * @param b            the button, for its toggle state and colours
     * @param background   unused; the colour ids decide
     * @param highlighted  the mouse is over it
     * @param down         it is pressed
     */
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour& background,
                              bool highlighted, bool down) override;
    /**
     * @brief The button's text centred, in the on or off colour, faded when disabled.
     * @param g            the graphics context
     * @param b            the button
     * @param highlighted  unused
     * @param down         unused
     */
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    /**
     * @param buttonHeight  the button's height in pixels
     * @return the body face, 55 \% of the height and at most 13 pt
     */
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;

    /**
     * @brief A rounded card with an edge behind a menu.
     * @param g       the graphics context
     * @param width   the menu's width
     * @param height  the menu's height
     */
    void drawPopupMenuBackground(juce::Graphics&, int width, int height) override;
    /** @return the body face at 13 pt */
    juce::Font getPopupMenuFont() override;
    /**
     * @brief The label's text fitted into its bounds, faded when disabled; nothing while it is being edited.
     * @param g  the graphics context
     * @param l  the label
     */
    void drawLabel(juce::Graphics&, juce::Label&) override;

    /**
     * @brief A thin rounded thumb and no track, brighter under the mouse and while dragged.
     * @param g          the graphics context
     * @param x          the bar's bounds
     * @param y          the bar's bounds
     * @param width      the bar's bounds
     * @param height     the bar's bounds
     * @param vertical   the bar's direction
     * @param thumbPos   the thumb's start along the bar, in pixels
     * @param thumbSize  the thumb's length in pixels; 0 or less draws nothing
     * @param mouseOver  the mouse is over the bar
     * @param down       the thumb is being dragged
     */
    void drawScrollbar(juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height,
                       bool vertical, int thumbPos, int thumbSize, bool mouseOver, bool down) override;
};
