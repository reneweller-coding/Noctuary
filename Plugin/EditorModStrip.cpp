/**
 * @file EditorModStrip.cpp
 * @brief The modulation strip along the bottom: the cards, the LFO and envelope editors, the matrix.
 *
 * NoctuaryEditor::ModView (PluginEditor.h) is the strip Pigments taught everyone to want under a
 * synth: a lane of every modulation source as a small card with its live shape, a row of tabs, and
 * under them the full editors of whichever group is open -- the eight LFOs, the envelopes (the six
 * modulation envelopes on one page, the four sources' own on the other), or the matrix as a table
 * of routes (EditorMatrix.cpp). The editor owns it as m_mod inside m_modPort and builds it once. It
 * runs on the message thread, reads the parameters through apvts and the live values -- LFO phases,
 * envelope times, the sources' outputs this instant -- through engine() getters, and writes back
 * through the parameter attachments, Engine::setEnvShape and the matrix text.
 *
 * Three gestures start here and end elsewhere. A card dragged out of the lane onto a knob becomes a
 * route: the drawing of that flight and the depth knob that appears where it lands are the editor's
 * (EditorDrag.cpp), the route itself is added by addRoute(), as the same text every other door into
 * the matrix uses. A breakpoint of an envelope dragged, added, removed or given a shape from the
 * right-click menu is written to the engine as the envelope's text, with an undo point taken first
 * (envCommit). And a right click on a card lists what that source drives and lets a route go.
 *
 * The envelope editor's arithmetic is in one place on purpose: envGeom() gives the geometry of a
 * row, envToXY() and envFromXY() map through it in both directions, and paintEnv() and the mouse
 * handlers use nothing else, so what is grabbed is what is seen.
 * @author Rene Weller
 */
#include "EditorCommon.h"

using namespace ambient;


namespace edt {
/**
 * LFOs in the accent colour, envelopes in the foreground colour, macros in the morph colour, the
 * Kuramoto oscillators in the conductor's; everything else -- amp, note, velocity, distance, the
 * per-note random, beat, cascade and the rest -- in the cosmos colour. The cards, the drag overlay and the
 * depth popup all take their colour from here, so a route reads as its source wherever it shows.
 */
juce::Colour sourceColour(ambient::ModSourceE s)
{
    using MST = ambient::ModSourceE;
    const int i = static_cast<int>(s);
    if (i >= static_cast<int>(MST::LFO1) && i <= static_cast<int>(MST::LFO8)) return ui::Accent;
    if (i >= static_cast<int>(MST::ENV1) && i <= static_cast<int>(MST::ENV6)) return ui::ForeCol;
    if (i >= static_cast<int>(MST::MACRO_A) && i <= static_cast<int>(MST::MACRO_H)) return ui::MorphCol;
    if (i >= static_cast<int>(MST::KURA1) && i <= static_cast<int>(MST::KURA4)) return ui::CondCol;
    return ui::CosmosCol;
}
} // namespace edt

