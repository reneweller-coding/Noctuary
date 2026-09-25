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
 */
#include "PluginEditor.h"
#include "EditorCommon.h"

using namespace ambient;

// ---------------------------------------------------------------- the drag overlay

void NoctuaryEditor::DragOverlay::paint(juce::Graphics& g)
{
    if (!active) return;
    // The knob it would land on: a ring in the modulator's colour, and the parameter's name.
    if (!target.isEmpty()) {
        g.setColour(colour.withAlpha(0.22f));
        g.fillRoundedRectangle(target.expanded(5.0f), 6.0f);
        g.setColour(colour.withAlpha(0.95f));
        g.drawRoundedRectangle(target.expanded(5.0f), 6.0f, 2.0f);
        if (targetName.isNotEmpty()) {
            g.setFont(ui::title(11.0f));
            g.setColour(colour.brighter(0.4f));
            g.drawText(targetName, juce::Rectangle<float>(target.getCentreX() - 90.0f, target.getY() - 20.0f, 180.0f, 16.0f),
                       juce::Justification::centred, false);
        }
    }
    // The line, from the card to the hand. Slightly curved, so it reads as a lead and not as a
    // border of something.
    juce::Path p;
    p.startNewSubPath(from);
    const float dx = to.x - from.x, dy = to.y - from.y;
    p.quadraticTo(from.x + dx * 0.5f - dy * 0.12f, from.y + dy * 0.5f + dx * 0.12f, to.x, to.y);
    g.setColour(colour.withAlpha(0.85f));
    g.strokePath(p, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(colour.withAlpha(0.55f));
    g.fillEllipse(from.x - 4.0f, from.y - 4.0f, 8.0f, 8.0f);

    // The card itself, under the cursor: the same shape the lane draws, so what is in the hand is
    // recognisably the thing that was picked up.
    const float w = 74.0f, h = 26.0f;
    const juce::Rectangle<float> card(to.x + 12.0f, to.y - h * 0.5f, w, h);
    g.setColour(ui::card.withAlpha(0.96f));
    g.fillRoundedRectangle(card, 5.0f);
    g.setColour(colour.withAlpha(0.9f));
    g.drawRoundedRectangle(card.reduced(0.5f), 5.0f, 1.4f);
    g.setColour(colour.brighter(0.5f));
    g.setFont(ui::title(10.5f));
    g.drawText(name, card, juce::Justification::centred, false);
}

void NoctuaryEditor::showModDrag(juce::Point<int> screenFrom, juce::Point<int> screenTo,
                                     const juce::String& name, juce::Colour c)
{
    dragOverlay_.active = true;
    dragOverlay_.from = dragOverlay_.getLocalPoint(nullptr, screenFrom.toFloat());
    dragOverlay_.to = dragOverlay_.getLocalPoint(nullptr, screenTo.toFloat());
    dragOverlay_.name = name;
    dragOverlay_.colour = c;
    dragOverlay_.target = {};
    dragOverlay_.targetName = {};
    const int param = cellParamAt(screenTo);
    if (param >= 0) {
        const int ci = cellForParam(static_cast<ParamId>(param));
        const juce::Rectangle<int> b = cellScreenBounds(ci);
        if (!b.isEmpty()) {
            const auto tl = dragOverlay_.getLocalPoint(nullptr, b.getTopLeft().toFloat());
            const auto br = dragOverlay_.getLocalPoint(nullptr, b.getBottomRight().toFloat());
            dragOverlay_.target = juce::Rectangle<float>(tl, br);
            dragOverlay_.targetName = paramDesc(static_cast<ParamId>(param)).name;
        }
    }
    dragOverlay_.setVisible(true);
    dragOverlay_.toFront(false);
    dragOverlay_.repaint();
}

void NoctuaryEditor::hideModDrag()
{
    if (!dragOverlay_.active) return;
    dragOverlay_.active = false;
    dragOverlay_.repaint();
}

// ---------------------------------------------------------------- the route's depth, as text

float NoctuaryEditor::routeDepth(ModSource src, ParamId target) const
{
    const ModMatrix& m = proc_.engine().modMatrix();
    for (int i = 0; i < m.count(); ++i)
        if (m.route(i).source == src && m.route(i).target == target) return m.route(i).depth;
    return 0.0f;
}

/**
 * @brief One route changed, the rest written back exactly as it stands.
 *
 * The matrix has no "set this
 * row" call -- it travels as text everywhere in this program, which is what the preset files and
 * the OSC interface use as well, so there is one form of it and not two.
 *
 * @param m       the matrix as the engine holds it now, read route by route
 * @param src     the source of the one route to change or drop
 * @param target  the target of that route
 * @param depth   the new depth for the matching route, or nullptr to keep the depth it has
 * @param drop    true leaves the matching route out of the text altogether
 * @return        the whole matrix as `source>target:depth[:via][:u]` rows joined with ';', in the
 *                form Engine::setModMatrixText parses
 */
static juce::String matrixTextWith(const ModMatrix& m, ModSource src, ParamId target, const float* depth, bool drop)
{
    juce::String t;
    for (int i = 0; i < m.count(); ++i) {
        const ModRoute& r = m.route(i);
        const bool hit = r.source == src && r.target == target;
        if (hit && drop) continue;
        if (t.isNotEmpty()) t += ";";
        t += juce::String(modSourceName(r.source)) + ">" + paramDesc(r.target).key + ":"
           + juce::String(hit && depth != nullptr ? *depth : r.depth, 3);
        if (r.via != ModSource::None) t += ":" + juce::String(modSourceName(r.via));
        if (r.unipolar) t += r.via != ModSource::None ? ":u" : ":none:u";
    }
    return t;
}

bool NoctuaryEditor::setRouteDepth(ModSource src, ParamId target, float depth)
{
    const juce::String t = matrixTextWith(proc_.engine().modMatrix(), src, target, &depth, false);
    const bool ok = proc_.engine().setModMatrixText(t.toRawUTF8());
    if (ok) { if (mod_) mod_->pullMatrix(); repaint(); }
    return ok;
}

bool NoctuaryEditor::removeRoute(ModSource src, ParamId target)
{
    const juce::String t = matrixTextWith(proc_.engine().modMatrix(), src, target, nullptr, true);
    const bool ok = proc_.engine().setModMatrixText(t.toRawUTF8());
    if (ok) { if (mod_) mod_->pullMatrix(); repaint(); }
    return ok;
}

// ---------------------------------------------------------------- the depth popup

NoctuaryEditor::DepthPopup::DepthPopup(NoctuaryEditor& o, ModSource s, ParamId t, juce::Colour c)
    : owner(o), source(s), target(t), colour(c)
{
    depth.setSliderStyle(juce::Slider::LinearHorizontal);
    depth.setRange(-1.0, 1.0, 0.001);
    depth.setValue(owner.routeDepth(s, t), juce::dontSendNotification);
    depth.setTextBoxStyle(juce::Slider::TextBoxRight, false, 46, 18);
    depth.setNumDecimalPlacesToDisplay(2);
    depth.setDoubleClickReturnValue(true, 0.25);
    depth.setColour(juce::Slider::trackColourId, colour.withAlpha(0.85f));
    depth.setColour(juce::Slider::thumbColourId, colour.brighter(0.3f));
    depth.setTooltip("How much of the target's range this modulator moves. Negative turns it the other way.");
    depth.onValueChange = [this] { owner.setRouteDepth(source, target, static_cast<float>(depth.getValue())); };
    addAndMakeVisible(depth);
    remove.setTooltip("Take this route away again");
    remove.onClick = [this] {
        owner.removeRoute(source, target);
        owner.hideDepthPopup();      // deletes this: nothing may touch it afterwards
    };
    addAndMakeVisible(remove);
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
    g.setColour(ui::card.withAlpha(0.97f));
    g.fillRoundedRectangle(r, 6.0f);
    g.setColour(colour.withAlpha(0.9f));
    g.drawRoundedRectangle(r, 6.0f, 1.4f);
    g.setColour(colour.brighter(0.45f));
    g.setFont(ui::title(10.5f));
    g.drawText(juce::String(modSourceName(source)).toUpperCase() + "  " + juce::String(juce::CharPointer_UTF8("\xe2\x86\x92")) + "  "
                   + paramDesc(target).name,
               getLocalBounds().removeFromTop(18).reduced(8, 0), juce::Justification::centredLeft, false);
}

void NoctuaryEditor::DepthPopup::resized()
{
    auto r = getLocalBounds().reduced(8, 6);
    r.removeFromTop(16);
    remove.setBounds(r.removeFromRight(22));
    r.removeFromRight(4);
    depth.setBounds(r);
}

void NoctuaryEditor::showDepthPopup(ModSource src, ParamId target, juce::Colour c, juce::Point<int> screenAt)
{
    hideDepthPopup();
    depthPopup_ = std::make_unique<DepthPopup>(*this, src, target, c);
    addAndMakeVisible(*depthPopup_);
    depthPopup_->setTransform(juce::AffineTransform::scale(scale_));
    const auto at = depthPopup_->getLocalPoint(nullptr, screenAt.toFloat());
    // Beside the knob, and inside the panel: a popup half off the edge is worse than none.
    const int w = depthPopup_->getWidth(), h = depthPopup_->getHeight();
    int x = juce::roundToInt(at.x) + 14, y = juce::roundToInt(at.y) - h / 2;
    x = juce::jlimit(4, juce::jmax(4, designW_ - w - 4), x);
    y = juce::jlimit(4, juce::jmax(4, designH_ - h - 4), y);
    depthPopup_->setTopLeftPosition(x, y);
    depthPopup_->toFront(false);
}

void NoctuaryEditor::hideDepthPopup()
{
    depthPopup_.reset();
}

