/**
 * @file NoctuaryLookAndFeel.cpp
 * @brief The editor's look, drawn: the two typefaces, the arc helper, and every LookAndFeel override.
 *
 * The constructor maps JUCE's colour ids onto the palette of NoctuaryLookAndFeel.h; the rest is
 * drawing, in the order knob, sliders, boxes, buttons, menus, labels, scrollbars. Nothing here
 * keeps state: every override reads what it needs from the component it is handed, including the
 * slider properties the editor sets for the modulation arcs, the clock dial and the halo dot.
 */
#include "NoctuaryLookAndFeel.h"

namespace ui {

juce::Font title(float height)
{
    return juce::Font(juce::FontOptions(height, juce::Font::bold)).withExtraKerningFactor(0.14f);
}

juce::Font body(float height)
{
    return juce::Font(juce::FontOptions(height));
}

} // namespace ui

namespace {

/**
 * @brief An arc of the given thickness between two angles, with a soft wide copy underneath so a lit
 *        value seems to glow rather than to be outlined.
 * @param g          the graphics context
 * @param c          the centre
 * @param radius     the arc's radius in pixels
 * @param from       the start angle in radians (JUCE's rotary convention: 0 at the top, clockwise)
 * @param to         the end angle in radians; an arc shorter than 1e-4 draws nothing
 * @param thickness  the stroke width in pixels
 * @param colour     the stroke colour
 * @param glow       true also strokes a 2.6 times wider copy at 18 \% alpha underneath
 */
void arc(juce::Graphics& g, juce::Point<float> c, float radius, float from, float to,
         float thickness, juce::Colour colour, bool glow)
{
    if (std::abs(to - from) < 1.0e-4f) return;
    juce::Path p;
    p.addCentredArc(c.x, c.y, radius, radius, 0.0f, from, to, true);
    if (glow) {
        g.setColour(colour.withMultipliedAlpha(0.18f));
        g.strokePath(p, juce::PathStrokeType(thickness * 2.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    g.setColour(colour);
    g.strokePath(p, juce::PathStrokeType(thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

/**
 * @brief The slider's value as it prints it, trimmed, for the text inside the knob.
 * @param s  the slider
 * @return its text for the current value (unit suffix included), without surrounding spaces
 */
juce::String valueText(juce::Slider& s)
{
    juce::String t = s.getTextFromValue(s.getValue());
    return t.trim();
}

} // namespace

NoctuaryLookAndFeel::NoctuaryLookAndFeel()
{
    setColour(juce::ResizableWindow::backgroundColourId, ui::bg0);
    setColour(juce::Label::textColourId, ui::text);
    setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::rotarySliderFillColourId, ui::accent);
    setColour(juce::Slider::rotarySliderOutlineColourId, ui::track);
    setColour(juce::Slider::thumbColourId, ui::text);
    setColour(juce::Slider::textBoxTextColourId, ui::text);
    setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::ComboBox::backgroundColourId, ui::card.brighter(0.10f));
    setColour(juce::ComboBox::textColourId, ui::text);
    setColour(juce::ComboBox::outlineColourId, ui::cardEdge);
    setColour(juce::ComboBox::arrowColourId, ui::dim);
    setColour(juce::PopupMenu::backgroundColourId, ui::card.brighter(0.04f));
    setColour(juce::PopupMenu::textColourId, ui::text);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, ui::accent.withAlpha(0.22f));
    setColour(juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
    setColour(juce::TextButton::buttonColourId, ui::card.brighter(0.10f));
    setColour(juce::TextButton::buttonOnColourId, ui::accent.withAlpha(0.30f));
    setColour(juce::TextButton::textColourOffId, ui::text);
    setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    setColour(juce::ToggleButton::textColourId, ui::text);
    setColour(juce::ToggleButton::tickColourId, ui::accent);
    setColour(juce::TextEditor::backgroundColourId, ui::bg0);
    setColour(juce::TextEditor::textColourId, ui::text);
    setColour(juce::TextEditor::outlineColourId, ui::cardEdge);
    setColour(juce::TextEditor::focusedOutlineColourId, ui::accent.withAlpha(0.6f));
    setColour(juce::TextEditor::highlightColourId, ui::accent.withAlpha(0.3f));
    setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::ScrollBar::thumbColourId, ui::faint);
    setColour(juce::AlertWindow::backgroundColourId, ui::card);
    setColour(juce::AlertWindow::textColourId, ui::text);
    setColour(juce::AlertWindow::outlineColourId, ui::cardEdge);
}

// ---------------------------------------------------------------- rotary

void NoctuaryLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                          float pos, float startAngle, float endAngle, juce::Slider& s)
{
    const juce::Rectangle<float> area(static_cast<float>(x), static_cast<float>(y),
                                      static_cast<float>(width), static_cast<float>(height));
    const float side = juce::jmin(area.getWidth(), area.getHeight());
    const auto c = area.getCentre();
    const float r = side * 0.5f - 2.0f;
    if (r < 6.0f) return;

    const bool enabled = s.isEnabled();
    const bool hot = enabled && (s.isMouseOverOrDragging() || s.isMouseButtonDown());
    const juce::Colour fill = s.findColour(juce::Slider::rotarySliderFillColourId)
                                 .withMultipliedAlpha(enabled ? 1.0f : 0.35f);
    const float ringR = r - 1.5f;
    const float thick = juce::jlimit(2.0f, 4.0f, r * 0.13f);
    const float angle = startAngle + pos * (endAngle - startAngle);

    // Inner disc first, so the arc sits on top of its edge.
    {
        juce::ColourGradient grad(ui::card.brighter(hot ? 0.16f : 0.09f), c.x, c.y - r,
                                  ui::bg0.brighter(0.02f), c.x, c.y + r, false);
        g.setGradientFill(grad);
        g.fillEllipse(c.x - (ringR - thick - 1.5f), c.y - (ringR - thick - 1.5f),
                      2.0f * (ringR - thick - 1.5f), 2.0f * (ringR - thick - 1.5f));
    }

    arc(g, c, ringR, startAngle, endAngle, thick, ui::track, false);
    const bool bipolar = static_cast<bool>(s.getProperties().getWithDefault("bipolar", false));
    const float from = bipolar ? 0.5f * (startAngle + endAngle) : startAngle;
    arc(g, c, ringR, from, angle, thick, fill, hot);

    // A modulated knob wears a thin outer ring in its source's colour, and a second arc from the
    // value to where the modulation has pushed it this instant -- the Vital idiom, and the only
    // way to see from the panel what the matrix is doing.
    if (s.getProperties().contains("modColour")) {
        const juce::Colour mc(static_cast<juce::uint32>(static_cast<int>(s.getProperties()["modColour"])));
        const float off = static_cast<float>(s.getProperties().getWithDefault("modOffset", 0.0));
        const float outer = ringR + thick * 0.5f + 2.0f;
        g.setColour(mc.withAlpha(0.55f));
        g.drawEllipse(c.x - outer, c.y - outer, 2.0f * outer, 2.0f * outer, 1.2f);
        const float to = juce::jlimit(startAngle, endAngle, angle + off * (endAngle - startAngle));
        if (std::abs(to - angle) > 1.0e-3f) arc(g, c, ringR, angle, to, thick * 0.6f, mc, true);
    }

    // Not the matrix, but something playing a value the knob does not show -- the morph, the
    // map's blend, a route: the same arc in the neutral warm colour, no outer ring.
    if (s.getProperties().contains("liveOffset")) {
        const float off = static_cast<float>(s.getProperties().getWithDefault("liveOffset", 0.0));
        const float to = juce::jlimit(startAngle, endAngle, angle + off * (endAngle - startAngle));
        if (std::abs(to - angle) > 1.0e-3f) arc(g, c, ringR, angle, to, thick * 0.6f, ui::live.withAlpha(0.85f), true);
    }
    // A slow process's own state -- where the arc or the tide stands in its swing -- as a dot on
    // the outer ring, from one end of the knob's travel (-1) to the other (+1).
    if (s.getProperties().contains("clockHour")) {
        // The day as a dial round the knob: midnight at the top, the hour as a dot, and faint
        // marks at four, ten, sixteen and twenty-two -- the bottom, the crossings and the top of
        // the arc that follows the clock.
        const float outer = ringR + thick * 0.5f + 2.0f;
        const float hour = static_cast<float>(s.getProperties().getWithDefault("clockHour", 0.0));
        g.setColour(ui::track.withAlpha(0.8f));
        g.drawEllipse(c.x - outer, c.y - outer, 2.0f * outer, 2.0f * outer, 1.0f);
        for (float h : { 4.0f, 10.0f, 16.0f, 22.0f }) {
            const float a = juce::MathConstants<float>::twoPi * h / 24.0f;
            const juce::Point<float> p0(c.x + (outer - 2.5f) * std::sin(a), c.y - (outer - 2.5f) * std::cos(a));
            const juce::Point<float> p1(c.x + (outer + 2.5f) * std::sin(a), c.y - (outer + 2.5f) * std::cos(a));
            g.setColour(ui::live.withAlpha(h == 4.0f || h == 16.0f ? 0.8f : 0.4f));
            g.drawLine(p0.x, p0.y, p1.x, p1.y, 1.2f);
        }
        const float a = juce::MathConstants<float>::twoPi * hour / 24.0f;
        const juce::Point<float> p(c.x + outer * std::sin(a), c.y - outer * std::cos(a));
        g.setColour(ui::live.withAlpha(0.35f)); g.fillEllipse(p.x - 4.0f, p.y - 4.0f, 8.0f, 8.0f);
        g.setColour(ui::live); g.fillEllipse(p.x - 2.0f, p.y - 2.0f, 4.0f, 4.0f);
    }
    if (s.getProperties().contains("halo")) {
        const float h = juce::jlimit(-1.0f, 1.0f, static_cast<float>(s.getProperties().getWithDefault("halo", 0.0)));
        const float a = startAngle + (0.5f + 0.5f * h) * (endAngle - startAngle);
        const float outer = ringR + thick * 0.5f + 2.0f;
        const juce::Point<float> p(c.x + outer * std::sin(a), c.y - outer * std::cos(a));
        g.setColour(ui::live.withAlpha(0.35f));
        g.fillEllipse(p.x - 4.0f, p.y - 4.0f, 8.0f, 8.0f);
        g.setColour(ui::live);
        g.fillEllipse(p.x - 2.0f, p.y - 2.0f, 4.0f, 4.0f);
    }

    // Pointer: a short radial tick, not a full needle.
    {
        juce::Path p;
        p.addLineSegment({ 0.0f, -(ringR - thick - 2.0f), 0.0f, -(ringR - thick * 0.5f - 4.5f) }, 1.0f);
        g.setColour(enabled ? ui::text.withAlpha(hot ? 1.0f : 0.75f) : ui::faint);
        g.strokePath(p, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                     juce::AffineTransform::rotation(angle).translated(c.x, c.y));
    }

    // The value lives inside the knob; the cell's label below carries the name. The small knobs of
    // the modulation strip have no room for it and show nothing rather than an overflowing number.
    if (r >= 18.0f) {
        const juce::String txt = valueText(s);
        if (txt.isNotEmpty()) {
            g.setColour(enabled ? (hot ? ui::text : ui::text.withAlpha(0.82f)) : ui::faint);
            // A fixed-width face for the figures, so a value that moves does not shiver in width.
            g.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), juce::jlimit(9.0f, 13.0f, r * 0.62f), juce::Font::plain)));
            g.drawFittedText(txt, area.reduced(r * 0.30f).toNearestInt(), juce::Justification::centred, 1, 0.7f);   // the fixed-width figures are wider: shrink rather than cut ("40 mi")
        }
    }
}