NoctuaryEditor::ModView::ModView(NoctuaryProcessor& p, NoctuaryEditor& o)
    : m_proc(p), m_owner(o)
{
    auto addKnob = [this](Row& row, const juce::String& key, const juce::String& name) {
        const ParamDesc* d = findParam(key.toRawUTF8());
        if (d == nullptr) return;
        if (d->m_kind == ParamKindE::CHOICE) {
            auto cb = std::make_unique<juce::ComboBox>();
            for (int i = 0; i < d->m_numChoices; ++i) cb->addItem(d->m_choices[i], i + 1);
            cb->setTooltip(ambient::paramHelp(d->m_id));
            addAndMakeVisible(*cb);
            row.m_combos.push_back(std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(m_proc.m_apvts, key, *cb));
            row.m_controls.push_back(std::move(cb));
        } else {
            auto s = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
            if (d->m_unit[0] != 0) s->setTextValueSuffix(juce::String(" ") + d->m_unit);
            s->setTooltip(ambient::paramHelp(d->m_id));
            s->setColour(juce::Slider::rotarySliderFillColourId, ui::Accent);
            addAndMakeVisible(*s);
            row.m_sliders.push_back(std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(m_proc.m_apvts, key, *s));
            row.m_controls.push_back(std::move(s));
        }
        m_owner.registerHelp(row.m_controls.back().get(), d->m_id);   // the header's help line covers the strip too
        auto l = std::make_unique<juce::Label>(juce::String(), name);
        l->setJustificationType(juce::Justification::centred);
        l->setFont(ui::body(10.0f));
        l->setColour(juce::Label::textColourId, ui::Dim);
        addAndMakeVisible(*l);
        row.m_labels.push_back(std::move(l));
    };

    for (int i = 0; i < ambient::NumLfos; ++i) {
        Row& r = m_lfos[static_cast<size_t>(i)];
        r.m_title = "LFO " + juce::String(i + 1);
        const juce::String n(i + 1);
        addKnob(r, "lfo" + n + "_shape", "Shape");
        addKnob(r, "lfo" + n + "_rate", "Rate");
        addKnob(r, "lfo" + n + "_phase", "Phase");
        addKnob(r, "lfo" + n + "_depth", "Depth");
        addKnob(r, "lfo" + n + "_mode", "Mode");
        addKnob(r, "lfo" + n + "_table", "Table");
        addKnob(r, "lfo" + n + "_sync", "Sync");
    }
    for (int i = 0; i < ambient::NumModEnvs; ++i) {
        Row& r = m_envs[static_cast<size_t>(i)];
        r.m_title = "ENV " + juce::String(i + 1);
        const juce::String n(i + 1);
        addKnob(r, "env" + n + "_mode", "Mode");
        addKnob(r, "env" + n + "_time", "Time");
        addKnob(r, "env" + n + "_depth", "Depth");
        addKnob(r, "env" + n + "_sync", "Sync");
    }
    // The four sources' own envelopes, on the ENV tab's second page: the same editor and the same
    // four knobs, for a shape that is a level and runs from every note.
    for (int k = 0; k < ambient::Slots; ++k) {
        Row& r = m_envs[static_cast<size_t>(ambient::NumModEnvs + k)];
        r.m_title = "SOURCE " + juce::String(k + 1);
        const juce::String n(k + 1);
        addKnob(r, "src" + n + "_env_mode", "Mode");
        addKnob(r, "src" + n + "_env_time", "Time");
        addKnob(r, "src" + n + "_env_depth", "Depth");
        addKnob(r, "src" + n + "_env_sync", "Sync");
    }

    // The lane: every source that can drive something, in the order the matrix names them.
    using MST = ambient::ModSourceE;
    auto card = [this](MST s, const juce::String& label, int t, int idx) {
        m_cards.push_back({ s, label, sourceColour(s), {}, t, idx });
    };
    for (int i = 0; i < ambient::NumLfos; ++i) card(static_cast<MST>(static_cast<int>(MST::LFO1) + i), "LFO " + juce::String(i + 1), 0, i);
    for (int i = 0; i < ambient::NumModEnvs; ++i) card(static_cast<MST>(static_cast<int>(MST::ENV1) + i), "ENV " + juce::String(i + 1), 1, i);
    for (int i = 0; i < 8; ++i) card(static_cast<MST>(static_cast<int>(MST::MACRO_A) + i), juce::String("MACRO ") + static_cast<char>('A' + i), 2, i);
    for (int i = 0; i < 4; ++i) card(static_cast<MST>(static_cast<int>(MST::KURA1) + i), "KURA " + juce::String(i + 1), 2, i);
    card(MST::AMP, "AMP", 2, 0);
    card(MST::NOTE, "NOTE", 2, 0);
    card(MST::VELOCITY, "VELO", 2, 0);
    card(MST::DISTANCE, "DIST", 2, 0);
    card(MST::RANDOM_PER_NOTE, "RAND", 2, 0);
    card(MST::BEAT, "BEAT", 2, 0);   // the chord listening to how far out of tune it is
    card(MST::CASCADE, "CASC", 2, 0);   // the conductor's excitation: drag it at anything that should swell where the piece is busy

    for (auto* b : { &m_tabLfo, &m_tabEnv, &m_tabMatrix }) {
        b->setClickingTogglesState(true);
        b->setRadioGroupId(4711);
        addAndMakeVisible(*b);
    }
    m_tabLfo.onClick = [this] { setTab(0); };
    m_tabEnv.onClick = [this] { setTab(1); };
    m_tabMatrix.onClick = [this] { setTab(2); };
    m_tabLfo.setToggleState(true, juce::dontSendNotification);
    for (auto* b : { &m_pageMod, &m_pageSrc }) {   // the ENV tab's two pages
        b->setClickingTogglesState(true);
        b->setRadioGroupId(4712);
        addChildComponent(*b);
    }
    m_pageMod.onClick = [this] { setEnvPage(0); };
    m_pageSrc.onClick = [this] { setEnvPage(1); };
    m_pageMod.setToggleState(true, juce::dontSendNotification);

    // The matrix page is a table of routes now (EditorMatrix.cpp); the text box it replaces is
    // still the format everything travels in, one level down.
    m_table = std::make_unique<edt::RouteTable>(m_proc, [this] {
        m_matrixInfo.setText(juce::String(m_proc.engine().modMatrix().count()) + " of 32 routes", juce::dontSendNotification);
        m_owner.repaint();
    });
    addChildComponent(*m_table);
    m_matrixInfo.setFont(ui::body(11.5f));
    m_matrixInfo.setColour(juce::Label::textColourId, ui::Dim);
    addChildComponent(m_matrixInfo);
    m_hint.setText("one row per route: source, target, depth, an optional via source that scales it, and 0..1 -- or drag a card onto a knob",
                 juce::dontSendNotification);
    m_hint.setFont(ui::body(11.5f));
    m_hint.setColour(juce::Label::textColourId, ui::Faint);
    addChildComponent(m_hint);
    pullMatrix();
    setTab(0);
    startTimerHz(15);   // was 30: this strip was 54 % of all the drawing the panel did
}

void NoctuaryEditor::ModView::setTab(int t)
{
    m_tab = t;
    for (int i = 0; i < ambient::NumLfos; ++i) {
        for (auto& c : m_lfos[static_cast<size_t>(i)].m_controls) c->setVisible(t == 0);
        for (auto& l : m_lfos[static_cast<size_t>(i)].m_labels) l->setVisible(t == 0);
    }
    for (int i = 0; i < EnvRows; ++i) {
        const bool on = t == 1 && envVisible(i);
        for (auto& c : m_envs[static_cast<size_t>(i)].m_controls) c->setVisible(on);
        for (auto& l : m_envs[static_cast<size_t>(i)].m_labels) l->setVisible(on);
    }
    m_pageMod.setVisible(t == 1);
    m_pageSrc.setVisible(t == 1);
    juce::Component* const matrixParts[] = { m_table.get(), &m_matrixInfo, &m_hint };
    for (juce::Component* c : matrixParts) c->setVisible(t == 2);
    if (t == 2) pullMatrix();
    m_tabLfo.setToggleState(t == 0, juce::dontSendNotification);
    m_tabEnv.setToggleState(t == 1, juce::dontSendNotification);
    m_tabMatrix.setToggleState(t == 2, juce::dontSendNotification);
    resized();
    repaint();
}

void NoctuaryEditor::ModView::setEnvPage(int page)
{
    m_envPage = juce::jlimit(0, 1, page);
    m_pageMod.setToggleState(m_envPage == 0, juce::dontSendNotification);
    m_pageSrc.setToggleState(m_envPage == 1, juce::dontSendNotification);
    m_hoverEnv = m_hoverPoint = -1;
    setTab(m_tab);
}

/** Rows 0..5 are the six modulation envelopes, 6..9 the sources' own; one page shows either. */
bool NoctuaryEditor::ModView::envVisible(int env) const
{
    return (env < ambient::NumModEnvs) == (m_envPage == 0);
}

const ambient::ModEnv& NoctuaryEditor::ModView::shapeOf(int env) const
{
    return env < ambient::NumModEnvs ? m_proc.engine().envShape(env) : m_proc.engine().srcEnvShape(env - ambient::NumModEnvs);
}

/** "env3_depth" for the six, "src2_env_depth" for the sources' own. */
juce::String NoctuaryEditor::ModView::envKey(int env, const char* field) const
{
    return env < ambient::NumModEnvs ? "env" + juce::String(env + 1) + "_" + field
                                      : "src" + juce::String(env - ambient::NumModEnvs + 1) + "_env_" + field;
}

