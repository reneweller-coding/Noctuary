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
 * @author Rene Weller
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
 * @param[in,out] box  the chooser to fill (emptied of nothing: called once on a fresh box)
 */
void fillPresetBox(juce::ComboBox& box)
{
    const bool grouped = numPresetMeta() >= numPresets() && numPresetFamilies() > 1;
    if (!grouped) {
        for (int i = 0; i < numPresets(); ++i) box.addItem(preset(i).m_name, i + 1);
        return;
    }
    juce::PopupMenu* root = box.getRootMenu();
    juce::PopupMenu family;
    int lastFamily = -1;
    for (int i = 0; i < numPresets(); ++i) {
        const int fam = presetMeta(i).m_family;
        if (fam != lastFamily) {
            if (lastFamily >= 0) root->addSubMenu(presetFamilyName(lastFamily), family);
            family.clear();
            lastFamily = fam;
        }
        family.addItem(i + 1, preset(i).m_name);
    }
    if (lastFamily >= 0) root->addSubMenu(presetFamilyName(lastFamily), family);
}
} // namespace


namespace {
}


namespace {

/**
 * @brief A source as a person reads it: "lfo1" -> "LFO 1", "macro_a" -> "Macro A", "lorenz_x" -> "Lorenz X".
 * @param[in] s  the source
 * @return its name
 */
juce::String prettySource(ambient::ModSourceE s)
{
    juce::String t(ambient::modSourceName(s));
    t = t.replace("_", " ");
    juce::String out;
    for (int i = 0; i < t.length(); ++i) {
        const juce::juce_wchar c = t[i];
        if (i > 0 && juce::CharacterFunctions::isDigit(c) && !juce::CharacterFunctions::isDigit(t[i - 1]) && t[i - 1] != ' ') out << ' ';
        out << juce::String::charToString(c);
    }
    juce::StringArray words = juce::StringArray::fromTokens(out, " ", "");
    for (auto& w : words) w = w == "lfo" ? juce::String("LFO") : w.substring(0, 1).toUpperCase() + w.substring(1);
    return words.joinIntoString(" ");
}

/**
 * @brief A source's section in the "Modulate with" menu.
 * @param[in] name  the source's name
 * @return its section
 */
juce::String sourceSection(const juce::String& name)
{
    const juce::String l = name.toLowerCase();
    if (l.startsWith("lfo")) return "LFOs";
    if (l.startsWith("env") || l == "amp") return "Envelopes";
    if (l.startsWith("macro")) return "Macros";
    if (l.startsWith("kura") || l.startsWith("lenia") || l.startsWith("lorenz") || l.startsWith("rossler") || l.startsWith("cascade")) return "Fields and chaos";
    if (l.startsWith("note") || l.startsWith("velocity") || l.startsWith("distance") || l.startsWith("random") || l.startsWith("pressure")
        || l.startsWith("wheel") || l.startsWith("slide"))
        return "The note and the hands";
    return "The piece and what it hears";
}

} // namespace

