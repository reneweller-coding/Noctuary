/**
 * @file EditorDrag.cpp
 * @brief What a modulation route looks like while it is being made.
 *
 * Dragging a card from the strip onto a knob worked, but you could not see it: the strip drew a
 * line inside its own bounds, and the gesture leaves those bounds immediately. So the modulator
 * in flight is drawn over the whole panel now -- a line from the card it came from, a small copy
 * of the card under the cursor, and a ring around the knob it would land on with its name -- and
 * when it lands, a knob for the depth appears where it landed, in the modulator's colour, and
 * goes away at the next click somewhere else. Both are Pigments' idea; both are the difference
 * between a gesture you have to remember and one you can see.
 * @author Rene Weller
 */
#include "PluginEditor.h"
#include "EditorCommon.h"

using namespace ambient;

// ---------------------------------------------------------------- the drag overlay

void NoctuaryEditor::DragOverlay::paint(juce::Graphics& g)
{
    if (!m_active) return;
    // The knob it would land on: a ring in the modulator's colour, and the parameter's name.
    if (!m_target.isEmpty()) {
        g.setColour(m_colour.withAlpha(0.22f));
        g.fillRoundedRectangle(m_target.expanded(5.0f), 6.0f);
        g.setColour(m_colour.withAlpha(0.95f));
        g.drawRoundedRectangle(m_target.expanded(5.0f), 6.0f, 2.0f);
        if (m_targetName.isNotEmpty()) {
            g.setFont(ui::title(11.0f));
            g.setColour(m_colour.brighter(0.4f));
            g.drawText(m_targetName, juce::Rectangle<float>(m_target.getCentreX() - 90.0f, m_target.getY() - 20.0f, 180.0f, 16.0f),
                       juce::Justification::centred, false);
        }
    }
    // The line, from the card to the hand. Slightly curved, so it reads as a lead and not as a
    // border of something.
    juce::Path p;
    p.startNewSubPath(m_from);
    const float dx = m_to.x - m_from.x, dy = m_to.y - m_from.y;
    p.quadraticTo(m_from.x + dx * 0.5f - dy * 0.12f, m_from.y + dy * 0.5f + dx * 0.12f, m_to.x, m_to.y);
    g.setColour(m_colour.withAlpha(0.85f));
    g.strokePath(p, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(m_colour.withAlpha(0.55f));
    g.fillEllipse(m_from.x - 4.0f, m_from.y - 4.0f, 8.0f, 8.0f);

    // The card itself, under the cursor: the same shape the lane draws, so what is in the hand is
    // recognisably the thing that was picked up.
    const float w = 74.0f, h = 26.0f;
    const juce::Rectangle<float> CardColour(m_to.x + 12.0f, m_to.y - h * 0.5f, w, h);
    g.setColour(ui::CardColour.withAlpha(0.96f));
    g.fillRoundedRectangle(CardColour, 5.0f);
    g.setColour(m_colour.withAlpha(0.9f));
    g.drawRoundedRectangle(CardColour.reduced(0.5f), 5.0f, 1.4f);
    g.setColour(m_colour.brighter(0.5f));
    g.setFont(ui::title(10.5f));
    g.drawText(m_name, CardColour, juce::Justification::centred, false);
}

void NoctuaryEditor::showModDrag(juce::Point<int> screenFrom, juce::Point<int> screenTo,
                                     const juce::String& name, juce::Colour c)
{
    m_dragOverlay.m_active = true;
    m_dragOverlay.m_from = m_dragOverlay.getLocalPoint(nullptr, screenFrom.toFloat());
    m_dragOverlay.m_to = m_dragOverlay.getLocalPoint(nullptr, screenTo.toFloat());
    m_dragOverlay.m_name = name;
    m_dragOverlay.m_colour = c;
    m_dragOverlay.m_target = {};
    m_dragOverlay.m_targetName = {};
    const int param = cellParamAt(screenTo);
    if (param >= 0) {
        const int ci = cellForParam(static_cast<ParamIdE>(param));
        const juce::Rectangle<int> b = cellScreenBounds(ci);
        if (!b.isEmpty()) {
            const auto tl = m_dragOverlay.getLocalPoint(nullptr, b.getTopLeft().toFloat());
            const auto br = m_dragOverlay.getLocalPoint(nullptr, b.getBottomRight().toFloat());
            m_dragOverlay.m_target = juce::Rectangle<float>(tl, br);
            m_dragOverlay.m_targetName = paramDesc(static_cast<ParamIdE>(param)).m_name;
        }
    }
    m_dragOverlay.setVisible(true);
    m_dragOverlay.toFront(false);
    m_dragOverlay.repaint();
}

void NoctuaryEditor::hideModDrag()
{
    if (!m_dragOverlay.m_active) return;
    m_dragOverlay.m_active = false;
    m_dragOverlay.repaint();
}

// ---------------------------------------------------------------- the route's depth, as text

float NoctuaryEditor::routeDepth(ModSourceE src, ParamIdE target) const
{
    const ModMatrix& m = m_proc.engine().modMatrix();
    for (int i = 0; i < m.count(); ++i)
        if (m.route(i).m_source == src && m.route(i).m_target == target) return m.route(i).m_depth;
    return 0.0f;
}

/**
 * @brief One route changed, the rest written back exactly as it stands.
 *
 * The matrix has no "set this
 * row" call -- it travels as text everywhere in this program, which is what the preset files and
 * the OSC interface use as well, so there is one form of it and not two.
 * @param[in] m       the matrix as the engine holds it now, read route by route
 * @param[in] src     the source of the one route to change or drop
 * @param[in] target  the target of that route
 * @param[in] depth   the new depth for the matching route, or nullptr to keep the depth it has
 * @param[in] drop    true leaves the matching route out of the text altogether
 * @return the whole matrix as `source>target:depth[:via][:u]` rows joined with ';', in the form
 *         Engine::setModMatrixText parses
 */
static juce::String matrixTextWith(const ModMatrix& m, ModSourceE src, ParamIdE target, const float* depth, bool drop)
{
    juce::String t;
    for (int i = 0; i < m.count(); ++i) {
        const ModRoute& r = m.route(i);
        const bool hit = r.m_source == src && r.m_target == target;
        if (hit && drop) continue;
        if (t.isNotEmpty()) t += ";";
        t += juce::String(modSourceName(r.m_source)) + ">" + paramDesc(r.m_target).m_key + ":"
           + juce::String(hit && depth != nullptr ? *depth : r.m_depth, 3);
        if (r.m_via != ModSourceE::NONE) t += ":" + juce::String(modSourceName(r.m_via));
        if (r.m_unipolar) t += r.m_via != ModSourceE::NONE ? ":u" : ":none:u";
    }
    return t;
}

bool NoctuaryEditor::setRouteDepth(ModSourceE src, ParamIdE target, float depth)
{
    const juce::String t = matrixTextWith(m_proc.engine().modMatrix(), src, target, &depth, false);
    const bool ok = m_proc.engine().setModMatrixText(t.toRawUTF8());
    if (ok) { if (m_mod) m_mod->pullMatrix(); repaint(); }
    return ok;
}

bool NoctuaryEditor::removeRoute(ModSourceE src, ParamIdE target)
{
    const juce::String t = matrixTextWith(m_proc.engine().modMatrix(), src, target, nullptr, true);
    const bool ok = m_proc.engine().setModMatrixText(t.toRawUTF8());
    if (ok) { if (m_mod) m_mod->pullMatrix(); repaint(); }
    return ok;
}

// ---------------------------------------------------------------- the depth popup

NoctuaryEditor::DepthPopup::DepthPopup(NoctuaryEditor& o, ModSourceE s, ParamIdE t, juce::Colour c)
    : m_owner(o), m_source(s), m_target(t), m_colour(c)
{
    m_depth.setSliderStyle(juce::Slider::LinearHorizontal);
    m_depth.setRange(-1.0, 1.0, 0.001);
    m_depth.setValue(m_owner.routeDepth(s, t), juce::dontSendNotification);
    m_depth.setTextBoxStyle(juce::Slider::TextBoxRight, false, 46, 18);
    m_depth.setNumDecimalPlacesToDisplay(2);
    m_depth.setDoubleClickReturnValue(true, 0.25);
    m_depth.setColour(juce::Slider::trackColourId, m_colour.withAlpha(0.85f));
    m_depth.setColour(juce::Slider::thumbColourId, m_colour.brighter(0.3f));
    m_depth.setTooltip("How much of the target's range this modulator moves. Negative turns it the other way.");
    m_depth.onValueChange = [this] { m_owner.setRouteDepth(m_source, m_target, static_cast<float>(m_depth.getValue())); };
    addAndMakeVisible(m_depth);
    m_remove.setTooltip("Take this route away again");
    m_remove.onClick = [this] {
        m_owner.removeRoute(m_source, m_target);
        m_owner.hideDepthPopup();      // deletes this: nothing may touch it afterwards
    };
    addAndMakeVisible(m_remove);
    setSize(232, 46);
    // The next click anywhere but in here puts it away, which is what makes it a popup and not
    // another panel to tidy up after.
    juce::Desktop::getInstance().addGlobalMouseListener(this);
}

NoctuaryEditor::DepthPopup::~DepthPopup()
{
    juce::Desktop::getInstance().removeGlobalMouseListener(this);
}

void NoctuaryEditor::DepthPopup::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(ui::CardColour.withAlpha(0.97f));
    g.fillRoundedRectangle(r, 6.0f);
    g.setColour(m_colour.withAlpha(0.9f));
    g.drawRoundedRectangle(r, 6.0f, 1.4f);
    g.setColour(m_colour.brighter(0.45f));
    g.setFont(ui::title(10.5f));
    g.drawText(juce::String(modSourceName(m_source)).toUpperCase() + "  " + juce::String(juce::CharPointer_UTF8("\xe2\x86\x92")) + "  "
                   + paramDesc(m_target).m_name,
               getLocalBounds().removeFromTop(18).reduced(8, 0), juce::Justification::centredLeft, false);
}

