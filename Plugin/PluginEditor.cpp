/**
 * @file PluginEditor.cpp
 * @brief The editor's core: construction, the cells and sections, the layout, undo, the help page
 *        and the manual, the timer and the header's painting.
 *
 * This is the first and largest of the editor's translation units (the others are EditorViews.cpp,
 * EditorBrowse.cpp, EditorModStrip.cpp, EditorDrag.cpp, EditorMatrix.cpp and EditorSpectrum.cpp;
 * PluginEditor.h says which class each holds). What is here:
 *
 * - The constructor: the group and tab tables that arrange the sections, the header's buttons and
 *   boxes, the displays handed to their rows, the pages and the strip, the design size measured
 *   from the first layout, and the `AMBIENT_*` environment variables that open the panel in a
 *   given state for a picture or write the manual out.
 * - The cells: one per parameter, built from ambient::paramTable() (buildCells), plus the extra
 *   cells -- file loaders, the section preset banks, the morph slots, the journey row.
 * - The layout: which cells a section shows and whether it is closed (a function of the parameter
 *   state alone), the Expanded page with its wrapping rows (layoutBody), the tabbed page with every
 *   page fitted to its column (layoutTabbed), the tab click, and the window that follows the
 *   design's shape.
 * - Undo, redo, A/B and the die, all whole-parameter snapshots.
 * - The help page and the manual: the snapshots of sections, tabs, pages and the strip that both
 *   are made of, the ManualJob that writes them into a folder step by step, and the signal-flow
 *   diagram.
 * - The timer, which keeps the panel in step with the processor, and the cells a slot's type uses.
 * - The header's painting and the content's: groups, tabs, sections, dice and the notes on them.
 *
 * Everything here runs on the message thread and reads the engine only through
 * NoctuaryProcessor and the APVTS; the displays are the same, each on its own timer.
 */
#include "EditorCommon.h"

using namespace ambient;

namespace {
/**
 * @brief The library runs to thousands of presets.
 *
 * As one flat list with headings between the families
 * this was 8596 rows in a single popup -- unreadable, and past some size unusable: the menu opened
 * and a click on a row did nothing at all, the box kept the name it had, and the sound with it.
 * One submenu per family instead. The root holds sixty entries, each of them a few dozen, and the
 * ComboBox still reports the chosen id through onChange exactly as before.
 *
 * Fills the Sound box in the header and the two morph slot choosers; item id = preset index + 1.
 * @param box  the chooser to fill (emptied of nothing: called once on a fresh box)
 */
void fillPresetBox(juce::ComboBox& box)
{
    const bool grouped = numPresetMeta() >= numPresets() && numPresetFamilies() > 1;
    if (!grouped) {
        for (int i = 0; i < numPresets(); ++i) box.addItem(preset(i).name, i + 1);
        return;
    }
    juce::PopupMenu* root = box.getRootMenu();
    juce::PopupMenu family;
    int lastFamily = -1;
    for (int i = 0; i < numPresets(); ++i) {
        const int fam = presetMeta(i).family;
        if (fam != lastFamily) {
            if (lastFamily >= 0) root->addSubMenu(presetFamilyName(lastFamily), family);
            family.clear();
            lastFamily = fam;
        }
        family.addItem(i + 1, preset(i).name);
    }
    if (lastFamily >= 0) root->addSubMenu(presetFamilyName(lastFamily), family);
}
} // namespace


namespace {
}

NoctuaryEditor::NoctuaryEditor(NoctuaryProcessor& p)
    : AudioProcessorEditor(p), proc_(p)
{
    setLookAndFeel(&laf_);

    // Two columns, everything in sight at once: the voice and the morph on the left, the effects,
    // the cosmos and the conductor on the right. Rows whose sections are of a kind (the three
    // sources, the two filters, the effect pairs, the conductor's tables) page through tabs.
    // Every page is fitted to its column (layoutTabbed), so what the tables decide is which
    // sections share a page, and the pages are cut so that each fills its row: no page of one
    // small section with nothing beside it, and every page that can have a display has one.
    groups_ = {
        { "VOICE",      kVoice,     { { "Source 1", "Strands", "Source 2", "Source 3", "Source 4" }, { "Air", "Filter", "Z-Plane", "Envelope", "Expression" }, { "Space", "Foundation" } }, {}, 0 },   // rows 0 and 1 page through tabs
        // Morph, the macros and the vector in one row: the three controls that act on the whole
        // sound at once, with the vector's square as the row's display. (The vector used to be a
        // tab of the sources, and the macros a page with nothing beside them.)
        { "MORPH",      kMorph,     { { "Morph", "Macros", "Vector" } }, {}, 0 },
        { "FOREGROUND", kFore,      { { "Ensemble", "Delay", "Delay 2", "Near Reverb", "Blur" } }, {}, 1 },
        { "BACKGROUND", kBack,      { { "Cloud", "Far Reverb", "Feedback", "Room", "Early Room", "Body", "Patina" } }, {}, 1 },
        // Strike shares the Cosmos group as a tab: like the Cosmos it is a sound source that is
        // not one of the four oscillators, and beside them it read as a fifth. The Memory is the
        // second parallel world beside the Cosmos, and takes the third tab.
        // The near layer takes the last two tabs: its source and its events, each a page.
        { "COSMOS",     kCosmos,    { { "Cosmos", "Strike", "Memory", "Near Source", "Near Events" } }, {}, 1 },
        // Last in the right column and stretchy: every page of it has a display, and whatever
        // height the left column has over goes to that display, the page refitted taller.
        { "CONDUCTOR",  kConductor, { { "Cluster Brain", "Autoplay", "Brain 2", "Tuning", "Coherence", "Clock" } }, {}, 1, {}, 0, true },
        // Last in the left column, and the only group with no controls in it: the right column is
        // taller than the left, and the difference used to be an empty rectangle the width of the
        // page. It now holds the output's spectrum, and because the row stretches, the hole cannot
        // come back when a tab changes the height of a row above it.
        { "ANALYSIS",   kVoice,     { {} }, {}, 0, {}, 96, true },
    };
    tabRows_ = {
        // The strand bank is on Source 1's page beside the strip (it has no cells while the slot
        // is not additive, and then it is not there); the two filter pages share the filter's
        // picture; the envelope and the expression share a page and the envelope's picture.
        { 0, 0, { "SOURCE 1", "SOURCE 2", "SOURCE 3", "SOURCE 4" }, { { "Source 1", "Strands" }, { "Source 2" }, { "Source 3" }, { "Source 4" } } },
        { 0, 1, { "FILTER", "Z-PLANE", "ENVELOPE + EXPRESSION" }, { { "Air", "Filter" }, { "Z-Plane" }, { "Envelope", "Expression" } } },
        { 2, 0, { "ENSEMBLE + DELAY", "DELAY 2 + NEAR REVERB + BLUR" }, { { "Ensemble", "Delay" }, { "Delay 2", "Near Reverb", "Blur" } } },
        { 3, 0, { "CLOUD + FAR REVERB", "FEEDBACK + ROOM", "EARLY ROOM + BODY + PATINA" }, { { "Cloud", "Far Reverb" }, { "Feedback", "Room" }, { "Early Room", "Body", "Patina" } } },
        { 4, 0, { "COSMOS", "STRIKE", "MEMORY", "NEAR SOURCE", "NEAR EVENTS" }, { { "Cosmos" }, { "Strike" }, { "Memory" }, { "Near Source" }, { "Near Events" } } },
        // The clock shares the coherence page: three cells alone were a page three quarters empty.
        { 5, 0, { "BRAIN", "AUTOPLAY", "BRAIN 2", "TUNING", "COHERENCE + CLOCK" }, { { "Cluster Brain" }, { "Autoplay" }, { "Brain 2" }, { "Tuning" }, { "Coherence", "Clock" } } },
    };

    content_.onPaint = [this](juce::Graphics& g) { paintContent(g); };
    content_.onMouse = [this](const juce::MouseEvent& e) {
        if (e.mods.isPopupMenu()) return;
        for (const auto& d : diceOf_)
            if (d.second.contains(e.getPosition())) { randomiseSection(d.first, e.mods.isShiftDown()); return; }
        clickTabs(e.getPosition());
    };
    viewport_.setViewedComponent(&content_, false);
    viewport_.setScrollBarsShown(false, false);
    addAndMakeVisible(viewport_);
    addAndMakeVisible(dragOverlay_);
    buildCells();
    colourCellsByGroup();
    updateSourceCells();   // which cells the types use, before the first layout measures the page
    // The Master section lives in the header, so its cells belong to the editor, not the content.
    if (Section* ms = findSection("Master"))
        for (int ci : ms->cells) { addAndMakeVisible(*cells_[static_cast<size_t>(ci)].comp); addAndMakeVisible(*cells_[static_cast<size_t>(ci)].label); }

    recButton_ = std::make_unique<juce::TextButton>("Rec");
    recButton_->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffb03030));
    recButton_->setClickingTogglesState(false);
    recButton_->onClick = [this] {
        if (proc_.isRecording()) { proc_.stopRecording(); recButton_->setToggleState(false, juce::dontSendNotification); return; }
        chooser_ = std::make_unique<juce::FileChooser>("Record to WAV", juce::File(), "*.wav");
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
            [this](const juce::FileChooser& fc) {
                auto file = fc.getResult();
                if (file == juce::File()) return;
                if (!file.hasFileExtension("wav")) file = file.withFileExtension("wav");
                if (proc_.startRecording(file)) recButton_->setToggleState(true, juce::dontSendNotification);
                else juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Record", "Could not open the file for writing.");
            });
    };
    addAndMakeVisible(*recButton_);
    // Main: the panel, from wherever you are. It is the one page that had no button of its own --
    // you got back by switching the others off, which is a rule nobody should have to learn.
    mainButton_ = std::make_unique<juce::TextButton>("Main");
    mainButton_->setTooltip("The panel: every section on one page");
    mainButton_->setClickingTogglesState(true);
    mainButton_->setColour(juce::TextButton::buttonOnColourId, kAccent.withAlpha(0.5f));
    mainButton_->setToggleState(true, juce::dontSendNotification);
    mainButton_->onClick = [this] { setPage(0); };
    addAndMakeVisible(*mainButton_);
    // The two headset controls under one button: on a desk they are the two you never press.
    calibButton_ = std::make_unique<juce::TextButton>("Calibrate");
    calibButton_->onClick = [this] { proc_.gestures().startCalibration(6.0f); };
    mapButton_ = std::make_unique<juce::TextButton>("Gestures...");
    mapButton_->onClick = [this] { showMappingEditor(); };
    vrButton_ = std::make_unique<juce::TextButton>("VR");
    vrButton_->setTooltip("Hand tracking: calibrate the hands, edit the gesture table");
    vrButton_->onClick = [this] {
        juce::PopupMenu m;
        m.addItem(1, "Calibrate hands (6 s: together and apart, low and high, near and far)");
        m.addItem(2, "Gestures... (the gesture / macro mapping table)");
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(vrButton_.get()), [this](int r) {
            if (r == 1) proc_.gestures().startCalibration(6.0f);
            else if (r == 2) showMappingEditor();
        });
    };
    addAndMakeVisible(*vrButton_);
    performButton_ = std::make_unique<juce::TextButton>("Perform");
    performButton_->setTooltip("Only the eight macros and the morph, large: for playing a set");
    performButton_->setClickingTogglesState(true);
    performButton_->setColour(juce::TextButton::buttonOnColourId, kAccent.withAlpha(0.5f));
    performButton_->onClick = [this] { setPage(performButton_->getToggleState() ? 1 : 0); };
    addAndMakeVisible(*performButton_);
    perform_ = std::make_unique<PerformView>(proc_);
    addChildComponent(*perform_);
    browseButton_ = std::make_unique<juce::TextButton>("Browse");
    browseButton_->setTooltip("Preset browser: filters, tags, and the map of all presets (drag the cursor to blend)");
    browseButton_->setClickingTogglesState(true);
    browseButton_->setColour(juce::TextButton::buttonOnColourId, kAccent.withAlpha(0.5f));
    browseButton_->onClick = [this] { setPage(browseButton_->getToggleState() ? 2 : 0); };
    addAndMakeVisible(*browseButton_);
    browse_ = std::make_unique<BrowseView>(proc_);
    addChildComponent(*browse_);
    helpButton_ = std::make_unique<juce::TextButton>("Help");
    helpButton_->setTooltip("The manual, by topic (F1)");
    helpButton_->setClickingTogglesState(true);
    helpButton_->setColour(juce::TextButton::buttonOnColourId, kAccent.withAlpha(0.5f));
    helpButton_->onClick = [this] { setPage(helpButton_->getToggleState() ? 3 : 0); };
    addAndMakeVisible(*helpButton_);
    help_ = std::make_unique<HelpView>(proc_, *this);
    addChildComponent(*help_);
    // Which build this is. Asked for after a release where the answer mattered: two installers
    // carried the same version string for twenty minutes, and there was no way to tell from the
    // window which one was running.
    aboutButton_ = std::make_unique<juce::TextButton>("?");
    aboutButton_->setTooltip("Version and what is loaded");
    aboutButton_->onClick = [this] {
        const int packs = ambient::numPresetPacks();
        juce::String text;
        text << "Noctuary " << JucePlugin_VersionString << "\n"
             << "built " << juce::String(__DATE__).trim() << ", " << __TIME__ << "\n\n"
             << ambient::numPresets() << " presets";
        if (packs > 0) text << " (" << ambient::builtinPresetCount() << " built in, " << packs << " packs)";
        text << "\n" << ambient::numPresetClusters() << " measured groups, "
             << ambient::numPresetPhrases() << " phrases\n\n"
             << "Sample library: "
             << (proc_.engine().displayTexture(0) != nullptr && !proc_.engine().displayTexture(0)->empty()
                     ? "a clip is loaded" : "nothing loaded in slot 1")
             << "\n" << JucePlugin_Manufacturer << "   " << "github.com/reneweller-coding/Noctuary";
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::NoIcon, "About Noctuary", text, "Close", this);
    };
    addAndMakeVisible(*aboutButton_);
    undoButton_ = std::make_unique<juce::TextButton>("Undo");
    undoButton_->setTooltip("Undo the last preset, die roll or A/B swap (Ctrl+Z)");
    undoButton_->onClick = [this] { doUndo(); };
    addAndMakeVisible(*undoButton_);
    redoButton_ = std::make_unique<juce::TextButton>("Redo");
    redoButton_->setTooltip("Redo (Ctrl+Y)");
    redoButton_->onClick = [this] { doRedo(); };
    addAndMakeVisible(*redoButton_);
    abButton_ = std::make_unique<juce::TextButton>("A | B");
    abButton_->setTooltip("Two whole snapshots to compare: the first click parks what you have in A and hands you B, every click after swaps");
    abButton_->onClick = [this] { swapAB(); };
    addAndMakeVisible(*abButton_);
    // The layout, cycled by one button: Normal (one page, tabs), Compact (the same page in
    // narrower columns, every page refitted taller) and Expanded (every page of every tab row
    // under one another, no tabs -- for a tall screen, or for reading a preset through).
    compactButton_ = std::make_unique<juce::TextButton>("Normal");
    compactButton_->setTooltip("Layout. Normal: one page with tabs. Compact: the same page in narrower columns, every section refitted taller. Expanded: every page of every tab row laid out under one another -- no tabs, a wide page, everything in sight.");
    compactButton_->setClickingTogglesState(false);
    compactButton_->setColour(juce::TextButton::buttonOnColourId, kAccent.withAlpha(0.5f));
    compactButton_->onClick = [this] { applyLayoutMode((proc_.layoutMode() + 1) % 3); };
    addAndMakeVisible(*compactButton_);
    // Session recall, and the switch for it. Only in the standalone: in a plugin the host saves
    // the state with the project, which is what a plugin is supposed to do.
    if (NoctuaryProcessor::sessionRecallAvailable()) {
        recallButton_ = std::make_unique<juce::TextButton>("Recall");
        recallButton_->setTooltip("Start where you left off: the whole state is kept while the instrument runs and comes back next time. Off means it always starts at Init, and what was stored is forgotten.");
        recallButton_->setClickingTogglesState(true);
        recallButton_->setToggleState(proc_.sessionRecall(), juce::dontSendNotification);
        recallButton_->setColour(juce::TextButton::buttonOnColourId, kAccent.withAlpha(0.5f));
        recallButton_->onClick = [this] { proc_.setSessionRecall(recallButton_->getToggleState()); };
        addAndMakeVisible(*recallButton_);
    }
    outputView_ = std::make_unique<LoudnessView>(proc_);
    addAndMakeVisible(*outputView_);
    tooltips_ = std::make_unique<juce::TooltipWindow>(nullptr, 600);
    setWantsKeyboardFocus(true);

    // Header controls
    soundBox_ = std::make_unique<juce::ComboBox>();
    soundBox_->setTextWhenNothingSelected("Sound preset");
    fillPresetBox(*soundBox_);
    soundBox_->setSelectedId(proc_.soundPresetIndex() + 1, juce::dontSendNotification);
    soundBox_->onChange = [this] {
        const int idx = soundBox_->getSelectedId() - 1;
        if (idx >= 0 && idx != proc_.soundPresetIndex()) { pushUndo("preset"); proc_.applySoundPreset(idx); }
    };
    addAndMakeVisible(*soundBox_);

    saveButton_ = std::make_unique<juce::TextButton>("Save...");
    saveButton_->onClick = [this] {
        chooser_ = std::make_unique<juce::FileChooser>("Save preset", juce::File(), "*.noctuary");
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
            [this](const juce::FileChooser& fc) {
                auto file = fc.getResult();
                if (file == juce::File()) return;
                if (!file.hasFileExtension("noctuary")) file = file.withFileExtension("noctuary");
                if (!proc_.savePresetFile(file))
                    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Preset", "Could not write the preset file.");
            });
    };
    addAndMakeVisible(*saveButton_);
    loadButton_ = std::make_unique<juce::TextButton>("Load...");
    loadButton_->onClick = [this] {
        chooser_ = std::make_unique<juce::FileChooser>("Load preset", juce::File(), "*.noctuary");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& fc) {
                const auto file = fc.getResult();
                if (!file.existsAsFile()) return;
                if (!proc_.loadPresetFile(file))
                    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Preset", "This is not an Noctuary preset file.");
                repaint();
            });
    };
    addAndMakeVisible(*loadButton_);

    // Master gain knob lives in the header next to the Master section.
    master_ = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
    master_->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 16);
    addAndMakeVisible(*master_);
    masterAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(proc_.apvts, "master_gain", *master_);
    undoHook_.editor = this;
    undoHook_.names[master_.get()] = "Master"; master_->addMouseListener(&undoHook_, false);

    mod_ = std::make_unique<ModView>(proc_, *this);
    addAndMakeVisible(*mod_);

    // Displays inside the grid: each fills the room its row leaves after the knobs.
    scope_ = std::make_unique<ScopeView>(proc_);
    filterView_ = std::make_unique<FilterView>(proc_);
    source1View_ = std::make_unique<SourceView>(proc_, 1);
    source2View_ = std::make_unique<SourceView>(proc_, 2);
    source3View_ = std::make_unique<SourceView>(proc_, 3);
    source4View_ = std::make_unique<SourceView>(proc_, 4);
    brainView_ = std::make_unique<BrainView>(proc_);
    brainView2_ = std::make_unique<BrainView>(proc_);
    brainView3_ = std::make_unique<BrainView>(proc_);
    stageView_ = std::make_unique<StageView>(proc_);
    cosmosView_ = std::make_unique<CosmosView>(proc_);
    envView_ = std::make_unique<EnvView>(proc_);
    vectorView_ = std::make_unique<VectorView>(proc_);
    spectrumView_ = std::make_unique<SpectrumView>(proc_);
    tuningView_ = std::make_unique<TuningView>(proc_);
    coherenceView_ = std::make_unique<CoherenceView>(proc_);
    stageView_->onUndo = [this](const juce::String& what) { pushUndo(what); };
    for (juce::Component* c : { static_cast<juce::Component*>(scope_.get()), static_cast<juce::Component*>(filterView_.get()),
                                static_cast<juce::Component*>(source2View_.get()), static_cast<juce::Component*>(source3View_.get()),
                                static_cast<juce::Component*>(source4View_.get()),
                                static_cast<juce::Component*>(source1View_.get()), static_cast<juce::Component*>(brainView_.get()),
                                static_cast<juce::Component*>(brainView2_.get()),
                                static_cast<juce::Component*>(brainView3_.get()),
                                static_cast<juce::Component*>(stageView_.get()), static_cast<juce::Component*>(cosmosView_.get()),
                                static_cast<juce::Component*>(envView_.get()),
                                static_cast<juce::Component*>(spectrumView_.get()),
                                static_cast<juce::Component*>(vectorView_.get()),
                                static_cast<juce::Component*>(tuningView_.get()), static_cast<juce::Component*>(coherenceView_.get()) })
        content_.addAndMakeVisible(*c);
    if (tabRows_.size() > 5 && groups_.size() > 6) {
        tabRows_[0].displays = { source1View_.get(), source2View_.get(), source3View_.get(), source4View_.get() };
        // The strand scope drew the bank's cycle on the Strands tab; that tab is gone and the
        // additive-bank picture on Source 1's page shows the same partials. Expanded uses it.
        if (scope_) scope_->setVisible(false);
        // The filter picture draws both filters, so it is the display of both filter pages.
        tabRows_[1].displays = { filterView_.get(), filterView_.get(), envView_.get() };
        tabRows_[4].displays = { cosmosView_.get(), nullptr, nullptr };                                                           // COSMOS | STRIKE | MEMORY
        tabRows_[5].displays = { brainView_.get(), brainView3_.get(), brainView2_.get(), tuningView_.get(), coherenceView_.get() };   // BRAIN | AUTOPLAY | BRAIN 2 | TUNING | COHERENCE + CLOCK
        groups_[0].displays = { nullptr, nullptr, stageView_.get() };   // VOICE: the Space / Foundation row
        groups_[1].displays = { vectorView_.get() };                    // MORPH: the vector's square
        groups_[6].displays = { spectrumView_.get() };                  // ANALYSIS: the strip
    }
    // The Expanded page. Three columns: the voice's sources and its room on the left; its
    // shaping (filters, envelope, expression, z-plane, vector) with the effects, the background
    // and the small sections in the middle; morph, macros and the conductor on the right. The
    // closed sections -- feedback, room, early room, body, patina, cosmos, strike -- share one
    // row, which wraps if several of them are open; so do morph, macros and the second
    // conductor. The spectrum is the middle column's filler, small.
    expandedGroups_ = {
        // Rows without a display are flows: their sections run left to right and wrap, so a row
        // is as full as its sections allow and the closed ones sit beside the open ones. The
        // strand bank gets the bank's own scope as its display, and the stage grows to close
        // the column.
        { "VOICE",      kVoice,     { { "Source 1" }, { "Strands" }, { "Source 2" }, { "Source 3" }, { "Source 4" }, { "Space", "Foundation" } }, {}, 0,
                                    { source1View_.get(), scope_.get(), source2View_.get(), source3View_.get(), source4View_.get(), stageView_.get() }, 0, true },
        { "SHAPE",      kVoice,     { { "Air", "Filter" }, { "Envelope", "Expression" }, { "Z-Plane", "Vector" } }, {}, 1,
                                    { filterView_.get(), envView_.get(), vectorView_.get() } },
        { "FOREGROUND", kFore,      { { "Ensemble", "Delay", "Delay 2", "Near Reverb", "Blur" } }, {}, 1 },
        { "BACKGROUND", kBack,      { { "Cloud", "Far Reverb", "Feedback", "Room", "Early Room", "Body", "Patina", "Cosmos", "Strike", "Memory", "Near Source", "Near Events" } }, {}, 1 },
        { "ANALYSIS",   kVoice,     { {} }, {}, 1, { spectrumView_.get() }, 80, true },
        { "MORPH",      kMorph,     { { "Morph", "Macros", "Brain 2", "Clock" } }, {}, 2 },
        { "CONDUCTOR",  kConductor, { { "Cluster Brain" }, { "Autoplay" }, { "Tuning" }, { "Coherence" } }, {}, 2,
                                    { brainView_.get(), brainView3_.get(), tuningView_.get(), coherenceView_.get() }, 0, true },
    };

    // Free scaling: the corner is the zoom. The ratio is fixed so the arrangement never changes,
    // only its size, and the window opens at whatever fraction of the screen actually fits.
    setResizable(true, true);
    layoutBody();                       // measure once: the design size is what the body needs
    // Width and height: exactly what header, body and modulation strip need, so nothing scrolls.
    designW_ = std::max(kMinDesignW, bodyW_);
    designH_ = std::max(kMinDesignH, kHeaderH + bodyH_ + kStripH);
    if (auto* con = getConstrainer()) {
        con->setFixedAspectRatio(static_cast<double>(designW_) / static_cast<double>(designH_));
        con->setSizeLimits(designW_ / 3, designH_ / 3, designW_ * 2, designH_ * 2);
    }
    {
        float fit = 1.0f;
        if (auto* screen = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()) {
            const auto area = screen->userArea;
            fit = juce::jlimit(0.34f, 1.0f, juce::jmin(area.getWidth() * 0.94f / designW_,
                                                      area.getHeight() * 0.90f / designH_));
        }
        setSize(juce::roundToInt(designW_ * fit), juce::roundToInt(designH_ * fit));
    }
    {
        int mode = proc_.layoutMode();
        if (juce::SystemStats::getEnvironmentVariable("AMBIENT_COMPACT", "").isNotEmpty()) mode = 1;
        if (juce::SystemStats::getEnvironmentVariable("AMBIENT_EXPANDED", "").isNotEmpty()) mode = 2;
        const juce::String lm = juce::SystemStats::getEnvironmentVariable("AMBIENT_LAYOUT", "");   // 0, 1 or 2, over the recalled one
        if (lm.isNotEmpty()) mode = juce::jlimit(0, 2, lm.getIntValue());
        if (mode != 0) applyLayoutMode(mode);
        // A picture is taken at design size, so its pixels are the layout's own measurements.
        if (juce::SystemStats::getEnvironmentVariable("AMBIENT_SHOT", "").isNotEmpty()) setSize(designW_, designH_);
    }
    if (juce::SystemStats::getEnvironmentVariable("AMBIENT_PERFORM", "").isNotEmpty()) setPage(1);
    {   // AMBIENT_PRESET=<name>: open on a named preset (dev aid for photographing a modulated patch)
        const juce::String ps = juce::SystemStats::getEnvironmentVariable("AMBIENT_PRESET", "");
        for (int i = 0; ps.isNotEmpty() && i < numPresets(); ++i)
            if (ps == preset(i).name) { proc_.applySoundPreset(i); if (soundBox_) soundBox_->setSelectedId(i + 1, juce::dontSendNotification); }
        // AMBIENT_MORPH_TO=<name>: a second in, start a transition to this one, so the map's
        // crossing -- the line, the travelling point, the two labels -- can be photographed while
        // it is happening. Alongside AMBIENT_SHOT, which captures ten seconds after the chord.
        const juce::String mt = juce::SystemStats::getEnvironmentVariable("AMBIENT_MORPH_TO", "");
        for (int i = 0; mt.isNotEmpty() && i < numPresets(); ++i)
            if (mt == preset(i).name) {
                juce::Timer::callAfterDelay(1200, [safe = juce::Component::SafePointer<NoctuaryEditor>(this), i] {
                    if (safe != nullptr) safe->proc_.selectPreset(i, true);
                });
                break;
            }
    }   // open on the perform page
    {   // dev aids: AMBIENT_BROWSE=1|map opens the browser (map view with "map"), AMBIENT_ROUTE=<route preset> preloads a route
        const juce::String br = juce::SystemStats::getEnvironmentVariable("AMBIENT_BROWSE", "");
        if (br.isNotEmpty()) { setPage(2); if (br == "map") browse_->setMode(1); }
        const int hv = juce::SystemStats::getEnvironmentVariable("AMBIENT_HELP", "0").getIntValue();   // AMBIENT_HELP=<topic+1>: open the manual there
        if (hv > 0) { setPage(3); if (help_) help_->topics.selectRow(hv - 1); }
        const juce::String sc = juce::SystemStats::getEnvironmentVariable("AMBIENT_SCROLL", "");
        if (sc.isNotEmpty()) juce::MessageManager::callAsync([this, y = sc.getIntValue()] { viewport_.setViewPosition(0, y); });
        // AMBIENT_TAB=<row>,<page>[;<row>,<page>...]: open tabbed rows on a given page, so a page
        // that is not the first one can be photographed at all. Rows count from the top, pages
        // from the left, both from zero.
        for (const auto& one : juce::StringArray::fromTokens(juce::SystemStats::getEnvironmentVariable("AMBIENT_TAB", ""), ";", "")) {
            const auto rc = juce::StringArray::fromTokens(one, ",", "");
            const int r = rc.size() == 2 ? rc[0].getIntValue() : -1, pg = rc.size() == 2 ? rc[1].getIntValue() : -1;
            if (r >= 0 && r < static_cast<int>(tabRows_.size()) && pg >= 0 && pg < static_cast<int>(tabRows_[static_cast<size_t>(r)].names.size()))
                tabRows_[static_cast<size_t>(r)].active = pg;
        }
        // AMBIENT_MANUAL=<folder>: write the manual out and quit. A chord is played first and
        // then twelve seconds pass, because the pictures are of the running instrument and its
        // displays -- the spectrum, the stage, the note roll -- have nothing in them until it is
        // actually sounding. Waiting alone was not enough: with a slow conductor the first draft
        // illustrated the Sources chapter with a display that said "nothing sounding".
        const juce::String man = juce::SystemStats::getEnvironmentVariable("AMBIENT_MANUAL", "");
        if (man.isNotEmpty()) {
            // The manual's pictures are of the tabbed page. Session recall keeps the layout mode,
            // and one export ran Expanded because the run before it had been.
            applyLayoutMode(0);
            for (int n : { 45, 52, 57, 64, 69 }) proc_.engine().noteOn(n, 0.7f);
            juce::Timer::callAfterDelay(12000, [this, man] {
                exportManual(juce::File(man), [] { juce::JUCEApplicationBase::quit(); });
            });
        }
        // AMBIENT_WAVETABLE=<file>: a table file into the user slot, for a picture of a table the
        // built-ins cannot show -- they have four or five frames, a Serum bank has up to 256.
        {
            const juce::String wtf = juce::SystemStats::getEnvironmentVariable("AMBIENT_WAVETABLE", "");
            if (wtf.isNotEmpty()) proc_.loadWavetableFile(juce::File(wtf));
        }
        // AMBIENT_SET=key=value;key=value and AMBIENT_MATRIX=<routes>: parameters and routes for a
        // run, on top of the preset -- so a picture can show a state no preset has.
        {
            for (const auto& kv : juce::StringArray::fromTokens(juce::SystemStats::getEnvironmentVariable("AMBIENT_SET", ""), ";", "")) {
                const int eq = kv.indexOfChar('=');
                if (eq <= 0) continue;
                if (auto* p = proc_.apvts.getParameter(kv.substring(0, eq).trim())) {
                    if (auto* d = ambient::findParam(kv.substring(0, eq).trim().toRawUTF8()))
                        p->setValueNotifyingHost(p->convertTo0to1(ambient::paramValueFromText(*d, kv.substring(eq + 1).trim().toRawUTF8())));
                }
            }
            const juce::String mx = juce::SystemStats::getEnvironmentVariable("AMBIENT_MATRIX", "");
            if (mx.isNotEmpty()) proc_.engine().setModMatrixText(mx.toRawUTF8());
        }
        // AMBIENT_SHOT=<file.png>: a picture of the whole editor as it stands -- the layout mode,
        // the open tabs, the live displays -- ten seconds after a chord, then quit. The dev aid for
        // looking at the panel without a screen grab, which takes whatever else is on the screen.
        const juce::String shot = juce::SystemStats::getEnvironmentVariable("AMBIENT_SHOT", "");
        if (shot.isNotEmpty() && man.isEmpty()) {
            for (int n : { 45, 52, 57, 64, 69 }) proc_.engine().noteOn(n, 0.7f);
            juce::Timer::callAfterDelay(10000, [this, shot] {
                const juce::Image img = createComponentSnapshot(getLocalBounds(), true, 1.0f);
                juce::File f(shot);
                f.deleteFile();
                juce::PNGImageFormat png;
                if (auto out = std::unique_ptr<juce::FileOutputStream>(f.createOutputStream())) png.writeImageToStream(img, *out);
                juce::JUCEApplicationBase::quit();
            });
        }
        // AMBIENT_MOD=lfo|env|srcenv|matrix: which tab of the modulation strip to open (srcenv: the
        // envelope tab's SOURCES page). Only a dev aid, and the only way to photograph the editor.
        {
            const juce::String md = juce::SystemStats::getEnvironmentVariable("AMBIENT_MOD", "");
            if (mod_ != nullptr && md.isNotEmpty()) {
                mod_->setTab(md == "matrix" ? 2 : ((md == "env" || md == "srcenv") ? 1 : 0));
                if (md == "srcenv") mod_->setEnvPage(1);
            }
        }
        const juce::String rt = juce::SystemStats::getEnvironmentVariable("AMBIENT_ROUTE", "");
        for (int r = 0; rt.isNotEmpty() && r < numRoutePresets(); ++r) if (rt == routePreset(r).name) proc_.setRouteText(routePreset(r).points);
        rebuildLayout();
    }
    startTimerHz(12);
}

