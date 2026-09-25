/**
 * @file EditorBrowse.cpp
 * @brief The browse page (columns, list, map) and the perform page.
 *
 * Two of the editor's three pages live here; the third, the panel itself, is PluginEditor.cpp, which
 * owns both of these as browse_ and perform_ and swaps them in with setPage(). Everything runs on
 * the message thread and reads the instrument through the processor: the parameters through apvts,
 * the engine's map cursor, route and morph state through engine() getters, and the presets through
 * the accessors of Presets.h and PresetMeta.h. Nothing here touches audio.
 *
 * The browse page is BrowseView, and it has two modes. "Columns" is the classic browser --
 * Family | Character | Motion | Features narrowing the list, the preset's own card (InfoPanel)
 * beside the results. "Map" is the measured plane of Tools/library/map_all.py: every preset a dot at
 * the place its measurements put it (MapView), the four macro range sliders thinning the cloud along
 * one descriptor each, the blend cursor and its radius, and the route strip that walks the cursor
 * from waypoint to waypoint. Both modes share the search box, the sort, the favourites, the "similar"
 * narrowing and the morph-on-select, and both end in applyFilter(), which builds the one list
 * (filtered) that the list box, the map and the info line all read.
 *
 * The map is the expensive picture: with a library loaded there are thousands of dots, so the cloud
 * is drawn once into an image and only what moves -- the cursor, the ring of what is playing, the
 * crossing while a preset change travels, the names once zoomed in -- is drawn live over it. The
 * colours come from the two helpers at the top of the file: familyColour() for where a preset came
 * from, clusterColour() for the measured group it sounds like.
 *
 * The perform page is PerformView: the eight macros as large knobs, the morph slider between the
 * A and B slots, and the set recorder (record every knob, gesture and note with its time; play a set
 * back; ambient_render --set-file renders it again offline).
 */
#include "EditorCommon.h"

using namespace ambient;

// ---------------------------------------------------------------- browse page

namespace {
/**
 * @brief The colours of the twelve built-in preset families, indexed by family.
 *
 * The twelve built-in families keep their colours; loaded packs get their own hues, spaced by
 * the golden angle so neighbouring packs never look alike.
 */
const juce::Colour kFamilyColours[] = {
    juce::Colour(0xff7fb3d5), juce::Colour(0xff8ec9a8), juce::Colour(0xffe0c070), juce::Colour(0xffd08a8a),
    juce::Colour(0xffb094d8), juce::Colour(0xff70c8c8), juce::Colour(0xffe09a60), juce::Colour(0xffa0b8e0),
    juce::Colour(0xffc8d870), juce::Colour(0xffd880b8), juce::Colour(0xff90d0f0), juce::Colour(0xffd0d0d0),
};
}
namespace edt {
/**
 * @brief A colour per measured group.
 *
 * The families are hues by index; the groups get their own ramp so
 * the two colourings cannot be confused with one another at a glance. The hue steps by the golden
 * angle from group to group; when there is more than one group the brightness is stepped in three
 * as well, so two groups that landed on nearby hues still tell apart.
 *
 * @param c  the group index as presetClusterOf() gives it, 0 .. numPresetClusters() - 1; negative
 *           means the preset is in no group
 * @return   the group's colour, opaque; a neutral grey for a negative index
 */
juce::Colour clusterColour(int c)
{
    if (c < 0) return juce::Colour(0xff707880);
    const int n = juce::jmax(1, numPresetClusters());
    const float h = std::fmod(0.08f + 0.61803398f * static_cast<float>(c), 1.0f);
    return juce::Colour::fromHSV(h, 0.55f, 0.92f, 1.0f).withMultipliedBrightness(0.85f + 0.3f * (static_cast<float>(c % 3) / 3.0f) * (n > 1 ? 1.0f : 0.0f));
}

/**
 * The first twelve families are the built-ins and take their colour from kFamilyColours; a family
 * past them is a loaded pack, and is given a hue of its own, stepped by the golden angle from the
 * last built-in, so packs loaded one after another never share a colour with a neighbour. A
 * negative family reads as family 0.
 */
juce::Colour familyColour(int f)
{
    constexpr int kBuiltIn = static_cast<int>(sizeof(kFamilyColours) / sizeof(kFamilyColours[0]));
    if (f < 0) f = 0;
    if (f < kBuiltIn) return kFamilyColours[static_cast<size_t>(f)];
    const float hue = std::fmod(0.08f + 0.6180339f * static_cast<float>(f - kBuiltIn + 1), 1.0f);
    return juce::Colour::fromHSV(hue, 0.42f, 0.82f, 1.0f);
}
}