void NoctuaryEditor::ModView::pullMatrix()
{
    if (m_table) m_table->pull();
    m_matrixInfo.setColour(juce::Label::textColourId, ui::Dim);
    m_matrixInfo.setText(juce::String(m_proc.engine().modMatrix().count()) + " of 32 routes", juce::dontSendNotification);
}

/** here, where RouteTable is a complete type */
NoctuaryEditor::ModView::~ModView() = default;

bool NoctuaryEditor::ModView::addRoute(ambient::ModSourceE src, ParamIdE target)
{
    char buf[4096];
    const int n = m_proc.engine().writeModMatrix(buf, sizeof(buf));
    juce::String t(juce::CharPointer_UTF8(buf), static_cast<size_t>(juce::jmax(0, n)));
    if (t.isNotEmpty()) t += ";";
    // A quarter of the target's range: enough to hear, not enough to wreck the preset.
    t += juce::String(ambient::modSourceName(src)) + ">" + paramDesc(target).m_key + ":0.25";
    if (!m_proc.engine().setModMatrixText(t.toRawUTF8())) return false;
    if (m_tab == 2) pullMatrix();
    m_matrixInfo.setText(juce::String(m_proc.engine().modMatrix().count()) + " of 32 routes", juce::dontSendNotification);
    m_owner.repaint();
    return true;
}

void NoctuaryEditor::ModView::timerCallback() { if (isShowing()) repaint(); }

ambient::LfoSpec NoctuaryEditor::ModView::specOf(int i) const
{
    const juce::String n(i + 1);
    auto get = [this](const juce::String& key) {
        auto* v = m_proc.m_apvts.getRawParameterValue(key);
        return v != nullptr ? v->load() : 0.0f;
    };
    ambient::LfoSpec sp;
    sp.m_shape  = static_cast<ambient::LfoShapeE>(juce::jlimit(0, ambient::NumLfoShapes - 1,
                    static_cast<int>(std::lround(get("lfo" + n + "_shape")))));
    sp.m_rateHz = get("lfo" + n + "_rate");
    sp.m_phase  = get("lfo" + n + "_phase");
    sp.m_depth  = get("lfo" + n + "_depth");
    sp.m_table  = static_cast<int>(std::lround(get("lfo" + n + "_table")));
    const int sync = static_cast<int>(std::lround(get("lfo" + n + "_sync")));
    if (ambient::syncOn(sync)) sp.m_rateHz = static_cast<float>(ambient::syncHz(sync, m_proc.engine().tempo()));
    return sp;
}


// ---------------------------------------------------------------- strip: drawing

namespace {
/**
 * @brief The frame every LFO and envelope curve is drawn in: the dark card, its edge lit in the row's
 *        colour when the row is in use, and the zero line across it.
 *
 * The line is the zero of the curve: the middle for a bipolar shape, the floor (lineY) for a level.
 * @param[in,out] g      the strip's graphics context
 * @param[in]     r      the row's curve rectangle, in the strip's pixels
 * @param[in]     lit    whether the row does anything (a depth above zero; a source's envelope that is heard): lit rows
 *                       get their edge in @p c, unlit ones the plain card edge
 * @param[in]     c      the row's family colour, the accent for an LFO and the foreground colour for an envelope
 * @param[in]     lineY  the y of the zero line, or negative for the rectangle's vertical centre
 */
void curveFrame(juce::Graphics& g, juce::Rectangle<int> r, bool lit, juce::Colour c, float lineY = -1.0f)
{
    g.setColour(ui::Bg0.withAlpha(0.6f));
    g.fillRoundedRectangle(r.toFloat(), 5.0f);
    g.setColour(lit ? c.withAlpha(0.35f) : ui::CardEdge);
    g.drawRoundedRectangle(r.toFloat().reduced(0.5f), 5.0f, 1.0f);
    g.setColour(ui::Track.withAlpha(0.7f));
    const float y = lineY >= 0.0f ? lineY : r.toFloat().getCentreY();
    g.drawLine(static_cast<float>(r.getX() + 4), y, static_cast<float>(r.getRight() - 4), y, 1.0f);
}
}

void NoctuaryEditor::ModView::paintCard(juce::Graphics& g, const Card& c, bool hot)
{
    const auto r = c.m_bounds;
    const float v = m_proc.engine().modSource(static_cast<int>(c.m_source));
    const bool used = [&] {
        const ambient::ModMatrix& m = m_proc.engine().modMatrix();
        for (int i = 0; i < m.count(); ++i) if (m.route(i).m_source == c.m_source) return true;
        return false;
    }();
    g.setColour(used ? ui::CardColour.brighter(0.05f) : ui::CardColour.withAlpha(0.55f));
    g.fillRoundedRectangle(r.toFloat(), 4.0f);
    g.setColour(hot ? c.m_colour.withAlpha(0.9f) : (used ? c.m_colour.withAlpha(0.45f) : ui::CardEdge));
    g.drawRoundedRectangle(r.toFloat().reduced(0.5f), 4.0f, hot ? 1.6f : 1.0f);

    // Its shape, small. LFOs and envelopes draw their curve; everything else draws its value.
    const auto plot = r.reduced(5, 4).withTrimmedTop(13);
    const float cy = plot.toFloat().getCentreY(), h = plot.getHeight() * 0.42f;
    using MST = ambient::ModSourceE;
    const int si = static_cast<int>(c.m_source);
    juce::Path p;
    if (si >= static_cast<int>(MST::LFO1) && si <= static_cast<int>(MST::LFO8)) {
        const ambient::LfoSpec sp = specOf(c.m_index);
        const ambient::Lfo& live = m_proc.engine().lfo(c.m_index);
        for (int s = 0; s <= 40; ++s) {
            const float t = s / 40.0f;
            const float y = cy - juce::jlimit(-1.0f, 1.0f, ambient::Lfo::shapeAt(sp, t + sp.m_phase, nullptr, &live) * sp.m_depth) * h;
            const float x = plot.getX() + t * plot.getWidth();
            if (s == 0) p.startNewSubPath(x, y); else p.lineTo(x, y);
        }
    } else if (si >= static_cast<int>(MST::ENV1) && si <= static_cast<int>(MST::ENV6)) {
        const ambient::ModEnv& e = m_proc.engine().envShape(c.m_index);
        const float len = juce::jmax(0.001f, e.length());
        for (int s = 0; s <= 40; ++s) {
            const float t = s / 40.0f;
            const float y = cy - juce::jlimit(-1.0f, 1.0f, e.at(t * len, ambient::EnvModeE::ONE_SHOT, true)) * h;
            const float x = plot.getX() + t * plot.getWidth();
            if (s == 0) p.startNewSubPath(x, y); else p.lineTo(x, y);
        }
    } else {   // a bar for the plain sources
        const float y = cy - juce::jlimit(-1.0f, 1.0f, v) * h;
        g.setColour(c.m_colour.withAlpha(used ? 0.7f : 0.3f));
        g.fillRect(static_cast<float>(plot.getX()), juce::jmin(y, cy), static_cast<float>(plot.getWidth()), std::abs(cy - y) + 1.0f);
    }
    if (!p.isEmpty()) {
        g.setColour(c.m_colour.withAlpha(used ? 0.85f : 0.35f));
        g.strokePath(p, juce::PathStrokeType(1.3f));
    }
    // The live value as a dot on the right edge, so even a still source shows it is alive.
    g.setColour(ui::Live.withAlpha(0.9f));
    g.fillEllipse(static_cast<float>(plot.getRight()) - 2.0f, cy - juce::jlimit(-1.0f, 1.0f, v) * h - 2.0f, 4.0f, 4.0f);

    g.setColour(used ? ui::Text : ui::Dim);
    g.setFont(ui::title(9.5f));
    g.drawText(c.m_label, r.reduced(5, 3), juce::Justification::topLeft, false);
}