NoctuaryEditor::~NoctuaryEditor()
{
    setLookAndFeel(nullptr);
}

// ---------------------------------------------------------------- cells

void NoctuaryEditor::buildCells()
{
    for (const ParamDesc& d : paramTable()) {
        if (d.id == ParamId::MasterGain) continue;   // header knob
        Section* sec = findSection(d.section);
        if (sec == nullptr) {
            Section s; s.name = d.section;
            // The widths in Expanded's flows, where a section's shape is its own: mostly one long
            // row, so the closed sections stand beside the open ones on one line.
            if (s.name == "Tuning") s.maxUnits = 9;
            if (s.name == "Cosmos") s.maxUnits = 8;                          // two even rows
            if (s.name == "Morph") s.maxUnits = 9;
            if (s.name == "Macros") s.maxUnits = 10;                         // one row: the eight macros, Air, Inertia
            if (s.name == "Space") s.maxUnits = 10;                          // two rows of ten
            if (s.name == "Foundation") s.maxUnits = 5;
            if (s.name == "Early Room") s.maxUnits = 4;                      // one row
            if (s.name == "Source 1") s.maxUnits = 9;
            else if (s.name == "Source 2" || s.name == "Source 3" || s.name == "Source 4") s.maxUnits = 12;
            if (s.name == "Strands") s.maxUnits = 10;
            if (s.name == "Delay" || s.name == "Delay 2") s.maxUnits = 12;   // one row with the two Sync choices and Absorb
            if (s.name == "Far Reverb") s.maxUnits = 12;                     // one row with Rotate, Unmask, Diffuse and Width
            if (s.name == "Master") s.maxUnits = 8;                          // the header's row
            if (s.name == "Body") s.maxUnits = 7;                            // one row
            if (s.name == "Room") s.maxUnits = 9;                            // one row with Morph and both impulses
            if (s.name == "Cluster Brain" || s.name == "Brain 2") s.maxUnits = 10;
            if (s.name == "Autoplay") s.maxUnits = 12;   // the seven controls and the button in one row
            if (s.name == "Expression") s.maxUnits = 7;
            if (s.name == "Filter") s.maxUnits = 10;                         // one row: On, Model, five knobs, Drive, Fold
            if (s.name == "Cloud") s.maxUnits = 9;                           // two rows: the grains, then the loop and the resonators
            if (s.name == "Memory") s.maxUnits = 9;                          // two rows: the lines, then the recall and the tape
            if (s.name == "Z-Plane") s.maxUnits = 11;
            s.flowUnits = s.maxUnits;
            // The natural widths on the tabbed page. There they decide one thing only: how wide
            // each column is, which is the widest page at these widths (Source 1 with the strand
            // bank and its picture on the left, the cluster brain with its roll on the right).
            // Every page is then fitted into that column (layoutTabbed), so a section's rows on
            // the screen are whatever its page needs, not these.
            if (s.name == "Cosmos") s.maxUnits = 9;
            if (s.name == "Morph") s.maxUnits = 5;
            if (s.name == "Macros") s.maxUnits = 5;
            if (s.name == "Vector") s.maxUnits = 3;
            if (s.name == "Strands") s.maxUnits = 6;                         // two rows beside Source 1
            if (s.name == "Delay" || s.name == "Delay 2") s.maxUnits = 7;    // two rows, the second with Duck
            if (s.name == "Far Reverb") s.maxUnits = 9;                      // two rows
            if (s.name == "Body") s.maxUnits = 4;
            if (s.name == "Room") s.maxUnits = 6;
            if (s.name == "Cloud") s.maxUnits = 4;
            if (s.name == "Memory") s.maxUnits = 9;
            if (s.name == "Clock") s.maxUnits = 2;
            for (int gi = 0; gi < static_cast<int>(groups_.size()); ++gi)
                for (auto& row : groups_[static_cast<size_t>(gi)].rows)
                    for (auto& n : row) if (n == s.name) s.group = gi;
            sections_.push_back(std::move(s));
            sec = &sections_.back();
        }
        Cell c;
        c.param = static_cast<int>(d.id);
        c.baseLabel = d.name;
        c.label = std::make_unique<juce::Label>(juce::String(), d.name);
        c.label->setJustificationType(juce::Justification::centred);
        c.label->setFont(juce::FontOptions(11.0f));
        c.label->setColour(juce::Label::textColourId, kDim);
        content_.addAndMakeVisible(*c.label);
        switch (d.kind) {
        case ParamKind::Float:
        case ParamKind::Int: {
            auto s = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
            if (d.unit[0] != 0) s->setTextValueSuffix(juce::String(" ") + d.unit);
            // A parameter that spans zero reads much better as an arc growing out of the centre.
            if (d.min < -1.0e-6f && d.max > 1.0e-6f) s->getProperties().set("bipolar", true);
            content_.addAndMakeVisible(*s);
            c.slider = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(proc_.apvts, d.key, *s);
            undoHook_.names[s.get()] = d.name; s->addMouseListener(&undoHook_, false);
            c.comp = std::move(s);
            break;
        }
        case ParamKind::Bool: {
            auto b = std::make_unique<juce::ToggleButton>();
            content_.addAndMakeVisible(*b);
            c.button = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(proc_.apvts, d.key, *b);
            undoHook_.names[b.get()] = d.name; b->addMouseListener(&undoHook_, false);
            c.comp = std::move(b);
            break;
        }
        case ParamKind::Choice: {
            auto cb = std::make_unique<juce::ComboBox>();
            if (d.id == ParamId::ZShape) {
                // A hundred and fifty-five filters in one flat list is a list nobody reads. The
                // item IDs stay the parameter's own numbering -- which is historical and frozen,
                // because the library names its shapes by text -- while the order they are shown
                // in is by family, so the display and the value are free of each other.
                for (int cat = 0; cat < ambient::kZCategories; ++cat) {
                    cb->addSectionHeading(ambient::kZCategoryNames[cat]);
                    for (int i = 0; i < d.numChoices; ++i)
                        if (ambient::kZShapeCategory[i] == cat) cb->addItem(d.choices[i], i + 1);
                }
            } else {
                for (int i = 0; i < d.numChoices; ++i) cb->addItem(d.choices[i], i + 1);
            }
            content_.addAndMakeVisible(*cb);
            c.combo = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(proc_.apvts, d.key, *cb);
            undoHook_.names[cb.get()] = d.name; cb->addMouseListener(&undoHook_, false);
            c.comp = std::move(cb);
            c.units = 2;
            break;
        }
        }
        c.comp->addMouseListener(this, false);
        registerHelp(c.comp.get(), d.id);
        if (auto* tc = dynamic_cast<juce::SettableTooltipClient*>(c.comp.get())) tc->setTooltip(ambient::paramHelp(d.id));
        cellOf_[c.comp.get()] = static_cast<int>(cells_.size());
        sec->cells.push_back(static_cast<int>(cells_.size()));
        cells_.push_back(std::move(c));
    }

    // Extra cells: Scala loader, source files, morph slot pickers and capture buttons.
    auto scala = std::make_unique<juce::TextButton>("Load Scala...");
    scala->onClick = [this] { chooseScalaFile(); };
    addExtraCell("Tuning", std::move(scala), "User scale", 2);
    auto table = std::make_unique<juce::TextButton>("Wavetable...");
    table->onClick = [this] { chooseSourceFile(true); };
    tableCell_ = addExtraCell("Source 2", std::move(table), "User table", 2);
    // A clip loader in every source section, each for its own slot: four slots typed Texture
    // can play four different recordings, which is what the Vector's four corners are for.
    for (int k = 0; k < ambient::kSlots; ++k) {
        auto texture = std::make_unique<juce::TextButton>("Texture...");
        texture->onClick = [this, k] { chooseSourceFile(false, k); };
        textureCell_[k] = addExtraCell("Source " + juce::String(k + 1), std::move(texture), "Texture file", 2);
    }
    // Autoplay's trigger as a button of its own: the parameter is a switch a host can automate,
    // but by hand you want one press, not a switch you have to put back.
    auto step = std::make_unique<juce::TextButton>("Step now");
    step->setTooltip("Exchange one voice of the cluster now, whatever the interval says");
    step->onClick = [this] { proc_.engine().autoplayStep(); };
    addExtraCell("Autoplay", std::move(step), "by hand", 2);
    // The three section banks, each inside the section it belongs to rather than on the header:
    // 257 cosmos presets, 156 filters and 41 plucks are lists you go looking for, and the header
    // keeps only the Sound box, which is the one preset that is about the whole instrument.
    {
        auto cb = std::make_unique<juce::ComboBox>();
        cb->setTextWhenNothingSelected("Cosmos preset");
        cb->setTooltip("257 presets for the Cosmos section in sixteen families. Touches nothing outside it, so it lands on top of whatever sound is loaded.");
        for (int fam = 0; fam < numCosmosPresetFamilies(); ++fam) {
            cb->addSectionHeading(cosmosPresetFamily(fam));
            for (int i = 0; i < numCosmosPresets(); ++i)
                if (cosmosPresetCategory(i) == fam) cb->addItem(cosmosPreset(i).name, i + 1);
        }
        cb->addSectionHeading("Off");
        cb->addItem(cosmosPreset(0).name, 1);
        cb->setSelectedId(proc_.cosmosPresetIndex() + 1, juce::dontSendNotification);
        cb->onChange = [this] {
            const int idx = cosmosBox_->getSelectedId() - 1;
            if (idx >= 0 && idx != proc_.cosmosPresetIndex()) { pushUndo("cosmos preset"); proc_.applyCosmosPreset(idx); }
        };
        cosmosBox_ = cb.get();
        addExtraCell("Cosmos", std::move(cb), "Preset", 3);
    }
    // The Z-plane and Strike banks. Each is a layer like the Cosmos one -- it resets its own
    // section and nothing else -- and each sits inside the section it belongs to, grouped by
    // family, because a flat list of a hundred and fifty-six filters is a list nobody reads.
    {
        auto zb = std::make_unique<juce::ComboBox>();
        zb->setTextWhenNothingSelected("Filter preset");
        for (int cat = 0; cat < ambient::kZCategories; ++cat) {
            zb->addSectionHeading(ambient::kZCategoryNames[cat]);
            for (int i = 0; i < numZPresets(); ++i)
                if (zPresetCategory(i) == cat) zb->addItem(zPreset(i).name, i + 1);
        }
        zb->addSectionHeading("Off");
        zb->addItem(zPreset(0).name, 1);
        zb->setTooltip("One preset per filter shape: the shape, where its point sits in the cube, how sharp and how much of it you hear. Touches nothing outside the Z-Plane section.");
        zb->onChange = [this] {
            const int idx = zPresetBox_->getSelectedId() - 1;
            if (idx >= 0) { pushUndo("filter preset"); proc_.applyZPreset(idx); }
        };
        zPresetBox_ = zb.get();
        addExtraCell("Z-Plane", std::move(zb), "Preset", 3);
    }
    {
        auto sb = std::make_unique<juce::ComboBox>();
        sb->setTextWhenNothingSelected("Strike preset");
        for (int fam = 0; fam < numStrikePresetFamilies(); ++fam) {
            sb->addSectionHeading(strikePresetFamily(fam));
            for (int i = 0; i < numStrikePresets(); ++i)
                if (strikePresetCategory(i) == fam) sb->addItem(strikePreset(i).name, i + 1);
        }
        sb->addSectionHeading("Off");
        sb->addItem(strikePreset(0).name, 1);
        sb->setTooltip("The Karplus-Strong pluck on its own: what is struck, how long it rings, how dull, and whether the conductor fires it too. Touches nothing outside the Strike section.");
        sb->onChange = [this] {
            const int idx = strikePresetBox_->getSelectedId() - 1;
            if (idx >= 0) { pushUndo("strike preset"); proc_.applyStrikePreset(idx); }
        };
        strikePresetBox_ = sb.get();
        addExtraCell("Strike", std::move(sb), "Preset", 3);
    }
    {
        // The near layer's bank: a foreground for the night, kept while the backgrounds change.
        auto nb = std::make_unique<juce::ComboBox>();
        nb->setTextWhenNothingSelected("Near preset");
        for (int fam = 0; fam < numNearPresetFamilies(); ++fam) {
            nb->addSectionHeading(nearPresetFamily(fam));
            for (int i = 0; i < numNearPresets(); ++i)
                if (nearPresetCategory(i) == fam) nb->addItem(nearPreset(i).name, i + 1);
        }
        nb->addSectionHeading("Off");
        nb->addItem(nearPreset(0).name, 1);
        nb->setTooltip("The foreground on its own: what is played close to the ear, how it is shaped, and how often. A layer like the Cosmos -- it stays while sound presets change under it -- and it touches nothing outside the two Near sections.");
        nb->onChange = [this] {
            const int idx = nearPresetBox_->getSelectedId() - 1;
            if (idx >= 0) {
                pushUndo("near preset");
                // A foreground chosen by hand is pinned: Auto off, so the next sound preset does not
                // bring its own over it. Auto on again draws for the preset that is playing.
                proc_.setNearAuto(false);
                autoSeen_ = false;
                proc_.applyNearPreset(idx);
            }
        };
        nearPresetBox_ = nb.get();
        addExtraCell("Near Events", std::move(nb), "Preset", 3);
        // The near source's own clip: what its Clip, Texture, Stretch and Spectral types read.
        auto clip = std::make_unique<juce::TextButton>("Clip...");
        clip->setTooltip("A recording for the near source -- a near preset brings its own from the library's archive (a single one, or a folder of phrases, one of them at random per event); this opens any file. With none, the near source reads Source 4's clip.");
        clip->onClick = [this] { chooseNearClipFile(); };
        nearClipCell_ = addExtraCell("Near Source", std::move(clip), "Source 4's clip", 2);
    }
    auto impulse = std::make_unique<juce::TextButton>("Impulse A...");
    impulse->onClick = [this] { chooseImpulseFile(false); };
    impulseCell_ = addExtraCell("Room", std::move(impulse), "Dark Hall (built in)", 2);
    auto impulseB = std::make_unique<juce::TextButton>("Impulse B...");
    impulseB->onClick = [this] { chooseImpulseFile(true); };
    impulseBCell_ = addExtraCell("Room", std::move(impulseB), "no second room", 2);
    for (auto& s : sections_) s.wideUnits = s.maxUnits;   // the natural width, which every layout starts from

    auto boxA = std::make_unique<juce::ComboBox>();
    boxA->setTextWhenNothingSelected("A: preset");
    fillPresetBox(*boxA);
    morphABox_ = boxA.get();
    boxA->onChange = [this] { if (morphABox_->getSelectedId() > 0) proc_.setMorphSlotFromPreset(0, morphABox_->getSelectedId() - 1); };
    addExtraCell("Morph", std::move(boxA), "A", 2);
    auto boxB = std::make_unique<juce::ComboBox>();
    boxB->setTextWhenNothingSelected("B: preset");
    fillPresetBox(*boxB);
    morphBBox_ = boxB.get();
    boxB->onChange = [this] { if (morphBBox_->getSelectedId() > 0) proc_.setMorphSlotFromPreset(1, morphBBox_->getSelectedId() - 1); };
    addExtraCell("Morph", std::move(boxB), "B", 2);
    auto setA = std::make_unique<juce::TextButton>("A <- now");
    setA->onClick = [this] { proc_.setMorphSlotFromCurrent(0); morphABox_->setSelectedId(0, juce::dontSendNotification); };
    addExtraCell("Morph", std::move(setA), "capture", 1);
    auto setB = std::make_unique<juce::TextButton>("B <- now");
    setB->onClick = [this] { proc_.setMorphSlotFromCurrent(1); morphBBox_->setSelectedId(0, juce::dontSendNotification); };
    addExtraCell("Morph", std::move(setB), "capture", 1);
    {
        // Journeys (13.09.2026): presets in a row with dwell and fade ranges, cyclic for an evening.
        // The templates lie beside the library, the player's own in Documents/Noctuary/Journeys.
        auto jb = std::make_unique<juce::ComboBox>();
        jb->setTextWhenNothingSelected("Journey");
        jb->setTooltip("A journey: presets in a row, each held for a while drawn from its range and crossfaded into the next over a drawn fade, round and round when it is cyclic. Choosing one starts it. The files are plain text (*.journey): one preset a line with its dwell and fade, editable by hand.");
        journeyBox_ = jb.get();
        fillJourneyBox();
        jb->onChange = [this] {
            const int id = journeyBox_->getSelectedId();
            if (id <= 0 || id > journeyFiles_.size()) return;
            if (!proc_.startJourney(journeyFiles_[id - 1]))
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Journey", "This journey could not be read (a malformed line, or no steps).");
        };
        addExtraCell("Morph", std::move(jb), "Journey", 3);
        auto stop = std::make_unique<juce::TextButton>("Stop");
        stop->setTooltip("Stops the journey; the preset playing stays.");
        stop->onClick = [this] { proc_.stopJourney(); journeyBox_->setSelectedId(0, juce::dontSendNotification); };
        addExtraCell("Morph", std::move(stop), "journey", 1);
        auto add = std::make_unique<juce::TextButton>("+ now");
        add->setTooltip("Appends the sound preset that is playing to the journey being written (5-10 minutes, a fade of 30-90 seconds; edit the file for other ranges). Save... writes it.");
        add->onClick = [this] { proc_.journeyAddCurrent(300.0, 600.0, 30.0, 90.0); };
        addExtraCell("Morph", std::move(add), "write", 1);
        auto save = std::make_unique<juce::TextButton>("Save...");
        save->setTooltip("Writes the journey being written (the presets added with + now) into Documents/Noctuary/Journeys under a name, and offers it in the box.");
        save->onClick = [this] { saveJourneyAs(); };
        addExtraCell("Morph", std::move(save), "write", 1);
        auto status = std::make_unique<juce::Label>();
        status->setJustificationType(juce::Justification::centredLeft);
        status->setMinimumHorizontalScale(0.7f);
        journeyStatus_ = status.get();
        addExtraCell("Morph", std::move(status), "step", 2);
    }
}

