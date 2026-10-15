/**
 * @file JourneyTimeline.cpp
 * @brief The journeys over time (JourneyTimeline.h).
 */
#include "JourneyTimeline.h"

#include "NoctuaryLookAndFeel.h"

#include <algorithm>
#include <cmath>

namespace {

/** @brief A step's average length, its crossfade and its dwell. @param s the step @return seconds */
double stepSeconds(const ambient::JourneyStep& s) { return 0.5 * (s.fadeLo + s.fadeHi) + 0.5 * (s.dwellLo + s.dwellHi); }

/** @brief A journey's average length. @param j the journey @return seconds */
double journeySeconds(const ambient::Journey& j)
{
    double t = 0.0;
    for (const ambient::JourneyStep& s : j.steps) t += stepSeconds(s);
    return t;
}

/** @brief Whether two journeys are the same row of the same presets. @param a one @param b the other @return true then */
bool sameJourney(const ambient::Journey& a, const ambient::Journey& b)
{
    if (a.name != b.name || a.steps.size() != b.steps.size()) return false;
    for (size_t i = 0; i < a.steps.size(); ++i)
        if (a.steps[i].preset != b.steps[i].preset) return false;
    return true;
}

} // namespace

JourneyTimeline::JourneyTimeline(NoctuaryProcessor& proc, std::function<bool(ambient::Journey&)> chosen, std::function<void(int)> startAt)
    : proc_(proc), chosen_(std::move(chosen)), startAt_(std::move(startAt))
{
    copy_.setTooltip("Takes the journey above into the one being written, to change here and save from the Morph section's journey row");
    copy_.onClick = [this] {
        ambient::Journey j;
        if (!upper(j)) return;
        j.name = j.name.empty() ? std::string("My Journey") : j.name + " (mine)";
        proc_.setJourney(j);
        repaint();
    };
    addAndMakeVisible(copy_);
    startTimerHz(2);
}

void JourneyTimeline::timerCallback()
{
    if (isShowing()) repaint();
}

bool JourneyTimeline::upper(ambient::Journey& out) const
{
    if (proc_.journeyPlayer().running()) {
        out = proc_.journeyPlayer().journey();
        return true;
    }
    return chosen_ && chosen_(out);
}

bool JourneyTimeline::hasDraft() const
{
    const ambient::Journey& d = proc_.journey();
    if (d.steps.empty()) return false;
    ambient::Journey u;
    return !upper(u) || !sameJourney(u, d);
}

juce::Rectangle<float> JourneyTimeline::band(int which) const
{
    auto r = getLocalBounds().toFloat().reduced(18.0f, 0.0f).withTrimmedTop(28.0f).withTrimmedBottom(6.0f);
    if (!hasDraft()) return which == 0 ? r.withTrimmedBottom(20.0f) : juce::Rectangle<float>();
    const float h = (r.getHeight() - 20.0f - 22.0f) / 2.0f;   // two bands, the hours under the first, a title over the second
    return which == 0 ? r.withHeight(h) : r.withTrimmedTop(h + 20.0f + 22.0f);
}

std::vector<JourneyTimeline::Block> JourneyTimeline::blocks(const ambient::Journey& j, juce::Rectangle<float> r)
{
    std::vector<Block> out;
    const double total = journeySeconds(j);
    if (total <= 0.0) return out;
    double t = 0.0;
    for (const ambient::JourneyStep& s : j.steps) {
        Block b;
        b.x0 = r.getX() + r.getWidth() * static_cast<float>(t / total);
        t += 0.5 * (s.fadeLo + s.fadeHi);
        b.xg = r.getX() + r.getWidth() * static_cast<float>(t / total);
        t += 0.5 * (s.dwellLo + s.dwellHi);
        b.x1 = r.getX() + r.getWidth() * static_cast<float>(t / total);
        out.push_back(b);
    }
    return out;
}

