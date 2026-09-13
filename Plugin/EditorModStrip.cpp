#include "EditorCommon.h"

using namespace ambient;

// The modulation strip along the bottom: the cards, the LFO and envelope editors, the matrix.


namespace edt {
juce::Colour sourceColour(ambient::ModSource s)
{
    using MS = ambient::ModSource;
    const int i = static_cast<int>(s);
    if (i >= static_cast<int>(MS::Lfo1) && i <= static_cast<int>(MS::Lfo8)) return ui::accent;
    if (i >= static_cast<int>(MS::Env1) && i <= static_cast<int>(MS::Env6)) return ui::foreCol;
    if (i >= static_cast<int>(MS::MacroA) && i <= static_cast<int>(MS::MacroH)) return ui::morphCol;
    if (i >= static_cast<int>(MS::Kura1) && i <= static_cast<int>(MS::Kura4)) return ui::condCol;
    return ui::cosmosCol;
}
} // namespace edt

NoctuaryEditor::ModView::ModView(NoctuaryProcessor& p, NoctuaryEditor& o)
    : proc(p), owner(o)
{
    auto addKnob = [this](Row& row, const juce::String& key, const juce::String& name) {
        const ParamDesc* d = findParam(key.toRawUTF8());
        if (d == nullptr) return;
        if (d->kind == ParamKind::Choice) {
            auto cb = std::make_unique<juce::ComboBox>();
            for (int i = 0; i < d->numChoices; ++i) cb->addItem(d->choices[i], i + 1);
            cb->setTooltip(ambient::paramHelp(d->id));
            addAndMakeVisible(*cb);
            row.combos.push_back(std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(proc.apvts, key, *cb));
            row.controls.push_back(std::move(cb));
        } else {
            auto s = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
            if (d->unit[0] != 0) s->setTextValueSuffix(juce::String(" ") + d->unit);
            s->setTooltip(ambient::paramHelp(d->id));
            s->setColour(juce::Slider::rotarySliderFillColourId, ui::accent);
            addAndMakeVisible(*s);
            row.sliders.push_back(std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(proc.apvts, key, *s));
            row.controls.push_back(std::move(s));
        }
        owner.registerHelp(row.controls.back().get(), d->id);   // the header's help line covers the strip too
        auto l = std::make_unique<juce::Label>(juce::String(), name);
        l->setJustificationType(juce::Justification::centred);
        l->setFont(ui::body(10.0f));
        l->setColour(juce::Label::textColourId, ui::dim);
        addAndMakeVisible(*l);
        row.labels.push_back(std::move(l));
    };

    for (int i = 0; i < ambient::kNumLfos; ++i) {
        Row& r = lfos[static_cast<size_t>(i)];
        r.title = "LFO " + juce::String(i + 1);
        const juce::String n(i + 1);
        addKnob(r, "lfo" + n + "_shape", "Shape");
        addKnob(r, "lfo" + n + "_rate", "Rate");
        addKnob(r, "lfo" + n + "_phase", "Phase");
        addKnob(r, "lfo" + n + "_depth", "Depth");
        addKnob(r, "lfo" + n + "_mode", "Mode");
        addKnob(r, "lfo" + n + "_table", "Table");
        addKnob(r, "lfo" + n + "_sync", "Sync");
    }
    for (int i = 0; i < ambient::kNumModEnvs; ++i) {
        Row& r = envs[static_cast<size_t>(i)];
        r.title = "ENV " + juce::String(i + 1);
        const juce::String n(i + 1);
        addKnob(r, "env" + n + "_mode", "Mode");
        addKnob(r, "env" + n + "_time", "Time");
        addKnob(r, "env" + n + "_depth", "Depth");
        addKnob(r, "env" + n + "_sync", "Sync");
    }
    // The four sources' own envelopes, on the ENV tab's second page: the same editor and the same
    // four knobs, for a shape that is a level and runs from every note.
    for (int k = 0; k < ambient::kSlots; ++k) {
        Row& r = envs[static_cast<size_t>(ambient::kNumModEnvs + k)];
        r.title = "SOURCE " + juce::String(k + 1);
        const juce::String n(k + 1);
        addKnob(r, "src" + n + "_env_mode", "Mode");
        addKnob(r, "src" + n + "_env_time", "Time");
        addKnob(r, "src" + n + "_env_depth", "Depth");
        addKnob(r, "src" + n + "_env_sync", "Sync");
    }

    // The lane: every source that can drive something, in the order the matrix names them.
    using MS = ambient::ModSource;
    auto card = [this](MS s, const juce::String& label, int t, int idx) {
        cards.push_back({ s, label, sourceColour(s), {}, t, idx });
    };
    for (int i = 0; i < ambient::kNumLfos; ++i) card(static_cast<MS>(static_cast<int>(MS::Lfo1) + i), "LFO " + juce::String(i + 1), 0, i);
    for (int i = 0; i < ambient::kNumModEnvs; ++i) card(static_cast<MS>(static_cast<int>(MS::Env1) + i), "ENV " + juce::String(i + 1), 1, i);
    for (int i = 0; i < 8; ++i) card(static_cast<MS>(static_cast<int>(MS::MacroA) + i), juce::String("MACRO ") + static_cast<char>('A' + i), 2, i);
    for (int i = 0; i < 4; ++i) card(static_cast<MS>(static_cast<int>(MS::Kura1) + i), "KURA " + juce::String(i + 1), 2, i);
    card(MS::Amp, "AMP", 2, 0);
    card(MS::Note, "NOTE", 2, 0);
    card(MS::Velocity, "VELO", 2, 0);
    card(MS::Distance, "DIST", 2, 0);
    card(MS::RandomPerNote, "RAND", 2, 0);
    card(MS::Beat, "BEAT", 2, 0);   // the chord listening to how far out of tune it is
    card(MS::Cascade, "CASC", 2, 0);   // the conductor's excitation: drag it at anything that should swell where the piece is busy

    for (auto* b : { &tabLfo, &tabEnv, &tabMatrix }) {
        b->setClickingTogglesState(true);
        b->setRadioGroupId(4711);
        addAndMakeVisible(*b);
    }
    tabLfo.onClick = [this] { setTab(0); };
    tabEnv.onClick = [this] { setTab(1); };
    tabMatrix.onClick = [this] { setTab(2); };
    tabLfo.setToggleState(true, juce::dontSendNotification);
    for (auto* b : { &pageMod, &pageSrc }) {   // the ENV tab's two pages
        b->setClickingTogglesState(true);
        b->setRadioGroupId(4712);
        addChildComponent(*b);
    }
    pageMod.onClick = [this] { setEnvPage(0); };
    pageSrc.onClick = [this] { setEnvPage(1); };
    pageMod.setToggleState(true, juce::dontSendNotification);

    // The matrix page is a table of routes now (EditorMatrix.cpp); the text box it replaces is
    // still the format everything travels in, one level down.
    table = std::make_unique<edt::RouteTable>(proc, [this] {
        matrixInfo.setText(juce::String(proc.engine().modMatrix().count()) + " of 32 routes", juce::dontSendNotification);
        owner.repaint();
    });
    addChildComponent(*table);
    matrixInfo.setFont(ui::body(11.5f));
    matrixInfo.setColour(juce::Label::textColourId, ui::dim);
    addChildComponent(matrixInfo);
    hint.setText("one row per route: source, target, depth, an optional via source that scales it, and 0..1 -- or drag a card onto a knob",
                 juce::dontSendNotification);
    hint.setFont(ui::body(11.5f));
    hint.setColour(juce::Label::textColourId, ui::faint);
    addChildComponent(hint);
    pullMatrix();
    setTab(0);
    startTimerHz(15);   // was 30: this strip was 54 % of all the drawing the panel did
}