void NoctuaryEditor::fillJourneyBox()
{
    if (journeyBox_ == nullptr) return;
    journeyFiles_ = NoctuaryProcessor::journeyFiles();
    journeyBox_->clear(juce::dontSendNotification);
    for (int i = 0; i < journeyFiles_.size(); ++i) journeyBox_->addItem(journeyFiles_[i].getFileNameWithoutExtension(), i + 1);
}

void NoctuaryEditor::saveJourneyAs()
{
    auto* w = new juce::AlertWindow("Save journey", "A name for the journey being written (" + juce::String(static_cast<int>(proc_.journey().steps.size())) + " steps):", juce::MessageBoxIconType::NoIcon);
    w->addTextEditor("name", proc_.journey().name.empty() ? "My Journey" : juce::String(proc_.journey().name), "Name");
    w->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([this, w](int result) {
        std::unique_ptr<juce::AlertWindow> owner(w);
        if (result != 1) return;
        const juce::String name = juce::File::createLegalFileName(w->getTextEditorContents("name").trim());
        if (name.isEmpty()) return;
        const juce::File file = NoctuaryProcessor::userJourneyFolder().getChildFile(name + ".journey");
        if (!proc_.saveJourney(file))
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Journey", "Nothing to save: add presets with + now first.");
        else fillJourneyBox();
    }), false);
}

/**
 * Which parameter sits under a screen point: the drop target for a dragged modulation source.
 */
int NoctuaryEditor::cellParamAt(juce::Point<int> screenPos) const
{
    for (const auto& c : cells_) {
        if (c.param < 0 || c.comp == nullptr || !c.comp->isShowing()) continue;
        if (c.comp->getScreenBounds().contains(screenPos)) return c.param;
    }
    return -1;
}

juce::Rectangle<int> NoctuaryEditor::cellScreenBounds(int cellIndex) const
{
    if (cellIndex < 0 || cellIndex >= static_cast<int>(cells_.size())) return {};
    const Cell& c = cells_[static_cast<size_t>(cellIndex)];
    return c.comp != nullptr ? c.comp->getScreenBounds() : juce::Rectangle<int>();
}

void NoctuaryEditor::colourCellsByGroup()
{
    for (const auto& sec : sections_) {
        const juce::Colour col = sec.group >= 0 ? groups_[static_cast<size_t>(sec.group)].colour : kMaster;
        for (int ci : sec.cells) {
            Cell& c = cells_[static_cast<size_t>(ci)];
            if (auto* sl = dynamic_cast<juce::Slider*>(c.comp.get())) sl->setColour(juce::Slider::rotarySliderFillColourId, col);
            else if (auto* tb = dynamic_cast<juce::ToggleButton*>(c.comp.get())) tb->setColour(juce::ToggleButton::tickColourId, col);
        }
    }
}

int NoctuaryEditor::addExtraCell(const juce::String& section, std::unique_ptr<juce::Component> comp, const juce::String& label, int units)
{
    Section* sec = findSection(section);
    if (sec == nullptr) return -1;
    Cell c;
    c.units = units;
    c.baseLabel = label;
    c.label = std::make_unique<juce::Label>(juce::String(), label);
    c.label->setJustificationType(juce::Justification::centred);
    c.label->setFont(juce::FontOptions(11.0f));
    c.label->setColour(juce::Label::textColourId, kDim);
    content_.addAndMakeVisible(*c.label);
    content_.addAndMakeVisible(*comp);
    c.comp = std::move(comp);
    const int idx = static_cast<int>(cells_.size());
    sec->cells.push_back(idx);
    cells_.push_back(std::move(c));
    return idx;
}

NoctuaryEditor::Section* NoctuaryEditor::findSection(const juce::String& name)
{
    for (auto& s : sections_) if (s.name == name) return &s;
    return nullptr;
}

// ---------------------------------------------------------------- layout

namespace {
/** @brief The switches that close a section, and the cells it keeps while closed. */
struct Closer {
    const char* section;                  ///< the section's name
    ambient::ParamId sw;                  ///< the switch: a type of Off, a level at zero, an Active that is not
    std::vector<ambient::ParamId> keep;   ///< the cells still shown while it is closed
};
/**
 * @brief The closer table, built on first use.
 * @return one entry per section that closes; a section not in it is always open
 */
const std::vector<Closer>& closers()
{
    using ambient::ParamId;
    static const std::vector<Closer> k = {
        { "Source 1",   ParamId::Src1Type,     { ParamId::Src1Type, ParamId::OscLevel } },
        { "Source 2",   ParamId::Src2Type,     { ParamId::Src2Type, ParamId::Src2Level } },
        { "Source 3",   ParamId::Src3Type,     { ParamId::Src3Type, ParamId::Src3Level } },
        { "Source 4",   ParamId::Src4Type,     { ParamId::Src4Type, ParamId::Src4Level } },
        { "Z-Plane",    ParamId::ZMode,        { ParamId::ZMode } },
        { "Cosmos",     ParamId::CosmosSend,   { ParamId::CosmosSend, ParamId::CosmosReturn } },
        { "Memory",     ParamId::MemSend,      { ParamId::MemSend, ParamId::MemReturn } },
        { "Strike",     ParamId::StrikeLevel,  { ParamId::StrikeLevel, ParamId::StrikeType } },
        // No entry for Morph any more (13.09.2026). A closed section keeps only its switch, and the
        // Journey row lives in this one -- so the way to a journey was to switch Morph on, which held
        // the whole instrument at the snapshots. The section stays open; it fits in every layout.
        { "Brain 2",    ParamId::Brain2On,     { ParamId::Brain2On } },
        { "Early Room", ParamId::EarlyLevel,   { ParamId::EarlyLevel } },
        { "Body",       ParamId::BodyLevel,    { ParamId::BodyLevel } },
        { "Room",       ParamId::RoomLevel,    { ParamId::RoomLevel } },
        { "Cloud",      ParamId::CloudSend,    { ParamId::CloudSend } },
        { "Feedback",   ParamId::FeedbackBus,  { ParamId::FeedbackBus, ParamId::FeedbackFm } },
        { "Patina",     ParamId::PatinaAmount, { ParamId::PatinaAmount } },
    };
    return k;
}
/**
 * @brief The closer of a section.
 * @param section  the section's name
 * @return its entry, or null if the section never closes
 */
const Closer* closerFor(const juce::String& section)
{
    for (const auto& c : closers()) if (section == c.section) return &c;
    return nullptr;
}
}

bool NoctuaryEditor::sectionCollapsed(const Section& s) const
{
    if (openAll_) return false;
    const Closer* c = closerFor(s.name);
    if (c == nullptr) return false;
    auto off = [this](ParamId id) {
        const ParamDesc& d = paramDesc(id);
        const float v = proc_.engine().getParam(id);
        if (d.kind == ParamKind::Choice) return std::lround(v) == 0;
        if (d.kind == ParamKind::Bool) return v < 0.5f;
        return v <= d.min + 1.0e-6f;
    };
    if (!off(c->sw)) return false;
    if (s.name == "Feedback" && !off(ParamId::FeedbackFm)) return false;   // either path keeps the loop open
    return true;
}

bool NoctuaryEditor::cellShown(const Section& s, const Cell& c) const
{
    if (c.unused) return false;
    if (!s.closedNow()) return true;
    const Closer* k = closerFor(s.name);
    if (k == nullptr || c.param < 0) return false;
    for (ParamId id : k->keep) if (static_cast<int>(id) == c.param) return true;
    return false;
}

int NoctuaryEditor::shownCells(const Section& s) const
{
    int n = 0;
    for (int ci : s.cells) if (cellShown(s, cells_[static_cast<size_t>(ci)])) ++n;
    return n;
}

int NoctuaryEditor::sectionWidth(const Section& s) const
{
    int widest = 0, row = 0;
    for (int ci : s.cells) {
        if (!cellShown(s, cells_[static_cast<size_t>(ci)])) continue;
        const int u = cells_[static_cast<size_t>(ci)].units;
        if (row + u > s.maxUnits) row = 0;
        row += u; widest = std::max(widest, row);
    }
    return widest * kCellW + 2 * kPad;
}

int NoctuaryEditor::sectionHeight(const Section& s) const
{
    int rows = 1, row = 0;
    for (int ci : s.cells) {
        if (!cellShown(s, cells_[static_cast<size_t>(ci)])) continue;
        const int u = cells_[static_cast<size_t>(ci)].units;
        if (row + u > s.maxUnits) { ++rows; row = 0; }
        row += u;
    }
    return rows * kCellH + kTitleH + kPad;
}

void NoctuaryEditor::layoutSection(Section& s, int x, int y)
{
    s.bounds = { x, y, sectionWidth(s), sectionHeight(s) };
    int cx = x + kPad, cy = y + kTitleH, row = 0;
    for (int ci : s.cells) {
        Cell& c = cells_[static_cast<size_t>(ci)];
        if (!cellShown(s, c)) { c.comp->setVisible(false); c.label->setVisible(false); continue; }
        if (row + c.units > s.maxUnits) { row = 0; cx = x + kPad; cy += kCellH; }
        juce::Rectangle<int> cell(cx, cy, c.units * kCellW, kCellH);
        c.label->setBounds(cell.removeFromBottom(15));
        if (dynamic_cast<juce::ComboBox*>(c.comp.get()) != nullptr) c.comp->setBounds(cell.withSizeKeepingCentre(cell.getWidth() - 6, 24));
        else if (dynamic_cast<juce::ToggleButton*>(c.comp.get()) != nullptr) c.comp->setBounds(cell.withSizeKeepingCentre(24, 24));
        else if (dynamic_cast<juce::TextButton*>(c.comp.get()) != nullptr) c.comp->setBounds(cell.withSizeKeepingCentre(cell.getWidth() - 6, 24));
        else c.comp->setBounds(cell.reduced(2, 0));
        cx += c.units * kCellW; row += c.units;
    }
}

void NoctuaryEditor::resized()
{
    // Measure the body first: the design width is whatever it needs, so the right-hand column can
    // never fall off the edge, and the scale follows from that.
    layoutBody();
    designW_ = std::max(kMinDesignW, bodyW_);
    designH_ = std::max(kMinDesignH, kHeaderH + bodyH_ + kStripH);
    scale_ = juce::jmax(0.05f, static_cast<float>(getWidth()) / static_cast<float>(designW_));
    const auto tf = juce::AffineTransform::scale(scale_);
    // Every child is laid out in design coordinates and scaled to the window -- except the corner
    // resizer, which is JUCE's own and which JUCE places at the window's bottom right in window
    // coordinates (AudioProcessorEditor::editorResized). Scaling that one too moved it to
    // (width - 18) * scale: a stray drag handle floating in the middle of the panel, at half the
    // way across whenever the window showed the layout at half size. It stays untransformed.
    for (auto* child : getChildren())
        if (dynamic_cast<juce::ResizableCornerComponent*>(child) == nullptr)
            child->setTransform(tf);

    header_ = juce::Rectangle<int>(0, 0, designW_, kHeaderH);
    // The Sound box gets the room the Cosmos box used to take: it holds the longest names, and
    // with the packs loaded it holds five thousand of them.
    if (soundBox_) soundBox_->setBounds(200, 8, 260, 24);
    if (saveButton_) saveButton_->setBounds(474, 8, 64, 24);
    if (loadButton_) loadButton_->setBounds(544, 8, 64, 24);
    if (recButton_) recButton_->setBounds(614, 8, 56, 24);
    // The pages first (Main, Perform, Browse), then what acts on the state, Help last where a
    // manual belongs. Calibrate and Gestures live in the VR menu and take no room here.
    if (mainButton_) mainButton_->setBounds(880, 8, 56, 24);
    if (performButton_) performButton_->setBounds(942, 8, 70, 24);
    if (browseButton_) browseButton_->setBounds(1018, 8, 66, 24);
    if (vrButton_) vrButton_->setBounds(1090, 8, 44, 24);
    if (undoButton_) undoButton_->setBounds(1140, 8, 50, 24);
    if (redoButton_) redoButton_->setBounds(1194, 8, 50, 24);
    if (abButton_) abButton_->setBounds(1248, 8, 52, 24);
    if (compactButton_) compactButton_->setBounds(1304, 8, 70, 24);
    if (recallButton_) recallButton_->setBounds(1378, 8, 62, 24);
    if (helpButton_) helpButton_->setBounds(1446, 8, 56, 24);
    if (aboutButton_) aboutButton_->setBounds(1506, 8, 24, 24);

    const int W = designW_, H = designH_;
    if (perform_) perform_->setBounds(0, kHeaderH, W, H - kHeaderH);
    if (browse_) browse_->setBounds(0, kHeaderH, W, H - kHeaderH);
    if (help_) help_->setBounds(0, kHeaderH, W, H - kHeaderH);
    // The modulation strip sits along the bottom of the main page, the way Pigments puts its
    // modulator lane there: the sources are always in sight, and a route is a drag away.
    const int stripH = kStripH;
    if (mod_) { mod_->setBounds(0, H - stripH, W, stripH); mod_->toFront(false); }
    helpLine_ = { 12, 40, W / 2 - 30, kHeaderH - 62 };
    keys_ = { W / 2, 44, juce::jmax(160, W - W / 2 - 360), 24 };
    if (outputView_) outputView_->setBounds(W / 2, 74, juce::jmax(160, W - W / 2 - 360), kHeaderH - 92);
    if (master_) master_->setBounds(W - 74, 6, 66, 52);

    // The Master section (mid/side) sits in the header, left of the master knob.
    if (Section* ms = findSection("Master")) {
        layoutSection(*ms, W - 74 - sectionWidth(*ms) - 6, 4);
        ms->bounds = ms->bounds.withTrimmedTop(-2);
    }

    // Everything else scrolls below the header.
    viewport_.setBounds(0, kHeaderH, W, H - kHeaderH - stripH);
    // The drag overlay covers everything and catches nothing: it is only ever drawn on.
    dragOverlay_.setBounds(0, 0, W, H);
    dragOverlay_.toFront(false);

    content_.setSize(std::max(bodyW_, viewport_.getMaximumVisibleWidth()),
                     std::max(bodyH_, viewport_.getMaximumVisibleHeight()));
}

/**
 * Places every group and section, and records how much room the whole body needs. The design
 * size is measured from this once, in the constructor -- guessing it meant the right-hand column
 * kept falling off the edge. A tabbed row is as wide and as tall as its widest and tallest page,
 * so switching a tab never moves anything else.
 * Compact: the columns are kCompactFactor as wide and every page is refitted into them. The page
 * loses width and gains height; nothing scrolls either way, and the arrangement is otherwise untouched.
 */
void NoctuaryEditor::applyLayoutMode(int mode)
{
    mode = juce::jlimit(0, 2, mode);
    compact_ = mode == 1;
    expanded_ = mode == 2;
    proc_.setLayoutMode(mode);
    if (compactButton_) compactButton_->setButtonText(mode == 0 ? "Normal" : mode == 1 ? "Compact" : "Expanded");
    rebuildLayout();
}

void NoctuaryEditor::rebuildLayout()
{
    resized();
    // The design's shape has changed, so the window has to follow: keep the width, take the new
    // height from the new ratio, and tell the constrainer about it or the corner would fight it.
    if (auto* con = getConstrainer()) {
        con->setFixedAspectRatio(static_cast<double>(designW_) / static_cast<double>(designH_));
        con->setSizeLimits(designW_ / 3, designH_ / 3, designW_ * 2, designH_ * 2);
    }
    setSize(getWidth(), juce::roundToInt(getWidth() * static_cast<double>(designH_) / static_cast<double>(designW_)));
    content_.repaint();
    repaint();
}

