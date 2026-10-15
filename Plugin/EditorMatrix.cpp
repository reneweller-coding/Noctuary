/**
 * @file EditorMatrix.cpp
 * @brief The modulation matrix as a table of routes.
 *
 * The MATRIX tab used to be a text box: one route per line, "lfo1>cutoff:0.4", with an Apply
 * button. Exact, scriptable, and the wrong thing to put in front of a musician -- somebody who
 * opens a tab called Matrix expects to see the routes, not their spelling. A grid of every source
 * against every parameter is not the answer either: thirty-odd sources by four hundred targets
 * is a wall with a dozen live cells in it. What a Pigments or a Bitwig shows is what this shows:
 * one row per route, each row a source, a target, a depth you can drag, an optional second
 * source that scales it, and whether the source is read as 0..1 or -1..1.
 *
 * The table edits a copy and hands the whole matrix back to the engine as the same text the box
 * used to, so the parser and the presets see nothing new -- and dragging a card onto a knob
 * still adds a row here, because it goes through the same door.
 */
#include "EditorCommon.h"

using namespace ambient;

namespace edt {

namespace {
constexpr int RowH = 26,    ///< height of a route row, px
              HeadH = 18,   ///< height of the column heads
              Gap = 4;      ///< gap between two columns

/**
 * @brief The combo-box item id that stands for a modulation source: combo ids start at 1, so it is
 *        the source's index plus one.
 * @param s  the source
 * @return   its item id in the source and via combos (ModSource::None is 1)
 */
int sourceItemId(ModSourceE s) { return static_cast<int>(s) + 1; }
/**
 * @brief The modulation source a combo's selected item stands for: the inverse of sourceItemId().
 * @param id  the selected item id, 0 when nothing is selected
 * @return    the source, clamped into 0 .. NumModSources - 1 so a stray id reads as None or as the
 *            last source rather than as garbage
 */
ModSourceE sourceFromItem(int id) { return static_cast<ModSourceE>(juce::jlimit(0, NumModSources - 1, id - 1)); }
}

/**
 * @brief One row of the table: the six controls of a single route, and the index of the route they show.
 *
 * A row owns nothing but its widgets. Every change in them is reported to the table (changed()),
 * which reads all rows back with get() and hands the whole matrix to the engine as text; the index
 * is rewritten by RouteTable::pull() whenever rows above it come or go, so the remove button always
 * names the right route.
 */
struct RouteTable::Row : juce::Component {
    /**
     * @brief Builds the six controls and fills the combos: every source but None, the targets grouped
     *        by section in the parameter table's order, the performance parameters left out.
     * @param t      the table this row reports to
     * @param index  the row's position, which is the route's index in the engine's matrix
     */
    Row(RouteTable& t, int index) : m_table(t), m_idx(index)
    {
        for (int s = 1; s < NumModSources; ++s) m_source.addItem(modSourceName(static_cast<ModSourceE>(s)), sourceItemId(static_cast<ModSourceE>(s)));
        m_via.addItem("-", sourceItemId(ModSourceE::NONE));
        for (int s = 1; s < NumModSources; ++s) m_via.addItem(modSourceName(static_cast<ModSourceE>(s)), sourceItemId(static_cast<ModSourceE>(s)));
        // Targets grouped by section, in the table's order; the performance parameters are
        // not modulation targets and stay out.
        juce::String section;
        for (int i = 0; i < NumParams; ++i) {
            const ParamDesc& d = paramTable()[static_cast<size_t>(i)];
            if (isPerformanceParam(static_cast<ParamIdE>(i))) continue;
            if (juce::String(d.m_section) != section) { section = d.m_section; m_target.addSectionHeading(section); }
            m_target.addItem(juce::String(d.m_section) + ": " + d.m_name, i + 1);
        }
        m_depth.setRange(-1.0, 1.0, 0.001);
        // A plain horizontal slider with the number beside it: the bar style hid the value.
        m_depth.setSliderStyle(juce::Slider::LinearHorizontal);
        m_depth.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 52, 20);
        m_depth.setNumDecimalPlacesToDisplay(2);
        m_depth.setDoubleClickReturnValue(true, 0.25);
        m_uni.setButtonText("0..1");
        m_uni.setTooltip("Read the source as 0..1 instead of -1..1: an LFO then only adds, never subtracts");
        m_remove.setButtonText("x");
        m_remove.setTooltip("Remove this route");
        for (juce::Component* c : { static_cast<juce::Component*>(&m_source), static_cast<juce::Component*>(&m_target),
                                    static_cast<juce::Component*>(&m_depth), static_cast<juce::Component*>(&m_via),
                                    static_cast<juce::Component*>(&m_uni), static_cast<juce::Component*>(&m_remove) })
            addAndMakeVisible(*c);
        m_source.onChange = [this] { m_table.changed(); };
        m_target.onChange = [this] { m_table.changed(); };
        m_via.onChange    = [this] { m_table.changed(); };
        m_uni.onClick     = [this] { m_table.changed(); };
        m_depth.onValueChange = [this] { m_table.changed(); };
        m_remove.onClick  = [this] { m_table.removeRow(m_idx); };
    }
    /**
     * @brief Shows a route in the controls, without firing their callbacks.
     * @param r  the route as the engine holds it
     */
    void set(const ModRoute& r)
    {
        m_source.setSelectedId(sourceItemId(r.m_source), juce::dontSendNotification);
        m_target.setSelectedId(static_cast<int>(r.m_target) + 1, juce::dontSendNotification);
        m_depth.setValue(r.m_depth, juce::dontSendNotification);
        m_via.setSelectedId(sourceItemId(r.m_via), juce::dontSendNotification);
        m_uni.setToggleState(r.m_unipolar, juce::dontSendNotification);
    }
    /**
     * @brief Reads the controls back as a route.
     * @return the route the controls describe: source and via clamped to valid ModSource values, the
     *         target to a valid ParamId, the depth as the slider stands
     */
    ModRoute get() const
    {
        ModRoute r;
        r.m_source = sourceFromItem(m_source.getSelectedId());
        r.m_target = static_cast<ParamIdE>(juce::jlimit(0, NumParams - 1, m_target.getSelectedId() - 1));
        r.m_depth = static_cast<float>(m_depth.getValue());
        r.m_via = sourceFromItem(m_via.getSelectedId());
        r.m_unipolar = m_uni.getToggleState();
        return r;
    }
    /**
     * @brief Places the controls in their columns: source 96 wide, target 210, the remove button,
     *        the 0..1 toggle and the via combo cut from the right, the depth slider taking the rest.
     *        RouteTable::resized makes the same cuts for the heads.
     */
    void resized() override
    {
        auto a = getLocalBounds().reduced(2, 2);
        m_source.setBounds(a.removeFromLeft(96));  a.removeFromLeft(Gap);
        m_target.setBounds(a.removeFromLeft(210)); a.removeFromLeft(Gap);
        m_remove.setBounds(a.removeFromRight(24)); a.removeFromRight(Gap);
        m_uni.setBounds(a.removeFromRight(56));    a.removeFromRight(Gap);
        m_via.setBounds(a.removeFromRight(96));    a.removeFromRight(Gap);
        m_depth.setBounds(a);
    }
    RouteTable& m_table;   ///< the table that owns this row and takes its changes
    int m_idx;             ///< the route's index in the matrix, kept in step by RouteTable::pull()
    juce::ComboBox m_source,   ///< the route's source, any but None
                   m_target,   ///< the target parameter
                   m_via;      ///< the second source that scales the depth, "-" for none
    juce::Slider m_depth;        ///< -1 .. 1 of the target's range; double-click returns it to 0.25
    juce::ToggleButton m_uni;    ///< "0..1": read the source unipolar, so an LFO only adds
    juce::TextButton m_remove;   ///< "x": takes this row out of the matrix
};