void JourneyTimeline::drawBand(juce::Graphics& g, const ambient::Journey& j, juce::Rectangle<float> r, int lit, double done, bool faint) const
{
    const juce::Colour hue = ui::morphCol;
    const auto bs = blocks(j, r);
    g.setFont(juce::FontOptions(11.0f));
    for (size_t i = 0; i < bs.size(); ++i) {
        const Block& b = bs[i];
        const bool now = static_cast<int>(i) == lit;
        const juce::Colour c = now ? hue : (faint ? ui::card : ui::card.brighter(0.25f));
        juce::Path ramp;   // the crossfade: rising into the step
        ramp.startNewSubPath(b.x0, r.getBottom());
        ramp.lineTo(b.xg, r.getY());
        ramp.lineTo(b.xg, r.getBottom());
        ramp.closeSubPath();
        g.setColour(c.withAlpha(now ? 0.35f : 0.6f));
        g.fillPath(ramp);
        g.setColour(c.withAlpha(now ? 0.35f : 0.85f));
        g.fillRect(juce::Rectangle<float>::leftTopRightBottom(b.xg, r.getY(), b.x1 - 1.0f, r.getBottom()));
        if (now) {
            g.setColour(hue.withAlpha(0.85f));
            g.fillRect(juce::Rectangle<float>::leftTopRightBottom(b.xg, r.getY(), b.xg + (b.x1 - 1.0f - b.xg) * static_cast<float>(done), r.getBottom()));
        }
        if (static_cast<int>(i) == dropAt_ && r == band(1)) {   // where a moved block would land
            g.setColour(ui::accent);
            g.fillRect(b.x0 - 1.5f, r.getY() - 2.0f, 3.0f, r.getHeight() + 4.0f);
        }
        g.setColour(now ? ui::bg0 : ui::text.withAlpha(faint ? 0.55f : 0.85f));
        g.drawFittedText(j.steps[i].preset, juce::Rectangle<float>(b.xg + 4.0f, r.getY() + 2.0f, std::max(0.0f, b.x1 - b.xg - 8.0f), r.getHeight() - 4.0f).toNearestInt(),
                         juce::Justification::centredLeft, 2, 0.7f);
    }
}

void JourneyTimeline::paint(juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setColour(ui::group.withAlpha(0.96f));
    g.fillRoundedRectangle(b, 6.0f);
    g.setColour(ui::cardEdge);
    g.drawRoundedRectangle(b.reduced(0.5f), 6.0f, 1.0f);
    const ambient::JourneyPlayer& player = proc_.journeyPlayer();
    const bool running = player.running();
    ambient::Journey j;
    const bool any = upper(j);
    const double total = journeySeconds(j);
    g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    g.setColour(running ? ui::morphCol : ui::dim);
    if (!any) {
        g.drawText("Choose a journey in the Morph section's journey row: its steps are drawn here over time, the one under way lit while it runs.",
                   b.reduced(18.0f, 4.0f).withHeight(20.0f), juce::Justification::centredLeft);
    } else {
        juce::String head = juce::String(j.name) + "   " + juce::String(static_cast<int>(j.steps.size())) + " steps, about "
                            + juce::String(ambient::Journey::timeText(total)) + (j.cyclic ? ", then again" : "");
        if (running) head << "   -- step " << (player.step() + 1) << ", " << juce::String(ambient::Journey::timeText(player.remaining())) << " left in it (click a step: there)";
        else head << "   (not under way: a click on a step starts it there)";
        g.drawText(head, b.reduced(18.0f, 4.0f).withHeight(20.0f).withTrimmedRight(120.0f), juce::Justification::centredLeft);
        const auto r0 = band(0);
        double done = 0.0;
        if (running && player.step() >= 0 && player.step() < static_cast<int>(j.steps.size())) {
            const ambient::JourneyStep& s = j.steps[static_cast<size_t>(player.step())];
            done = juce::jlimit(0.0, 1.0, 1.0 - player.remaining() / std::max(0.5 * (s.dwellLo + s.dwellHi), 1.0));
        }
        drawBand(g, j, r0, running ? player.step() : -1, done, !running);
        g.setColour(ui::dim);   // the hours under it
        g.setFont(juce::FontOptions(11.0f));
        const double every = total > 4.0 * 3600.0 ? 3600.0 : (total > 3600.0 ? 1800.0 : 600.0);
        for (double h = 0.0; total > 0.0 && h <= total; h += every) {
            const float x = r0.getX() + r0.getWidth() * static_cast<float>(h / total);
            g.drawVerticalLine(juce::roundToInt(x), r0.getBottom(), r0.getBottom() + 4.0f);
            g.drawText(juce::String(ambient::Journey::timeText(h)), juce::Rectangle<float>(x - 30.0f, r0.getBottom() + 4.0f, 60.0f, 14.0f), juce::Justification::centred);
        }
    }
    // The one being written, to change.
    if (hasDraft()) {
        const ambient::Journey& d = proc_.journey();
        const auto r1 = band(1);
        g.setColour(ui::accent);
        g.setFont(juce::FontOptions(12.5f, juce::Font::bold));
        g.drawText("Being written: " + juce::String(d.name.empty() ? "your journey" : d.name) + ", " + juce::String(static_cast<int>(d.steps.size()))
                       + " steps, about " + juce::String(ambient::Journey::timeText(journeySeconds(d)))
                       + "   -- drag an edge: how long it stays; the ramp: how long it fades in; the block: where it goes; right-click: remove",
                   juce::Rectangle<float>(r1.getX(), r1.getY() - 21.0f, r1.getWidth(), 18.0f), juce::Justification::centredLeft);
        drawBand(g, d, r1, -1, 0.0, false);
    }
}

