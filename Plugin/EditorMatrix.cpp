// Noctuary -- the modulation matrix as a table of routes.
//
// The MATRIX tab used to be a text box: one route per line, "lfo1>cutoff:0.4", with an Apply
// button. Exact, scriptable, and the wrong thing to put in front of a musician -- somebody who
// opens a tab called Matrix expects to see the routes, not their spelling. A grid of every source
// against every parameter is not the answer either: thirty-odd sources by four hundred targets
// is a wall with a dozen live cells in it. What a Pigments or a Bitwig shows is what this shows:
// one row per route, each row a source, a target, a depth you can drag, an optional second
// source that scales it, and whether the source is read as 0..1 or -1..1.
//
// The table edits a copy and hands the whole matrix back to the engine as the same text the box
// used to, so the parser and the presets see nothing new -- and dragging a card onto a knob
// still adds a row here, because it goes through the same door.
#include "EditorCommon.h"

using namespace ambient;

namespace edt {

namespace {
constexpr int kRowH = 26, kHeadH = 18, kGap = 4;

int sourceItemId(ModSource s) { return static_cast<int>(s) + 1; }   // combo ids start at 1
ModSource sourceFromItem(int id) { return static_cast<ModSource>(juce::jlimit(0, kNumModSources - 1, id - 1)); }
}

struct RouteTable::Row : juce::Component {
    Row(RouteTable& t, int index) : table(t), idx(index)
    {
        for (int s = 1; s < kNumModSources; ++s) source.addItem(modSourceName(static_cast<ModSource>(s)), sourceItemId(static_cast<ModSource>(s)));
        via.addItem("-", sourceItemId(ModSource::None));
        for (int s = 1; s < kNumModSources; ++s) via.addItem(modSourceName(static_cast<ModSource>(s)), sourceItemId(static_cast<ModSource>(s)));
        // Targets grouped by section, in the table's order; the performance parameters are
        // not modulation targets and stay out.
        juce::String section;
        for (int i = 0; i < kNumParams; ++i) {
            const ParamDesc& d = paramTable()[static_cast<size_t>(i)];
            if (isPerformanceParam(static_cast<ParamId>(i))) continue;
            if (juce::String(d.section) != section) { section = d.section; target.addSectionHeading(section); }
            target.addItem(juce::String(d.section) + ": " + d.name, i + 1);
        }
        depth.setRange(-1.0, 1.0, 0.001);
        // A plain horizontal slider with the number beside it: the bar style hid the value.
        depth.setSliderStyle(juce::Slider::LinearHorizontal);
        depth.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 52, 20);
        depth.setNumDecimalPlacesToDisplay(2);
        depth.setDoubleClickReturnValue(true, 0.25);
        uni.setButtonText("0..1");
        uni.setTooltip("Read the source as 0..1 instead of -1..1: an LFO then only adds, never subtracts");
        remove.setButtonText("x");
        remove.setTooltip("Remove this route");
        for (juce::Component* c : { static_cast<juce::Component*>(&source), static_cast<juce::Component*>(&target),
                                    static_cast<juce::Component*>(&depth), static_cast<juce::Component*>(&via),
                                    static_cast<juce::Component*>(&uni), static_cast<juce::Component*>(&remove) })
            addAndMakeVisible(*c);
        source.onChange = [this] { table.changed(); };
        target.onChange = [this] { table.changed(); };
        via.onChange    = [this] { table.changed(); };
        uni.onClick     = [this] { table.changed(); };
        depth.onValueChange = [this] { table.changed(); };
        remove.onClick  = [this] { table.removeRow(idx); };
    }
    void set(const ModRoute& r)
    {
        source.setSelectedId(sourceItemId(r.source), juce::dontSendNotification);
        target.setSelectedId(static_cast<int>(r.target) + 1, juce::dontSendNotification);
        depth.setValue(r.depth, juce::dontSendNotification);
        via.setSelectedId(sourceItemId(r.via), juce::dontSendNotification);
        uni.setToggleState(r.unipolar, juce::dontSendNotification);
    }
    ModRoute get() const
    {
        ModRoute r;
        r.source = sourceFromItem(source.getSelectedId());
        r.target = static_cast<ParamId>(juce::jlimit(0, kNumParams - 1, target.getSelectedId() - 1));
        r.depth = static_cast<float>(depth.getValue());
        r.via = sourceFromItem(via.getSelectedId());
        r.unipolar = uni.getToggleState();
        return r;
    }
    void resized() override
    {
        auto a = getLocalBounds().reduced(2, 2);
        source.setBounds(a.removeFromLeft(96));  a.removeFromLeft(kGap);
        target.setBounds(a.removeFromLeft(210)); a.removeFromLeft(kGap);
        remove.setBounds(a.removeFromRight(24)); a.removeFromRight(kGap);
        uni.setBounds(a.removeFromRight(56));    a.removeFromRight(kGap);
        via.setBounds(a.removeFromRight(96));    a.removeFromRight(kGap);
        depth.setBounds(a);
    }
    RouteTable& table;
    int idx;
    juce::ComboBox source, target, via;
    juce::Slider depth;
    juce::ToggleButton uni;
    juce::TextButton remove;
};