void NoctuaryEditor::ModView::setTab(int t)
{
    tab = t;
    for (int i = 0; i < ambient::kNumLfos; ++i) {
        for (auto& c : lfos[static_cast<size_t>(i)].controls) c->setVisible(t == 0);
        for (auto& l : lfos[static_cast<size_t>(i)].labels) l->setVisible(t == 0);
    }
    for (int i = 0; i < kEnvRows; ++i) {
        const bool on = t == 1 && envVisible(i);
        for (auto& c : envs[static_cast<size_t>(i)].controls) c->setVisible(on);
        for (auto& l : envs[static_cast<size_t>(i)].labels) l->setVisible(on);
    }
    pageMod.setVisible(t == 1);
    pageSrc.setVisible(t == 1);
    juce::Component* const matrixParts[] = { table.get(), &matrixInfo, &hint };
    for (juce::Component* c : matrixParts) c->setVisible(t == 2);
    if (t == 2) pullMatrix();
    tabLfo.setToggleState(t == 0, juce::dontSendNotification);
    tabEnv.setToggleState(t == 1, juce::dontSendNotification);
    tabMatrix.setToggleState(t == 2, juce::dontSendNotification);
    resized();
    repaint();
}

void NoctuaryEditor::ModView::setEnvPage(int page)
{
    envPage = juce::jlimit(0, 1, page);
    pageMod.setToggleState(envPage == 0, juce::dontSendNotification);
    pageSrc.setToggleState(envPage == 1, juce::dontSendNotification);
    hoverEnv = hoverPoint = -1;
    setTab(tab);
}

// Rows 0..5 are the six modulation envelopes, 6..9 the sources' own; one page shows either.
bool NoctuaryEditor::ModView::envVisible(int env) const
{
    return (env < ambient::kNumModEnvs) == (envPage == 0);
}

const ambient::ModEnv& NoctuaryEditor::ModView::shapeOf(int env) const
{
    return env < ambient::kNumModEnvs ? proc.engine().envShape(env) : proc.engine().srcEnvShape(env - ambient::kNumModEnvs);
}

// "env3_depth" for the six, "src2_env_depth" for the sources' own.
juce::String NoctuaryEditor::ModView::envKey(int env, const char* field) const
{
    return env < ambient::kNumModEnvs ? "env" + juce::String(env + 1) + "_" + field
                                      : "src" + juce::String(env - ambient::kNumModEnvs + 1) + "_env_" + field;
}

void NoctuaryEditor::ModView::pullMatrix()
{
    if (table) table->pull();
    matrixInfo.setColour(juce::Label::textColourId, ui::dim);
    matrixInfo.setText(juce::String(proc.engine().modMatrix().count()) + " of 32 routes", juce::dontSendNotification);
}