void JourneyTimeline::resized() { copy_.setBounds(getWidth() - 118, 5, 104, 20); }

int JourneyTimeline::draftAt(juce::Point<float> p, Drag& drag) const
{
    drag = Drag::None;
    const auto r = band(1);
    if (r.isEmpty() || !r.expanded(4.0f).contains(p)) return -1;
    const auto bs = blocks(proc_.journey(), r);
    for (size_t i = 0; i < bs.size(); ++i) {
        const Block& b = bs[i];
        if (p.x < b.x0 || p.x > b.x1 + 4.0f) continue;
        if (p.x >= b.x1 - 6.0f) drag = Drag::Dwell;
        else if (p.x < b.xg) drag = Drag::Fade;
        else drag = Drag::Move;
        return static_cast<int>(i);
    }
    return -1;
}

void JourneyTimeline::mouseMove(const juce::MouseEvent& e)
{
    Drag d = Drag::None;
    draftAt(e.position, d);
    setMouseCursor(d == Drag::Dwell || d == Drag::Fade ? juce::MouseCursor::LeftRightResizeCursor
                   : d == Drag::Move                    ? juce::MouseCursor::DraggingHandCursor
                                                        : juce::MouseCursor::NormalCursor);
}

void JourneyTimeline::mouseDown(const juce::MouseEvent& e)
{
    // The journey above: a click on a step goes there (starting the one chosen when none runs).
    const auto r0 = band(0);
    ambient::Journey j;
    if (r0.contains(e.position) && upper(j)) {
        const auto bs = blocks(j, r0);
        for (size_t i = 0; i < bs.size(); ++i)
            if (e.position.x >= bs[i].x0 && e.position.x < bs[i].x1) {
                if (!proc_.journeyPlayer().running()) {
                    if (startAt_) startAt_(static_cast<int>(i));
                } else {
                    proc_.skipJourneyTo(static_cast<int>(i));
                }
                repaint();
                return;
            }
        return;
    }
    // The one being written.
    Drag d = Drag::None;
    const int i = draftAt(e.position, d);
    if (i < 0) return;
    if (e.mods.isPopupMenu()) {
        juce::PopupMenu m;
        m.addItem("Remove " + juce::String(proc_.journey().steps[static_cast<size_t>(i)].preset), [this, i] {
            ambient::Journey dj = proc_.journey();
            dj.steps.erase(dj.steps.begin() + i);
            proc_.setJourney(dj);
            repaint();
        });
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withTargetScreenArea(juce::Rectangle<int>(e.getScreenX(), e.getScreenY(), 1, 1)));
        return;
    }
    drag_ = d;
    dragStep_ = i;
    dragX_ = e.position.x;
    before_ = proc_.journey().steps[static_cast<size_t>(i)];
    perPixel_ = journeySeconds(proc_.journey()) / std::max(1.0f, band(1).getWidth());
    startTimerHz(10);
}