juce::Slider::SliderLayout NoctuaryLookAndFeel::getSliderLayout(juce::Slider& s)
{
    juce::Slider::SliderLayout layout;
    layout.sliderBounds = s.getLocalBounds();
    layout.textBoxBounds = {};
    return layout;
}

juce::Label* NoctuaryLookAndFeel::createSliderTextBox(juce::Slider& s)
{
    auto* l = LookAndFeel_V4::createSliderTextBox(s);
    l->setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    l->setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
    return l;
}

void NoctuaryLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                                          float pos, float, float, juce::Slider::SliderStyle style,
                                          juce::Slider& s)
{
    const juce::Colour fill = s.findColour(juce::Slider::rotarySliderFillColourId);
    if (style == juce::Slider::LinearHorizontal || style == juce::Slider::LinearBar) {
        const float cy = y + height * 0.5f;
        g.setColour(ui::track);
        g.fillRoundedRectangle(static_cast<float>(x), cy - 2.0f, static_cast<float>(width), 4.0f, 2.0f);
        g.setColour(fill);
        g.fillRoundedRectangle(static_cast<float>(x), cy - 2.0f, pos - x, 4.0f, 2.0f);
        g.setColour(ui::text);
        g.fillEllipse(pos - 5.0f, cy - 5.0f, 10.0f, 10.0f);
    } else {
        const float cx = x + width * 0.5f;
        g.setColour(ui::track);
        g.fillRoundedRectangle(cx - 2.0f, static_cast<float>(y), 4.0f, static_cast<float>(height), 2.0f);
        g.setColour(fill);
        g.fillRoundedRectangle(cx - 2.0f, pos, 4.0f, static_cast<float>(y + height) - pos, 2.0f);
        g.setColour(ui::text);
        g.fillEllipse(cx - 5.0f, pos - 5.0f, 10.0f, 10.0f);
    }
}