NoctuaryEditor::BrowseView::BrowseView(NoctuaryProcessor& p) : proc(p), map(*this)
{
    search.setTextToShowWhenEmpty("search", kDim);
    search.onTextChange = [this] { applyFilter(); };
    addAndMakeVisible(search);
    family.addItem("all families", 1);
    for (int f = 0; f < numPresetFamilies(); ++f) family.addItem(presetFamilyName(f), f + 2);
    family.setSelectedId(1, juce::dontSendNotification);
    family.onChange = [this] { applyFilter(); };
    addAndMakeVisible(family);
    sort.addItem("by number", 1); sort.addItem("by name", 2); sort.addItem("dark -> bright", 3); sort.addItem("calm -> moving", 4);
    sort.addItem("narrow -> wide", 5); sort.addItem("tonal -> noisy", 6); sort.addItem("sparse -> dense", 7);
    sort.addItem("still -> evolving", 8); sort.addItem("smooth -> rough", 9); sort.addItem("near -> far", 10);
    sort.setSelectedId(1, juce::dontSendNotification);
    sort.onChange = [this] { applyFilter(); };
    addAndMakeVisible(sort);
    for (int t = 0; t < kNumPresetTags; ++t) {
        auto b = std::make_unique<juce::ToggleButton>(presetTagName(t));
        b->onClick = [this] { applyFilter(); };
        addAndMakeVisible(*b);
        tagButtons.push_back(std::move(b));
    }
    list.setModel(this);
    list.setRowHeight(22);
    list.setColour(juce::ListBox::backgroundColourId, kBg);
    addAndMakeVisible(list);
    addAndMakeVisible(map);
    mapActive.setButtonText("Map blend (drag the cursor)");
    addAndMakeVisible(mapActive);
    mapActiveAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(proc.apvts, paramDesc(ParamId::MapActive).key, mapActive);
    radius.setSliderStyle(juce::Slider::LinearHorizontal);
    radius.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 18);
    addAndMakeVisible(radius);
    radiusAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(proc.apvts, paramDesc(ParamId::MapRadius).key, radius);

    // The four macro sliders: one measured axis each, as a range rather than a point, so "show me
    // the dark half" and "show me the darkest tenth" are the same gesture. Wide open they filter
    // nothing. Narrow one and the cloud thins to what is left and the view closes in on it -- the
    // thing an even field could never show, that a hundred presets here are nearly one sound.
    static const char* const kMacroNames[4] = { "dark - bright", "still - moving", "smooth - rough", "near - far" };
    float PresetMeta::* const kMacroFields[4] = { &PresetMeta::bright, &PresetMeta::evolve, &PresetMeta::rough, &PresetMeta::wet };
    static const char* const kMacroTips[4] = {
        "Where the spectrum's weight sits: left the dark presets, right the bright ones.",
        "How far the sound travels over a minute -- the drone's own axis, in place of the attack an ambient patch does not have.",
        "The roughness of its partials against each other (Plomp-Levelt): smooth and fused, or beating and grinding.",
        "How much of what you hear comes back from the far planes rather than standing in the near one." };
    for (int i = 0; i < 4; ++i) {
        macros[i].field = kMacroFields[i];
        macros[i].slider.setSliderStyle(juce::Slider::TwoValueHorizontal);
        macros[i].slider.setRange(0.0, 1.0, 0.01);
        macros[i].slider.setMinAndMaxValues(0.0, 1.0, juce::dontSendNotification);
        macros[i].slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        macros[i].slider.setTooltip(kMacroTips[i]);
        macros[i].slider.onValueChange = [this] { fitPending = true; applyFilter(); };
        macros[i].label.setText(kMacroNames[i], juce::dontSendNotification);
        macros[i].label.setFont(juce::FontOptions(10.5f));
        macros[i].label.setColour(juce::Label::textColourId, kDim);
        macros[i].label.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(macros[i].slider);
        addAndMakeVisible(macros[i].label);
    }
    colourByGroup.setToggleState(true, juce::dontSendNotification);
    colourByGroup.setTooltip("Colour the cloud by the measured groups -- presets that sound alike -- rather than by the pack a preset was written for.");
    colourByGroup.onClick = [this] { map.repaint(); };
    addAndMakeVisible(colourByGroup);
    info.setFont(juce::FontOptions(12.0f)); info.setColour(juce::Label::textColourId, kDim);
    addAndMakeVisible(info);
    load.onClick = [this] { if (selected >= 0) proc.selectPreset(selected, morphOnSelect.getToggleState()); };
    toA.onClick = [this] { if (selected >= 0) proc.setMorphSlotFromPreset(0, selected); };
    toB.onClick = [this] { if (selected >= 0) proc.setMorphSlotFromPreset(1, selected); };
    star.onClick = [this] { if (selected >= 0) { proc.setFavourite(selected, !proc.isFavourite(selected)); applyFilter(); } };
    onlyFavourites.onClick = [this] { applyFilter(); };
    favouritesFirst.setToggleState(proc.favouritesFirst(), juce::dontSendNotification);
    favouritesFirst.setTooltip("Puts the starred presets at the top of the list, whatever the sort. Remembered with the favourites (Documents\\Noctuary\\favourites.txt).");
    favouritesFirst.onClick = [this] { proc.setFavouritesFirst(favouritesFirst.getToggleState()); applyFilter(); };
    hideDull.onClick = [this] { applyFilter(); };
    hideDull.setTooltip("Hides presets that measured as barely moving and barely wide -- the dull tail of a generated library");
    for (auto* b : { &load, &toA, &toB, &star }) addAndMakeVisible(*b);
    // The map already knows which presets are alike: its neighbours are the ones that measured
    // alike. This narrows the list to the selected preset's six nearest and zooms the map onto
    // them; off again, the list is what the other filters make it.
    similar.onClick = [this] {
        similarSet.clear();
        similarOf = -1;
        if (similar.getToggleState() && selected >= 0 && selected < numPresetMeta()) {
            const PresetMeta& m = presetMeta(selected);
            const PresetMap::Blend b = PresetMap::neighbours(m.x, m.y, 0.12f);
            similarSet.push_back(selected);
            for (int k = 0; k < b.count; ++k) if (b.index[k] != selected) similarSet.push_back(b.index[k]);
            similarOf = selected;
            map.zoomTo(m.x, m.y, 8.0f);
        } else if (!similar.getToggleState()) {
            map.zoomTo(0.5f, 0.5f, 1.0f);
        }
        applyFilter();
    };
    similar.setTooltip("Narrow the list to the presets that measured most like the selected one, and zoom the map onto them");
    addAndMakeVisible(similar);
    addAndMakeVisible(onlyFavourites);
    addAndMakeVisible(favouritesFirst);
    addAndMakeVisible(hideDull);
    addAndMakeVisible(info2);
    // Choosing a preset as a journey. On by default: an instrument that plays for hours has no
    // business cutting from one world to another because somebody clicked a name.
    morphOnSelect.setToggleState(proc.morphOnSelect(), juce::dontSendNotification);
    morphOnSelect.setTooltip("Choosing a preset morphs the whole instrument into it over the time beside this, instead of switching to it at once. What is playing becomes A, the chosen preset B; when it arrives, the preset is simply loaded.");
    morphOnSelect.onClick = [this] { proc.setMorphOnSelect(morphOnSelect.getToggleState()); };
    addAndMakeVisible(morphOnSelect);
    morphSeconds.setSliderStyle(juce::Slider::LinearHorizontal);
    morphSeconds.setRange(0.5, 600.0, 0.5);
    morphSeconds.setSkewFactor(0.4);
    morphSeconds.setValue(proc.morphSelectSeconds(), juce::dontSendNotification);
    morphSeconds.setTextBoxStyle(juce::Slider::TextBoxRight, false, 56, 18);
    morphSeconds.setTextValueSuffix(" s");
    morphSeconds.setTooltip("How long that journey takes: half a second to ten minutes.");
    morphSeconds.onValueChange = [this] { proc.setMorphSelectSeconds(static_cast<float>(morphSeconds.getValue())); };
    addAndMakeVisible(morphSeconds);

    // Classic columns: Family | Character | Motion & density | Features. A click narrows,
    // several rows in one column combine with OR, columns combine with AND, "All" clears.
    struct Def { const char* title; std::vector<int> tags; bool families; };
    const Def defs[4] = {
        { "Family", {}, true },
        { "Character", { 0, 1, 4, 5, 6, 7, 21, 22, 23, 24 }, false },   // Dark Bright Tonal Noisy Wide Bass Smooth Rough Near Far
        { "Motion", { 2, 3, 8, 9, 19, 20 }, false },              // Calm Moving Dense Sparse Still Evolving
        { "Features", { 10, 11, 12, 13, 14, 15, 16, 17, 18 }, false },   // Keys Generative Cosmos Feedback Sources JI Sub Stack Air
    };
    for (int c = 0; c < 4; ++c) {
        Column& col = columns[c];
        col.owner = this; col.title = defs[c].title;
        col.items.add("All"); col.tagBits.push_back(0); col.familyIdx.push_back(-1);
        if (defs[c].families) {
            for (int f = 0; f < numPresetFamilies(); ++f) { col.items.add(presetFamilyName(f)); col.tagBits.push_back(0); col.familyIdx.push_back(f); }
        } else {
            for (int t : defs[c].tags) { col.items.add(presetTagName(t)); col.tagBits.push_back(1u << t); col.familyIdx.push_back(-1); }
        }
        col.box.setModel(&col);
        col.box.setRowHeight(20);
        col.box.setColour(juce::ListBox::backgroundColourId, kBg);
        addAndMakeVisible(col.box);
    }
    // Route strip
    routeBox.setTextWhenNothingSelected("route preset");
    for (int r = 0; r < numRoutePresets(); ++r) routeBox.addItem(routePreset(r).name, r + 1);
    routeBox.onChange = [this] {
        const int r = routeBox.getSelectedId() - 1;
        if (r >= 0 && r < numRoutePresets()) { proc.setRouteText(routePreset(r).points); map.repaint(); }
    };
    addAndMakeVisible(routeBox);
    routePlay.setButtonText("Play route"); addAndMakeVisible(routePlay);
    routePlayAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(proc.apvts, paramDesc(ParamId::RouteActive).key, routePlay);
    routeLoop.setButtonText("loop"); addAndMakeVisible(routeLoop);
    routeLoopAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(proc.apvts, paramDesc(ParamId::RouteLoop).key, routeLoop);
    routeSpeed.setSliderStyle(juce::Slider::LinearHorizontal); routeSpeed.setTextBoxStyle(juce::Slider::TextBoxRight, false, 44, 18); routeSpeed.setTextValueSuffix("x");
    addAndMakeVisible(routeSpeed);
    routeSpeedAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(proc.apvts, paramDesc(ParamId::RouteSpeed).key, routeSpeed);
    routeAdd.setTooltip("Append the current cursor (position and radius) as a waypoint: 60 s travel, 60 s hold");
    routeAdd.onClick = [this] {
        Waypoint w; w.x = proc.engine().getParam(ParamId::MapX); w.y = proc.engine().getParam(ParamId::MapY); w.radius = proc.engine().getParam(ParamId::MapRadius);
        w.travel = 60.0f; w.hold = 60.0f;
        const int near = map.nearestPreset(map.toScreen(w.x, w.y), 8.0f);
        if (near >= 0) { w.preset = near; w.x = presetMeta(near).x; w.y = presetMeta(near).y; }
        proc.addRoutePoint(w); routeBox.setSelectedId(0, juce::dontSendNotification); map.repaint();
    };
    routeClear.onClick = [this] { proc.clearRoute(); routeBox.setSelectedId(0, juce::dontSendNotification); map.repaint(); };
    routeEdit.setTooltip("Edit the route as text: preset|travel|hold[|radius] or x,y|travel|hold[|radius], separated by ;");
    routeEdit.onClick = [this] { showRouteEditor(); };
    for (auto* b : { &routeAdd, &routeClear, &routeEdit }) addAndMakeVisible(*b);

    modeClassic.setClickingTogglesState(true); modeMap.setClickingTogglesState(true);
    modeClassic.setRadioGroupId(77); modeMap.setRadioGroupId(77);
    modeClassic.setColour(juce::TextButton::buttonOnColourId, kAccent.withAlpha(0.5f));
    modeMap.setColour(juce::TextButton::buttonOnColourId, kAccent.withAlpha(0.5f));
    modeClassic.onClick = [this] { setMode(0); };
    modeMap.onClick = [this] { setMode(1); };
    addAndMakeVisible(modeClassic); addAndMakeVisible(modeMap);
    setMode(0);
    applyFilter();
    startTimerHz(15);
}