NoctuaryEditor::NoctuaryEditor(NoctuaryProcessor& p)
    : AudioProcessorEditor(p), m_proc(p)
{
    setLookAndFeel(&m_laf);

    // Two columns, everything in sight at once: the voice and the morph on the left, the effects,
    // the cosmos and the conductor on the right. Rows whose sections are of a kind (the three
    // sources, the two filters, the effect pairs, the conductor's tables) page through tabs.
    // Every page is fitted to its column (layoutTabbed), so what the tables decide is which
    // sections share a page, and the pages are cut so that each fills its row: no page of one
    // small section with nothing beside it, and every page that can have a display has one.
    m_groups = {
        { "VOICE",      VoiceHue,     { { "Source 1", "Strands", "Source 2", "Source 3", "Source 4" }, { "Air", "Filter", "Z-Plane", "Envelope", "Expression" }, { "Space", "Foundation" } }, {}, 0 },   // rows 0 and 1 page through tabs
        // Morph, the macros and the vector in one row: the three controls that act on the whole
        // sound at once, with the vector's square as the row's display. (The vector used to be a
        // tab of the sources, and the macros a page with nothing beside them.)
        { "MORPH",      MorphHue,     { { "Morph", "Macros", "Vector" } }, {}, 0 },
        { "FOREGROUND", ForeHue,      { { "Ensemble", "Delay", "Delay 2", "Near Reverb", "Blur" } }, {}, 1 },
        { "BACKGROUND", BackHue,      { { "Cloud", "Far Reverb", "Feedback", "Room", "Early Room", "Body", "Patina" } }, {}, 1 },
        // Strike shares the Cosmos group as a tab: like the Cosmos it is a sound source that is
        // not one of the four oscillators, and beside them it read as a fifth. The Memory is the
        // second parallel world beside the Cosmos, and takes the third tab.
        // The near layer takes the last two tabs: its source and its events, each a page.
        { "COSMOS",     CosmosHue,    { { "Cosmos", "Strike", "Memory", "Near Source", "Near Events" } }, {}, 1 },
        // Last in the right column and stretchy: every page of it has a display, and whatever
        // height the left column has over goes to that display, the page refitted taller.
        { "CONDUCTOR",  ConductorHue, { { "Cluster Brain", "Autoplay", "Brain 2", "Tuning", "Coherence", "Clock" } }, {}, 1, {}, 0, true },
        // Last in the left column, and the only group with no controls in it: the right column is
        // taller than the left, and the difference used to be an empty rectangle the width of the
        // page. It now holds the output's spectrum, and because the row stretches, the hole cannot
        // come back when a tab changes the height of a row above it.
        { "ANALYSIS",   VoiceHue,     { {} }, {}, 0, {}, 96, true },
    };
    m_tabRows = {
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

    m_content.m_onPaint = [this](juce::Graphics& g) { paintContent(g); };
    m_content.m_onMouse = [this](const juce::MouseEvent& e) {
        if (e.mods.isPopupMenu()) return;
        for (const auto& d : m_diceOf)
            if (d.second.contains(e.getPosition())) { randomiseSection(d.first, e.mods.isShiftDown()); return; }
        clickTabs(e.getPosition());
    };
    m_viewport.setViewedComponent(&m_content, false);
    m_viewport.setScrollBarsShown(false, false);
    addAndMakeVisible(m_viewport);
    addAndMakeVisible(m_dragOverlay);
    buildCells();
    colourCellsByGroup();
    updateSourceCells();   // which cells the types use, before the first layout measures the page
    // The Master section lives in the header, so its cells belong to the editor, not the content.
    if (Section* ms = findSection("Master"))
        for (int ci : ms->m_cells) { addAndMakeVisible(*m_cells[static_cast<size_t>(ci)].m_comp); addAndMakeVisible(*m_cells[static_cast<size_t>(ci)].m_label); }

    m_recButton = std::make_unique<frame::IconButton>(frame::IconButton::IconE::RECORD, "Record the output to a WAV file; press again to stop");
    m_recButton->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffb03030));
    m_recButton->setClickingTogglesState(false);
    m_recButton->onClick = [this] {
        if (m_proc.isRecording()) { m_proc.stopRecording(); m_recButton->setToggleState(false, juce::dontSendNotification); return; }
        m_chooser = std::make_unique<juce::FileChooser>("Record to WAV", juce::File(), "*.wav");
        m_chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
            [this](const juce::FileChooser& fc) {
                auto file = fc.getResult();
                if (file == juce::File()) return;
                if (!file.hasFileExtension("wav")) file = file.withFileExtension("wav");
                if (m_proc.startRecording(file)) m_recButton->setToggleState(true, juce::dontSendNotification);
                else juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Record", "Could not open the file for writing.");
            });
    };
    addAndMakeVisible(*m_recButton);
    // Main: the panel, from wherever you are. It is the one page that had no button of its own --
    // you got back by switching the others off, which is a rule nobody should have to learn.
    m_mainButton = std::make_unique<juce::TextButton>("Main");
    m_mainButton->setTooltip("The panel: every section on one page");
    m_mainButton->setClickingTogglesState(true);
    m_mainButton->setColour(juce::TextButton::buttonOnColourId, Accent.withAlpha(0.5f));
    m_mainButton->setToggleState(true, juce::dontSendNotification);
    m_mainButton->onClick = [this] { setPage(0); };
    addAndMakeVisible(*m_mainButton);
    // The two headset controls under one button: on a desk they are the two you never press.
    m_calibButton = std::make_unique<juce::TextButton>("Calibrate");
    m_calibButton->onClick = [this] { m_proc.gestures().startCalibration(6.0f); };
    m_mapButton = std::make_unique<juce::TextButton>("Gestures...");
    m_mapButton->onClick = [this] { showMappingEditor(); };
    m_vrButton = std::make_unique<juce::TextButton>("VR");
    m_vrButton->setTooltip("Hand tracking: calibrate the hands, edit the gesture table");
    m_vrButton->onClick = [this] {
        juce::PopupMenu m;
        m.addItem(1, "Calibrate hands (6 s: together and apart, low and high, near and far)");
        m.addItem(2, "Gestures... (the gesture / macro mapping table)");
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(m_vrButton.get()), [this](int r) {
            if (r == 1) m_proc.gestures().startCalibration(6.0f);
            else if (r == 2) showMappingEditor();
        });
    };
    addChildComponent(*m_vrButton);   // shown while a headset sends, or when the settings say always (01.10.2026)
    m_performButton = std::make_unique<juce::TextButton>("Perform");
    m_performButton->setTooltip("Only the eight macros and the morph, large: for playing a set");
    m_performButton->setClickingTogglesState(true);
    m_performButton->setColour(juce::TextButton::buttonOnColourId, Accent.withAlpha(0.5f));
    m_performButton->onClick = [this] { setPage(m_performButton->getToggleState() ? 1 : 0); };
    addAndMakeVisible(*m_performButton);
    m_perform = std::make_unique<PerformView>(m_proc);
    addChildComponent(*m_perform);
    m_browseButton = std::make_unique<juce::TextButton>("Browse");
    m_browseButton->setTooltip("Preset browser: filters, tags, and the map of all presets (drag the cursor to blend)");
    m_browseButton->setClickingTogglesState(true);
    m_browseButton->setColour(juce::TextButton::buttonOnColourId, Accent.withAlpha(0.5f));
    m_browseButton->onClick = [this] { setPage(m_browseButton->getToggleState() ? 2 : 0); };
    addAndMakeVisible(*m_browseButton);
    m_browse = std::make_unique<BrowseView>(m_proc);
    addChildComponent(*m_browse);
    m_helpButton = std::make_unique<juce::TextButton>("Help");
    m_helpButton->setTooltip("The manual, by topic (F1)");
    m_helpButton->setClickingTogglesState(true);
    m_helpButton->setColour(juce::TextButton::buttonOnColourId, Accent.withAlpha(0.5f));
    m_helpButton->onClick = [this] { setPage(m_helpButton->getToggleState() ? 3 : 0); };
    addChildComponent(*m_helpButton);   // the frame's icons stand in for these three (01.10.2026)
    m_help = std::make_unique<HelpView>(m_proc, *this);
    addChildComponent(*m_help);
    // Which build this is. Asked for after a release where the answer mattered: two installers
    // carried the same version string for twenty minutes, and there was no way to tell from the
    // window which one was running.
    m_aboutButton = std::make_unique<juce::TextButton>("?");
    m_aboutButton->setTooltip("Version and what is loaded");
    m_aboutButton->onClick = [this] {
        const int packs = ambient::numPresetPacks();
        juce::String text;
        text << "Noctuary " << JucePlugin_VersionString << "\n"
             << "built " << juce::String(__DATE__).trim() << ", " << __TIME__ << "\n\n"
             << ambient::numPresets() << " presets";
        if (packs > 0) text << " (" << ambient::builtinPresetCount() << " built in, " << packs << " packs)";
        text << "\n" << ambient::numPresetClusters() << " measured groups, "
             << ambient::numPresetPhrases() << " phrases\n\n"
             << "Sample library: "
             << (m_proc.engine().displayTexture(0) != nullptr && !m_proc.engine().displayTexture(0)->empty()
                     ? "a clip is loaded" : "nothing loaded in slot 1")
             << "\n" << JucePlugin_Manufacturer << "   " << "github.com/reneweller-coding/Noctuary";
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::NoIcon, "About Noctuary", text, "Close", this);
    };
    addChildComponent(*m_aboutButton);   // About is in the settings menu (01.10.2026)
    m_undoButton = std::make_unique<juce::TextButton>("Undo");
    m_undoButton->setTooltip("Undo the last preset, die roll or A/B swap (Ctrl+Z)");
    m_undoButton->onClick = [this] { doUndo(); };
    addChildComponent(*m_undoButton);
    m_redoButton = std::make_unique<juce::TextButton>("Redo");
    m_redoButton->setTooltip("Redo (Ctrl+Y)");
    m_redoButton->onClick = [this] { doRedo(); };
    addChildComponent(*m_redoButton);
    m_abButton = std::make_unique<juce::TextButton>("A | B");
    m_abButton->setTooltip("Two whole snapshots to compare: the first click parks what you have in A and hands you B, every click after swaps");
    m_abButton->onClick = [this] { swapAB(); };
    addAndMakeVisible(*m_abButton);
    // The layout, cycled by one button: Normal (one page, tabs), Compact (the same page in
    // narrower columns, every page refitted taller) and Expanded (every page of every tab row
    // under one another, no tabs -- for a tall screen, or for reading a preset through).
    m_compactButton = std::make_unique<juce::TextButton>("Normal");
    m_compactButton->setTooltip("Layout. Normal: one page with tabs. Compact: the same page in narrower columns, every section refitted taller. Expanded: every page of every tab row laid out under one another -- no tabs, a wide page, everything in sight.");
    m_compactButton->setClickingTogglesState(false);
    m_compactButton->setColour(juce::TextButton::buttonOnColourId, Accent.withAlpha(0.5f));
    m_compactButton->onClick = [this] { applyLayoutMode((m_proc.layoutMode() + 1) % 3); };
    addChildComponent(*m_compactButton);   // the layout is in the settings menu (01.10.2026)
    // Session recall, and the switch for it. Only in the standalone: in a plugin the host saves
    // the state with the project, which is what a plugin is supposed to do.
    if (NoctuaryProcessor::sessionRecallAvailable()) {
        m_recallButton = std::make_unique<juce::TextButton>("Recall");
        m_recallButton->setTooltip("Start where you left off: the whole state is kept while the instrument runs and comes back next time. Off means it always starts at Init, and what was stored is forgotten.");
        m_recallButton->setClickingTogglesState(true);
        m_recallButton->setToggleState(m_proc.sessionRecall(), juce::dontSendNotification);
        m_recallButton->setColour(juce::TextButton::buttonOnColourId, Accent.withAlpha(0.5f));
        m_recallButton->onClick = [this] { m_proc.setSessionRecall(m_recallButton->getToggleState()); };
        addChildComponent(*m_recallButton);   // in the settings menu (01.10.2026)
    }
    // The right of the header, as every instrument of the family has it (Frame.h, 01.10.2026).
    m_undoIcon = std::make_unique<frame::IconButton>(frame::IconButton::IconE::UNDO, "Undo the last preset, die roll or A/B swap (Ctrl+Z)");
    m_redoIcon = std::make_unique<frame::IconButton>(frame::IconButton::IconE::REDO, "Redo (Ctrl+Y)");
    m_helpIcon = std::make_unique<frame::IconButton>(frame::IconButton::IconE::HELP, "The manual, by topic (F1)");
    m_settingsIcon = std::make_unique<frame::IconButton>(frame::IconButton::IconE::SETTINGS, "Settings: the layout, the session recall, the headset, the keys, About");
    m_undoIcon->onClick = [this] { doUndo(); };
    m_redoIcon->onClick = [this] { doRedo(); };
    m_helpIcon->setColour(juce::TextButton::buttonOnColourId, Accent.withAlpha(0.5f));
    m_helpIcon->onClick = [this] { setPage(m_help && m_help->isVisible() ? 0 : 3); };
    m_settingsIcon->onClick = [this] { showSettings(); };
    for (auto* b : { m_undoIcon.get(), m_redoIcon.get(), m_helpIcon.get(), m_settingsIcon.get() }) addAndMakeVisible(*b);
    m_outputView = std::make_unique<LoudnessView>(m_proc);
    addAndMakeVisible(*m_outputView);
    m_tooltips = std::make_unique<juce::TooltipWindow>(nullptr, 600);
    setWantsKeyboardFocus(true);

    // Header controls
    m_soundBox = std::make_unique<juce::ComboBox>();
    m_soundBox->setTextWhenNothingSelected("Sound preset");
    fillPresetBox(*m_soundBox);
    m_soundBox->setSelectedId(m_proc.soundPresetIndex() + 1, juce::dontSendNotification);
    m_soundBox->onChange = [this] {
        const int idx = m_soundBox->getSelectedId() - 1;
        if (idx >= 0 && idx != m_proc.soundPresetIndex()) { pushUndo("preset"); m_proc.applySoundPreset(idx); }
    };
    addAndMakeVisible(*m_soundBox);

    m_saveButton = std::make_unique<frame::IconButton>(frame::IconButton::IconE::SAVE, "Save the sound as a preset file (.noctuary)");
    m_saveButton->onClick = [this] {
        m_chooser = std::make_unique<juce::FileChooser>("Save preset", juce::File(), "*.noctuary");
        m_chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
            [this](const juce::FileChooser& fc) {
                auto file = fc.getResult();
                if (file == juce::File()) return;
                if (!file.hasFileExtension("noctuary")) file = file.withFileExtension("noctuary");
                if (!m_proc.savePresetFile(file))
                    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Preset", "Could not write the preset file.");
            });
    };
    addAndMakeVisible(*m_saveButton);
    m_loadButton = std::make_unique<frame::IconButton>(frame::IconButton::IconE::OPEN, "Load a preset file (.noctuary)");
    m_loadButton->onClick = [this] {
        m_chooser = std::make_unique<juce::FileChooser>("Load preset", juce::File(), "*.noctuary");
        m_chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& fc) {
                const auto file = fc.getResult();
                if (!file.existsAsFile()) return;
                if (!m_proc.loadPresetFile(file))
                    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Preset", "This is not an Noctuary preset file.");
                repaint();
            });
    };
    addAndMakeVisible(*m_loadButton);

    // Master gain knob lives in the header next to the Master section.
    m_master = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
    m_master->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 16);
    addAndMakeVisible(*m_master);
    m_masterAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(m_proc.m_apvts, "master_gain", *m_master);
    m_undoHook.m_editor = this;
    m_undoHook.m_names[m_master.get()] = "Master"; m_master->addMouseListener(&m_undoHook, false);

    m_mod = std::make_unique<ModView>(m_proc, *this);
    addAndMakeVisible(*m_mod);

    // Displays inside the grid: each fills the room its row leaves after the knobs.
    m_scope = std::make_unique<ScopeView>(m_proc);
    m_filterView = std::make_unique<FilterView>(m_proc);
    m_source1View = std::make_unique<SourceView>(m_proc, 1);
    m_source2View = std::make_unique<SourceView>(m_proc, 2);
    m_source3View = std::make_unique<SourceView>(m_proc, 3);
    m_source4View = std::make_unique<SourceView>(m_proc, 4);
    m_brainView = std::make_unique<BrainView>(m_proc);
    m_brainView2 = std::make_unique<BrainView>(m_proc);
    m_brainView3 = std::make_unique<BrainView>(m_proc);
    m_stageView = std::make_unique<StageView>(m_proc);
    m_cosmosView = std::make_unique<CosmosView>(m_proc);
    m_envView = std::make_unique<EnvView>(m_proc);
    m_vectorView = std::make_unique<VectorView>(m_proc);
    m_spectrumView = std::make_unique<SpectrumView>(m_proc);
    m_tuningView = std::make_unique<TuningView>(m_proc);
    m_coherenceView = std::make_unique<CoherenceView>(m_proc);
    m_stageView->m_onUndo = [this](const juce::String& what) { pushUndo(what); };
    for (juce::Component* c : { static_cast<juce::Component*>(m_scope.get()), static_cast<juce::Component*>(m_filterView.get()),
                                static_cast<juce::Component*>(m_source2View.get()), static_cast<juce::Component*>(m_source3View.get()),
                                static_cast<juce::Component*>(m_source4View.get()),
                                static_cast<juce::Component*>(m_source1View.get()), static_cast<juce::Component*>(m_brainView.get()),
                                static_cast<juce::Component*>(m_brainView2.get()),
                                static_cast<juce::Component*>(m_brainView3.get()),
                                static_cast<juce::Component*>(m_stageView.get()), static_cast<juce::Component*>(m_cosmosView.get()),
                                static_cast<juce::Component*>(m_envView.get()),
                                static_cast<juce::Component*>(m_spectrumView.get()),
                                static_cast<juce::Component*>(m_vectorView.get()),
                                static_cast<juce::Component*>(m_tuningView.get()), static_cast<juce::Component*>(m_coherenceView.get()) })
        m_content.addAndMakeVisible(*c);
    if (m_tabRows.size() > 5 && m_groups.size() > 6) {
        m_tabRows[0].m_displays = { m_source1View.get(), m_source2View.get(), m_source3View.get(), m_source4View.get() };
        // The strand scope drew the bank's cycle on the Strands tab; that tab is gone and the
        // additive-bank picture on Source 1's page shows the same partials. Expanded uses it.
        if (m_scope) m_scope->setVisible(false);
        // The filter picture draws both filters, so it is the display of both filter pages.
        m_tabRows[1].m_displays = { m_filterView.get(), m_filterView.get(), m_envView.get() };
        m_tabRows[4].m_displays = { m_cosmosView.get(), nullptr, nullptr };                                                           // COSMOS | STRIKE | MEMORY
        m_tabRows[5].m_displays = { m_brainView.get(), m_brainView3.get(), m_brainView2.get(), m_tuningView.get(), m_coherenceView.get() };   // BRAIN | AUTOPLAY | BRAIN 2 | TUNING | COHERENCE + CLOCK
        m_groups[0].m_displays = { nullptr, nullptr, m_stageView.get() };   // VOICE: the Space / Foundation row
        m_groups[1].m_displays = { m_vectorView.get() };                    // MORPH: the vector's square
        m_groups[6].m_displays = { m_spectrumView.get() };                  // ANALYSIS: the strip
    }
    // The Expanded page. Three columns: the voice's sources and its room on the left; its
    // shaping (filters, envelope, expression, z-plane, vector) with the effects, the background
    // and the small sections in the middle; morph, macros and the conductor on the right. The
    // closed sections -- feedback, room, early room, body, patina, cosmos, strike -- share one
    // row, which wraps if several of them are open; so do morph, macros and the second
    // conductor. The spectrum is the middle column's filler, small.
    m_expandedGroups = {
        // Rows without a display are flows: their sections run left to right and wrap, so a row
        // is as full as its sections allow and the closed ones sit beside the open ones. The
        // strand bank gets the bank's own scope as its display, and the stage grows to close
        // the column.
        { "VOICE",      VoiceHue,     { { "Source 1" }, { "Strands" }, { "Source 2" }, { "Source 3" }, { "Source 4" }, { "Space", "Foundation" } }, {}, 0,
                                    { m_source1View.get(), m_scope.get(), m_source2View.get(), m_source3View.get(), m_source4View.get(), m_stageView.get() }, 0, true },
        { "SHAPE",      VoiceHue,     { { "Air", "Filter" }, { "Envelope", "Expression" }, { "Z-Plane", "Vector" } }, {}, 1,
                                    { m_filterView.get(), m_envView.get(), m_vectorView.get() } },
        { "FOREGROUND", ForeHue,      { { "Ensemble", "Delay", "Delay 2", "Near Reverb", "Blur" } }, {}, 1 },
        { "BACKGROUND", BackHue,      { { "Cloud", "Far Reverb", "Feedback", "Room", "Early Room", "Body", "Patina", "Cosmos", "Strike", "Memory", "Near Source", "Near Events" } }, {}, 1 },
        { "ANALYSIS",   VoiceHue,     { {} }, {}, 1, { m_spectrumView.get() }, 80, true },
        { "MORPH",      MorphHue,     { { "Morph", "Macros", "Brain 2", "Clock" } }, {}, 2 },
        { "CONDUCTOR",  ConductorHue, { { "Cluster Brain" }, { "Autoplay" }, { "Tuning" }, { "Coherence" } }, {}, 2,
                                    { m_brainView.get(), m_brainView3.get(), m_tuningView.get(), m_coherenceView.get() }, 0, true },
    };

    // Free scaling: the corner is the zoom. The ratio is fixed so the arrangement never changes,
    // only its size, and the window opens at whatever fraction of the screen actually fits.
    setResizable(true, true);
    layoutBody();                       // measure once: the design size is what the body needs
    // Width and height: exactly what header, body and modulation strip need, so nothing scrolls.
    m_designW = std::max(MinDesignW, m_bodyW);
    m_designH = std::max(MinDesignH, HeaderH + m_bodyH + StripH);
    if (auto* con = getConstrainer()) {
        con->setFixedAspectRatio(static_cast<double>(m_designW) / static_cast<double>(m_designH));
        con->setSizeLimits(m_designW / 3, m_designH / 3, m_designW * 2, m_designH * 2);
    }
    {
        float fit = 1.0f;
        if (auto* screen = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()) {
            const auto area = screen->userArea;
            fit = juce::jlimit(0.34f, 1.0f, juce::jmin(area.getWidth() * 0.94f / m_designW,
                                                      area.getHeight() * 0.90f / m_designH));
        }
        setSize(juce::roundToInt(m_designW * fit), juce::roundToInt(m_designH * fit));
    }
    {
        int mode = m_proc.layoutMode();
        if (juce::SystemStats::getEnvironmentVariable("AMBIENT_COMPACT", "").isNotEmpty()) mode = 1;
        if (juce::SystemStats::getEnvironmentVariable("AMBIENT_EXPANDED", "").isNotEmpty()) mode = 2;
        const juce::String lm = juce::SystemStats::getEnvironmentVariable("AMBIENT_LAYOUT", "");   // 0, 1 or 2, over the recalled one
        if (lm.isNotEmpty()) mode = juce::jlimit(0, 2, lm.getIntValue());
        if (mode != 0) applyLayoutMode(mode);
        // A picture is taken at design size, so its pixels are the layout's own measurements.
        if (juce::SystemStats::getEnvironmentVariable("AMBIENT_SHOT", "").isNotEmpty()) setSize(m_designW, m_designH);
    }
    if (juce::SystemStats::getEnvironmentVariable("AMBIENT_PERFORM", "").isNotEmpty()) setPage(1);
    {   // AMBIENT_PRESET=\<name\>: open on a named preset (dev aid for photographing a modulated patch)
        const juce::String ps = juce::SystemStats::getEnvironmentVariable("AMBIENT_PRESET", "");
        for (int i = 0; ps.isNotEmpty() && i < numPresets(); ++i)
            if (ps == preset(i).m_name) { m_proc.applySoundPreset(i); if (m_soundBox) m_soundBox->setSelectedId(i + 1, juce::dontSendNotification); }
        // AMBIENT_MORPH_TO=\<name\>: a second in, start a transition to this one, so the map's
        // crossing -- the line, the travelling point, the two labels -- can be photographed while
        // it is happening. Alongside AMBIENT_SHOT, which captures ten seconds after the chord.
        const juce::String mt = juce::SystemStats::getEnvironmentVariable("AMBIENT_MORPH_TO", "");
        for (int i = 0; mt.isNotEmpty() && i < numPresets(); ++i)
            if (mt == preset(i).m_name) {
                juce::Timer::callAfterDelay(1200, [safe = juce::Component::SafePointer<NoctuaryEditor>(this), i] {
                    if (safe != nullptr) safe->m_proc.selectPreset(i, true);
                });
                break;
            }
    }   // open on the perform page
    {   // dev aids: AMBIENT_BROWSE=1|map opens the browser (map view with "map"), AMBIENT_ROUTE=<route preset> preloads a route
        const juce::String br = juce::SystemStats::getEnvironmentVariable("AMBIENT_BROWSE", "");
        if (br.isNotEmpty()) { setPage(2); if (br == "map") m_browse->setMode(1); }
        const int hv = juce::SystemStats::getEnvironmentVariable("AMBIENT_HELP", "0").getIntValue();   // AMBIENT_HELP=<topic+1>: open the manual there
        if (hv > 0) { setPage(3); if (m_help) m_help->m_topics.selectRow(hv - 1); }
        const juce::String sc = juce::SystemStats::getEnvironmentVariable("AMBIENT_SCROLL", "");
        if (sc.isNotEmpty()) juce::MessageManager::callAsync([this, y = sc.getIntValue()] { m_viewport.setViewPosition(0, y); });
        // AMBIENT_TAB=\<row\>,\<page\>[;\<row\>,\<page\>...]: open tabbed rows on a given page, so a page
        // that is not the first one can be photographed at all. Rows count from the top, pages
        // from the left, both from zero.
        for (const auto& one : juce::StringArray::fromTokens(juce::SystemStats::getEnvironmentVariable("AMBIENT_TAB", ""), ";", "")) {
            const auto rc = juce::StringArray::fromTokens(one, ",", "");
            const int r = rc.size() == 2 ? rc[0].getIntValue() : -1, pg = rc.size() == 2 ? rc[1].getIntValue() : -1;
            if (r >= 0 && r < static_cast<int>(m_tabRows.size()) && pg >= 0 && pg < static_cast<int>(m_tabRows[static_cast<size_t>(r)].m_names.size()))
                m_tabRows[static_cast<size_t>(r)].m_active = pg;
        }
        // AMBIENT_MANUAL=\<folder\>: write the manual out and quit. A chord is played first and
        // then twelve seconds pass, because the pictures are of the running instrument and its
        // displays -- the spectrum, the stage, the note roll -- have nothing in them until it is
        // actually sounding. Waiting alone was not enough: with a slow conductor the first draft
        // illustrated the Sources chapter with a display that said "nothing sounding".
        const juce::String man = juce::SystemStats::getEnvironmentVariable("AMBIENT_MANUAL", "");
        if (man.isNotEmpty()) {
            // The manual's pictures are of the tabbed page. Session recall keeps the layout mode,
            // and one export ran Expanded because the run before it had been.
            applyLayoutMode(0);
            for (int n : { 45, 52, 57, 64, 69 }) m_proc.engine().noteOn(n, 0.7f);
            juce::Timer::callAfterDelay(12000, [this, man] {
                exportManual(juce::File(man), [] { juce::JUCEApplicationBase::quit(); });
            });
        }
        // AMBIENT_WAVETABLE=\<file\>: a table file into the user slot, for a picture of a table the
        // built-ins cannot show -- they have four or five frames, a Serum bank has up to 256.
        {
            const juce::String wtf = juce::SystemStats::getEnvironmentVariable("AMBIENT_WAVETABLE", "");
            if (wtf.isNotEmpty()) m_proc.loadWavetableFile(juce::File(wtf));
        }
        // AMBIENT_SET=key=value;key=value and AMBIENT_MATRIX=\<routes\>: parameters and routes for a
        // run, on top of the preset -- so a picture can show a state no preset has.
        {
            for (const auto& kv : juce::StringArray::fromTokens(juce::SystemStats::getEnvironmentVariable("AMBIENT_SET", ""), ";", "")) {
                const int eq = kv.indexOfChar('=');
                if (eq <= 0) continue;
                if (auto* p = m_proc.m_apvts.getParameter(kv.substring(0, eq).trim())) {
                    if (auto* d = ambient::findParam(kv.substring(0, eq).trim().toRawUTF8()))
                        p->setValueNotifyingHost(p->convertTo0to1(ambient::paramValueFromText(*d, kv.substring(eq + 1).trim().toRawUTF8())));
                }
            }
            const juce::String mx = juce::SystemStats::getEnvironmentVariable("AMBIENT_MATRIX", "");
            if (mx.isNotEmpty()) m_proc.engine().setModMatrixText(mx.toRawUTF8());
        }
        // AMBIENT_SHOT=\<file.png\>: a picture of the whole editor as it stands -- the layout mode,
        // the open tabs, the live displays -- ten seconds after a chord, then quit. The dev aid for
        // looking at the panel without a screen grab, which takes whatever else is on the screen.
        const juce::String shot = juce::SystemStats::getEnvironmentVariable("AMBIENT_SHOT", "");
        if (shot.isNotEmpty() && man.isEmpty()) {
            for (int n : { 45, 52, 57, 64, 69 }) m_proc.engine().noteOn(n, 0.7f);
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
            if (m_mod != nullptr && md.isNotEmpty()) {
                m_mod->setTab(md == "matrix" ? 2 : ((md == "env" || md == "srcenv") ? 1 : 0));
                if (md == "srcenv") m_mod->setEnvPage(1);
            }
        }
        const juce::String rt = juce::SystemStats::getEnvironmentVariable("AMBIENT_ROUTE", "");
        for (int r = 0; rt.isNotEmpty() && r < numRoutePresets(); ++r) if (rt == routePreset(r).m_name) m_proc.setRouteText(routePreset(r).m_points);
        rebuildLayout();
    }
    // The family's ways around the panel (15.10.2026, Frame.h): the search, the night, the preset held, the journeys
    // over time, the star beside the Sound box; the palette, the first steps and the timeline over everything.
    m_searchIcon = std::make_unique<frame::IconButton>(frame::IconButton::IconE::SEARCH, "Search (Ctrl+K): any knob, preset, page or action by a few letters");
    m_searchIcon->onClick = [this] { openPalette(); };
    m_nightIcon = std::make_unique<frame::IconButton>(frame::IconButton::IconE::NIGHT, "Night: the window dim and warm for a dark room");
    m_nightIcon->onClick = [this] { setNight(!m_night); };
    m_holdIcon = std::make_unique<frame::IconButton>(frame::IconButton::IconE::COMPARE, "Hold to hear the preset as it was loaded; let go for your sound");
    m_holdIcon->onStateChange = [this] { if (m_holdIcon->isDown() != m_holding) holdPreset(m_holdIcon->isDown()); };
    m_journeyButton = std::make_unique<juce::TextButton>("Journey");
    m_journeyButton->setTooltip("The journeys over time: the one chosen or under way, step by step (a click goes there), and the one being written, to change by hand");
    m_journeyButton->onClick = [this] { if (m_journeyPanel != nullptr) m_journeyPanel->open(); };
    m_star = std::make_unique<juce::Label>();
    m_star->setColour(juce::Label::textColourId, ui::Accent);
    m_star->setFont(juce::FontOptions(16.0f, juce::Font::bold));
    m_star->setJustificationType(juce::Justification::centred);
    m_star->setTooltip("The knobs have moved from the preset (Back: choose it again; the disk saves the sound)");
    for (juce::Component* c : { static_cast<juce::Component*>(m_searchIcon.get()), static_cast<juce::Component*>(m_nightIcon.get()),
                                static_cast<juce::Component*>(m_holdIcon.get()), static_cast<juce::Component*>(m_journeyButton.get()),
                                static_cast<juce::Component*>(m_star.get()) })
        addAndMakeVisible(c);
    m_veil.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(m_veil);
    m_palette = std::make_unique<frame::CommandPalette>(ui::skin());
    m_tour = std::make_unique<frame::Tour>(ui::skin());
    m_tour->m_onDone = [] { frame::Settings::of("Noctuary").setValue("tourDone", "1"); };
    m_journeyPanel = std::make_unique<JourneyPanel>(m_proc, [this](ambient::Journey& j) { return chosenJourney(j); }, [this](int step) { startJourneyAt(step); });
    for (juce::Component* c : { static_cast<juce::Component*>(m_palette.get()), static_cast<juce::Component*>(m_tour.get()),
                                static_cast<juce::Component*>(m_journeyPanel.get()) })
        addChildComponent(c);
    {   // A knob's menu: its routes, each with its depth to drag and a button to remove it, and every source to add.
        frame::ModAccess a;
        a.m_rowsOn = [this](int id) {
            std::vector<frame::ModAccess::Row> rows;
            const ambient::ModMatrix& m = m_proc.engine().modMatrix();
            for (int k = 0; k < m.count(); ++k)
                if (static_cast<int>(m.route(k).m_target) == id) {
                    const juce::String n = prettySource(m.route(k).m_source);
                    rows.push_back({ k, n, frame::detail::sourceColour(n, ui::skin()), m.route(k).m_depth });
                }
            return rows;
        };
        a.m_setDepth = [this](int row, float d) { setRouteDepth(row, d); };
        a.m_remove = [this](int row) { removeRoute(row); };
        a.m_sources = [](int) {
            std::vector<frame::ModAccess::Source> out;
            for (int k = 1; k < ambient::NumModSources; ++k) {
                const auto s = static_cast<ambient::ModSourceE>(k);
                out.push_back({ k, prettySource(s), sourceSection(juce::String(ambient::modSourceName(s))) });
            }
            static const juce::StringArray order{ "LFOs", "Envelopes", "Macros", "The note and the hands", "Fields and chaos", "The piece and what it hears" };
            std::stable_sort(out.begin(), out.end(), [](const frame::ModAccess::Source& x, const frame::ModAccess::Source& y) {
                return order.indexOf(x.m_section) < order.indexOf(y.m_section);
            });
            return out;
        };
        a.m_add = [this](int id, int src) { return m_mod != nullptr && m_mod->addRoute(static_cast<ambient::ModSourceE>(src), static_cast<ParamIdE>(id)); };
        m_modulation = std::make_unique<frame::ModulationAtKnob>(std::move(a));
    }
    // Every knob, switch and chooser says which parameter it is: the search and the shared parts find it (Frame.h).
    for (auto& c : m_cells)
        if (c.m_param >= 0 && c.m_comp != nullptr) c.m_comp->getProperties().set("frameId", c.m_param);
    setNight(frame::Settings::of("Noctuary").value("night") == "1");
    resized();
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
        if (d.m_id == ParamIdE::MASTER_GAIN) continue;   // header knob
        Section* sec = findSection(d.m_section);
        if (sec == nullptr) {
            Section s; s.m_name = d.m_section;
            // The widths in Expanded's flows, where a section's shape is its own: mostly one long
            // row, so the closed sections stand beside the open ones on one line.
            if (s.m_name == "Tuning") s.m_maxUnits = 9;
            if (s.m_name == "Cosmos") s.m_maxUnits = 8;                          // two even rows
            if (s.m_name == "Morph") s.m_maxUnits = 9;
            if (s.m_name == "Macros") s.m_maxUnits = 10;                         // one row: the eight macros, Air, Inertia
            if (s.m_name == "Space") s.m_maxUnits = 10;                          // two rows of ten
            if (s.m_name == "Foundation") s.m_maxUnits = 5;
            if (s.m_name == "Early Room") s.m_maxUnits = 4;                      // one row
            if (s.m_name == "Source 1") s.m_maxUnits = 9;
            else if (s.m_name == "Source 2" || s.m_name == "Source 3" || s.m_name == "Source 4") s.m_maxUnits = 12;
            if (s.m_name == "Strands") s.m_maxUnits = 10;
            if (s.m_name == "Delay" || s.m_name == "Delay 2") s.m_maxUnits = 12;   // one row with the two Sync choices and Absorb
            if (s.m_name == "Far Reverb") s.m_maxUnits = 12;                     // one row with Rotate, Unmask, Diffuse and Width
            if (s.m_name == "Master") s.m_maxUnits = 8;                          // the header's row
            if (s.m_name == "Body") s.m_maxUnits = 7;                            // one row
            if (s.m_name == "Room") s.m_maxUnits = 9;                            // one row with Morph and both impulses
            if (s.m_name == "Cluster Brain" || s.m_name == "Brain 2") s.m_maxUnits = 10;
            if (s.m_name == "Autoplay") s.m_maxUnits = 12;   // the seven controls and the button in one row
            if (s.m_name == "Expression") s.m_maxUnits = 7;
            if (s.m_name == "Filter") s.m_maxUnits = 10;                         // one row: On, Model, five knobs, Drive, Fold, Morph
            if (s.m_name == "Cloud") s.m_maxUnits = 9;                           // two rows: the grains, then the loop and the resonators
            if (s.m_name == "Memory") s.m_maxUnits = 9;                          // two rows: the lines, then the recall and the tape
            if (s.m_name == "Z-Plane") s.m_maxUnits = 11;
            s.m_flowUnits = s.m_maxUnits;
            // The natural widths on the tabbed page. There they decide one thing only: how wide
            // each column is, which is the widest page at these widths (Source 1 with the strand
            // bank and its picture on the left, the cluster brain with its roll on the right).
            // Every page is then fitted into that column (layoutTabbed), so a section's rows on
            // the screen are whatever its page needs, not these.
            if (s.m_name == "Cosmos") s.m_maxUnits = 9;
            if (s.m_name == "Morph") s.m_maxUnits = 5;
            if (s.m_name == "Macros") s.m_maxUnits = 5;
            if (s.m_name == "Vector") s.m_maxUnits = 3;
            if (s.m_name == "Strands") s.m_maxUnits = 6;                         // two rows beside Source 1
            if (s.m_name == "Delay" || s.m_name == "Delay 2") s.m_maxUnits = 7;    // two rows, the second with Duck
            if (s.m_name == "Far Reverb") s.m_maxUnits = 9;                      // two rows
            if (s.m_name == "Body") s.m_maxUnits = 4;
            if (s.m_name == "Room") s.m_maxUnits = 6;
            if (s.m_name == "Cloud") s.m_maxUnits = 4;
            if (s.m_name == "Memory") s.m_maxUnits = 9;
            if (s.m_name == "Clock") s.m_maxUnits = 2;
            for (int gi = 0; gi < static_cast<int>(m_groups.size()); ++gi)
                for (auto& row : m_groups[static_cast<size_t>(gi)].m_rows)
                    for (auto& n : row) if (n == s.m_name) s.m_group = gi;
            m_sections.push_back(std::move(s));
            sec = &m_sections.back();
        }
        Cell c;
        c.m_param = static_cast<int>(d.m_id);
        c.m_baseLabel = d.m_name;
        c.m_label = std::make_unique<juce::Label>(juce::String(), d.m_name);
        c.m_label->setJustificationType(juce::Justification::centred);
        c.m_label->setFont(juce::FontOptions(11.0f));
        c.m_label->setColour(juce::Label::textColourId, Dim);
        m_content.addAndMakeVisible(*c.m_label);
        switch (d.m_kind) {
        case ParamKindE::FLOAT:
        case ParamKindE::INT: {
            auto s = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
            if (d.m_unit[0] != 0) s->setTextValueSuffix(juce::String(" ") + d.m_unit);
            // A parameter that spans zero reads much better as an arc growing out of the centre.
            if (d.m_min < -1.0e-6f && d.m_max > 1.0e-6f) s->getProperties().set("bipolar", true);
            m_content.addAndMakeVisible(*s);
            c.m_slider = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(m_proc.m_apvts, d.m_key, *s);
            m_undoHook.m_names[s.get()] = d.m_name; s->addMouseListener(&m_undoHook, false);
            c.m_comp = std::move(s);
            break;
        }
        case ParamKindE::BOOL: {
            auto b = std::make_unique<juce::ToggleButton>();
            m_content.addAndMakeVisible(*b);
            c.m_button = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(m_proc.m_apvts, d.m_key, *b);
            m_undoHook.m_names[b.get()] = d.m_name; b->addMouseListener(&m_undoHook, false);
            c.m_comp = std::move(b);
            break;
        }
        case ParamKindE::CHOICE: {
            auto cb = std::make_unique<juce::ComboBox>();
            if (d.m_id == ParamIdE::Z_SHAPE) {
                // A hundred and fifty-five filters in one flat list is a list nobody reads. The
                // item IDs stay the parameter's own numbering -- which is historical and frozen,
                // because the library names its shapes by text -- while the order they are shown
                // in is by family, so the display and the value are free of each other.
                for (int cat = 0; cat < ambient::ZCategories; ++cat) {
                    cb->addSectionHeading(ambient::ZCategoryNames[cat]);
                    for (int i = 0; i < d.m_numChoices; ++i)
                        if (ambient::ZShapeCategory[i] == cat) cb->addItem(d.m_choices[i], i + 1);
                }
            } else {
                for (int i = 0; i < d.m_numChoices; ++i) cb->addItem(d.m_choices[i], i + 1);
            }
            m_content.addAndMakeVisible(*cb);
            c.m_combo = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(m_proc.m_apvts, d.m_key, *cb);
            m_undoHook.m_names[cb.get()] = d.m_name; cb->addMouseListener(&m_undoHook, false);
            c.m_comp = std::move(cb);
            c.m_units = 2;
            break;
        }
        }
        c.m_comp->addMouseListener(this, false);
        registerHelp(c.m_comp.get(), d.m_id);
        if (auto* tc = dynamic_cast<juce::SettableTooltipClient*>(c.m_comp.get())) tc->setTooltip(ambient::paramHelp(d.m_id));
        m_cellOf[c.m_comp.get()] = static_cast<int>(m_cells.size());
        sec->m_cells.push_back(static_cast<int>(m_cells.size()));
        m_cells.push_back(std::move(c));
    }

    // Extra cells: Scala loader, source files, morph slot pickers and capture buttons.
    auto scala = std::make_unique<juce::TextButton>("Load Scala...");
    scala->onClick = [this] { chooseScalaFile(); };
    addExtraCell("Tuning", std::move(scala), "User scale", 2);
    auto table = std::make_unique<juce::TextButton>("Wavetable...");
    table->onClick = [this] { chooseSourceFile(true); };
    m_tableCell = addExtraCell("Source 2", std::move(table), "User table", 2);
    // A clip loader in every source section, each for its own slot: four slots typed Texture
    // can play four different recordings, which is what the Vector's four corners are for.
    for (int k = 0; k < ambient::Slots; ++k) {
        auto texture = std::make_unique<juce::TextButton>("Texture...");
        texture->onClick = [this, k] { chooseSourceFile(false, k); };
        m_textureCell[k] = addExtraCell("Source " + juce::String(k + 1), std::move(texture), "Texture file", 2);
    }
    // Autoplay's trigger as a button of its own: the parameter is a switch a host can automate,
    // but by hand you want one press, not a switch you have to put back.
    auto step = std::make_unique<juce::TextButton>("Step now");
    step->setTooltip("Exchange one voice of the cluster now, whatever the interval says");
    step->onClick = [this] { m_proc.engine().autoplayStep(); };
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
                if (cosmosPresetCategory(i) == fam) cb->addItem(cosmosPreset(i).m_name, i + 1);
        }
        cb->addSectionHeading("Off");
        cb->addItem(cosmosPreset(0).m_name, 1);
        cb->setSelectedId(m_proc.cosmosPresetIndex() + 1, juce::dontSendNotification);
        cb->onChange = [this] {
            const int idx = m_cosmosBox->getSelectedId() - 1;
            if (idx >= 0 && idx != m_proc.cosmosPresetIndex()) { pushUndo("cosmos preset"); m_proc.applyCosmosPreset(idx); }
        };
        m_cosmosBox = cb.get();
        addExtraCell("Cosmos", std::move(cb), "Preset", 3);
    }
    // The Z-plane and Strike banks. Each is a layer like the Cosmos one -- it resets its own
    // section and nothing else -- and each sits inside the section it belongs to, grouped by
    // family, because a flat list of a hundred and fifty-six filters is a list nobody reads.
    {
        auto zb = std::make_unique<juce::ComboBox>();
        zb->setTextWhenNothingSelected("Filter preset");
        for (int cat = 0; cat < ambient::ZCategories; ++cat) {
            zb->addSectionHeading(ambient::ZCategoryNames[cat]);
            for (int i = 0; i < numZPresets(); ++i)
                if (zPresetCategory(i) == cat) zb->addItem(zPreset(i).m_name, i + 1);
        }
        zb->addSectionHeading("Off");
        zb->addItem(zPreset(0).m_name, 1);
        zb->setTooltip("One preset per filter shape: the shape, where its point sits in the cube, how sharp and how much of it you hear. Touches nothing outside the Z-Plane section.");
        zb->onChange = [this] {
            const int idx = m_zPresetBox->getSelectedId() - 1;
            if (idx >= 0) { pushUndo("filter preset"); m_proc.applyZPreset(idx); }
        };
        m_zPresetBox = zb.get();
        addExtraCell("Z-Plane", std::move(zb), "Preset", 3);
    }
    {
        auto sb = std::make_unique<juce::ComboBox>();
        sb->setTextWhenNothingSelected("Strike preset");
        for (int fam = 0; fam < numStrikePresetFamilies(); ++fam) {
            sb->addSectionHeading(strikePresetFamily(fam));
            for (int i = 0; i < numStrikePresets(); ++i)
                if (strikePresetCategory(i) == fam) sb->addItem(strikePreset(i).m_name, i + 1);
        }
        sb->addSectionHeading("Off");
        sb->addItem(strikePreset(0).m_name, 1);
        sb->setTooltip("The Karplus-Strong pluck on its own: what is struck, how long it rings, how dull, and whether the conductor fires it too. Touches nothing outside the Strike section.");
        sb->onChange = [this] {
            const int idx = m_strikePresetBox->getSelectedId() - 1;
            if (idx >= 0) { pushUndo("strike preset"); m_proc.applyStrikePreset(idx); }
        };
        m_strikePresetBox = sb.get();
        addExtraCell("Strike", std::move(sb), "Preset", 3);
    }
    {
        // The near layer's bank: a foreground for the night, kept while the backgrounds change.
        auto nb = std::make_unique<juce::ComboBox>();
        nb->setTextWhenNothingSelected("Near preset");
        for (int fam = 0; fam < numNearPresetFamilies(); ++fam) {
            nb->addSectionHeading(nearPresetFamily(fam));
            for (int i = 0; i < numNearPresets(); ++i)
                if (nearPresetCategory(i) == fam) nb->addItem(nearPreset(i).m_name, i + 1);
        }
        nb->addSectionHeading("Off");
        nb->addItem(nearPreset(0).m_name, 1);
        nb->setTooltip("The foreground on its own: what is played close to the ear, how it is shaped, and how often. A layer like the Cosmos -- it stays while sound presets change under it -- and it touches nothing outside the two Near sections.");
        nb->onChange = [this] {
            const int idx = m_nearPresetBox->getSelectedId() - 1;
            if (idx >= 0) {
                pushUndo("near preset");
                // A foreground chosen by hand is pinned: Auto off, so the next sound preset does not
                // bring its own over it. Auto on again draws for the preset that is playing.
                m_proc.setNearAuto(false);
                m_autoSeen = false;
                m_proc.applyNearPreset(idx);
            }
        };
        m_nearPresetBox = nb.get();
        addExtraCell("Near Events", std::move(nb), "Preset", 3);
        // The near source's own clip: what its Clip, Texture, Stretch and Spectral types read.
        auto clip = std::make_unique<juce::TextButton>("Clip...");
        clip->setTooltip("A recording for the near source -- a near preset brings its own from the library's archive (a single one, or a folder of phrases, one of them at random per event); this opens any file. With none, the near source reads Source 4's clip.");
        clip->onClick = [this] { chooseNearClipFile(); };
        m_nearClipCell = addExtraCell("Near Source", std::move(clip), "Source 4's clip", 2);
    }
    auto impulse = std::make_unique<juce::TextButton>("Impulse A...");
    impulse->onClick = [this] { chooseImpulseFile(false); };
    m_impulseCell = addExtraCell("Room", std::move(impulse), "Dark Hall (built in)", 2);
    auto impulseB = std::make_unique<juce::TextButton>("Impulse B...");
    impulseB->onClick = [this] { chooseImpulseFile(true); };
    m_impulseBCell = addExtraCell("Room", std::move(impulseB), "no second room", 2);
    for (auto& s : m_sections) s.m_wideUnits = s.m_maxUnits;   // the natural width, which every layout starts from

    auto boxA = std::make_unique<juce::ComboBox>();
    boxA->setTextWhenNothingSelected("A: preset");
    fillPresetBox(*boxA);
    m_morphABox = boxA.get();
    boxA->onChange = [this] { if (m_morphABox->getSelectedId() > 0) m_proc.setMorphSlotFromPreset(0, m_morphABox->getSelectedId() - 1); };
    addExtraCell("Morph", std::move(boxA), "A", 2);
    auto boxB = std::make_unique<juce::ComboBox>();
    boxB->setTextWhenNothingSelected("B: preset");
    fillPresetBox(*boxB);
    m_morphBBox = boxB.get();
    boxB->onChange = [this] { if (m_morphBBox->getSelectedId() > 0) m_proc.setMorphSlotFromPreset(1, m_morphBBox->getSelectedId() - 1); };
    addExtraCell("Morph", std::move(boxB), "B", 2);
    auto setA = std::make_unique<juce::TextButton>("A <- now");
    setA->onClick = [this] { m_proc.setMorphSlotFromCurrent(0); m_morphABox->setSelectedId(0, juce::dontSendNotification); };
    addExtraCell("Morph", std::move(setA), "capture", 1);
    auto setB = std::make_unique<juce::TextButton>("B <- now");
    setB->onClick = [this] { m_proc.setMorphSlotFromCurrent(1); m_morphBBox->setSelectedId(0, juce::dontSendNotification); };
    addExtraCell("Morph", std::move(setB), "capture", 1);
    {
        // Journeys (13.09.2026): presets in a row with dwell and fade ranges, cyclic for an evening.
        // The templates lie beside the library, the player's own in Documents/Noctuary/Journeys.
        auto jb = std::make_unique<juce::ComboBox>();
        jb->setTextWhenNothingSelected("Journey");
        jb->setTooltip("A journey: presets in a row, each held for a while drawn from its range and crossfaded into the next over a drawn fade, round and round when it is cyclic. Choosing one starts it. The files are plain text (*.journey): one preset a line with its dwell and fade, editable by hand.");
        m_journeyBox = jb.get();
        fillJourneyBox();
        jb->onChange = [this] {
            const int id = m_journeyBox->getSelectedId();
            if (id <= 0 || id > m_journeyFiles.size()) return;
            if (!m_proc.startJourney(m_journeyFiles[id - 1]))
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Journey", "This journey could not be read (a malformed line, or no steps).");
        };
        addExtraCell("Morph", std::move(jb), "Journey", 3);
        auto stop = std::make_unique<frame::IconButton>(frame::IconButton::IconE::STOP, "Stops the journey; the preset playing stays.");
        stop->onClick = [this] { m_proc.stopJourney(); m_journeyBox->setSelectedId(0, juce::dontSendNotification); };
        addExtraCell("Morph", std::move(stop), "journey", 1);
        auto add = std::make_unique<juce::TextButton>("+ now");
        add->setTooltip("Appends the sound preset that is playing to the journey being written (5-10 minutes, a fade of 30-90 seconds; edit the file for other ranges). The disk beside it writes it.");
        add->onClick = [this] { m_proc.journeyAddCurrent(300.0, 600.0, 30.0, 90.0); };
        addExtraCell("Morph", std::move(add), "write", 1);
        auto save = std::make_unique<frame::IconButton>(frame::IconButton::IconE::SAVE, "Writes the journey being written (the presets added with + now) into Documents/Noctuary/Journeys under a name, and offers it in the box.");
        save->onClick = [this] { saveJourneyAs(); };
        addExtraCell("Morph", std::move(save), "write", 1);
        auto status = std::make_unique<juce::Label>();
        status->setJustificationType(juce::Justification::centredLeft);
        status->setMinimumHorizontalScale(0.7f);
        m_journeyStatus = status.get();
        addExtraCell("Morph", std::move(status), "step", 2);
    }
}

