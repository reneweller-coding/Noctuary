/**
 * @file EditorViews.cpp
 * @brief The displays that live inside the grid, each drawn from the numbers the engine is using.
 *
 * The panel's grid is knobs in sections, and the room a row's knobs leave over is a display -- what
 * a Pigments-style layout puts there, and the part of the panel that moves. Every one of them is a
 * small Component declared in PluginEditor.h and owned by the editor (m_brainView and its two
 * siblings, m_stageView, m_tuningView, m_coherenceView, m_cosmosView, m_envView, m_filterView,
 * m_source1View .. m_source4View, m_vectorView, m_outputView), placed by the layout into its section
 * row; this file is their paint routines and the few mouse handlers among them that are controls.
 * The frame, the axes and the parameter reader they all draw with are in EditorCommon.h; the wide
 * spectrum strip has a file of its own (EditorSpectrum.cpp).
 *
 * Nothing here is an illustration. The piano roll (BrainView) is the conductor's notes as they
 * sound; the stage (StageView) is every voice at the pan and the plane the engine gave it, with the
 * three planes as lines a hand can drag; the tuning display is the timbre's own roughness curve
 * (Sethares) from the spectrum the conductor judges with; the coherence display is the Kuramoto
 * ring, the Lenia field and the two attractors; the cosmos return is an FFT of the tap; the
 * amplitude envelope carries the loudest voice's level; the filter response is the voice's own
 * arithmetic (FilterCurve); a source's picture is its table in depth, its FM cycle, its clip with
 * the grains that are reading it this instant, or the colour of its noise; the vector square shows
 * the four slot weights the point is making; the loudness strip is the meter an ambient master is
 * judged by.
 *
 * All of it runs on the message thread at the rate each view's timer sets, reads the host's
 * parameter atomics (rawParam) and the engine's display getters, and never blocks: the one costly
 * picture here, the wavetable slab, is drawn into an image once and blitted until the table or the
 * size changes, as the map's cloud is in EditorBrowse.cpp.
 */
#include "EditorCommon.h"

using namespace ambient;



void NoctuaryEditor::BrainView::timerCallback()
{
    if (!isShowing()) return;
    bool now[128] = {};
    m_proc.engine().soundingNotes(now);
    auto& col = m_hist[static_cast<size_t>(m_head)];
    for (int n = 0; n < 128; ++n) {
        col[static_cast<size_t>(n)] = now[n];
        if (now[n]) { m_lo = juce::jmin(m_lo, n - 2); m_hi = juce::jmax(m_hi, n + 2); }
    }
    m_head = (m_head + 1) % Cols;
    repaint();
}

void NoctuaryEditor::BrainView::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced(6.0f);
    g.setColour(ui::CardColour);
    g.fillRoundedRectangle(r, 5.0f);
    g.setColour(ui::CondCol.withAlpha(0.85f));
    g.setFont(ui::title(10.0f));
    g.drawText("NOTES", r.reduced(8.0f, 4.0f), juce::Justification::topLeft);
    const auto plot = r.reduced(8.0f).withTrimmedTop(16.0f);
    const int span = juce::jmax(12, m_hi - m_lo);
    const float rowH = plot.getHeight() / static_cast<float>(span);
    const float colW = plot.getWidth() / static_cast<float>(Cols);
    // octave lines, faint, so the register can be read
    g.setColour(ui::Faint.withAlpha(0.5f));
    for (int n = (m_lo / 12 + 1) * 12; n < m_lo + span; n += 12) {
        const float y = plot.getBottom() - (n - m_lo) * rowH;
        g.drawHorizontalLine(juce::roundToInt(y), plot.getX(), plot.getRight());
    }
    // the roll: newest column at the right; a held note becomes a bar
    for (int k = 0; k < Cols; ++k) {
        const auto& col = m_hist[static_cast<size_t>((m_head + k) % Cols)];
        const float x = plot.getX() + k * colW;
        const float age = static_cast<float>(k) / static_cast<float>(Cols);
        g.setColour(ui::CondCol.withAlpha(0.25f + 0.7f * age));
        for (int n = juce::jmax(0, m_lo); n < juce::jmin(128, m_lo + span); ++n)
            if (col[static_cast<size_t>(n)])
                g.fillRect(x, plot.getBottom() - (n - m_lo + 1) * rowH + rowH * 0.15f, colW + 0.6f, rowH * 0.7f);
    }
    g.setColour(ui::Dim);
    g.setFont(ui::body(9.5f));
    g.drawText(juce::MidiMessage::getMidiNoteName(m_lo, true, true, 4), plot.getX(), plot.getBottom() - 12.0f, 40.0f, 12.0f, juce::Justification::left);
    g.drawText(juce::MidiMessage::getMidiNoteName(m_lo + span, true, true, 4), plot.getX(), plot.getY(), 40.0f, 12.0f, juce::Justification::left);
    // The cascade's aura at the newest edge: the excitation the clock is running on, as a glow
    // that each event lifts and the seconds let down.
    {
        const float ex = juce::jlimit(0.0f, 1.0f, m_proc.engine().brainExcitation() / 1.6f);
        if (ex > 0.02f) {
            juce::ColourGradient glow(ui::Live.withAlpha(0.0f), plot.getRight() - 60.0f, 0.0f, ui::Live.withAlpha(0.35f * ex), plot.getRight(), 0.0f, false);
            g.setGradientFill(glow);
            g.fillRect(plot.withLeft(plot.getRight() - 60.0f));
        }
    }
    // The deja-vu ring, when it is in use: the loop's places as note names, the place the ring
    // stands on lit. A figure that is coming back can be read off before it is heard.
    if (rawParam(m_proc, "brain_dejavu") > 0.0f) {
        const int len = juce::jlimit(1, 16, static_cast<int>(std::lround(rawParam(m_proc, "brain_loop"))));
        const int pos = m_proc.engine().brainRingPos();
        const float cw = 26.0f;
        float x = r.getRight() - 8.0f - cw * len;
        g.setFont(ui::body(9.0f));
        g.setColour(ui::Dim);
        g.drawText("deja vu", x - 46.0f, r.getY() + 4.0f, 44.0f, 12.0f, juce::Justification::centredRight);
        for (int i = 0; i < len; ++i, x += cw) {
            const int n = m_proc.engine().brainRingNote(i);
            const bool here = i == pos;
            g.setColour(here ? ui::Live.withAlpha(0.35f) : ui::CardColour.brighter(0.08f));
            g.fillRoundedRectangle(x, r.getY() + 3.0f, cw - 3.0f, 14.0f, 3.0f);
            g.setColour(here ? ui::Text : ui::Dim);
            g.drawText(n >= 0 ? juce::MidiMessage::getMidiNoteName(n, true, true, 4) : juce::String(juce::CharPointer_UTF8("\xc2\xb7")),
                       juce::roundToInt(x), juce::roundToInt(r.getY()) + 3, juce::roundToInt(cw) - 3, 14, juce::Justification::centred, false);
        }
    }
    // The homeostat's needle, when it is steering: where the lean stands between "more
    // predictable" and "more surprising" than the aim.
    if (rawParam(m_proc, "brain_homeostat") > 0.0f) {
        const float lean = juce::jlimit(-1.0f, 1.0f, m_proc.engine().brainLean());
        const float w = 90.0f, x0 = plot.getRight() - w - 52.0f, y = plot.getBottom() - 8.0f;   // room for the right label
        g.setColour(ui::Track); g.drawLine(x0, y, x0 + w, y, 1.0f);
        g.setColour(ui::Faint); g.drawLine(x0 + w * 0.5f, y - 3.0f, x0 + w * 0.5f, y + 3.0f, 1.0f);
        const float nx = x0 + w * 0.5f * (1.0f + lean);
        g.setColour(ui::Live); g.fillEllipse(nx - 2.5f, y - 2.5f, 5.0f, 5.0f);
        g.setColour(ui::Dim); g.setFont(ui::body(8.5f));
        g.drawText("steady", x0 - 40.0f, y - 6.0f, 38.0f, 12.0f, juce::Justification::centredRight, false);
        g.drawText("surprise", x0 + w + 2.0f, y - 6.0f, 44.0f, 12.0f, juce::Justification::centredLeft, false);
    }
}

// ---------------------------------------------------------------- stage

juce::Rectangle<float> NoctuaryEditor::StageView::plotRect() const
{
    return getLocalBounds().toFloat().reduced(12.0f, 8.0f).withTrimmedTop(14.0f);
}

namespace {
/** @brief The three planes a hand can move on the stage, each the parameter that is its depth. */
struct StagePlane {
    const char* m_key;     ///< the parameter key of the plane's depth, 0 near .. 1 far
    const char* m_label;   ///< the name drawn at the line's right end
};
/**
 * @brief The conductor's, the keys' and the second conductor's plane, in the order lineAt() and
 *        StageView::paint() index them.
 */
const StagePlane StagePlanes[3] = { { "depth", "conductor" }, { "keys_depth", "keys" }, { "brain2_depth", "conductor 2" } };
}

int NoctuaryEditor::StageView::lineAt(juce::Point<int> pos) const
{
    const auto plot = plotRect();
    int best = -1; float bestD = 7.0f;
    for (int i = 0; i < 3; ++i) {
        const float d = rawParam(m_proc, StagePlanes[i].m_key);
        const float y = plot.getBottom() - 8.0f - d * (plot.getHeight() - 16.0f);
        const float dist = std::abs(static_cast<float>(pos.y) - y);
        if (dist < bestD) { bestD = dist; best = i; }
    }
    return best;
}