void NoctuaryEditor::layoutBody()
{
    for (auto& s : sections_) {
        s.collapsed = sectionCollapsed(s);
        s.opened = false;   // decided again below, from the room each row turns out to have
        const int natural = expanded_ ? s.flowUnits : s.wideUnits;
        if (natural > 0) s.maxUnits = natural;
    }
    // Which page: the tabbed two-column page, or the Expanded three-column one with no tabs.
    if (!expanded_) { layoutTabbed(); return; }
    std::vector<Group>& G = expandedGroups_;
    for (auto& t : tabRows_) { t.tabs.clear(); t.bar = {}; }
    {   // Displays the tabbed page owns and this one does not use go out of sight.
        std::set<juce::Component*> used;
        for (auto& g : G) for (auto* d : g.displays) if (d != nullptr) used.insert(d);
        for (auto& t : tabRows_) for (auto* d : t.displays) if (d != nullptr && used.count(d) == 0) d->setVisible(false);
        for (auto& g : groups_) for (auto* d : g.displays) if (d != nullptr && used.count(d) == 0) d->setVisible(false);
    }
    int nCols = 1;
    for (auto& g : G) nCols = std::max(nCols, g.column + 1);
    std::vector<int> colWidth(static_cast<size_t>(nCols), 0), colX(static_cast<size_t>(nCols), 0), colY(static_cast<size_t>(nCols), kPad);
    auto pageWidth = [this](const std::vector<juce::String>& names) {
        int w = kPad;
        for (auto& n : names) if (Section* s = findSection(n)) w += sectionWidth(*s) + kPad;
        return w;
    };
    auto pageHeight = [this](const std::vector<juce::String>& names) {
        int h = 0;
        for (auto& n : names) if (Section* s = findSection(n)) h = std::max(h, sectionHeight(*s));
        return h;
    };
    // A row that wraps: sections left to right, a new line when the next would run past maxW.
    // Measures without placing (place == false) or places (true); returns the width used and,
    // through heightOut, the height of all its lines.
    auto wrapped = [this](const std::vector<juce::String>& names, int x0, int y0, int maxW, bool place, int* heightOut) {
        int x = x0 + kPad, y = y0, lineH = 0, widest = 0;
        for (auto& n : names) {
            Section* s = findSection(n);
            if (s == nullptr) continue;
            // Closed only where closing buys room: if it fits open on the line as it stands, it
            // is laid out open. (Both passes see the same flags, so measuring and placing agree.)
            if (s->closedNow()) {
                const int before = shownCells(*s);
                s->opened = true;
                if (shownCells(*s) <= before || x + sectionWidth(*s) > x0 + maxW) s->opened = false;
            }
            const int w = sectionWidth(*s), h = sectionHeight(*s);
            if (x > x0 + kPad && x + w > x0 + maxW) { x = x0 + kPad; y += lineH + kPad; lineH = 0; }
            if (place) { setSectionVisible(*s, true); layoutSection(*s, x, y); }
            x += w + kPad; lineH = std::max(lineH, h); widest = std::max(widest, x - x0);
        }
        if (heightOut != nullptr) *heightOut = (y - y0) + lineH;
        return widest;
    };
    // The rows of a group as they stand: a row whose only section is closed has no display, and
    // rows without a display that follow one another merge into one flow -- two Off slots share
    // a line instead of each leaving an empty band beside it.
    auto rowsOf = [&](Group& g) {
        std::vector<std::pair<std::vector<juce::String>, juce::Component*>> out;
        for (size_t ri = 0; ri < g.rows.size(); ++ri) {
            juce::Component* d = ri < g.displays.size() ? g.displays[ri] : nullptr;
            const auto& names = g.rows[ri];
            // A row whose only section has no cells (the strand bank while Source 1 is not
            // additive) is not there at all: no title, no display, no line.
            if (names.size() == 1)
                if (Section* only = findSection(names[0]); only != nullptr && shownCells(*only) == 0) {
                    setSectionVisible(*only, false);
                    if (d != nullptr) d->setVisible(false);
                    continue;
                }
            if (d != nullptr && names.size() == 1)
                if (const Section* only = findSection(names[0]); only != nullptr && only->closedNow()) { d->setVisible(false); d = nullptr; }
            if (d == nullptr && !names.empty() && !out.empty() && out.back().second == nullptr && !out.back().first.empty())
                out.back().first.insert(out.back().first.end(), names.begin(), names.end());
            else out.push_back({ names, d });
        }
        return out;
    };
    for (size_t gi = 0; gi < G.size(); ++gi) {
        auto& g = G[gi];
        const auto R = rowsOf(g);
        for (size_t ri = 0; ri < R.size(); ++ri) {
            // A page with a display asks for room to draw it in, or the picture would be squeezed out.
            const bool hasDisp = R[ri].second != nullptr;
            const int w = hasDisp ? pageWidth(R[ri].first) + kDisplayMinW : wrapped(R[ri].first, 0, 0, kWrapW, false, nullptr) + kPad;
            colWidth[static_cast<size_t>(g.column)] = std::max(colWidth[static_cast<size_t>(g.column)], w);
        }
    }
    colX[0] = kPad;
    for (size_t ci = 1; ci < colX.size(); ++ci) colX[ci] = colX[ci - 1] + colWidth[ci - 1] + kPad;
    // The stretchy row of each column, filled in as it is placed and grown at the end of the pass.
    struct Stretch { juce::Component* disp = nullptr; Group* group = nullptr; };
    std::vector<Stretch> stretch(static_cast<size_t>(nCols));
    for (size_t gi = 0; gi < G.size(); ++gi) {
        auto& g = G[gi];
        const size_t col = static_cast<size_t>(g.column);
        const int x0 = colX[col];
        int y = colY[col] + kGroupTitleH;
        const auto R = rowsOf(g);
        for (size_t ri = 0; ri < R.size(); ++ri) {
            const std::vector<juce::String>& names = R[ri].first;
            juce::Component* disp = R[ri].second;
            int rowH = std::max(pageHeight(names), g.minRowH);
            int x = x0 + kPad;
            if (disp == nullptr) {
                // No display: the row wraps, and is as tall as its lines.
                int h = 0;
                x += wrapped(names, x0, y, kWrapW, true, &h);
                rowH = std::max(h, g.minRowH);
            } else {
                for (auto& n : names) {
                    Section* s = findSection(n);
                    if (s == nullptr) continue;
                    setSectionVisible(*s, true);
                    layoutSection(*s, x, y);
                    x += s->bounds.getWidth() + kPad;
                }
                // Whatever the row leaves free goes to its display.
                const int right = x0 + colWidth[col] - kPad;
                if (right - x >= 120 && rowH > 0) { disp->setBounds(x, y, right - x, rowH); disp->setVisible(true); }
                else disp->setVisible(false);
                if (g.stretch && disp->isVisible()) { stretch[col].disp = disp; stretch[col].group = &g; }
            }
            y += rowH + kPad;
        }
        g.bounds = { x0, colY[col], colWidth[col], y - colY[col] };
        colY[col] = y + kPad;
    }
    bodyW_ = colX.back() + colWidth.back() + kPad;
    bodyH_ = 0;
    for (int yy : colY) bodyH_ = std::max(bodyH_, yy);
    // The stretch. A stretchy group is the last one in its column, so growing its row only reaches
    // downwards and nothing else has to move: the columns end level.
    for (size_t ci = 0; ci < stretch.size(); ++ci) {
        if (stretch[ci].disp == nullptr || stretch[ci].group == nullptr) continue;
        const int deficit = bodyH_ - colY[ci];
        if (deficit <= 0) continue;
        stretch[ci].disp->setBounds(stretch[ci].disp->getBounds().withHeight(stretch[ci].disp->getHeight() + deficit));
        stretch[ci].group->bounds = stretch[ci].group->bounds.withHeight(stretch[ci].group->bounds.getHeight() + deficit);
        colY[ci] += deficit;
    }
}

/**
 * The tabbed page: Normal and Compact. See the header for the rule; the pieces are these.
 */
void NoctuaryEditor::layoutTabbed()
{
    for (auto& g : expandedGroups_) for (auto* d : g.displays) if (d != nullptr) d->setVisible(false);
    std::vector<Group>& G = groups_;
    int nCols = 1;
    for (auto& g : G) nCols = std::max(nCols, g.column + 1);
    const size_t NC = static_cast<size_t>(nCols);
    std::vector<int> colWidth(NC, 0), colX(NC, 0), colY(NC, kPad), colYMax(NC, kPad);

    // A page: the sections that stand side by side, and the display that takes the rest.
    struct Page { std::vector<juce::String> names; juce::Component* disp = nullptr; };
    auto pagesOf = [&](size_t gi, size_t ri) {
        std::vector<Page> out;
        Group& g = G[gi];
        if (TabRow* t = tabRowFor(static_cast<int>(gi), static_cast<int>(ri))) {
            for (size_t pi = 0; pi < t->pages.size(); ++pi)
                out.push_back({ t->pages[pi], pi < t->displays.size() ? t->displays[pi] : nullptr });
        } else out.push_back({ g.rows[ri], ri < g.displays.size() ? g.displays[ri] : nullptr });
        return out;
    };
    // The sections of a page that have anything to show. The strand bank has no cells while
    // Source 1 is not additive, and then it is not on the page at all.
    auto live = [&](const std::vector<juce::String>& names) {
        std::vector<Section*> out;
        for (auto& n : names)
            if (Section* s = findSection(n)) {
                if (shownCells(*s) > 0) out.push_back(s); else setSectionVisible(*s, false);
            }
        return out;
    };
    // A section's width and its rows if it were w cells wide; the widest cell it holds.
    auto unitOf  = [&](const Section& s) { int u = 1; for (int ci : s.cells) if (cellShown(s, cells_[static_cast<size_t>(ci)])) u = std::max(u, cells_[static_cast<size_t>(ci)].units); return u; };
    auto widthAt = [&](Section& s, int w) { const int keep = s.maxUnits; s.maxUnits = w; const int r = sectionWidth(s); s.maxUnits = keep; return r; };
    auto rowsAt  = [&](Section& s, int w) { const int keep = s.maxUnits; s.maxUnits = w; const int h = sectionHeight(s); s.maxUnits = keep; return (h - kTitleH - kPad) / kCellH; };
    auto pageH   = [&](const std::vector<Section*>& secs) { int h = 0; for (auto* s : secs) h = std::max(h, sectionHeight(*s)); return h; };
    // A page's width at the natural widths, and at the narrowest (every section one cell wide).
    auto naturalW = [&](const Page& p) { int w = kPad; for (auto* s : live(p.names)) w += widthAt(*s, s->wideUnits) + kPad; return w + (p.disp != nullptr ? kDisplayMinW : 0); };
    auto narrowW  = [&](const Page& p) { int w = kPad; for (auto* s : live(p.names)) w += widthAt(*s, unitOf(*s)) + kPad; return w + (p.disp != nullptr ? kDisplayMinW : 0); };
    // The widths of a page's sections at r rows each: the narrowest at which none needs more.
    // Returns the width they take side by side, with the pads.
    auto widthsAt = [&](const std::vector<Section*>& secs, int r, std::vector<int>& ws) {
        int total = kPad; ws.clear();
        for (Section* s : secs) {
            int w = unitOf(*s);
            while (w < 64 && rowsAt(*s, w) > r) ++w;
            ws.push_back(w); total += widthAt(*s, w) + kPad;
        }
        return total;
    };
    // The fit: the fewest rows at which the page fits its column, its sections each as narrow as
    // that allows. Where the row is taller than the page needs (the stretch), as many rows as the
    // height holds instead, so the sections stand tall and the display takes the width.
    auto fit = [&](const Page& p, int availW, int availH) {
        auto secs = live(p.names);
        const int room = availW - (p.disp != nullptr ? kDisplayMinW : 0);
        std::vector<int> ws;
        int r = 1;
        for (; r < 12; ++r) if (widthsAt(secs, r, ws) <= room) break;
        int used = widthsAt(secs, r, ws);
        // Closed only where closing buys room. A section whose switch is off shrinks to its title
        // to save the page room; where the page has the room anyway, it is laid out open instead,
        // because a title beside an empty band is worse than the section it is hiding.
        for (Section* s : secs) {
            if (!s->closedNow()) continue;
            const int before = shownCells(*s);
            s->opened = true;
            std::vector<int> ws2;
            const int t = widthsAt(secs, r, ws2);
            if (shownCells(*s) > before && t <= room) { ws = ws2; used = t; }
            else { s->opened = false; used = widthsAt(secs, r, ws); }
        }
        // What the row has over is handed out a cell at a time, round robin, to the sections that
        // can take it without losing a row: a last row that is not full reads as a section, an
        // empty band at the end of the row reads as a hole. A display takes it instead.
        if (p.disp == nullptr) {
            bool any = true;
            while (any && used + kCellW <= room) {
                any = false;
                for (size_t i = 0; i < secs.size() && used + kCellW <= room; ++i) {
                    const int w2 = ws[i] + 1;
                    if (rowsAt(*secs[i], w2) != rowsAt(*secs[i], ws[i])) continue;
                    const int grow = widthAt(*secs[i], w2) - widthAt(*secs[i], ws[i]);
                    if (grow <= 0 || used + grow > room) continue;
                    ws[i] = w2; used += grow; any = true;
                }
            }
        }
        for (size_t i = 0; i < secs.size(); ++i) secs[i]->maxUnits = ws[i];
        if (p.disp != nullptr && availH > 0) {
            for (int rr = r + 1; rr < 12; ++rr) {
                std::vector<int> ws2;
                if (widthsAt(secs, rr, ws2) > room) break;
                bool taller = false, fits = true;
                for (size_t i = 0; i < secs.size(); ++i) {
                    const int rows2 = rowsAt(*secs[i], ws2[i]);
                    if (rows2 * kCellH + kTitleH + kPad > availH) fits = false;
                    if (rows2 > rowsAt(*secs[i], ws[i])) taller = true;
                }
                if (!fits) break;
                if (taller) { ws = ws2; for (size_t i = 0; i < secs.size(); ++i) secs[i]->maxUnits = ws[i]; }
            }
        }
        return pageH(secs);
    };
    // Column widths: the widest page at its natural width. Compact takes a fraction of that,
    // never less than the narrowest page needs.
    std::vector<int> narrow(NC, 0);
    for (size_t gi = 0; gi < G.size(); ++gi)
        for (size_t ri = 0; ri < G[gi].rows.size(); ++ri)
            for (const Page& p : pagesOf(gi, ri)) {
                const size_t col = static_cast<size_t>(G[gi].column);
                colWidth[col] = std::max(colWidth[col], naturalW(p));
                narrow[col] = std::max(narrow[col], narrowW(p));
            }
    if (compact_) for (size_t c = 0; c < NC; ++c) colWidth[c] = std::max(narrow[c], juce::roundToInt(colWidth[c] * kCompactFactor));
    colX[0] = kPad;
    for (size_t ci = 1; ci < colX.size(); ++ci) colX[ci] = colX[ci - 1] + colWidth[ci - 1] + kPad;

    const juce::Font tabFont = ui::title(10.5f);
    // The stretchy row of each column: placed here, refitted and grown once the deficit is known.
    struct Stretch { Group* group = nullptr; Page page; int x0 = 0, y = 0, rowH = 0; bool set = false; };
    std::vector<Stretch> stretch(NC);
    for (size_t gi = 0; gi < G.size(); ++gi) {
        auto& g = G[gi];
        const size_t col = static_cast<size_t>(g.column);
        const int x0 = colX[col];
        int y = colY[col] + kGroupTitleH, yMax = colYMax[col] + kGroupTitleH;
        for (size_t ri = 0; ri < g.rows.size(); ++ri) {
            const auto pages = pagesOf(gi, ri);
            TabRow* t = tabRowFor(static_cast<int>(gi), static_cast<int>(ri));
            // Every page fitted, so the tallest is known -- the design height counts that one.
            int rowHMax = g.minRowH;
            for (const Page& p : pages) rowHMax = std::max(rowHMax, fit(p, colWidth[col], 0));
            const size_t act = t != nullptr ? static_cast<size_t>(juce::jlimit(0, static_cast<int>(pages.size()) - 1, t->active)) : 0;
            const Page& p = pages[act];
            if (t != nullptr) {
                t->bar = { x0 + kPad, y, colWidth[col] - 2 * kPad, kTabH };
                t->tabs.clear();
                int tx = t->bar.getX();
                for (auto& nm : t->names) {
                    const int tw = juce::GlyphArrangement::getStringWidthInt(tabFont, nm) + 30;
                    t->tabs.push_back({ tx, y, tw, kTabH - 4 });
                    tx += tw + 4;
                }
                for (size_t pi = 0; pi < pages.size(); ++pi) {
                    if (pi == act) continue;
                    for (auto& n : pages[pi].names) if (Section* s = findSection(n)) setSectionVisible(*s, false);
                    if (pages[pi].disp != nullptr && pages[pi].disp != p.disp) pages[pi].disp->setVisible(false);
                }
                y += kTabH; yMax += kTabH;
            }
            auto secs = live(p.names);
            const int rowH = std::max(pageH(secs), g.minRowH);
            int x = x0 + kPad;
            for (Section* s : secs) { setSectionVisible(*s, true); layoutSection(*s, x, y); x += s->bounds.getWidth() + kPad; }
            if (p.disp != nullptr) {
                const int right = x0 + colWidth[col] - kPad;
                if (right - x >= 120 && rowH > 0) { p.disp->setBounds(x, y, right - x, rowH); p.disp->setVisible(true); }
                else p.disp->setVisible(false);
                if (g.stretch && p.disp->isVisible()) stretch[col] = { &g, p, x0, y, rowH, true };
            }
            y += rowH + kPad; yMax += rowHMax + kPad;
        }
        g.bounds = { x0, colY[col], colWidth[col], y - colY[col] };
        colY[col] = y + kPad; colYMax[col] = yMax + kPad;
    }
    bodyW_ = colX.back() + colWidth.back() + kPad;
    bodyH_ = 0;
    for (int yy : colYMax) bodyH_ = std::max(bodyH_, yy);   // the tallest pages: the shape a tab click cannot change
    // The stretch: the last row of each column grows to the body's height. Its page is refitted
    // for the new height -- the sections as tall as it holds, the display beside them as wide as
    // that leaves -- so the room under the knobs is not a hole but more picture.
    for (size_t ci = 0; ci < stretch.size(); ++ci) {
        Stretch& st = stretch[ci];
        if (!st.set) continue;
        const int deficit = bodyH_ - colY[ci];
        if (deficit <= 0) continue;
        const int rowH = st.rowH + deficit;
        fit(st.page, colWidth[ci], rowH);
        auto secs = live(st.page.names);
        int x = st.x0 + kPad;
        for (Section* s : secs) { setSectionVisible(*s, true); layoutSection(*s, x, st.y); x += s->bounds.getWidth() + kPad; }
        const int right = st.x0 + colWidth[ci] - kPad;
        st.page.disp->setBounds(x, st.y, std::max(120, right - x), rowH);
        st.group->bounds = st.group->bounds.withHeight(st.group->bounds.getHeight() + deficit);
        colY[ci] += deficit;
    }
}

NoctuaryEditor::TabRow* NoctuaryEditor::tabRowFor(int group, int row)
{
    for (auto& t : tabRows_) if (t.group == group && t.row == row) return &t;
    return nullptr;
}

void NoctuaryEditor::setSectionVisible(Section& s, bool v)
{
    s.visible = v;
    for (int ci : s.cells) {
        Cell& c = cells_[static_cast<size_t>(ci)];
        const bool show = v && cellShown(s, c);
        if (c.comp) c.comp->setVisible(show);
        if (c.label) c.label->setVisible(show);
    }
}

/**
 * The layout's inputs from the parameters: which cells a slot's type uses, which sections are
 * closed. Called from the timer; when anything moved, the page is laid out again -- on the
 * player's own action (a type chosen, a switch thrown), never on the window's.
 */
bool NoctuaryEditor::refreshLayoutState()
{
    bool changed = false;
    for (auto& s : sections_) {
        const bool c = sectionCollapsed(s);
        if (c != s.collapsed) { s.collapsed = c; changed = true; }
    }
    return changed;
}

void NoctuaryEditor::clickTabs(juce::Point<int> pos)
{
    if (expanded_) return;   // every page is open; the titles are titles, not tabs
    for (auto& t : tabRows_)
        for (size_t i = 0; i < t.tabs.size(); ++i)
            if (t.tabs[i].contains(pos) && static_cast<int>(i) != t.active) {
                t.active = static_cast<int>(i);
                layoutBody();
                content_.repaint();
                return;
            }
}

// ---------------------------------------------------------------- perform page

void NoctuaryEditor::setPerforming(bool on) { setPage(on ? 1 : 0); }

void NoctuaryEditor::setPage(int page)
{
    performing_ = page == 1;
    viewport_.setVisible(page == 0);
    perform_->setVisible(page == 1);
    browse_->setVisible(page == 2);
    if (help_) help_->setVisible(page == 3);
    if (mod_) mod_->setVisible(page == 0);
    if (mainButton_ && mainButton_->getToggleState() != (page == 0)) mainButton_->setToggleState(page == 0, juce::dontSendNotification);
    if (performButton_->getToggleState() != (page == 1)) performButton_->setToggleState(page == 1, juce::dontSendNotification);
    if (browseButton_->getToggleState() != (page == 2)) browseButton_->setToggleState(page == 2, juce::dontSendNotification);
    if (helpButton_ && helpButton_->getToggleState() != (page == 3)) helpButton_->setToggleState(page == 3, juce::dontSendNotification);

    if (page == 2) browse_->applyFilter();

}


// ---------------------------------------------------------------- modulation strip

// ---------------------------------------------------------------- interaction

void NoctuaryEditor::mouseDown(const juce::MouseEvent& e)
{
    if (!e.mods.isPopupMenu()) return;
    auto it = cellOf_.find(e.eventComponent);
    if (it == cellOf_.end()) return;
    const int idx = it->second;
    const int param = cells_[static_cast<size_t>(idx)].param;
    if (param < 0) return;
    const ParamId id = static_cast<ParamId>(param);
    const int cc = proc_.midiCcFor(id);
    juce::PopupMenu menu;
    menu.addItem(1, proc_.learnTarget() == param ? "Learning... move a controller" : "MIDI Learn", proc_.learnTarget() != param);
    if (cc >= 0) menu.addItem(2, "Clear MIDI (CC " + juce::String(cc) + ")");
    {   // what modulates this knob, each route removable -- the other side of the cards' menu
        const ambient::ModMatrix& m = proc_.engine().modMatrix();
        bool any = false;
        for (int k = 0; k < m.count(); ++k) {
            if (static_cast<int>(m.route(k).target) != param) continue;
            if (!any) { menu.addSeparator(); menu.addSectionHeader("Modulated by"); any = true; }
            menu.addItem(1000 + k, juce::String(ambient::modSourceName(m.route(k).source)) + "   " + juce::String(m.route(k).depth, 2) + "   (remove)");
        }
        if (!any) { menu.addSeparator(); menu.addItem(3, "not modulated -- drag a card from the strip onto the knob", false, false); }
    }
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(e.eventComponent), [this, id](int result) {
        if (result == 1) proc_.armMidiLearn(id);
        else if (result == 2) proc_.clearMidiLearn(id);
        else if (result >= 1000) {
            char buf[4096];
            const int len = proc_.engine().writeModMatrix(buf, sizeof(buf));
            juce::StringArray rows = juce::StringArray::fromTokens(juce::String(juce::CharPointer_UTF8(buf), static_cast<size_t>(juce::jmax(0, len))), ";", "");
            rows.remove(result - 1000);
            proc_.engine().setModMatrixText(rows.joinIntoString(";").toRawUTF8());
            repaint();
        }
    });
}

void NoctuaryEditor::registerHelp(juce::Component* c, ambient::ParamId id)
{
    if (c == nullptr) return;
    helpParamOf_[c] = static_cast<int>(id);
    c->addMouseListener(this, false);
}

void NoctuaryEditor::mouseEnter(const juce::MouseEvent& e)
{
    auto it = helpParamOf_.find(e.eventComponent);
    const int param = it != helpParamOf_.end() ? it->second : -1;
    if (param != hoveredParam_) { hoveredParam_ = param; repaint(); }
}

void NoctuaryEditor::mouseExit(const juce::MouseEvent& e)
{
    auto it = helpParamOf_.find(e.eventComponent);
    if (it != helpParamOf_.end() && it->second == hoveredParam_) { hoveredParam_ = -1; repaint(); }
}

// ---------------------------------------------------------------- undo, redo, A/B, the die

NoctuaryEditor::Snapshot NoctuaryEditor::takeSnapshot(const juce::String& what) const
{
    Snapshot s;
    s.what = what;
    s.v.resize(static_cast<size_t>(kNumParams));
    for (int i = 0; i < kNumParams; ++i)
        if (auto* v = proc_.apvts.getRawParameterValue(paramTable()[static_cast<size_t>(i)].key)) s.v[static_cast<size_t>(i)] = v->load();
    return s;
}

void NoctuaryEditor::restore(const Snapshot& s)
{
    if (static_cast<int>(s.v.size()) != kNumParams) return;
    for (int i = 0; i < kNumParams; ++i)
        if (auto* p = proc_.apvts.getParameter(paramTable()[static_cast<size_t>(i)].key))
            p->setValueNotifyingHost(p->convertTo0to1(s.v[static_cast<size_t>(i)]));
    repaint();
}