void NoctuaryEditor::fillJourneyBox()
{
    if (m_journeyBox == nullptr) return;
    m_journeyFiles = NoctuaryProcessor::journeyFiles();
    m_journeyBox->clear(juce::dontSendNotification);
    for (int i = 0; i < m_journeyFiles.size(); ++i) m_journeyBox->addItem(m_journeyFiles[i].getFileNameWithoutExtension(), i + 1);
}

void NoctuaryEditor::saveJourneyAs()
{
    auto* w = new juce::AlertWindow("Save journey", "A name for the journey being written (" + juce::String(static_cast<int>(m_proc.journey().m_steps.size())) + " steps):", juce::MessageBoxIconType::NoIcon);
    w->addTextEditor("name", m_proc.journey().m_name.empty() ? "My Journey" : juce::String(m_proc.journey().m_name), "Name");
    w->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([this, w](int result) {
        std::unique_ptr<juce::AlertWindow> owner(w);
        if (result != 1) return;
        const juce::String name = juce::File::createLegalFileName(w->getTextEditorContents("name").trim());
        if (name.isEmpty()) return;
        const juce::File file = NoctuaryProcessor::userJourneyFolder().getChildFile(name + ".journey");
        if (!m_proc.saveJourney(file))
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Journey", "Nothing to save: add presets with + now first.");
        else fillJourneyBox();
    }), false);
}

/**
 * Which parameter sits under a screen point: the drop target for a dragged modulation source.
 */
int NoctuaryEditor::cellParamAt(juce::Point<int> screenPos) const
{
    for (const auto& c : m_cells) {
        if (c.m_param < 0 || c.m_comp == nullptr || !c.m_comp->isShowing()) continue;
        if (c.m_comp->getScreenBounds().contains(screenPos)) return c.m_param;
    }
    return -1;
}

juce::Rectangle<int> NoctuaryEditor::cellScreenBounds(int cellIndex) const
{
    if (cellIndex < 0 || cellIndex >= static_cast<int>(m_cells.size())) return {};
    const Cell& c = m_cells[static_cast<size_t>(cellIndex)];
    return c.m_comp != nullptr ? c.m_comp->getScreenBounds() : juce::Rectangle<int>();
}

void NoctuaryEditor::colourCellsByGroup()
{
    for (const auto& sec : m_sections) {
        const juce::Colour col = sec.m_group >= 0 ? m_groups[static_cast<size_t>(sec.m_group)].m_colour : MasterHue;
        for (int ci : sec.m_cells) {
            Cell& c = m_cells[static_cast<size_t>(ci)];
            if (auto* sl = dynamic_cast<juce::Slider*>(c.m_comp.get())) sl->setColour(juce::Slider::rotarySliderFillColourId, col);
            else if (auto* tb = dynamic_cast<juce::ToggleButton*>(c.m_comp.get())) tb->setColour(juce::ToggleButton::tickColourId, col);
        }
    }
}

int NoctuaryEditor::addExtraCell(const juce::String& section, std::unique_ptr<juce::Component> comp, const juce::String& label, int units)
{
    Section* sec = findSection(section);
    if (sec == nullptr) return -1;
    Cell c;
    c.m_units = units;
    c.m_baseLabel = label;
    c.m_label = std::make_unique<juce::Label>(juce::String(), label);
    c.m_label->setJustificationType(juce::Justification::centred);
    c.m_label->setFont(juce::FontOptions(11.0f));
    c.m_label->setColour(juce::Label::textColourId, Dim);
    m_content.addAndMakeVisible(*c.m_label);
    m_content.addAndMakeVisible(*comp);
    c.m_comp = std::move(comp);
    const int idx = static_cast<int>(m_cells.size());
    sec->m_cells.push_back(idx);
    m_cells.push_back(std::move(c));
    return idx;
}

NoctuaryEditor::Section* NoctuaryEditor::findSection(const juce::String& name)
{
    for (auto& s : m_sections) if (s.m_name == name) return &s;
    return nullptr;
}

// ---------------------------------------------------------------- layout

namespace {
/** @brief The switches that close a section, and the cells it keeps while closed. */
struct Closer {
    const char* m_section;                  ///< the section's name
    ambient::ParamIdE m_sw;                  ///< the switch: a type of Off, a level at zero, an Active that is not
    std::vector<ambient::ParamIdE> m_keep;   ///< the cells still shown while it is closed
};
/**
 * @brief The closer table, built on first use.
 * @return one entry per section that closes; a section not in it is always open
 */
const std::vector<Closer>& closers()
{
    using ambient::ParamIdE;
    static const std::vector<Closer> k = {
        { "Source 1",   ParamIdE::SRC1_TYPE,     { ParamIdE::SRC1_TYPE, ParamIdE::OSC_LEVEL } },
        { "Source 2",   ParamIdE::SRC2_TYPE,     { ParamIdE::SRC2_TYPE, ParamIdE::SRC2_LEVEL } },
        { "Source 3",   ParamIdE::SRC3_TYPE,     { ParamIdE::SRC3_TYPE, ParamIdE::SRC3_LEVEL } },
        { "Source 4",   ParamIdE::SRC4_TYPE,     { ParamIdE::SRC4_TYPE, ParamIdE::SRC4_LEVEL } },
        { "Z-Plane",    ParamIdE::Z_MODE,        { ParamIdE::Z_MODE } },
        { "Cosmos",     ParamIdE::COSMOS_SEND,   { ParamIdE::COSMOS_SEND, ParamIdE::COSMOS_RETURN } },
        { "Memory",     ParamIdE::MEM_SEND,      { ParamIdE::MEM_SEND, ParamIdE::MEM_RETURN } },
        { "Strike",     ParamIdE::STRIKE_LEVEL,  { ParamIdE::STRIKE_LEVEL, ParamIdE::STRIKE_TYPE } },
        // No entry for Morph any more (13.09.2026). A closed section keeps only its switch, and the
        // Journey row lives in this one -- so the way to a journey was to switch Morph on, which held
        // the whole instrument at the snapshots. The section stays open; it fits in every layout.
        { "Brain 2",    ParamIdE::BRAIN2_ON,     { ParamIdE::BRAIN2_ON } },
        { "Early Room", ParamIdE::EARLY_LEVEL,   { ParamIdE::EARLY_LEVEL } },
        { "Body",       ParamIdE::BODY_LEVEL,    { ParamIdE::BODY_LEVEL } },
        { "Room",       ParamIdE::ROOM_LEVEL,    { ParamIdE::ROOM_LEVEL } },
        { "Cloud",      ParamIdE::CLOUD_SEND,    { ParamIdE::CLOUD_SEND } },
        { "Feedback",   ParamIdE::FEEDBACK_BUS,  { ParamIdE::FEEDBACK_BUS, ParamIdE::FEEDBACK_FM } },
        { "Patina",     ParamIdE::PATINA_AMOUNT, { ParamIdE::PATINA_AMOUNT } },
    };
    return k;
}
/**
 * @brief The closer of a section.
 * @param[in] section  the section's name
 * @return its entry, or null if the section never closes
 */
const Closer* closerFor(const juce::String& section)
{
    for (const auto& c : closers()) if (section == c.m_section) return &c;
    return nullptr;
}
}

bool NoctuaryEditor::sectionCollapsed(const Section& s) const
{
    if (m_openAll) return false;
    const Closer* c = closerFor(s.m_name);
    if (c == nullptr) return false;
    auto off = [this](ParamIdE id) {
        const ParamDesc& d = paramDesc(id);
        const float v = m_proc.engine().getParam(id);
        if (d.m_kind == ParamKindE::CHOICE) return std::lround(v) == 0;
        if (d.m_kind == ParamKindE::BOOL) return v < 0.5f;
        return v <= d.m_min + 1.0e-6f;
    };
    if (!off(c->m_sw)) return false;
    if (s.m_name == "Feedback" && !off(ParamIdE::FEEDBACK_FM)) return false;   // either path keeps the loop open
    return true;
}

bool NoctuaryEditor::cellShown(const Section& s, const Cell& c) const
{
    if (c.m_unused) return false;
    if (!s.closedNow()) return true;
    const Closer* k = closerFor(s.m_name);
    if (k == nullptr || c.m_param < 0) return false;
    for (ParamIdE id : k->m_keep) if (static_cast<int>(id) == c.m_param) return true;
    return false;
}

int NoctuaryEditor::shownCells(const Section& s) const
{
    int n = 0;
    for (int ci : s.m_cells) if (cellShown(s, m_cells[static_cast<size_t>(ci)])) ++n;
    return n;
}

int NoctuaryEditor::sectionWidth(const Section& s) const
{
    int widest = 0, row = 0;
    for (int ci : s.m_cells) {
        if (!cellShown(s, m_cells[static_cast<size_t>(ci)])) continue;
        const int u = m_cells[static_cast<size_t>(ci)].m_units;
        if (row + u > s.m_maxUnits) row = 0;
        row += u; widest = std::max(widest, row);
    }
    return widest * CellW + 2 * Pad;
}

int NoctuaryEditor::sectionHeight(const Section& s) const
{
    int rows = 1, row = 0;
    for (int ci : s.m_cells) {
        if (!cellShown(s, m_cells[static_cast<size_t>(ci)])) continue;
        const int u = m_cells[static_cast<size_t>(ci)].m_units;
        if (row + u > s.m_maxUnits) { ++rows; row = 0; }
        row += u;
    }
    return rows * CellH + TitleH + Pad;
}

void NoctuaryEditor::layoutSection(Section& s, int x, int y)
{
    s.m_bounds = { x, y, sectionWidth(s), sectionHeight(s) };
    int cx = x + Pad, cy = y + TitleH, row = 0;
    for (int ci : s.m_cells) {
        Cell& c = m_cells[static_cast<size_t>(ci)];
        if (!cellShown(s, c)) { c.m_comp->setVisible(false); c.m_label->setVisible(false); continue; }
        if (row + c.m_units > s.m_maxUnits) { row = 0; cx = x + Pad; cy += CellH; }
        juce::Rectangle<int> cell(cx, cy, c.m_units * CellW, CellH);
        c.m_label->setBounds(cell.removeFromBottom(15));
        if (dynamic_cast<juce::ComboBox*>(c.m_comp.get()) != nullptr) c.m_comp->setBounds(cell.withSizeKeepingCentre(cell.getWidth() - 6, 24));
        else if (dynamic_cast<juce::ToggleButton*>(c.m_comp.get()) != nullptr) c.m_comp->setBounds(cell.withSizeKeepingCentre(24, 24));
        else if (dynamic_cast<juce::TextButton*>(c.m_comp.get()) != nullptr) c.m_comp->setBounds(cell.withSizeKeepingCentre(cell.getWidth() - 6, 24));
        else c.m_comp->setBounds(cell.reduced(2, 0));
        cx += c.m_units * CellW; row += c.m_units;
    }
}

void NoctuaryEditor::resized()
{
    // Measure the body first: the design width is whatever it needs, so the right-hand column can
    // never fall off the edge, and the scale follows from that.
    layoutBody();
    m_designW = std::max(MinDesignW, m_bodyW);
    m_designH = std::max(MinDesignH, HeaderH + m_bodyH + StripH);
    m_scale = juce::jmax(0.05f, static_cast<float>(getWidth()) / static_cast<float>(m_designW));
    const auto tf = juce::AffineTransform::scale(m_scale);
    // Every child is laid out in design coordinates and scaled to the window -- except the corner
    // resizer, which is JUCE's own and which JUCE places at the window's bottom right in window
    // coordinates (AudioProcessorEditor::editorResized). Scaling that one too moved it to
    // (width - 18) * scale: a stray drag handle floating in the middle of the panel, at half the
    // way across whenever the window showed the layout at half size. It stays untransformed.
    for (auto* child : getChildren())
        if (dynamic_cast<juce::ResizableCornerComponent*>(child) == nullptr)
            child->setTransform(tf);

    m_header = juce::Rectangle<int>(0, 0, m_designW, HeaderH);
    // The Sound box gets the room the Cosmos box used to take: it holds the longest names, and
    // with the packs loaded it holds five thousand of them.
    if (m_soundBox) m_soundBox->setBounds(200, 8, 260, 24);
    if (m_saveButton) m_saveButton->setBounds(474, 8, 28, 24);   // icons since 03.10.2026
    if (m_loadButton) m_loadButton->setBounds(504, 8, 28, 24);
    if (m_recButton) m_recButton->setBounds(540, 8, 28, 24);
    // The pages first (Main, Perform, Browse), then A | B, then the frame's tools as every instrument of the family has
    // them (01.10.2026): undo and redo, help and the settings; VR after them, while a headset sends (or when asked).
    if (m_mainButton) m_mainButton->setBounds(880, 8, 56, 24);
    if (m_performButton) m_performButton->setBounds(942, 8, 70, 24);
    if (m_browseButton) m_browseButton->setBounds(1018, 8, 66, 24);
    if (m_abButton) m_abButton->setBounds(1090, 8, 52, 24);
    if (m_undoIcon) m_undoIcon->setBounds(1156, 8, 28, 24);
    if (m_redoIcon) m_redoIcon->setBounds(1186, 8, 28, 24);
    if (m_helpIcon) m_helpIcon->setBounds(1224, 8, 28, 24);
    if (m_settingsIcon) m_settingsIcon->setBounds(1254, 8, 28, 24);
    if (m_vrButton) m_vrButton->setBounds(1292, 8, 44, 24);
    // The family's (15.10.2026): the search, the night, the preset held, the journeys; the star beside the Sound box.
    if (m_searchIcon) m_searchIcon->setBounds(580, 8, 28, 24);
    if (m_nightIcon) m_nightIcon->setBounds(610, 8, 28, 24);
    if (m_holdIcon) m_holdIcon->setBounds(640, 8, 28, 24);
    if (m_journeyButton) m_journeyButton->setBounds(674, 8, 66, 24);
    if (m_star) m_star->setBounds(460, 8, 14, 24);
    if (m_undoButton) m_undoButton->setBounds(1156, 8, 28, 24);
    if (m_redoButton) m_redoButton->setBounds(1186, 8, 28, 24);
    if (m_helpButton) m_helpButton->setBounds(1224, 8, 28, 24);

    const int W = m_designW, H = m_designH;
    if (m_perform) m_perform->setBounds(0, HeaderH, W, H - HeaderH);
    if (m_browse) m_browse->setBounds(0, HeaderH, W, H - HeaderH);
    if (m_help) m_help->setBounds(0, HeaderH, W, H - HeaderH);
    // The modulation strip sits along the bottom of the main page, the way Pigments puts its
    // modulator lane there: the sources are always in sight, and a route is a drag away.
    const int stripH = StripH;
    if (m_mod) { m_mod->setBounds(0, H - stripH, W, stripH); m_mod->toFront(false); }
    m_helpLine = { 12, 40, W / 2 - 30, HeaderH - 62 };
    m_keys = { W / 2, 44, juce::jmax(160, W - W / 2 - 360), 24 };
    if (m_outputView) m_outputView->setBounds(W / 2, 74, juce::jmax(160, W - W / 2 - 360), HeaderH - 92);
    if (m_master) m_master->setBounds(W - 74, 6, 66, 52);

    // The Master section (mid/side) sits in the header, left of the master knob.
    if (Section* ms = findSection("Master")) {
        layoutSection(*ms, W - 74 - sectionWidth(*ms) - 6, 4);
        ms->m_bounds = ms->m_bounds.withTrimmedTop(-2);
    }

    // Everything else scrolls below the header.
    m_viewport.setBounds(0, HeaderH, W, H - HeaderH - stripH);
    // The drag overlay covers everything and catches nothing: it is only ever drawn on.
    m_dragOverlay.setBounds(0, 0, W, H);
    m_dragOverlay.toFront(false);
    // The night's veil over the panel, the overlays over the veil.
    m_veil.setBounds(0, 0, W, H);
    m_veil.toFront(false);
    for (juce::Component* c : { static_cast<juce::Component*>(m_palette.get()), static_cast<juce::Component*>(m_tour.get()),
                                static_cast<juce::Component*>(m_journeyPanel.get()) })
        if (c != nullptr) {
            c->setBounds(0, 0, W, H);
            if (c->isVisible()) c->toFront(false);
        }

    m_content.setSize(std::max(m_bodyW, m_viewport.getMaximumVisibleWidth()),
                     std::max(m_bodyH, m_viewport.getMaximumVisibleHeight()));
}

/**
 * Places every group and section, and records how much room the whole body needs. The design
 * size is measured from this once, in the constructor -- guessing it meant the right-hand column
 * kept falling off the edge. A tabbed row is as wide and as tall as its widest and tallest page,
 * so switching a tab never moves anything else.
 * Compact: the columns are CompactFactor as wide and every page is refitted into them. The page
 * loses width and gains height; nothing scrolls either way, and the arrangement is otherwise untouched.
 */