void NoctuaryEditor::BrowseView::setMode(int m)
{
    mode = m;
    modeClassic.setToggleState(m == 0, juce::dontSendNotification);
    modeMap.setToggleState(m == 1, juce::dontSendNotification);
    for (auto& c : columns) c.box.setVisible(m == 0);
    for (auto& b : tagButtons) b->setVisible(m == 1);
    family.setVisible(m == 1);
    map.setVisible(m == 1); mapActive.setVisible(m == 1); radius.setVisible(m == 1);
    colourByGroup.setVisible(m == 1);
    for (auto& mc : macros) { mc.slider.setVisible(m == 1); mc.label.setVisible(m == 1); }
    for (juce::Component* c : { static_cast<juce::Component*>(&routeBox), static_cast<juce::Component*>(&routePlay), static_cast<juce::Component*>(&routeLoop),
                                static_cast<juce::Component*>(&routeSpeed), static_cast<juce::Component*>(&routeAdd), static_cast<juce::Component*>(&routeClear), static_cast<juce::Component*>(&routeEdit) })
        c->setVisible(m == 1);
    resized();
    applyFilter();
}

void NoctuaryEditor::BrowseView::Column::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool)
{
    const bool on = (row == 0) ? chosen.empty() : chosen.count(row) > 0;
    if (on) g.fillAll(kAccent.withAlpha(0.18f));
    g.setColour(on ? kText : kDim); g.setFont(juce::FontOptions(12.5f));
    g.drawText(items[row], 8, 0, w - 40, h, juce::Justification::centredLeft);
    // count of presets this row would leave -- cached, because counting five thousand presets
    // per painted row per repaint is not something a list box should be doing
    if (row > 0 && owner != nullptr) {
        updateCounts();
        if (static_cast<size_t>(row) < counts.size()) {
            g.setColour(kDim.withAlpha(0.7f)); g.setFont(juce::FontOptions(10.5f));
            g.drawText(juce::String(counts[static_cast<size_t>(row)]), w - 34, 0, 28, h, juce::Justification::centredRight);
        }
    }
}

void NoctuaryEditor::BrowseView::Column::updateCounts()
{
    if (countsFor == numPresets() && counts.size() == static_cast<size_t>(items.size())) return;
    countsFor = numPresets();
    counts.assign(static_cast<size_t>(items.size()), 0);
    for (int i = 0; i < numPresets(); ++i) {
        const PresetMeta& m = presetMeta(i);
        for (size_t r = 1; r < counts.size(); ++r)
            if ((familyIdx[r] >= 0 && m.family == familyIdx[r]) || (tagBits[r] && (m.tags & tagBits[r]))) ++counts[r];
    }
}

void NoctuaryEditor::BrowseView::Column::listBoxItemClicked(int row, const juce::MouseEvent& e)
{
    if (row == 0) chosen.clear();
    else if (e.mods.isCommandDown() || e.mods.isCtrlDown()) { if (chosen.count(row)) chosen.erase(row); else chosen.insert(row); }
    else if (chosen.size() == 1 && chosen.count(row)) chosen.clear();
    else { chosen.clear(); chosen.insert(row); }
    box.repaint();
    if (owner) owner->applyFilter();
}

bool NoctuaryEditor::BrowseView::Column::passes(int preset) const
{
    if (chosen.empty()) return true;
    const PresetMeta& m = presetMeta(preset);
    for (int row : chosen) {
        const size_t r = static_cast<size_t>(row);
        if (familyIdx[r] >= 0 && m.family == familyIdx[r]) return true;
        if (tagBits[r] && (m.tags & tagBits[r])) return true;
    }
    return false;
}

void NoctuaryEditor::BrowseView::applyFilter()
{
    ++filterGen;          // the map's cached cloud is drawn from this list
    filtered.clear();
    const juce::String needle = search.getText().trim().toLowerCase();
    const int fam = family.getSelectedId() - 2;
    uint32_t need = 0;
    for (int t = 0; t < kNumPresetTags; ++t) if (tagButtons[static_cast<size_t>(t)]->getToggleState()) need |= (1u << t);
    for (int i = 0; i < numPresets(); ++i) {
        const PresetMeta& m = presetMeta(i);
        if (needle.isNotEmpty() && !juce::String(preset(i).name).toLowerCase().contains(needle)) continue;
        if (onlyFavourites.getToggleState() && !proc.isFavourite(i)) continue;
        if (!similarSet.empty() && std::find(similarSet.begin(), similarSet.end(), i) == similarSet.end()) continue;
        // Dull, measured rather than judged: in the bottom fifth for movement and for width, and
        // not carrying the sparseness that would make that a deliberate character.
        if (hideDull.getToggleState() && m.motion < 0.2f && m.width < 0.2f && m.density < 0.5f) continue;
        if (mode == 1) {   // the macro ranges
            bool inRange = true;
            for (const auto& mc : macros) {
                const float v = m.*(mc.field);
                const float lo = static_cast<float>(mc.slider.getMinValue()), hi = static_cast<float>(mc.slider.getMaxValue());
                if (lo > 0.0f || hi < 1.0f) if (v < lo - 1.0e-4f || v > hi + 1.0e-4f) { inRange = false; break; }
            }
            if (!inRange) continue;
        }
        if (mode == 1) {
            if (fam >= 0 && m.family != fam) continue;
            if ((m.tags & need) != need) continue;
        } else {
            bool ok = true;
            for (const auto& c : columns) if (!c.passes(i)) { ok = false; break; }
            if (!ok) continue;
        }
        filtered.push_back(i);
    }
    const int s = sort.getSelectedId();
    auto key = [s](int i) -> float {
        const PresetMeta& m = presetMeta(i);
        switch (s) { case 3: return m.bright; case 4: return m.motion; case 5: return m.width; case 6: return m.noisy; case 7: return m.density;
                     case 8: return m.evolve; case 9: return m.rough; case 10: return m.wet; default: return 0.0f; }
    };
    if (s == 2) std::sort(filtered.begin(), filtered.end(), [](int a, int b) { return juce::String(preset(a).name).compareIgnoreCase(preset(b).name) < 0; });
    else if (s >= 3) std::stable_sort(filtered.begin(), filtered.end(), [&](int a, int b) { return key(a) < key(b); });
    // The stars to the top, in the order the sort left them (Rene, 13.09.2026).
    if (favouritesFirst.getToggleState())
        std::stable_partition(filtered.begin(), filtered.end(), [this](int i) { return proc.isFavourite(i); });
    list.updateContent();
    info.setText(juce::String(static_cast<int>(filtered.size())) + " of " + juce::String(numPresets()) + " presets" +
                 (numPresetMeta() == 0 ? "   (map not measured yet: run Tools/library/map_all.py)" : ""), juce::dontSendNotification);
    if (fitPending) fitToFilter();
    map.repaint();
}

/**
 * The info panel. Headings in the accent colour, the prose under them, wrapped -- the same shape
 * u-he uses, because it is the right one: name at the top, then what it is, then what your hands
 * do, then where it is filed.
 */