void NoctuaryEditor::ModView::paintLane(juce::Graphics& g)
{
    g.setColour(ui::GroupColour);
    g.fillRect(m_lane);
    for (size_t i = 0; i < m_cards.size(); ++i)
        paintCard(g, m_cards[i], static_cast<int>(i) == m_hoverCard || static_cast<int>(i) == m_dragCard);
}

void NoctuaryEditor::ModView::paintLfo(juce::Graphics& g, int i)
{
    const Row& row = m_lfos[static_cast<size_t>(i)];
    const auto r = row.m_curve;
    if (r.isEmpty()) return;
    const ambient::LfoSpec sp = specOf(i);
    const bool lit = sp.m_depth > 0.001f;
    curveFrame(g, r, lit, ui::Accent);
    const ambient::Lfo& live = m_proc.engine().lfo(i);
    juce::Path p;
    const float x0 = static_cast<float>(r.getX() + 5), w = static_cast<float>(r.getWidth() - 10);
    const float cy = r.toFloat().getCentreY(), h = r.getHeight() * 0.36f;
    const int steps = juce::jlimit(48, 400, r.getWidth());
    for (int s = 0; s <= steps; ++s) {
        const float t = static_cast<float>(s) / static_cast<float>(steps);
        const float y = cy - juce::jlimit(-1.0f, 1.0f, ambient::Lfo::shapeAt(sp, t + sp.m_phase, nullptr, &live) * sp.m_depth) * h;
        if (s == 0) p.startNewSubPath(x0 + t * w, y); else p.lineTo(x0 + t * w, y);
    }
    g.setColour(ui::Accent.withAlpha(lit ? 0.22f : 0.08f));
    g.strokePath(p, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(lit ? ui::Accent : ui::Faint);
    g.strokePath(p, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    const float ph = m_proc.engine().lfoPhase(i);
    const float v = m_proc.engine().modSource(static_cast<int>(ambient::ModSourceE::LFO1) + i);
    const float dx = x0 + ph * w, dy = cy - juce::jlimit(-1.0f, 1.0f, v) * h;
    g.setColour(ui::Live.withAlpha(0.30f)); g.fillEllipse(dx - 7.0f, dy - 7.0f, 14.0f, 14.0f);
    g.setColour(ui::Live);                  g.fillEllipse(dx - 3.0f, dy - 3.0f, 6.0f, 6.0f);
    const float period = 1.0f / juce::jmax(1.0e-5f, sp.m_rateHz);
    g.setColour(ui::Dim); g.setFont(ui::body(10.0f));
    g.drawText(period >= 90.0f ? juce::String(period / 60.0f, 1) + " min" : juce::String(period, period < 10.0f ? 2 : 1) + " s",
               r.reduced(7, 3), juce::Justification::topRight, false);
    g.setColour(lit ? ui::Text : ui::Faint); g.setFont(ui::title(10.5f));
    g.drawText(row.m_title, r.reduced(7, 3), juce::Justification::topLeft, false);
}

void NoctuaryEditor::ModView::paintEnv(juce::Graphics& g, int i)
{
    const Row& row = m_envs[static_cast<size_t>(i)];
    const auto r = row.m_curve;
    if (r.isEmpty()) return;
    const ambient::ModEnv& e = shapeOf(i);
    const bool source = i >= ambient::NumModEnvs;
    auto get = [this](const juce::String& key) {
        auto* v = m_proc.m_apvts.getRawParameterValue(key);
        return v != nullptr ? v->load() : 0.0f;
    };
    const float depth = get(envKey(i, "depth")), scale = juce::jmax(0.01f, get(envKey(i, "time")));
    // A source's envelope is heard only where that source's Env is Own: drawn dim otherwise, and
    // the title says what would make it count. One point is a level, and a level is a shape too.
    const bool used = !source
        || juce::roundToInt(get("src" + juce::String(i - ambient::NumModEnvs + 1) + "_env")) == ambient::NumSlotEnvs - 1;
    const bool lit = used && depth > 0.001f && e.count() > (source ? 0 : 1);
    const EnvGeom geom = envGeom(i);
    curveFrame(g, r, lit, ui::ForeCol, geom.m_uni ? geom.m_cy : -1.0f);
    const float x0 = geom.m_x0, w = geom.m_w, len = geom.m_len;
    if (e.loopFrom() >= 0 && e.loopTo() > e.loopFrom() && e.loopTo() < e.count()) {
        const float a = e.point(e.loopFrom()).m_time / len, b = e.point(e.loopTo()).m_time / len;
        g.setColour(ui::ForeCol.withAlpha(0.08f));
        g.fillRect(x0 + a * w, static_cast<float>(r.getY() + 4), (b - a) * w, static_cast<float>(r.getHeight() - 8));
    }
    juce::Path p;
    const int steps = juce::jlimit(48, 400, r.getWidth());
    for (int s = 0; s <= steps; ++s) {
        const float t = static_cast<float>(s) / static_cast<float>(steps) * len;
        const auto pt = envToXY(i, t, e.at(t, ambient::EnvModeE::ONE_SHOT, true));
        if (s == 0) p.startNewSubPath(pt.x, pt.y); else p.lineTo(pt.x, pt.y);
    }
    g.setColour(ui::ForeCol.withAlpha(lit ? 0.22f : 0.08f));
    g.strokePath(p, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(lit ? ui::ForeCol : ui::Faint);
    g.strokePath(p, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    for (int k = 0; k < e.count(); ++k) {
        const ambient::EnvPoint& pt = e.point(k);
        const auto at = envToXY(i, pt.m_time, pt.m_value);
        const float x = at.x, y = at.y;
        const bool hot = (m_hoverEnv == i && m_hoverPoint == k) || (m_dragEnv == i && m_dragPoint == k);
        g.setColour(hot ? ui::Live : (lit ? ui::ForeCol.withAlpha(0.85f) : ui::Faint));
        const float rad = hot ? 4.5f : 2.5f;
        g.fillEllipse(x - rad, y - rad, rad * 2.0f, rad * 2.0f);
        if (k == e.sustain()) { g.setColour(ui::Live); g.drawEllipse(x - 5.0f, y - 5.0f, 10.0f, 10.0f, 1.4f); }
        if (k == e.loopFrom() || k == e.loopTo()) {
            g.setColour(ui::ForeCol.withAlpha(0.55f));
            g.drawLine(x, static_cast<float>(r.getY()) + 3.0f, x, static_cast<float>(r.getBottom()) - 3.0f, 1.0f);
        }
    }
    // The one line that turns a picture into a control. Always there, because a control nobody
    // knows about is not a control -- brighter when the mouse is on this row.
    {
        g.setColour(ui::Dim.withAlpha(m_hoverEnv == i ? 1.0f : 0.60f));
        g.setFont(ui::body(9.5f));
        // ASCII only. A middle dot in a narrow literal reaches the screen as mojibake here, and
        // it did: the first version of this line read "drag a point A. double-click...".
        g.drawText("drag a point   -   double-click to add or remove   -   right-click for shapes",
                   r.reduced(7, 3), juce::Justification::bottomRight, false);
    }
    {   // The playhead. Its value already has Depth in it, so it is placed without applying it
        // again -- envToXY would, which is why this one keeps its own arithmetic. A source's
        // envelope runs in every voice from that voice's own note: the loudest voice's is shown.
        float t = 0.0f, v = 0.0f;
        bool show = true;
        if (source) show = m_proc.engine().displaySlotEnv(i - ambient::NumModEnvs, t, v);
        else {
            t = m_proc.engine().envTime(i) / scale;
            v = m_proc.engine().modSource(static_cast<int>(ambient::ModSourceE::ENV1) + i);
        }
        if (lit && show && t <= len * 1.02f) {
            const float dx = geom.m_x0 + juce::jlimit(0.0f, 1.0f, t / len) * geom.m_w;
            const float dy = geom.m_cy - (source ? juce::jlimit(0.0f, 1.0f, v) : juce::jlimit(-1.0f, 1.0f, v)) * geom.m_h;
            g.setColour(ui::Live.withAlpha(0.30f)); g.fillEllipse(dx - 7.0f, dy - 7.0f, 14.0f, 14.0f);
            g.setColour(ui::Live);                  g.fillEllipse(dx - 3.0f, dy - 3.0f, 6.0f, 6.0f);
        }
    }
    g.setColour(ui::Dim); g.setFont(ui::body(10.0f));
    g.drawText(juce::String(len * scale, len * scale < 10.0f ? 1 : 0) + " s", r.reduced(7, 3), juce::Justification::topRight, false);
    g.setColour(lit ? ui::Text : ui::Faint); g.setFont(ui::title(10.5f));
    g.drawText(used ? row.m_title : row.m_title + "   -   not heard: this source's Env is not Own", r.reduced(7, 3), juce::Justification::topLeft, false);
}

void NoctuaryEditor::ModView::paint(juce::Graphics& g)
{
    g.setColour(ui::Bg1);
    g.fillRect(getLocalBounds());
    g.setColour(ui::CardEdge);
    g.drawLine(0.0f, 0.5f, static_cast<float>(getWidth()), 0.5f, 1.0f);
    paintLane(g);
    if (m_tab == 0) for (int i = 0; i < ambient::NumLfos; ++i) paintLfo(g, i);
    if (m_tab == 1) for (int i = 0; i < EnvRows; ++i) if (envVisible(i)) paintEnv(g, i);

    // While dragging: a line from the card to the mouse, so the gesture is visible.
    if (m_dragCard >= 0) {
        const auto from = m_cards[static_cast<size_t>(m_dragCard)].m_bounds.getCentre().toFloat();
        g.setColour(m_cards[static_cast<size_t>(m_dragCard)].m_colour.withAlpha(0.8f));
        g.drawLine(juce::Line<float>(from, m_dragPos.toFloat()), 2.0f);
        g.fillEllipse(m_dragPos.x - 4.0f, m_dragPos.y - 4.0f, 8.0f, 8.0f);
    }
}

// ---------------------------------------------------------------- strip: mouse

void NoctuaryEditor::ModView::mouseMove(const juce::MouseEvent& e)
{
    int h = -1;
    for (size_t i = 0; i < m_cards.size(); ++i) if (m_cards[i].m_bounds.contains(e.getPosition())) h = static_cast<int>(i);
    if (h != m_hoverCard) { m_hoverCard = h; repaint(m_lane); }
    // Which breakpoint the mouse is over. Drawn larger, and the row says what can be done with
    // it -- an envelope you can drag is worth nothing if nothing suggests dragging it.
    int point = -1;
    const int env = envAt(e.getPosition(), &point);
    if (env != m_hoverEnv || point != m_hoverPoint) {
        m_hoverEnv = env; m_hoverPoint = point;
        setMouseCursor(point >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

// ---------------------------------------------------------------- editing an envelope

/**
 * The engine has always had proper envelopes: up to sixteen breakpoints, a curve on every
 * segment, a sustain point and a loop. What it did not have was any way to reach them. The four
 * knobs under each curve set the mode, the time scale, the depth and the sync -- the SHAPE could
 * only arrive from a preset, so on the panel the curve was a picture of something you could not
 * touch, and the instrument looked as though it could not manage an ADSR.
 *
 * Now: drag a point, double-click to add or remove one, right-click for the sustain point, the
 * loop, the curvature of a segment, and a handful of shapes to start from.
 *
 * envAt is the door to all of that: every mouse handler of the strip asks it first which row is
 * under the pointer and, if one is, which of its breakpoints lies within nine pixels of it.
 */
int NoctuaryEditor::ModView::envAt(juce::Point<int> pos, int* pointOut) const
{
    if (m_tab != 1) return -1;
    for (int i = 0; i < EnvRows; ++i) {
        const auto r = m_envs[static_cast<size_t>(i)].m_curve;
        if (!envVisible(i) || r.isEmpty() || !r.contains(pos)) continue;
        if (pointOut != nullptr) {
            *pointOut = -1;
            const ambient::ModEnv& e = shapeOf(i);
            float best = 9.0e9f;
            for (int k = 0; k < e.count(); ++k) {
                const auto p = envToXY(i, e.point(k).m_time, e.point(k).m_value);
                const float d = p.getDistanceFrom(pos.toFloat());
                if (d < 9.0f && d < best) { best = d; *pointOut = k; }
            }
        }
        return i;
    }
    return -1;
}

/**
 * The same mapping paintEnv draws with, so what is grabbed is what is seen.
 * The geometry of one envelope row, in one place. The drawing and the mouse must agree to the
 * pixel or a point is grabbed next to where it is seen -- which is what the first version did,
 * with 8/16/0.34 against the drawing's 5/10/0.36, and Depth left out of the vertical entirely.
 */
NoctuaryEditor::ModView::EnvGeom NoctuaryEditor::ModView::envGeom(int env) const
{
    const auto r = m_envs[static_cast<size_t>(env)].m_curve;
    const ambient::ModEnv& e = shapeOf(env);
    auto* v = m_proc.m_apvts.getRawParameterValue(envKey(env, "depth"));
    EnvGeom g;
    g.m_x0 = static_cast<float>(r.getX() + 5);
    g.m_w  = juce::jmax(1.0f, static_cast<float>(r.getWidth() - 10));
    // A source's envelope is a level: 0 at the bottom and 1 at the top of the same band of the
    // frame that the six use from -1 to 1, so cy is the floor rather than the middle.
    g.m_uni = env >= ambient::NumModEnvs;
    g.m_cy = g.m_uni ? static_cast<float>(r.getY()) + static_cast<float>(r.getHeight()) * 0.86f : r.toFloat().getCentreY();
    g.m_h  = juce::jmax(1.0f, r.getHeight() * (g.m_uni ? 0.72f : 0.36f));
    g.m_len = juce::jmax(0.001f, e.length());
    // Depth scales what is drawn, so it has to scale what is grabbed. At zero the curve is a flat
    // line and there would be nothing to aim at, so the floor keeps it editable.
    g.m_depth = juce::jmax(0.05f, v != nullptr ? v->load() : 1.0f);
    return g;
}

juce::Point<float> NoctuaryEditor::ModView::envToXY(int env, float time, float value) const
{
    const EnvGeom g = envGeom(env);
    const float x = g.m_x0 + juce::jlimit(0.0f, 1.0f, time / g.m_len) * g.m_w;
    if (g.m_uni)   // what the source is given: Depth lifts the whole contour towards full level
        return { x, g.m_cy - juce::jlimit(0.0f, 1.0f, 1.0f - g.m_depth * (1.0f - juce::jlimit(0.0f, 1.0f, value))) * g.m_h };
    return { x, g.m_cy - juce::jlimit(-1.0f, 1.0f, value * g.m_depth) * g.m_h };
}

void NoctuaryEditor::ModView::envFromXY(int env, juce::Point<int> pos, float& time, float& value) const
{
    const EnvGeom g = envGeom(env);
    time  = juce::jlimit(0.0f, g.m_len, (static_cast<float>(pos.x) - g.m_x0) / g.m_w * g.m_len);
    if (g.m_uni) {
        const float gain = juce::jlimit(0.0f, 1.0f, (g.m_cy - static_cast<float>(pos.y)) / g.m_h);
        value = juce::jlimit(0.0f, 1.0f, 1.0f - (1.0f - gain) / g.m_depth);
        return;
    }
    value = juce::jlimit(-1.0f, 1.0f, (g.m_cy - static_cast<float>(pos.y)) / g.m_h / g.m_depth);
}

ambient::ModEnv NoctuaryEditor::ModView::envCopy(int env) const
{
    return shapeOf(env);
}

void NoctuaryEditor::ModView::envCommit(int env, const ambient::ModEnv& e, const juce::String& what)
{
    char buf[512];
    if (e.write(buf, sizeof(buf)) <= 0) return;
    if (what.isNotEmpty()) m_owner.pushUndo(what);
    if (env < ambient::NumModEnvs) m_proc.engine().setEnvShape(env, buf);
    else m_proc.engine().setSrcEnvShape(env - ambient::NumModEnvs, buf);
    repaint();
}

void NoctuaryEditor::ModView::envShapeMenu(int env, int point)
{
    ambient::ModEnv e = envCopy(env);
    juce::PopupMenu menu;
    if (point >= 0) {
        menu.addSectionHeader("Point " + juce::String(point + 1) + " of " + juce::String(e.count()));
        menu.addItem(10, "Sustain here", true, e.sustain() == point);
        menu.addItem(11, "No sustain point", e.sustain() >= 0);
        menu.addItem(12, "Loop starts here", point < e.count() - 1);
        menu.addItem(13, "Loop ends here", point > 0);
        menu.addItem(14, "No loop", e.loopFrom() >= 0);
        juce::PopupMenu curve;
        curve.addItem(20, "Fast, then slow", true, e.point(point).m_curve < -0.05f);
        curve.addItem(21, "Straight", true, std::fabs(e.point(point).m_curve) <= 0.05f);
        curve.addItem(22, "Slow, then fast", true, e.point(point).m_curve > 0.05f);
        menu.addSubMenu("Curve into this point", curve, point > 0);
        menu.addItem(15, "Delete this point", e.count() > 2);
        menu.addSeparator();
    }
    juce::PopupMenu shapes;
    for (int i = 0; i < ambient::NumEnvShapePresets; ++i) shapes.addItem(100 + i, ambient::EnvShapePresetNames[i]);
    menu.addSubMenu("Shape", shapes);
    menu.showMenuAsync(juce::PopupMenu::Options(), [this, env, point](int r) {
        if (r <= 0) return;
        if (r >= 100) {
            ambient::ModEnv shaped;
            if (shaped.parse(ambient::EnvShapePresetTexts[r - 100])) envCommit(env, shaped, "envelope shape");
            return;
        }
        ambient::ModEnv e = envCopy(env);
        ambient::EnvPoint pts[ambient::MaxEnvPoints];
        int n = e.count();
        for (int k = 0; k < n; ++k) pts[k] = e.point(k);
        int sus = e.sustain(), lo = e.loopFrom(), hi = e.loopTo();
        switch (r) {
        case 10: sus = point; break;
        case 11: sus = -1; break;
        case 12: lo = point; if (hi <= lo) hi = juce::jmin(n - 1, lo + 1); break;
        case 13: hi = point; if (lo < 0 || lo >= hi) lo = juce::jmax(0, hi - 1); break;
        case 14: lo = hi = -1; break;
        case 15:
            if (n > 2 && point > 0) {
                for (int k = point; k + 1 < n; ++k) pts[k] = pts[k + 1];
                --n;
                if (sus >= n) sus = -1;
                if (lo >= n || hi >= n) lo = hi = -1;
            }
            break;
        case 20: pts[point].m_curve = -0.6f; break;
        case 21: pts[point].m_curve = 0.0f; break;
        case 22: pts[point].m_curve = 0.6f; break;
        default: return;
        }
        ambient::ModEnv out;
        if (!out.set(pts, n)) return;
        out.setSustain(sus);
        out.setLoop(lo, hi);
        envCommit(env, out, "envelope");
    });
}

void NoctuaryEditor::ModView::mouseDown(const juce::MouseEvent& e)
{
    {   // On an envelope curve: grab a breakpoint, or open its menu.
        int point = -1;
        const int env = envAt(e.getPosition(), &point);
        if (env >= 0) {
            if (e.mods.isPopupMenu()) { envShapeMenu(env, point); return; }
            if (point >= 0) { m_dragEnv = env; m_dragPoint = point; m_owner.pushUndo("envelope"); return; }
        }
    }
    for (size_t i = 0; i < m_cards.size(); ++i) {
        if (!m_cards[i].m_bounds.contains(e.getPosition())) continue;
        if (e.mods.isPopupMenu()) {   // right-click: what this source drives, and a way to drop it
            const ambient::ModMatrix& m = m_proc.engine().modMatrix();
            juce::PopupMenu menu;
            menu.addSectionHeader(m_cards[i].m_label + " drives");
            int n = 0;
            for (int k = 0; k < m.count(); ++k) {
                if (m.route(k).m_source != m_cards[i].m_source) continue;
                menu.addItem(1000 + k, juce::String(paramDesc(m.route(k).m_target).m_name) + "   "
                             + juce::String(m.route(k).m_depth, 2) + "   (remove)");
                ++n;
            }
            if (n == 0) menu.addItem(1, "nothing yet -- drag this card onto a knob", false, false);
            menu.showMenuAsync(juce::PopupMenu::Options(), [this](int r) {
                if (r >= 1000) {
                    char buf[4096];
                    const int len = m_proc.engine().writeModMatrix(buf, sizeof(buf));
                    juce::StringArray rows = juce::StringArray::fromTokens(
                        juce::String(juce::CharPointer_UTF8(buf), static_cast<size_t>(juce::jmax(0, len))), ";", "");
                    rows.remove(r - 1000);
                    m_proc.engine().setModMatrixText(rows.joinIntoString(";").toRawUTF8());
                    if (m_tab == 2) pullMatrix();
                    m_owner.repaint();
                }
            });
            return;
        }
        m_dragCard = static_cast<int>(i);
        m_dragPos = e.getPosition();
        if (m_cards[i].m_tab == 1 && m_envPage != 0) setEnvPage(0);   // an envelope card is one of the six
        if (m_cards[i].m_tab != 2) setTab(m_cards[i].m_tab);
        repaint();
        return;
    }
}

/**
 * Double click: on a point, remove it; on the curve, put one there. Sixteen is the engine's
 * limit, and past it the click does nothing rather than silently dropping a point somewhere else.
 */
void NoctuaryEditor::ModView::mouseDoubleClick(const juce::MouseEvent& e)
{
    int point = -1;
    const int env = envAt(e.getPosition(), &point);
    if (env < 0) return;
    ambient::ModEnv src = envCopy(env);
    ambient::EnvPoint pts[ambient::MaxEnvPoints];
    int n = src.count();
    for (int k = 0; k < n; ++k) pts[k] = src.point(k);
    int sus = src.sustain(), lo = src.loopFrom(), hi = src.loopTo();
    if (point > 0) {                                   // remove it -- but never the first
        if (n <= 2) return;
        for (int k = point; k + 1 < n; ++k) pts[k] = pts[k + 1];
        --n;
        if (sus >= n) sus = -1;
        if (lo >= n || hi >= n) lo = hi = -1;
    } else if (point < 0) {                            // add one where the mouse is
        if (n >= ambient::MaxEnvPoints) return;
        float t = 0.0f, v = 0.0f;
        envFromXY(env, e.getPosition(), t, v);
        int at = n;
        for (int k = 0; k < n; ++k) if (pts[k].m_time > t) { at = k; break; }
        if (at == 0) return;                           // nothing goes before the start
        for (int k = n; k > at; --k) pts[k] = pts[k - 1];
        pts[at] = { t, v, 0.0f };
        ++n;
        if (sus >= at) ++sus;
        if (lo >= at) ++lo;
        if (hi >= at) ++hi;
    } else {
        return;
    }
    ambient::ModEnv out;
    if (!out.set(pts, n)) return;
    out.setSustain(sus);
    out.setLoop(lo, hi);
    envCommit(env, out, "envelope");
}

void NoctuaryEditor::ModView::mouseDrag(const juce::MouseEvent& e)
{
    if (m_dragCard >= 0) {
        // The strip can only draw inside itself, and this gesture leaves it at once: the editor
        // draws the lead, the card and the knob it is over, on top of everything.
        m_dragPos = e.getPosition();
        const Card& c = m_cards[static_cast<size_t>(m_dragCard)];
        m_owner.showModDrag(localPointToGlobal(c.m_bounds.getCentre()), e.getScreenPosition(), c.m_label, c.m_colour);
        repaint();
        return;
    }
    if (m_dragEnv >= 0 && m_dragPoint >= 0) {
        ambient::ModEnv src = envCopy(m_dragEnv);
        ambient::EnvPoint pts[ambient::MaxEnvPoints];
        const int n = src.count();
        for (int k = 0; k < n; ++k) pts[k] = src.point(k);
        float t = 0.0f, v = 0.0f;
        envFromXY(m_dragEnv, e.getPosition(), t, v);
        // The first point stays at zero -- an envelope that starts late is a delay, and there is
        // a knob for that. Every other point stays between its neighbours, so the list keeps its
        // order and ModEnv::set never has to reject what the mouse is doing.
        if (m_dragPoint == 0) t = 0.0f;
        else t = juce::jmax(t, pts[m_dragPoint - 1].m_time + 0.001f);
        if (m_dragPoint + 1 < n) t = juce::jmin(t, pts[m_dragPoint + 1].m_time - 0.001f);
        pts[m_dragPoint].m_time = juce::jmax(0.0f, t);
        pts[m_dragPoint].m_value = v;
        ambient::ModEnv out;
        if (out.set(pts, n)) {
            out.setSustain(src.sustain());
            out.setLoop(src.loopFrom(), src.loopTo());
            envCommit(m_dragEnv, out, {});         // the undo point was taken when the drag began
        }
        return;
    }
    if (m_dragCard < 0) return;
    m_dragPos = e.getPosition();
    repaint();
}

void NoctuaryEditor::ModView::mouseUp(const juce::MouseEvent& e)
{
    if (m_dragEnv >= 0) { m_dragEnv = m_dragPoint = -1; repaint(); return; }
    if (m_dragCard >= 0) {
        const int param = m_owner.cellParamAt(e.getScreenPosition());
        const Card& c = m_cards[static_cast<size_t>(m_dragCard)];
        m_owner.hideModDrag();
        if (param >= 0 && addRoute(c.m_source, static_cast<ParamIdE>(param)))
            m_owner.showDepthPopup(c.m_source, static_cast<ParamIdE>(param), c.m_colour, e.getScreenPosition());
        m_dragCard = -1;
        repaint();
    }
}

// ---------------------------------------------------------------- strip: layout

void NoctuaryEditor::ModView::resized()
{
    auto area = getLocalBounds().reduced(10, 8);
    m_lane = area.removeFromTop(56);
    {   // the cards, wrapped over as many rows as they need
        const int cw = 74, ch = 26, gap = 4;
        int x = m_lane.getX(), y = m_lane.getY();
        for (auto& c : m_cards) {
            if (x + cw > m_lane.getRight()) { x = m_lane.getX(); y += ch + gap; }
            c.m_bounds = { x, y, cw, ch };
            x += cw + gap;
        }
    }
    area.removeFromTop(6);
    m_tabsArea = area.removeFromTop(22);
    {
        auto t = m_tabsArea;
        m_tabLfo.setBounds(t.removeFromLeft(90));    t.removeFromLeft(4);
        m_tabEnv.setBounds(t.removeFromLeft(110));   t.removeFromLeft(4);
        m_tabMatrix.setBounds(t.removeFromLeft(90));
        t.removeFromLeft(24);                      // the ENV tab's pages, set a little apart from the tabs
        m_pageMod.setBounds(t.removeFromLeft(130));  t.removeFromLeft(4);
        m_pageSrc.setBounds(t.removeFromLeft(110));
    }
    area.removeFromTop(6);
    m_content = area;

    auto layoutRow = [](Row& row, juce::Rectangle<int> r) {
        row.m_curve = r.removeFromTop(juce::jmax(38, r.getHeight() * 52 / 100));
        r.removeFromTop(3);
        const int n = static_cast<int>(std::min(row.m_controls.size(), row.m_labels.size()));
        if (n <= 0) return;
        const int cw = r.getWidth() / n;
        for (int k = 0; k < n; ++k) {
            auto cell = r.removeFromLeft(cw);
            row.m_labels[static_cast<size_t>(k)]->setBounds(cell.removeFromBottom(12));
            if (dynamic_cast<juce::ComboBox*>(row.m_controls[static_cast<size_t>(k)].get()) != nullptr)
                row.m_controls[static_cast<size_t>(k)]->setBounds(cell.withSizeKeepingCentre(cell.getWidth() - 4, 20));
            else
                row.m_controls[static_cast<size_t>(k)]->setBounds(cell.reduced(2, 0));
        }
    };

    if (m_tab == 0) {
        const int cols = 4, rows = 2;
        const int w = m_content.getWidth() / cols, h = m_content.getHeight() / rows;
        for (int i = 0; i < ambient::NumLfos; ++i)
            layoutRow(m_lfos[static_cast<size_t>(i)],
                      juce::Rectangle<int>(m_content.getX() + (i % cols) * w, m_content.getY() + (i / cols) * h, w, h).reduced(4, 2));
    } else if (m_tab == 1) {
        // The six on three columns, or the four sources' own on two: two rows of the same height.
        const int first = m_envPage == 0 ? 0 : ambient::NumModEnvs;
        const int count = m_envPage == 0 ? ambient::NumModEnvs : ambient::Slots;
        const int cols = m_envPage == 0 ? 3 : 2, rows = 2;
        const int w = m_content.getWidth() / cols, h = m_content.getHeight() / rows;
        for (int i = 0; i < EnvRows; ++i) if (!envVisible(i)) m_envs[static_cast<size_t>(i)].m_curve = {};
        for (int j = 0; j < count; ++j)
            layoutRow(m_envs[static_cast<size_t>(first + j)],
                      juce::Rectangle<int>(m_content.getX() + (j % cols) * w, m_content.getY() + (j / cols) * h, w, h).reduced(4, 2));
    } else {
        auto m = m_content;
        auto bottom = m.removeFromBottom(18);
        m_hint.setBounds(bottom.removeFromLeft(bottom.getWidth() * 2 / 3));
        m_matrixInfo.setBounds(bottom);
        if (m_table) m_table->setBounds(m);
    }
}