void NoctuaryEditor::applyLayoutMode(int mode)
{
    mode = juce::jlimit(0, 2, mode);
    m_compact = mode == 1;
    m_expanded = mode == 2;
    m_proc.setLayoutMode(mode);
    if (m_compactButton) m_compactButton->setButtonText(mode == 0 ? "Normal" : mode == 1 ? "Compact" : "Expanded");
    rebuildLayout();
}

void NoctuaryEditor::rebuildLayout()
{
    resized();
    // The design's shape has changed, so the window has to follow: keep the width, take the new
    // height from the new ratio, and tell the constrainer about it or the corner would fight it.
    if (auto* con = getConstrainer()) {
        con->setFixedAspectRatio(static_cast<double>(m_designW) / static_cast<double>(m_designH));
        con->setSizeLimits(m_designW / 3, m_designH / 3, m_designW * 2, m_designH * 2);
    }
    setSize(getWidth(), juce::roundToInt(getWidth() * static_cast<double>(m_designH) / static_cast<double>(m_designW)));
    m_content.repaint();
    repaint();
}

void NoctuaryEditor::layoutBody()
{
    for (auto& s : m_sections) {
        s.m_collapsed = sectionCollapsed(s);
        s.m_opened = false;   // decided again below, from the room each row turns out to have
        const int natural = m_expanded ? s.m_flowUnits : s.m_wideUnits;
        if (natural > 0) s.m_maxUnits = natural;
    }
    // Which page: the tabbed two-column page, or the Expanded three-column one with no tabs.
    if (!m_expanded) { layoutTabbed(); return; }
    std::vector<Group>& G = m_expandedGroups;
    for (auto& t : m_tabRows) { t.m_tabs.clear(); t.m_bar = {}; }
    {   // Displays the tabbed page owns and this one does not use go out of sight.
        std::set<juce::Component*> used;
        for (auto& g : G) for (auto* d : g.m_displays) if (d != nullptr) used.insert(d);
        for (auto& t : m_tabRows) for (auto* d : t.m_displays) if (d != nullptr && used.count(d) == 0) d->setVisible(false);
        for (auto& g : m_groups) for (auto* d : g.m_displays) if (d != nullptr && used.count(d) == 0) d->setVisible(false);
    }
    int nCols = 1;
    for (auto& g : G) nCols = std::max(nCols, g.m_column + 1);
    std::vector<int> colWidth(static_cast<size_t>(nCols), 0), colX(static_cast<size_t>(nCols), 0), colY(static_cast<size_t>(nCols), Pad);
    auto pageWidth = [this](const std::vector<juce::String>& names) {
        int w = Pad;
        for (auto& n : names) if (Section* s = findSection(n)) w += sectionWidth(*s) + Pad;
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
        int x = x0 + Pad, y = y0, lineH = 0, widest = 0;
        for (auto& n : names) {
            Section* s = findSection(n);
            if (s == nullptr) continue;
            // Closed only where closing buys room: if it fits open on the line as it stands, it
            // is laid out open. (Both passes see the same flags, so measuring and placing agree.)
            if (s->closedNow()) {
                const int before = shownCells(*s);
                s->m_opened = true;
                if (shownCells(*s) <= before || x + sectionWidth(*s) > x0 + maxW) s->m_opened = false;
            }
            const int w = sectionWidth(*s), h = sectionHeight(*s);
            if (x > x0 + Pad && x + w > x0 + maxW) { x = x0 + Pad; y += lineH + Pad; lineH = 0; }
            if (place) { setSectionVisible(*s, true); layoutSection(*s, x, y); }
            x += w + Pad; lineH = std::max(lineH, h); widest = std::max(widest, x - x0);
        }
        if (heightOut != nullptr) *heightOut = (y - y0) + lineH;
        return widest;
    };
    // The rows of a group as they stand: a row whose only section is closed has no display, and
    // rows without a display that follow one another merge into one flow -- two Off slots share
    // a line instead of each leaving an empty band beside it.
    auto rowsOf = [&](Group& g) {
        std::vector<std::pair<std::vector<juce::String>, juce::Component*>> out;
        for (size_t ri = 0; ri < g.m_rows.size(); ++ri) {
            juce::Component* d = ri < g.m_displays.size() ? g.m_displays[ri] : nullptr;
            const auto& names = g.m_rows[ri];
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
            const int w = hasDisp ? pageWidth(R[ri].first) + DisplayMinW : wrapped(R[ri].first, 0, 0, WrapW, false, nullptr) + Pad;
            colWidth[static_cast<size_t>(g.m_column)] = std::max(colWidth[static_cast<size_t>(g.m_column)], w);
        }
    }
    colX[0] = Pad;
    for (size_t ci = 1; ci < colX.size(); ++ci) colX[ci] = colX[ci - 1] + colWidth[ci - 1] + Pad;
    // The stretchy row of each column, filled in as it is placed and grown at the end of the pass.
    struct Stretch { juce::Component* m_disp = nullptr; Group* m_group = nullptr; };
    std::vector<Stretch> stretch(static_cast<size_t>(nCols));
    for (size_t gi = 0; gi < G.size(); ++gi) {
        auto& g = G[gi];
        const size_t col = static_cast<size_t>(g.m_column);
        const int x0 = colX[col];
        int y = colY[col] + GroupTitleH;
        const auto R = rowsOf(g);
        for (size_t ri = 0; ri < R.size(); ++ri) {
            const std::vector<juce::String>& names = R[ri].first;
            juce::Component* disp = R[ri].second;
            int rowH = std::max(pageHeight(names), g.m_minRowH);
            int x = x0 + Pad;
            if (disp == nullptr) {
                // No display: the row wraps, and is as tall as its lines.
                int h = 0;
                x += wrapped(names, x0, y, WrapW, true, &h);
                rowH = std::max(h, g.m_minRowH);
            } else {
                for (auto& n : names) {
                    Section* s = findSection(n);
                    if (s == nullptr) continue;
                    setSectionVisible(*s, true);
                    layoutSection(*s, x, y);
                    x += s->m_bounds.getWidth() + Pad;
                }
                // Whatever the row leaves free goes to its display.
                const int right = x0 + colWidth[col] - Pad;
                if (right - x >= 120 && rowH > 0) { disp->setBounds(x, y, right - x, rowH); disp->setVisible(true); }
                else disp->setVisible(false);
                if (g.m_stretch && disp->isVisible()) { stretch[col].m_disp = disp; stretch[col].m_group = &g; }
            }
            y += rowH + Pad;
        }
        g.m_bounds = { x0, colY[col], colWidth[col], y - colY[col] };
        colY[col] = y + Pad;
    }
    m_bodyW = colX.back() + colWidth.back() + Pad;
    m_bodyH = 0;
    for (int yy : colY) m_bodyH = std::max(m_bodyH, yy);
    // The stretch. A stretchy group is the last one in its column, so growing its row only reaches
    // downwards and nothing else has to move: the columns end level.
    for (size_t ci = 0; ci < stretch.size(); ++ci) {
        if (stretch[ci].m_disp == nullptr || stretch[ci].m_group == nullptr) continue;
        const int deficit = m_bodyH - colY[ci];
        if (deficit <= 0) continue;
        stretch[ci].m_disp->setBounds(stretch[ci].m_disp->getBounds().withHeight(stretch[ci].m_disp->getHeight() + deficit));
        stretch[ci].m_group->m_bounds = stretch[ci].m_group->m_bounds.withHeight(stretch[ci].m_group->m_bounds.getHeight() + deficit);
        colY[ci] += deficit;
    }
}

/**
 * The tabbed page: Normal and Compact. See the header for the rule; the pieces are these.
 */
void NoctuaryEditor::layoutTabbed()
{
    for (auto& g : m_expandedGroups) for (auto* d : g.m_displays) if (d != nullptr) d->setVisible(false);
    std::vector<Group>& G = m_groups;
    int nCols = 1;
    for (auto& g : G) nCols = std::max(nCols, g.m_column + 1);
    const size_t NC = static_cast<size_t>(nCols);
    std::vector<int> colWidth(NC, 0), colX(NC, 0), colY(NC, Pad), colYMax(NC, Pad);

    // A page: the sections that stand side by side, and the display that takes the rest.
    struct Page { std::vector<juce::String> m_names; juce::Component* m_disp = nullptr; };
    auto pagesOf = [&](size_t gi, size_t ri) {
        std::vector<Page> out;
        Group& g = G[gi];
        if (TabRow* t = tabRowFor(static_cast<int>(gi), static_cast<int>(ri))) {
            for (size_t pi = 0; pi < t->m_pages.size(); ++pi)
                out.push_back({ t->m_pages[pi], pi < t->m_displays.size() ? t->m_displays[pi] : nullptr });
        } else out.push_back({ g.m_rows[ri], ri < g.m_displays.size() ? g.m_displays[ri] : nullptr });
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
    auto unitOf  = [&](const Section& s) { int u = 1; for (int ci : s.m_cells) if (cellShown(s, m_cells[static_cast<size_t>(ci)])) u = std::max(u, m_cells[static_cast<size_t>(ci)].m_units); return u; };
    auto widthAt = [&](Section& s, int w) { const int keep = s.m_maxUnits; s.m_maxUnits = w; const int r = sectionWidth(s); s.m_maxUnits = keep; return r; };
    auto rowsAt  = [&](Section& s, int w) { const int keep = s.m_maxUnits; s.m_maxUnits = w; const int h = sectionHeight(s); s.m_maxUnits = keep; return (h - TitleH - Pad) / CellH; };
    auto pageH   = [&](const std::vector<Section*>& secs) { int h = 0; for (auto* s : secs) h = std::max(h, sectionHeight(*s)); return h; };
    // A page's width at the natural widths, and at the narrowest (every section one cell wide).
    auto naturalW = [&](const Page& p) { int w = Pad; for (auto* s : live(p.m_names)) w += widthAt(*s, s->m_wideUnits) + Pad; return w + (p.m_disp != nullptr ? DisplayMinW : 0); };
    auto narrowW  = [&](const Page& p) { int w = Pad; for (auto* s : live(p.m_names)) w += widthAt(*s, unitOf(*s)) + Pad; return w + (p.m_disp != nullptr ? DisplayMinW : 0); };
    // The widths of a page's sections at r rows each: the narrowest at which none needs more.
    // Returns the width they take side by side, with the pads.
    auto widthsAt = [&](const std::vector<Section*>& secs, int r, std::vector<int>& ws) {
        int total = Pad; ws.clear();
        for (Section* s : secs) {
            int w = unitOf(*s);
            while (w < 64 && rowsAt(*s, w) > r) ++w;
            ws.push_back(w); total += widthAt(*s, w) + Pad;
        }
        return total;
    };
    // The fit: the fewest rows at which the page fits its column, its sections each as narrow as
    // that allows. Where the row is taller than the page needs (the stretch), as many rows as the
    // height holds instead, so the sections stand tall and the display takes the width.
    auto fit = [&](const Page& p, int availW, int availH) {
        auto secs = live(p.m_names);
        const int room = availW - (p.m_disp != nullptr ? DisplayMinW : 0);
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
            s->m_opened = true;
            std::vector<int> ws2;
            const int t = widthsAt(secs, r, ws2);
            if (shownCells(*s) > before && t <= room) { ws = ws2; used = t; }
            else { s->m_opened = false; used = widthsAt(secs, r, ws); }
        }
        // What the row has over is handed out a cell at a time, round robin, to the sections that
        // can take it without losing a row: a last row that is not full reads as a section, an
        // empty band at the end of the row reads as a hole. A display takes it instead.
        if (p.m_disp == nullptr) {
            bool any = true;
            while (any && used + CellW <= room) {
                any = false;
                for (size_t i = 0; i < secs.size() && used + CellW <= room; ++i) {
                    const int w2 = ws[i] + 1;
                    if (rowsAt(*secs[i], w2) != rowsAt(*secs[i], ws[i])) continue;
                    const int grow = widthAt(*secs[i], w2) - widthAt(*secs[i], ws[i]);
                    if (grow <= 0 || used + grow > room) continue;
                    ws[i] = w2; used += grow; any = true;
                }
            }
        }
        for (size_t i = 0; i < secs.size(); ++i) secs[i]->m_maxUnits = ws[i];
        if (p.m_disp != nullptr && availH > 0) {
            for (int rr = r + 1; rr < 12; ++rr) {
                std::vector<int> ws2;
                if (widthsAt(secs, rr, ws2) > room) break;
                bool taller = false, fits = true;
                for (size_t i = 0; i < secs.size(); ++i) {
                    const int rows2 = rowsAt(*secs[i], ws2[i]);
                    if (rows2 * CellH + TitleH + Pad > availH) fits = false;
                    if (rows2 > rowsAt(*secs[i], ws[i])) taller = true;
                }
                if (!fits) break;
                if (taller) { ws = ws2; for (size_t i = 0; i < secs.size(); ++i) secs[i]->m_maxUnits = ws[i]; }
            }
        }
        return pageH(secs);
    };
    // Column widths: the widest page at its natural width. Compact takes a fraction of that,
    // never less than the narrowest page needs.
    std::vector<int> narrow(NC, 0);
    for (size_t gi = 0; gi < G.size(); ++gi)
        for (size_t ri = 0; ri < G[gi].m_rows.size(); ++ri)
            for (const Page& p : pagesOf(gi, ri)) {
                const size_t col = static_cast<size_t>(G[gi].m_column);
                colWidth[col] = std::max(colWidth[col], naturalW(p));
                narrow[col] = std::max(narrow[col], narrowW(p));
            }
    if (m_compact) for (size_t c = 0; c < NC; ++c) colWidth[c] = std::max(narrow[c], juce::roundToInt(colWidth[c] * CompactFactor));
    colX[0] = Pad;
    for (size_t ci = 1; ci < colX.size(); ++ci) colX[ci] = colX[ci - 1] + colWidth[ci - 1] + Pad;

    const juce::Font tabFont = ui::title(10.5f);
    // The stretchy row of each column: placed here, refitted and grown once the deficit is known.
    struct Stretch { Group* m_group = nullptr; Page m_page; int m_x0 = 0, m_y = 0, m_rowH = 0; bool m_set = false; };
    std::vector<Stretch> stretch(NC);
    for (size_t gi = 0; gi < G.size(); ++gi) {
        auto& g = G[gi];
        const size_t col = static_cast<size_t>(g.m_column);
        const int x0 = colX[col];
        int y = colY[col] + GroupTitleH, yMax = colYMax[col] + GroupTitleH;
        for (size_t ri = 0; ri < g.m_rows.size(); ++ri) {
            const auto pages = pagesOf(gi, ri);
            TabRow* t = tabRowFor(static_cast<int>(gi), static_cast<int>(ri));
            // Every page fitted, so the tallest is known -- the design height counts that one.
            int rowHMax = g.m_minRowH;
            for (const Page& p : pages) rowHMax = std::max(rowHMax, fit(p, colWidth[col], 0));
            const size_t act = t != nullptr ? static_cast<size_t>(juce::jlimit(0, static_cast<int>(pages.size()) - 1, t->m_active)) : 0;
            const Page& p = pages[act];
            if (t != nullptr) {
                t->m_bar = { x0 + Pad, y, colWidth[col] - 2 * Pad, TabH };
                t->m_tabs.clear();
                int tx = t->m_bar.getX();
                for (auto& nm : t->m_names) {
                    const int tw = juce::GlyphArrangement::getStringWidthInt(tabFont, nm) + 30;
                    t->m_tabs.push_back({ tx, y, tw, TabH - 4 });
                    tx += tw + 4;
                }
                for (size_t pi = 0; pi < pages.size(); ++pi) {
                    if (pi == act) continue;
                    for (auto& n : pages[pi].m_names) if (Section* s = findSection(n)) setSectionVisible(*s, false);
                    if (pages[pi].m_disp != nullptr && pages[pi].m_disp != p.m_disp) pages[pi].m_disp->setVisible(false);
                }
                y += TabH; yMax += TabH;
            }
            auto secs = live(p.m_names);
            const int rowH = std::max(pageH(secs), g.m_minRowH);
            int x = x0 + Pad;
            for (Section* s : secs) { setSectionVisible(*s, true); layoutSection(*s, x, y); x += s->m_bounds.getWidth() + Pad; }
            if (p.m_disp != nullptr) {
                const int right = x0 + colWidth[col] - Pad;
                if (right - x >= 120 && rowH > 0) { p.m_disp->setBounds(x, y, right - x, rowH); p.m_disp->setVisible(true); }
                else p.m_disp->setVisible(false);
                if (g.m_stretch && p.m_disp->isVisible()) stretch[col] = { &g, p, x0, y, rowH, true };
            }
            y += rowH + Pad; yMax += rowHMax + Pad;
        }
        g.m_bounds = { x0, colY[col], colWidth[col], y - colY[col] };
        colY[col] = y + Pad; colYMax[col] = yMax + Pad;
    }
    m_bodyW = colX.back() + colWidth.back() + Pad;
    m_bodyH = 0;
    for (int yy : colYMax) m_bodyH = std::max(m_bodyH, yy);   // the tallest pages: the shape a tab click cannot change
    // The stretch: the last row of each column grows to the body's height. Its page is refitted
    // for the new height -- the sections as tall as it holds, the display beside them as wide as
    // that leaves -- so the room under the knobs is not a hole but more picture.
    for (size_t ci = 0; ci < stretch.size(); ++ci) {
        Stretch& st = stretch[ci];
        if (!st.m_set) continue;
        const int deficit = m_bodyH - colY[ci];
        if (deficit <= 0) continue;
        const int rowH = st.m_rowH + deficit;
        fit(st.m_page, colWidth[ci], rowH);
        auto secs = live(st.m_page.m_names);
        int x = st.m_x0 + Pad;
        for (Section* s : secs) { setSectionVisible(*s, true); layoutSection(*s, x, st.m_y); x += s->m_bounds.getWidth() + Pad; }
        const int right = st.m_x0 + colWidth[ci] - Pad;
        st.m_page.m_disp->setBounds(x, st.m_y, std::max(120, right - x), rowH);
        st.m_group->m_bounds = st.m_group->m_bounds.withHeight(st.m_group->m_bounds.getHeight() + deficit);
        colY[ci] += deficit;
    }
}

NoctuaryEditor::TabRow* NoctuaryEditor::tabRowFor(int group, int row)
{
    for (auto& t : m_tabRows) if (t.m_group == group && t.m_row == row) return &t;
    return nullptr;
}

void NoctuaryEditor::setSectionVisible(Section& s, bool v)
{
    s.m_visible = v;
    for (int ci : s.m_cells) {
        Cell& c = m_cells[static_cast<size_t>(ci)];
        const bool show = v && cellShown(s, c);
        if (c.m_comp) c.m_comp->setVisible(show);
        if (c.m_label) c.m_label->setVisible(show);
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
    for (auto& s : m_sections) {
        const bool c = sectionCollapsed(s);
        if (c != s.m_collapsed) { s.m_collapsed = c; changed = true; }
    }
    return changed;
}

void NoctuaryEditor::clickTabs(juce::Point<int> pos)
{
    if (m_expanded) return;   // every page is open; the titles are titles, not tabs
    for (auto& t : m_tabRows)
        for (size_t i = 0; i < t.m_tabs.size(); ++i)
            if (t.m_tabs[i].contains(pos) && static_cast<int>(i) != t.m_active) {
                t.m_active = static_cast<int>(i);
                layoutBody();
                m_content.repaint();
                return;
            }
}

// ---------------------------------------------------------------- perform page

void NoctuaryEditor::setPerforming(bool on) { setPage(on ? 1 : 0); }

void NoctuaryEditor::setPage(int page)
{
    m_performing = page == 1;
    m_viewport.setVisible(page == 0);
    m_perform->setVisible(page == 1);
    m_browse->setVisible(page == 2);
    if (m_help) m_help->setVisible(page == 3);
    if (m_mod) m_mod->setVisible(page == 0);
    if (m_mainButton && m_mainButton->getToggleState() != (page == 0)) m_mainButton->setToggleState(page == 0, juce::dontSendNotification);
    if (m_performButton->getToggleState() != (page == 1)) m_performButton->setToggleState(page == 1, juce::dontSendNotification);
    if (m_browseButton->getToggleState() != (page == 2)) m_browseButton->setToggleState(page == 2, juce::dontSendNotification);
    if (m_helpButton && m_helpButton->getToggleState() != (page == 3)) m_helpButton->setToggleState(page == 3, juce::dontSendNotification);

    if (page == 2) m_browse->applyFilter();

}


// ---------------------------------------------------------------- modulation strip

// ---------------------------------------------------------------- interaction

void NoctuaryEditor::mouseDown(const juce::MouseEvent& e)
{
    if (!e.mods.isPopupMenu()) return;
    auto it = m_cellOf.find(e.eventComponent);
    if (it == m_cellOf.end()) return;
    const int idx = it->second;
    const int param = m_cells[static_cast<size_t>(idx)].m_param;
    if (param < 0) return;
    const ParamIdE id = static_cast<ParamIdE>(param);
    const int cc = m_proc.midiCcFor(id);
    juce::PopupMenu menu;
    menu.addItem(1, m_proc.learnTarget() == param ? "Learning... move a controller" : "MIDI Learn", m_proc.learnTarget() != param);
    if (cc >= 0) menu.addItem(2, "Clear MIDI (CC " + juce::String(cc) + ")");
    // What modulates this knob -- each route with its depth to drag and a button to remove it -- and every source to add
    // (15.10.2026, the family's Frame.h; the other side of the cards' menu, and of a card dragged onto the knob).
    if (m_modulation != nullptr) m_modulation->addMenuItems(menu, param);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(e.eventComponent), [this, id](int result) {
        if (result == 1) m_proc.armMidiLearn(id);
        else if (result == 2) m_proc.clearMidiLearn(id);
    });
}

void NoctuaryEditor::registerHelp(juce::Component* c, ambient::ParamIdE id)
{
    if (c == nullptr) return;
    m_helpParamOf[c] = static_cast<int>(id);
    c->addMouseListener(this, false);
}

void NoctuaryEditor::mouseEnter(const juce::MouseEvent& e)
{
    auto it = m_helpParamOf.find(e.eventComponent);
    const int param = it != m_helpParamOf.end() ? it->second : -1;
    if (param != m_hoveredParam) { m_hoveredParam = param; repaint(); }
}

void NoctuaryEditor::mouseExit(const juce::MouseEvent& e)
{
    auto it = m_helpParamOf.find(e.eventComponent);
    if (it != m_helpParamOf.end() && it->second == m_hoveredParam) { m_hoveredParam = -1; repaint(); }
}

// ---------------------------------------------------------------- undo, redo, A/B, the die

NoctuaryEditor::Snapshot NoctuaryEditor::takeSnapshot(const juce::String& what) const
{
    Snapshot s;
    s.m_what = what;
    s.m_v.resize(static_cast<size_t>(NumParams));
    for (int i = 0; i < NumParams; ++i)
        if (auto* v = m_proc.m_apvts.getRawParameterValue(paramTable()[static_cast<size_t>(i)].m_key)) s.m_v[static_cast<size_t>(i)] = v->load();
    return s;
}

void NoctuaryEditor::restore(const Snapshot& s)
{
    if (static_cast<int>(s.m_v.size()) != NumParams) return;
    for (int i = 0; i < NumParams; ++i)
        if (auto* p = m_proc.m_apvts.getParameter(paramTable()[static_cast<size_t>(i)].m_key))
            p->setValueNotifyingHost(p->convertTo0to1(s.m_v[static_cast<size_t>(i)]));
    repaint();
}

void NoctuaryEditor::pushUndo(const juce::String& what)
{
    m_undo.push_back(takeSnapshot(what));
    if (m_undo.size() > 32) m_undo.erase(m_undo.begin());   // a session's worth, not a history
    m_redo.clear();
}

void NoctuaryEditor::doUndo()
{
    if (m_undo.empty()) return;
    m_redo.push_back(takeSnapshot(m_undo.back().m_what));
    restore(m_undo.back());
    m_undo.pop_back();
}

void NoctuaryEditor::doRedo()
{
    if (m_redo.empty()) return;
    m_undo.push_back(takeSnapshot(m_redo.back().m_what));
    restore(m_redo.back());
    m_redo.pop_back();
}