void NoctuaryEditor::BrowseView::InfoPanel::paint(juce::Graphics& g)
{
    auto r = getLocalBounds();
    g.setColour(ui::card.withAlpha(0.55f));
    g.fillRoundedRectangle(r.toFloat(), 6.0f);
    r = r.reduced(12, 10);
    if (title.isEmpty()) {
        g.setColour(kDim); g.setFont(juce::FontOptions(11.5f));
        g.drawText("PRESET INFO", r.removeFromTop(16), juce::Justification::topLeft);
        g.setColour(ui::faint); g.setFont(juce::FontOptions(11.0f));
        g.drawText("choose a preset to read what it is", r, juce::Justification::topLeft);
        return;
    }
    g.setColour(kText); g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    g.drawText(title, r.removeFromTop(18), juce::Justification::topLeft);
    r.removeFromTop(4);
    g.setFont(juce::FontOptions(11.5f));
    juce::StringArray lines;
    lines.addLines(body);
    // The phrases under SOUNDS LIKE are the only text here nobody wrote for this preset: a model
    // picked them out of our vocabulary. They are set in italic so the eye can tell the measured
    // sentence from the chosen one without being told.
    bool quoted = false;
    for (const auto& line : lines) {
        if (r.getHeight() <= 0) break;
        const juce::String t = line.trim();
        if (t.isEmpty()) { r.removeFromTop(5); continue; }
        // A heading is a line in capitals: draw it as one, with a rule under it.
        const bool heading = t == t.toUpperCase() && t.containsOnly("ABCDEFGHIJKLMNOPQRSTUVWXYZ -");
        if (heading) {
            quoted = (t == "SOUNDS LIKE");
            r.removeFromTop(5);
            auto h = r.removeFromTop(14);
            g.setColour(kAccent.withAlpha(0.85f)); g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
            g.drawText(t, h, juce::Justification::topLeft);
            g.setColour(kAccent.withAlpha(0.25f));
            g.drawLine(static_cast<float>(h.getX()), static_cast<float>(h.getBottom()),
                       static_cast<float>(h.getRight()), static_cast<float>(h.getBottom()), 1.0f);
            g.setFont(juce::FontOptions(11.5f));
            continue;
        }
        // Prose wraps; the arrow rows do not need to.
        juce::GlyphArrangement ga;
        const juce::FontOptions bodyFont = quoted ? juce::FontOptions(11.5f).withStyle("Italic")
                                                  : juce::FontOptions(11.5f);
        ga.addFittedText(bodyFont, t, static_cast<float>(r.getX()), static_cast<float>(r.getY()),
                         static_cast<float>(r.getWidth()), static_cast<float>(juce::jmin(r.getHeight(), 44)),
                         juce::Justification::topLeft, 3, 1.0f);
        g.setColour(t.contains("->") ? kDim : kText.withAlpha(0.92f));
        ga.draw(g);
        const auto bb = ga.getBoundingBox(0, -1, true);
        r.removeFromTop(juce::jmax(14, static_cast<int>(bb.getHeight()) + 2));
    }
}

void NoctuaryEditor::BrowseView::updateInfo()
{
    const int want = selected >= 0 ? selected : proc.getCurrentProgram();
    if (want == infoFor) return;
    infoFor = want;
    if (want < 0 || want >= numPresets()) { info2.title = {}; info2.body = {}; }
    else {
        info2.title = preset(want).name;
        info2.body = juce::String(ambient::presetInfoText(want));
    }
    info2.repaint();
}

bool NoctuaryEditor::BrowseView::macroActive() const
{
    for (const auto& mc : macros)
        if (mc.slider.getMinValue() > 0.0 || mc.slider.getMaxValue() < 1.0) return true;
    return false;
}

/**
 * What Absynth's browser does when a tag is chosen: the cloud condenses. Ours cannot move the
 * points -- their places are what they mean -- so the view closes in on what is left instead,
 * which is the same gesture from the other side. Only on the filter's own action, never while
 * the mouse is panning or zooming, so the view never fights the hand.
 */
void NoctuaryEditor::BrowseView::fitToFilter()
{
    fitPending = false;
    if (mode != 1) return;
    const int shown = juce::jmin(numPresetMeta(), numPresets());
    if (!macroActive()) { map.zoomTo(0.5f, 0.5f, 1.0f); return; }
    float x0 = 1.0f, x1 = 0.0f, y0 = 1.0f, y1 = 0.0f;
    int n = 0;
    for (int i : filtered) {
        if (i >= shown) continue;
        const PresetMeta& m = presetMeta(i);
        x0 = juce::jmin(x0, m.x); x1 = juce::jmax(x1, m.x);
        y0 = juce::jmin(y0, m.y); y1 = juce::jmax(y1, m.y);
        ++n;
    }
    if (n < 2) return;
    const float w = juce::jmax(0.02f, x1 - x0), h = juce::jmax(0.02f, y1 - y0);
    const float z = juce::jlimit(1.0f, 40.0f, 0.85f / juce::jmax(w, h));
    map.zoomTo(0.5f * (x0 + x1), 0.5f * (y0 + y1), z);
}

void NoctuaryEditor::BrowseView::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool sel)
{
    if (row < 0 || row >= static_cast<int>(filtered.size())) return;
    const int i = filtered[static_cast<size_t>(row)];
    const PresetMeta& m = presetMeta(i);
    if (sel) g.fillAll(kAccent.withAlpha(0.18f));
    else if (i == proc.getCurrentProgram()) g.fillAll(kAccent.withAlpha(0.08f));
    g.setColour(familyColour(m.family)); g.fillEllipse(6.0f, h * 0.5f - 4.0f, 8.0f, 8.0f);
    g.setColour(proc.isFavourite(i) ? juce::Colour(0xffe0c070) : kSectionFill); g.setFont(juce::FontOptions(13.0f));
    g.drawText(juce::String(juce::CharPointer_UTF8("\xe2\x98\x85")), 18, 0, 16, h, juce::Justification::centred);
    g.setColour(kText); g.setFont(juce::FontOptions(13.0f));
    g.drawText(preset(i).name, 38, 0, w - 320, h, juce::Justification::centredLeft);
    if (mode == 0) { g.setColour(kDim); g.setFont(juce::FontOptions(11.0f)); g.drawText(presetFamilyName(m.family), w - 320, 0, 140, h, juce::Justification::centredLeft); }
    // small bars: bright, motion, width, density
    const float vals[4] = { m.bright, m.motion, m.width, m.density };
    const juce::Colour cols[4] = { juce::Colour(0xffe0c070), juce::Colour(0xff8ec9a8), juce::Colour(0xff7fb3d5), juce::Colour(0xffd08a8a) };
    for (int k = 0; k < 4; ++k) {
        const int x = w - 176 + k * 42;
        g.setColour(kSectionFill); g.fillRect(x, h / 2 - 3, 36, 6);
        g.setColour(cols[k]); g.fillRect(x, h / 2 - 3, static_cast<int>(36 * vals[k]), 6);
    }
}

void NoctuaryEditor::BrowseView::listBoxItemClicked(int row, const juce::MouseEvent& e)
{
    if (row < 0 || row >= static_cast<int>(filtered.size())) return;
    selected = filtered[static_cast<size_t>(row)];
    if (e.x >= 16 && e.x < 36) {   // the star
        proc.setFavourite(selected, !proc.isFavourite(selected));
        if (favouritesFirst.getToggleState()) applyFilter(); else { list.repaint(); map.repaint(); }
        return;
    }
    if (e.getNumberOfClicks() >= 2 || e.mods.isLeftButtonDown()) proc.selectPreset(selected, morphOnSelect.getToggleState());
    if (mapActive.getToggleState()) {   // the cursor jumps to the preset's point
        const PresetMeta& m = presetMeta(selected);
        if (auto* px = proc.apvts.getParameter(paramDesc(ParamId::MapX).key)) px->setValueNotifyingHost(m.x);
        if (auto* py = proc.apvts.getParameter(paramDesc(ParamId::MapY).key)) py->setValueNotifyingHost(m.y);
    }
    map.repaint();
}

void NoctuaryEditor::BrowseView::timerCallback()
{
    map.repaint(); list.repaint(); updateInfo();
    if (routeEditOpen) {   // same pattern as the gesture editor: mirror while open, apply when gone
        if (routeEditor != nullptr) routeEditText = routeEditor->getText();
        else {
            routeEditOpen = false;
            if (routeEditText.trim().isEmpty()) proc.clearRoute();
            else if (!proc.setRouteText(routeEditText))
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Route", "A point could not be parsed (unknown preset name or malformed); the previous route is kept.");
            routeBox.setSelectedId(0, juce::dontSendNotification);
        }
    }
}

void NoctuaryEditor::BrowseView::showRouteEditor()
{
    auto* editor = new juce::TextEditor();
    editor->setMultiLine(true, true);
    editor->setReturnKeyStartsNewLine(true);
    editor->setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
    editor->setText(proc.routeText().replace(";", ";\n"), false);
    editor->setSize(560, 320);
    auto* content = new juce::Component();
    content->setSize(560, 360);
    content->addAndMakeVisible(editor);
    editor->setTopLeftPosition(0, 0);
    auto* hint = new juce::Label(juce::String(), "one point per line:  Preset Name|travel s|hold s[|radius]   or   x,y|travel|hold[|radius]");
    hint->setFont(juce::FontOptions(11.0f)); hint->setColour(juce::Label::textColourId, kDim);
    hint->setBounds(0, 324, 560, 30); content->addAndMakeVisible(hint);
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(content);
    o.dialogTitle = "Route over the map";
    o.componentToCentreAround = this;
    o.dialogBackgroundColour = kGroupFill;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
    routeEditor = editor; routeEditText = editor->getText(); routeEditOpen = true;
}