void NoctuaryEditor::pushUndo(const juce::String& what)
{
    undo_.push_back(takeSnapshot(what));
    if (undo_.size() > 32) undo_.erase(undo_.begin());   // a session's worth, not a history
    redo_.clear();
}

void NoctuaryEditor::doUndo()
{
    if (undo_.empty()) return;
    redo_.push_back(takeSnapshot(undo_.back().what));
    restore(undo_.back());
    undo_.pop_back();
}

void NoctuaryEditor::doRedo()
{
    if (redo_.empty()) return;
    undo_.push_back(takeSnapshot(redo_.back().what));
    restore(redo_.back());
    redo_.pop_back();
}

/**
 * A/B: the first click parks what you have in A and leaves you on B (a copy, so nothing is lost);
 * every click after that swaps the two. This is the comparison a sound gets judged by.
 */
void NoctuaryEditor::swapAB()
{
    const Snapshot now = takeSnapshot("A/B");
    if (slotA_.v.empty()) { slotA_ = now; slotB_ = now; showingB_ = true; }
    else {
        (showingB_ ? slotB_ : slotA_) = now;
        showingB_ = !showingB_;
        restore(showingB_ ? slotB_ : slotA_);
    }
    if (abButton_) abButton_->setButtonText(showingB_ ? "B | a" : "A | b");
}

/**
 * The die on a section: every parameter of that section is drawn again. Choices and switches are
 * picked at random, numbers land inside the middle of their range (the ends of a range are
 * usually where a preset stops being usable), and shift keeps them near where they already are.
 */
void NoctuaryEditor::randomiseSection(const juce::String& name, bool subtle)
{
    Section* sec = findSection(name);
    if (sec == nullptr) return;
    pushUndo("randomise " + name);
    juce::Random rng;
    for (int ci : sec->cells) {
        const Cell& c = cells_[static_cast<size_t>(ci)];
        if (c.param < 0) continue;
        const ParamId id = static_cast<ParamId>(c.param);
        if (isPerformanceParam(id)) continue;
        const ParamDesc& d = paramDesc(id);
        auto* p = proc_.apvts.getParameter(d.key);
        if (p == nullptr) continue;
        float value;
        if (d.kind == ParamKind::Choice || d.kind == ParamKind::Bool || d.kind == ParamKind::Int) {
            const int lo = static_cast<int>(std::lround(d.min)), hi = static_cast<int>(std::lround(d.max));
            const int cur = static_cast<int>(std::lround(proc_.engine().getParam(id)));
            value = static_cast<float>(subtle ? juce::jlimit(lo, hi, cur + rng.nextInt(3) - 1) : lo + rng.nextInt(hi - lo + 1));
        } else {
            // Drawn in the parameter's own skewed domain, so a logarithmic knob is not biased
            // towards its top end -- the same domain the morph and the map blend in.
            const float span = std::max(d.max - d.min, 1e-9f);
            const float cur = std::pow(juce::jlimit(0.0f, 1.0f, (proc_.engine().getParam(id) - d.min) / span), d.skew);
            const float t = subtle ? juce::jlimit(0.0f, 1.0f, cur + 0.2f * (rng.nextFloat() * 2.0f - 1.0f))
                                   : 0.15f + 0.7f * rng.nextFloat();
            value = d.min + span * std::pow(t, 1.0f / d.skew);
        }
        p->setValueNotifyingHost(p->convertTo0to1(value));
    }
    repaint();
}

bool NoctuaryEditor::keyPressed(const juce::KeyPress& k)
{
    if (k == juce::KeyPress::F1Key) { setPage(help_ && help_->isVisible() ? 0 : 3); return true; }
    if (k == juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0)) { doUndo(); return true; }
    if (k == juce::KeyPress('y', juce::ModifierKeys::commandModifier, 0)) { doRedo(); return true; }
    if (k == juce::KeyPress::escapeKey && help_ && help_->isVisible()) { setPage(0); return true; }
    return false;
}

// ---------------------------------------------------------------- help page

juce::Image NoctuaryEditor::snapshotSection(const juce::String& name)
{
    Section* sec = findSection(name);
    if (sec == nullptr) return {};
    // If the section lives on a tab that is not open, open it for the picture and put it back.
    TabRow* row = nullptr; int was = 0;
    for (auto& t : tabRows_)
        for (size_t pi = 0; pi < t.pages.size(); ++pi)
            for (auto& n : t.pages[pi]) if (n == name && static_cast<int>(pi) != t.active) { row = &t; was = t.active; t.active = static_cast<int>(pi); }
    if (row != nullptr) layoutBody();
    // The Master section is painted in the header and its bounds are the editor's; every other
    // section belongs to the scrolling content. Photographed from the wrong component, the
    // Master came out as a strip of the panel's top-left corner.
    juce::Image img = (name == "Master" ? static_cast<juce::Component&>(*this) : static_cast<juce::Component&>(content_))
                          .createComponentSnapshot(sec->bounds.expanded(2), true, 1.0f);
    if (row != nullptr) { row->active = was; layoutBody(); }
    return img;
}

/**
 * One tab of the panel, as it looks when it is open: every section of the page, the tab bar over
 * it, the display beside it and -- on Source 1 -- the strand bank under that display. A picture
 * per section would have been easier and would have shown the manual's reader something that is
 * not on their screen; what they see is a tab.
 */
juce::Image NoctuaryEditor::snapshotTab(int rowIndex, int page)
{
    if (rowIndex < 0 || rowIndex >= static_cast<int>(tabRows_.size())) return {};
    TabRow& t = tabRows_[static_cast<size_t>(rowIndex)];
    if (page < 0 || page >= static_cast<int>(t.pages.size())) return {};
    const int was = t.active;
    const bool wasExpanded = expanded_;   // the manual's pictures are of the page as it is normally: one tab open
    expanded_ = false;
    t.active = page;
    layoutBody();
    juce::Rectangle<int> box = t.bar;                      // the tab bar itself belongs in the picture
    auto add = [&](const juce::String& name) {
        if (name.isEmpty()) return;
        if (Section* s = findSection(name)) box = box.isEmpty() ? s->bounds : box.getUnion(s->bounds);
    };
    for (const auto& n : t.pages[static_cast<size_t>(page)]) add(n);
    if (page < static_cast<int>(t.displays.size()) && t.displays[static_cast<size_t>(page)] != nullptr) {
        juce::Component* d = t.displays[static_cast<size_t>(page)];
        if (d->isVisible() && d->getWidth() > 0) box = box.getUnion(d->getBounds());
    }
    juce::Image img;
    if (!box.isEmpty()) img = content_.createComponentSnapshot(box.expanded(4), true, 1.0f);
    t.active = was;
    expanded_ = wasExpanded;
    layoutBody();
    return img;
}

/**
 * A whole page of the instrument -- Perform, Browse, the modulation strip, the help page itself --
 * rather than one section of it. Pages are siblings of the panel and are shown one at a time, so
 * the one being photographed is made visible for the picture and put back afterwards.
 */
juce::Image NoctuaryEditor::snapshotPage(juce::Component* page)
{
    if (page == nullptr || page->getWidth() <= 0 || page->getHeight() <= 0) return {};
    const bool was = page->isVisible();
    page->setVisible(true);
    juce::Image img = page->createComponentSnapshot(page->getLocalBounds(), true, 1.0f);
    page->setVisible(was);
    return img;
}

juce::Image NoctuaryEditor::snapshotPerform() { return snapshotPage(perform_.get()); }

juce::Image NoctuaryEditor::snapshotBrowseMap()
{
    if (!browse_) return {};
    const bool vis = browse_->isVisible();
    const int was = browse_->modeMap.getToggleState() ? 1 : 0;
    browse_->setVisible(true);
    browse_->setMode(1);
    browse_->resized();
    juce::Image img = browse_->createComponentSnapshot(browse_->getLocalBounds(), true, 1.0f);
    browse_->setMode(was);
    browse_->setVisible(vis);
    return img;
}

/**
 * The map as it looks once it has been zoomed in: the current preset in the middle at eight
 * times, its neighbours around it, and names on everything that has room for one.
 */
juce::Image NoctuaryEditor::snapshotBrowseMapZoomed()
{
    if (!browse_) return {};
    const bool vis = browse_->isVisible();
    const int was = browse_->modeMap.getToggleState() ? 1 : 0;
    browse_->setVisible(true);
    browse_->setMode(1);
    browse_->resized();
    const int cur = proc_.getCurrentProgram();
    float cx = 0.5f, cy = 0.5f;
    if (cur >= 0 && cur < ambient::numPresetMeta()) { cx = ambient::presetMeta(cur).x; cy = ambient::presetMeta(cur).y; }
    browse_->map.zoomTo(cx, cy, 8.0f);
    juce::Image img = browse_->createComponentSnapshot(browse_->getLocalBounds(), true, 1.0f);
    browse_->map.zoomTo(0.5f, 0.5f, 1.0f);
    browse_->setMode(was);
    browse_->setVisible(vis);
    return img;
}

juce::Image NoctuaryEditor::snapshotHeader()
{
    // The master's corner of the header: the mid/side section, the loudness meter and the
    // master knob. The whole header is four thousand pixels wide and unreadable on a page.
    // The meter is hidden while the help page is up, which is when the manual is exported, so
    // its bounds are read whether or not it is visible; without that the picture began in the
    // middle of the meter.
    juce::Rectangle<int> box;
    if (Section* ms = findSection("Master")) box = ms->bounds;
    if (outputView_ != nullptr && !outputView_->getBounds().isEmpty()) box = box.isEmpty() ? outputView_->getBounds() : box.getUnion(outputView_->getBounds());
    if (!keys_.isEmpty()) box = box.isEmpty() ? keys_ : box.getUnion(keys_);   // the note roll over the meter
    if (box.isEmpty()) return {};
    box = box.expanded(8).withRight(getWidth()).withTop(0);
    box.setBottom(juce::jmin(kHeaderH - 2, box.getBottom()));
    return createComponentSnapshot(box, true, 1.0f);
}

juce::Image NoctuaryEditor::snapshotStripTab(int tab, bool detail)
{
    if (!mod_) return {};
    const int was = mod_->tab;
    mod_->setTab(tab);
    juce::Image img;
    if (detail) {
        // The strip is nearly three thousand pixels wide and shrinks to a ribbon on a page; the
        // left third -- the cards and the first panel -- at twice the size is where it can be read.
        const bool vis = mod_->isVisible();
        mod_->setVisible(true);
        img = mod_->createComponentSnapshot(mod_->getLocalBounds().withWidth(mod_->getWidth() * 24 / 70), true, 2.0f);
        mod_->setVisible(vis);
    } else img = snapshotStrip();
    mod_->setTab(was);
    return img;
}

void NoctuaryEditor::openSourceEnvelope(int slot)
{
    // The pictures that call this sit on the main page, where the strip is, so it only needs its
    // page turned. Which source asked is not singled out: the page shows all four side by side.
    juce::ignoreUnused(slot);
    if (!mod_) return;
    mod_->setTab(1);
    mod_->setEnvPage(1);
}

juce::StringArray NoctuaryEditor::tabSectionNames(int rowIndex, int page) const
{
    juce::StringArray out;
    if (rowIndex < 0 || rowIndex >= static_cast<int>(tabRows_.size())) return out;
    const TabRow& t = tabRows_[static_cast<size_t>(rowIndex)];
    if (page < 0 || page >= static_cast<int>(t.pages.size())) return out;
    for (const auto& n : t.pages[static_cast<size_t>(page)]) out.add(n);
    return out;
}

/**
 * What a tab is called on its own bar, so the manual's caption is the word the reader will look
 * for on the screen.
 */
juce::String NoctuaryEditor::tabName(int rowIndex, int page) const
{
    if (rowIndex < 0 || rowIndex >= static_cast<int>(tabRows_.size())) return {};
    const TabRow& t = tabRows_[static_cast<size_t>(rowIndex)];
    if (page < 0 || page >= static_cast<int>(t.names.size())) return {};
    return t.names[static_cast<size_t>(page)];
}

juce::Image NoctuaryEditor::snapshotStrip()
{
    if (!mod_) return {};
    const bool vis = mod_->isVisible();
    mod_->setVisible(true);
    juce::Image img = mod_->createComponentSnapshot(mod_->getLocalBounds(), true, 1.0f);
    mod_->setVisible(vis);
    return img;
}

juce::Image NoctuaryEditor::snapshotBrowse()
{
    if (!browse_) return {};
    const bool vis = browse_->isVisible();
    browse_->setVisible(true);
    juce::Image img = browse_->createComponentSnapshot(browse_->getLocalBounds(), true, 1.0f);
    browse_->setVisible(vis);
    return img;
}

NoctuaryEditor::HelpView::HelpView(NoctuaryProcessor& p, NoctuaryEditor& o)
    : proc(p), owner(o), flow(p)
{
    addChildComponent(flow);
    topics.setModel(this);
    topics.setRowHeight(26);
    topics.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    addAndMakeVisible(topics);
    text.setMultiLine(true, true);
    text.setReadOnly(true);
    text.setCaretVisible(false);
    text.setScrollbarsShown(true);
    text.setFont(ui::body(15.0f));
    text.setColour(juce::TextEditor::backgroundColourId, ui::card.withAlpha(0.5f));
    text.setColour(juce::TextEditor::outlineColourId, ui::cardEdge);
    text.setColour(juce::TextEditor::textColourId, ui::text);
    text.setIndents(14, 12);
    addAndMakeVisible(text);
    // The last topic is generated: every parameter, by section, with its help text.
    juce::String last;
    for (const ParamDesc& d : paramTable()) {
        if (last != d.section) { last = d.section; parameters += (parameters.isEmpty() ? "" : "\n") + juce::String(d.section).toUpperCase() + "\n"; }
        juce::String range;
        if (d.kind == ParamKind::Choice) { for (int i = 0; i < d.numChoices; ++i) range += (i ? " / " : "") + juce::String(d.choices[i]); }
        else if (d.kind == ParamKind::Bool) range = "on / off";
        else range = juce::String(d.min, d.kind == ParamKind::Int ? 0 : 2) + " .. " + juce::String(d.max, d.kind == ParamKind::Int ? 0 : 2) + (d.unit[0] ? juce::String(" ") + d.unit : juce::String());
        parameters += "  " + juce::String(d.name) + "  (" + d.key + ", " + range + ")\n      " + ambient::paramHelp(d.id) + "\n";
    }
    topics.updateContent();
    topics.selectRow(0, false, true);
    selectedRowsChanged(0);
}

void NoctuaryEditor::HelpView::paint(juce::Graphics& g)
{
    g.fillAll(ui::bg0);
    g.setColour(ui::group);
    g.fillRoundedRectangle(getLocalBounds().reduced(10).toFloat(), 8.0f);
    for (size_t i = 0; i < pics.size() && i < picRects.size(); ++i) {
        if (!pics[i].isValid() || picRects[i].isEmpty()) continue;
        g.drawImage(pics[i], picRects[i].toFloat(), juce::RectanglePlacement::stretchToFit);
        g.setColour(ui::cardEdge); g.drawRoundedRectangle(picRects[i].toFloat().reduced(0.5f), 4.0f, 1.0f);
    }
    g.setColour(ui::text);
    g.setFont(ui::title(13.0f));
    g.drawText("MANUAL", 26, 18, 200, 20, juce::Justification::centredLeft, false);
    g.setColour(ui::dim);
    g.setFont(ui::body(11.0f));
    g.drawText("F1 or Help closes it again  --  the pictures are the panel as it stands right now, and the displays are live", 120, 18, getWidth() - 160, 20, juce::Justification::centredLeft, false);
}

void NoctuaryEditor::HelpView::resized()
{
    auto r = getLocalBounds().reduced(20).withTrimmedTop(26);
    topics.setBounds(r.removeFromLeft(230));
    r.removeFromLeft(12);
    // Text on the left at a readable line length, the pictures in the column to its right.
    const int textW = juce::jlimit(360, 820, r.getWidth() * 42 / 100);
    text.setBounds(r.removeFromLeft(textW));
    r.removeFromLeft(16);
    flow.setBounds(r);
    picRects.clear();
    auto col = r;
    for (const auto& img : pics) {
        if (!img.isValid() || col.getHeight() < 60) { picRects.push_back({}); continue; }
        const float scale = juce::jmin(1.0f, static_cast<float>(col.getWidth()) / static_cast<float>(img.getWidth()),
                                       static_cast<float>(juce::jmin(320, col.getHeight() - (live ? 240 : 0))) / static_cast<float>(img.getHeight()));
        const int w = juce::roundToInt(img.getWidth() * scale), h = juce::roundToInt(img.getHeight() * scale);
        picRects.push_back(col.removeFromTop(h).withWidth(w));
        col.removeFromTop(10);
    }
    if (live) live->setBounds(col.removeFromTop(juce::jmin(260, col.getHeight())));
}

// ---------------------------------------------------------------- the manual, as files

/**
 * @brief The manual export in progress: the folder, the steps still to run, and what has been written.
 *
 * The help page is the manual, and its pictures are snapshots of the panel itself -- the real
 * sections with their real values, taken as the page is opened. That is what makes them right
 * and what makes them impossible to produce from a script: they only exist while an editor is
 * running. So the export runs in one, writes every topic's pictures and text into a folder, and
 * Tools/make_manual.py turns that folder into an HTML manual and a PDF.
 *
 * It runs as a list of steps a third of a second apart rather than as one function, because
 * some of the pictures need the instrument to have MOVED between two of them: the gallery of
 * source types sets Source 2 to each type in turn, and its display and its greyed-out knobs
 * follow on the next timer tick, not in the same call.
 */
struct NoctuaryEditor::ManualJob {
    juce::File dir;   ///< the folder everything is written into
    std::vector<std::function<void()>> steps;   ///< the steps, run one per 350 ms by runManualStep
    size_t next = 0;   ///< the step that runs next
    std::function<void()> done;   ///< called after the last step: puts openAll_ back and quits, when asked to
    /**
     * @brief What has been written so far, per topic: the pictures and the tabs with their captions,
     * their blurbs and the sections whose parameters belong under them.
     */
    struct Tab {
        juce::String file,            ///< the PNG's file name in dir
        name,                         ///< the tab's name on its bar
        caption,                      ///< the caption under the picture
        blurb;                        ///< the tab's own text from ambient::tabHelp
        juce::StringArray sections;   ///< the sections whose parameters the manual prints under it
    };
    /** @brief One section picture of a topic. */
    struct Pic {
        juce::String file,   ///< the PNG's file name in dir
        caption;             ///< the caption under it
    };
    std::vector<std::vector<Pic>> images;   ///< per topic: its section pictures
    std::vector<std::vector<Tab>> tabs;   ///< per topic: its tab pictures
    juce::String originalType;    ///< Source 2's type before the gallery, put back afterwards
};