/**
 * A/B: the first click parks what you have in A and leaves you on B (a copy, so nothing is lost);
 * every click after that swaps the two. This is the comparison a sound gets judged by.
 */
void NoctuaryEditor::swapAB()
{
    const Snapshot now = takeSnapshot("A/B");
    if (m_slotA.m_v.empty()) { m_slotA = now; m_slotB = now; m_showingB = true; }
    else {
        (m_showingB ? m_slotB : m_slotA) = now;
        m_showingB = !m_showingB;
        restore(m_showingB ? m_slotB : m_slotA);
    }
    if (m_abButton) m_abButton->setButtonText(m_showingB ? "B | a" : "A | b");
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
    for (int ci : sec->m_cells) {
        const Cell& c = m_cells[static_cast<size_t>(ci)];
        if (c.m_param < 0) continue;
        const ParamIdE id = static_cast<ParamIdE>(c.m_param);
        if (isPerformanceParam(id)) continue;
        const ParamDesc& d = paramDesc(id);
        auto* p = m_proc.m_apvts.getParameter(d.m_key);
        if (p == nullptr) continue;
        float value;
        if (d.m_kind == ParamKindE::CHOICE || d.m_kind == ParamKindE::BOOL || d.m_kind == ParamKindE::INT) {
            const int lo = static_cast<int>(std::lround(d.m_min)), hi = static_cast<int>(std::lround(d.m_max));
            const int cur = static_cast<int>(std::lround(m_proc.engine().getParam(id)));
            value = static_cast<float>(subtle ? juce::jlimit(lo, hi, cur + rng.nextInt(3) - 1) : lo + rng.nextInt(hi - lo + 1));
        } else {
            // Drawn in the parameter's own skewed domain, so a logarithmic knob is not biased
            // towards its top end -- the same domain the morph and the map blend in.
            const float span = std::max(d.m_max - d.m_min, 1e-9f);
            const float cur = std::pow(juce::jlimit(0.0f, 1.0f, (m_proc.engine().getParam(id) - d.m_min) / span), d.m_skew);
            const float t = subtle ? juce::jlimit(0.0f, 1.0f, cur + 0.2f * (rng.nextFloat() * 2.0f - 1.0f))
                                   : 0.15f + 0.7f * rng.nextFloat();
            value = d.m_min + span * std::pow(t, 1.0f / d.m_skew);
        }
        p->setValueNotifyingHost(p->convertTo0to1(value));
    }
    repaint();
}

bool NoctuaryEditor::keyPressed(const juce::KeyPress& k)
{
    if (k == juce::KeyPress::F1Key) { setPage(m_help && m_help->isVisible() ? 0 : 3); return true; }
    if (k == juce::KeyPress('k', juce::ModifierKeys::commandModifier, 0)) { openPalette(); return true; }
    if (k == juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0)) { doUndo(); return true; }
    if (k == juce::KeyPress('y', juce::ModifierKeys::commandModifier, 0)) { doRedo(); return true; }
    if (k == juce::KeyPress::escapeKey && m_help && m_help->isVisible()) { setPage(0); return true; }
    return false;
}

// ---------------------------------------------------------------- help page