void NoctuaryEditor::DepthPopup::resized()
{
    auto r = getLocalBounds().reduced(8, 6);
    r.removeFromTop(16);
    m_remove.setBounds(r.removeFromRight(22));
    r.removeFromRight(4);
    m_depth.setBounds(r);
}

void NoctuaryEditor::showDepthPopup(ModSourceE src, ParamIdE target, juce::Colour c, juce::Point<int> screenAt)
{
    hideDepthPopup();
    m_depthPopup = std::make_unique<DepthPopup>(*this, src, target, c);
    addAndMakeVisible(*m_depthPopup);
    m_depthPopup->setTransform(juce::AffineTransform::scale(m_scale));
    const auto at = m_depthPopup->getLocalPoint(nullptr, screenAt.toFloat());
    // Beside the knob, and inside the panel: a popup half off the edge is worse than none.
    const int w = m_depthPopup->getWidth(), h = m_depthPopup->getHeight();
    int x = juce::roundToInt(at.x) + 14, y = juce::roundToInt(at.y) - h / 2;
    x = juce::jlimit(4, juce::jmax(4, m_designW - w - 4), x);
    y = juce::jlimit(4, juce::jmax(4, m_designH - h - 4), y);
    m_depthPopup->setTopLeftPosition(x, y);
    m_depthPopup->toFront(false);
}

void NoctuaryEditor::hideDepthPopup()
{
    m_depthPopup.reset();
}