void JourneyTimeline::mouseDrag(const juce::MouseEvent& e)
{
    if (drag_ == Drag::None || dragStep_ < 0 || dragStep_ >= static_cast<int>(proc_.journey().steps.size())) return;
    ambient::Journey dj = proc_.journey();
    ambient::JourneyStep& s = dj.steps[static_cast<size_t>(dragStep_)];
    const double moved = (e.position.x - dragX_) * perPixel_;   // seconds, on the scale the drag began with
    if (drag_ == Drag::Dwell || drag_ == Drag::Fade) {
        const double lo = drag_ == Drag::Dwell ? before_.dwellLo : before_.fadeLo, hi = drag_ == Drag::Dwell ? before_.dwellHi : before_.fadeHi;
        const double mean = 0.5 * (lo + hi), least = drag_ == Drag::Dwell ? 30.0 : 0.0;
        const double now = std::max(least, mean + (drag_ == Drag::Fade ? -moved : moved));   // the ramp: dragged left, a longer fade
        const double k = mean > 0.0 ? now / mean : 1.0;   // the span kept in proportion
        const double newLo = mean > 0.0 ? lo * k : now, newHi = mean > 0.0 ? hi * k : now;
        if (drag_ == Drag::Dwell) { s.dwellLo = newLo; s.dwellHi = newHi; }
        else { s.fadeLo = newLo; s.fadeHi = newHi; }
        proc_.setJourney(dj);
    } else {   // the block: where it would land
        Drag ignored = Drag::None;
        dropAt_ = draftAt({ e.position.x, band(1).getCentreY() }, ignored);
    }
    repaint();
}

void JourneyTimeline::mouseUp(const juce::MouseEvent&)
{
    if (drag_ == Drag::Move && dropAt_ >= 0 && dropAt_ != dragStep_ && dragStep_ < static_cast<int>(proc_.journey().steps.size())) {
        ambient::Journey dj = proc_.journey();
        const ambient::JourneyStep s = dj.steps[static_cast<size_t>(dragStep_)];
        dj.steps.erase(dj.steps.begin() + dragStep_);
        dj.steps.insert(dj.steps.begin() + std::min(dropAt_, static_cast<int>(dj.steps.size())), s);
        proc_.setJourney(dj);
    }
    drag_ = Drag::None;
    dragStep_ = -1;
    dropAt_ = -1;
    startTimerHz(2);
    repaint();
}

// ---------------------------------------------------------------- the overlay

JourneyPanel::JourneyPanel(NoctuaryProcessor& proc, std::function<bool(ambient::Journey&)> chosen, std::function<void(int)> startAt)
    : timeline_(proc, std::move(chosen), std::move(startAt))
{
    close_.onClick = [this] { setVisible(false); };
    addAndMakeVisible(timeline_);
    addAndMakeVisible(close_);
    setWantsKeyboardFocus(true);
    setVisible(false);
}

void JourneyPanel::open()
{
    if (auto* p = getParentComponent()) setBounds(p->getLocalBounds());
    setVisible(true);
    toFront(true);
    resized();
    grabKeyboardFocus();
}

juce::Rectangle<int> JourneyPanel::panel() const
{
    const int w = std::min(1100, getWidth() - 60), h = std::min(300, getHeight() - 160);
    return { (getWidth() - w) / 2, 140, w, h };
}

void JourneyPanel::resized()
{
    auto r = panel().reduced(10);
    auto top = r.removeFromTop(28);
    close_.setBounds(top.removeFromRight(80).reduced(2));
    r.removeFromTop(6);
    timeline_.setBounds(r);
}

void JourneyPanel::paint(juce::Graphics& g)
{
    g.fillAll(ui::bg0.withAlpha(0.55f));
    const auto p = panel().toFloat();
    g.setColour(ui::group);
    g.fillRoundedRectangle(p, 8.0f);
    g.setColour(ui::morphCol.withAlpha(0.6f));
    g.drawRoundedRectangle(p.reduced(0.5f), 8.0f, 1.2f);
    g.setColour(ui::morphCol);
    g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    g.drawText("Journeys over time", panel().reduced(14, 10).withHeight(24), juce::Justification::centredLeft);
}

void JourneyPanel::mouseDown(const juce::MouseEvent& e)
{
    if (!panel().contains(e.getPosition())) setVisible(false);
}

bool JourneyPanel::keyPressed(const juce::KeyPress& key)
{
    if (key.isKeyCode(juce::KeyPress::escapeKey)) {
        setVisible(false);
        return true;
    }
    return false;
}