juce::Image NoctuaryEditor::snapshotSection(const juce::String& name)
{
    Section* sec = findSection(name);
    if (sec == nullptr) return {};
    // If the section lives on a tab that is not open, open it for the picture and put it back.
    TabRow* row = nullptr; int was = 0;
    for (auto& t : m_tabRows)
        for (size_t pi = 0; pi < t.m_pages.size(); ++pi)
            for (auto& n : t.m_pages[pi]) if (n == name && static_cast<int>(pi) != t.m_active) { row = &t; was = t.m_active; t.m_active = static_cast<int>(pi); }
    if (row != nullptr) layoutBody();
    // The Master section is painted in the header and its bounds are the editor's; every other
    // section belongs to the scrolling content. Photographed from the wrong component, the
    // Master came out as a strip of the panel's top-left corner.
    juce::Image img = (name == "Master" ? static_cast<juce::Component&>(*this) : static_cast<juce::Component&>(m_content))
                          .createComponentSnapshot(sec->m_bounds.expanded(2), true, 1.0f);
    if (row != nullptr) { row->m_active = was; layoutBody(); }
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
    if (rowIndex < 0 || rowIndex >= static_cast<int>(m_tabRows.size())) return {};
    TabRow& t = m_tabRows[static_cast<size_t>(rowIndex)];
    if (page < 0 || page >= static_cast<int>(t.m_pages.size())) return {};
    const int was = t.m_active;
    const bool wasExpanded = m_expanded;   // the manual's pictures are of the page as it is normally: one tab open
    m_expanded = false;
    t.m_active = page;
    layoutBody();
    juce::Rectangle<int> box = t.m_bar;                      // the tab bar itself belongs in the picture
    auto add = [&](const juce::String& name) {
        if (name.isEmpty()) return;
        if (Section* s = findSection(name)) box = box.isEmpty() ? s->m_bounds : box.getUnion(s->m_bounds);
    };
    for (const auto& n : t.m_pages[static_cast<size_t>(page)]) add(n);
    if (page < static_cast<int>(t.m_displays.size()) && t.m_displays[static_cast<size_t>(page)] != nullptr) {
        juce::Component* d = t.m_displays[static_cast<size_t>(page)];
        if (d->isVisible() && d->getWidth() > 0) box = box.getUnion(d->getBounds());
    }
    juce::Image img;
    if (!box.isEmpty()) img = m_content.createComponentSnapshot(box.expanded(4), true, 1.0f);
    t.m_active = was;
    m_expanded = wasExpanded;
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

juce::Image NoctuaryEditor::snapshotPerform() { return snapshotPage(m_perform.get()); }

juce::Image NoctuaryEditor::snapshotBrowseMap()
{
    if (!m_browse) return {};
    const bool vis = m_browse->isVisible();
    const int was = m_browse->m_modeMap.getToggleState() ? 1 : 0;
    m_browse->setVisible(true);
    m_browse->setMode(1);
    m_browse->resized();
    juce::Image img = m_browse->createComponentSnapshot(m_browse->getLocalBounds(), true, 1.0f);
    m_browse->setMode(was);
    m_browse->setVisible(vis);
    return img;
}

/**
 * The map as it looks once it has been zoomed in: the current preset in the middle at eight
 * times, its neighbours around it, and names on everything that has room for one.
 */
juce::Image NoctuaryEditor::snapshotBrowseMapZoomed()
{
    if (!m_browse) return {};
    const bool vis = m_browse->isVisible();
    const int was = m_browse->m_modeMap.getToggleState() ? 1 : 0;
    m_browse->setVisible(true);
    m_browse->setMode(1);
    m_browse->resized();
    const int cur = m_proc.getCurrentProgram();
    float cx = 0.5f, cy = 0.5f;
    if (cur >= 0 && cur < ambient::numPresetMeta()) { cx = ambient::presetMeta(cur).m_x; cy = ambient::presetMeta(cur).m_y; }
    m_browse->m_map.zoomTo(cx, cy, 8.0f);
    juce::Image img = m_browse->createComponentSnapshot(m_browse->getLocalBounds(), true, 1.0f);
    m_browse->m_map.zoomTo(0.5f, 0.5f, 1.0f);
    m_browse->setMode(was);
    m_browse->setVisible(vis);
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
    if (Section* ms = findSection("Master")) box = ms->m_bounds;
    if (m_outputView != nullptr && !m_outputView->getBounds().isEmpty()) box = box.isEmpty() ? m_outputView->getBounds() : box.getUnion(m_outputView->getBounds());
    if (!m_keys.isEmpty()) box = box.isEmpty() ? m_keys : box.getUnion(m_keys);   // the note roll over the meter
    if (box.isEmpty()) return {};
    box = box.expanded(8).withRight(getWidth()).withTop(0);
    box.setBottom(juce::jmin(HeaderH - 2, box.getBottom()));
    return createComponentSnapshot(box, true, 1.0f);
}

juce::Image NoctuaryEditor::snapshotStripTab(int tab, bool detail)
{
    if (!m_mod) return {};
    const int was = m_mod->m_tab;
    m_mod->setTab(tab);
    juce::Image img;
    if (detail) {
        // The strip is nearly three thousand pixels wide and shrinks to a ribbon on a page; the
        // left third -- the cards and the first panel -- at twice the size is where it can be read.
        const bool vis = m_mod->isVisible();
        m_mod->setVisible(true);
        img = m_mod->createComponentSnapshot(m_mod->getLocalBounds().withWidth(m_mod->getWidth() * 24 / 70), true, 2.0f);
        m_mod->setVisible(vis);
    } else img = snapshotStrip();
    m_mod->setTab(was);
    return img;
}

void NoctuaryEditor::openSourceEnvelope(int slot)
{
    // The pictures that call this sit on the main page, where the strip is, so it only needs its
    // page turned. Which source asked is not singled out: the page shows all four side by side.
    juce::ignoreUnused(slot);
    if (!m_mod) return;
    m_mod->setTab(1);
    m_mod->setEnvPage(1);
}

juce::StringArray NoctuaryEditor::tabSectionNames(int rowIndex, int page) const
{
    juce::StringArray out;
    if (rowIndex < 0 || rowIndex >= static_cast<int>(m_tabRows.size())) return out;
    const TabRow& t = m_tabRows[static_cast<size_t>(rowIndex)];
    if (page < 0 || page >= static_cast<int>(t.m_pages.size())) return out;
    for (const auto& n : t.m_pages[static_cast<size_t>(page)]) out.add(n);
    return out;
}

/**
 * What a tab is called on its own bar, so the manual's caption is the word the reader will look
 * for on the screen.
 */
juce::String NoctuaryEditor::tabName(int rowIndex, int page) const
{
    if (rowIndex < 0 || rowIndex >= static_cast<int>(m_tabRows.size())) return {};
    const TabRow& t = m_tabRows[static_cast<size_t>(rowIndex)];
    if (page < 0 || page >= static_cast<int>(t.m_names.size())) return {};
    return t.m_names[static_cast<size_t>(page)];
}

juce::Image NoctuaryEditor::snapshotStrip()
{
    if (!m_mod) return {};
    const bool vis = m_mod->isVisible();
    m_mod->setVisible(true);
    juce::Image img = m_mod->createComponentSnapshot(m_mod->getLocalBounds(), true, 1.0f);
    m_mod->setVisible(vis);
    return img;
}

juce::Image NoctuaryEditor::snapshotBrowse()
{
    if (!m_browse) return {};
    const bool vis = m_browse->isVisible();
    m_browse->setVisible(true);
    juce::Image img = m_browse->createComponentSnapshot(m_browse->getLocalBounds(), true, 1.0f);
    m_browse->setVisible(vis);
    return img;
}

NoctuaryEditor::HelpView::HelpView(NoctuaryProcessor& p, NoctuaryEditor& o)
    : m_proc(p), m_owner(o), m_flow(p)
{
    addChildComponent(m_flow);
    m_topics.setModel(this);
    m_topics.setRowHeight(26);
    m_topics.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    addAndMakeVisible(m_topics);
    m_text.setMultiLine(true, true);
    m_text.setReadOnly(true);
    m_text.setCaretVisible(false);
    m_text.setScrollbarsShown(true);
    m_text.setFont(ui::body(15.0f));
    m_text.setColour(juce::TextEditor::backgroundColourId, ui::CardColour.withAlpha(0.5f));
    m_text.setColour(juce::TextEditor::outlineColourId, ui::CardEdge);
    m_text.setColour(juce::TextEditor::textColourId, ui::Text);
    m_text.setIndents(14, 12);
    addAndMakeVisible(m_text);
    // The last topic is generated: every parameter, by section, with its help text.
    juce::String last;
    for (const ParamDesc& d : paramTable()) {
        if (last != d.m_section) { last = d.m_section; m_parameters += (m_parameters.isEmpty() ? "" : "\n") + juce::String(d.m_section).toUpperCase() + "\n"; }
        juce::String range;
        if (d.m_kind == ParamKindE::CHOICE) { for (int i = 0; i < d.m_numChoices; ++i) range += (i ? " / " : "") + juce::String(d.m_choices[i]); }
        else if (d.m_kind == ParamKindE::BOOL) range = "on / off";
        else range = juce::String(d.m_min, d.m_kind == ParamKindE::INT ? 0 : 2) + " .. " + juce::String(d.m_max, d.m_kind == ParamKindE::INT ? 0 : 2) + (d.m_unit[0] ? juce::String(" ") + d.m_unit : juce::String());
        m_parameters += "  " + juce::String(d.m_name) + "  (" + d.m_key + ", " + range + ")\n      " + ambient::paramHelp(d.m_id) + "\n";
    }
    m_topics.updateContent();
    m_topics.selectRow(0, false, true);
    selectedRowsChanged(0);
}

void NoctuaryEditor::HelpView::paint(juce::Graphics& g)
{
    g.fillAll(ui::Bg0);
    g.setColour(ui::GroupColour);
    g.fillRoundedRectangle(getLocalBounds().reduced(10).toFloat(), 8.0f);
    for (size_t i = 0; i < m_pics.size() && i < m_picRects.size(); ++i) {
        if (!m_pics[i].isValid() || m_picRects[i].isEmpty()) continue;
        g.drawImage(m_pics[i], m_picRects[i].toFloat(), juce::RectanglePlacement::stretchToFit);
        g.setColour(ui::CardEdge); g.drawRoundedRectangle(m_picRects[i].toFloat().reduced(0.5f), 4.0f, 1.0f);
    }
    g.setColour(ui::Text);
    g.setFont(ui::title(13.0f));
    g.drawText("MANUAL", 26, 18, 200, 20, juce::Justification::centredLeft, false);
    g.setColour(ui::Dim);
    g.setFont(ui::body(11.0f));
    g.drawText("F1 or Help closes it again  --  the pictures are the panel as it stands right now, and the displays are live", 120, 18, getWidth() - 160, 20, juce::Justification::centredLeft, false);
}

void NoctuaryEditor::HelpView::resized()
{
    auto r = getLocalBounds().reduced(20).withTrimmedTop(26);
    m_topics.setBounds(r.removeFromLeft(230));
    r.removeFromLeft(12);
    // Text on the left at a readable line length, the pictures in the column to its right.
    const int textW = juce::jlimit(360, 820, r.getWidth() * 42 / 100);
    m_text.setBounds(r.removeFromLeft(textW));
    r.removeFromLeft(16);
    m_flow.setBounds(r);
    m_picRects.clear();
    auto col = r;
    for (const auto& img : m_pics) {
        if (!img.isValid() || col.getHeight() < 60) { m_picRects.push_back({}); continue; }
        const float scale = juce::jmin(1.0f, static_cast<float>(col.getWidth()) / static_cast<float>(img.getWidth()),
                                       static_cast<float>(juce::jmin(320, col.getHeight() - (m_live ? 240 : 0))) / static_cast<float>(img.getHeight()));
        const int w = juce::roundToInt(img.getWidth() * scale), h = juce::roundToInt(img.getHeight() * scale);
        m_picRects.push_back(col.removeFromTop(h).withWidth(w));
        col.removeFromTop(10);
    }
    if (m_live) m_live->setBounds(col.removeFromTop(juce::jmin(260, col.getHeight())));
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
    juce::File m_dir;   ///< the folder everything is written into
    std::vector<std::function<void()>> m_steps;   ///< the steps, run one per 350 ms by runManualStep
    size_t m_next = 0;   ///< the step that runs next
    std::function<void()> m_done;   ///< called after the last step: puts m_openAll back and quits, when asked to
    /**
     * @brief What has been written so far, per topic: the pictures and the tabs with their captions,
     * their blurbs and the sections whose parameters belong under them.
     */
    struct Tab {
        juce::String m_file,            ///< the PNG's file name in dir
        m_name,                         ///< the tab's name on its bar
        m_caption,                      ///< the caption under the picture
        m_blurb;                        ///< the tab's own text from ambient::tabHelp
        juce::StringArray m_sections;   ///< the sections whose parameters the manual prints under it
    };
    /** @brief One section picture of a topic. */
    struct Pic {
        juce::String m_file,   ///< the PNG's file name in dir
        m_caption;             ///< the caption under it
    };
    std::vector<std::vector<Pic>> m_images;   ///< per topic: its section pictures
    std::vector<std::vector<Tab>> m_tabs;   ///< per topic: its tab pictures
    juce::String m_originalType;    ///< Source 2's type before the gallery, put back afterwards
};

void NoctuaryEditor::exportManual(const juce::File& dir, std::function<void()> onDone)
{
    dir.createDirectory();
    if (m_help == nullptr) { if (onDone) onDone(); return; }
    m_manual = std::make_unique<ManualJob>();
    ManualJob& job = *m_manual;
    job.m_dir = dir;
    // Every section open for its picture, whatever its switch says, and back afterwards.
    m_openAll = true;
    rebuildLayout();
    job.m_done = [this, cb = std::move(onDone)] { m_openAll = false; rebuildLayout(); if (cb) cb(); };
    const int last = ambient::numHelpTopics();    // the generated "All parameters" topic comes after
    job.m_images.resize(static_cast<size_t>(last + 1));
    job.m_tabs.resize(static_cast<size_t>(last + 1));

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
    m_help->setBounds(0, HeaderH, m_designW, m_designH - HeaderH);

    // ---- one step per topic: its sections, its live display, its tabs
    for (int row = 0; row <= last; ++row) {
        job.m_steps.push_back([this, row, stem, writePng, &job] {
            m_help->m_topics.selectRow(row);
            m_help->showTopic(row);
            m_help->resized();
            auto& files = job.m_images[static_cast<size_t>(row)];
            // The section pictures the help page shows are the manual's only when the chapter
            // has no tab pictures: where it has, every section is already in one of them.
            for (size_t i = 0; m_help->m_tabPics.empty() && i < m_help->m_pics.size(); ++i) {
                const juce::String f = writePng(m_help->m_pics[i], stem(row) + "-" + juce::String(static_cast<int>(i)) + ".png");
                if (f.isNotEmpty()) files.push_back({ f, i < static_cast<size_t>(m_help->m_picCaptions.size()) ? m_help->m_picCaptions[static_cast<int>(i)] : juce::String() });
            }
            auto shot = [&](juce::Component& c, const juce::String& suffix, const juce::String& caption, juce::Rectangle<int> area = {}) {
                if (c.getWidth() <= 0 || c.getHeight() <= 0) return;
                if (area.isEmpty()) area = c.getLocalBounds();
                const juce::String f = writePng(c.createComponentSnapshot(area, true, 2.0f), stem(row) + "-" + suffix + ".png");
                if (f.isNotEmpty()) files.push_back({ f, caption });
            };
            if (row == 0) shot(m_help->m_flow, "flow", "The signal flow: every unit as a box in its group's colour, the buses as arrows -- the same picture the help page's first topic shows.", m_help->m_flow.drawn());
            if (m_help->m_live != nullptr) shot(*m_help->m_live, "live", m_help->m_liveCaption);
            for (size_t i = 0; i < m_help->m_tabPics.size(); ++i) {
                ManualJob::Tab t;
                t.m_file = writePng(m_help->m_tabPics[i], stem(row) + "-tab" + juce::String(static_cast<int>(i)) + ".png");
                if (t.m_file.isEmpty()) continue;
                t.m_name = i < static_cast<size_t>(m_help->m_tabNames.size()) ? m_help->m_tabNames[static_cast<int>(i)] : juce::String();
                t.m_caption = i < static_cast<size_t>(m_help->m_tabCaptions.size()) ? m_help->m_tabCaptions[static_cast<int>(i)] : juce::String();
                t.m_sections = i < m_help->m_tabSections.size() ? m_help->m_tabSections[i] : juce::StringArray();
                t.m_blurb = juce::CharPointer_UTF8(ambient::tabHelp(t.m_name.toRawUTF8()));
                job.m_tabs[static_cast<size_t>(row)].push_back(t);
            }
        });
    }

    // ---- the gallery of source types, on Source 2, one type per pair of steps. The clip for
    // the Texture, Stretch and Spectral types comes from AMBIENT_MANUAL_CLIP; without it those
    // three show an empty display, which is at least honest.
    {
        const int sourcesRow = 1;
        auto* typeParam = m_proc.m_apvts.getParameter("src2_type");
        const juce::String clip = juce::SystemStats::getEnvironmentVariable("AMBIENT_MANUAL_CLIP", "");
        job.m_steps.push_back([this, typeParam, &job] {
            job.m_originalType = typeParam != nullptr ? juce::String(typeParam->getValue()) : juce::String();
        });
        for (const char* type : { "Additive", "Harmonic", "Wavetable", "FM", "Texture", "Stretch", "Bow", "Spectral", "Noise" }) {
            const juce::String typeName(type);
            int index = -1;
            for (int i = 0; i < ambient::NumSourceTypes; ++i) if (typeName == ambient::SourceTypeNames[i]) index = i;
            if (index < 0 || typeParam == nullptr) continue;
            job.m_steps.push_back([this, typeParam, index, typeName, clip] {
                if ((typeName == "Texture" || typeName == "Stretch" || typeName == "Spectral") && clip.isNotEmpty())
                    m_proc.loadTextureFile(1, juce::File(clip));
                typeParam->setValueNotifyingHost(typeParam->convertTo0to1(static_cast<float>(index)));
                if (auto* lvl = m_proc.m_apvts.getParameter("src2_level")) lvl->setValueNotifyingHost(lvl->convertTo0to1(0.5f));
            });
            job.m_steps.push_back([this, typeName, sourcesRow, stem, writePng, &job] {
                m_help->m_topics.selectRow(sourcesRow);
                m_help->showTopic(sourcesRow);
                ManualJob::Tab t;
                t.m_name = "TYPE " + typeName;
                t.m_caption = "Source 2 switched to the " + typeName + " type: the knobs that type uses are lit, the rest greyed out, and the display shows "
                          + (typeName == "Additive" ? juce::String("the partials of its bank.")
                           : typeName == "Harmonic" ? juce::String("the current frame of the table, drawn from its spectrum.")
                           : typeName == "Wavetable" ? juce::String("the current frame as the samples it is, its neighbours faint behind it.")
                           : typeName == "FM" ? juce::String("the modulated waveform of the pair.")
                           : typeName == "Texture" ? juce::String("the loaded clip with the grains reading it.")
                           : typeName == "Stretch" ? juce::String("the loaded clip with the stretched read position crawling through it.")
                           : juce::String("the spectrum of the noise colour."));
                t.m_file = writePng(snapshotTab(0, 1), stem(sourcesRow) + "-type-" + typeName.toLowerCase() + ".png");
                if (t.m_file.isEmpty()) return;
                t.m_sections.add("Source 2");
                t.m_blurb = juce::CharPointer_UTF8(ambient::tabHelp(t.m_name.toRawUTF8()));
                job.m_tabs[static_cast<size_t>(sourcesRow)].push_back(t);
            });
        }
        job.m_steps.push_back([typeParam, &job] {
            if (typeParam != nullptr && job.m_originalType.isNotEmpty()) typeParam->setValueNotifyingHost(job.m_originalType.getFloatValue());
        });
    }

    // ---- the last step: the file that names it all, and the cover pictures
    job.m_steps.push_back([this, last, dir, writePng, &job] {
        juce::String json = "{\n  \"topics\": [\n";
        for (int row = 0; row <= last; ++row) {
            const juce::String title = row < last ? juce::String(ambient::helpTopicTitle(row)) : "All parameters";
            const juce::String body  = row < last ? juce::String(juce::CharPointer_UTF8(ambient::helpTopicText(row)))
                                                  : m_help->m_parameters;
            json << "    { \"title\": " << juce::JSON::toString(juce::var(title))
                 << ", \"text\": " << juce::JSON::toString(juce::var(body))
                 << ", \"images\": [";
            const auto& files = job.m_images[static_cast<size_t>(row)];
            for (size_t i = 0; i < files.size(); ++i)
                json << (i ? ", " : "") << "{ \"file\": " << juce::JSON::toString(juce::var(files[i].m_file))
                     << ", \"caption\": " << juce::JSON::toString(juce::var(files[i].m_caption)) << " }";
            json << "], \"tabs\": [";
            const auto& tabs = job.m_tabs[static_cast<size_t>(row)];
            for (size_t i = 0; i < tabs.size(); ++i) {
                json << (i ? ", " : "") << "{ \"file\": " << juce::JSON::toString(juce::var(tabs[i].m_file))
                     << ", \"name\": " << juce::JSON::toString(juce::var(tabs[i].m_name))
                     << ", \"caption\": " << juce::JSON::toString(juce::var(tabs[i].m_caption))
                     << ", \"blurb\": " << juce::JSON::toString(juce::var(tabs[i].m_blurb))
                     << ", \"sections\": [";
                for (int k = 0; k < tabs[i].m_sections.size(); ++k) json << (k ? ", " : "") << juce::JSON::toString(juce::var(tabs[i].m_sections[k]));
                json << "] }";
            }
            json << "] }" << (row < last ? ",\n" : "\n");
        }
        // Every parameter, structured, so the manual can print each tab's own under its picture.
        json << "  ],\n  \"params\": [\n";
        bool first = true;
        for (const ParamDesc& d : paramTable()) {
            juce::String range;
            if (d.m_kind == ParamKindE::CHOICE) { for (int i = 0; i < d.m_numChoices; ++i) range += (i ? " / " : "") + juce::String(d.m_choices[i]); }
            else if (d.m_kind == ParamKindE::BOOL) range = "on / off";
            else range = juce::String(d.m_min, d.m_kind == ParamKindE::INT ? 0 : 2) + " .. " + juce::String(d.m_max, d.m_kind == ParamKindE::INT ? 0 : 2) + (d.m_unit[0] ? juce::String(" ") + d.m_unit : juce::String());
            json << (first ? "" : ",\n") << "    { \"section\": " << juce::JSON::toString(juce::var(juce::String(d.m_section)))
                 << ", \"name\": " << juce::JSON::toString(juce::var(juce::String(d.m_name)))
                 << ", \"key\": " << juce::JSON::toString(juce::var(juce::String(d.m_key)))
                 << ", \"range\": " << juce::JSON::toString(juce::var(range))
                 << ", \"help\": " << juce::JSON::toString(juce::var(juce::String(juce::CharPointer_UTF8(ambient::paramHelp(d.m_id))))) << " }";
            first = false;
        }
        json << "\n  ],\n  \"version\": " << juce::JSON::toString(juce::var(juce::String(JucePlugin_VersionString)))
             << ",\n  \"shapes\": " << ambient::ZShapes
             << ",\n  \"presets\": " << numPresets()
             << ",\n  \"cosmos\": " << numCosmosPresets()
             << ",\n  \"zpresets\": " << numZPresets()
             << ",\n  \"strike\": " << numStrikePresets() << "\n}\n";
        dir.getChildFile("manual.json").replaceWithText(json);
        // Coverage: every page of every tab row must have been photographed exactly once. Written
        // as a file so make_manual.py can refuse to print a manual with a hole in it.
        juce::String coverage;
        for (int r = 0; r < static_cast<int>(m_tabRows.size()); ++r)
            for (int pg = 0; pg < static_cast<int>(m_tabRows[static_cast<size_t>(r)].m_names.size()); ++pg) {
                const juce::String nm = m_tabRows[static_cast<size_t>(r)].m_names[static_cast<size_t>(pg)];
                int seen = 0;
                for (const auto& chapter : job.m_tabs) for (const auto& t : chapter) if (t.m_name == nm) ++seen;
                coverage << nm << ": " << seen << "\n";
            }
        dir.getChildFile("coverage.txt").replaceWithText(coverage);

        // One picture of the whole panel, for the cover: the instrument as it actually looks.
        // And one of the header on its own, which is where the pages, the master and the
        // loudness meter live.
        setPage(0);
        resized();
        writePng(createComponentSnapshot(getLocalBounds(), true, 1.0f), "panel.png");
        writePng(createComponentSnapshot(getLocalBounds().withHeight(HeaderH), true, 2.0f), "header.png");
        if (job.m_done) job.m_done();
    });
    runManualStep();
}

void NoctuaryEditor::runManualStep()
{
    if (m_manual == nullptr || m_manual->m_next >= m_manual->m_steps.size()) return;
    m_manual->m_steps[m_manual->m_next++]();
    if (m_manual != nullptr && m_manual->m_next < m_manual->m_steps.size())
        juce::Timer::callAfterDelay(350, [this] { runManualStep(); });
}

void NoctuaryEditor::HelpView::showTopic(int row)
{
    m_topic = row;
    m_pics.clear();
    m_picCaptions.clear();
    m_tabPics.clear();
    m_tabNames.clear();
    m_tabCaptions.clear();
    m_tabSections.clear();
    m_live.reset();
    m_flow.setVisible(row == 0);
    // Which sections and which live display belong to a topic. The order follows Topics in
    // Help.cpp. `sections` are the pictures the help page itself shows, at most two or three:
    // the column they are stacked in runs out of height after that.
    struct Spec { std::vector<juce::String> m_sections; int m_liveKind; };   // liveKind: 0 none, 1 source, 2 filter, 3 stage, 4 cosmos, 5 brain, 6 env
    static const Spec Specs[] = {
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
    static const char* const LiveCaption[7] = {
        "", "The live display of Source 1: the partials of the loudest voice, exactly the amplitudes the oscillator is summing right now.",
        "The live filter response: the voice filter, the z-plane, and what a note actually meets after both, from the same maths the audio path uses.",
        "The live stage: every sounding voice as a dot at its distance from the ear, the far plane at the back.",
        "The live Cosmos display: the shifter, the resonator and the vowel as the section currently has them.",
        "The live conductor: the brain's notes, their holds and where on the planes it has put them.",
        "The live envelope: the six modulation envelopes' shapes and where each one's clock currently is.",
    };
    const int n = static_cast<int>(sizeof(Specs) / sizeof(Specs[0]));
    // The sections and the live display, for the help page and the manual both.
    if (row >= 0 && row < n) {
        for (const auto& name : Specs[row].m_sections) {
            juce::Image img = m_owner.snapshotSection(name);
            if (!img.isValid()) continue;
            m_pics.push_back(img);
            m_picCaptions.add("The " + name + " section, with the values of the preset the manual was exported from.");
        }
        switch (Specs[row].m_liveKind) {
        case 1: m_live = std::make_unique<SourceView>(m_proc, 1); break;
        case 2: m_live = std::make_unique<FilterView>(m_proc); break;
        case 3: m_live = std::make_unique<StageView>(m_proc); break;
        case 4: m_live = std::make_unique<CosmosView>(m_proc); break;
        case 5: m_live = std::make_unique<BrainView>(m_proc); break;
        case 6: m_live = std::make_unique<EnvView>(m_proc); break;
        default: break;
        }
        m_liveCaption = LiveCaption[Specs[row].m_liveKind];
        if (m_live) addAndMakeVisible(*m_live);
    }
    // The tabs, for the manual only. Every page of every tab row belongs to exactly one chapter
    // -- by its row, with three exceptions -- and is enumerated from the rows themselves, so a
    // tab that is added to the panel is in the manual the next time it is exported, and one
    // cannot be forgotten by a list written by hand. (One was, twice.)
    auto addTab = [&](juce::Image img, const juce::String& name, const juce::String& caption, juce::StringArray sections) {
        if (!img.isValid()) return;
        m_tabPics.push_back(img); m_tabNames.add(name); m_tabCaptions.add(caption); m_tabSections.push_back(sections);
    };
    static const int RowChapter[] = { 1, 2, 4, 4, 5, 6 };    // tab row -> chapter
    for (int r = 0; r < static_cast<int>(m_owner.m_tabRows.size()); ++r) {
        const auto& t = m_owner.m_tabRows[static_cast<size_t>(r)];
        for (int pg = 0; pg < static_cast<int>(t.m_pages.size()); ++pg) {
            int chapter = r < 6 ? RowChapter[r] : 4;
            if (r == 1 && pg == 2) chapter = 7;      // ENVELOPE + EXPRESSION with the modulation
            if (chapter != row) continue;
            const juce::StringArray secs = m_owner.tabSectionNames(r, pg);
            juce::String what = secs.joinIntoString(", ");
            juce::String caption = "The " + t.m_names[static_cast<size_t>(pg)] + " tab as it opens: the " + what
                                 + (secs.size() > 1 ? " sections" : " section")
                                 + " with the values of the preset the manual was exported from"
                                 + (pg < static_cast<int>(t.m_displays.size()) && t.m_displays[static_cast<size_t>(pg)] != nullptr
                                        ? ", and beside them the tab's live display." : ".");
            addTab(m_owner.snapshotTab(r, pg), t.m_names[static_cast<size_t>(pg)], caption, secs);
        }
    }
    if (row == 1)   // the strand bank, beside Source 1 on its page
        addTab(m_owner.snapshotSection("Strands"), "STRANDS",
               "The Strands section, which stands beside Source 1 on its page while the slot is additive: the strand bank's ten controls.", { "Strands" });
    if (row == 3) {
        addTab(m_owner.snapshotSection("Space"), "SPACE", "The Space section: the spatial model's controls, two rows side by side.", { "Space" });
        addTab(m_owner.snapshotSection("Foundation"), "FOUNDATION", "The Foundation section: the sub and its source.", { "Foundation" });
    }
    if (row == 4)   // the master, in its corner of the header
        addTab(m_owner.snapshotHeader(), "MASTER",
               "The right-hand half of the header: the note roll (every sounding note as a bar, the keys' notes lit), the loudness meter under it, the Master section (tilt, bass mono, side air, width, mono safe, subsonic) and the master gain knob.", { "Master" });
    if (row == 7) {
        static const char* const Strip[3] = { "LFO", "ENV", "MATRIX" };
        static const char* const StripCap[3] = {
            "The modulation strip along the bottom of the page, LFO tab: the eight LFO cards, each a shape, a rate, a phase, a depth and a mode. A card is dragged onto a knob to route it.",
            "The strip's ENV tab: the six envelope cards and the shape editor, where an envelope is drawn as points on a curve.",
            "The strip's MATRIX tab: every route as a row -- source, target, depth, via, and the 0..1 flag." };
        static const char* const StripSection[3] = { "LFO 1", "Env 1", "" };
        static const char* const StripDetail[3] = {
            "The left third of the LFO tab at readable size: the source cards -- LFO 1 to 4 are the first four -- and LFO 1's own panel: Shape, Rate, Phase, Depth, Mode, Sync.",
            "The left third of the ENV tab at readable size: the envelope editor of Env 1 -- points on a curve, dragged; double-click adds or removes one -- with its Mode, Time, Depth and Sync.",
            "The left third of the MATRIX tab at readable size: the first routes, each a source, a target, a depth slider, a Via source and the 0..1 flag, and the '+ route' button." };
        for (int t = 0; t < 3; ++t) {
            juce::StringArray secs; if (StripSection[t][0]) secs.add(StripSection[t]);
            addTab(m_owner.snapshotStripTab(t), Strip[t], StripCap[t], {});
            addTab(m_owner.snapshotStripTab(t, true), juce::String(Strip[t]) + " (detail)", StripDetail[t], secs);
        }
    }
    if (row == 8)
        addTab(m_owner.snapshotPerform(), "PERFORM",
               "The Perform page: the macros large, the morph and the map cursor, the note roll and the stage, and the set recorder.", {});
    if (row == 9) {
        addTab(m_owner.snapshotBrowse(), "BROWSE",
               "The Browse page in its list view: the columns that narrow the library (family, character, motion, features), the search, and the list of every preset the instrument knows.", {});
        addTab(m_owner.snapshotBrowseMap(), "MAP",
               "The Browse page in its map view: every preset as a point, clustered by what it sounds like; the cursor and its radius, and the Map blend switch that makes the space between presets playable.", { "Map" });
        addTab(m_owner.snapshotBrowseMapZoomed(), "MAP, ZOOMED IN",
               "The same map closed in on the current preset at eight times, with the wheel or with \"more like this\": the cluster it sits in opens into single presets, and the ones with room for a name show it. Drag to move about, double-click to see the whole plane again.", { "Map" });
    }
    resized();
    repaint();
}

int NoctuaryEditor::HelpView::getNumRows() { return ambient::numHelpTopics() + 1; }

void NoctuaryEditor::HelpView::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (selected) { g.setColour(ui::Accent.withAlpha(0.22f)); g.fillRoundedRectangle(2.0f, 1.0f, static_cast<float>(w - 4), static_cast<float>(h - 2), 4.0f); }
    g.setColour(selected ? ui::Text : ui::Dim);
    g.setFont(ui::body(12.5f));
    const juce::String title = row < ambient::numHelpTopics() ? ambient::helpTopicTitle(row) : "All parameters";
    g.drawText(title, 10, 0, w - 14, h, juce::Justification::centredLeft, true);
}

void NoctuaryEditor::HelpView::selectedRowsChanged(int row)
{
    if (row < 0) return;
    m_text.setText(row < ambient::numHelpTopics() ? juce::String(juce::CharPointer_UTF8(ambient::helpTopicText(row))) : m_parameters, false);
    m_text.moveCaretToTop(false);
    showTopic(row);
}

juce::Rectangle<int> NoctuaryEditor::HelpView::FlowDiagram::drawn() const
{
    const float sc = juce::jmin(getWidth() / CanvasW, getHeight() / CanvasH);
    if (sc <= 0.0f) return getLocalBounds();
    return juce::Rectangle<int>(0, 0, juce::roundToInt(CanvasW * sc), juce::roundToInt(CanvasH * sc));
}

/**
 * The signal flow as a picture: the units as boxes in their group colours, the buses as arrows.
 */
void NoctuaryEditor::HelpView::FlowDiagram::paint(juce::Graphics& g)
{
    // Drawn on a fixed canvas, scaled to fit whatever the column offers.
    const float sx = getWidth() / CanvasW, sy = getHeight() / CanvasH, sc = juce::jmin(sx, sy);
    if (sc <= 0.05f) return;
    g.addTransform(juce::AffineTransform::scale(sc));
    auto node = [&](float x, float y, float w, float h, const juce::String& t, juce::Colour c, float fs = 12.0f) {
        juce::Rectangle<float> r(x, y, w, h);
        g.setColour(ui::CardColour); g.fillRoundedRectangle(r, 6.0f);
        g.setColour(c.withAlpha(0.9f)); g.drawRoundedRectangle(r.reduced(0.5f), 6.0f, 1.2f);
        g.setColour(ui::Text); g.setFont(ui::body(fs));
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

    const juce::Colour V = ui::VoiceCol, F = ui::ForeCol, B = ui::BackCol, C = ui::CosmosCol, K = ui::CondCol, M = ui::MasterCol, A = ui::Accent;
    // conductors
    auto brain = node(20, 14, 150, 36, "Cluster Brain", K);
    auto keys  = node(185, 14, 120, 36, "MIDI keys", K);
    auto hands = node(320, 14, 130, 36, "OSC / hands / macros", K, 11.0f);
    label(20, 54, "every note gets a DISTANCE: 0 at the ear, 1 the infinite background", ui::Dim);
    // The types, once, where there is room for them: every slot of the voice and the near source
    // alike may be set to any of these, so naming them in five boxes would be the same list five
    // times. The boxes below say "any type" and mean this.
    g.setColour(V.withAlpha(0.30f)); g.drawRoundedRectangle(470.0f, 12.0f, 510.0f, 62.0f, 8.0f, 1.0f);
    g.setColour(V); g.setFont(ui::title(9.5f));
    g.drawText("SOURCE TYPES  --  any slot, any of them", 478, 15, 400, 12, juce::Justification::centredLeft, false);
    g.setColour(ui::Dim); g.setFont(ui::body(8.0f));
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
    label(150, 332, "splits by distance:  near = cos(d),  far = sin(d)", ui::Dim);
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
    label(500, 236, "send / return -- added, never replacing", ui::Dim);
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
    label(500, 478, "no compressor anywhere: what you hear is the dynamics of the drone", ui::Dim);
    // feedback, modulation
    arrow({ 500, 470 }, { 120, 470 }, A, true); arrow({ 120, 470 }, { 120, 312 }, A, true);
    label(130, 474, "Feedback: to bus / to pitch (phase-modulates every partial), tape", A);
    node(20, 500, 946, 34, "Modulation: 8 LFOs  -  6 envelopes  -  aftertouch, wheel, slide  -  the note, its velocity, its distance  -  matrix -> any knob", A, 10.0f);
    juce::ignoreUnused(ens, d1, d2, nr, cos, cloud, fr, rm, sh, out1, out2, env, filt, zp, s4, vec, air, blur, ne, ns, nty, nrt);
}

bool NoctuaryEditor::headsetShown() const
{
    const auto mode = frame::Settings::of("Noctuary").headset();
    return mode == frame::Settings::HeadsetModeE::ON
        || (mode == frame::Settings::HeadsetModeE::AUTO && const_cast<NoctuaryProcessor&>(m_proc).gestures().handsRecently(3.0));
}

void NoctuaryEditor::showSettings()
{
    frame::SettingsMenu m;
    m.m_app = "Noctuary";
    m.m_version = JucePlugin_VersionString;
    m.m_headsetOffText = "Off: hide its controls (OSC stays on for other controllers)";
    if (m_proc.wrapperType == juce::AudioProcessor::wrapperType_Standalone)   // Ableton Link, the standalone only (02.10.2026)
        m.m_linkStatus = [this] { return m_proc.linkStatus(); };
    m.m_cueStatus = [this] { return m_proc.cueStatus(); };   // score cues for a visualiser, plugin and standalone (02.10.2026)
    m.m_jamStatus = [this] { return m_proc.jamStatus(); };   // the family jam (02.10.2026)
    m.m_headsetStatus = [this] {
        if (!m_proc.oscRunning()) return juce::String("OSC off: ") + m_proc.oscError();
        return juce::String(m_proc.gestures().handsRecently(3.0) ? "a headset sends its hands" : "no headset sends")
             + " (OSC :" + juce::String(m_proc.oscPort()) + ")";
    };
    m.m_headsetItems = [this](juce::PopupMenu& h) {
        h.addItem("Calibrate hands (6 s: together and apart, low and high, near and far)", [this] { m_proc.gestures().startCalibration(6.0f); });
        h.addItem("Gestures ... (the gesture / macro mapping table)", [this] { showMappingEditor(); });
    };
    m.m_moreItems = [this](juce::PopupMenu& menu) {
        juce::PopupMenu layout;
        const int mode = m_proc.layoutMode();
        static const char* const Modes[3] = { "Normal: one page with tabs", "Compact: narrower columns, taller sections",
                                               "Expanded: every page laid out under one another" };
        for (int i = 0; i < 3; ++i) layout.addItem(Modes[i], true, mode == i, [this, i] { applyLayoutMode(i); });
        menu.addSubMenu("Layout", layout);
        if (NoctuaryProcessor::sessionRecallAvailable())
            menu.addItem("Start where you left off (session recall)", true, m_proc.sessionRecall(), [this] {
                m_proc.setSessionRecall(!m_proc.sessionRecall());
                if (m_recallButton) m_recallButton->setToggleState(m_proc.sessionRecall(), juce::dontSendNotification);
            });
    };
    {
        auto more = m.m_moreItems;
        m.m_moreItems = [this, more](juce::PopupMenu& menu) {
            if (more) more(menu);
            menu.addItem("Night: the window dim and warm", true, m_night, [this] { setNight(!m_night); });
            menu.addItem("Journeys over time ...", [this] { if (m_journeyPanel != nullptr) m_journeyPanel->open(); });
            menu.addItem("Search: any knob, preset or action (Ctrl+K) ...", [this] { openPalette(); });
            menu.addItem("First steps (the tour) ...", [this] { startTour(); });
        };
    }
    m.m_showAbout = [this] { if (m_aboutButton && m_aboutButton->onClick) m_aboutButton->onClick(); };
    m.show(*m_settingsIcon);
}

void NoctuaryEditor::timerCallback()
{
    // The first steps on the first start (not in an automated run).
    if (m_tourTicks >= 0 && ++m_tourTicks > 20) {
        m_tourTicks = -1;
        if (!automated() && frame::Settings::of("Noctuary").value("tourDone").isEmpty()) startTour();
    }
    // The star beside the Sound box: the knobs have moved from what the preset put (taken a tick after it came).
    if (m_star != nullptr && !m_holding) {
        const int idx = m_proc.soundPresetIndex();
        if (idx != m_presetSeen) {
            m_presetSeen = idx;
            m_presetSnap = takeSnapshot("preset");
            m_star->setText({}, juce::dontSendNotification);
        } else if (static_cast<int>(m_presetSnap.m_v.size()) == NumParams) {
            const Snapshot now = takeSnapshot({});
            bool moved = false;
            for (int i = 0; i < NumParams && !moved; ++i) {
                if (juce::String(paramTable()[static_cast<size_t>(i)].m_section) == "Master") continue;   // the level is the room's, not the sound's
                const float a = now.m_v[static_cast<size_t>(i)], b = m_presetSnap.m_v[static_cast<size_t>(i)];
                moved = std::abs(a - b) > 1.0e-4f * (1.0f + std::abs(b));
            }
            if (m_star->getText().isNotEmpty() != moved) m_star->setText(moved ? "*" : "", juce::dontSendNotification);
        }
    }
    m_proc.engine().soundingNotes(m_sounding);
    {   // The frame's tools (01.10.2026): help lit while its page is up; VR while a headset sends (or when asked).
        if (m_helpIcon) m_helpIcon->setToggleState(m_help && m_help->isVisible(), juce::dontSendNotification);
        const bool hs = headsetShown();
        if (m_vrButton && m_vrButton->isVisible() != hs) m_vrButton->setVisible(hs);
    }
    {   // Auto switched on (the toggle, or the host): the preset that is playing brings its foreground now.
        const bool autoOn = m_proc.engine().getParam(ParamIdE::FORE_AUTO) >= 0.5f;
        if (autoOn && !m_autoSeen) m_proc.nearAutoNow();
        m_autoSeen = autoOn;
        // The box says what Auto (or a state) put there, without the change counting as a hand's.
        if (m_nearPresetBox != nullptr && m_proc.nearPresetIndex() >= 0 && m_nearPresetBox->getSelectedId() != m_proc.nearPresetIndex() + 1)
            m_nearPresetBox->setSelectedId(m_proc.nearPresetIndex() + 1, juce::dontSendNotification);
        if (m_journeyStatus != nullptr) {
            const juce::String s = m_proc.journeyRunning() ? m_proc.journeyStatus()
                                 : (m_proc.journey().m_steps.empty() ? juce::String() : juce::String(static_cast<int>(m_proc.journey().m_steps.size())) + " steps written");
            if (m_journeyStatus->getText() != s) m_journeyStatus->setText(s, juce::dontSendNotification);
        }
    }
    {   // Mark what is moving each knob right now. The matrix, in its source's colour, as before;
        // and everything else that plays a value the knob does not show -- the morph between two
        // presets, the map's blend, a route -- as a neutral arc from the knob's value to the live
        // one. The slow processes with a state of their own (the arc, the tide) get a dot on the
        // outer ring that says where in their swing they are: the panel's answer to a modulation
        // too slow for the eye to catch as motion.
        const ambient::ModMatrix& m = m_proc.engine().modMatrix();
        for (auto& c : m_cells) {
            auto* sl = dynamic_cast<juce::Slider*>(c.m_comp.get());
            if (sl == nullptr || c.m_param < 0) continue;
            const ParamIdE id = static_cast<ParamIdE>(c.m_param);
            int colour = 0;
            for (int k = 0; k < m.count(); ++k) {
                if (static_cast<int>(m.route(k).m_target) != c.m_param) continue;
                using MST = ambient::ModSourceE;
                const int si = static_cast<int>(m.route(k).m_source);
                juce::Colour col = ui::CosmosCol;
                if (si >= static_cast<int>(MST::LFO1) && si <= static_cast<int>(MST::LFO8)) col = ui::Accent;
                else if (si >= static_cast<int>(MST::ENV1) && si <= static_cast<int>(MST::ENV6)) col = ui::ForeCol;
                else if (si >= static_cast<int>(MST::MACRO_A) && si <= static_cast<int>(MST::MACRO_H)) col = ui::MorphCol;
                else if (si >= static_cast<int>(MST::KURA1) && si <= static_cast<int>(MST::KURA4)) col = ui::CondCol;
                else if (si >= static_cast<int>(MST::LENIA1) && si <= static_cast<int>(MST::LENIA4)) col = ui::BackCol;
                colour = static_cast<int>(col.getARGB());
                break;
            }
            const ParamDesc& d = paramDesc(id);
            const float range = juce::jmax(1.0e-6f, d.m_max - d.m_min);
            const float raw = m_proc.engine().getParam(id);
            const float live = m_proc.engine().effectiveParam(id) + m_proc.engine().modAmount(id);
            const float off = (live - raw) / range;
            bool changed = false;
            auto setP = [&](const char* key, const juce::var& v) { if (!sl->getProperties().contains(key) || sl->getProperties()[key] != v) { sl->getProperties().set(key, v); changed = true; } };
            auto remP = [&](const char* key) { if (sl->getProperties().contains(key)) { sl->getProperties().remove(key); changed = true; } };
            if (colour != 0) { setP("modColour", colour); setP("modOffset", static_cast<double>(off)); remP("liveOffset"); }
            else {
                remP("modColour"); remP("modOffset");
                if (std::fabs(off) > 1.0e-4f) setP("liveOffset", static_cast<double>(off)); else remP("liveOffset");
            }
            if (id == ParamIdE::ARC_AMOUNT) {
                // Following the clock, the knob wears a dial: the hour on a full circle, with the
                // turning points of the day marked. Drifting, a dot for where the swing stands.
                if (m_proc.engine().arcClockOn()) { setP("clockHour", m_proc.engine().clockHour()); remP("halo"); }
                else { setP("halo", static_cast<double>(m_proc.engine().arcNow())); remP("clockHour"); }
            }
            else if (id == ParamIdE::TIDE) setP("halo", static_cast<double>(juce::jlimit(-1.0f, 1.0f, m_proc.engine().tideNow() / juce::jmax(1.0f, raw))));
            if (changed) sl->repaint();
        }
    }
    if (m_mapOpen) {
        if (m_mapEditor != nullptr) m_mapText = m_mapEditor->getText();
        else {
            m_mapOpen = false;
            if (!m_proc.setGestureMappings(m_mapText))
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Mappings", "A line could not be parsed; the previous table is kept.");
        }
    }
    if (m_soundBox && m_soundBox->getSelectedId() != m_proc.soundPresetIndex() + 1)
        m_soundBox->setSelectedId(m_proc.soundPresetIndex() + 1, juce::dontSendNotification);
    // The three section boxes follow the processor rather than the other way round, so a layer
    // loaded from a program change, from OSC or by a whole state coming back shows up in them --
    // and so does a sound preset clearing them, which it does, because a sound preset brings its
    // own filter and its own pluck and the name of a layer preset would then be a lie.
    if (m_cosmosBox && m_cosmosBox->getSelectedId() != m_proc.cosmosPresetIndex() + 1)
        m_cosmosBox->setSelectedId(m_proc.cosmosPresetIndex() + 1, juce::dontSendNotification);
    if (m_zPresetBox && m_zPresetBox->getSelectedId() != m_proc.zPresetIndex() + 1)
        m_zPresetBox->setSelectedId(m_proc.zPresetIndex() + 1, juce::dontSendNotification);
    if (m_strikePresetBox && m_strikePresetBox->getSelectedId() != m_proc.strikePresetIndex() + 1)
        m_strikePresetBox->setSelectedId(m_proc.strikePresetIndex() + 1, juce::dontSendNotification);
    const int learn = m_proc.learnTarget();
    for (auto& c : m_cells) {
        if (c.m_param < 0) continue;
        const int cc = m_proc.midiCcFor(static_cast<ParamIdE>(c.m_param));
        juce::String text = c.m_baseLabel;
        if (learn == c.m_param) text += " - learn";
        else if (cc >= 0) text += " - CC" + juce::String(cc);
        if (c.m_label->getText() != text) {
            c.m_label->setText(text, juce::dontSendNotification);
            c.m_label->setColour(juce::Label::textColourId, (cc >= 0 || learn == c.m_param) ? Accent : Dim);
        }
    }
    updateSourceCells();
    repaint(m_header);
    if (Section* m = findSection("Morph")) m_content.repaint(m->m_bounds.withTrimmedTop(-GroupTitleH));
}

int NoctuaryEditor::cellForParam(ParamIdE id) const
{
    for (int i = 0; i < static_cast<int>(m_cells.size()); ++i) if (m_cells[static_cast<size_t>(i)].m_param == static_cast<int>(id)) return i;
    return -1;
}

/**
 * The Near Source's cells by its type, the way updateSourceCells does it for the four slots: what
 * the chosen type ignores is gone from the strip, not greyed.
 */
bool NoctuaryEditor::updateNearCells()
{
    enum { OFF = 0, HARMONIC = 1, FM = 2, TEXTURE = 3, NOISE = 4, ADDITIVE = 5, STRETCH = 6, BOW = 7, SPECTRAL = 8, WAVETABLE = 9,
           FLUTE = 10, MURMUR = 11, BOWL = 12, ICE = 13, DROPS = 14, CLIP = 15,
           WHISTLER = 16, SHAKER = 17, CHIME = 18, GEIGER = 19, TUBE = 20, KRELL = 21, BEACON = 22, MORSE = 23, DIAL = 24 };
    const int type = static_cast<int>(std::lround(m_proc.engine().getParam(ParamIdE::FORE_TYPE)));
    const bool rubbed = type == BOWL || type == ICE, near = type >= FLUTE && type <= DROPS, signal = type >= WHISTLER;
    struct Rule { ParamIdE m_id; bool m_on; };
    const Rule rules[] = {
        { ParamIdE::FORE_OCTAVE,   true },
        { ParamIdE::FORE_RATIO,    true },
        { ParamIdE::FORE_POSITION, type == HARMONIC || type == WAVETABLE || type == TEXTURE || type == NOISE || type == STRETCH || type == BOW || type == SPECTRAL || near || type == CLIP
                                 || type == SHAKER || type == GEIGER || type == TUBE || type == KRELL || type == MORSE || type == DIAL },
        { ParamIdE::FORE_POS_DRIFT, type != CLIP && (!signal || type == CHIME) },
        { ParamIdE::FORE_DENSITY,  type == TEXTURE || type == NOISE || type == DROPS || type == SHAKER || type == GEIGER || type == BEACON },
        { ParamIdE::FORE_FOLLOW,   type == TEXTURE || type == NOISE || type == STRETCH || type == SPECTRAL || type == DROPS || type == CLIP || type == SHAKER },
        { ParamIdE::FORE_BRIGHT,   type == ADDITIVE || type == BOW || type == SPECTRAL || near || type == WHISTLER || type == CHIME || type == TUBE || type == MORSE || type == DIAL },
        { ParamIdE::FORE_FORCE,    type == BOW || type == FLUTE || type == MURMUR || rubbed || type == SHAKER || type == CHIME || type == GEIGER },
        { ParamIdE::FORE_SPEED,    type == BOW || type == FLUTE || type == MURMUR || rubbed || type == WHISTLER || type == KRELL || type == MORSE },
        { ParamIdE::FORE_NOISE,    type == NOISE },
        { ParamIdE::FORE_NOISE_Q,   type == NOISE || type == SHAKER || type == GEIGER },
        { ParamIdE::FORE_FM_RATIO,  type == FM || type == KRELL },
        { ParamIdE::FORE_FM_INDEX,  type == FM || type == KRELL },
        { ParamIdE::FORE_PARTIALS, type == ADDITIVE },
        { ParamIdE::FORE_TILT,     type == ADDITIVE || type == CHIME },
        { ParamIdE::FORE_INHARM,   type == ADDITIVE },
        { ParamIdE::FORE_DRIFT,    type != OFF && type != NOISE && type != DROPS && type != CLIP && !signal },
        { ParamIdE::FORE_TABLE,    type == HARMONIC || type == WAVETABLE },
    };
    bool changed = false;
    for (const Rule& r : rules) {
        const int ci = cellForParam(r.m_id);
        if (ci < 0) continue;
        Cell& c = m_cells[static_cast<size_t>(ci)];
        if (c.m_unused != !r.m_on) { c.m_unused = !r.m_on; c.m_comp->setEnabled(r.m_on); changed = true; }
    }
    if (m_nearClipCell >= 0) {   // the clip button belongs to the types that read a recording
        Cell& cc = m_cells[static_cast<size_t>(m_nearClipCell)];
        const bool on = type == CLIP || type == TEXTURE || type == STRETCH || type == SPECTRAL;
        if (cc.m_unused != !on) { cc.m_unused = !on; changed = true; }
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
    enum { OFF = 0, HARMONIC = 1, FM = 2, TEXTURE = 3, NOISE = 4, ADDITIVE = 5, STRETCH = 6, BOW = 7, SPECTRAL = 8, WAVETABLE = 9,
           FLUTE = 10, MURMUR = 11, BOWL = 12, ICE = 13, DROPS = 14, CLIP = 15,
           WHISTLER = 16, SHAKER = 17, CHIME = 18, GEIGER = 19, TUBE = 20, KRELL = 21, BEACON = 22, MORSE = 23, DIAL = 24 };
    // The ids come from Params.h (slotParamIds), so this list and the engine's cannot drift apart.
    // Slots, not a number: a literal 3 here quietly left Source 4's cells lit whatever its type.
    bool cellsChanged = false;
    for (int k = 0; k < ambient::Slots; ++k) {
        const ParamIdE* ids = slotParamIds(k);
        const int type = static_cast<int>(std::lround(m_proc.engine().getParam(ids[0])));
        const bool rubbed = type == BOWL || type == ICE, near = type >= FLUTE && type <= DROPS, signal = type >= WHISTLER;
        // Every field, not the first 33: the loop used to stop at Transport, and everything added after
        // it -- Interp, Unison and its detune and width, Root -- stood lit for every type, Root on a clip.
        for (int off = 1; off < ambient::SlotFields; ++off) {
            bool on = type != OFF;
            switch (off) {
            case 5:  on = type == HARMONIC || type == WAVETABLE; break;         // table choice
            case 6:  on = type == HARMONIC || type == WAVETABLE || type == TEXTURE || type == NOISE || type == STRETCH || type == BOW || type == SPECTRAL || near || type == CLIP
                          || type == SHAKER || type == GEIGER || type == TUBE || type == KRELL || type == MORSE || type == DIAL; break;   // position, or where the bow sits, the embouchure, the stick, the medium, the vessel, where the clip starts, the shell, the tube, the mains, the quantiser, the letters, the whistles
            case 7:  on = type != OFF && (!signal || type == CHIME); break;      // pos drift moves all of them; of the signals only the chime's split
            case 8:
            case 9:  on = type == FM || type == KRELL; break;
            case 10: on = type == TEXTURE || type == STRETCH; break;             // grain: the grain, or the spectral window
            case 13:
            case 14: on = type == TEXTURE; break;                                // grains, spread
            case 11: on = type == TEXTURE || type == NOISE || type == DROPS || type == SHAKER || type == GEIGER || type == BEACON; break;   // density: grains, crackle, drops, shakes, clicks, packets
            case 12: on = type == TEXTURE || type == NOISE || type == STRETCH || type == SPECTRAL || type == DROPS || type == CLIP || type == SHAKER; break;   // pitch follow
            case 15: on = type == NOISE; break;
            case 16: on = type == NOISE || type == SHAKER || type == GEIGER; break;   // noise q: the band, the shell's ring, the tube's damping
            case 19: on = type == ADDITIVE || type == BOW || type == SPECTRAL || near || type == WHISTLER || type == CHIME || type == TUBE || type == MORSE || type == DIAL; break;   // bright
            case 17: case 20: case 21: case 22: case 23: on = type == ADDITIVE; break;
            case 18: on = type == ADDITIVE || type == CHIME; break;               // tilt: the bank's, or the bell's hum
            case 24: on = type == TEXTURE || type == NOISE || type == DROPS || type == SHAKER || type == GEIGER || type == BEACON; break;   // density sync
            case 25: on = type != OFF && type != NOISE && type != DROPS && type != CLIP && !signal; break;   // pitch drift
            case 26:
            case 27: on = type == STRETCH; break;                                 // stretch, loop fade
            case 28: on = type == BOW || type == FLUTE || type == MURMUR || rubbed || type == SHAKER || type == CHIME || type == GEIGER; break;   // force: the bow, the breath, the effort, the stick, the beans, the ring, the cluster
            case 29: on = type == BOW || type == FLUTE || type == MURMUR || rubbed || type == WHISTLER || type == KRELL || type == MORSE; break;   // speed: the bow, the breath, the effort, the stick, the fall, the pace, the words a minute
            case 30:
            case 31: on = type == SPECTRAL; break;                                // spectral rate, breath
            case 32: on = type == HARMONIC; break;                                // transport: how the spectra morph
            case 36: on = type == TEXTURE; break;                                 // interpolation: how a clip is read
            case 37:
            case 38:
            case 39: on = type == HARMONIC || type == WAVETABLE; break;           // unison, its detune and width: the table types
            case 40: on = type == HARMONIC; break;                                // root: a floor under a spectrum's fundamental
            case 41: on = type != OFF; break;                                     // the role: which notes the slot sounds in
            default: break;
            }
            const int ci = cellForParam(ids[off]);
            if (ci < 0) continue;
            Cell& c = m_cells[static_cast<size_t>(ci)];
            // Not greyed: gone. The strip is packed from the cells the type uses, so its height
            // is the type's own and not the union of every type's.
            if (c.m_unused != !on) { c.m_unused = !on; c.m_comp->setEnabled(on); cellsChanged = true; }
        }
        // The slot's file button belongs to the types that read a clip; the wavetable button to the table.
        if (m_textureCell[k] >= 0) {
            Cell& tc = m_cells[static_cast<size_t>(m_textureCell[k])];
            const bool on = type == TEXTURE || type == STRETCH || type == SPECTRAL || type == CLIP;
            if (tc.m_unused != !on) { tc.m_unused = !on; cellsChanged = true; }
        }
        if (k == 1 && m_tableCell >= 0) {
            Cell& wc = m_cells[static_cast<size_t>(m_tableCell)];
            const bool on = type == HARMONIC || type == WAVETABLE;
            if (wc.m_unused != !on) { wc.m_unused = !on; cellsChanged = true; }
        }
    }
    if (Section* strands = findSection("Strands")) {   // the strand bank exists only while Source 1 is additive: not greyed, gone
        const bool on = std::lround(m_proc.engine().getParam(ParamIdE::SRC1_TYPE)) == ADDITIVE;
        for (int ci : strands->m_cells) {
            Cell& c = m_cells[static_cast<size_t>(ci)];
            if (c.m_unused != !on) { c.m_unused = !on; c.m_comp->setEnabled(on); cellsChanged = true; }
        }
    }
    if (updateNearCells()) cellsChanged = true;
    if (refreshLayoutState()) cellsChanged = true;
    if (cellsChanged && getWidth() > 0) rebuildLayout();   // at construction the size is not set yet; the first layout follows
    auto nameCell = [&](int ci, const juce::String& base, const juce::String& file) {
        if (ci < 0) return;
        Cell& c = m_cells[static_cast<size_t>(ci)];
        const juce::String text = file.isNotEmpty() ? file : base;
        if (c.m_label->getText() != text) c.m_label->setText(text, juce::dontSendNotification);
    };
    nameCell(m_tableCell, "User table", m_proc.wavetableName());
    for (int k = 0; k < ambient::Slots; ++k) nameCell(m_textureCell[k], "Texture file", m_proc.textureName(k));
    nameCell(m_impulseCell, "Dark Hall (built in)", m_proc.impulseName());
    nameCell(m_impulseBCell, "no second room", m_proc.impulseBName());
    nameCell(m_nearClipCell, "Source 4's clip", m_proc.nearClipName());
}

void NoctuaryEditor::showMappingEditor()
{
    // A plain text editor over the mapping table: one line per mapping,
    //   input param min max smooth deadzone clutch invert
    auto* editor = new juce::TextEditor();
    editor->setMultiLine(true, false);
    editor->setReturnKeyStartsNewLine(true);
    editor->setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
    editor->setText(m_proc.gestureMappings(), false);
    editor->setSize(640, 420);
    auto* content = new juce::Component();
    content->setSize(640, 470);
    content->addAndMakeVisible(editor);
    editor->setBounds(0, 0, 640, 420);
    auto* hint = new juce::Label(juce::String(), "input param min max [smooth] [deadzone] [clutch|none] [invert]   inputs: HandDistance LeftHeight RightHeight LeftForward RightForward LeftTilt RightTilt LeftPinch RightPinch HeadYaw HeadPitch HeadRoll Custom0..7 (= macros A..H)");
    hint->setFont(juce::FontOptions(11.0f));
    hint->setColour(juce::Label::textColourId, Dim);
    hint->setBounds(0, 424, 640, 44);
    hint->setMinimumHorizontalScale(0.5f);
    content->addAndMakeVisible(hint);
    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = "Gesture and macro mappings";
    o.content.setOwned(content);
    o.componentToCentreAround = this;
    o.dialogBackgroundColour = GroupFill;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
    // The timer mirrors the text while the dialog lives and applies it once the dialog is gone.
    m_mapEditor = editor;
    m_mapText = editor->getText();
    m_mapOpen = true;
}

void NoctuaryEditor::chooseSourceFile(bool wavetable, int slot)
{
    const juce::String what = wavetable ? juce::String("Load a wavetable (Serum, Vital, Hive, Surge .wt, WaveEdit, a single cycle)")
                            : slot >= 0 ? "Load a clip for Source " + juce::String(slot + 1)
                                        : juce::String("Load a texture sample");
    m_chooser = std::make_unique<juce::FileChooser>(what, juce::File(), wavetable ? "*.wav;*.wt;*.flac;*.aif;*.aiff"
                                                                                : "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
    m_chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this, wavetable, slot](const juce::FileChooser& fc) {
            const auto file = fc.getResult();
            if (!file.existsAsFile()) return;
            const bool ok = wavetable ? m_proc.loadWavetableFile(file)
                          : slot >= 0 ? m_proc.loadTextureFile(slot, file) : m_proc.loadTextureFile(file);
            if (!ok)
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, wavetable ? "Wavetable" : "Texture",
                    wavetable ? "Could not read this file as a wavetable (no whole cycle in it, or nothing but silence)." : "Could not read this audio file.");
            repaint();
        });
}

void NoctuaryEditor::chooseNearClipFile()
{
    m_chooser = std::make_unique<juce::FileChooser>("Load a recording for the near source", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
    m_chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& fc) {
            const auto file = fc.getResult();
            if (!file.existsAsFile()) return;
            if (!m_proc.loadNearClipFile(file))
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Near clip", "Could not read this audio file.");
            repaint();
        });
}