void NoctuaryEditor::BrowseView::paint(juce::Graphics& g)
{
    g.fillAll(kBg);
    g.setColour(kGroupFill);
    g.fillRoundedRectangle(getLocalBounds().reduced(8).toFloat(), 8.0f);
    g.setColour(kDim); g.setFont(juce::FontOptions(11.0f));
    g.drawText("bright  motion  width  density", list.getRight() - 176, list.getY() - 16, 176, 14, juce::Justification::centredLeft);
    if (mode == 0) {
        g.drawText("preset", list.getX() + 38, list.getY() - 16, 200, 14, juce::Justification::centredLeft);
        g.drawText("family", list.getRight() - 320, list.getY() - 16, 140, 14, juce::Justification::centredLeft);
        for (auto& c : columns) {
            g.setColour(kText); g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
            g.drawText(c.title.toUpperCase(), c.box.getX(), c.box.getY() - 18, c.box.getWidth(), 16, juce::Justification::centredLeft);
        }
    }
}

void NoctuaryEditor::BrowseView::resized()
{
    auto area = getLocalBounds().reduced(16);
    auto top = area.removeFromTop(26);
    modeClassic.setBounds(top.removeFromLeft(80)); modeMap.setBounds(top.removeFromLeft(60)); top.removeFromLeft(12);
    search.setBounds(top.removeFromLeft(180)); top.removeFromLeft(6);
    if (mode == 1) { family.setBounds(top.removeFromLeft(150)); top.removeFromLeft(6); }
    sort.setBounds(top.removeFromLeft(150)); top.removeFromLeft(12);
    onlyFavourites.setBounds(top.removeFromLeft(130));
    favouritesFirst.setBounds(top.removeFromLeft(130));
    similar.setBounds(top.removeFromLeft(150));
    hideDull.setBounds(top.removeFromLeft(160));
    area.removeFromTop(8);
    auto buttons = area.removeFromBottom(26);
    load.setBounds(buttons.removeFromLeft(80)); buttons.removeFromLeft(6);
    toA.setBounds(buttons.removeFromLeft(60)); buttons.removeFromLeft(6);
    toB.setBounds(buttons.removeFromLeft(60)); buttons.removeFromLeft(6);
    star.setBounds(buttons.removeFromLeft(90)); buttons.removeFromLeft(12);
    morphOnSelect.setBounds(buttons.removeFromLeft(130));
    morphSeconds.setBounds(buttons.removeFromLeft(juce::jmin(190, juce::jmax(110, buttons.getWidth() / 3))));
    buttons.removeFromLeft(12);
    if (mode == 1) {
        auto mapControls = buttons.removeFromRight(juce::jmax(300, buttons.getWidth() * 3 / 5));
        mapActive.setBounds(mapControls.removeFromLeft(220)); mapControls.removeFromLeft(12);
        radius.setBounds(mapControls);
    }
    info.setBounds(buttons);
    area.removeFromBottom(6);
    if (mode == 1) {   // the macro sliders: two pairs across the bottom of the map side
        auto strip = area.removeFromBottom(24);
        strip.removeFromLeft(juce::jmax(420, (getLocalBounds().reduced(16).getWidth()) * 2 / 5) + 12);
        colourByGroup.setBounds(strip.removeFromRight(90));
        const int each = juce::jmax(90, strip.getWidth() / 4);
        for (int i = 0; i < 4; ++i) {
            auto cell = strip.removeFromLeft(each);
            macros[i].label.setBounds(cell.removeFromLeft(juce::jmin(86, each / 2)));
            macros[i].slider.setBounds(cell);
        }
        area.removeFromBottom(4);
    }
    if (mode == 1) {   // route strip above the bottom row, on the map side
        auto strip = area.removeFromBottom(26);
        strip.removeFromLeft(juce::jmax(420, (getLocalBounds().reduced(16).getWidth()) * 2 / 5) + 12);
        routeBox.setBounds(strip.removeFromLeft(170)); strip.removeFromLeft(6);
        routePlay.setBounds(strip.removeFromLeft(96)); strip.removeFromLeft(2);
        routeLoop.setBounds(strip.removeFromLeft(56)); strip.removeFromLeft(6);
        routeSpeed.setBounds(strip.removeFromLeft(juce::jmax(120, strip.getWidth() - 250))); strip.removeFromLeft(6);
        routeAdd.setBounds(strip.removeFromLeft(70)); strip.removeFromLeft(4);
        routeClear.setBounds(strip.removeFromLeft(56)); strip.removeFromLeft(4);
        routeEdit.setBounds(strip.removeFromLeft(70));
        area.removeFromBottom(6);
    }

    if (mode == 0) {
        // Omnisphere-style: four columns on top, the results below, and the preset's own card on
        // the right of them -- Zebra puts it exactly there, and it is where the eye goes next.
        auto cols = area.removeFromTop(juce::jmax(160, area.getHeight() * 2 / 5));
        const int colW = cols.getWidth() / 4;
        for (int c = 0; c < 4; ++c) {
            auto r = cols.removeFromLeft(colW).reduced(0, 0);
            r.removeFromTop(18);   // title painted above
            columns[c].box.setBounds(r.reduced(c == 0 ? 0 : 4, 0).withTrimmedRight(4));
        }
        area.removeFromTop(24);    // column header of the result list
        {   // the list, and the preset's card beside it
            auto card = area.removeFromRight(juce::jlimit(240, 420, area.getWidth() / 3));
            info2.setBounds(card.withTrimmedLeft(12));
        }
        list.setBounds(area);
    } else {
        auto left = area.removeFromLeft(juce::jmax(420, area.getWidth() * 2 / 5));
        area.removeFromLeft(12);
        const int perRow = 5, tagH = 22;
        const int rows = (kNumPresetTags + perRow - 1) / perRow;
        auto tags = left.removeFromTop(rows * tagH);
        // The card paints until it runs out of room, so on the map side it gets as much as half
        // the column: the description, the phrases and the filing all have to fit above the fold.
        info2.setBounds(left.removeFromBottom(juce::jmin(240, left.getHeight() / 2)).withTrimmedTop(8));
        for (int t = 0; t < kNumPresetTags; ++t) {
            const int r = t / perRow, c = t % perRow;
            tagButtons[static_cast<size_t>(t)]->setBounds(tags.getX() + c * (tags.getWidth() / perRow), tags.getY() + r * tagH, tags.getWidth() / perRow, tagH);
        }
        left.removeFromTop(18);
        list.setBounds(left);
        map.setBounds(area);
    }
}

/** The window onto the plane: at zoom 1 and centre (0.5, 0.5) this is exactly the old fixed view. */
juce::Point<float> NoctuaryEditor::BrowseView::MapView::toScreen(float x, float y) const
{
    const auto r = getLocalBounds().toFloat().reduced(18.0f);
    return { r.getCentreX() + (x - centre.x) * zoom * r.getWidth(),
             r.getCentreY() - (y - centre.y) * zoom * r.getHeight() };
}

juce::Point<float> NoctuaryEditor::BrowseView::MapView::toMapRaw(juce::Point<float> s) const
{
    const auto r = getLocalBounds().toFloat().reduced(18.0f);
    return { centre.x + (s.x - r.getCentreX()) / juce::jmax(1.0f, zoom * r.getWidth()),
             centre.y + (r.getCentreY() - s.y) / juce::jmax(1.0f, zoom * r.getHeight()) };
}

juce::Point<float> NoctuaryEditor::BrowseView::MapView::toMap(juce::Point<float> s) const
{
    const auto m = toMapRaw(s);
    return { juce::jlimit(0.0f, 1.0f, m.x), juce::jlimit(0.0f, 1.0f, m.y) };
}

void NoctuaryEditor::BrowseView::MapView::zoomTo(float x, float y, float z)
{
    zoom = juce::jlimit(1.0f, 40.0f, z);
    centre = { x, y };
    if (zoom <= 1.0f) centre = { 0.5f, 0.5f };
    repaint();
}

void NoctuaryEditor::BrowseView::MapView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    // Zoom about the mouse: the point of the plane under the cursor stays under the cursor, so
    // the eye can follow a cluster in while it opens up.
    const auto pivot = toMapRaw(e.position);
    const float factor = std::pow(1.25f, w.deltaY * 4.0f);
    const float newZoom = juce::jlimit(1.0f, 40.0f, zoom * factor);
    if (newZoom == zoom) return;
    zoom = newZoom;
    const auto r = getLocalBounds().toFloat().reduced(18.0f);
    centre = { pivot.x - (e.position.x - r.getCentreX()) / (zoom * r.getWidth()),
               pivot.y - (r.getCentreY() - e.position.y) / (zoom * r.getHeight()) };
    if (zoom <= 1.0f) centre = { 0.5f, 0.5f };
    repaint();
}