RouteTable::RouteTable(NoctuaryProcessor& p, std::function<void()> onChanged)
    : m_proc(p), m_changedCallback(std::move(onChanged))
{
    m_port.setViewedComponent(&m_content, false);
    m_port.setScrollBarsShown(true, false);
    addAndMakeVisible(m_port);
    m_add.setButtonText("+ route");
    m_add.setTooltip("A new route, LFO 1 to the cutoff at a quarter: change it to what you mean");
    m_add.onClick = [this] { addRow(); };
    addAndMakeVisible(m_add);
    m_clear.setButtonText("Clear");
    m_clear.onClick = [this] { m_rows.clear(); m_content.removeAllChildren(); push(); };
    addAndMakeVisible(m_clear);
    // Column heads placed with the rows' own geometry (see resized), not spaced with blanks: a
    // string of spaces lines up with nothing once the strip is any other width.
    struct { juce::Label* m_l; const char* m_t; } heads[] = {
        { &m_headSource, "source" }, { &m_headTarget, "target" }, { &m_headDepth, "depth  (-1 .. 1 of the target's range)" },
        { &m_headVia, "via" }, { &m_headUni, "0..1" } };
    for (auto& h : heads) {
        h.m_l->setFont(ui::body(10.0f));
        h.m_l->setColour(juce::Label::textColourId, ui::Dim);
        h.m_l->setText(h.m_t, juce::dontSendNotification);
        addAndMakeVisible(*h.m_l);
    }
}