void NoctuaryEditor::chooseImpulseFile(bool second)
{
    m_chooser = std::make_unique<juce::FileChooser>(second ? "Load the second impulse response (Room Morph fades to it)"
                                                          : "Load an impulse response (mono or stereo)", juce::File(), "*.wav;*.aif;*.aiff;*.flac");
    m_chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this, second](const juce::FileChooser& fc) {
            const auto file = fc.getResult();
            if (!file.existsAsFile()) return;
            if (!m_proc.loadImpulseFile(file, second))
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Impulse", "Could not read this audio file.");
            repaint();
        });
}

void NoctuaryEditor::chooseScalaFile()
{
    m_chooser = std::make_unique<juce::FileChooser>("Load a Scala scale", juce::File(), "*.scl");
    m_chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& fc) {
            const auto file = fc.getResult();
            if (!file.existsAsFile()) return;
            if (!m_proc.loadScalaText(file.loadFileAsString(), file.getFileNameWithoutExtension()))
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Scala", "This file is not a valid .scl scale.");
            repaint();
        });
}

// ---------------------------------------------------------------- painting

void NoctuaryEditor::ScopeView::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    g.setColour(ui::CardColour.withAlpha(0.55f));
    g.fillRoundedRectangle(r, 6.0f);
    g.setColour(ui::CardEdge);
    g.drawRoundedRectangle(r.reduced(0.5f), 6.0f, 1.0f);

    float live[ambient::MaxPartials] = {};
    const int n = m_proc.engine().displayPartials(live, ambient::MaxPartials);
    m_count = juce::jmax(m_count, n);
    // Glide towards the engine's values: at 30 Hz the raw numbers would flicker, and the point is
    // to see the slow breathing, not the control-block edges.
    for (int i = 0; i < ambient::MaxPartials; ++i) {
        const float target = i < n ? std::abs(live[i]) : 0.0f;
        m_amp[i] += (target - m_amp[i]) * 0.25f;
    }
    float peak = 1.0e-6f;
    for (int i = 0; i < m_count; ++i) peak = juce::jmax(peak, m_amp[i]);
    const bool silent = peak < 1.0e-4f;

    const auto plot = r.reduced(9.0f, 7.0f);
    if (silent) {
        g.setColour(ui::Faint);
        g.setFont(ui::body(11.0f));
        g.drawText("oscillator", plot, juce::Justification::centred, false);
        g.setColour(ui::Track);
        g.drawLine(plot.getX(), plot.getCentreY(), plot.getRight(), plot.getCentreY(), 1.0f);
        return;
    }

    // Spectrum behind: one thin bar per partial, on a decibel scale.
    {
        const float bw = juce::jmin(9.0f, plot.getWidth() / static_cast<float>(juce::jmax(m_count, 1)));
        for (int i = 0; i < m_count; ++i) {
            const float a = m_amp[i] / peak;
            if (a < 1.0e-4f) continue;
            const float db = juce::jlimit(0.0f, 1.0f, 1.0f + std::log10(a) / 2.5f);   // -50 dB .. 0
            const float h = db * plot.getHeight() * 0.9f;
            g.setColour(ui::Accent.withAlpha(0.10f + 0.13f * db));
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
        for (int i = 0; i < m_count; ++i) {
            if (m_amp[i] < 1.0e-5f) continue;
            const float ph = 0.381966f * static_cast<float>(i);
            v += m_amp[i] * std::sin(juce::MathConstants<float>::twoPi * ((i + 1) * t + ph));
        }
        const float y = mid - juce::jlimit(-1.0f, 1.0f, v / (peak * 2.2f)) * half;
        const float px = plot.getX() + t * plot.getWidth();
        if (x == 0) wave.startNewSubPath(px, y); else wave.lineTo(px, y);
    }
    g.setColour(ui::Accent.withAlpha(0.20f));
    g.strokePath(wave, juce::PathStrokeType(4.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(ui::Accent);
    g.strokePath(wave, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const float hz = m_proc.engine().displayFrequency();
    if (hz > 0.0f) {
        g.setColour(ui::Dim);
        g.setFont(ui::body(10.0f));
        g.drawText(juce::String(hz, 1) + " Hz   " + juce::String(m_count) + " partials",
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
    // The standalone's window back where it was left (15.10.2026), once the window has put its content in --
    // not while pictures are taken (they want the design size), as in the generators; a muted start keeps it.
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<NoctuaryEditor>(this)] {
        const bool pictures = juce::SystemStats::getEnvironmentVariable("AMBIENT_SHOT", "").isNotEmpty()
                           || juce::SystemStats::getEnvironmentVariable("AMBIENT_MANUAL", "").isNotEmpty();
        if (safe != nullptr && !pictures) safe->m_window.attach(*safe, "Noctuary");
    });
}

void NoctuaryEditor::paint(juce::Graphics& g)
{
    // Everything below is drawn in the design space; one transform scales the whole editor.
    g.fillAll(ui::Bg0);
    g.addTransform(juce::AffineTransform::scale(m_scale));
    {   // a little vertical lift, so the window is not a flat grey field
        juce::ColourGradient sky(ui::Bg1, 0.0f, 0.0f, ui::Bg0, 0.0f, static_cast<float>(HeaderH * 3), false);
        g.setGradientFill(sky);
        g.fillRect(0, 0, m_designW, HeaderH * 3);
    }

    // Header
    g.setColour(GroupFill);
    g.fillRect(m_header);
    g.setColour(Text);
    g.setFont(juce::FontOptions(22.0f, juce::Font::bold));
    g.drawText("Noctuary", 14, 6, 180, 26, juce::Justification::centredLeft);
    {   // The help line: the control under the mouse, its value and what it does -- otherwise
        // the three things worth knowing first. This is where the routing map used to be; the
        // signal flow is in the manual now, where it can be read rather than deciphered.
        g.setColour(ui::CardColour.withAlpha(0.6f));
        g.fillRoundedRectangle(m_helpLine.toFloat(), 5.0f);
        juce::String title, body;
        if (m_hoveredParam >= 0 && m_hoveredParam < NumParams) {
            const ParamIdE id = static_cast<ParamIdE>(m_hoveredParam);
            const ParamDesc& d = paramDesc(id);
            juce::String value;
            if (auto* v = m_proc.m_apvts.getRawParameterValue(d.m_key)) {
                const float raw = v->load();
                if (d.m_kind == ParamKindE::CHOICE) value = d.m_choices[juce::jlimit(0, d.m_numChoices - 1, static_cast<int>(std::lround(raw)))];
                else if (d.m_kind == ParamKindE::BOOL) value = raw >= 0.5f ? "on" : "off";
                else if (auto* pp = m_proc.m_apvts.getParameter(d.m_key)) value = pp->getText(pp->convertTo0to1(raw), 24) + (d.m_unit[0] ? juce::String(" ") + d.m_unit : juce::String());
            }
            title = juce::String(d.m_section).toUpperCase() + "   " + d.m_name + "      " + value;
            body = ambient::paramHelp(id);
            const int mods = [&] { int n = 0; const auto& m = m_proc.engine().modMatrix(); for (int k = 0; k < m.count(); ++k) if (m.route(k).m_target == id) ++n; return n; }();
            if (mods > 0) body += "   [" + juce::String(mods) + (mods == 1 ? " modulation route" : " modulation routes") + " -- right-click to see them]";
        } else {
            title = "Noctuary";
            body = "Point at any control to read what it does. Help (or F1) opens the manual. Right-click a knob for MIDI learn and its modulation routes; drag a card from the strip at the bottom onto a knob to modulate it.";
        }
        g.setColour(Text);
        g.setFont(ui::title(10.5f));
        g.drawText(title, m_helpLine.reduced(10, 5), juce::Justification::topLeft, false);
        g.setColour(Dim);
        g.setFont(ui::body(11.5f));
        g.drawFittedText(body, m_helpLine.reduced(10, 5).withTrimmedTop(17), juce::Justification::topLeft, 3, 1.0f);
    }

    // Keyboard strip with near (bright) / far (dim) notes and the brain's root.
    const int root = m_proc.engine().brainRoot();
    const int first = 24, last = 108;
    const float keyW = static_cast<float>(m_keys.getWidth()) / static_cast<float>(last - first + 1);
    for (int n = first; n <= last; ++n) {
        const float kx = m_keys.getX() + (n - first) * keyW;
        const bool black = juce::MidiMessage::isMidiNoteBlack(n);
        juce::Colour col = black ? juce::Colour(0xff2a2e36) : juce::Colour(0xff3a3f4a);
        if (m_sounding[n]) {
            const float d = juce::jlimit(0.0f, 1.0f, m_proc.engine().noteDistance(n));
            col = Accent.interpolatedWith(juce::Colour(0xff35506a), d);
        }
        if (n == root) col = col.interpolatedWith(juce::Colours::orange, 0.6f);
        g.setColour(col);
        g.fillRect(kx + 0.5f, static_cast<float>(m_keys.getY()), keyW - 1.0f, static_cast<float>(m_keys.getHeight()));
    }
    const int voices = m_proc.engine().activeVoices();
    juce::String keyText;
    {   // what the conductor has found itself in, when it has found anything
        const ambient::KeyEstimate key = m_proc.engine().brainKey();
        if (key.m_key >= 0 && key.m_confidence > 0.0f)
            keyText = "   key " + juce::MidiMessage::getMidiNoteName(key.tonic(), true, false, 0) + (key.m_modal ? juce::String(" (scale)") : juce::String(key.minor() ? " minor" : " major")) + " r " + juce::String(key.m_confidence, 2);
    }
    juce::String info = juce::String(voices) + " voice" + (voices == 1 ? "" : "s")
        + "   root " + juce::MidiMessage::getMidiNoteName(root, true, true, 4)
        + "   " + juce::String(m_proc.engine().scale().m_name)
        + "   arc " + juce::String(m_proc.engine().arcValue(), 2) + keyText
        + "   " + juce::String(m_proc.engine().tempo(), 1) + " bpm  bar " + juce::String(1 + static_cast<int>(std::floor(m_proc.engine().beatPosition() / 4.0)))
        + (m_proc.engine().clockRunning() ? "" : " (stopped)");
    if (m_proc.userScaleName().isNotEmpty()) info += "   (user: " + m_proc.userScaleName() + ")";
    info += m_proc.oscRunning() ? "   OSC :" + juce::String(m_proc.oscPort()) + " (" + juce::String(static_cast<juce::int64>(m_proc.oscMessages())) + " msg)"
                               : "   OSC off: " + m_proc.oscError();
    const auto& gl = m_proc.gestures();
    if (gl.calibrating()) {
        info += "   CALIBRATING " + juce::String(static_cast<int>(gl.calibrationProgress() * 100.0f)) + " %";
    } else if (headsetShown()) {   // the hands only while a headset sends, or when the settings say always (01.10.2026)
        const float clutch = gl.input(GestureInputE::RIGHT_PINCH);
        info += clutch > 0.5f ? "   hands: engaged" : "   hands: free";
        info += "   L " + juce::String(gl.input(GestureInputE::LEFT_HEIGHT), 2) + "  R " + juce::String(gl.input(GestureInputE::RIGHT_HEIGHT), 2)
              + "  dist " + juce::String(gl.input(GestureInputE::HAND_DISTANCE), 2) + "  pinch " + juce::String(clutch, 2);
    }
    g.setColour(Dim);
    g.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain)));   // figures that do not jump
    g.drawText(info, 14, m_header.getBottom() - 16, m_designW - 420, 14, juce::Justification::centredLeft);
    {   // The cascade's aura: a soft glow after the line that brightens with each event the
        // conductor's clock breeds and fades as the excitation does -- the Hawkes process, seen.
        const float ex = m_proc.engine().brainExcitation();
        if (ex > 0.02f) {
            const float a = juce::jlimit(0.0f, 1.0f, ex / 1.6f);
            const int x = 14 + juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), info) + 12;
            const float cy = static_cast<float>(m_header.getBottom() - 9);
            g.setColour(ui::Live.withAlpha(0.25f * a)); g.fillEllipse(x - 8.0f, cy - 8.0f, 16.0f, 16.0f);
            g.setColour(ui::Live.withAlpha(0.9f * a)); g.fillEllipse(x - 3.0f, cy - 3.0f, 6.0f, 6.0f);
            g.setColour(Dim.withAlpha(0.5f + 0.5f * a)); g.drawText("cascade", x + 12, m_header.getBottom() - 16, 60, 14, juce::Justification::centredLeft);
        }
    }

    if (m_proc.isRecording()) {
        g.setColour(juce::Colour(0xffe05050));
        g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        g.drawText("REC " + juce::String(static_cast<int>(m_proc.recordedSeconds() / 60)) + ":" + juce::String(static_cast<int>(m_proc.recordedSeconds()) % 60).paddedLeft('0', 2),
                   760, 8, 90, 24, juce::Justification::centredLeft);
    }
    // The Master section is the only section painted here (it sits in the header).
    if (const Section* ms = const_cast<NoctuaryEditor*>(this)->findSection("Master")) {
        g.setColour(SectionFill);
        g.fillRoundedRectangle(ms->m_bounds.toFloat(), 5.0f);
        g.setColour(MasterHue.withAlpha(0.85f));
        g.setFont(juce::FontOptions(11.5f, juce::Font::bold));
        g.drawText("MASTER", ms->m_bounds.getX() + Pad, ms->m_bounds.getY() + 1, ms->m_bounds.getWidth() - 2 * Pad, TitleH, juce::Justification::centredLeft);
    }
}

