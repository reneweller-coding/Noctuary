// Noctuary -- the editor's look. A dark, cool ground with one warm accent for anything that
// is actually sounding; knobs are a thin value arc rather than a chrome cap, and the value lives
// inside the knob so a cell is a knob and a name, not a knob, a box and a name.
//
// The palette is deliberately not the flat grey of a mixing tool: the instrument is about
// distance and slow motion, so the ground is a deep blue-black with a little vertical lift, and
// each signal group keeps its own hue on the arc and on the card's edge.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace ui {

// Ground and surfaces
const juce::Colour bg0        { 0xff080b12 };   // window, top
const juce::Colour bg1        { 0xff0e1220 };   // window, bottom
const juce::Colour card       { 0xff141a29 };   // section card
const juce::Colour cardEdge   { 0xff1f2739 };
const juce::Colour group      { 0xff10151f };   // group panel behind the cards
const juce::Colour track      { 0xff232c41 };   // unfilled part of an arc

// Type
const juce::Colour text       { 0xffcfd7e6 };
const juce::Colour dim        { 0xff6d7a92 };
const juce::Colour faint      { 0xff3d4658 };

// Accents
const juce::Colour accent     { 0xff6fd8d2 };   // the instrument's own colour: cold, wide
const juce::Colour live       { 0xffe8a45c };   // warm: something is sounding or moving now
const juce::Colour voiceCol   { 0xff74b6e0 };
const juce::Colour foreCol    { 0xff86d3a6 };
const juce::Colour backCol    { 0xffb197e8 };
const juce::Colour cosmosCol  { 0xffe8a45c };
const juce::Colour condCol    { 0xff6fd8d2 };
const juce::Colour morphCol   { 0xffe07fb0 };
const juce::Colour masterCol  { 0xff9aa7bd };

juce::Font title(float height);      // letter-spaced small caps for headings
juce::Font body(float height);

} // namespace ui

// Slider properties the look reads:
//   "bipolar" (bool)  arc grows from the centre instead of from the left
//   "suffix"  (String) unit drawn after the value inside the knob
class NoctuaryLookAndFeel : public juce::LookAndFeel_V4
{
public:
    NoctuaryLookAndFeel();

    void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height, float pos,
                          float startAngle, float endAngle, juce::Slider&) override;
    juce::Slider::SliderLayout getSliderLayout(juce::Slider&) override;
    juce::Label* createSliderTextBox(juce::Slider&) override;

    void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height, float pos,
                          float min, float max, juce::Slider::SliderStyle, juce::Slider&) override;

    void drawComboBox(juce::Graphics&, int width, int height, bool down, int buttonX, int buttonY,
                      int buttonW, int buttonH, juce::ComboBox&) override;
    void positionComboBoxText(juce::ComboBox&, juce::Label&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;

    void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;

    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour& background,
                              bool highlighted, bool down) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;

    void drawPopupMenuBackground(juce::Graphics&, int width, int height) override;
    juce::Font getPopupMenuFont() override;
    void drawLabel(juce::Graphics&, juce::Label&) override;

    void drawScrollbar(juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height,
                       bool vertical, int thumbPos, int thumbSize, bool mouseOver, bool down) override;
};