NoctuaryEditor::ModView::~ModView() = default;   // here, where RouteTable is a complete type

bool NoctuaryEditor::ModView::addRoute(ambient::ModSource src, ParamId target)
{
    char buf[4096];
    const int n = proc.engine().writeModMatrix(buf, sizeof(buf));
    juce::String t(juce::CharPointer_UTF8(buf), static_cast<size_t>(juce::jmax(0, n)));
    if (t.isNotEmpty()) t += ";";
    // A quarter of the target's range: enough to hear, not enough to wreck the preset.
    t += juce::String(ambient::modSourceName(src)) + ">" + paramDesc(target).key + ":0.25";
    if (!proc.engine().setModMatrixText(t.toRawUTF8())) return false;
    if (tab == 2) pullMatrix();
    matrixInfo.setText(juce::String(proc.engine().modMatrix().count()) + " of 32 routes", juce::dontSendNotification);
    owner.repaint();
    return true;
}

void NoctuaryEditor::ModView::timerCallback() { if (isShowing()) repaint(); }

ambient::LfoSpec NoctuaryEditor::ModView::specOf(int i) const
{
    const juce::String n(i + 1);
    auto get = [this](const juce::String& key) {
        auto* v = proc.apvts.getRawParameterValue(key);
        return v != nullptr ? v->load() : 0.0f;
    };
    ambient::LfoSpec sp;
    sp.shape  = static_cast<ambient::LfoShape>(juce::jlimit(0, ambient::kNumLfoShapes - 1,
                    static_cast<int>(std::lround(get("lfo" + n + "_shape")))));
    sp.rateHz = get("lfo" + n + "_rate");
    sp.phase  = get("lfo" + n + "_phase");
    sp.depth  = get("lfo" + n + "_depth");
    sp.table  = static_cast<int>(std::lround(get("lfo" + n + "_table")));
    const int sync = static_cast<int>(std::lround(get("lfo" + n + "_sync")));
    if (ambient::syncOn(sync)) sp.rateHz = static_cast<float>(ambient::syncHz(sync, proc.engine().tempo()));
    return sp;
}


// ---------------------------------------------------------------- strip: drawing

namespace {
// The line is the zero of the curve: the middle for a bipolar shape, the floor (lineY) for a level.
void curveFrame(juce::Graphics& g, juce::Rectangle<int> r, bool lit, juce::Colour c, float lineY = -1.0f)
{
    g.setColour(ui::bg0.withAlpha(0.6f));
    g.fillRoundedRectangle(r.toFloat(), 5.0f);
    g.setColour(lit ? c.withAlpha(0.35f) : ui::cardEdge);
    g.drawRoundedRectangle(r.toFloat().reduced(0.5f), 5.0f, 1.0f);
    g.setColour(ui::track.withAlpha(0.7f));
    const float y = lineY >= 0.0f ? lineY : r.toFloat().getCentreY();
    g.drawLine(static_cast<float>(r.getX() + 4), y, static_cast<float>(r.getRight() - 4), y, 1.0f);
}
}

void NoctuaryEditor::ModView::paintCard(juce::Graphics& g, const Card& c, bool hot)
{
    const auto r = c.bounds;
    const float v = proc.engine().modSource(static_cast<int>(c.source));
    const bool used = [&] {
        const ambient::ModMatrix& m = proc.engine().modMatrix();
        for (int i = 0; i < m.count(); ++i) if (m.route(i).source == c.source) return true;
        return false;
    }();
    g.setColour(used ? ui::card.brighter(0.05f) : ui::card.withAlpha(0.55f));
    g.fillRoundedRectangle(r.toFloat(), 4.0f);
    g.setColour(hot ? c.colour.withAlpha(0.9f) : (used ? c.colour.withAlpha(0.45f) : ui::cardEdge));
    g.drawRoundedRectangle(r.toFloat().reduced(0.5f), 4.0f, hot ? 1.6f : 1.0f);

    // Its shape, small. LFOs and envelopes draw their curve; everything else draws its value.
    const auto plot = r.reduced(5, 4).withTrimmedTop(13);
    const float cy = plot.toFloat().getCentreY(), h = plot.getHeight() * 0.42f;
    using MS = ambient::ModSource;
    const int si = static_cast<int>(c.source);
    juce::Path p;
    if (si >= static_cast<int>(MS::Lfo1) && si <= static_cast<int>(MS::Lfo8)) {
        const ambient::LfoSpec sp = specOf(c.index);
        const ambient::Lfo& live = proc.engine().lfo(c.index);
        for (int s = 0; s <= 40; ++s) {
            const float t = s / 40.0f;
            const float y = cy - juce::jlimit(-1.0f, 1.0f, ambient::Lfo::shapeAt(sp, t + sp.phase, nullptr, &live) * sp.depth) * h;
            const float x = plot.getX() + t * plot.getWidth();
            if (s == 0) p.startNewSubPath(x, y); else p.lineTo(x, y);
        }
    } else if (si >= static_cast<int>(MS::Env1) && si <= static_cast<int>(MS::Env6)) {
        const ambient::ModEnv& e = proc.engine().envShape(c.index);
        const float len = juce::jmax(0.001f, e.length());
        for (int s = 0; s <= 40; ++s) {
            const float t = s / 40.0f;
            const float y = cy - juce::jlimit(-1.0f, 1.0f, e.at(t * len, ambient::EnvMode::OneShot, true)) * h;
            const float x = plot.getX() + t * plot.getWidth();
            if (s == 0) p.startNewSubPath(x, y); else p.lineTo(x, y);
        }
    } else {   // a bar for the plain sources
        const float y = cy - juce::jlimit(-1.0f, 1.0f, v) * h;
        g.setColour(c.colour.withAlpha(used ? 0.7f : 0.3f));
        g.fillRect(static_cast<float>(plot.getX()), juce::jmin(y, cy), static_cast<float>(plot.getWidth()), std::abs(cy - y) + 1.0f);
    }
    if (!p.isEmpty()) {
        g.setColour(c.colour.withAlpha(used ? 0.85f : 0.35f));
        g.strokePath(p, juce::PathStrokeType(1.3f));
    }
    // The live value as a dot on the right edge, so even a still source shows it is alive.
    g.setColour(ui::live.withAlpha(0.9f));
    g.fillEllipse(static_cast<float>(plot.getRight()) - 2.0f, cy - juce::jlimit(-1.0f, 1.0f, v) * h - 2.0f, 4.0f, 4.0f);

    g.setColour(used ? ui::text : ui::dim);
    g.setFont(ui::title(9.5f));
    g.drawText(c.label, r.reduced(5, 3), juce::Justification::topLeft, false);
}