// ---------------------------------------------------------------- boxes and buttons

void NoctuaryLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool,
                                      int, int, int, int, juce::ComboBox& box)
{
    const juce::Rectangle<float> r(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    const bool hot = box.isMouseOver() || box.isPopupActive();
    g.setColour(box.findColour(juce::ComboBox::backgroundColourId).brighter(hot ? 0.08f : 0.0f));
    g.fillRoundedRectangle(r.reduced(0.5f), 5.0f);
    g.setColour(hot ? ui::accent.withAlpha(0.5f) : box.findColour(juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle(r.reduced(0.5f), 5.0f, 1.0f);

    juce::Path chevron;                       // a chevron, not a filled triangle
    const float cx = r.getRight() - 13.0f, cy = r.getCentreY();
    chevron.startNewSubPath(cx - 4.0f, cy - 2.0f);
    chevron.lineTo(cx, cy + 2.5f);
    chevron.lineTo(cx + 4.0f, cy - 2.0f);
    g.setColour(box.isEnabled() ? ui::dim : ui::faint);
    g.strokePath(chevron, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void NoctuaryLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    label.setBounds(9, 0, box.getWidth() - 26, box.getHeight());
    label.setFont(getComboBoxFont(box));
    label.setColour(juce::Label::textColourId, box.isEnabled() ? ui::text : ui::faint);
}

juce::Font NoctuaryLookAndFeel::getComboBoxFont(juce::ComboBox& box)
{
    return ui::body(juce::jmin(13.0f, box.getHeight() * 0.6f));
}

void NoctuaryLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& b,
                                          bool highlighted, bool)
{
    // A pill switch: on/off is a position, not a tick, which reads at a glance in a wall of cells.
    const auto r = b.getLocalBounds().toFloat();
    const float h = juce::jmin(18.0f, r.getHeight());
    const float w = juce::jmin(32.0f, r.getWidth());
    const juce::Rectangle<float> pill(r.getCentreX() - w * 0.5f, r.getCentreY() - h * 0.5f, w, h);
    const bool on = b.getToggleState();
    const juce::Colour accent = b.findColour(juce::ToggleButton::tickColourId);
    g.setColour(on ? accent.withAlpha(b.isEnabled() ? 0.85f : 0.3f) : ui::track);
    g.fillRoundedRectangle(pill, h * 0.5f);
    if (highlighted) { g.setColour(ui::text.withAlpha(0.15f)); g.drawRoundedRectangle(pill.reduced(0.5f), h * 0.5f, 1.0f); }
    const float kr = h - 6.0f;
    const float kx = on ? pill.getRight() - kr - 3.0f : pill.getX() + 3.0f;
    g.setColour(on ? juce::Colours::white.withAlpha(0.92f) : ui::dim);
    g.fillEllipse(kx, pill.getCentreY() - kr * 0.5f, kr, kr);

    const juce::String t = b.getButtonText();
    if (t.isNotEmpty() && r.getWidth() > w + 16.0f) {
        g.setColour(b.isEnabled() ? ui::text : ui::faint);
        g.setFont(ui::body(12.0f));
        g.drawText(t, static_cast<int>(pill.getRight()) + 8, 0, b.getWidth() - static_cast<int>(pill.getRight()) - 8,
                   b.getHeight(), juce::Justification::centredLeft);
    }
}

void NoctuaryLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour&,
                                              bool highlighted, bool down)
{
    const auto r = b.getLocalBounds().toFloat().reduced(0.5f);
    const bool on = b.getToggleState();
    juce::Colour base = on ? b.findColour(juce::TextButton::buttonOnColourId)
                           : b.findColour(juce::TextButton::buttonColourId);
    if (down) base = base.brighter(0.16f);
    else if (highlighted) base = base.brighter(0.09f);
    g.setColour(base);
    g.fillRoundedRectangle(r, 5.0f);
    g.setColour(on ? ui::accent.withAlpha(0.55f) : (highlighted ? ui::accent.withAlpha(0.35f) : ui::cardEdge));
    g.drawRoundedRectangle(r, 5.0f, 1.0f);
}

void NoctuaryLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    g.setColour(b.findColour(b.getToggleState() ? juce::TextButton::textColourOnId
                                                : juce::TextButton::textColourOffId)
                    .withMultipliedAlpha(b.isEnabled() ? 1.0f : 0.4f));
    g.setFont(getTextButtonFont(b, b.getHeight()));
    g.drawText(b.getButtonText(), b.getLocalBounds().reduced(6, 0), juce::Justification::centred, false);
}

juce::Font NoctuaryLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    return ui::body(juce::jmin(13.0f, buttonHeight * 0.55f));
}

void NoctuaryLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
{
    const juce::Rectangle<float> r(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    g.setColour(ui::card.brighter(0.05f));
    g.fillRoundedRectangle(r.reduced(0.5f), 6.0f);
    g.setColour(ui::cardEdge);
    g.drawRoundedRectangle(r.reduced(0.5f), 6.0f, 1.0f);
}

juce::Font NoctuaryLookAndFeel::getPopupMenuFont() { return ui::body(13.0f); }

void NoctuaryLookAndFeel::drawLabel(juce::Graphics& g, juce::Label& l)
{
    if (!l.isBeingEdited()) {
        g.setColour(l.findColour(juce::Label::textColourId).withMultipliedAlpha(l.isEnabled() ? 1.0f : 0.4f));
        g.setFont(l.getFont());
        g.drawFittedText(l.getText(), l.getLocalBounds(), l.getJustificationType(), 1, 1.0f);
    }
}

void NoctuaryLookAndFeel::drawScrollbar(juce::Graphics& g, juce::ScrollBar&, int x, int y, int width,
                                       int height, bool vertical, int thumbPos, int thumbSize,
                                       bool mouseOver, bool down)
{
    if (thumbSize <= 0) return;
    juce::Rectangle<float> thumb = vertical
        ? juce::Rectangle<float>(x + width * 0.35f, static_cast<float>(thumbPos), width * 0.3f, static_cast<float>(thumbSize))
        : juce::Rectangle<float>(static_cast<float>(thumbPos), y + height * 0.35f, static_cast<float>(thumbSize), height * 0.3f);
    g.setColour(ui::faint.withAlpha(down ? 0.95f : (mouseOver ? 0.75f : 0.5f)));
    g.fillRoundedRectangle(thumb, juce::jmin(thumb.getWidth(), thumb.getHeight()) * 0.5f);
}