void NoctuaryEditor::BrowseView::MapView::mouseDoubleClick(const juce::MouseEvent&)
{
    zoomTo(0.5f, 0.5f, 1.0f);
}

int NoctuaryEditor::BrowseView::MapView::nearestPreset(juce::Point<float> p, float maxDist) const
{
    int best = -1; float bd = maxDist * maxDist;
    for (int i = 0; i < std::min(numPresetMeta(), numPresets()); ++i) {
        const auto s = toScreen(presetMeta(i).x, presetMeta(i).y);
        const float d = (s.x - p.x) * (s.x - p.x) + (s.y - p.y) * (s.y - p.y);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

void NoctuaryEditor::BrowseView::MapView::paint(juce::Graphics& g)
{
    g.setColour(kBg.brighter(0.03f));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);
    g.setColour(kSectionFill);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 6.0f, 1.0f);
    if (numPresetMeta() == 0) {
        g.setColour(kDim); g.setFont(juce::FontOptions(13.0f));
        g.drawText("The map is not measured yet: run Tools/preset_map.py and rebuild.", getLocalBounds(), juce::Justification::centred);
        return;
    }
    const bool blend = owner.mapActive.getToggleState();
    const float x = owner.proc.engine().getParam(ParamId::MapX), y = owner.proc.engine().getParam(ParamId::MapY);
    const float rad = owner.proc.engine().getParam(ParamId::MapRadius);
    const auto cursor = toScreen(x, y);
    const auto r = getLocalBounds().toFloat().reduced(18.0f);
    if (blend) {   // the blend radius as a soft disc
        const float rr = rad * r.getWidth() * zoom;
        g.setColour(kAccent.withAlpha(0.10f)); g.fillEllipse(cursor.x - rr, cursor.y - rr, 2 * rr, 2 * rr);
        g.setColour(kAccent.withAlpha(0.25f)); g.drawEllipse(cursor.x - rr, cursor.y - rr, 2 * rr, 2 * rr, 1.0f);
    }
    // dimmed points first, then the filtered ones, then the current program
    const bool groups = owner.colourByGroup.getToggleState() && numPresetClusters() > 0;
    const int current = owner.proc.getCurrentProgram();
    // With a preset library loaded there can be thousands of points: shrink the dots so the
    // plane stays readable, and draw the dimmed ones as squares, which is much cheaper to fill.
    const int shown = std::min(numPresetMeta(), numPresets());
    // The dots grow a little with the zoom -- not with it, or a cluster opened up would be
    // blobs -- and everything outside the window is skipped, which at forty times is most of it.
    const float dotScale = juce::jlimit(0.34f, 1.0f, std::sqrt(200.0f / juce::jmax(1, shown))) * std::pow(zoom, 0.35f);
    const bool many = shown > 800;
    const auto screen = getLocalBounds().toFloat().expanded(12.0f);
    // The cloud is the same picture until the window, the zoom, the pan, the filter or the
    // colouring changes, so it is drawn into an image and blitted. Live at fifteen frames a
    // second it was eight and a half thousand filled ellipses per frame.
    if (cloud.isNull() || cloudW != getWidth() || cloudH != getHeight()
        || cloudZoom != zoom || cloudCentre != centre
        || cloudFilter != owner.filterGen || cloudGroups != groups || cloudCount != shown) {
        cloudW = juce::jmax(1, getWidth()); cloudH = juce::jmax(1, getHeight());
        cloudZoom = zoom; cloudCentre = centre; cloudFilter = owner.filterGen;
        cloudGroups = groups; cloudCount = shown;
        cloud = juce::Image(juce::Image::ARGB, cloudW, cloudH, true);
        juce::Graphics cg(cloud);
        std::vector<bool> inFilter(static_cast<size_t>(numPresets()), false);
        for (int i : owner.filtered) inFilter[static_cast<size_t>(i)] = true;
        for (int pass = 0; pass < 2; ++pass) {
            for (int i = 0; i < shown; ++i) {
                if (inFilter[static_cast<size_t>(i)] != (pass == 1)) continue;
                const PresetMeta& m = presetMeta(i);
                const auto s = toScreen(m.x, m.y);
                if (!screen.contains(s)) continue;
                const float size = juce::jmax(2.0f, (6.0f + 6.0f * m.density) * dotScale);
                juce::Colour c = groups ? clusterColour(presetClusterOf(m)) : familyColour(m.family);
                if (pass == 0) c = c.withAlpha(0.18f);
                cg.setColour(c);
                if (pass == 0 && many) cg.fillRect(s.x - size / 2, s.y - size / 2, size, size);
                else                   cg.fillEllipse(s.x - size / 2, s.y - size / 2, size, size);
            }
        }
    }
    g.drawImageAt(cloud, 0, 0);
    // The favourites, ringed in the star's gold, live over the cloud: a star is set with a click
    // and the cloud must not be drawn again for that. Only those the filter lets through, so the
    // map and the list agree.
    if (owner.proc.favouriteCount() > 0) {
        g.setColour(juce::Colour(0xffe0c070).withAlpha(0.9f));
        for (int i : owner.filtered) {
            if (i >= shown || !owner.proc.isFavourite(i)) continue;
            const PresetMeta& m = presetMeta(i);
            const auto s = toScreen(m.x, m.y);
            if (!screen.contains(s)) continue;
            const float size = juce::jmax(2.0f, (6.0f + 6.0f * m.density) * dotScale);
            g.drawEllipse(s.x - size / 2 - 2.0f, s.y - size / 2 - 2.0f, size + 4.0f, size + 4.0f, 1.2f);
        }
    }
    // The chosen preset's own ring is live: picking one in the list must not redraw the cloud.
    if (owner.selected >= 0 && owner.selected < shown && owner.selected != current) {
        const PresetMeta& m = presetMeta(owner.selected);
        const auto s = toScreen(m.x, m.y);
        const float size = juce::jmax(2.0f, (6.0f + 6.0f * m.density) * dotScale);
        g.setColour(kText);
        g.drawEllipse(s.x - size / 2 - 3, s.y - size / 2 - 3, size + 6, size + 6, 1.5f);
    }
    // What is playing, marked so it can be found at a glance: everything else on this plane is a
    // dot of a few pixels, and "where am I" is the first question a map has to answer. A ring, a
    // crosshair, and the name -- at every zoom, whether or not it survived the filter. While a
    // preset change is travelling (the browser's "morph into it"), a line runs from where the
    // sound started to where it is going, filled in as far as it has come.
    // The ring is on the preset the instrument has been given; while a transition is in flight the
    // sound is still on its way there from the one that is leaving, so the line is drawn from THAT
    // one to here. It used to be drawn from the program to the morph target, which were the same
    // preset from the moment the change was made once a preset change became a crossfade -- a line
    // of no length, and its label printed over the ring's own name.
    const int from = owner.proc.morphingFrom();
    const bool travelling = from >= 0 && from < shown && from != current;
    if (current >= 0 && current < shown) {
        const PresetMeta& m = presetMeta(current);
        const auto s = toScreen(m.x, m.y);
        if (travelling) {
            const PresetMeta& mf = presetMeta(from);
            const auto d = toScreen(mf.x, mf.y);
            const float t = juce::jlimit(0.0f, 1.0f, owner.proc.morphProgress());
            g.setColour(kAccent.withAlpha(0.35f));
            g.drawLine(juce::Line<float>(d, s), 1.4f);               // the whole way
            const juce::Point<float> at = d + (s - d) * t;           // how far it has come
            g.setColour(kAccent);
            g.drawLine(juce::Line<float>(d, at), 2.2f);
            g.fillEllipse(at.x - 4.0f, at.y - 4.0f, 8.0f, 8.0f);
            // Both labels stay at the departure, where nothing else writes: the moving point
            // carries the eye, and a number that travels with it would end up on the ring's name.
            g.setColour(kText.withAlpha(0.75f)); g.setFont(juce::FontOptions(10.5f));
            g.drawText(preset(from).name, static_cast<int>(d.x) + 10, static_cast<int>(d.y) - 7, 240, 14,
                       juce::Justification::centredLeft);
            g.setColour(kAccent.withAlpha(0.8f)); g.setFont(juce::FontOptions(9.5f));
            g.drawText(juce::String(juce::roundToInt(t * 100.0f)) + " % across",
                       static_cast<int>(d.x) + 10, static_cast<int>(d.y) + 7, 160, 12, juce::Justification::centredLeft);
        }
        const float r = 13.0f;
        g.setColour(kAccent.withAlpha(0.16f)); g.fillEllipse(s.x - r, s.y - r, 2 * r, 2 * r);
        g.setColour(kAccent);                  g.drawEllipse(s.x - r, s.y - r, 2 * r, 2 * r, 2.0f);
        g.setColour(kAccent.withAlpha(0.55f)); g.drawEllipse(s.x - r - 4.0f, s.y - r - 4.0f, 2 * r + 8.0f, 2 * r + 8.0f, 1.0f);
        for (int k = 0; k < 4; ++k) {   // a crosshair, so it is found even inside a dense ball
            const float a = k * 1.57079633f;
            const float c1 = std::cos(a), s1 = std::sin(a);
            g.drawLine(s.x + c1 * (r + 3.0f), s.y + s1 * (r + 3.0f), s.x + c1 * (r + 9.0f), s.y + s1 * (r + 9.0f), 1.6f);
        }
        g.setColour(kText); g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        g.drawText(preset(current).name, static_cast<int>(s.x) + 18, static_cast<int>(s.y) - 8, 240, 15, juce::Justification::centredLeft);
        g.setColour(kAccent.withAlpha(0.8f)); g.setFont(juce::FontOptions(9.5f));
        // "arriving" while the crossfade runs: the ring is already on the new preset, but what is
        // mostly being heard at that moment is still the old one.
        g.drawText(travelling ? "arriving" : "playing",
                   static_cast<int>(s.x) + 18, static_cast<int>(s.y) + 6, 120, 12, juce::Justification::centredLeft);
    }
    // Names, once there is room for them. Zoomed in past four, every preset in the filter whose
    // label would not sit on another's gets its name; the check is a coarse grid of the label's
    // own size, so a cluster shows the few names that fit and not a smear of all of them.
    if (zoom >= 4.0f) {
        g.setFont(juce::FontOptions(10.5f));
        std::set<std::pair<int, int>> taken;
        const int cw = 130, ch = 13;   // a label's width, so two names never share a line
        // The two labels that were already written -- what is playing, and what is being left --
        // take their cells first, so no other preset's name is printed across them. The grid only
        // ever kept THESE labels apart from each other; the ring's own name was invisible to it.
        auto reserve = [&](int i) {
            if (i < 0 || i >= shown) return;
            const auto p = toScreen(presetMeta(i).x, presetMeta(i).y);
            for (int dx = 0; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                    taken.insert({ static_cast<int>(p.x) / cw + dx, static_cast<int>(p.y) / ch + dy });
        };
        reserve(current);
        if (travelling) reserve(from);
        for (int i : owner.filtered) {
            if (i >= shown) continue;
            // Not the ones that have a label of their own already: what is playing carries its
            // name in bold beside the ring, and a preset being left carries it at the line's
            // start. Drawing them here as well put the same word twice over itself, eleven
            // pixels apart -- one smeared line of glyphs that read as neither name.
            if (i == current || (travelling && i == from)) continue;
            const PresetMeta& m = presetMeta(i);
            const auto s = toScreen(m.x, m.y);
            if (!screen.contains(s)) continue;
            const std::pair<int, int> cell { static_cast<int>(s.x) / cw, static_cast<int>(s.y) / ch };
            if (!taken.insert(cell).second) continue;
            g.setColour(i == owner.selected ? kText : kDim);
            g.drawText(preset(i).name, static_cast<int>(s.x) + 7, static_cast<int>(s.y) - 7, cw + 40, ch, juce::Justification::centredLeft);
        }
    }
    if (zoom > 1.0f) {   // say where we are, and how to get back
        g.setColour(kDim); g.setFont(juce::FontOptions(10.5f));
        g.drawText(juce::String("zoom x") + juce::String(zoom, 1) + "   drag to pan, double-click to reset",
                   getLocalBounds().reduced(10).removeFromTop(14), juce::Justification::topRight);
    }
    // the route: numbered points joined by a line, the segment being walked highlighted
    {
        const Route& rt = owner.proc.engine().route();
        if (rt.count() > 0) {
            const bool running = rt.running();
            const int seg = rt.segment();
            for (int i = 0; i < rt.count(); ++i) {
                const Waypoint& w = rt.point(i);
                const auto s = toScreen(w.x, w.y);
                if (i > 0) {
                    const Waypoint& pw = rt.point(i - 1);
                    const auto ps = toScreen(pw.x, pw.y);
                    g.setColour(juce::Colour(0xffe0c070).withAlpha(running && i == seg ? 0.9f : 0.35f));
                    g.drawLine(juce::Line<float>(ps, s), running && i == seg ? 2.0f : 1.0f);
                }
                g.setColour(juce::Colour(0xffe0c070).withAlpha(0.8f));
                g.drawEllipse(s.x - 9, s.y - 9, 18, 18, 1.2f);
                g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
                g.drawText(juce::String(i + 1), static_cast<int>(s.x) - 9, static_cast<int>(s.y) - 9, 18, 18, juce::Justification::centred);
            }
            if (owner.proc.engine().getParam(ParamId::RouteLoop) >= 0.5f && rt.count() > 1) {
                g.setColour(juce::Colour(0xffe0c070).withAlpha(0.2f));
                g.drawLine(juce::Line<float>(toScreen(rt.point(rt.count() - 1).x, rt.point(rt.count() - 1).y), toScreen(rt.point(0).x, rt.point(0).y)), 1.0f);
            }
        }
    }
    // neighbour lines while blending
    if (blend) {
        const PresetMap::Blend b = PresetMap::neighbours(x, y, rad);
        for (int k = 0; k < b.count; ++k) {
            if (b.weight[k] < 0.02f) continue;
            const PresetMeta& m = presetMeta(b.index[k]);
            const auto s = toScreen(m.x, m.y);
            g.setColour(kAccent.withAlpha(0.2f + 0.7f * b.weight[k]));
            g.drawLine(juce::Line<float>(cursor, s), 1.0f + 2.0f * b.weight[k]);
            g.setFont(juce::FontOptions(11.0f));
            g.drawText(juce::String(preset(b.index[k]).name) + " " + juce::String(static_cast<int>(100 * b.weight[k])) + "%", static_cast<int>(s.x) + 8, static_cast<int>(s.y) - 7, 220, 14, juce::Justification::centredLeft);
        }
        g.setColour(kText);
        g.drawLine(cursor.x - 8, cursor.y, cursor.x + 8, cursor.y, 1.2f);
        g.drawLine(cursor.x, cursor.y - 8, cursor.x, cursor.y + 8, 1.2f);
    }
    if (hover >= 0 && hover < numPresets()) {
        const PresetMeta& m = presetMeta(hover);
        const auto s = toScreen(m.x, m.y);
        juce::String tags;
        for (int t = 0; t < kNumPresetTags; ++t) if (m.tags & (1u << t)) tags += juce::String(presetTagName(t)) + " ";
        g.setColour(kText); g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        g.drawText(preset(hover).name, static_cast<int>(s.x) + 10, static_cast<int>(s.y) - 20, 260, 14, juce::Justification::centredLeft);
        g.setColour(kDim); g.setFont(juce::FontOptions(10.5f));
        g.drawText(juce::String(presetFamilyName(m.family)) + "  -  " + tags.trim(), static_cast<int>(s.x) + 10, static_cast<int>(s.y) - 6, 360, 14, juce::Justification::centredLeft);
    }
    // legend: the groups if the cloud is coloured by them, otherwise where the presets came from
    g.setFont(juce::FontOptions(10.5f));
    int lx = 10, ly = getHeight() - 16;
    const int legendCount = groups ? numPresetClusters() : numPresetFamilies();
    for (int f = 0; f < legendCount; ++f) {
        const char* nm = groups ? presetClusterName(f) : presetFamilyName(f);
        g.setColour(groups ? clusterColour(f) : familyColour(f));
        g.fillEllipse(static_cast<float>(lx), static_cast<float>(ly) + 3.0f, 7.0f, 7.0f);
        g.setColour(kDim); g.drawText(nm, lx + 10, ly, 120, 13, juce::Justification::centredLeft);
        lx += 14 + 7 * juce::jmin(16, static_cast<int>(std::strlen(nm)));
        if (lx > getWidth() - 120) break;
    }
}

void NoctuaryEditor::BrowseView::MapView::mouseMove(const juce::MouseEvent& e)
{
    const int h = nearestPreset(e.position, 10.0f);
    if (h != hover) { hover = h; repaint(); }
}

void NoctuaryEditor::BrowseView::MapView::mouseDown(const juce::MouseEvent& e)
{
    dragging = false;
    panning = false;
    // The right button, the middle button, or a drag on empty plane while the blend cursor is
    // off: all of those pan. With the blend on, the left button is the cursor, as it always was.
    if (e.mods.isRightButtonDown() || e.mods.isMiddleButtonDown()) {
        panning = true; panFrom = e.position; centreFrom = centre; return;
    }
    const int h = nearestPreset(e.position, 10.0f);
    if (h >= 0 && !owner.mapActive.getToggleState()) {   // plain click on a point loads it
        owner.selected = h; owner.proc.selectPreset(h, owner.morphOnSelect.getToggleState()); owner.list.repaint(); repaint(); return;
    }
    if (!owner.mapActive.getToggleState()) { panning = true; panFrom = e.position; centreFrom = centre; return; }
    mouseDrag(e);
}

void NoctuaryEditor::BrowseView::MapView::mouseDrag(const juce::MouseEvent& e)
{
    if (panning) {
        const auto r = getLocalBounds().toFloat().reduced(18.0f);
        centre = { centreFrom.x - (e.position.x - panFrom.x) / (zoom * juce::jmax(1.0f, r.getWidth())),
                   centreFrom.y + (e.position.y - panFrom.y) / (zoom * juce::jmax(1.0f, r.getHeight())) };
        // Keep the plane on screen: the centre may not leave the unit square by more than the
        // half-window, so the last row of presets stays reachable and nothing is lost off the edge.
        const float half = 0.5f / zoom;
        centre = { juce::jlimit(half, 1.0f - half, centre.x), juce::jlimit(half, 1.0f - half, centre.y) };
        repaint();
        return;
    }
    dragging = true;
    if (!owner.mapActive.getToggleState()) return;
    const auto m = toMap(e.position);
    if (auto* px = owner.proc.apvts.getParameter(paramDesc(ParamId::MapX).key)) px->setValueNotifyingHost(m.x);
    if (auto* py = owner.proc.apvts.getParameter(paramDesc(ParamId::MapY).key)) py->setValueNotifyingHost(m.y);
    repaint();
}

void NoctuaryEditor::BrowseView::MapView::mouseUp(const juce::MouseEvent&) { dragging = false; panning = false; }

NoctuaryEditor::PerformView::PerformView(NoctuaryProcessor& p) : proc(p)
{
    for (int m = 0; m < 8; ++m) {
        const ParamDesc& d = paramDesc(static_cast<ParamId>(static_cast<int>(ParamId::MacroA) + m));
        auto s = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
        s->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 20);
        addAndMakeVisible(*s);
        auto l = std::make_unique<juce::Label>(juce::String(), d.name);
        l->setJustificationType(juce::Justification::centred);
        l->setFont(juce::FontOptions(20.0f, juce::Font::bold));
        l->setColour(juce::Label::textColourId, kText);
        addAndMakeVisible(*l);
        attachments.push_back(std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(proc.apvts, d.key, *s));
        knobs.push_back(std::move(s));
        labels.push_back(std::move(l));
    }
    morph.setSliderStyle(juce::Slider::LinearHorizontal);
    morph.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 22);
    addAndMakeVisible(morph);
    morphAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(proc.apvts, paramDesc(ParamId::MorphPos).key, morph);
    morphActive.setButtonText("Morph");
    addAndMakeVisible(morphActive);
    morphActiveAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(proc.apvts, paramDesc(ParamId::MorphActive).key, morphActive);
    for (auto* l : { &morphLabel, &aLabel, &bLabel }) {
        l->setFont(juce::FontOptions(16.0f));
        l->setColour(juce::Label::textColourId, kDim);
        addAndMakeVisible(*l);
    }
    morphLabel.setText("A  <  position  >  B", juce::dontSendNotification);
    morphLabel.setJustificationType(juce::Justification::centred);
    aLabel.setJustificationType(juce::Justification::centredRight);
    bLabel.setJustificationType(juce::Justification::centredLeft);

    // Set timeline
    setRec.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffb03030));
    setRec.onClick = [this] {
        if (proc.isRecordingSet()) {
            chooser = std::make_unique<juce::FileChooser>("Save the set", juce::File(), "*.ambientset");
            chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                [this](const juce::FileChooser& fc) {
                    auto file = fc.getResult();
                    if (file != juce::File() && !file.hasFileExtension("ambientset")) file = file.withFileExtension("ambientset");
                    if (!proc.stopSetRecording(file) && file != juce::File())
                        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Set", "Could not write the set file.");
                    updateSetInfo();
                });
        } else { proc.startSetRecording(); updateSetInfo(); }
    };
    setPlay.onClick = [this] {
        chooser = std::make_unique<juce::FileChooser>("Play a set", juce::File(), "*.ambientset");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& fc) {
                const auto file = fc.getResult();
                if (!file.existsAsFile()) return;
                if (!proc.playSetFile(file)) juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Set", "This is not a readable set file.");
                updateSetInfo();
            });
    };
    setStop.onClick = [this] { proc.stopSetPlayback(); if (proc.isRecordingSet()) proc.stopSetRecording(juce::File()); updateSetInfo(); };
    for (auto* b : { &setRec, &setPlay, &setStop }) addAndMakeVisible(*b);
    setInfo.setFont(juce::FontOptions(12.0f)); setInfo.setColour(juce::Label::textColourId, kDim);
    addAndMakeVisible(setInfo);
    updateSetInfo();
}