RouteTable::~RouteTable() = default;

void RouteTable::pull()
{
    const ModMatrix& m = m_proc.engine().modMatrix();
    while (static_cast<int>(m_rows.size()) > m.count()) { m_content.removeChildComponent(m_rows.back().get()); m_rows.pop_back(); }
    while (static_cast<int>(m_rows.size()) < m.count()) {
        m_rows.push_back(std::make_unique<Row>(*this, static_cast<int>(m_rows.size())));
        m_content.addAndMakeVisible(*m_rows.back());
    }
    for (int i = 0; i < m.count(); ++i) { m_rows[static_cast<size_t>(i)]->m_idx = i; m_rows[static_cast<size_t>(i)]->set(m.route(i)); }
    resized();
}

void RouteTable::push()
{
    // The whole matrix back to the engine as text -- the one door every route goes through, so
    // the parser, the presets and the drag-and-drop never see a second format.
    juce::String t;
    for (auto& r : m_rows) {
        const ModRoute m = r->get();
        if (m.m_source == ModSourceE::NONE) continue;
        if (t.isNotEmpty()) t += ";";
        t += juce::String(modSourceName(m.m_source)) + ">" + paramDesc(m.m_target).m_key + ":" + juce::String(m.m_depth, 3);
        if (m.m_via != ModSourceE::NONE) t += juce::String(":") + modSourceName(m.m_via);
        if (m.m_unipolar) t += ":u";
    }
    m_proc.engine().setModMatrixText(t.toRawUTF8());
    pull();
    if (m_changedCallback) m_changedCallback();
}

void RouteTable::changed() { push(); }

void RouteTable::addRow()
{
    if (static_cast<int>(m_rows.size()) >= MaxModRoutes) return;
    m_rows.push_back(std::make_unique<Row>(*this, static_cast<int>(m_rows.size())));
    m_content.addAndMakeVisible(*m_rows.back());
    ModRoute r; r.m_source = ModSourceE::LFO1; r.m_target = ParamIdE::CUTOFF; r.m_depth = 0.25f;
    m_rows.back()->set(r);
    push();
    m_port.setViewPosition(0, juce::jmax(0, m_content.getHeight() - m_port.getHeight()));
}

void RouteTable::removeRow(int index)
{
    if (index < 0 || index >= static_cast<int>(m_rows.size())) return;
    m_content.removeChildComponent(m_rows[static_cast<size_t>(index)].get());
    m_rows.erase(m_rows.begin() + index);
    push();
}

int RouteTable::count() const { return static_cast<int>(m_rows.size()); }

void RouteTable::resized()
{
    auto a = getLocalBounds();
    auto bottom = a.removeFromBottom(24);
    m_add.setBounds(bottom.removeFromLeft(80)); bottom.removeFromLeft(6);
    m_clear.setBounds(bottom.removeFromLeft(60));
    {   // the heads over their columns, the same cuts Row::resized makes
        auto h = a.removeFromTop(HeadH).reduced(2, 0);
        m_headSource.setBounds(h.removeFromLeft(96));  h.removeFromLeft(Gap);
        m_headTarget.setBounds(h.removeFromLeft(210)); h.removeFromLeft(Gap);
        h.removeFromRight(24 + Gap);                                   // the remove button's column
        m_headUni.setBounds(h.removeFromRight(56));    h.removeFromRight(Gap);
        m_headVia.setBounds(h.removeFromRight(96));    h.removeFromRight(Gap);
        m_headDepth.setBounds(h);
    }
    m_port.setBounds(a);
    const int w = juce::jmax(200, a.getWidth() - 14);
    m_content.setSize(w, juce::jmax(1, static_cast<int>(m_rows.size())) * RowH);
    for (size_t i = 0; i < m_rows.size(); ++i) m_rows[i]->setBounds(0, static_cast<int>(i) * RowH, w, RowH);
}

}   // namespace edt