RouteTable::RouteTable(NoctuaryProcessor& p, std::function<void()> onChanged)
    : proc(p), changedCallback(std::move(onChanged))
{
    port.setViewedComponent(&content, false);
    port.setScrollBarsShown(true, false);
    addAndMakeVisible(port);
    add.setButtonText("+ route");
    add.setTooltip("A new route, LFO 1 to the cutoff at a quarter: change it to what you mean");
    add.onClick = [this] { addRow(); };
    addAndMakeVisible(add);
    clear.setButtonText("Clear");
    clear.onClick = [this] { rows.clear(); content.removeAllChildren(); push(); };
    addAndMakeVisible(clear);
    // Column heads placed with the rows' own geometry (see resized), not spaced with blanks: a
    // string of spaces lines up with nothing once the strip is any other width.
    struct { juce::Label* l; const char* t; } heads[] = {
        { &headSource, "source" }, { &headTarget, "target" }, { &headDepth, "depth  (-1 .. 1 of the target's range)" },
        { &headVia, "via" }, { &headUni, "0..1" } };
    for (auto& h : heads) {
        h.l->setFont(ui::body(10.0f));
        h.l->setColour(juce::Label::textColourId, ui::dim);
        h.l->setText(h.t, juce::dontSendNotification);
        addAndMakeVisible(*h.l);
    }
}

RouteTable::~RouteTable() = default;

void RouteTable::pull()
{
    const ModMatrix& m = proc.engine().modMatrix();
    while (static_cast<int>(rows.size()) > m.count()) { content.removeChildComponent(rows.back().get()); rows.pop_back(); }
    while (static_cast<int>(rows.size()) < m.count()) {
        rows.push_back(std::make_unique<Row>(*this, static_cast<int>(rows.size())));
        content.addAndMakeVisible(*rows.back());
    }
    for (int i = 0; i < m.count(); ++i) { rows[static_cast<size_t>(i)]->idx = i; rows[static_cast<size_t>(i)]->set(m.route(i)); }
    resized();
}

void RouteTable::push()
{
    // The whole matrix back to the engine as text -- the one door every route goes through, so
    // the parser, the presets and the drag-and-drop never see a second format.
    juce::String t;
    for (auto& r : rows) {
        const ModRoute m = r->get();
        if (m.source == ModSource::None) continue;
        if (t.isNotEmpty()) t += ";";
        t += juce::String(modSourceName(m.source)) + ">" + paramDesc(m.target).key + ":" + juce::String(m.depth, 3);
        if (m.via != ModSource::None) t += juce::String(":") + modSourceName(m.via);
        if (m.unipolar) t += ":u";
    }
    proc.engine().setModMatrixText(t.toRawUTF8());
    pull();
    if (changedCallback) changedCallback();
}

void RouteTable::changed() { push(); }

void RouteTable::addRow()
{
    if (static_cast<int>(rows.size()) >= kMaxModRoutes) return;
    rows.push_back(std::make_unique<Row>(*this, static_cast<int>(rows.size())));
    content.addAndMakeVisible(*rows.back());
    ModRoute r; r.source = ModSource::Lfo1; r.target = ParamId::Cutoff; r.depth = 0.25f;
    rows.back()->set(r);
    push();
    port.setViewPosition(0, juce::jmax(0, content.getHeight() - port.getHeight()));
}

void RouteTable::removeRow(int index)
{
    if (index < 0 || index >= static_cast<int>(rows.size())) return;
    content.removeChildComponent(rows[static_cast<size_t>(index)].get());
    rows.erase(rows.begin() + index);
    push();
}

int RouteTable::count() const { return static_cast<int>(rows.size()); }

void RouteTable::resized()
{
    auto a = getLocalBounds();
    auto bottom = a.removeFromBottom(24);
    add.setBounds(bottom.removeFromLeft(80)); bottom.removeFromLeft(6);
    clear.setBounds(bottom.removeFromLeft(60));
    {   // the heads over their columns, the same cuts Row::resized makes
        auto h = a.removeFromTop(kHeadH).reduced(2, 0);
        headSource.setBounds(h.removeFromLeft(96));  h.removeFromLeft(kGap);
        headTarget.setBounds(h.removeFromLeft(210)); h.removeFromLeft(kGap);
        h.removeFromRight(24 + kGap);                                   // the remove button's column
        headUni.setBounds(h.removeFromRight(56));    h.removeFromRight(kGap);
        headVia.setBounds(h.removeFromRight(96));    h.removeFromRight(kGap);
        headDepth.setBounds(h);
    }
    port.setBounds(a);
    const int w = juce::jmax(200, a.getWidth() - 14);
    content.setSize(w, juce::jmax(1, static_cast<int>(rows.size())) * kRowH);
    for (size_t i = 0; i < rows.size(); ++i) rows[i]->setBounds(0, static_cast<int>(i) * kRowH, w, kRowH);
}

}   // namespace edt