void NoctuaryEditor::exportManual(const juce::File& dir, std::function<void()> onDone)
{
    dir.createDirectory();
    if (help_ == nullptr) { if (onDone) onDone(); return; }
    manual_ = std::make_unique<ManualJob>();
    ManualJob& job = *manual_;
    job.dir = dir;
    // Every section open for its picture, whatever its switch says, and back afterwards.
    openAll_ = true;
    rebuildLayout();
    job.done = [this, cb = std::move(onDone)] { openAll_ = false; rebuildLayout(); if (cb) cb(); };
    const int last = ambient::numHelpTopics();    // the generated "All parameters" topic comes after
    job.images.resize(static_cast<size_t>(last + 1));
    job.tabs.resize(static_cast<size_t>(last + 1));

    auto writePng = [dir](const juce::Image& img, const juce::String& name) -> juce::String {
        if (!img.isValid()) return {};
        const juce::File f = dir.getChildFile(name);
        juce::PNGImageFormat png;
        std::unique_ptr<juce::FileOutputStream> out(f.createOutputStream());
        return (out != nullptr && png.writeImageToStream(img, *out)) ? f.getFileName() : juce::String();
    };
    // Built by concatenation, not by formatted(): JUCE's formatted is wide-character, so a %s
    // handed a const char* writes the bytes as UTF-16 and the file comes out called
    // "topic-00-汦睧.png". It did, once.
    auto stem = [](int row) { return "topic-" + juce::String(row).paddedLeft('0', 2); };

    setPage(3);                                   // the help page, so its views are laid out
    help_->setBounds(0, kHeaderH, designW_, designH_ - kHeaderH);

    // ---- one step per topic: its sections, its live display, its tabs
    for (int row = 0; row <= last; ++row) {
        job.steps.push_back([this, row, stem, writePng, &job] {
            help_->topics.selectRow(row);
            help_->showTopic(row);
            help_->resized();
            auto& files = job.images[static_cast<size_t>(row)];
            // The section pictures the help page shows are the manual's only when the chapter
            // has no tab pictures: where it has, every section is already in one of them.
            for (size_t i = 0; help_->tabPics.empty() && i < help_->pics.size(); ++i) {
                const juce::String f = writePng(help_->pics[i], stem(row) + "-" + juce::String(static_cast<int>(i)) + ".png");
                if (f.isNotEmpty()) files.push_back({ f, i < static_cast<size_t>(help_->picCaptions.size()) ? help_->picCaptions[static_cast<int>(i)] : juce::String() });
            }
            auto shot = [&](juce::Component& c, const juce::String& suffix, const juce::String& caption, juce::Rectangle<int> area = {}) {
                if (c.getWidth() <= 0 || c.getHeight() <= 0) return;
                if (area.isEmpty()) area = c.getLocalBounds();
                const juce::String f = writePng(c.createComponentSnapshot(area, true, 2.0f), stem(row) + "-" + suffix + ".png");
                if (f.isNotEmpty()) files.push_back({ f, caption });
            };
            if (row == 0) shot(help_->flow, "flow", "The signal flow: every unit as a box in its group's colour, the buses as arrows -- the same picture the help page's first topic shows.", help_->flow.drawn());
            if (help_->live != nullptr) shot(*help_->live, "live", help_->liveCaption);
            for (size_t i = 0; i < help_->tabPics.size(); ++i) {
                ManualJob::Tab t;
                t.file = writePng(help_->tabPics[i], stem(row) + "-tab" + juce::String(static_cast<int>(i)) + ".png");
                if (t.file.isEmpty()) continue;
                t.name = i < static_cast<size_t>(help_->tabNames.size()) ? help_->tabNames[static_cast<int>(i)] : juce::String();
                t.caption = i < static_cast<size_t>(help_->tabCaptions.size()) ? help_->tabCaptions[static_cast<int>(i)] : juce::String();
                t.sections = i < help_->tabSections.size() ? help_->tabSections[i] : juce::StringArray();
                t.blurb = juce::CharPointer_UTF8(ambient::tabHelp(t.name.toRawUTF8()));
                job.tabs[static_cast<size_t>(row)].push_back(t);
            }
        });
    }

    // ---- the gallery of source types, on Source 2, one type per pair of steps. The clip for
    // the Texture, Stretch and Spectral types comes from AMBIENT_MANUAL_CLIP; without it those
    // three show an empty display, which is at least honest.
    {
        const int sourcesRow = 1;
        auto* typeParam = proc_.apvts.getParameter("src2_type");
        const juce::String clip = juce::SystemStats::getEnvironmentVariable("AMBIENT_MANUAL_CLIP", "");
        job.steps.push_back([this, typeParam, &job] {
            job.originalType = typeParam != nullptr ? juce::String(typeParam->getValue()) : juce::String();
        });
        for (const char* type : { "Additive", "Harmonic", "Wavetable", "FM", "Texture", "Stretch", "Bow", "Spectral", "Noise" }) {
            const juce::String typeName(type);
            int index = -1;
            for (int i = 0; i < ambient::kNumSourceTypes; ++i) if (typeName == ambient::kSourceTypeNames[i]) index = i;
            if (index < 0 || typeParam == nullptr) continue;
            job.steps.push_back([this, typeParam, index, typeName, clip] {
                if ((typeName == "Texture" || typeName == "Stretch" || typeName == "Spectral") && clip.isNotEmpty())
                    proc_.loadTextureFile(1, juce::File(clip));
                typeParam->setValueNotifyingHost(typeParam->convertTo0to1(static_cast<float>(index)));
                if (auto* lvl = proc_.apvts.getParameter("src2_level")) lvl->setValueNotifyingHost(lvl->convertTo0to1(0.5f));
            });
            job.steps.push_back([this, typeName, sourcesRow, stem, writePng, &job] {
                help_->topics.selectRow(sourcesRow);
                help_->showTopic(sourcesRow);
                ManualJob::Tab t;
                t.name = "TYPE " + typeName;
                t.caption = "Source 2 switched to the " + typeName + " type: the knobs that type uses are lit, the rest greyed out, and the display shows "
                          + (typeName == "Additive" ? juce::String("the partials of its bank.")
                           : typeName == "Harmonic" ? juce::String("the current frame of the table, drawn from its spectrum.")
                           : typeName == "Wavetable" ? juce::String("the current frame as the samples it is, its neighbours faint behind it.")
                           : typeName == "FM" ? juce::String("the modulated waveform of the pair.")
                           : typeName == "Texture" ? juce::String("the loaded clip with the grains reading it.")
                           : typeName == "Stretch" ? juce::String("the loaded clip with the stretched read position crawling through it.")
                           : juce::String("the spectrum of the noise colour."));
                t.file = writePng(snapshotTab(0, 1), stem(sourcesRow) + "-type-" + typeName.toLowerCase() + ".png");
                if (t.file.isEmpty()) return;
                t.sections.add("Source 2");
                t.blurb = juce::CharPointer_UTF8(ambient::tabHelp(t.name.toRawUTF8()));
                job.tabs[static_cast<size_t>(sourcesRow)].push_back(t);
            });
        }
        job.steps.push_back([typeParam, &job] {
            if (typeParam != nullptr && job.originalType.isNotEmpty()) typeParam->setValueNotifyingHost(job.originalType.getFloatValue());
        });
    }

    // ---- the last step: the file that names it all, and the cover pictures
    job.steps.push_back([this, last, dir, writePng, &job] {
        juce::String json = "{\n  \"topics\": [\n";
        for (int row = 0; row <= last; ++row) {
            const juce::String title = row < last ? juce::String(ambient::helpTopicTitle(row)) : "All parameters";
            const juce::String body  = row < last ? juce::String(juce::CharPointer_UTF8(ambient::helpTopicText(row)))
                                                  : help_->parameters;
            json << "    { \"title\": " << juce::JSON::toString(juce::var(title))
                 << ", \"text\": " << juce::JSON::toString(juce::var(body))
                 << ", \"images\": [";
            const auto& files = job.images[static_cast<size_t>(row)];
            for (size_t i = 0; i < files.size(); ++i)
                json << (i ? ", " : "") << "{ \"file\": " << juce::JSON::toString(juce::var(files[i].file))
                     << ", \"caption\": " << juce::JSON::toString(juce::var(files[i].caption)) << " }";
            json << "], \"tabs\": [";
            const auto& tabs = job.tabs[static_cast<size_t>(row)];
            for (size_t i = 0; i < tabs.size(); ++i) {
                json << (i ? ", " : "") << "{ \"file\": " << juce::JSON::toString(juce::var(tabs[i].file))
                     << ", \"name\": " << juce::JSON::toString(juce::var(tabs[i].name))
                     << ", \"caption\": " << juce::JSON::toString(juce::var(tabs[i].caption))
                     << ", \"blurb\": " << juce::JSON::toString(juce::var(tabs[i].blurb))
                     << ", \"sections\": [";
                for (int k = 0; k < tabs[i].sections.size(); ++k) json << (k ? ", " : "") << juce::JSON::toString(juce::var(tabs[i].sections[k]));
                json << "] }";
            }
            json << "] }" << (row < last ? ",\n" : "\n");
        }
        // Every parameter, structured, so the manual can print each tab's own under its picture.
        json << "  ],\n  \"params\": [\n";
        bool first = true;
        for (const ParamDesc& d : paramTable()) {
            juce::String range;
            if (d.kind == ParamKind::Choice) { for (int i = 0; i < d.numChoices; ++i) range += (i ? " / " : "") + juce::String(d.choices[i]); }
            else if (d.kind == ParamKind::Bool) range = "on / off";
            else range = juce::String(d.min, d.kind == ParamKind::Int ? 0 : 2) + " .. " + juce::String(d.max, d.kind == ParamKind::Int ? 0 : 2) + (d.unit[0] ? juce::String(" ") + d.unit : juce::String());
            json << (first ? "" : ",\n") << "    { \"section\": " << juce::JSON::toString(juce::var(juce::String(d.section)))
                 << ", \"name\": " << juce::JSON::toString(juce::var(juce::String(d.name)))
                 << ", \"key\": " << juce::JSON::toString(juce::var(juce::String(d.key)))
                 << ", \"range\": " << juce::JSON::toString(juce::var(range))
                 << ", \"help\": " << juce::JSON::toString(juce::var(juce::String(juce::CharPointer_UTF8(ambient::paramHelp(d.id))))) << " }";
            first = false;
        }
        json << "\n  ],\n  \"version\": " << juce::JSON::toString(juce::var(juce::String(JucePlugin_VersionString)))
             << ",\n  \"shapes\": " << ambient::kZShapes
             << ",\n  \"presets\": " << numPresets()
             << ",\n  \"cosmos\": " << numCosmosPresets()
             << ",\n  \"zpresets\": " << numZPresets()
             << ",\n  \"strike\": " << numStrikePresets() << "\n}\n";
        dir.getChildFile("manual.json").replaceWithText(json);
        // Coverage: every page of every tab row must have been photographed exactly once. Written
        // as a file so make_manual.py can refuse to print a manual with a hole in it.
        juce::String coverage;
        for (int r = 0; r < static_cast<int>(tabRows_.size()); ++r)
            for (int pg = 0; pg < static_cast<int>(tabRows_[static_cast<size_t>(r)].names.size()); ++pg) {
                const juce::String nm = tabRows_[static_cast<size_t>(r)].names[static_cast<size_t>(pg)];
                int seen = 0;
                for (const auto& chapter : job.tabs) for (const auto& t : chapter) if (t.name == nm) ++seen;
                coverage << nm << ": " << seen << "\n";
            }
        dir.getChildFile("coverage.txt").replaceWithText(coverage);

        // One picture of the whole panel, for the cover: the instrument as it actually looks.
        // And one of the header on its own, which is where the pages, the master and the
        // loudness meter live.
        setPage(0);
        resized();
        writePng(createComponentSnapshot(getLocalBounds(), true, 1.0f), "panel.png");
        writePng(createComponentSnapshot(getLocalBounds().withHeight(kHeaderH), true, 2.0f), "header.png");
        if (job.done) job.done();
    });
    runManualStep();
}

void NoctuaryEditor::runManualStep()
{
    if (manual_ == nullptr || manual_->next >= manual_->steps.size()) return;
    manual_->steps[manual_->next++]();
    if (manual_ != nullptr && manual_->next < manual_->steps.size())
        juce::Timer::callAfterDelay(350, [this] { runManualStep(); });
}

void NoctuaryEditor::HelpView::showTopic(int row)
{
    topic = row;
    pics.clear();
    picCaptions.clear();
    tabPics.clear();
    tabNames.clear();
    tabCaptions.clear();
    tabSections.clear();
    live.reset();
    flow.setVisible(row == 0);
    // Which sections and which live display belong to a topic. The order follows kTopics in
    // Help.cpp. `sections` are the pictures the help page itself shows, at most two or three:
    // the column they are stacked in runs out of height after that.
    struct Spec { std::vector<juce::String> sections; int liveKind; };   // liveKind: 0 none, 1 source, 2 filter, 3 stage, 4 cosmos, 5 brain, 6 env
    static const Spec kSpecs[] = {
        { {}, 0 },                                          // overview: the diagram
        { { "Source 1", "Vector" }, 1 },                    // sources
        { { "Filter", "Z-Plane" }, 2 },                     // filters
        { { "Space", "Foundation", "Air" }, 3 },            // space
        { { "Delay", "Far Reverb" }, 0 },                   // effects
        { { "Cosmos", "Memory" }, 4 },                      // cosmos (and the memory beside it)
        { { "Cluster Brain", "Tuning" }, 5 },               // conductor
        { {}, 0 },                                          // modulation: the strip
        { { "Morph", "Macros" }, 6 },                       // morph
        { {}, 0 },                                          // presets: the browser
        { { "Clock" }, 0 },                                 // clock
        { { "Master", "Expression" }, 0 },                  // midi / osc / files (Expression: the MPE dimensions)
        { {}, 0 },                                          // shortcuts
    };
    static const char* const kLiveCaption[7] = {
        "", "The live display of Source 1: the partials of the loudest voice, exactly the amplitudes the oscillator is summing right now.",
        "The live filter response: the voice filter, the z-plane, and what a note actually meets after both, from the same maths the audio path uses.",
        "The live stage: every sounding voice as a dot at its distance from the ear, the far plane at the back.",
        "The live Cosmos display: the shifter, the resonator and the vowel as the section currently has them.",
        "The live conductor: the brain's notes, their holds and where on the planes it has put them.",
        "The live envelope: the six modulation envelopes' shapes and where each one's clock currently is.",
    };
    const int n = static_cast<int>(sizeof(kSpecs) / sizeof(kSpecs[0]));
    // The sections and the live display, for the help page and the manual both.
    if (row >= 0 && row < n) {
        for (const auto& name : kSpecs[row].sections) {
            juce::Image img = owner.snapshotSection(name);
            if (!img.isValid()) continue;
            pics.push_back(img);
            picCaptions.add("The " + name + " section, with the values of the preset the manual was exported from.");
        }
        switch (kSpecs[row].liveKind) {
        case 1: live = std::make_unique<SourceView>(proc, 1); break;
        case 2: live = std::make_unique<FilterView>(proc); break;
        case 3: live = std::make_unique<StageView>(proc); break;
        case 4: live = std::make_unique<CosmosView>(proc); break;
        case 5: live = std::make_unique<BrainView>(proc); break;
        case 6: live = std::make_unique<EnvView>(proc); break;
        default: break;
        }
        liveCaption = kLiveCaption[kSpecs[row].liveKind];
        if (live) addAndMakeVisible(*live);
    }
    // The tabs, for the manual only. Every page of every tab row belongs to exactly one chapter
    // -- by its row, with three exceptions -- and is enumerated from the rows themselves, so a
    // tab that is added to the panel is in the manual the next time it is exported, and one
    // cannot be forgotten by a list written by hand. (One was, twice.)
    auto addTab = [&](juce::Image img, const juce::String& name, const juce::String& caption, juce::StringArray sections) {
        if (!img.isValid()) return;
        tabPics.push_back(img); tabNames.add(name); tabCaptions.add(caption); tabSections.push_back(sections);
    };
    static const int kRowChapter[] = { 1, 2, 4, 4, 5, 6 };    // tab row -> chapter
    for (int r = 0; r < static_cast<int>(owner.tabRows_.size()); ++r) {
        const auto& t = owner.tabRows_[static_cast<size_t>(r)];
        for (int pg = 0; pg < static_cast<int>(t.pages.size()); ++pg) {
            int chapter = r < 6 ? kRowChapter[r] : 4;
            if (r == 1 && pg == 2) chapter = 7;      // ENVELOPE + EXPRESSION with the modulation
            if (chapter != row) continue;
            const juce::StringArray secs = owner.tabSectionNames(r, pg);
            juce::String what = secs.joinIntoString(", ");
            juce::String caption = "The " + t.names[static_cast<size_t>(pg)] + " tab as it opens: the " + what
                                 + (secs.size() > 1 ? " sections" : " section")
                                 + " with the values of the preset the manual was exported from"
                                 + (pg < static_cast<int>(t.displays.size()) && t.displays[static_cast<size_t>(pg)] != nullptr
                                        ? ", and beside them the tab's live display." : ".");
            addTab(owner.snapshotTab(r, pg), t.names[static_cast<size_t>(pg)], caption, secs);
        }
    }
    if (row == 1)   // the strand bank, beside Source 1 on its page
        addTab(owner.snapshotSection("Strands"), "STRANDS",
               "The Strands section, which stands beside Source 1 on its page while the slot is additive: the strand bank's ten controls.", { "Strands" });
    if (row == 3) {
        addTab(owner.snapshotSection("Space"), "SPACE", "The Space section: the spatial model's controls, two rows side by side.", { "Space" });
        addTab(owner.snapshotSection("Foundation"), "FOUNDATION", "The Foundation section: the sub and its source.", { "Foundation" });
    }
    if (row == 4)   // the master, in its corner of the header
        addTab(owner.snapshotHeader(), "MASTER",
               "The right-hand half of the header: the note roll (every sounding note as a bar, the keys' notes lit), the loudness meter under it, the Master section (tilt, bass mono, side air, width, mono safe, subsonic) and the master gain knob.", { "Master" });
    if (row == 7) {
        static const char* const kStrip[3] = { "LFO", "ENV", "MATRIX" };
        static const char* const kStripCap[3] = {
            "The modulation strip along the bottom of the page, LFO tab: the eight LFO cards, each a shape, a rate, a phase, a depth and a mode. A card is dragged onto a knob to route it.",
            "The strip's ENV tab: the six envelope cards and the shape editor, where an envelope is drawn as points on a curve.",
            "The strip's MATRIX tab: every route as a row -- source, target, depth, via, and the 0..1 flag." };
        static const char* const kStripSection[3] = { "LFO 1", "Env 1", "" };
        static const char* const kStripDetail[3] = {
            "The left third of the LFO tab at readable size: the source cards -- LFO 1 to 4 are the first four -- and LFO 1's own panel: Shape, Rate, Phase, Depth, Mode, Sync.",
            "The left third of the ENV tab at readable size: the envelope editor of Env 1 -- points on a curve, dragged; double-click adds or removes one -- with its Mode, Time, Depth and Sync.",
            "The left third of the MATRIX tab at readable size: the first routes, each a source, a target, a depth slider, a Via source and the 0..1 flag, and the '+ route' button." };
        for (int t = 0; t < 3; ++t) {
            juce::StringArray secs; if (kStripSection[t][0]) secs.add(kStripSection[t]);
            addTab(owner.snapshotStripTab(t), kStrip[t], kStripCap[t], {});
            addTab(owner.snapshotStripTab(t, true), juce::String(kStrip[t]) + " (detail)", kStripDetail[t], secs);
        }
    }
    if (row == 8)
        addTab(owner.snapshotPerform(), "PERFORM",
               "The Perform page: the macros large, the morph and the map cursor, the note roll and the stage, and the set recorder.", {});
    if (row == 9) {
        addTab(owner.snapshotBrowse(), "BROWSE",
               "The Browse page in its list view: the columns that narrow the library (family, character, motion, features), the search, and the list of every preset the instrument knows.", {});
        addTab(owner.snapshotBrowseMap(), "MAP",
               "The Browse page in its map view: every preset as a point, clustered by what it sounds like; the cursor and its radius, and the Map blend switch that makes the space between presets playable.", { "Map" });
        addTab(owner.snapshotBrowseMapZoomed(), "MAP, ZOOMED IN",
               "The same map closed in on the current preset at eight times, with the wheel or with \"more like this\": the cluster it sits in opens into single presets, and the ones with room for a name show it. Drag to move about, double-click to see the whole plane again.", { "Map" });
    }
    resized();
    repaint();
}

int NoctuaryEditor::HelpView::getNumRows() { return ambient::numHelpTopics() + 1; }

void NoctuaryEditor::HelpView::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (selected) { g.setColour(ui::accent.withAlpha(0.22f)); g.fillRoundedRectangle(2.0f, 1.0f, static_cast<float>(w - 4), static_cast<float>(h - 2), 4.0f); }
    g.setColour(selected ? ui::text : ui::dim);
    g.setFont(ui::body(12.5f));
    const juce::String title = row < ambient::numHelpTopics() ? ambient::helpTopicTitle(row) : "All parameters";
    g.drawText(title, 10, 0, w - 14, h, juce::Justification::centredLeft, true);
}

void NoctuaryEditor::HelpView::selectedRowsChanged(int row)
{
    if (row < 0) return;
    text.setText(row < ambient::numHelpTopics() ? juce::String(juce::CharPointer_UTF8(ambient::helpTopicText(row))) : parameters, false);
    text.moveCaretToTop(false);
    showTopic(row);
}

juce::Rectangle<int> NoctuaryEditor::HelpView::FlowDiagram::drawn() const
{
    const float sc = juce::jmin(getWidth() / kCanvasW, getHeight() / kCanvasH);
    if (sc <= 0.0f) return getLocalBounds();
    return juce::Rectangle<int>(0, 0, juce::roundToInt(kCanvasW * sc), juce::roundToInt(kCanvasH * sc));
}

/**
 * The signal flow as a picture: the units as boxes in their group colours, the buses as arrows.
 */