void NoctuaryEditor::ModView::paintLane(juce::Graphics& g)
{
    g.setColour(ui::group);
    g.fillRect(lane);
    for (size_t i = 0; i < cards.size(); ++i)
        paintCard(g, cards[i], static_cast<int>(i) == hoverCard || static_cast<int>(i) == dragCard);
}

void NoctuaryEditor::ModView::paintLfo(juce::Graphics& g, int i)
{
    const Row& row = lfos[static_cast<size_t>(i)];
    const auto r = row.curve;
    if (r.isEmpty()) return;
    const ambient::LfoSpec sp = specOf(i);
    const bool lit = sp.depth > 0.001f;
    curveFrame(g, r, lit, ui::accent);
    const ambient::Lfo& live = proc.engine().lfo(i);
    juce::Path p;
    const float x0 = static_cast<float>(r.getX() + 5), w = static_cast<float>(r.getWidth() - 10);
    const float cy = r.toFloat().getCentreY(), h = r.getHeight() * 0.36f;
    const int steps = juce::jlimit(48, 400, r.getWidth());
    for (int s = 0; s <= steps; ++s) {
        const float t = static_cast<float>(s) / static_cast<float>(steps);
        const float y = cy - juce::jlimit(-1.0f, 1.0f, ambient::Lfo::shapeAt(sp, t + sp.phase, nullptr, &live) * sp.depth) * h;
        if (s == 0) p.startNewSubPath(x0 + t * w, y); else p.lineTo(x0 + t * w, y);
    }
    g.setColour(ui::accent.withAlpha(lit ? 0.22f : 0.08f));
    g.strokePath(p, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(lit ? ui::accent : ui::faint);
    g.strokePath(p, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    const float ph = proc.engine().lfoPhase(i);
    const float v = proc.engine().modSource(static_cast<int>(ambient::ModSource::Lfo1) + i);
    const float dx = x0 + ph * w, dy = cy - juce::jlimit(-1.0f, 1.0f, v) * h;
    g.setColour(ui::live.withAlpha(0.30f)); g.fillEllipse(dx - 7.0f, dy - 7.0f, 14.0f, 14.0f);
    g.setColour(ui::live);                  g.fillEllipse(dx - 3.0f, dy - 3.0f, 6.0f, 6.0f);
    const float period = 1.0f / juce::jmax(1.0e-5f, sp.rateHz);
    g.setColour(ui::dim); g.setFont(ui::body(10.0f));
    g.drawText(period >= 90.0f ? juce::String(period / 60.0f, 1) + " min" : juce::String(period, period < 10.0f ? 2 : 1) + " s",
               r.reduced(7, 3), juce::Justification::topRight, false);
    g.setColour(lit ? ui::text : ui::faint); g.setFont(ui::title(10.5f));
    g.drawText(row.title, r.reduced(7, 3), juce::Justification::topLeft, false);
}

void NoctuaryEditor::ModView::paintEnv(juce::Graphics& g, int i)
{
    const Row& row = envs[static_cast<size_t>(i)];
    const auto r = row.curve;
    if (r.isEmpty()) return;
    const ambient::ModEnv& e = shapeOf(i);
    const bool source = i >= ambient::kNumModEnvs;
    auto get = [this](const juce::String& key) {
        auto* v = proc.apvts.getRawParameterValue(key);
        return v != nullptr ? v->load() : 0.0f;
    };
    const float depth = get(envKey(i, "depth")), scale = juce::jmax(0.01f, get(envKey(i, "time")));
    // A source's envelope is heard only where that source's Env is Own: drawn dim otherwise, and
    // the title says what would make it count. One point is a level, and a level is a shape too.
    const bool used = !source
        || juce::roundToInt(get("src" + juce::String(i - ambient::kNumModEnvs + 1) + "_env")) == ambient::kNumSlotEnvs - 1;
    const bool lit = used && depth > 0.001f && e.count() > (source ? 0 : 1);
    const EnvGeom geom = envGeom(i);
    curveFrame(g, r, lit, ui::foreCol, geom.uni ? geom.cy : -1.0f);
    const float x0 = geom.x0, w = geom.w, len = geom.len;
    if (e.loopFrom() >= 0 && e.loopTo() > e.loopFrom() && e.loopTo() < e.count()) {
        const float a = e.point(e.loopFrom()).time / len, b = e.point(e.loopTo()).time / len;
        g.setColour(ui::foreCol.withAlpha(0.08f));
        g.fillRect(x0 + a * w, static_cast<float>(r.getY() + 4), (b - a) * w, static_cast<float>(r.getHeight() - 8));
    }
    juce::Path p;
    const int steps = juce::jlimit(48, 400, r.getWidth());
    for (int s = 0; s <= steps; ++s) {
        const float t = static_cast<float>(s) / static_cast<float>(steps) * len;
        const auto pt = envToXY(i, t, e.at(t, ambient::EnvMode::OneShot, true));
        if (s == 0) p.startNewSubPath(pt.x, pt.y); else p.lineTo(pt.x, pt.y);
    }
    g.setColour(ui::foreCol.withAlpha(lit ? 0.22f : 0.08f));
    g.strokePath(p, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(lit ? ui::foreCol : ui::faint);
    g.strokePath(p, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    for (int k = 0; k < e.count(); ++k) {
        const ambient::EnvPoint& pt = e.point(k);
        const auto at = envToXY(i, pt.time, pt.value);
        const float x = at.x, y = at.y;
        const bool hot = (hoverEnv == i && hoverPoint == k) || (dragEnv == i && dragPoint == k);
        g.setColour(hot ? ui::live : (lit ? ui::foreCol.withAlpha(0.85f) : ui::faint));
        const float rad = hot ? 4.5f : 2.5f;
        g.fillEllipse(x - rad, y - rad, rad * 2.0f, rad * 2.0f);
        if (k == e.sustain()) { g.setColour(ui::live); g.drawEllipse(x - 5.0f, y - 5.0f, 10.0f, 10.0f, 1.4f); }
        if (k == e.loopFrom() || k == e.loopTo()) {
            g.setColour(ui::foreCol.withAlpha(0.55f));
            g.drawLine(x, static_cast<float>(r.getY()) + 3.0f, x, static_cast<float>(r.getBottom()) - 3.0f, 1.0f);
        }
    }
    // The one line that turns a picture into a control. Always there, because a control nobody
    // knows about is not a control -- brighter when the mouse is on this row.
    {
        g.setColour(ui::dim.withAlpha(hoverEnv == i ? 1.0f : 0.60f));
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
        if (source) show = proc.engine().displaySlotEnv(i - ambient::kNumModEnvs, t, v);
        else {
            t = proc.engine().envTime(i) / scale;
            v = proc.engine().modSource(static_cast<int>(ambient::ModSource::Env1) + i);
        }
        if (lit && show && t <= len * 1.02f) {
            const float dx = geom.x0 + juce::jlimit(0.0f, 1.0f, t / len) * geom.w;
            const float dy = geom.cy - (source ? juce::jlimit(0.0f, 1.0f, v) : juce::jlimit(-1.0f, 1.0f, v)) * geom.h;
            g.setColour(ui::live.withAlpha(0.30f)); g.fillEllipse(dx - 7.0f, dy - 7.0f, 14.0f, 14.0f);
            g.setColour(ui::live);                  g.fillEllipse(dx - 3.0f, dy - 3.0f, 6.0f, 6.0f);
        }
    }
    g.setColour(ui::dim); g.setFont(ui::body(10.0f));
    g.drawText(juce::String(len * scale, len * scale < 10.0f ? 1 : 0) + " s", r.reduced(7, 3), juce::Justification::topRight, false);
    g.setColour(lit ? ui::text : ui::faint); g.setFont(ui::title(10.5f));
    g.drawText(used ? row.title : row.title + "   -   not heard: this source's Env is not Own", r.reduced(7, 3), juce::Justification::topLeft, false);
}

void NoctuaryEditor::ModView::paint(juce::Graphics& g)
{
    g.setColour(ui::bg1);
    g.fillRect(getLocalBounds());
    g.setColour(ui::cardEdge);
    g.drawLine(0.0f, 0.5f, static_cast<float>(getWidth()), 0.5f, 1.0f);
    paintLane(g);
    if (tab == 0) for (int i = 0; i < ambient::kNumLfos; ++i) paintLfo(g, i);
    if (tab == 1) for (int i = 0; i < kEnvRows; ++i) if (envVisible(i)) paintEnv(g, i);

    // While dragging: a line from the card to the mouse, so the gesture is visible.
    if (dragCard >= 0) {
        const auto from = cards[static_cast<size_t>(dragCard)].bounds.getCentre().toFloat();
        g.setColour(cards[static_cast<size_t>(dragCard)].colour.withAlpha(0.8f));
        g.drawLine(juce::Line<float>(from, dragPos.toFloat()), 2.0f);
        g.fillEllipse(dragPos.x - 4.0f, dragPos.y - 4.0f, 8.0f, 8.0f);
    }
}

// ---------------------------------------------------------------- strip: mouse

void NoctuaryEditor::ModView::mouseMove(const juce::MouseEvent& e)
{
    int h = -1;
    for (size_t i = 0; i < cards.size(); ++i) if (cards[i].bounds.contains(e.getPosition())) h = static_cast<int>(i);
    if (h != hoverCard) { hoverCard = h; repaint(lane); }
    // Which breakpoint the mouse is over. Drawn larger, and the row says what can be done with
    // it -- an envelope you can drag is worth nothing if nothing suggests dragging it.
    int point = -1;
    const int env = envAt(e.getPosition(), &point);
    if (env != hoverEnv || point != hoverPoint) {
        hoverEnv = env; hoverPoint = point;
        setMouseCursor(point >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

// ---------------------------------------------------------------- editing an envelope
//
// The engine has always had proper envelopes: up to sixteen breakpoints, a curve on every
// segment, a sustain point and a loop. What it did not have was any way to reach them. The four
// knobs under each curve set the mode, the time scale, the depth and the sync -- the SHAPE could
// only arrive from a preset, so on the panel the curve was a picture of something you could not
// touch, and the instrument looked as though it could not manage an ADSR.
//
// Now: drag a point, double-click to add or remove one, right-click for the sustain point, the
// loop, the curvature of a segment, and a handful of shapes to start from.

int NoctuaryEditor::ModView::envAt(juce::Point<int> pos, int* pointOut) const
{
    if (tab != 1) return -1;
    for (int i = 0; i < kEnvRows; ++i) {
        const auto r = envs[static_cast<size_t>(i)].curve;
        if (!envVisible(i) || r.isEmpty() || !r.contains(pos)) continue;
        if (pointOut != nullptr) {
            *pointOut = -1;
            const ambient::ModEnv& e = shapeOf(i);
            float best = 9.0e9f;
            for (int k = 0; k < e.count(); ++k) {
                const auto p = envToXY(i, e.point(k).time, e.point(k).value);
                const float d = p.getDistanceFrom(pos.toFloat());
                if (d < 9.0f && d < best) { best = d; *pointOut = k; }
            }
        }
        return i;
    }
    return -1;
}

// The same mapping paintEnv draws with, so what is grabbed is what is seen.
// The geometry of one envelope row, in one place. The drawing and the mouse must agree to the
// pixel or a point is grabbed next to where it is seen -- which is what the first version did,
// with 8/16/0.34 against the drawing's 5/10/0.36, and Depth left out of the vertical entirely.
NoctuaryEditor::ModView::EnvGeom NoctuaryEditor::ModView::envGeom(int env) const
{
    const auto r = envs[static_cast<size_t>(env)].curve;
    const ambient::ModEnv& e = shapeOf(env);
    auto* v = proc.apvts.getRawParameterValue(envKey(env, "depth"));
    EnvGeom g;
    g.x0 = static_cast<float>(r.getX() + 5);
    g.w  = juce::jmax(1.0f, static_cast<float>(r.getWidth() - 10));
    // A source's envelope is a level: 0 at the bottom and 1 at the top of the same band of the
    // frame that the six use from -1 to 1, so cy is the floor rather than the middle.
    g.uni = env >= ambient::kNumModEnvs;
    g.cy = g.uni ? static_cast<float>(r.getY()) + static_cast<float>(r.getHeight()) * 0.86f : r.toFloat().getCentreY();
    g.h  = juce::jmax(1.0f, r.getHeight() * (g.uni ? 0.72f : 0.36f));
    g.len = juce::jmax(0.001f, e.length());
    // Depth scales what is drawn, so it has to scale what is grabbed. At zero the curve is a flat
    // line and there would be nothing to aim at, so the floor keeps it editable.
    g.depth = juce::jmax(0.05f, v != nullptr ? v->load() : 1.0f);
    return g;
}

juce::Point<float> NoctuaryEditor::ModView::envToXY(int env, float time, float value) const
{
    const EnvGeom g = envGeom(env);
    const float x = g.x0 + juce::jlimit(0.0f, 1.0f, time / g.len) * g.w;
    if (g.uni)   // what the source is given: Depth lifts the whole contour towards full level
        return { x, g.cy - juce::jlimit(0.0f, 1.0f, 1.0f - g.depth * (1.0f - juce::jlimit(0.0f, 1.0f, value))) * g.h };
    return { x, g.cy - juce::jlimit(-1.0f, 1.0f, value * g.depth) * g.h };
}

void NoctuaryEditor::ModView::envFromXY(int env, juce::Point<int> pos, float& time, float& value) const
{
    const EnvGeom g = envGeom(env);
    time  = juce::jlimit(0.0f, g.len, (static_cast<float>(pos.x) - g.x0) / g.w * g.len);
    if (g.uni) {
        const float gain = juce::jlimit(0.0f, 1.0f, (g.cy - static_cast<float>(pos.y)) / g.h);
        value = juce::jlimit(0.0f, 1.0f, 1.0f - (1.0f - gain) / g.depth);
        return;
    }
    value = juce::jlimit(-1.0f, 1.0f, (g.cy - static_cast<float>(pos.y)) / g.h / g.depth);
}

ambient::ModEnv NoctuaryEditor::ModView::envCopy(int env) const
{
    return shapeOf(env);
}

void NoctuaryEditor::ModView::envCommit(int env, const ambient::ModEnv& e, const juce::String& what)
{
    char buf[512];
    if (e.write(buf, sizeof(buf)) <= 0) return;
    if (what.isNotEmpty()) owner.pushUndo(what);
    if (env < ambient::kNumModEnvs) proc.engine().setEnvShape(env, buf);
    else proc.engine().setSrcEnvShape(env - ambient::kNumModEnvs, buf);
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
        curve.addItem(20, "Fast, then slow", true, e.point(point).curve < -0.05f);
        curve.addItem(21, "Straight", true, std::fabs(e.point(point).curve) <= 0.05f);
        curve.addItem(22, "Slow, then fast", true, e.point(point).curve > 0.05f);
        menu.addSubMenu("Curve into this point", curve, point > 0);
        menu.addItem(15, "Delete this point", e.count() > 2);
        menu.addSeparator();
    }
    juce::PopupMenu shapes;
    for (int i = 0; i < ambient::kNumEnvShapePresets; ++i) shapes.addItem(100 + i, ambient::kEnvShapePresetNames[i]);
    menu.addSubMenu("Shape", shapes);
    menu.showMenuAsync(juce::PopupMenu::Options(), [this, env, point](int r) {
        if (r <= 0) return;
        if (r >= 100) {
            ambient::ModEnv shaped;
            if (shaped.parse(ambient::kEnvShapePresetTexts[r - 100])) envCommit(env, shaped, "envelope shape");
            return;
        }
        ambient::ModEnv e = envCopy(env);
        ambient::EnvPoint pts[ambient::kMaxEnvPoints];
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
        case 20: pts[point].curve = -0.6f; break;
        case 21: pts[point].curve = 0.0f; break;
        case 22: pts[point].curve = 0.6f; break;
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
            if (point >= 0) { dragEnv = env; dragPoint = point; owner.pushUndo("envelope"); return; }
        }
    }
    for (size_t i = 0; i < cards.size(); ++i) {
        if (!cards[i].bounds.contains(e.getPosition())) continue;
        if (e.mods.isPopupMenu()) {   // right-click: what this source drives, and a way to drop it
            const ambient::ModMatrix& m = proc.engine().modMatrix();
            juce::PopupMenu menu;
            menu.addSectionHeader(cards[i].label + " drives");
            int n = 0;
            for (int k = 0; k < m.count(); ++k) {
                if (m.route(k).source != cards[i].source) continue;
                menu.addItem(1000 + k, juce::String(paramDesc(m.route(k).target).name) + "   "
                             + juce::String(m.route(k).depth, 2) + "   (remove)");
                ++n;
            }
            if (n == 0) menu.addItem(1, "nothing yet -- drag this card onto a knob", false, false);
            menu.showMenuAsync(juce::PopupMenu::Options(), [this](int r) {
                if (r >= 1000) {
                    char buf[4096];
                    const int len = proc.engine().writeModMatrix(buf, sizeof(buf));
                    juce::StringArray rows = juce::StringArray::fromTokens(
                        juce::String(juce::CharPointer_UTF8(buf), static_cast<size_t>(juce::jmax(0, len))), ";", "");
                    rows.remove(r - 1000);
                    proc.engine().setModMatrixText(rows.joinIntoString(";").toRawUTF8());
                    if (tab == 2) pullMatrix();
                    owner.repaint();
                }
            });
            return;
        }
        dragCard = static_cast<int>(i);
        dragPos = e.getPosition();
        if (cards[i].tab == 1 && envPage != 0) setEnvPage(0);   // an envelope card is one of the six
        if (cards[i].tab != 2) setTab(cards[i].tab);
        repaint();
        return;
    }
}

// Double click: on a point, remove it; on the curve, put one there. Sixteen is the engine's
// limit, and past it the click does nothing rather than silently dropping a point somewhere else.
void NoctuaryEditor::ModView::mouseDoubleClick(const juce::MouseEvent& e)
{
    int point = -1;
    const int env = envAt(e.getPosition(), &point);
    if (env < 0) return;
    ambient::ModEnv src = envCopy(env);
    ambient::EnvPoint pts[ambient::kMaxEnvPoints];
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
        if (n >= ambient::kMaxEnvPoints) return;
        float t = 0.0f, v = 0.0f;
        envFromXY(env, e.getPosition(), t, v);
        int at = n;
        for (int k = 0; k < n; ++k) if (pts[k].time > t) { at = k; break; }
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
    if (dragCard >= 0) {
        // The strip can only draw inside itself, and this gesture leaves it at once: the editor
        // draws the lead, the card and the knob it is over, on top of everything.
        dragPos = e.getPosition();
        const Card& c = cards[static_cast<size_t>(dragCard)];
        owner.showModDrag(localPointToGlobal(c.bounds.getCentre()), e.getScreenPosition(), c.label, c.colour);
        repaint();
        return;
    }
    if (dragEnv >= 0 && dragPoint >= 0) {
        ambient::ModEnv src = envCopy(dragEnv);
        ambient::EnvPoint pts[ambient::kMaxEnvPoints];
        const int n = src.count();
        for (int k = 0; k < n; ++k) pts[k] = src.point(k);
        float t = 0.0f, v = 0.0f;
        envFromXY(dragEnv, e.getPosition(), t, v);
        // The first point stays at zero -- an envelope that starts late is a delay, and there is
        // a knob for that. Every other point stays between its neighbours, so the list keeps its
        // order and ModEnv::set never has to reject what the mouse is doing.
        if (dragPoint == 0) t = 0.0f;
        else t = juce::jmax(t, pts[dragPoint - 1].time + 0.001f);
        if (dragPoint + 1 < n) t = juce::jmin(t, pts[dragPoint + 1].time - 0.001f);
        pts[dragPoint].time = juce::jmax(0.0f, t);
        pts[dragPoint].value = v;
        ambient::ModEnv out;
        if (out.set(pts, n)) {
            out.setSustain(src.sustain());
            out.setLoop(src.loopFrom(), src.loopTo());
            envCommit(dragEnv, out, {});         // the undo point was taken when the drag began
        }
        return;
    }
    if (dragCard < 0) return;
    dragPos = e.getPosition();
    repaint();
}

void NoctuaryEditor::ModView::mouseUp(const juce::MouseEvent& e)
{
    if (dragEnv >= 0) { dragEnv = dragPoint = -1; repaint(); return; }
    if (dragCard >= 0) {
        const int param = owner.cellParamAt(e.getScreenPosition());
        const Card& c = cards[static_cast<size_t>(dragCard)];
        owner.hideModDrag();
        if (param >= 0 && addRoute(c.source, static_cast<ParamId>(param)))
            owner.showDepthPopup(c.source, static_cast<ParamId>(param), c.colour, e.getScreenPosition());
        dragCard = -1;
        repaint();
    }
}

// ---------------------------------------------------------------- strip: layout

void NoctuaryEditor::ModView::resized()
{
    auto area = getLocalBounds().reduced(10, 8);
    lane = area.removeFromTop(56);
    {   // the cards, wrapped over as many rows as they need
        const int cw = 74, ch = 26, gap = 4;
        int x = lane.getX(), y = lane.getY();
        for (auto& c : cards) {
            if (x + cw > lane.getRight()) { x = lane.getX(); y += ch + gap; }
            c.bounds = { x, y, cw, ch };
            x += cw + gap;
        }
    }
    area.removeFromTop(6);
    tabsArea = area.removeFromTop(22);
    {
        auto t = tabsArea;
        tabLfo.setBounds(t.removeFromLeft(90));    t.removeFromLeft(4);
        tabEnv.setBounds(t.removeFromLeft(110));   t.removeFromLeft(4);
        tabMatrix.setBounds(t.removeFromLeft(90));
        t.removeFromLeft(24);                      // the ENV tab's pages, set a little apart from the tabs
        pageMod.setBounds(t.removeFromLeft(130));  t.removeFromLeft(4);
        pageSrc.setBounds(t.removeFromLeft(110));
    }
    area.removeFromTop(6);
    content = area;

    auto layoutRow = [](Row& row, juce::Rectangle<int> r) {
        row.curve = r.removeFromTop(juce::jmax(38, r.getHeight() * 52 / 100));
        r.removeFromTop(3);
        const int n = static_cast<int>(std::min(row.controls.size(), row.labels.size()));
        if (n <= 0) return;
        const int cw = r.getWidth() / n;
        for (int k = 0; k < n; ++k) {
            auto cell = r.removeFromLeft(cw);
            row.labels[static_cast<size_t>(k)]->setBounds(cell.removeFromBottom(12));
            if (dynamic_cast<juce::ComboBox*>(row.controls[static_cast<size_t>(k)].get()) != nullptr)
                row.controls[static_cast<size_t>(k)]->setBounds(cell.withSizeKeepingCentre(cell.getWidth() - 4, 20));
            else
                row.controls[static_cast<size_t>(k)]->setBounds(cell.reduced(2, 0));
        }
    };

    if (tab == 0) {
        const int cols = 4, rows = 2;
        const int w = content.getWidth() / cols, h = content.getHeight() / rows;
        for (int i = 0; i < ambient::kNumLfos; ++i)
            layoutRow(lfos[static_cast<size_t>(i)],
                      juce::Rectangle<int>(content.getX() + (i % cols) * w, content.getY() + (i / cols) * h, w, h).reduced(4, 2));
    } else if (tab == 1) {
        // The six on three columns, or the four sources' own on two: two rows of the same height.
        const int first = envPage == 0 ? 0 : ambient::kNumModEnvs;
        const int count = envPage == 0 ? ambient::kNumModEnvs : ambient::kSlots;
        const int cols = envPage == 0 ? 3 : 2, rows = 2;
        const int w = content.getWidth() / cols, h = content.getHeight() / rows;
        for (int i = 0; i < kEnvRows; ++i) if (!envVisible(i)) envs[static_cast<size_t>(i)].curve = {};
        for (int j = 0; j < count; ++j)
            layoutRow(envs[static_cast<size_t>(first + j)],
                      juce::Rectangle<int>(content.getX() + (j % cols) * w, content.getY() + (j / cols) * h, w, h).reduced(4, 2));
    } else {
        auto m = content;
        auto bottom = m.removeFromBottom(18);
        hint.setBounds(bottom.removeFromLeft(bottom.getWidth() * 2 / 3));
        matrixInfo.setBounds(bottom);
        if (table) table->setBounds(m);
    }
}