void NoctuaryEditor::StageView::mouseMove(const juce::MouseEvent& e)
{
    const int was = m_hoverLine;
    m_hoverLine = lineAt(e.getPosition());
    setMouseCursor(m_hoverLine >= 0 ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
    if (was != m_hoverLine) repaint();
}

void NoctuaryEditor::StageView::mouseDown(const juce::MouseEvent& e)
{
    m_dragLine = lineAt(e.getPosition());
    if (m_dragLine < 0) return;
    if (m_onUndo) m_onUndo(juce::String(StagePlanes[m_dragLine].m_label) + " depth");
    if (auto* p = m_proc.m_apvts.getParameter(StagePlanes[m_dragLine].m_key)) p->beginChangeGesture();
    mouseDrag(e);
}

void NoctuaryEditor::StageView::mouseDrag(const juce::MouseEvent& e)
{
    if (m_dragLine < 0) return;
    const auto plot = plotRect();
    const float d = juce::jlimit(0.0f, 1.0f, (plot.getBottom() - 8.0f - static_cast<float>(e.getPosition().y)) / juce::jmax(1.0f, plot.getHeight() - 16.0f));
    if (auto* p = m_proc.m_apvts.getParameter(StagePlanes[m_dragLine].m_key)) p->setValueNotifyingHost(p->convertTo0to1(d));
    repaint();
}

void NoctuaryEditor::StageView::mouseUp(const juce::MouseEvent&)
{
    if (m_dragLine < 0) return;
    if (auto* p = m_proc.m_apvts.getParameter(StagePlanes[m_dragLine].m_key)) p->endChangeGesture();
    m_dragLine = -1;
    repaint();
}

void NoctuaryEditor::StageView::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds();
    displayFrame(g, r, "STAGE", ui::VoiceCol);
    const auto plot = plotRect();
    // The planes: where the conductor, the keys and the second conductor stand in the room, each
    // a line a hand can take hold of. The one under the mouse brightens; the one being dragged
    // carries its value.
    for (int i = 0; i < 3; ++i) {
        const float d = rawParam(m_proc, StagePlanes[i].m_key);
        const float y = plot.getBottom() - 8.0f - d * (plot.getHeight() - 16.0f);
        const bool hot = i == m_hoverLine || i == m_dragLine;
        g.setColour((i == 1 ? ui::Live : ui::VoiceCol).withAlpha(hot ? 0.9f : 0.35f));
        g.drawLine(plot.getX() + 26.0f, y, plot.getRight() - 4.0f, y, hot ? 1.6f : 1.0f);
        g.setFont(ui::body(9.0f));
        g.drawText(juce::String(StagePlanes[i].m_label) + (hot ? "  " + juce::String(d, 2) : juce::String()),
                   juce::roundToInt(plot.getRight()) - 96, juce::roundToInt(y) - 11, 92, 10, juce::Justification::centredRight, false);
    }
    // The room: near plane at the bottom, far at the top, faint depth lines between.
    g.setColour(ui::Track.withAlpha(0.5f));
    for (int i = 1; i < 4; ++i) g.drawHorizontalLine(juce::roundToInt(plot.getY() + plot.getHeight() * i / 4.0f), plot.getX(), plot.getRight());
    g.drawVerticalLine(juce::roundToInt(plot.getCentreX()), plot.getY(), plot.getBottom());
    g.setColour(ui::Faint); g.setFont(ui::body(9.0f));
    g.drawText("far", plot.withHeight(10.0f), juce::Justification::topLeft, false);
    g.drawText("near", plot.withTrimmedTop(plot.getHeight() - 10.0f), juce::Justification::bottomLeft, false);
    g.drawText("L", plot.withTrimmedTop(plot.getHeight() - 10.0f).withTrimmedLeft(24.0f), juce::Justification::bottomLeft, false);
    g.drawText("R", plot.withTrimmedTop(plot.getHeight() - 10.0f), juce::Justification::bottomRight, false);
    // the listener
    g.setColour(ui::Text.withAlpha(0.5f));
    g.fillEllipse(plot.getCentreX() - 3.0f, plot.getBottom() - 4.0f, 6.0f, 6.0f);

    ambient::Engine::VoiceStage vs[ambient::Engine::MaxVoices];
    const int n = m_proc.engine().voiceStage(vs, ambient::Engine::MaxVoices);
    // Dots glide towards their voices (the picture is about the slow breathing of the planes,
    // not the control blocks); a voice that has gone fades out.
    for (auto& d : m_dots) d.m_on = false;
    for (int i = 0; i < n; ++i) {
        const auto& v = vs[i];
        Dot* d = nullptr;
        for (auto& c : m_dots) if (c.m_note == v.m_note && !c.m_on) { d = &c; break; }
        if (d == nullptr) for (auto& c : m_dots) if (c.m_note < 0) { d = &c; d->m_x = v.m_pan; d->m_y = v.m_distance; d->m_r = 0.0f; break; }
        if (d == nullptr) continue;
        d->m_on = true; d->m_note = v.m_note;
        d->m_x += (v.m_pan - d->m_x) * 0.2f;
        d->m_y += (v.m_distance - d->m_y) * 0.2f;
        d->m_r += (juce::jlimit(0.0f, 1.0f, v.m_level) - d->m_r) * 0.3f;
        const float x = plot.getCentreX() + d->m_x * plot.getWidth() * 0.46f;
        const float y = plot.getBottom() - 8.0f - d->m_y * (plot.getHeight() - 16.0f);
        const float rad = 3.0f + 9.0f * d->m_r;
        const juce::Colour col = v.m_owner == 1 ? ui::VoiceCol : ui::Live;
        g.setColour(col.withAlpha(0.18f + 0.25f * d->m_r));
        g.fillEllipse(x - rad * 1.8f, y - rad * 1.8f, rad * 3.6f, rad * 3.6f);
        g.setColour(col.withAlpha(0.55f + 0.45f * (1.0f - d->m_y)));
        g.fillEllipse(x - rad, y - rad, 2.0f * rad, 2.0f * rad);
        g.setColour(ui::Text.withAlpha(0.75f)); g.setFont(ui::body(9.0f));
        g.drawText(juce::MidiMessage::getMidiNoteName(v.m_note, true, true, 4), juce::roundToInt(x) - 16, juce::roundToInt(y - rad) - 12, 32, 11, juce::Justification::centred, false);
    }
    for (auto& d : m_dots) if (!d.m_on) d.m_note = -1;
    g.setColour(ui::Dim); g.setFont(ui::body(10.0f));
    g.drawText(juce::String(n) + (n == 1 ? " voice" : " voices") + "   brain " + juce::String(juce::CharPointer_UTF8("\xe2\x97\x8f")) + " keys " + juce::String(juce::CharPointer_UTF8("\xe2\x97\x8f")),
               r.reduced(9, 5), juce::Justification::topRight, false);
}

// ---------------------------------------------------------------- tuning

void NoctuaryEditor::TuningView::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds();
    displayFrame(g, r, "TUNING", ui::CondCol);
    const auto plot = r.toFloat().reduced(12.0f, 8.0f).withTrimmedTop(16.0f).withTrimmedBottom(14.0f);
    // The timbre's roughness against itself, swept across the octave (Sethares): where it dips is
    // where its own scale lies. Drawn from the same template the conductor judges with.
    const ambient::BrainSpectrum& sp = m_proc.engine().brainSpectrum();
    const double base = 261.6255653005986;
    float lo = 1.0e9f, hi = -1.0e9f;
    if (sp.m_count > 0) {
        for (int k = 0; k < Points; ++k) {
            const double cents = k * 1200.0 / (Points - 1);
            const float v = static_cast<float>(ambient::spectralRoughness(base, base * std::pow(2.0, cents / 1200.0), sp));
            m_curve[k] += (v - m_curve[k]) * 0.5f;
            lo = juce::jmin(lo, m_curve[k]); hi = juce::jmax(hi, m_curve[k]);
        }
    }
    auto xOf = [&](double cents) { return plot.getX() + static_cast<float>(cents / 1200.0) * plot.getWidth(); };
    // The scale's degrees, as ticks: they should sit in the dips when the scale is the timbre's own.
    const ambient::FixedScale& sc = m_proc.engine().scale();
    for (int i = 0; i < sc.m_count; ++i) {
        const double cents = 1200.0 * std::log2(sc.m_ratios[i]);
        if (cents < 0.0 || cents > 1200.0) continue;
        const float x = xOf(cents);
        g.setColour(ui::CondCol.withAlpha(i == 0 ? 0.7f : 0.4f));
        g.drawLine(x, plot.getY(), x, plot.getBottom(), 1.0f);
    }
    if (sp.m_count > 0 && hi > lo + 1.0e-9f) {
        juce::Path p;
        for (int k = 0; k < Points; ++k) {
            const float t = (m_curve[k] - lo) / (hi - lo);
            const float x = plot.getX() + plot.getWidth() * k / static_cast<float>(Points - 1);
            const float y = plot.getBottom() - t * plot.getHeight();   // rough is high; the dips are the scale
            if (k == 0) p.startNewSubPath(x, y); else p.lineTo(x, y);
        }
        juce::Path fill(p);
        fill.lineTo(plot.getRight(), plot.getBottom()); fill.lineTo(plot.getX(), plot.getBottom()); fill.closeSubPath();
        g.setColour(ui::Accent.withAlpha(0.12f));
        g.fillPath(fill);
        g.setColour(ui::Accent.withAlpha(0.85f));
        g.strokePath(p, juce::PathStrokeType(1.4f));
    } else {
        g.setColour(ui::Faint); g.setFont(ui::body(11.0f));
        g.drawText("roughness curve -- nothing sounding", plot, juce::Justification::centred, false);
    }
    // Axis: the just intervals as landmarks, so a dip can be named.
    g.setColour(ui::Faint); g.setFont(ui::body(9.0f));
    struct Mark { double m_cents; const char* m_name; };
    static const Mark Marks[] = { { 0.0, "1/1" }, { 386.3, "5/4" }, { 498.0, "4/3" }, { 702.0, "3/2" }, { 884.4, "5/3" }, { 1200.0, "2/1" } };
    for (const auto& m : Marks) g.drawText(m.m_name, juce::roundToInt(xOf(m.m_cents)) - 14, juce::roundToInt(plot.getBottom()) + 1, 28, 11, juce::Justification::centred, false);
    // The words: the key the conductor has found, the comma's offset, the tide.
    const ambient::KeyEstimate key = m_proc.engine().brainKey();
    juce::String txt = key.m_key >= 0 && key.m_confidence > 0.0f
        ? "key " + juce::MidiMessage::getMidiNoteName(key.tonic(), true, false, 0) + (key.m_modal ? juce::String(" (scale)") : juce::String(key.minor() ? " minor" : " major")) + "  r " + juce::String(key.m_confidence, 2)
        : juce::String("no key yet");
    if (rawParam(m_proc, "purity_adapt") > 0.0f) txt += "   comma " + juce::String(m_proc.engine().commaCents(), 1) + " ct";
    if (rawParam(m_proc, "tide") > 0.0f) txt += "   tide " + juce::String(m_proc.engine().tideNow(), 1) + " ct";
    txt += "   " + juce::String(sc.m_name);
    g.setColour(ui::Dim); g.setFont(ui::body(10.0f));
    g.drawText(txt, r.reduced(9, 5), juce::Justification::topRight, false);
}

// ---------------------------------------------------------------- coherence

void NoctuaryEditor::CoherenceView::timerCallback()
{
    if (!isShowing()) return;
    if (m_proc.engine().chaosSteps() > 0) {
        m_lorenzTrail.push_back({ m_proc.engine().chaosOut(0), m_proc.engine().chaosOut(1) });
        m_rosslerTrail.push_back({ m_proc.engine().chaosOut(3), m_proc.engine().chaosOut(4) });
        if (m_lorenzTrail.size() > 400) m_lorenzTrail.erase(m_lorenzTrail.begin());
        if (m_rosslerTrail.size() > 400) m_rosslerTrail.erase(m_rosslerTrail.begin());
    }
    repaint();
}