void NoctuaryEditor::HelpView::FlowDiagram::paint(juce::Graphics& g)
{
    // Drawn on a fixed canvas, scaled to fit whatever the column offers.
    const float sx = getWidth() / kCanvasW, sy = getHeight() / kCanvasH, sc = juce::jmin(sx, sy);
    if (sc <= 0.05f) return;
    g.addTransform(juce::AffineTransform::scale(sc));
    auto node = [&](float x, float y, float w, float h, const juce::String& t, juce::Colour c, float fs = 12.0f) {
        juce::Rectangle<float> r(x, y, w, h);
        g.setColour(ui::card); g.fillRoundedRectangle(r, 6.0f);
        g.setColour(c.withAlpha(0.9f)); g.drawRoundedRectangle(r.reduced(0.5f), 6.0f, 1.2f);
        g.setColour(ui::text); g.setFont(ui::body(fs));
        g.drawFittedText(t, r.reduced(6.0f, 2.0f).toNearestInt(), juce::Justification::centred, 3, 0.9f);
        return r;
    };
    auto arrow = [&](juce::Point<float> a, juce::Point<float> b, juce::Colour c, bool dashed = false) {
        g.setColour(c.withAlpha(0.85f));
        juce::Line<float> l(a, b);
        if (dashed) { const float d[] = { 5.0f, 4.0f }; juce::Path p; p.startNewSubPath(a); p.lineTo(b); juce::PathStrokeType(1.4f).createDashedStroke(p, p, d, 2); g.fillPath(p); }
        else g.drawLine(l, 1.4f);
        juce::Path head; const auto u = (b - a) / juce::jmax(1.0f, l.getLength()); const juce::Point<float> nrm(-u.y, u.x);
        head.startNewSubPath(b); head.lineTo(b - u * 7.0f + nrm * 3.5f); head.lineTo(b - u * 7.0f - nrm * 3.5f); head.closeSubPath();
        g.fillPath(head);
    };
    auto label = [&](float x, float y, const juce::String& t, juce::Colour c) { g.setColour(c); g.setFont(ui::body(10.5f)); g.drawText(t, juce::roundToInt(x), juce::roundToInt(y), 480, 14, juce::Justification::centredLeft, false); };

    const juce::Colour V = ui::voiceCol, F = ui::foreCol, B = ui::backCol, C = ui::cosmosCol, K = ui::condCol, M = ui::masterCol, A = ui::accent;
    // conductors
    auto brain = node(20, 14, 150, 36, "Cluster Brain", K);
    auto keys  = node(185, 14, 120, 36, "MIDI keys", K);
    auto hands = node(320, 14, 130, 36, "OSC / hands / macros", K, 11.0f);
    label(20, 54, "every note gets a DISTANCE: 0 at the ear, 1 the infinite background", ui::dim);
    // The types, once, where there is room for them: every slot of the voice and the near source
    // alike may be set to any of these, so naming them in five boxes would be the same list five
    // times. The boxes below say "any type" and mean this.
    g.setColour(V.withAlpha(0.30f)); g.drawRoundedRectangle(470.0f, 12.0f, 510.0f, 62.0f, 8.0f, 1.0f);
    g.setColour(V); g.setFont(ui::title(9.5f));
    g.drawText("SOURCE TYPES  --  any slot, any of them", 478, 15, 400, 12, juce::Justification::centredLeft, false);
    g.setColour(ui::dim); g.setFont(ui::body(8.0f));
    g.drawText("additive bank  -  harmonic table (spectra)  -  wavetable (cycles)  -  FM  -  texture grains  -  spectral model  -  stretch  -  bow  -  noise",
               478, 29, 496, 11, juce::Justification::centredLeft, false);
    g.setColour(F); g.setFont(ui::body(8.0f));
    g.drawText("near:  flute  -  murmur  -  bowl  -  ice  -  drops  -  clip", 478, 43, 496, 11, juce::Justification::centredLeft, false);
    g.drawText("signals:  whistler  -  shaker  -  chime  -  geiger  -  tube  -  Krell  -  beacon  -  morse  -  dial",
               478, 57, 496, 11, juce::Justification::centredLeft, false);
    // the voice
    g.setColour(V.withAlpha(0.35f)); g.drawRoundedRectangle(14.0f, 74.0f, 442.0f, 250.0f, 8.0f, 1.0f);
    g.setColour(V); g.setFont(ui::title(10.5f)); g.drawText("VOICE  x16", 24, 78, 200, 14, juce::Justification::centredLeft, false);
    // Four equal source slots. Slot 1 is the strand bank while its type is Additive, and then the
    // Strands section is its own; set to anything else it renders like the other three.
    auto s1 = node(24, 96, 100, 38, "Source 1\nstrand bank / any", V, 9.5f);
    auto s2 = node(130, 96, 100, 38, "Source 2\nany type", V, 9.5f);
    auto s3 = node(236, 96, 100, 38, "Source 3\nany type", V, 9.5f);
    auto s4 = node(342, 96, 92, 38, "Source 4\nany type", V, 9.5f);
    auto vec = node(24, 142, 410, 26, "Vector: the four slots on the corners of one square   -   Strike: a struck string, wood or metal on top", V, 9.5f);
    auto filt = node(24, 176, 200, 36, "Filter (ten models) + fold", V, 11.0f);
    auto zp   = node(234, 176, 200, 36, "Z-plane filter (155 shapes)\nafter the filter, or beside it", V, 9.5f);
    auto env  = node(24, 228, 410, 36, "Envelope  -  x (1 - distance/2)  -  interaural time difference  -  presence on the near plane", V, 9.5f);
    auto air  = node(24, 274, 410, 36, "Air: noise on the note (band) or resonators on its harmonics (ghost)", V, 10.0f);
    arrow(brain.getBottomLeft().translated(75, 0), { 74, 96 }, K);
    arrow(keys.getBottomLeft().translated(60, 0), { 245, 96 }, K);
    arrow(hands.getBottomLeft().translated(65, 0), { 388, 96 }, K, true);
    for (auto* r : { &s1, &s2, &s3, &s4 }) arrow({ r->getCentreX(), r->getBottom() }, { r->getCentreX(), 142.0f }, V);
    arrow({ 124, 168 }, { 124, 176 }, V); arrow({ 334, 168 }, { 334, 176 }, V);
    arrow({ 124, 212 }, { 124, 228 }, V); arrow({ 334, 212 }, { 334, 228 }, V);
    arrow({ 229, 264 }, { 229, 274 }, V);
    // The split, said once and in words: two labels hung on the two arrows sat across them.
    label(150, 332, "splits by distance:  near = cos(d),  far = sin(d)", ui::dim);
    arrow({ 434, 292 }, { 500, 130 }, F);
    arrow({ 434, 300 }, { 500, 356 }, B);
    // The near layer (2.0): a second, small conductor and a source of its own, playing one thing
    // close to the ear while the voice above carries the piece. It is drawn here, under the voice,
    // because it reaches the planes at the same split -- with a distance of its own, a share that
    // goes past every reverb, and two sends. Its own bank of presets, beside the sound presets.
    // The left margin is left clear: the feedback's return runs down it.
    g.setColour(F.withAlpha(0.35f)); g.drawRoundedRectangle(148.0f, 344.0f, 308.0f, 118.0f, 8.0f, 1.0f);
    g.setColour(F); g.setFont(ui::title(10.0f));
    // The label is as long as the box is wide: "presets" spelled out past this runs off the edge.
    g.drawText("NEAR LAYER  --  its own 108 presets", 156, 347, 300, 13, juce::Justification::centredLeft, false);
    auto ne = node(156, 362, 144, 30, "Near Events\na second conductor, waiting", F, 8.5f);
    auto ns = node(308, 362, 142, 30, "Near Source\nany slot may hold one", F, 8.5f);
    // What it may be set to is the legend at the top right; what is its own is the kind of event.
    auto nty = node(156, 396, 294, 32, "Note  -  Phrase (a line that glides)  -  Sequence\n(a Berlin-school shift register that mutates)", F, 8.0f);
    auto nrt = node(156, 432, 294, 24, "Distance / Approach  -  Dry  -  To Delay 2  -  To Cosmos", F, 8.0f);
    arrow({ 300, 377 }, { 308, 377 }, F);
    arrow({ 303, 392 }, { 303, 396 }, F);
    arrow({ 303, 428 }, { 303, 432 }, F);
    arrow({ 452, 420 }, { 494, 312 }, F);   // to the same split the voice's two planes come from
    // near chain
    label(500, 92, "NEAR  --  the dry, bright foreground", F);
    auto ens = node(500, 110, 108, 38, "Ensemble\nchorus / microshift", F, 9.5f);
    auto d1  = node(614, 110, 96, 38, "Delay", F);
    auto d2  = node(716, 110, 96, 38, "Delay 2", F);
    auto nr  = node(818, 110, 148, 38, "Near reverb\n+ the Haas band", F, 9.5f);
    arrow({ 608, 129 }, { 614, 129 }, F); arrow({ 710, 129 }, { 716, 129 }, F); arrow({ 812, 129 }, { 818, 129 }, F);
    auto blur = node(500, 158, 210, 26, "Blur: attacks wiped into texture", F, 9.5f);
    auto cos = node(500, 194, 320, 38, "Cosmos (parallel): frequency shifter -> resonator -> vowel -> nebula", C, 9.5f);
    arrow({ 540, 148 }, { 540, 194 }, C); arrow({ 780, 194 }, { 780, 148 }, C);
    label(500, 236, "send / return -- added, never replacing", ui::dim);
    auto cloud = node(846, 194, 120, 38, "Cloud (grains)", B, 10.5f);
    arrow({ 906, 148 }, { 906, 194 }, B); arrow({ 906, 232 }, { 906, 348 }, B);
    // To the far plane, down the gap between the Cosmos and the Cloud rather than through them.
    arrow({ 834, 148 }, { 834, 348 }, B, true); label(700, 268, "delay sends to far", B);
    // far
    label(500, 326, "FAR  --  the infinite background, 100 % wet, unmasked band by band", B);
    auto fr  = node(500, 348, 150, 38, "Far reverb\n+ its own width", B, 9.5f);
    auto rm  = node(662, 348, 150, 38, "Room (convolution)", B, 10.5f);
    auto sh  = node(824, 348, 142, 38, "Shimmer loop", B);
    arrow({ 650, 367 }, { 662, 367 }, B);
    arrow({ 895, 386 }, { 580, 394 }, B, true);
    // output: the master chain in the order it actually runs
    auto out1 = node(500, 410, 466, 30, "Body (twelve tuned modes)   ->   Mid / Side  (bass mono, side air, width)", M, 9.5f);
    auto out2 = node(500, 446, 466, 30, "+ Foundation sub   ->   Patina   ->   subsonic   ->   Master   ->   soft clip", M, 9.5f);
    arrow({ 575, 386 }, { 575, 410 }, B); arrow({ 737, 386 }, { 737, 410 }, B);
    arrow({ 966, 129 }, { 972, 406 }, F);      // the near bus, down the right-hand margin
    arrow({ 972, 406 }, { 966, 414 }, F);
    arrow({ 733, 440 }, { 733, 446 }, M);
    label(500, 478, "no compressor anywhere: what you hear is the dynamics of the drone", ui::dim);
    // feedback, modulation
    arrow({ 500, 470 }, { 120, 470 }, A, true); arrow({ 120, 470 }, { 120, 312 }, A, true);
    label(130, 474, "Feedback: to bus / to pitch (phase-modulates every partial), tape", A);
    node(20, 500, 946, 34, "Modulation: 8 LFOs  -  6 envelopes  -  aftertouch, wheel, slide  -  the note, its velocity, its distance  -  matrix -> any knob", A, 10.0f);
    juce::ignoreUnused(ens, d1, d2, nr, cos, cloud, fr, rm, sh, out1, out2, env, filt, zp, s4, vec, air, blur, ne, ns, nty, nrt);
}

void NoctuaryEditor::timerCallback()
{
    proc_.engine().soundingNotes(sounding_);
    {   // Auto switched on (the toggle, or the host): the preset that is playing brings its foreground now.
        const bool autoOn = proc_.engine().getParam(ParamId::ForeAuto) >= 0.5f;
        if (autoOn && !autoSeen_) proc_.nearAutoNow();
        autoSeen_ = autoOn;
        // The box says what Auto (or a state) put there, without the change counting as a hand's.
        if (nearPresetBox_ != nullptr && proc_.nearPresetIndex() >= 0 && nearPresetBox_->getSelectedId() != proc_.nearPresetIndex() + 1)
            nearPresetBox_->setSelectedId(proc_.nearPresetIndex() + 1, juce::dontSendNotification);
        if (journeyStatus_ != nullptr) {
            const juce::String s = proc_.journeyRunning() ? proc_.journeyStatus()
                                 : (proc_.journey().steps.empty() ? juce::String() : juce::String(static_cast<int>(proc_.journey().steps.size())) + " steps written");
            if (journeyStatus_->getText() != s) journeyStatus_->setText(s, juce::dontSendNotification);
        }
    }
    {   // Mark what is moving each knob right now. The matrix, in its source's colour, as before;
        // and everything else that plays a value the knob does not show -- the morph between two
        // presets, the map's blend, a route -- as a neutral arc from the knob's value to the live
        // one. The slow processes with a state of their own (the arc, the tide) get a dot on the
        // outer ring that says where in their swing they are: the panel's answer to a modulation
        // too slow for the eye to catch as motion.
        const ambient::ModMatrix& m = proc_.engine().modMatrix();
        for (auto& c : cells_) {
            auto* sl = dynamic_cast<juce::Slider*>(c.comp.get());
            if (sl == nullptr || c.param < 0) continue;
            const ParamId id = static_cast<ParamId>(c.param);
            int colour = 0;
            for (int k = 0; k < m.count(); ++k) {
                if (static_cast<int>(m.route(k).target) != c.param) continue;
                using MS = ambient::ModSource;
                const int si = static_cast<int>(m.route(k).source);
                juce::Colour col = ui::cosmosCol;
                if (si >= static_cast<int>(MS::Lfo1) && si <= static_cast<int>(MS::Lfo8)) col = ui::accent;
                else if (si >= static_cast<int>(MS::Env1) && si <= static_cast<int>(MS::Env6)) col = ui::foreCol;
                else if (si >= static_cast<int>(MS::MacroA) && si <= static_cast<int>(MS::MacroH)) col = ui::morphCol;
                else if (si >= static_cast<int>(MS::Kura1) && si <= static_cast<int>(MS::Kura4)) col = ui::condCol;
                else if (si >= static_cast<int>(MS::Lenia1) && si <= static_cast<int>(MS::Lenia4)) col = ui::backCol;
                colour = static_cast<int>(col.getARGB());
                break;
            }
            const ParamDesc& d = paramDesc(id);
            const float range = juce::jmax(1.0e-6f, d.max - d.min);
            const float raw = proc_.engine().getParam(id);
            const float live = proc_.engine().effectiveParam(id) + proc_.engine().modAmount(id);
            const float off = (live - raw) / range;
            bool changed = false;
            auto setP = [&](const char* key, const juce::var& v) { if (!sl->getProperties().contains(key) || sl->getProperties()[key] != v) { sl->getProperties().set(key, v); changed = true; } };
            auto remP = [&](const char* key) { if (sl->getProperties().contains(key)) { sl->getProperties().remove(key); changed = true; } };
            if (colour != 0) { setP("modColour", colour); setP("modOffset", static_cast<double>(off)); remP("liveOffset"); }
            else {
                remP("modColour"); remP("modOffset");
                if (std::fabs(off) > 1.0e-4f) setP("liveOffset", static_cast<double>(off)); else remP("liveOffset");
            }
            if (id == ParamId::ArcAmount) {
                // Following the clock, the knob wears a dial: the hour on a full circle, with the
                // turning points of the day marked. Drifting, a dot for where the swing stands.
                if (proc_.engine().arcClockOn()) { setP("clockHour", proc_.engine().clockHour()); remP("halo"); }
                else { setP("halo", static_cast<double>(proc_.engine().arcNow())); remP("clockHour"); }
            }
            else if (id == ParamId::Tide) setP("halo", static_cast<double>(juce::jlimit(-1.0f, 1.0f, proc_.engine().tideNow() / juce::jmax(1.0f, raw))));
            if (changed) sl->repaint();
        }
    }
    if (mapOpen_) {
        if (mapEditor_ != nullptr) mapText_ = mapEditor_->getText();
        else {
            mapOpen_ = false;
            if (!proc_.setGestureMappings(mapText_))
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Mappings", "A line could not be parsed; the previous table is kept.");
        }
    }
    if (soundBox_ && soundBox_->getSelectedId() != proc_.soundPresetIndex() + 1)
        soundBox_->setSelectedId(proc_.soundPresetIndex() + 1, juce::dontSendNotification);
    // The three section boxes follow the processor rather than the other way round, so a layer
    // loaded from a program change, from OSC or by a whole state coming back shows up in them --
    // and so does a sound preset clearing them, which it does, because a sound preset brings its
    // own filter and its own pluck and the name of a layer preset would then be a lie.
    if (cosmosBox_ && cosmosBox_->getSelectedId() != proc_.cosmosPresetIndex() + 1)
        cosmosBox_->setSelectedId(proc_.cosmosPresetIndex() + 1, juce::dontSendNotification);
    if (zPresetBox_ && zPresetBox_->getSelectedId() != proc_.zPresetIndex() + 1)
        zPresetBox_->setSelectedId(proc_.zPresetIndex() + 1, juce::dontSendNotification);
    if (strikePresetBox_ && strikePresetBox_->getSelectedId() != proc_.strikePresetIndex() + 1)
        strikePresetBox_->setSelectedId(proc_.strikePresetIndex() + 1, juce::dontSendNotification);
    const int learn = proc_.learnTarget();
    for (auto& c : cells_) {
        if (c.param < 0) continue;
        const int cc = proc_.midiCcFor(static_cast<ParamId>(c.param));
        juce::String text = c.baseLabel;
        if (learn == c.param) text += " - learn";
        else if (cc >= 0) text += " - CC" + juce::String(cc);
        if (c.label->getText() != text) {
            c.label->setText(text, juce::dontSendNotification);
            c.label->setColour(juce::Label::textColourId, (cc >= 0 || learn == c.param) ? kAccent : kDim);
        }
    }
    updateSourceCells();
    repaint(header_);
    if (Section* m = findSection("Morph")) content_.repaint(m->bounds.withTrimmedTop(-kGroupTitleH));
}

int NoctuaryEditor::cellForParam(ParamId id) const
{
    for (int i = 0; i < static_cast<int>(cells_.size()); ++i) if (cells_[static_cast<size_t>(i)].param == static_cast<int>(id)) return i;
    return -1;
}

/**
 * The Near Source's cells by its type, the way updateSourceCells does it for the four slots: what
 * the chosen type ignores is gone from the strip, not greyed.
 */
bool NoctuaryEditor::updateNearCells()
{
    enum { Off = 0, Harmonic = 1, Fm = 2, Texture = 3, Noise = 4, Additive = 5, Stretch = 6, Bow = 7, Spectral = 8, Wavetable = 9,
           Flute = 10, Murmur = 11, Bowl = 12, Ice = 13, Drops = 14, Clip = 15,
           Whistler = 16, Shaker = 17, Chime = 18, Geiger = 19, Tube = 20, Krell = 21, Beacon = 22, Morse = 23, Dial = 24 };
    const int type = static_cast<int>(std::lround(proc_.engine().getParam(ParamId::ForeType)));
    const bool rubbed = type == Bowl || type == Ice, near = type >= Flute && type <= Drops, signal = type >= Whistler;
    struct Rule { ParamId id; bool on; };
    const Rule rules[] = {
        { ParamId::ForeOctave,   true },
        { ParamId::ForeRatio,    true },
        { ParamId::ForePosition, type == Harmonic || type == Wavetable || type == Texture || type == Noise || type == Stretch || type == Bow || type == Spectral || near || type == Clip
                                 || type == Shaker || type == Geiger || type == Tube || type == Krell || type == Morse || type == Dial },
        { ParamId::ForePosDrift, type != Clip && (!signal || type == Chime) },
        { ParamId::ForeDensity,  type == Texture || type == Noise || type == Drops || type == Shaker || type == Geiger || type == Beacon },
        { ParamId::ForeFollow,   type == Texture || type == Noise || type == Stretch || type == Spectral || type == Drops || type == Clip || type == Shaker },
        { ParamId::ForeBright,   type == Additive || type == Bow || type == Spectral || near || type == Whistler || type == Chime || type == Tube || type == Morse || type == Dial },
        { ParamId::ForeForce,    type == Bow || type == Flute || type == Murmur || rubbed || type == Shaker || type == Chime || type == Geiger },
        { ParamId::ForeSpeed,    type == Bow || type == Flute || type == Murmur || rubbed || type == Whistler || type == Krell || type == Morse },
        { ParamId::ForeNoise,    type == Noise },
        { ParamId::ForeNoiseQ,   type == Noise || type == Shaker || type == Geiger },
        { ParamId::ForeFmRatio,  type == Fm || type == Krell },
        { ParamId::ForeFmIndex,  type == Fm || type == Krell },
        { ParamId::ForePartials, type == Additive },
        { ParamId::ForeTilt,     type == Additive || type == Chime },
        { ParamId::ForeInharm,   type == Additive },
        { ParamId::ForeDrift,    type != Off && type != Noise && type != Drops && type != Clip && !signal },
        { ParamId::ForeTable,    type == Harmonic || type == Wavetable },
    };
    bool changed = false;
    for (const Rule& r : rules) {
        const int ci = cellForParam(r.id);
        if (ci < 0) continue;
        Cell& c = cells_[static_cast<size_t>(ci)];
        if (c.unused != !r.on) { c.unused = !r.on; c.comp->setEnabled(r.on); changed = true; }
    }
    if (nearClipCell_ >= 0) {   // the clip button belongs to the types that read a recording
        Cell& cc = cells_[static_cast<size_t>(nearClipCell_)];
        const bool on = type == Clip || type == Texture || type == Stretch || type == Spectral;
        if (cc.unused != !on) { cc.unused = !on; changed = true; }
    }
    return changed;
}

void NoctuaryEditor::updateSourceCells()
{
    // The 24 slot fields (see Params.h): 0 type 1 level 2 octave 3 ratio 4 pan 5 table 6 position
    // 7 pos drift 8 fm ratio 9 fm index 10 grain 11 density 12 follow 13 grains 14 spread 15 noise
    // 16 noise q 17 partials 18 tilt 19 bright 20 odd/even 21 inharmonic 22 shimmer 23 shimmer rate.
    // Source 1 has the same fields under other ids. Grey out what the chosen type ignores; the
    // Strands section (unison, detune, stack...) belongs to Source 1's additive bank alone.
    enum { Off = 0, Harmonic = 1, Fm = 2, Texture = 3, Noise = 4, Additive = 5, Stretch = 6, Bow = 7, Spectral = 8, Wavetable = 9,
           Flute = 10, Murmur = 11, Bowl = 12, Ice = 13, Drops = 14, Clip = 15,
           Whistler = 16, Shaker = 17, Chime = 18, Geiger = 19, Tube = 20, Krell = 21, Beacon = 22, Morse = 23, Dial = 24 };
    // The ids come from Params.h (slotParamIds), so this list and the engine's cannot drift apart.
    // kSlots, not a number: a literal 3 here quietly left Source 4's cells lit whatever its type.
    bool cellsChanged = false;
    for (int k = 0; k < ambient::kSlots; ++k) {
        const ParamId* ids = slotParamIds(k);
        const int type = static_cast<int>(std::lround(proc_.engine().getParam(ids[0])));
        const bool rubbed = type == Bowl || type == Ice, near = type >= Flute && type <= Drops, signal = type >= Whistler;
        // Every field, not the first 33: the loop used to stop at Transport, and everything added after
        // it -- Interp, Unison and its detune and width, Root -- stood lit for every type, Root on a clip.
        for (int off = 1; off < ambient::kSlotFields; ++off) {
            bool on = type != Off;
            switch (off) {
            case 5:  on = type == Harmonic || type == Wavetable; break;         // table choice
            case 6:  on = type == Harmonic || type == Wavetable || type == Texture || type == Noise || type == Stretch || type == Bow || type == Spectral || near || type == Clip
                          || type == Shaker || type == Geiger || type == Tube || type == Krell || type == Morse || type == Dial; break;   // position, or where the bow sits, the embouchure, the stick, the medium, the vessel, where the clip starts, the shell, the tube, the mains, the quantiser, the letters, the whistles
            case 7:  on = type != Off && (!signal || type == Chime); break;      // pos drift moves all of them; of the signals only the chime's split
            case 8:
            case 9:  on = type == Fm || type == Krell; break;
            case 10: on = type == Texture || type == Stretch; break;             // grain: the grain, or the spectral window
            case 13:
            case 14: on = type == Texture; break;                                // grains, spread
            case 11: on = type == Texture || type == Noise || type == Drops || type == Shaker || type == Geiger || type == Beacon; break;   // density: grains, crackle, drops, shakes, clicks, packets
            case 12: on = type == Texture || type == Noise || type == Stretch || type == Spectral || type == Drops || type == Clip || type == Shaker; break;   // pitch follow
            case 15: on = type == Noise; break;
            case 16: on = type == Noise || type == Shaker || type == Geiger; break;   // noise q: the band, the shell's ring, the tube's damping
            case 19: on = type == Additive || type == Bow || type == Spectral || near || type == Whistler || type == Chime || type == Tube || type == Morse || type == Dial; break;   // bright
            case 17: case 20: case 21: case 22: case 23: on = type == Additive; break;
            case 18: on = type == Additive || type == Chime; break;               // tilt: the bank's, or the bell's hum
            case 24: on = type == Texture || type == Noise || type == Drops || type == Shaker || type == Geiger || type == Beacon; break;   // density sync
            case 25: on = type != Off && type != Noise && type != Drops && type != Clip && !signal; break;   // pitch drift
            case 26:
            case 27: on = type == Stretch; break;                                 // stretch, loop fade
            case 28: on = type == Bow || type == Flute || type == Murmur || rubbed || type == Shaker || type == Chime || type == Geiger; break;   // force: the bow, the breath, the effort, the stick, the beans, the ring, the cluster
            case 29: on = type == Bow || type == Flute || type == Murmur || rubbed || type == Whistler || type == Krell || type == Morse; break;   // speed: the bow, the breath, the effort, the stick, the fall, the pace, the words a minute
            case 30:
            case 31: on = type == Spectral; break;                                // spectral rate, breath
            case 32: on = type == Harmonic; break;                                // transport: how the spectra morph
            case 36: on = type == Texture; break;                                 // interpolation: how a clip is read
            case 37:
            case 38:
            case 39: on = type == Harmonic || type == Wavetable; break;           // unison, its detune and width: the table types
            case 40: on = type == Harmonic; break;                                // root: a floor under a spectrum's fundamental
            case 41: on = type != Off; break;                                     // the role: which notes the slot sounds in
            default: break;
            }
            const int ci = cellForParam(ids[off]);
            if (ci < 0) continue;
            Cell& c = cells_[static_cast<size_t>(ci)];
            // Not greyed: gone. The strip is packed from the cells the type uses, so its height
            // is the type's own and not the union of every type's.
            if (c.unused != !on) { c.unused = !on; c.comp->setEnabled(on); cellsChanged = true; }
        }
        // The slot's file button belongs to the types that read a clip; the wavetable button to the table.
        if (textureCell_[k] >= 0) {
            Cell& tc = cells_[static_cast<size_t>(textureCell_[k])];
            const bool on = type == Texture || type == Stretch || type == Spectral || type == Clip;
            if (tc.unused != !on) { tc.unused = !on; cellsChanged = true; }
        }
        if (k == 1 && tableCell_ >= 0) {
            Cell& wc = cells_[static_cast<size_t>(tableCell_)];
            const bool on = type == Harmonic || type == Wavetable;
            if (wc.unused != !on) { wc.unused = !on; cellsChanged = true; }
        }
    }
    if (Section* strands = findSection("Strands")) {   // the strand bank exists only while Source 1 is additive: not greyed, gone
        const bool on = std::lround(proc_.engine().getParam(ParamId::Src1Type)) == Additive;
        for (int ci : strands->cells) {
            Cell& c = cells_[static_cast<size_t>(ci)];
            if (c.unused != !on) { c.unused = !on; c.comp->setEnabled(on); cellsChanged = true; }
        }
    }
    if (updateNearCells()) cellsChanged = true;
    if (refreshLayoutState()) cellsChanged = true;
    if (cellsChanged && getWidth() > 0) rebuildLayout();   // at construction the size is not set yet; the first layout follows
    auto nameCell = [&](int ci, const juce::String& base, const juce::String& file) {
        if (ci < 0) return;
        Cell& c = cells_[static_cast<size_t>(ci)];
        const juce::String text = file.isNotEmpty() ? file : base;
        if (c.label->getText() != text) c.label->setText(text, juce::dontSendNotification);
    };
    nameCell(tableCell_, "User table", proc_.wavetableName());
    for (int k = 0; k < ambient::kSlots; ++k) nameCell(textureCell_[k], "Texture file", proc_.textureName(k));
    nameCell(impulseCell_, "Dark Hall (built in)", proc_.impulseName());
    nameCell(impulseBCell_, "no second room", proc_.impulseBName());
    nameCell(nearClipCell_, "Source 4's clip", proc_.nearClipName());
}