void NoctuaryEditor::PerformView::updateSetInfo()
{
    setRec.setToggleState(proc.isRecordingSet(), juce::dontSendNotification);
    const int t = static_cast<int>(proc.setTime());
    juce::String text;
    if (proc.isRecordingSet()) text = juce::String::formatted("recording set  %d:%02d", t / 60, t % 60);
    else if (proc.isPlayingSet()) text = juce::String::formatted("playing set  %d:%02d", t / 60, t % 60);
    else text = "a set = every knob, gesture and note with its time; Record, play a while, stop to save; render it again with ambient_render --set-file";
    setInfo.setText(text, juce::dontSendNotification);
}

void NoctuaryEditor::PerformView::paint(juce::Graphics& g)
{
    g.fillAll(kBg);
    g.setColour(kGroupFill);
    g.fillRoundedRectangle(getLocalBounds().reduced(12).toFloat(), 10.0f);
    g.setColour(kDim);
    g.setFont(juce::FontOptions(13.0f));
    g.drawText("PERFORM  -  eight macros, one morph. Each knob moves several parameters; a knob acts once it has been moved.",
               getLocalBounds().reduced(24).removeFromTop(24), juce::Justification::centredLeft);
    aLabel.setText("A: " + proc.morphSlotName(0), juce::dontSendNotification);
    bLabel.setText("B: " + proc.morphSlotName(1), juce::dontSendNotification);
    updateSetInfo();
}