void NoctuaryEditor::paintContent(juce::Graphics& g)
{
    g.fillAll(Bg);
    for (const auto& grp : (m_expanded ? m_expandedGroups : m_groups)) {
        g.setColour(GroupFill);
        g.fillRoundedRectangle(grp.m_bounds.toFloat(), 8.0f);
        g.setColour(grp.m_colour);
        g.fillRoundedRectangle(juce::Rectangle<float>(static_cast<float>(grp.m_bounds.getX()), static_cast<float>(grp.m_bounds.getY()), 4.0f, static_cast<float>(grp.m_bounds.getHeight())), 2.0f);
        g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.drawText(grp.m_name, grp.m_bounds.getX() + 12, grp.m_bounds.getY() + 2, grp.m_bounds.getWidth() - 20, GroupTitleH, juce::Justification::centredLeft);
    }
    for (const auto& t : m_tabRows) {
        if (m_expanded) break;   // no tabs on the Expanded page
        const juce::Colour col = m_groups[static_cast<size_t>(t.m_group)].m_colour;
        for (size_t i = 0; i < t.m_tabs.size(); ++i) {
            const bool on = static_cast<int>(i) == t.m_active;
            const auto r = t.m_tabs[i].toFloat();
            g.setColour(on ? col.withAlpha(0.26f) : SectionFill.brighter(0.04f));
            g.fillRoundedRectangle(r, 5.0f);
            if (on) { g.setColour(col.withAlpha(0.9f)); g.drawRoundedRectangle(r.reduced(0.5f), 5.0f, 1.0f); }
            g.setColour(on ? Text : Dim);
            g.setFont(ui::title(10.5f));
            g.drawText(t.m_names[i], t.m_tabs[i], juce::Justification::centred);
        }
    }
    for (const auto& s : m_sections) {
        if (s.m_name == "Master" || !s.m_visible) continue;   // painted in the header / behind another tab
        const juce::Colour col = s.m_group >= 0 ? m_groups[static_cast<size_t>(s.m_group)].m_colour : MasterHue;
        g.setColour(SectionFill);
        g.fillRoundedRectangle(s.m_bounds.toFloat(), 5.0f);
        g.setColour(col.withAlpha(0.85f));
        g.setFont(juce::FontOptions(11.5f, juce::Font::bold));
        g.drawText(s.m_name.toUpperCase(), s.m_bounds.getX() + Pad, s.m_bounds.getY() + 1, s.m_bounds.getWidth() - 2 * Pad, TitleH, juce::Justification::centredLeft);
        {   // The die: five pips in a small square at the right of the title. Click to redraw this
            // section, shift-click to nudge it.
            const juce::Rectangle<int> die(s.m_bounds.getRight() - Pad - 13, s.m_bounds.getY() + 4, 13, 13);
            m_diceOf[s.m_name] = die;
            g.setColour(ui::CardColour.brighter(0.10f));
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
            g.setColour(Dim.withAlpha(0.8f));
            g.setFont(juce::FontOptions(10.5f));
            g.drawText("off -- switch on to open", s.m_bounds.getX() + 90, s.m_bounds.getY() + 1, s.m_bounds.getWidth() - 120, TitleH, juce::Justification::centredLeft);
        }
        if (s.m_name == "Feedback" || s.m_name == "Source 1") {
            const float bus = m_proc.m_apvts.getRawParameterValue("fb_bus")->load();
            const float fm = m_proc.m_apvts.getRawParameterValue("fb_fm")->load();
            juce::String note;
            if (s.m_name == "Feedback" && (bus > 0.0f || fm > 0.0f))
                note = juce::String(juce::CharPointer_UTF8("\xe2\x86\xba")) + "  returns to the sources" + (fm > 0.0f ? " (pitch)" : "");
            if (s.m_name == "Source 1" && fm > 0.0f)
                note = juce::String(juce::CharPointer_UTF8("\xe2\x86\xba")) + "  fed back from the output";
            if (note.isNotEmpty()) {
                g.setColour(ui::Live.withAlpha(0.85f));
                g.setFont(juce::FontOptions(10.5f));
                g.drawText(note, s.m_bounds.getX() + 90, s.m_bounds.getY() + 1, s.m_bounds.getWidth() - 120, TitleH, juce::Justification::centredLeft);
            }
        }
        if (s.m_name == "Morph") {
            const float pos = m_proc.engine().morphPosition();
            const bool active = m_proc.m_apvts.getRawParameterValue("morph_active")->load() >= 0.5f;
            juce::String txt = "A: " + m_proc.morphSlotName(0) + "   B: " + m_proc.morphSlotName(1)
                             + (active ? "   playing " + juce::String(static_cast<int>(std::lround((1.0f - pos) * 100))) + " % A / "
                                         + juce::String(static_cast<int>(std::lround(pos * 100))) + " % B" : "   (off)");
            g.setColour(Dim);
            g.setFont(juce::FontOptions(10.5f));
            g.drawText(txt, s.m_bounds.getX() + 70, s.m_bounds.getY() + 1, s.m_bounds.getWidth() - 80, TitleH, juce::Justification::centredLeft);
            // position bar
            juce::Rectangle<float> bar(static_cast<float>(s.m_bounds.getX() + Pad), static_cast<float>(s.m_bounds.getBottom() - 6), static_cast<float>(s.m_bounds.getWidth() - 2 * Pad), 3.0f);
            g.setColour(juce::Colour(0xff2c3038)); g.fillRect(bar);
            g.setColour(MorphHue); g.fillRect(bar.withWidth(bar.getWidth() * pos));
        }
    }
}

// ---------------------------------------------------------------- ways around the panel (15.10.2026)

bool NoctuaryEditor::automated()
{
    for (const char* v : { "AMBIENT_SHOT", "AMBIENT_MANUAL", "AMBIENT_MUTE" })
        if (juce::SystemStats::getEnvironmentVariable(v, "").isNotEmpty()) return true;
    return false;
}

void NoctuaryEditor::openPalette()
{
    if (m_palette == nullptr) return;
    m_palette->open(paletteCommands());
}

bool NoctuaryEditor::revealParam(int id)
{
    const int cell = id >= 0 && id < NumParams ? cellForParam(static_cast<ParamIdE>(id)) : -1;
    if (cell < 0) return false;
    setPage(0);
    // The tab holding its section, open.
    for (auto& t : m_tabRows)
        for (size_t p = 0; p < t.m_pages.size(); ++p)
            for (const juce::String& name : t.m_pages[p])
                if (Section* s = findSection(name); s != nullptr && std::find(s->m_cells.begin(), s->m_cells.end(), cell) != s->m_cells.end()
                    && static_cast<int>(p) != t.m_active) {
                    t.m_active = static_cast<int>(p);
                    layoutBody();
                    m_content.repaint();
                }
    juce::Component* comp = m_cells[static_cast<size_t>(cell)].m_comp.get();
    if (comp == nullptr) return false;
    // Scrolled to it, and lit once the page has laid itself out.
    if (comp->getParentComponent() == &m_content) {
        const auto r = comp->getBounds();
        if (!m_viewport.getViewArea().contains(r)) m_viewport.setViewPosition(std::max(0, r.getX() - 40), std::max(0, r.getY() - 60));
    }
    m_hoveredParam = id;
    repaint();
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<juce::Component>(comp)] {
        if (safe != nullptr && safe->isShowing()) frame::flash(*safe, ui::Accent);
    });
    return true;
}

std::vector<frame::CommandPalette::Command> NoctuaryEditor::paletteCommands()
{
    using Command = frame::CommandPalette::Command;
    std::vector<Command> out;
    auto action = [&out](const juce::String& title, const juce::String& detail, std::function<void()> f) {
        out.push_back({ title, detail, "Action", false, [f](const juce::String&) { f(); } });
    };
    action("Main page", "the panel", [this] { setPage(0); });
    action("Perform page", "the macros, the morph, the set recorder", [this] { setPage(1); });
    action("Browse page", "every preset: columns, the map, routes", [this] { setPage(2); });
    action("Help", "F1: the manual by topic", [this] { setPage(3); });
    action("Undo", "Ctrl+Z", [this] { doUndo(); });
    action("Redo", "Ctrl+Y", [this] { doRedo(); });
    action("A | B", "park the sound, then swap the two", [this] { swapAB(); });
    if (m_saveButton) action("Save the sound as a preset file", "the disk", [this] { m_saveButton->triggerClick(); });
    if (m_loadButton) action("Load a preset file", "the folder", [this] { m_loadButton->triggerClick(); });
    if (m_recButton) action("Record the output", "a WAV", [this] { m_recButton->triggerClick(); });
    action("Night mode", "the window dim and warm", [this] { setNight(!m_night); });
    action("Journeys over time", "the timeline", [this] { if (m_journeyPanel != nullptr) m_journeyPanel->open(); });
    action("Stop the journey", "", [this] { m_proc.stopJourney(); });
    action("First steps", "the tour of the panel", [this] { startTour(); });
    action("Settings", "the layout, the headset, the jam, Link", [this] { showSettings(); });
    static const char* const Modes[3] = { "Layout: Normal", "Layout: Compact", "Layout: Expanded" };
    for (int i = 0; i < 3; ++i) action(Modes[i], "settings", [this, i] { applyLayoutMode(i); });
    // The tabs of the paging rows.
    for (size_t ti = 0; ti < m_tabRows.size(); ++ti) {
        const TabRow& t = m_tabRows[ti];
        const juce::String GroupColour = t.m_group >= 0 && t.m_group < static_cast<int>(m_groups.size()) ? m_groups[static_cast<size_t>(t.m_group)].m_name : juce::String();
        for (size_t p = 0; p < t.m_names.size(); ++p)
            out.push_back({ GroupColour + ": " + juce::String(t.m_names[p]), "tab", "Page", false, [this, ti, p](const juce::String&) {
                               setPage(0);
                               if (ti < m_tabRows.size()) {
                                   m_tabRows[ti].m_active = static_cast<int>(p);
                                   layoutBody();
                                   m_content.repaint();
                               }
                           } });
    }
    // Every knob: its tab opens and it lights up; a number after its name sets it.
    for (const Cell& c : m_cells) {
        if (c.m_param < 0) continue;
        const ambient::ParamDesc& d = paramDesc(static_cast<ParamIdE>(c.m_param));
        const bool number = d.m_kind == ambient::ParamKindE::FLOAT || d.m_kind == ambient::ParamKindE::INT;
        const int id = c.m_param;
        out.push_back({ d.m_name, d.m_section, "Control", number, [this, id](const juce::String& n) {
                           if (n.isNotEmpty())
                               if (auto* p = m_proc.m_apvts.getParameter(paramDesc(static_cast<ParamIdE>(id)).m_key)) {
                                   pushUndo(paramDesc(static_cast<ParamIdE>(id)).m_name);
                                   p->setValueNotifyingHost(p->convertTo0to1(n.getFloatValue()));
                               }
                           revealParam(id);
                       } });
    }
    // Every preset of the bank (the packs' too): the Sound box takes it, as a choice there does.
    for (int i = 0; i < numPresets(); ++i)
        out.push_back({ preset(i).m_name, "sound preset", "Preset", false, [this, i](const juce::String&) {
                           if (m_soundBox != nullptr) m_soundBox->setSelectedId(i + 1, juce::sendNotificationSync);
                       } });
    return out;
}

void NoctuaryEditor::startTour()
{
    if (m_tour == nullptr) return;
    setPage(0);
    std::vector<frame::Tour::Step> steps;
    if (m_soundBox) steps.push_back({ m_soundBox.get(), "The sound", "The sound presets, a submenu per family; the disk beside saves the sound as a file, the folder loads one. "
                                                               "A star beside it says the knobs have moved from the preset." });
    if (m_browseButton) steps.push_back({ m_browseButton.get(), "Browse", "Every preset by family, character and motion -- or on the measured map, with routes that walk it." });
    if (m_performButton) steps.push_back({ m_performButton.get(), "Perform", "The eight macros as large knobs, the morph between two sounds, the set recorder." });
    if (m_mod) steps.push_back({ m_mod.get(), "Modulation", "Every source as a card: drag one onto a knob and it moves it; a right-click on a knob lists what moves it, "
                                                         "with each depth to drag." });
    steps.push_back({ m_searchIcon.get(), "Search", "Ctrl+K: any knob, preset, page or action by a few letters -- its tab opens and it lights up. A number after a "
                                                   "knob's name sets it." });
    steps.push_back({ m_holdIcon.get(), "The preset held", "Hold it to hear the preset as it was loaded; let go for your sound." });
    steps.push_back({ m_journeyButton.get(), "Journeys", "Presets in a row for a whole night: the one under way over time, a click on a step goes there; "
                                                        "the one being written, to change by hand." });
    steps.push_back({ m_nightIcon.get(), "Night", "The window dim and warm, for a dark room." });
    if (m_settingsIcon) steps.push_back({ m_settingsIcon.get(), "Settings", "The layout, the headset, the family jam, Ableton Link -- and these first steps again." });
    for (auto& s : steps) if (s.m_target == nullptr) s.m_title = {};
    steps.erase(std::remove_if(steps.begin(), steps.end(), [](const frame::Tour::Step& s) { return s.m_title.isEmpty(); }), steps.end());
    m_tour->start(std::move(steps));
}

void NoctuaryEditor::setNight(bool on)
{
    m_night = on;
    frame::Settings::of("Noctuary").setValue("night", on ? "1" : "0");
    m_veil.m_alpha = on ? 0.45f : 0.0f;
    m_veil.setVisible(on);
    m_veil.repaint();
    if (m_nightIcon) m_nightIcon->setToggleState(on, juce::dontSendNotification);
}

void NoctuaryEditor::holdPreset(bool on)
{
    if (on == m_holding) return;
    const int idx = m_proc.soundPresetIndex();
    if (on) {
        if (idx < 0 || idx >= numPresets()) return;
        m_held = takeSnapshot("held");
        m_holding = true;
        m_proc.applySoundPreset(idx);   // the preset as it was loaded, heard while held
    } else {
        m_holding = false;
        restore(m_held);
    }
}

bool NoctuaryEditor::chosenJourney(ambient::Journey& out) const
{
    if (m_journeyBox == nullptr) return false;
    const int id = m_journeyBox->getSelectedId();
    if (id <= 0 || id > m_journeyFiles.size()) return false;
    const juce::File f = m_journeyFiles[id - 1];
    if (!f.existsAsFile()) return false;
    out = ambient::Journey();
    out.parse(f.loadFileAsString().toRawUTF8());
    if (out.m_name.empty()) out.m_name = f.getFileNameWithoutExtension().toStdString();
    return !out.m_steps.empty();
}

void NoctuaryEditor::startJourneyAt(int step)
{
    if (m_journeyBox == nullptr) return;
    const int id = m_journeyBox->getSelectedId();
    if (id <= 0 || id > m_journeyFiles.size()) return;
    if (m_proc.startJourney(m_journeyFiles[id - 1])) m_proc.skipJourneyTo(step);
}

void NoctuaryEditor::setRouteDepth(int row, float depth)
{
    char buf[4096];
    const int len = m_proc.engine().writeModMatrix(buf, sizeof(buf));
    juce::StringArray rows = juce::StringArray::fromTokens(juce::String(juce::CharPointer_UTF8(buf), static_cast<size_t>(juce::jmax(0, len))), ";", "");
    if (row < 0 || row >= rows.size()) return;
    juce::StringArray fields = juce::StringArray::fromTokens(rows[row], ":", "");   // \<source\>>\<target\>:\<depth\>[:\<via\>][:u]
    if (fields.size() < 2) return;
    fields.set(1, juce::String(juce::jlimit(-1.0f, 1.0f, depth), 3));
    rows.set(row, fields.joinIntoString(":"));
    m_proc.engine().setModMatrixText(rows.joinIntoString(";").toRawUTF8());
    if (m_mod != nullptr) m_mod->repaint();
}

void NoctuaryEditor::removeRoute(int row)
{
    char buf[4096];
    const int len = m_proc.engine().writeModMatrix(buf, sizeof(buf));
    juce::StringArray rows = juce::StringArray::fromTokens(juce::String(juce::CharPointer_UTF8(buf), static_cast<size_t>(juce::jmax(0, len))), ";", "");
    if (row < 0 || row >= rows.size()) return;
    rows.remove(row);
    m_proc.engine().setModMatrixText(rows.joinIntoString(";").toRawUTF8());
    repaint();
}