void NoctuaryEditor::showMappingEditor()
{
    // A plain text editor over the mapping table: one line per mapping,
    //   input param min max smooth deadzone clutch invert
    auto* editor = new juce::TextEditor();
    editor->setMultiLine(true, false);
    editor->setReturnKeyStartsNewLine(true);
    editor->setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
    editor->setText(proc_.gestureMappings(), false);
    editor->setSize(640, 420);
    auto* content = new juce::Component();
    content->setSize(640, 470);
    content->addAndMakeVisible(editor);
    editor->setBounds(0, 0, 640, 420);
    auto* hint = new juce::Label(juce::String(), "input param min max [smooth] [deadzone] [clutch|none] [invert]   inputs: HandDistance LeftHeight RightHeight LeftForward RightForward LeftTilt RightTilt LeftPinch RightPinch HeadYaw HeadPitch HeadRoll Custom0..7 (= macros A..H)");
    hint->setFont(juce::FontOptions(11.0f));
    hint->setColour(juce::Label::textColourId, kDim);
    hint->setBounds(0, 424, 640, 44);
    hint->setMinimumHorizontalScale(0.5f);
    content->addAndMakeVisible(hint);
    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = "Gesture and macro mappings";
    o.content.setOwned(content);
    o.componentToCentreAround = this;
    o.dialogBackgroundColour = kGroupFill;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
    // The timer mirrors the text while the dialog lives and applies it once the dialog is gone.
    mapEditor_ = editor;
    mapText_ = editor->getText();
    mapOpen_ = true;
}

void NoctuaryEditor::chooseSourceFile(bool wavetable, int slot)
{
    const juce::String what = wavetable ? juce::String("Load a wavetable (Serum, Vital, Hive, Surge .wt, WaveEdit, a single cycle)")
                            : slot >= 0 ? "Load a clip for Source " + juce::String(slot + 1)
                                        : juce::String("Load a texture sample");
    chooser_ = std::make_unique<juce::FileChooser>(what, juce::File(), wavetable ? "*.wav;*.wt;*.flac;*.aif;*.aiff"
                                                                                : "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this, wavetable, slot](const juce::FileChooser& fc) {
            const auto file = fc.getResult();
            if (!file.existsAsFile()) return;
            const bool ok = wavetable ? proc_.loadWavetableFile(file)
                          : slot >= 0 ? proc_.loadTextureFile(slot, file) : proc_.loadTextureFile(file);
            if (!ok)
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, wavetable ? "Wavetable" : "Texture",
                    wavetable ? "Could not read this file as a wavetable (no whole cycle in it, or nothing but silence)." : "Could not read this audio file.");
            repaint();
        });
}

void NoctuaryEditor::chooseNearClipFile()
{
    chooser_ = std::make_unique<juce::FileChooser>("Load a recording for the near source", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& fc) {
            const auto file = fc.getResult();
            if (!file.existsAsFile()) return;
            if (!proc_.loadNearClipFile(file))
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Near clip", "Could not read this audio file.");
            repaint();
        });
}

void NoctuaryEditor::chooseImpulseFile(bool second)
{
    chooser_ = std::make_unique<juce::FileChooser>(second ? "Load the second impulse response (Room Morph fades to it)"
                                                          : "Load an impulse response (mono or stereo)", juce::File(), "*.wav;*.aif;*.aiff;*.flac");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this, second](const juce::FileChooser& fc) {
            const auto file = fc.getResult();
            if (!file.existsAsFile()) return;
            if (!proc_.loadImpulseFile(file, second))
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Impulse", "Could not read this audio file.");
            repaint();
        });
}

void NoctuaryEditor::chooseScalaFile()
{
    chooser_ = std::make_unique<juce::FileChooser>("Load a Scala scale", juce::File(), "*.scl");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& fc) {
            const auto file = fc.getResult();
            if (!file.existsAsFile()) return;
            if (!proc_.loadScalaText(file.loadFileAsString(), file.getFileNameWithoutExtension()))
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Scala", "This file is not a valid .scl scale.");
            repaint();
        });
}

// ---------------------------------------------------------------- painting

void NoctuaryEditor::ScopeView::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    g.setColour(ui::card.withAlpha(0.55f));
    g.fillRoundedRectangle(r, 6.0f);
    g.setColour(ui::cardEdge);
    g.drawRoundedRectangle(r.reduced(0.5f), 6.0f, 1.0f);

    float live[ambient::kMaxPartials] = {};
    const int n = proc.engine().displayPartials(live, ambient::kMaxPartials);
    count = juce::jmax(count, n);
    // Glide towards the engine's values: at 30 Hz the raw numbers would flicker, and the point is
    // to see the slow breathing, not the control-block edges.
    for (int i = 0; i < ambient::kMaxPartials; ++i) {
        const float target = i < n ? std::abs(live[i]) : 0.0f;
        amp[i] += (target - amp[i]) * 0.25f;
    }
    float peak = 1.0e-6f;
    for (int i = 0; i < count; ++i) peak = juce::jmax(peak, amp[i]);
    const bool silent = peak < 1.0e-4f;

    const auto plot = r.reduced(9.0f, 7.0f);
    if (silent) {
        g.setColour(ui::faint);
        g.setFont(ui::body(11.0f));
        g.drawText("oscillator", plot, juce::Justification::centred, false);
        g.setColour(ui::track);
        g.drawLine(plot.getX(), plot.getCentreY(), plot.getRight(), plot.getCentreY(), 1.0f);
        return;
    }

    // Spectrum behind: one thin bar per partial, on a decibel scale.
    {
        const float bw = juce::jmin(9.0f, plot.getWidth() / static_cast<float>(juce::jmax(count, 1)));
        for (int i = 0; i < count; ++i) {
            const float a = amp[i] / peak;
            if (a < 1.0e-4f) continue;
            const float db = juce::jlimit(0.0f, 1.0f, 1.0f + std::log10(a) / 2.5f);   // -50 dB .. 0
            const float h = db * plot.getHeight() * 0.9f;
            g.setColour(ui::accent.withAlpha(0.10f + 0.13f * db));
            g.fillRect(plot.getX() + i * bw + 0.5f, plot.getBottom() - h, juce::jmax(1.0f, bw - 1.2f), h);
        }
    }

    // One cycle of the wave those partials make. The phases are fixed (golden angle) so the shape
    // is characteristic and readable; what moves is the amplitudes, which is what actually moves.
    juce::Path wave;
    const int steps = juce::jlimit(96, 512, static_cast<int>(plot.getWidth()));
    const float mid = plot.getCentreY(), half = plot.getHeight() * 0.42f;
    for (int x = 0; x <= steps; ++x) {
        const float t = static_cast<float>(x) / static_cast<float>(steps);
        float v = 0.0f;
        for (int i = 0; i < count; ++i) {
            if (amp[i] < 1.0e-5f) continue;
            const float ph = 0.381966f * static_cast<float>(i);
            v += amp[i] * std::sin(juce::MathConstants<float>::twoPi * ((i + 1) * t + ph));
        }
        const float y = mid - juce::jlimit(-1.0f, 1.0f, v / (peak * 2.2f)) * half;
        const float px = plot.getX() + t * plot.getWidth();
        if (x == 0) wave.startNewSubPath(px, y); else wave.lineTo(px, y);
    }
    g.setColour(ui::accent.withAlpha(0.20f));
    g.strokePath(wave, juce::PathStrokeType(4.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(ui::accent);
    g.strokePath(wave, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const float hz = proc.engine().displayFrequency();
    if (hz > 0.0f) {
        g.setColour(ui::dim);
        g.setFont(ui::body(10.0f));
        g.drawText(juce::String(hz, 1) + " Hz   " + juce::String(count) + " partials",
                   r.reduced(10.0f, 5.0f), juce::Justification::topRight, false);
    }
}

void NoctuaryEditor::parentHierarchyChanged()
{
    // A maximise button beside the other two. Only the standalone has a DocumentWindow of its own;
    // in a host the plug-in lives in the host's window and this finds nothing, which is right.
    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
        window->setTitleBarButtonsRequired(juce::DocumentWindow::minimiseButton
                                         | juce::DocumentWindow::maximiseButton
                                         | juce::DocumentWindow::closeButton, false);
}

void NoctuaryEditor::paint(juce::Graphics& g)
{
    // Everything below is drawn in the design space; one transform scales the whole editor.
    g.fillAll(ui::bg0);
    g.addTransform(juce::AffineTransform::scale(scale_));
    {   // a little vertical lift, so the window is not a flat grey field
        juce::ColourGradient sky(ui::bg1, 0.0f, 0.0f, ui::bg0, 0.0f, static_cast<float>(kHeaderH * 3), false);
        g.setGradientFill(sky);
        g.fillRect(0, 0, designW_, kHeaderH * 3);
    }

    // Header
    g.setColour(kGroupFill);
    g.fillRect(header_);
    g.setColour(kText);
    g.setFont(juce::FontOptions(22.0f, juce::Font::bold));
    g.drawText("Noctuary", 14, 6, 180, 26, juce::Justification::centredLeft);
    {   // The help line: the control under the mouse, its value and what it does -- otherwise
        // the three things worth knowing first. This is where the routing map used to be; the
        // signal flow is in the manual now, where it can be read rather than deciphered.
        g.setColour(ui::card.withAlpha(0.6f));
        g.fillRoundedRectangle(helpLine_.toFloat(), 5.0f);
        juce::String title, body;
        if (hoveredParam_ >= 0 && hoveredParam_ < kNumParams) {
            const ParamId id = static_cast<ParamId>(hoveredParam_);
            const ParamDesc& d = paramDesc(id);
            juce::String value;
            if (auto* v = proc_.apvts.getRawParameterValue(d.key)) {
                const float raw = v->load();
                if (d.kind == ParamKind::Choice) value = d.choices[juce::jlimit(0, d.numChoices - 1, static_cast<int>(std::lround(raw)))];
                else if (d.kind == ParamKind::Bool) value = raw >= 0.5f ? "on" : "off";
                else if (auto* pp = proc_.apvts.getParameter(d.key)) value = pp->getText(pp->convertTo0to1(raw), 24) + (d.unit[0] ? juce::String(" ") + d.unit : juce::String());
            }
            title = juce::String(d.section).toUpperCase() + "   " + d.name + "      " + value;
            body = ambient::paramHelp(id);
            const int mods = [&] { int n = 0; const auto& m = proc_.engine().modMatrix(); for (int k = 0; k < m.count(); ++k) if (m.route(k).target == id) ++n; return n; }();
            if (mods > 0) body += "   [" + juce::String(mods) + (mods == 1 ? " modulation route" : " modulation routes") + " -- right-click to see them]";
        } else {
            title = "Noctuary";
            body = "Point at any control to read what it does. Help (or F1) opens the manual. Right-click a knob for MIDI learn and its modulation routes; drag a card from the strip at the bottom onto a knob to modulate it.";
        }
        g.setColour(kText);
        g.setFont(ui::title(10.5f));
        g.drawText(title, helpLine_.reduced(10, 5), juce::Justification::topLeft, false);
        g.setColour(kDim);
        g.setFont(ui::body(11.5f));
        g.drawFittedText(body, helpLine_.reduced(10, 5).withTrimmedTop(17), juce::Justification::topLeft, 3, 1.0f);
    }

    // Keyboard strip with near (bright) / far (dim) notes and the brain's root.
    const int root = proc_.engine().brainRoot();
    const int first = 24, last = 108;
    const float keyW = static_cast<float>(keys_.getWidth()) / static_cast<float>(last - first + 1);
    for (int n = first; n <= last; ++n) {
        const float kx = keys_.getX() + (n - first) * keyW;
        const bool black = juce::MidiMessage::isMidiNoteBlack(n);
        juce::Colour col = black ? juce::Colour(0xff2a2e36) : juce::Colour(0xff3a3f4a);
        if (sounding_[n]) {
            const float d = juce::jlimit(0.0f, 1.0f, proc_.engine().noteDistance(n));
            col = kAccent.interpolatedWith(juce::Colour(0xff35506a), d);
        }
        if (n == root) col = col.interpolatedWith(juce::Colours::orange, 0.6f);
        g.setColour(col);
        g.fillRect(kx + 0.5f, static_cast<float>(keys_.getY()), keyW - 1.0f, static_cast<float>(keys_.getHeight()));
    }
    const int voices = proc_.engine().activeVoices();
    juce::String keyText;
    {   // what the conductor has found itself in, when it has found anything
        const ambient::KeyEstimate key = proc_.engine().brainKey();
        if (key.key >= 0 && key.confidence > 0.0f)
            keyText = "   key " + juce::MidiMessage::getMidiNoteName(key.tonic(), true, false, 0) + (key.minor() ? " minor" : " major") + " r " + juce::String(key.confidence, 2);
    }
    juce::String info = juce::String(voices) + " voice" + (voices == 1 ? "" : "s")
        + "   root " + juce::MidiMessage::getMidiNoteName(root, true, true, 4)
        + "   " + juce::String(proc_.engine().scale().name)
        + "   arc " + juce::String(proc_.engine().arcValue(), 2) + keyText
        + "   " + juce::String(proc_.engine().tempo(), 1) + " bpm  bar " + juce::String(1 + static_cast<int>(std::floor(proc_.engine().beatPosition() / 4.0)))
        + (proc_.engine().clockRunning() ? "" : " (stopped)");
    if (proc_.userScaleName().isNotEmpty()) info += "   (user: " + proc_.userScaleName() + ")";
    info += proc_.oscRunning() ? "   OSC :" + juce::String(proc_.oscPort()) + " (" + juce::String(static_cast<juce::int64>(proc_.oscMessages())) + " msg)"
                               : "   OSC off: " + proc_.oscError();
    const auto& gl = proc_.gestures();
    const float clutch = gl.input(GestureInput::RightPinch);
    if (gl.calibrating()) info += "   CALIBRATING " + juce::String(static_cast<int>(gl.calibrationProgress() * 100.0f)) + " %";
    else info += clutch > 0.5f ? "   hands: engaged" : "   hands: free";
    info += "   L " + juce::String(gl.input(GestureInput::LeftHeight), 2) + "  R " + juce::String(gl.input(GestureInput::RightHeight), 2)
          + "  dist " + juce::String(gl.input(GestureInput::HandDistance), 2) + "  pinch " + juce::String(clutch, 2);
    g.setColour(kDim);
    g.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain)));   // figures that do not jump
    g.drawText(info, 14, header_.getBottom() - 16, designW_ - 420, 14, juce::Justification::centredLeft);
    {   // The cascade's aura: a soft glow after the line that brightens with each event the
        // conductor's clock breeds and fades as the excitation does -- the Hawkes process, seen.
        const float ex = proc_.engine().brainExcitation();
        if (ex > 0.02f) {
            const float a = juce::jlimit(0.0f, 1.0f, ex / 1.6f);
            const int x = 14 + juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), info) + 12;
            const float cy = static_cast<float>(header_.getBottom() - 9);
            g.setColour(ui::live.withAlpha(0.25f * a)); g.fillEllipse(x - 8.0f, cy - 8.0f, 16.0f, 16.0f);
            g.setColour(ui::live.withAlpha(0.9f * a)); g.fillEllipse(x - 3.0f, cy - 3.0f, 6.0f, 6.0f);
            g.setColour(kDim.withAlpha(0.5f + 0.5f * a)); g.drawText("cascade", x + 12, header_.getBottom() - 16, 60, 14, juce::Justification::centredLeft);
        }
    }

    if (proc_.isRecording()) {
        g.setColour(juce::Colour(0xffe05050));
        g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        g.drawText("REC " + juce::String(static_cast<int>(proc_.recordedSeconds() / 60)) + ":" + juce::String(static_cast<int>(proc_.recordedSeconds()) % 60).paddedLeft('0', 2),
                   760, 8, 90, 24, juce::Justification::centredLeft);
    }
    // The Master section is the only section painted here (it sits in the header).
    if (const Section* ms = const_cast<NoctuaryEditor*>(this)->findSection("Master")) {
        g.setColour(kSectionFill);
        g.fillRoundedRectangle(ms->bounds.toFloat(), 5.0f);
        g.setColour(kMaster.withAlpha(0.85f));
        g.setFont(juce::FontOptions(11.5f, juce::Font::bold));
        g.drawText("MASTER", ms->bounds.getX() + kPad, ms->bounds.getY() + 1, ms->bounds.getWidth() - 2 * kPad, kTitleH, juce::Justification::centredLeft);
    }
}

void NoctuaryEditor::paintContent(juce::Graphics& g)
{
    g.fillAll(kBg);
    for (const auto& grp : (expanded_ ? expandedGroups_ : groups_)) {
        g.setColour(kGroupFill);
        g.fillRoundedRectangle(grp.bounds.toFloat(), 8.0f);
        g.setColour(grp.colour);
        g.fillRoundedRectangle(juce::Rectangle<float>(static_cast<float>(grp.bounds.getX()), static_cast<float>(grp.bounds.getY()), 4.0f, static_cast<float>(grp.bounds.getHeight())), 2.0f);
        g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.drawText(grp.name, grp.bounds.getX() + 12, grp.bounds.getY() + 2, grp.bounds.getWidth() - 20, kGroupTitleH, juce::Justification::centredLeft);
    }
    for (const auto& t : tabRows_) {
        if (expanded_) break;   // no tabs on the Expanded page
        const juce::Colour col = groups_[static_cast<size_t>(t.group)].colour;
        for (size_t i = 0; i < t.tabs.size(); ++i) {
            const bool on = static_cast<int>(i) == t.active;
            const auto r = t.tabs[i].toFloat();
            g.setColour(on ? col.withAlpha(0.26f) : kSectionFill.brighter(0.04f));
            g.fillRoundedRectangle(r, 5.0f);
            if (on) { g.setColour(col.withAlpha(0.9f)); g.drawRoundedRectangle(r.reduced(0.5f), 5.0f, 1.0f); }
            g.setColour(on ? kText : kDim);
            g.setFont(ui::title(10.5f));
            g.drawText(t.names[i], t.tabs[i], juce::Justification::centred);
        }
    }
    for (const auto& s : sections_) {
        if (s.name == "Master" || !s.visible) continue;   // painted in the header / behind another tab
        const juce::Colour col = s.group >= 0 ? groups_[static_cast<size_t>(s.group)].colour : kMaster;
        g.setColour(kSectionFill);
        g.fillRoundedRectangle(s.bounds.toFloat(), 5.0f);
        g.setColour(col.withAlpha(0.85f));
        g.setFont(juce::FontOptions(11.5f, juce::Font::bold));
        g.drawText(s.name.toUpperCase(), s.bounds.getX() + kPad, s.bounds.getY() + 1, s.bounds.getWidth() - 2 * kPad, kTitleH, juce::Justification::centredLeft);
        {   // The die: five pips in a small square at the right of the title. Click to redraw this
            // section, shift-click to nudge it.
            const juce::Rectangle<int> die(s.bounds.getRight() - kPad - 13, s.bounds.getY() + 4, 13, 13);
            diceOf_[s.name] = die;
            g.setColour(ui::card.brighter(0.10f));
            g.fillRoundedRectangle(die.toFloat(), 3.0f);
            g.setColour(col.withAlpha(0.5f));
            g.drawRoundedRectangle(die.toFloat().reduced(0.5f), 3.0f, 1.0f);
            g.setColour(col.withAlpha(0.75f));
            const float cx = die.toFloat().getCentreX(), cy = die.toFloat().getCentreY(), d = 3.2f;
            for (auto o : { juce::Point<float>(-d, -d), { d, -d }, { 0.0f, 0.0f }, { -d, d }, { d, d } })
                g.fillEllipse(cx + o.x - 1.0f, cy + o.y - 1.0f, 2.0f, 2.0f);
        }
        // The loop, made visible where it closes: the feedback section says it is returning
        // into the sources, and Source 1 says it is being fed. A glyph, not an animation.
        if (s.closedNow()) {
            g.setColour(kDim.withAlpha(0.8f));
            g.setFont(juce::FontOptions(10.5f));
            g.drawText("off -- switch on to open", s.bounds.getX() + 90, s.bounds.getY() + 1, s.bounds.getWidth() - 120, kTitleH, juce::Justification::centredLeft);
        }
        if (s.name == "Feedback" || s.name == "Source 1") {
            const float bus = proc_.apvts.getRawParameterValue("fb_bus")->load();
            const float fm = proc_.apvts.getRawParameterValue("fb_fm")->load();
            juce::String note;
            if (s.name == "Feedback" && (bus > 0.0f || fm > 0.0f))
                note = juce::String(juce::CharPointer_UTF8("\xe2\x86\xba")) + "  returns to the sources" + (fm > 0.0f ? " (pitch)" : "");
            if (s.name == "Source 1" && fm > 0.0f)
                note = juce::String(juce::CharPointer_UTF8("\xe2\x86\xba")) + "  fed back from the output";
            if (note.isNotEmpty()) {
                g.setColour(ui::live.withAlpha(0.85f));
                g.setFont(juce::FontOptions(10.5f));
                g.drawText(note, s.bounds.getX() + 90, s.bounds.getY() + 1, s.bounds.getWidth() - 120, kTitleH, juce::Justification::centredLeft);
            }
        }
        if (s.name == "Morph") {
            const float pos = proc_.engine().morphPosition();
            const bool active = proc_.apvts.getRawParameterValue("morph_active")->load() >= 0.5f;
            juce::String txt = "A: " + proc_.morphSlotName(0) + "   B: " + proc_.morphSlotName(1)
                             + (active ? "   playing " + juce::String(static_cast<int>(std::lround((1.0f - pos) * 100))) + " % A / "
                                         + juce::String(static_cast<int>(std::lround(pos * 100))) + " % B" : "   (off)");
            g.setColour(kDim);
            g.setFont(juce::FontOptions(10.5f));
            g.drawText(txt, s.bounds.getX() + 70, s.bounds.getY() + 1, s.bounds.getWidth() - 80, kTitleH, juce::Justification::centredLeft);
            // position bar
            juce::Rectangle<float> bar(static_cast<float>(s.bounds.getX() + kPad), static_cast<float>(s.bounds.getBottom() - 6), static_cast<float>(s.bounds.getWidth() - 2 * kPad), 3.0f);
            g.setColour(juce::Colour(0xff2c3038)); g.fillRect(bar);
            g.setColour(kMorph); g.fillRect(bar.withWidth(bar.getWidth() * pos));
        }
    }
}