void NoctuaryEditor::PerformView::resized()
{
    auto area = getLocalBounds().reduced(24);
    auto top = area.removeFromTop(32);
    {   // set controls on the right of the title line
        auto s = top.removeFromRight(juce::jmin(720, top.getWidth() - 620));
        setStop.setBounds(s.removeFromRight(90).reduced(0, 4)); s.removeFromRight(6);
        setPlay.setBounds(s.removeFromRight(100).reduced(0, 4)); s.removeFromRight(6);
        setRec.setBounds(s.removeFromRight(100).reduced(0, 4)); s.removeFromRight(10);
        setInfo.setBounds(s);
    }
    auto morphArea = area.removeFromBottom(90);
    const int cols = 4, rows = 2;
    const int cellW = area.getWidth() / cols, cellH = area.getHeight() / rows;
    for (int m = 0; m < 8; ++m) {
        juce::Rectangle<int> cell(area.getX() + (m % cols) * cellW, area.getY() + (m / cols) * cellH, cellW, cellH);
        cell = cell.reduced(16);
        labels[static_cast<size_t>(m)]->setBounds(cell.removeFromBottom(30));
        const int side = std::min(cell.getWidth(), cell.getHeight());
        knobs[static_cast<size_t>(m)]->setBounds(cell.withSizeKeepingCentre(side, side));
    }
    morphActive.setBounds(morphArea.removeFromLeft(90).withSizeKeepingCentre(90, 28));
    morphLabel.setBounds(morphArea.removeFromTop(22));
    aLabel.setBounds(morphArea.removeFromLeft(220));
    bLabel.setBounds(morphArea.removeFromRight(220));
    morph.setBounds(morphArea.withSizeKeepingCentre(morphArea.getWidth(), 40));
}