void NoctuaryEditor::CoherenceView::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds();
    displayFrame(g, r, "COHERENCE", ui::CondCol);
    const auto plot = r.toFloat().reduced(12.0f, 8.0f).withTrimmedTop(14.0f);
    const float third = plot.getWidth() / 3.0f;
    // Left: the Kuramoto ring. Four points on a circle at their phases; lock and they bunch.
    {
        const auto box = plot.withWidth(third).reduced(6.0f);
        const float rad = juce::jmin(box.getWidth(), box.getHeight()) * 0.42f;
        const auto c = box.getCentre();
        g.setColour(ui::Track); g.drawEllipse(c.x - rad, c.y - rad, 2.0f * rad, 2.0f * rad, 1.0f);
        for (int i = 0; i < 4; ++i) {
            const float ph = m_proc.engine().coherencePhase(i);
            const juce::Point<float> p(c.x + rad * std::cos(ph), c.y - rad * std::sin(ph));
            g.setColour(ui::CondCol.withAlpha(0.35f)); g.drawLine(c.x, c.y, p.x, p.y, 1.0f);
            g.setColour(ui::CondCol); g.fillEllipse(p.x - 3.5f, p.y - 3.5f, 7.0f, 7.0f);
        }
        g.setColour(ui::Dim); g.setFont(ui::body(9.5f));
        g.drawText("kuramoto ring", box.withHeight(11.0f), juce::Justification::centredTop, false);
    }
    // Middle: the Lenia field, grey by value; asleep until a route reads it.
    {
        const auto box = plot.withX(plot.getX() + third).withWidth(third).reduced(6.0f);
        const int n = ambient::Engine::LeniaSize;
        const float cell = juce::jmin(box.getWidth(), box.getHeight() - 12.0f) / static_cast<float>(n);
        const float x0 = box.getCentreX() - cell * n * 0.5f, y0 = box.getY() + 12.0f;
        if (m_proc.engine().leniaSteps() > 0) {
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x) {
                    const float v = m_proc.engine().leniaCell(x, y);
                    if (v < 0.02f) continue;
                    g.setColour(ui::BackCol.withAlpha(0.15f + 0.85f * v));
                    g.fillRect(x0 + x * cell, y0 + y * cell, cell, cell);
                }
        } else {
            g.setColour(ui::Faint); g.setFont(ui::body(9.5f));
            g.drawText("asleep: route lenia1..4 to wake it", juce::Rectangle<float>(x0, y0, cell * n, cell * n), juce::Justification::centred, false);
        }
        g.setColour(ui::Track.withAlpha(0.6f)); g.drawRect(x0, y0, cell * n, cell * n, 1.0f);
        g.setColour(ui::Dim); g.setFont(ui::body(9.5f));
        g.drawText("lenia " + juce::String(n) + " x " + juce::String(n), box.withHeight(11.0f), juce::Justification::centredTop, false);
    }
    // Right: the attractors -- their orbits in x and y, the butterfly and the spiral, from where
    // they have been in the last forty seconds; and the six readings as bars beneath.
    {
        const auto box = plot.withX(plot.getX() + 2.0f * third).withWidth(third).reduced(6.0f);
        const bool awake = m_proc.engine().chaosSteps() > 0;
        const float orbitH = juce::jmax(0.0f, box.getHeight() * 0.55f - 12.0f);
        const float side = juce::jmin(orbitH, box.getWidth() * 0.46f);
        auto orbit = [&](const std::vector<juce::Point<float>>& trail, float x0, juce::Colour col) {
            const juce::Rectangle<float> sq(x0, box.getY() + 12.0f, side, side);
            g.setColour(ui::Track.withAlpha(0.6f)); g.drawRect(sq, 1.0f);
            if (trail.size() < 2) return;
            juce::Path p;
            for (size_t i = 0; i < trail.size(); ++i) {
                const float x = sq.getX() + (0.5f + 0.5f * trail[i].x) * sq.getWidth();
                const float y = sq.getBottom() - (0.5f + 0.5f * trail[i].y) * sq.getHeight();
                if (i == 0) p.startNewSubPath(x, y); else p.lineTo(x, y);
            }
            g.setColour(col.withAlpha(0.7f));
            g.strokePath(p, juce::PathStrokeType(1.0f));
            const auto& last = trail.back();
            const float x = sq.getX() + (0.5f + 0.5f * last.x) * sq.getWidth(), y = sq.getBottom() - (0.5f + 0.5f * last.y) * sq.getHeight();
            g.setColour(col); g.fillEllipse(x - 2.5f, y - 2.5f, 5.0f, 5.0f);
        };
        if (side > 20.0f) {
            orbit(m_lorenzTrail, box.getX(), ui::CosmosCol);
            orbit(m_rosslerTrail, box.getRight() - side, ui::MorphCol);
        }
        static const char* Names[6] = { "lx", "ly", "lz", "rx", "ry", "rz" };
        const float bw = (box.getWidth() - 10.0f) / 6.0f;
        const float top = box.getY() + 12.0f + (side > 20.0f ? side + 8.0f : 0.0f);
        const float mid = top + (box.getBottom() - 12.0f - top) * 0.5f, half = (box.getBottom() - 12.0f - top) * 0.5f;
        g.setColour(ui::Track); g.drawHorizontalLine(juce::roundToInt(mid), box.getX(), box.getRight());
        for (int k = 0; k < 6; ++k) {
            const float v = awake ? m_proc.engine().chaosOut(k) : 0.0f;
            const float x = box.getX() + 5.0f + k * bw;
            g.setColour((k < 3 ? ui::CosmosCol : ui::MorphCol).withAlpha(awake ? 0.8f : 0.25f));
            g.fillRect(x + 2.0f, v >= 0.0f ? mid - v * half : mid, bw - 4.0f, std::abs(v) * half);
            g.setColour(ui::Dim); g.setFont(ui::body(9.0f));
            g.drawText(Names[k], juce::roundToInt(x), juce::roundToInt(box.getBottom()) - 11, juce::roundToInt(bw), 11, juce::Justification::centred, false);
        }
        g.setColour(ui::Dim); g.setFont(ui::body(9.5f));
        g.drawText(awake ? "lorenz            roessler" : "attractors asleep: route one", box.withHeight(11.0f), juce::Justification::centredTop, false);
    }
}

// ---------------------------------------------------------------- cosmos spectrum

NoctuaryEditor::CosmosView::CosmosView(NoctuaryProcessor& p)
    : m_proc(p), m_re(N), m_im(N), m_window(N), m_fft(std::make_unique<ambient::Fft>(N))
{
    setInterceptsMouseClicks(false, false);
    for (int i = 0; i < N; ++i) m_window[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi * i / N);
    for (float& b : m_bins) b = -90.0f;
    startTimerHz(15);
}

void NoctuaryEditor::CosmosView::timerCallback()
{
    if (!isShowing()) return;
    m_proc.engine().cosmosTap(m_re.data(), N);
    float sq = 0.0f;
    for (int i = 0; i < N; ++i) { sq += m_re[static_cast<size_t>(i)] * m_re[static_cast<size_t>(i)]; m_re[static_cast<size_t>(i)] *= m_window[static_cast<size_t>(i)]; m_im[static_cast<size_t>(i)] = 0.0f; }
    m_silent = sq < 1.0e-9f;
    m_fft->transform(m_re.data(), m_im.data(), false);
    const float sr = static_cast<float>(m_proc.getSampleRate() > 0 ? m_proc.getSampleRate() : 48000.0);
    // Log-spaced bins 30 Hz .. 16 kHz, each the peak of the FFT bins it covers, in dB; fast up,
    // slow down, like a meter.
    for (int b = 0; b < Bins; ++b) {
        const float f0 = 30.0f * std::pow(16000.0f / 30.0f, static_cast<float>(b) / Bins);
        const float f1 = 30.0f * std::pow(16000.0f / 30.0f, static_cast<float>(b + 1) / Bins);
        int k0 = juce::jmax(1, static_cast<int>(f0 / sr * N)), k1 = juce::jmax(k0 + 1, static_cast<int>(f1 / sr * N));
        float peak = 0.0f;
        for (int k = k0; k < juce::jmin(k1, N / 2); ++k) peak = juce::jmax(peak, m_re[static_cast<size_t>(k)] * m_re[static_cast<size_t>(k)] + m_im[static_cast<size_t>(k)] * m_im[static_cast<size_t>(k)]);
        const float db = 10.0f * std::log10(peak / (N * N * 0.0625f) + 1.0e-12f);   // 0 dB = full-scale sine
        m_bins[b] = db > m_bins[b] ? db : m_bins[b] + (db - m_bins[b]) * 0.15f;
    }
    repaint();
}

void NoctuaryEditor::CosmosView::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds();
    displayFrame(g, r, "COSMOS RETURN", ui::CosmosCol);
    const auto plot = r.toFloat().reduced(10.0f, 8.0f).withTrimmedTop(12.0f);
    g.setColour(ui::Track.withAlpha(0.5f));
    for (float hz : { 100.0f, 1000.0f, 10000.0f }) {
        const float t = std::log(hz / 30.0f) / std::log(16000.0f / 30.0f);
        g.drawVerticalLine(juce::roundToInt(plot.getX() + t * plot.getWidth()), plot.getY(), plot.getBottom());
    }
    if (m_silent) {
        g.setColour(ui::Faint); g.setFont(ui::body(11.0f));
        g.drawText(rawParam(m_proc, "cosmos_send") > 0.001f ? "cosmos: nothing sounding" : "cosmos: send is off", plot, juce::Justification::centred, false);
        return;
    }
    juce::Path fill, line;
    const float bw = plot.getWidth() / Bins;
    fill.startNewSubPath(plot.getX(), plot.getBottom());
    for (int b = 0; b < Bins; ++b) {
        const float t = juce::jlimit(0.0f, 1.0f, (m_bins[b] + 72.0f) / 72.0f);   // -72 .. 0 dB
        const float x = plot.getX() + (b + 0.5f) * bw, y = plot.getBottom() - t * plot.getHeight();
        fill.lineTo(x, y);
        if (b == 0) line.startNewSubPath(x, y); else line.lineTo(x, y);
    }
    fill.lineTo(plot.getRight(), plot.getBottom()); fill.closeSubPath();
    g.setColour(ui::CosmosCol.withAlpha(0.18f)); g.fillPath(fill);
    g.setColour(ui::CosmosCol.withAlpha(0.9f)); g.strokePath(line, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(ui::Dim); g.setFont(ui::body(10.0f));
    juce::String legend;
    const float shift = rawParam(m_proc, "cosmos_shift");
    if (std::fabs(shift) > 0.01f) legend += "shift " + juce::String(shift, 1) + " Hz   ";
    if (rawParam(m_proc, "cosmos_res") > 0.001f) legend += "resonator   ";
    if (rawParam(m_proc, "cosmos_smear") > 0.001f && rawParam(m_proc, "cosmos_nebula") > 0.001f) legend += "nebula   ";
    if (rawParam(m_proc, "cosmos_shimmer") > 0.001f) legend += "shimmer";
    g.drawText(legend.trim(), r.reduced(9, 5), juce::Justification::topRight, false);
}

// ---------------------------------------------------------------- amplitude envelope

void NoctuaryEditor::EnvView::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds();
    displayFrame(g, r, "AMP ENVELOPE", ui::VoiceCol);
    const auto plot = r.toFloat().reduced(12.0f, 8.0f).withTrimmedTop(14.0f);
    const float a = rawParam(m_proc, "attack"), d = rawParam(m_proc, "decay"), su = rawParam(m_proc, "sustain"), rl = rawParam(m_proc, "release");
    // Time on a square-root axis so a 60 s attack and a 0.1 s one both read; the hold is a
    // fixed slice, since how long a key is down is not the envelope's business.
    const float hold = juce::jmax(1.0f, 0.25f * (a + d + rl));
    const float total = a + d + hold + rl;
    auto xAt = [&](float t) { return plot.getX() + std::sqrt(juce::jlimit(0.0f, 1.0f, t / total)) * plot.getWidth(); };
    auto yAt = [&](float v) { return plot.getBottom() - juce::jlimit(0.0f, 1.0f, v) * (plot.getHeight() - 4.0f); };
    juce::Path p;
    p.startNewSubPath(xAt(0.0f), yAt(0.0f));
    const int seg = 24;
    for (int i = 1; i <= seg; ++i) { const float t = i / static_cast<float>(seg); p.lineTo(xAt(a * t), yAt(1.0f - std::pow(1.0f - t, 2.2f))); }
    for (int i = 1; i <= seg; ++i) { const float t = i / static_cast<float>(seg); p.lineTo(xAt(a + d * t), yAt(su + (1.0f - su) * std::pow(1.0f - t, 2.2f))); }
    p.lineTo(xAt(a + d + hold), yAt(su));
    for (int i = 1; i <= seg; ++i) { const float t = i / static_cast<float>(seg); p.lineTo(xAt(a + d + hold + rl * t), yAt(su * std::pow(1.0f - t, 2.2f))); }
    g.setColour(ui::Track.withAlpha(0.6f));
    for (float t : { a, a + d, a + d + hold }) g.drawVerticalLine(juce::roundToInt(xAt(t)), plot.getY(), plot.getBottom());
    g.setColour(ui::VoiceCol.withAlpha(0.25f)); g.strokePath(p, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(ui::VoiceCol); g.strokePath(p, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    // the loudest voice's level, live
    const float level = 0.5f * (m_proc.engine().modSource(static_cast<int>(ambient::ModSourceE::AMP)) + 1.0f);
    if (level > 0.001f) {
        g.setColour(ui::Live.withAlpha(0.8f));
        g.drawHorizontalLine(juce::roundToInt(yAt(level)), plot.getX(), plot.getRight());
        g.fillEllipse(plot.getRight() - 5.0f, yAt(level) - 3.0f, 6.0f, 6.0f);
    }
    g.setColour(ui::Faint); g.setFont(ui::body(9.0f));
    g.drawText("A " + juce::String(a, 1) + " s", juce::roundToInt(xAt(0.0f)), juce::roundToInt(plot.getBottom()) - 11, 60, 10, juce::Justification::left, false);
    g.drawText("D " + juce::String(d, 1), juce::roundToInt(xAt(a)) + 2, juce::roundToInt(plot.getBottom()) - 11, 50, 10, juce::Justification::left, false);
    g.drawText("S " + juce::String(su, 2), juce::roundToInt(xAt(a + d)) + 2, juce::roundToInt(plot.getBottom()) - 11, 50, 10, juce::Justification::left, false);
    g.drawText("R " + juce::String(rl, 1) + " s", juce::roundToInt(xAt(a + d + hold)) + 2, juce::roundToInt(plot.getBottom()) - 11, 60, 10, juce::Justification::left, false);
}

void NoctuaryEditor::FilterView::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds();
    displayFrame(g, r, "FILTER RESPONSE", ui::VoiceCol);
    const auto plot = r.toFloat().reduced(10.0f, 8.0f).withTrimmedTop(12.0f);
    drawAxes(g, plot);

    const float sr = static_cast<float>(juce::jmax(8000.0, m_proc.getSampleRate() > 0 ? m_proc.getSampleRate() : 48000.0));
    FilterCurve fc;
    fc.capture(m_proc, sr);

    juce::Path svf, zp, both;
    const int steps = juce::jmax(64, static_cast<int>(plot.getWidth()));
    for (int i = 0; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float hz = 20.0f * std::pow(1000.0f, t);
        // Each branch on its own, and the two combined the way the voice combines the signals.
        // The parallel sum ignores the phase between them, which is the one thing this picture
        // cannot show.
        const float hs = fc.branchFilter(hz);
        const float hz_ = fc.branchZ(hz);
        const float combined = fc.magnitude(hz);
        const float x = plot.getX() + t * plot.getWidth();
        auto db = [](float m) { return 20.0f * std::log10(juce::jmax(m, 1.0e-6f)); };
        if (i == 0) { svf.startNewSubPath(x, yForDb(plot, db(hs))); zp.startNewSubPath(x, yForDb(plot, db(hz_))); both.startNewSubPath(x, yForDb(plot, db(combined))); }
        else        { svf.lineTo(x, yForDb(plot, db(hs)));           zp.lineTo(x, yForDb(plot, db(hz_)));           both.lineTo(x, yForDb(plot, db(combined))); }
    }
    if (fc.m_fOn) { g.setColour(ui::VoiceCol.withAlpha(0.55f)); g.strokePath(svf, juce::PathStrokeType(1.2f)); }
    if (fc.m_zUsed > 0)  { g.setColour(ui::Accent.withAlpha(0.55f));   g.strokePath(zp,  juce::PathStrokeType(1.2f)); }
    g.setColour(ui::Text.withAlpha(0.25f));
    g.strokePath(both, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(ui::Text);
    g.strokePath(both, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    g.setColour(ui::Dim);
    g.setFont(ui::body(10.0f));
    g.drawText(fc.legend(), r.reduced(9, 5), juce::Justification::topRight, false);
}

void NoctuaryEditor::SourceView::mouseDown(const juce::MouseEvent& e)
{
    if (ownEntrance() && entranceBox().contains(e.getPosition())) {   // the entrance picture: open it
        if (auto* ed = findParentComponentOfClass<NoctuaryEditor>()) ed->openSourceEnvelope(m_slot - 1);
        return;
    }
    const int type = static_cast<int>(std::lround(rawParam(m_proc, ("src" + juce::String(m_slot) + "_type").toRawUTF8())));
    if (type == 1 || type == 9) { m_flat = !m_flat; repaint(); }
}

/**
 * A table in depth. Every frame is a line of its cycle, and every step back into the table moves the
 * line up and to the right, so the table stands as one slab the eye reads as a single object -- the
 * way Serum taught everyone to look at a wavetable. The lines are drawn once into an image for as
 * long as the table and the size stay; what is drawn on every tick is the frame at Position, lit in
 * the warm colour of what is sounding, which is the part that moves.
 */
bool NoctuaryEditor::SourceView::paintTable3D(juce::Graphics& g, juce::Rectangle<float> plot, int type, int table, float pos)
{
    // Which table, and whether it has changed since it was sampled. A user table is loaded into the
    // same place as the one before it, so the signature reads a little of the content as well.
    const ambient::CycleTable* ct = nullptr;
    const ambient::Wavetable* wt = nullptr;
    int frames = 0;
    double sig = type * 7919.0 + table * 104729.0;
    if (type == 9) {
        ct = table >= ambient::NumTables - 1 ? m_proc.engine().userCycles() : &ambient::builtinCycleTable(table);
        if (ct != nullptr && !ct->empty()) {
            frames = ct->m_frames;
            for (int f : { 0, frames / 2, frames - 1 }) {
                const float* c = ct->cycle(ambient::CycleTable::Levels - 1, f);   // 32 samples a cycle
                sig += (c[0] + 3.0 * c[7] + 7.0 * c[19]) * (f + 1.0);
            }
        }
    } else {
        wt = table >= ambient::NumTables - 1 ? m_proc.engine().userWavetable() : &ambient::builtinTable(table);
        if (wt != nullptr && wt->m_frames > 0) {
            frames = wt->m_frames;
            for (int f : { 0, frames / 2, frames - 1 })
                for (int h = 0; h < 6; ++h) sig += wt->m_amp[f][h] * (h + 1.0) * (f + 1.0);
        }
    }
    if (frames < 2) return false;
    sig += frames * 31.0;
    const int stride = TablePoints + 1;

    // The table sampled: TablePoints points a cycle, at the level of the mipmap that still holds
    // more detail than the display, the whole table on one scale so a quiet frame looks quiet.
    if (sig != m_tableSig || frames != m_tableFrames) {
        m_tableSig = sig;
        m_tableFrames = frames;
        m_tableCycles.assign(static_cast<size_t>(frames * stride), 0.0f);
        float peak = 1.0e-6f;
        for (int f = 0; f < frames; ++f) {
            for (int i = 0; i <= TablePoints; ++i) {
                const double u = static_cast<double>(i % TablePoints) / TablePoints;
                float v = 0.0f;
                if (ct != nullptr) {
                    v = ct->sample(4, f, u);
                } else {
                    double s = 0.0;
                    for (int h = 0; h < ambient::TablePartials; ++h) {
                        const float a = wt->m_amp[f][h];
                        if (a > 1.0e-5f) s += a * std::sin(juce::MathConstants<double>::twoPi * (h + 1) * u);
                    }
                    v = static_cast<float>(s);
                }
                m_tableCycles[static_cast<size_t>(f * stride + i)] = v;
                peak = juce::jmax(peak, std::fabs(v));
            }
        }
        for (auto& v : m_tableCycles) v /= peak;
        m_tableMesh = {};
    }

    // The slab: the width of a cycle along x, the depth of the table up and to the right.
    // The panels are wide and low, where Serum's is nearly square, so the depth is bounded by the
    // height and the waves are allowed a little over the slab's top edge rather than drawn flat.
    const float depthX = juce::jmin(plot.getWidth() * 0.22f, plot.getHeight() * 1.4f), depthY = plot.getHeight() * 0.34f;
    const float width = plot.getWidth() - depthX;
    const float ampY = (plot.getHeight() - depthY) * 0.54f;
    const float baseY = plot.getBottom() - ampY;
    const auto at = [&](float t, float u, float v) {
        return juce::Point<float>(plot.getX() + t * depthX + u * width, baseY - t * depthY - v * ampY);
    };
    const auto frameT = [&](int f) { return static_cast<float>(f) / static_cast<float>(frames - 1); };

    // The lines, into an image at the pixels the editor's scale will put on the screen.
    const float scale = juce::jmax(1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    const auto bounds = plot.getSmallestIntegerContainer();
    if (!m_tableMesh.isValid() || bounds != m_meshBounds || scale != m_meshScale) {
        m_meshBounds = bounds;
        m_meshScale = scale;
        m_tableMesh = juce::Image(juce::Image::ARGB, juce::jmax(1, juce::roundToInt(bounds.getWidth() * scale)),
                                juce::jmax(1, juce::roundToInt(bounds.getHeight() * scale)), true);
        juce::Graphics mg(m_tableMesh);
        mg.addTransform(juce::AffineTransform::translation(-static_cast<float>(bounds.getX()), -static_cast<float>(bounds.getY())).scaled(scale));
        // The floor: every frame's zero line, the edges of the slab.
        juce::Path floor;
        floor.startNewSubPath(at(0.0f, 0.0f, 0.0f));
        floor.lineTo(at(0.0f, 1.0f, 0.0f));
        floor.lineTo(at(1.0f, 1.0f, 0.0f));
        floor.lineTo(at(1.0f, 0.0f, 0.0f));
        floor.closeSubPath();
        mg.setColour(ui::Track.withAlpha(0.8f));
        mg.strokePath(floor, juce::PathStrokeType(1.0f));
        // At most forty frames: a table of 256 would be a grey wall at this size.
        const int shown = juce::jmin(frames, 40);
        std::vector<int> pick(static_cast<size_t>(shown));
        for (int j = 0; j < shown; ++j) pick[static_cast<size_t>(j)] = juce::roundToInt(j * (frames - 1) / static_cast<double>(shown - 1));
        // Lines across the frames at thirteen places in the cycle, which make the slab a surface.
        for (int c = 0; c <= 12; ++c) {
            const int i = c * TablePoints / 12;
            juce::Path across;
            for (int j = 0; j < shown; ++j) {
                const int f = pick[static_cast<size_t>(j)];
                const auto p = at(frameT(f), static_cast<float>(i) / TablePoints, m_tableCycles[static_cast<size_t>(f * stride + i)]);
                if (j == 0) across.startNewSubPath(p); else across.lineTo(p);
            }
            mg.setColour(ui::VoiceCol.withAlpha(0.11f));
            mg.strokePath(across, juce::PathStrokeType(0.8f));
        }
        // The frames, back to front, the ones further back fainter.
        for (int j = shown - 1; j >= 0; --j) {
            const int f = pick[static_cast<size_t>(j)];
            const float t = frameT(f);
            juce::Path line;
            for (int i = 0; i <= TablePoints; ++i) {
                const auto p = at(t, static_cast<float>(i) / TablePoints, m_tableCycles[static_cast<size_t>(f * stride + i)]);
                if (i == 0) line.startNewSubPath(p); else line.lineTo(p);
            }
            mg.setColour(ui::VoiceCol.withAlpha(0.16f + 0.36f * (1.0f - t)));
            mg.strokePath(line, juce::PathStrokeType(1.0f));
        }
    }
    g.drawImage(m_tableMesh, bounds.toFloat());

    // The frame at Position, between two frames when it is between them, lit: its outline, a glow,
    // the area down to its zero line, and its place on the floor.
    const float x = pos * static_cast<float>(frames - 1);
    const int f0 = juce::jlimit(0, frames - 1, static_cast<int>(x));
    const int f1 = juce::jmin(f0 + 1, frames - 1);
    const float fr = x - static_cast<float>(f0);
    juce::Path lit, fill;
    fill.startNewSubPath(at(pos, 0.0f, 0.0f));
    for (int i = 0; i <= TablePoints; ++i) {
        const float a = m_tableCycles[static_cast<size_t>(f0 * stride + i)], b = m_tableCycles[static_cast<size_t>(f1 * stride + i)];
        const auto p = at(pos, static_cast<float>(i) / TablePoints, a + fr * (b - a));
        if (i == 0) lit.startNewSubPath(p); else lit.lineTo(p);
        fill.lineTo(p);
    }
    fill.lineTo(at(pos, 1.0f, 0.0f));
    fill.closeSubPath();
    g.setColour(ui::Live.withAlpha(0.13f));
    g.fillPath(fill);
    g.setColour(ui::Live.withAlpha(0.55f));
    g.drawLine(juce::Line<float>(at(pos, 0.0f, 0.0f), at(pos, 1.0f, 0.0f)), 1.0f);
    g.setColour(ui::Live.withAlpha(0.22f));
    g.strokePath(lit, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(ui::Live);
    g.strokePath(lit, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    return true;
}

void NoctuaryEditor::SourceView::paint(juce::Graphics& g)
{
    paintSource(g);
    paintEntrance(g);
}

bool NoctuaryEditor::SourceView::ownEntrance() const
{
    const juce::String pre = "src" + juce::String(m_slot) + "_";
    return juce::roundToInt(rawParam(m_proc, (pre + "type").toRawUTF8())) != 0
        && juce::roundToInt(rawParam(m_proc, (pre + "env").toRawUTF8())) == ambient::NumSlotEnvs - 1;
}

/** Under the right-hand line of text, where every type's picture is plot and none is text. */
juce::Rectangle<int> NoctuaryEditor::SourceView::entranceBox() const
{
    return getLocalBounds().reduced(9, 0).removeFromRight(84).withY(21).withHeight(26);
}

void NoctuaryEditor::SourceView::paintEntrance(juce::Graphics& g)
{
    if (!ownEntrance() || getWidth() < 200 || getHeight() < 60) return;
    const auto box = entranceBox().toFloat();
    g.setColour(ui::Bg0.withAlpha(0.85f));
    g.fillRoundedRectangle(box, 4.0f);
    g.setColour(ui::ForeCol.withAlpha(0.5f));
    g.drawRoundedRectangle(box.reduced(0.5f), 4.0f, 1.0f);
    const ambient::ModEnv& e = m_proc.engine().srcEnvShape(m_slot - 1);
    const float depth = juce::jlimit(0.0f, 1.0f, rawParam(m_proc, ("src" + juce::String(m_slot) + "_env_depth").toRawUTF8()));
    const auto plot = box.reduced(5.0f, 5.0f).withTrimmedLeft(20.0f);
    const float len = juce::jmax(0.001f, e.length());
    auto yOf = [&](float gain) { return plot.getBottom() - juce::jlimit(0.0f, 1.0f, gain) * plot.getHeight(); };
    juce::Path p;
    for (int s = 0; s <= 40; ++s) {
        const float t = static_cast<float>(s) / 40.0f;
        const float v = juce::jlimit(0.0f, 1.0f, e.at(t * len, ambient::EnvModeE::ONE_SHOT, true));
        const float x = plot.getX() + t * plot.getWidth();
        if (s == 0) p.startNewSubPath(x, yOf(1.0f - depth * (1.0f - v))); else p.lineTo(x, yOf(1.0f - depth * (1.0f - v)));
    }
    g.setColour(ui::ForeCol);
    g.strokePath(p, juce::PathStrokeType(1.3f));
    float at = 0.0f, gain = 0.0f;
    if (m_proc.engine().displaySlotEnv(m_slot - 1, at, gain) && at <= len * 1.02f) {
        const float x = plot.getX() + juce::jlimit(0.0f, 1.0f, at / len) * plot.getWidth();
        g.setColour(ui::Live);
        g.fillEllipse(x - 2.5f, yOf(gain) - 2.5f, 5.0f, 5.0f);
    }
    g.setColour(ui::Dim);
    g.setFont(ui::body(8.5f));
    g.drawText("ENV", box.withWidth(24.0f).toNearestInt(), juce::Justification::centred, false);
}

void NoctuaryEditor::SourceView::paintSource(juce::Graphics& g)
{
    const auto r = getLocalBounds();
    const juce::String pre = "src" + juce::String(m_slot) + "_";
    const int type = static_cast<int>(std::lround(rawParam(m_proc, (pre + "type").toRawUTF8())));
    static const char* const Titles[] = { "SOURCE OFF", "HARMONIC TABLE", "FM PAIR", "TEXTURE GRAINS", "NOISE COLOUR", "ADDITIVE BANK",
                                           "STRETCH", "BOWED STRING", "SPECTRAL MODEL", "WAVETABLE" };
    displayFrame(g, r, Titles[juce::jlimit(0, 9, type)], ui::VoiceCol);
    const auto plot = r.toFloat().reduced(10.0f, 8.0f).withTrimmedTop(12.0f);
    const float cy = plot.getCentreY(), hh = plot.getHeight() * 0.42f;
    // A table stands in depth, unless it has been clicked flat or has only one frame to stand.
    if ((type == 1 || type == 9) && !m_flat) {
        const int table = static_cast<int>(std::lround(rawParam(m_proc, (pre + "table").toRawUTF8())));
        const float pos = livePosition();
        m_shownPos = pos;
        if (paintTable3D(g, plot, type, table, pos)) {
            const int frames = m_tableFrames;
            g.setColour(ui::Dim); g.setFont(ui::body(10.0f));
            g.drawText(juce::String(ambient::TableNames[juce::jlimit(0, ambient::NumTables - 1, table)]) + "  " + juce::String(frames)
                           + " frames  pos " + juce::String(pos, 2),
                       r.reduced(9, 5), juce::Justification::topRight, false);
            // The place between two frames as a number, since the lit cycle is the blend of the
            // two: "frame 3.4 / 8" and not a count that jumps from 3 to 4 while the sound does not.
            g.drawText("frame " + juce::String(1.0f + pos * static_cast<float>(frames - 1), 1) + " / " + juce::String(frames),
                       r.reduced(9, 5), juce::Justification::bottomRight, false);
            g.setColour(ui::Faint); g.setFont(ui::body(9.5f));
            g.drawText("click: one frame", r.reduced(9, 5), juce::Justification::bottomLeft, false);
            return;
        }
    }
    g.setColour(ui::Track.withAlpha(0.6f));
    g.drawHorizontalLine(juce::roundToInt(cy), plot.getX(), plot.getRight());
    if (type == 0) {
        g.setColour(ui::Faint); g.setFont(ui::body(11.0f));
        g.drawText("choose a type to the left", plot, juce::Justification::centred, false);
        return;
    }

    // Which clip this is. The window showed its envelope, its grains and its length, and not
    // its name -- the one thing that says what one is listening to. Bottom left, under the
    // waveform, cut with an ellipsis when the frame is narrower than the name.
    auto clipName = [&](juce::Graphics& gg) {
        const juce::String name = m_proc.textureName(m_slot - 1);
        if (name.isEmpty()) return;
        gg.setColour(ui::Dim); gg.setFont(ui::body(10.0f));
        gg.drawText(name, r.reduced(9, 5).removeFromLeft(juce::jmax(40, r.getWidth() * 2 / 3)), juce::Justification::bottomLeft, true);
    };

    juce::Path p;
    const int steps = juce::jmax(64, static_cast<int>(plot.getWidth()));
    auto plotWave = [&](auto valueAt) {
        for (int i = 0; i <= steps; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(steps);
            const float y = cy - juce::jlimit(-1.0f, 1.0f, valueAt(t)) * hh;
            const float x = plot.getX() + t * plot.getWidth();
            if (i == 0) p.startNewSubPath(x, y); else p.lineTo(x, y);
        }
    };

    if (type == 1) {   // wavetable: one cycle of the frame at Position, resynthesised from its spectrum
        const int table = static_cast<int>(std::lround(rawParam(m_proc, (pre + "table").toRawUTF8())));
        const float pos = livePosition();
        m_shownPos = pos;
        const ambient::Wavetable* wt = table >= ambient::NumTables - 1 ? m_proc.engine().userWavetable()
                                                                        : &ambient::builtinTable(table);
        float spec[ambient::TablePartials] = {};
        // With the slot's Transport, as the engine reads the table: between two frames the
        // partials walk along the axis rather than fade, and the picture has to show the same.
        if (wt != nullptr && wt->m_frames > 0) wt->spectrumAt(pos, spec, rawParam(m_proc, (pre + "transport").toRawUTF8()));
        float norm = 0.0f; for (float a : spec) norm += a;
        plotWave([&](float t) {
            float v = 0.0f;
            for (int h = 0; h < ambient::TablePartials; ++h)
                if (spec[h] > 1.0e-5f) v += spec[h] * std::sin(juce::MathConstants<float>::twoPi * (h + 1) * t);
            return norm > 1.0e-6f ? v / norm * 1.4f : 0.0f;
        });
        g.setColour(ui::Dim); g.setFont(ui::body(10.0f));
        g.drawText(juce::String(ambient::TableNames[juce::jlimit(0, ambient::NumTables - 1, table)]) + "  pos " + juce::String(pos, 2),
                   r.reduced(9, 5), juce::Justification::topRight, false);
    } else if (type == 9) {   // wavetable: the frame at Position as the samples it is, a few neighbours faint behind it
        const int table = static_cast<int>(std::lround(rawParam(m_proc, (pre + "table").toRawUTF8())));
        const float pos = livePosition();
        m_shownPos = pos;
        const ambient::CycleTable* ct = table >= ambient::NumTables - 1 ? m_proc.engine().userCycles()
                                                                         : &ambient::builtinCycleTable(table);
        if (ct == nullptr || ct->empty()) {
            g.setColour(ui::Faint); g.setFont(ui::body(11.0f));
            g.drawText("no table loaded -- Wavetable... below, or a pack preset", plot, juce::Justification::centred, false);
            return;
        }
        const int level = 2;   // 512 samples a cycle: more than a display this wide can show
        const int last = ct->m_frames - 1;
        auto frameAt = [&](float at, float t) {
            const float x = juce::jlimit(0.0f, 1.0f, at) * static_cast<float>(last);
            const int f0 = juce::jlimit(0, last, static_cast<int>(x));
            const int f1 = juce::jmin(f0 + 1, last);
            const float fr = x - static_cast<float>(f0);
            const double ph = juce::jlimit(0.0, 0.99999, static_cast<double>(t));
            const float a = ct->sample(level, f0, ph), b = ct->sample(level, f1, ph);
            return (a + fr * (b - a)) / (ambient::CycleTable::TargetRms * 2.4f);
        };
        if (last > 0) {
            for (int k = -3; k <= 3; ++k) {
                const float at = pos + static_cast<float>(k) * 0.06f;
                if (k == 0 || at < 0.0f || at > 1.0f) continue;
                juce::Path ghost;
                for (int i = 0; i <= steps; ++i) {
                    const float t = static_cast<float>(i) / static_cast<float>(steps);
                    const float y = cy - juce::jlimit(-1.0f, 1.0f, frameAt(at, t)) * hh;
                    const float x = plot.getX() + t * plot.getWidth();
                    if (i == 0) ghost.startNewSubPath(x, y); else ghost.lineTo(x, y);
                }
                g.setColour(ui::VoiceCol.withAlpha(0.11f - 0.025f * static_cast<float>(std::abs(k))));
                g.strokePath(ghost, juce::PathStrokeType(1.0f));
            }
        }
        plotWave([&](float t) { return frameAt(pos, t); });
        g.setColour(ui::Dim); g.setFont(ui::body(10.0f));
        g.drawText(juce::String(ambient::TableNames[juce::jlimit(0, ambient::NumTables - 1, table)]) + "  " + juce::String(ct->m_frames)
                       + (ct->m_frames == 1 ? " cycle" : " frames") + "  pos " + juce::String(pos, 2),
                   r.reduced(9, 5), juce::Justification::topRight, false);
    } else if (type == 2) {   // FM: carrier phase-modulated by the modulator at the ratio and index
        const float ratio = rawParam(m_proc, (pre + "fm_ratio").toRawUTF8()), idx = rawParam(m_proc, (pre + "fm_index").toRawUTF8());
        plotWave([&](float t) {
            const float ph = juce::MathConstants<float>::twoPi * t;
            return std::sin(ph + idx * std::sin(ph * ratio));
        });
        g.setColour(ui::Dim); g.setFont(ui::body(10.0f));
        g.drawText("ratio " + juce::String(ratio, 2) + "   index " + juce::String(idx, 2), r.reduced(9, 5), juce::Justification::topRight, false);
    } else if (type == 6) {   // stretch: the clip's envelope, the read position and the window being analysed
        const ambient::Texture* tex = m_proc.engine().displayTexture(m_slot - 1);
        if (tex == nullptr || tex->empty()) {
            g.setColour(ui::Faint); g.setFont(ui::body(11.0f));
            g.drawText("no clip loaded -- Texture... below, or a pack preset", plot, juce::Justification::centred, false);
            return;
        }
        const int n = static_cast<int>(tex->m_mono.size());
        const int cols = juce::jmax(32, static_cast<int>(plot.getWidth()));
        g.setColour(ui::VoiceCol.withAlpha(0.55f));
        for (int c = 0; c < cols; ++c) {
            const int a = static_cast<int>(static_cast<long long>(c) * n / cols), b = juce::jmax(a + 1, static_cast<int>(static_cast<long long>(c + 1) * n / cols));
            float peak = 0.0f;
            const int stride = juce::jmax(1, (b - a) / 64);
            for (int i = a; i < b; i += stride) peak = juce::jmax(peak, std::fabs(tex->m_mono[static_cast<size_t>(i)]));
            const float x = plot.getX() + c * plot.getWidth() / cols;
            g.drawVerticalLine(juce::roundToInt(x), cy - peak * hh * 2.0f, cy + peak * hh * 2.0f);
        }
        const float pos = rawParam(m_proc, (pre + "pos").toRawUTF8());
        const float grainMs = rawParam(m_proc, (pre + "grain").toRawUTF8());
        const float stretch = rawParam(m_proc, (pre + "stretch").toRawUTF8());
        const float xfade = rawParam(m_proc, (pre + "xfade").toRawUTF8());
        const float clipSec = static_cast<float>(tex->m_mono.size() / juce::jmax(1.0, tex->m_sampleRate));
        // the window, centred on Position, as wide as it reads
        const float ww = juce::jmax(3.0f, grainMs * 0.001f / juce::jmax(0.05f, clipSec) * plot.getWidth());
        const float px = plot.getX() + juce::jlimit(0.0f, 1.0f, pos) * plot.getWidth();
        g.setColour(ui::Live.withAlpha(0.18f));
        g.fillRect(px - ww * 0.5f, plot.getY(), ww, plot.getHeight());
        g.setColour(ui::Live);
        g.drawVerticalLine(juce::roundToInt(px), plot.getY(), plot.getBottom());
        if (!tex->m_seamless && xfade > 0.001f) {   // the seam's crossfade zone, at the end of the clip
            const float zw = juce::jlimit(0.0f, 0.25f, 0.5f * xfade) * plot.getWidth();
            g.setColour(ui::Accent.withAlpha(0.15f));
            g.fillRect(plot.getRight() - zw, plot.getY(), zw, plot.getHeight());
        }
        g.setColour(ui::Dim); g.setFont(ui::body(10.0f));
        const float secs = clipSec * stretch;
        const juce::String length = secs < 90.0f ? juce::String(secs, 0) + " s" : secs < 5400.0f ? juce::String(secs / 60.0f, 1) + " min" : juce::String(secs / 3600.0f, 1) + " h";
        g.drawText(juce::String(clipSec, 1) + " s clip  x " + juce::String(stretch, 0) + "  =  " + length
                       + (tex->m_seamless ? "   seamless" : ""),
                   r.reduced(9, 5), juce::Justification::topRight, false);
        clipName(g);
        return;
    } else if (type == 3) {   // texture: the clip's envelope, and the window the grains are drawn from
        const ambient::Texture* tex = m_proc.engine().displayTexture(m_slot - 1);
        if (tex == nullptr || tex->empty()) {
            g.setColour(ui::Faint); g.setFont(ui::body(11.0f));
            g.drawText("no clip loaded -- Texture... below, or a pack preset", plot, juce::Justification::centred, false);
            return;
        }
        const int n = static_cast<int>(tex->m_mono.size());
        const int cols = juce::jmax(32, static_cast<int>(plot.getWidth()));
        g.setColour(ui::VoiceCol.withAlpha(0.55f));
        for (int c = 0; c < cols; ++c) {
            const int a = static_cast<int>(static_cast<long long>(c) * n / cols), b = juce::jmax(a + 1, static_cast<int>(static_cast<long long>(c + 1) * n / cols));
            float peak = 0.0f;
            const int stride = juce::jmax(1, (b - a) / 64);
            for (int i = a; i < b; i += stride) peak = juce::jmax(peak, std::fabs(tex->m_mono[static_cast<size_t>(i)]));
            const float x = plot.getX() + c * plot.getWidth() / cols;
            g.drawVerticalLine(juce::roundToInt(x), cy - peak * hh * 2.0f, cy + peak * hh * 2.0f);
        }
        const float pos = rawParam(m_proc, (pre + "pos").toRawUTF8()), spread = rawParam(m_proc, (pre + "spread").toRawUTF8());
        const float x0 = plot.getX() + juce::jlimit(0.0f, 1.0f, pos - spread) * plot.getWidth();
        const float x1 = plot.getX() + juce::jlimit(0.0f, 1.0f, pos + spread) * plot.getWidth();
        g.setColour(ui::Live.withAlpha(0.18f));
        g.fillRect(x0, plot.getY(), juce::jmax(2.0f, x1 - x0), plot.getHeight());
        g.setColour(ui::Live);
        g.drawVerticalLine(juce::roundToInt(plot.getX() + pos * plot.getWidth()), plot.getY(), plot.getBottom());
        // The grains themselves: each a window over the clip where it is reading right now, wide
        // as its length, fading as it ages -- the loudest voice's slot, live.
        ambient::SourceSlot::GrainInfo gi[ambient::SlotGrains];
        const int gn = m_proc.engine().displayGrains(m_slot - 1, gi, ambient::SlotGrains);
        // Each grain as what it actually is: a window travelling through the clip. The bar spans
        // the piece it has read so far, its height is its level times where the Hann window
        // stands, the head marks the sample it is on this instant, and the vertical place is its
        // pan. A grain of two hundred milliseconds in a minute of tape is a third of a pixel
        // wide, so the bar is what makes it visible at all -- as a single mark it was a hair.
        const float grainMs = rawParam(m_proc, (pre + "grain").toRawUTF8());
        const float clipSec = static_cast<float>(tex->m_mono.size() / juce::jmax(1.0, tex->m_sampleRate));
        const float gw = grainMs * 0.001f / juce::jmax(0.05f, clipSec) * plot.getWidth();
        for (int i = 0; i < gn; ++i) {
            const float age = juce::jlimit(0.0f, 1.0f, gi[i].m_age);
            const float w = std::sin(juce::MathConstants<float>::pi * age);        // the window, now
            const float level = juce::jlimit(0.0f, 1.0f, gi[i].m_gain) * w;
            const float x1 = plot.getX() + juce::jlimit(0.0f, 1.0f, gi[i].m_pos) * plot.getWidth();
            const float x0 = juce::jmax(plot.getX(), x1 - gw * age);               // where it began
            const float gh = juce::jmax(2.0f, (0.12f + 0.88f * level) * plot.getHeight() * 0.44f);
            const float gy = cy - gh * 0.5f + gi[i].m_pan * plot.getHeight() * 0.20f;
            const juce::Colour c = ui::Accent.withRotatedHue(0.06f * gi[i].m_pan);   // left and right differ a little
            g.setColour(c.withAlpha(0.10f + 0.35f * level));
            g.fillRoundedRectangle(x0, gy, juce::jmax(2.0f, x1 - x0), gh, 2.0f);
            g.setColour(c.withAlpha(0.35f + 0.55f * level));                       // the head: where it reads now
            g.fillRoundedRectangle(x1 - 1.5f, gy, 3.0f, gh, 1.5f);
            if (gh > 8.0f) {   // the window's own shape, so a grain reads as a grain and not a bar
                juce::Path win;
                const int steps = 12;
                win.startNewSubPath(x0, gy + gh);
                for (int k2 = 0; k2 <= steps; ++k2) {
                    const float t = static_cast<float>(k2) / steps;
                    const float e = std::sin(juce::MathConstants<float>::pi * t * age);
                    win.lineTo(x0 + (x1 - x0) * t, gy + gh - gh * juce::jlimit(0.0f, 1.0f, e));
                }
                g.setColour(c.withAlpha(0.5f));
                g.strokePath(win, juce::PathStrokeType(1.0f));
            }
        }
        g.setColour(ui::Dim); g.setFont(ui::body(10.0f));
        g.drawText(juce::String(gn) + " grains   " + juce::String(juce::roundToInt(grainMs)) + " ms",
                   r.reduced(9, 5).withTrimmedTop(12), juce::Justification::topRight, false);
        g.setColour(ui::Dim); g.setFont(ui::body(10.0f));
        g.drawText(juce::String(tex->m_mono.size() / juce::jmax(1.0, tex->m_sampleRate), 1) + " s   base " + juce::String(tex->m_baseHz, 1) + " Hz",
                   r.reduced(9, 5), juce::Justification::topRight, false);
        clipName(g);
        return;
    } else if (type == 5) {   // additive: the bank's partials as they are being summed, and the cycle they make
        float live[ambient::TablePartials] = {};
        const int n = m_slot == 1 ? m_proc.engine().displayPartials(live, ambient::TablePartials)
                                : m_proc.engine().displaySlotPartials(m_slot - 1, live, ambient::TablePartials);
        float peak = 1.0e-6f;
        for (int i = 0; i < ambient::TablePartials; ++i) {
            m_amp[i] += ((i < n ? std::abs(live[i]) : 0.0f) - m_amp[i]) * 0.25f;
            peak = juce::jmax(peak, m_amp[i]);
        }
        if (peak < 1.0e-4f) {
            g.setColour(ui::Faint); g.setFont(ui::body(11.0f));
            g.drawText("additive bank -- nothing sounding", plot, juce::Justification::centred, false);
            return;
        }
        const float bw = juce::jmin(9.0f, plot.getWidth() / static_cast<float>(ambient::TablePartials));
        for (int i = 0; i < ambient::TablePartials; ++i) {
            const float a = m_amp[i] / peak;
            if (a < 1.0e-4f) continue;
            const float db = juce::jlimit(0.0f, 1.0f, 1.0f + std::log10(a) / 2.5f);
            const float h = db * plot.getHeight() * 0.9f;
            g.setColour(ui::Accent.withAlpha(0.10f + 0.13f * db));
            g.fillRect(plot.getX() + i * bw + 0.5f, plot.getBottom() - h, juce::jmax(1.0f, bw - 1.2f), h);
        }
        plotWave([&](float t) {
            float v = 0.0f;
            for (int i = 0; i < ambient::TablePartials; ++i)
                if (m_amp[i] > 1.0e-5f) v += m_amp[i] * std::sin(juce::MathConstants<float>::twoPi * ((i + 1) * t + 0.381966f * i));
            return v / (peak * 2.2f);
        });
        g.setColour(ui::Dim); g.setFont(ui::body(10.0f));
        g.drawText(juce::String(n) + " partials" + (m_slot == 1 ? "   " + juce::String(rawParam(m_proc, "strands"), 0) + " strands" : juce::String()),
                   r.reduced(9, 5), juce::Justification::topRight, false);
        if (m_slot != 1) {
            // Said, rather than left to be guessed from greyed cells: this bank is one strand by
            // design, and why.
            g.setColour(ui::Faint); g.setFont(ui::body(9.5f));
            g.drawText("one strand: Strands (unison, detune, stack) belong to Source 1's bank -- a second bank costs what a wavetable slot costs",
                       r.reduced(9, 5), juce::Justification::bottomLeft, false);
        }
    } else {   // noise: the colour as a spectral slope, plus the band centre for Band and Wind
        const int kind = static_cast<int>(std::lround(rawParam(m_proc, (pre + "noise").toRawUTF8())));
        static const float Slope[] = { 0.0f, -3.0f, -6.0f, 3.0f, 6.0f, 0.0f, 0.0f, 0.0f, -2.0f, -6.0f };
        const float slope = Slope[juce::jlimit(0, 9, kind)];
        const auto sp = plot;
        g.setColour(ui::Track.withAlpha(0.5f));
        for (float hz : { 100.0f, 1000.0f, 10000.0f }) g.drawVerticalLine(juce::roundToInt(xForHz(sp, hz)), sp.getY(), sp.getBottom());
        juce::Path line;
        const bool banded = (kind == 6 || kind == 7);
        const float posN = rawParam(m_proc, (pre + "pos").toRawUTF8());
        const float centre = 40.0f * std::pow(300.0f, posN);
        const float q = rawParam(m_proc, (pre + "noise_q").toRawUTF8());
        for (int i = 0; i <= steps; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(steps);
            const float hz = 20.0f * std::pow(1000.0f, t);
            float db = slope * std::log2(hz / 1000.0f);
            if (kind == 5) db -= 6.0f * std::exp(-std::pow(std::log2(hz / 3000.0f), 2.0f) / 0.9f);   // grey: the dip at 3 kHz
            if (banded) {
                const float width = 0.15f + 2.2f * (1.0f - q);
                db = -30.0f + 30.0f * std::exp(-std::pow(std::log2(hz / centre) / width, 2.0f));
            }
            const float x = sp.getX() + t * sp.getWidth();
            const float y = yForDb(sp, juce::jlimit(-36.0f, 12.0f, db - 6.0f));
            if (i == 0) line.startNewSubPath(x, y); else line.lineTo(x, y);
        }
        g.setColour(ui::VoiceCol.withAlpha(0.25f)); g.strokePath(line, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(ui::VoiceCol);                  g.strokePath(line, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(ui::Dim); g.setFont(ui::body(10.0f));
        g.drawText(juce::String(ambient::NoiseKindNames[juce::jlimit(0, ambient::NumNoiseKinds - 1, kind)])
                       + (banded ? "   " + juce::String(centre, 0) + " Hz" : juce::String()),
                   r.reduced(9, 5), juce::Justification::topRight, false);
        return;
    }
    g.setColour(ui::VoiceCol.withAlpha(0.25f)); g.strokePath(p, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(ui::VoiceCol);                  g.strokePath(p, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

// ---------------------------------------------------------------- the output's spectrum

// ---------------------------------------------------------------- the Vector's square

juce::Rectangle<float> NoctuaryEditor::VectorView::square() const
{
    const auto plot = getLocalBounds().toFloat().reduced(12.0f, 9.0f).withTrimmedTop(15.0f);
    const float side = juce::jmin(plot.getWidth(), plot.getHeight());
    return juce::Rectangle<float>(plot.getCentreX() - side * 0.5f, plot.getCentreY() - side * 0.5f, side, side);
}

void NoctuaryEditor::VectorView::drag(const juce::MouseEvent& e)
{
    const auto sq = square();
    if (sq.getWidth() < 4.0f) return;
    const float x = juce::jlimit(0.0f, 1.0f, (e.position.x - sq.getX()) / sq.getWidth());
    const float y = juce::jlimit(0.0f, 1.0f, 1.0f - (e.position.y - sq.getY()) / sq.getHeight());
    if (auto* px = m_proc.m_apvts.getParameter(paramDesc(ambient::ParamIdE::VEC_X).m_key)) px->setValueNotifyingHost(x);
    if (auto* py = m_proc.m_apvts.getParameter(paramDesc(ambient::ParamIdE::VEC_Y).m_key)) py->setValueNotifyingHost(y);
}

void NoctuaryEditor::VectorView::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds();
    displayFrame(g, r, "VECTOR", ui::VoiceCol);
    const auto sq = square();
    if (sq.getWidth() < 20.0f) return;

    const float amount = rawParam(m_proc, "vec_amount");
    const float vx = rawParam(m_proc, "vec_x"), vy = rawParam(m_proc, "vec_y");
    auto toXY = [&](float x, float y) {
        return juce::Point<float>(sq.getX() + x * sq.getWidth(), sq.getBottom() - y * sq.getHeight());
    };

    g.setColour(ui::CardColour);
    g.fillRoundedRectangle(sq, 4.0f);
    g.setColour(ui::Track.withAlpha(0.5f));
    for (int i = 1; i < 4; ++i) {
        g.drawHorizontalLine(juce::roundToInt(sq.getY() + sq.getHeight() * i / 4.0f), sq.getX(), sq.getRight());
        g.drawVerticalLine(juce::roundToInt(sq.getX() + sq.getWidth() * i / 4.0f), sq.getY(), sq.getBottom());
    }
    g.setColour(ui::Faint);
    g.drawRoundedRectangle(sq, 4.0f, 1.0f);

    // The corners, named for what is at them.
    g.setFont(ui::body(9.0f));
    g.setColour(ui::Dim);
    g.drawText("SRC 1", sq.getX() + 3.0f, sq.getBottom() - 12.0f, 44.0f, 11.0f, juce::Justification::left, false);
    g.drawText("SRC 2", sq.getRight() - 47.0f, sq.getBottom() - 12.0f, 44.0f, 11.0f, juce::Justification::right, false);
    g.drawText("SRC 3", sq.getX() + 3.0f, sq.getY() + 2.0f, 44.0f, 11.0f, juce::Justification::left, false);
    g.drawText("SRC 4", sq.getRight() - 47.0f, sq.getY() + 2.0f, 44.0f, 11.0f, juce::Justification::right, false);

    // Where the wander has actually been. The point on the panel is where the knobs are; the trail
    // is where the engine's own drift has taken it, which is the part a knob cannot show.
    const bool live = amount > 0.0005f;
    const float wander = rawParam(m_proc, "vec_wander");
    if (live && wander > 0.0005f) {
        m_tx[m_head] = vx; m_ty[m_head] = vy;                 // the drift itself is inside the engine; this
        m_head = (m_head + 1) % Trail;                    // records where the setting has been
        if (m_filled < Trail) ++m_filled;
        juce::Path trail;
        for (int k = 0; k < m_filled; ++k) {
            const auto p = toXY(m_tx[(m_head - m_filled + k + Trail) % Trail], m_ty[(m_head - m_filled + k + Trail) % Trail]);
            if (k == 0) trail.startNewSubPath(p); else trail.lineTo(p);
        }
        g.setColour(ui::Accent.withAlpha(0.25f));
        g.strokePath(trail, juce::PathStrokeType(1.0f));
    }

    // The four slot weights, as a bubble in each corner: what the point is actually doing.
    const float f[4] = { (1.0f - vx) * (1.0f - vy), vx * (1.0f - vy), (1.0f - vx) * vy, vx * vy };
    const juce::Point<float> corner[4] = { toXY(0.0f, 0.0f), toXY(1.0f, 0.0f), toXY(0.0f, 1.0f), toXY(1.0f, 1.0f) };
    for (int k = 0; k < 4; ++k) {
        const float t = juce::jlimit(0.0f, 1.0f, f[k]);
        g.setColour(ui::Accent.withAlpha(0.15f + 0.5f * t));
        g.fillEllipse(corner[k].x - 4.0f - 10.0f * t, corner[k].y - 4.0f - 10.0f * t,
                      8.0f + 20.0f * t, 8.0f + 20.0f * t);
    }

    const auto p = toXY(vx, vy);
    g.setColour(live ? ui::Live : ui::Dim.withAlpha(0.5f));
    g.fillEllipse(p.x - 5.0f, p.y - 5.0f, 10.0f, 10.0f);
    g.setColour(ui::Text.withAlpha(live ? 0.9f : 0.4f));
    g.drawEllipse(p.x - 7.0f, p.y - 7.0f, 14.0f, 14.0f, 1.2f);

    // The three weights as bars beside the square: the picture says where the point is, these say
    // what that does to each slot, which is the question the ear is actually asking.
    const float barX = sq.getRight() + 18.0f;
    const float barW = r.getRight() - 14.0f - barX;
    if (barW > 90.0f) {
        static const char* const Names[4] = { "SOURCE 1", "SOURCE 2", "SOURCE 3", "SOURCE 4" };
        const float rowH = 20.0f;
        float by = sq.getCentreY() - 2.0f * rowH;
        g.setFont(ui::body(9.5f));
        for (int k = 0; k < 4; ++k, by += rowH) {
            g.setColour(ui::Dim);
            g.drawText(Names[k], barX, by, 58.0f, 12.0f, juce::Justification::left, false);
            const float x0 = barX + 62.0f, w = barW - 62.0f - 34.0f;
            g.setColour(ui::Track.withAlpha(0.5f));
            g.fillRoundedRectangle(x0, by + 2.0f, w, 8.0f, 3.0f);
            // The factor the engine applies, which is what the level is multiplied by.
            const float factor = live ? 1.0f + amount * (4.0f * f[k] - 1.0f) : 1.0f;
            g.setColour(ui::Accent.withAlpha(0.35f + 0.5f * juce::jlimit(0.0f, 1.0f, factor / 4.0f)));
            g.fillRoundedRectangle(x0, by + 2.0f, juce::jmax(2.0f, w * juce::jlimit(0.0f, 1.0f, factor / 4.0f)), 8.0f, 3.0f);
            g.setColour(ui::Text.withAlpha(0.75f));
            g.drawText(juce::String(factor, 2) + "x", x0 + w + 4.0f, by, 30.0f, 12.0f, juce::Justification::left, false);
        }
        g.setColour(ui::Faint);
        g.drawText("the factor on each slot's own level", barX, sq.getCentreY() + 2.4f * rowH, barW, 12.0f,
                   juce::Justification::left, false);
    }

    g.setColour(ui::Dim);
    g.setFont(ui::body(9.5f));
    g.drawText(live ? juce::String("drag the point") : juce::String("Amount is 0: the slots play at their own levels"),
               r.reduced(9, 5), juce::Justification::topRight, false);
}

// ---------------------------------------------------------------- loudness, in the header

void NoctuaryEditor::LoudnessView::mouseDown(const juce::MouseEvent&)
{
    m_proc.engine().resetLoudness();
    repaint();
}

void NoctuaryEditor::LoudnessView::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    g.setColour(ui::Bg0.withAlpha(0.5f));
    g.fillRoundedRectangle(r, 4.0f);
    const ambient::LoudnessReading ld = m_proc.engine().loudness();
    const auto plot = r.reduced(6.0f, 3.0f);

    // A bar for the short-term value, over the window an ambient master lives in: -30 to -6 LUFS,
    // with the -18 to -24 band the literature asks for marked out. The number matters more than
    // the bar, so the bar is thin and the type is not.
    const float lo = -30.0f, hi = -6.0f;
    auto xOf = [&](float lufs) { return plot.getX() + plot.getWidth() * 0.42f * juce::jlimit(0.0f, 1.0f, (lufs - lo) / (hi - lo)); };
    const float y = plot.getCentreY();
    g.setColour(ui::Track.withAlpha(0.5f));
    g.fillRoundedRectangle(plot.getX(), y - 3.0f, plot.getWidth() * 0.42f, 6.0f, 2.0f);
    // The window a dark-ambient master is asked to land in: -24 to -16 LUFS, the range the genre's
    // own mastering advice gives (-16 to -20) together with the quieter end the drone literature
    // uses. The thin line at -14 is where the streaming services normalise to: a master louder
    // than that is turned down again, and arrives flat rather than loud.
    g.setColour(ui::Accent.withAlpha(0.20f));
    g.fillRoundedRectangle(xOf(-24.0f), y - 3.0f, xOf(-16.0f) - xOf(-24.0f), 6.0f, 2.0f);
    g.setColour(ui::Dim.withAlpha(0.7f));
    g.drawVerticalLine(juce::roundToInt(xOf(-14.0f)), y - 5.0f, y + 5.0f);
    if (ld.m_shortTerm > -119.0f) {
        g.setColour(ld.m_shortTerm > -14.0f ? juce::Colour(0xffe0a060) : ui::Live);
        g.fillRoundedRectangle(plot.getX(), y - 3.0f, juce::jmax(2.0f, xOf(ld.m_shortTerm) - plot.getX()), 6.0f, 2.0f);
    }
    if (ld.m_integrated > -119.0f) {   // the integrated value as a mark on the same scale
        g.setColour(ui::Text.withAlpha(0.8f));
        g.drawVerticalLine(juce::roundToInt(xOf(ld.m_integrated)), y - 6.0f, y + 6.0f);
    }

    g.setFont(ui::body(9.5f));
    auto num = [](float v) { return v > -119.0f ? juce::String(v, 1) : juce::String("--"); };
    // The sone figure sits with the rest of them because it answers a different question: LUFS is
    // energy, sones are how loud the ear calls it, and a wide bed and a narrow drone at the same
    // LUFS can be a factor of two apart.
    const juce::String text = "I " + num(ld.m_integrated) + "   S " + num(ld.m_shortTerm)
                            + "   LRA " + juce::String(ld.m_range, 1)
                            + "   TP " + num(ld.m_truePeak)
                            + "   crest " + juce::String(ld.m_crest, 1)
                            + "   corr " + juce::String(ld.m_correlation, 2)
                            + "   " + juce::String(ld.m_sones, 1) + " sone";
    // Red when the true peak is over the -1 dBTP a lossy codec needs as headroom, amber when the
    // crest factor has fallen under the 14 dB that says the dynamics are still there, or when the
    // two channels have turned against each other (a correlation under zero folds away in mono;
    // the production guide wants the master between 0.3 and 0.7, and under zero only for a moment).
    const bool tpHot = ld.m_truePeak > -1.0f;
    const bool warn = (ld.m_crest > 0.0f && ld.m_crest < 14.0f) || (ld.m_seconds > 3.0f && ld.m_correlation < 0.0f);
    g.setColour(tpHot ? juce::Colour(0xffe06060) : (warn ? juce::Colour(0xffe0a060) : ui::Dim));
    g.drawText(text, plot.withTrimmedLeft(plot.getWidth() * 0.44f), juce::Justification::centredLeft, false);
}
