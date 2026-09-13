#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "NoctuaryLookAndFeel.h"
#include "ambient/Modulation.h"
#include "ambient/ZPlane.h"
#include "ambient/Sources.h"
#include "ambient/PresetMeta.h"
#include <vector>
#include <memory>
#include <functional>
#include <map>
#include <set>

namespace edt { class RouteTable; }   // EditorCommon.h / EditorMatrix.cpp

// The editor groups the sections the way the signal flows:
//   VOICE (Oscillator, Air, Envelope, Filter, Space) -> FOREGROUND (Ensemble, Delay, Delay 2,
//   Near Reverb) -> out; FOREGROUND -> COSMOS (parallel, returns) ; FOREGROUND -> BACKGROUND
//   (Cloud, Far Reverb) -> out ; CONDUCTOR (Cluster Brain, Tuning) drives the voices;
//   MORPH blends two full presets. A routing map in the header draws exactly this.
class NoctuaryEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit NoctuaryEditor(NoctuaryProcessor&);
    ~NoctuaryEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    // The standalone window is JUCE's own, and it is built with a minimise and a close button and
    // nothing else. The editor is the first thing that can reach it, and only once it has been put
    // inside it -- hence here rather than in the constructor.
    void parentHierarchyChanged() override;

private:
    void timerCallback() override;
    void chooseScalaFile();
    void chooseSourceFile(bool wavetable, int slot = -1);   // slot < 0: the clip goes into every slot
    void chooseImpulseFile(bool second = false);
    void chooseNearClipFile();   // the near source's own recording (the near layer's Clip type)
    void updateSourceCells();   // greys the cells a slot's type does not use, names the loaded files
    bool updateNearCells();     // the same for the Near Source's type; true if a cell changed
    int  cellForParam(ambient::ParamId id) const;
    int  tableCell_ = -1, impulseCell_ = -1, impulseBCell_ = -1, nearClipCell_ = -1;
    bool autoSeen_ = true;   // the near layer's Auto as the timer last saw it (a rising edge draws anew)
    // Journeys: the box of files, the status label, and the files behind the box's ids.
    juce::ComboBox* journeyBox_ = nullptr;
    juce::Label*    journeyStatus_ = nullptr;
    juce::Array<juce::File> journeyFiles_;
    void fillJourneyBox();
    void saveJourneyAs();
    int  textureCell_[ambient::kSlots] = { -1, -1, -1, -1 };   // a Texture... button in every source section
    void buildCells();
    void colourCellsByGroup();
    int  addExtraCell(const juce::String& section, std::unique_ptr<juce::Component> comp, const juce::String& label, int units);
    // Help: the line in the header follows the mouse; the page is the manual by topic.
    void mouseEnter(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;
    int  hoveredParam_ = -1;                              // what the mouse is over, for the help line
    std::map<juce::Component*, int> helpParamOf_;         // every control that has a parameter behind it
public:
    void registerHelp(juce::Component*, ambient::ParamId);   // the modulation strip registers its own
private:
    // Undo, redo and an A/B compare, all in terms of whole parameter snapshots: the instrument
    // has no other state that a knob can destroy, and a snapshot is 320 floats.
    struct Snapshot { std::vector<float> v; juce::String what; };
    std::vector<Snapshot> undo_, redo_;
    Snapshot slotA_, slotB_;
    bool     showingB_ = false;
    Snapshot takeSnapshot(const juce::String& what) const;
    void     restore(const Snapshot&);
    void     pushUndo(const juce::String& what);          // call before changing many parameters at once
    void     doUndo();
    void     doRedo();
    void     swapAB();
    std::unique_ptr<juce::TextButton> undoButton_, redoButton_, abButton_, compactButton_, recallButton_;
    // Undo covered presets, dice and envelopes but not the thing done most: a knob turned. One
    // listener on every parameter control takes a snapshot on the press or the wheel that starts a
    // change, named after the parameter, and throttles the wheel so a scroll is one step back, not
    // forty.
    struct UndoHook : juce::MouseListener {
        NoctuaryEditor* editor = nullptr;
        std::map<juce::Component*, juce::String> names;
        std::map<juce::Component*, double> lastWheel;
        void mouseDown(const juce::MouseEvent& e) override
        {
            auto it = names.find(e.eventComponent);
            if (it != names.end() && editor != nullptr) editor->pushUndo(it->second);
        }
        void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails&) override
        {
            auto it = names.find(e.eventComponent);
            if (it == names.end() || editor == nullptr) return;
            const double now = juce::Time::getMillisecondCounterHiRes();
            double& last = lastWheel[e.eventComponent];
            if (now - last > 1000.0) editor->pushUndo(it->second);
            last = now;
        }
    };
    UndoHook undoHook_;
    // Dragging a modulation card onto a knob. The gesture crosses half the panel, and the strip
    // could only draw inside itself, so as soon as the hand left the lane there was nothing to
    // see: no line, no card, no sign of which modulator was in flight. This is drawn on top of
    // everything instead -- the line from the card, the card under the cursor, and a ring around
    // the knob it would land on, named.
    struct DragOverlay : juce::Component {
        DragOverlay() { setInterceptsMouseClicks(false, false); setAlwaysOnTop(true); }
        void paint(juce::Graphics&) override;
        bool active = false;
        juce::Point<float> from, to;
        juce::String name, targetName;
        juce::Colour colour { 0xffffffff };
        juce::Rectangle<float> target;
    };
    DragOverlay dragOverlay_;
    // Pigments' little knob: after the drop, the depth of the route that was just made, in the
    // modulator's own colour and named after both ends of it. The next click anywhere else puts
    // it away.
    struct DepthPopup : juce::Component {   // a Component is already a MouseListener: inheriting it twice is ambiguous
        DepthPopup(NoctuaryEditor& o, ambient::ModSource s, ambient::ParamId t, juce::Colour c);
        ~DepthPopup() override;
        void paint(juce::Graphics&) override;
        void resized() override;
        // One callback serves both roles: the component's own presses and, because this is
        // registered as a global listener while it lives, everybody else's. A press inside keeps
        // it; a press anywhere else puts it away -- posted, since a component may not be deleted
        // from inside its own mouse callback.
        void mouseDown(const juce::MouseEvent& e) override
        {
            if (e.eventComponent == this || isParentOf(e.eventComponent)) return;
            juce::MessageManager::callAsync([w = juce::Component::SafePointer<DepthPopup>(this)] {
                if (w != nullptr) w->owner.hideDepthPopup();
            });
        }
        NoctuaryEditor& owner;
        ambient::ModSource source;
        ambient::ParamId target;
        juce::Colour colour;
        juce::Slider depth;
        juce::TextButton remove{ "x" };
    };
    std::unique_ptr<DepthPopup> depthPopup_;
public:
    void showModDrag(juce::Point<int> screenFrom, juce::Point<int> screenTo, const juce::String& name, juce::Colour);
    void hideModDrag();
    void showDepthPopup(ambient::ModSource, ambient::ParamId, juce::Colour, juce::Point<int> screenAt);
    void hideDepthPopup();
    // Rewrites one route's depth (the matrix travels as text, as everywhere else here).
    bool setRouteDepth(ambient::ModSource, ambient::ParamId, float depth);
    bool removeRoute(ambient::ModSource, ambient::ParamId);
    float routeDepth(ambient::ModSource, ambient::ParamId) const;
private:
    // The two section layers that live inside their own sections rather than in the header:
    // 156 filters and 41 plucks are lists you go looking for, not things you keep on the toolbar.
    // Not owned: the cell owns the component, the editor only needs to read the selection back.
    juce::ComboBox* zPresetBox_ = nullptr;
    juce::ComboBox* strikePresetBox_ = nullptr;
    juce::ComboBox* nearPresetBox_ = nullptr;
    // Compact: the same page in columns kCompactFactor as wide, every page refitted into them --
    // narrower and taller, nothing left out. Kept in the plugin state.
    bool compact_ = false;
    static constexpr float kCompactFactor = 0.88f;
    // Expanded: every page of every tab row placed under one another. No tabs to click, a
    // tall page, and the four sources, the two filters and the conductor's six tables all in
    // sight at once -- for a tall screen, or for reading a preset through.
    bool expanded_ = false;
    // The manual's pictures: every section open whatever its switch says, and the tabs.
    bool openAll_ = false;
    bool refreshLayoutState();   // updates unused/collapsed from the parameters; true if anything changed
    // The die in a section's title: one click randomises that section, shift-click nudges it.
    std::map<juce::String, juce::Rectangle<int>> diceOf_;
    void     randomiseSection(const juce::String& name, bool subtle);
    void     rebuildLayout();
    void     applyLayoutMode(int mode);   // 0 normal, 1 compact, 2 expanded
    struct HelpView : juce::Component, juce::ListBoxModel {
        HelpView(NoctuaryProcessor&, NoctuaryEditor&);
        void paint(juce::Graphics&) override;
        void resized() override;
        int  getNumRows() override;
        void paintListBoxItem(int row, juce::Graphics&, int w, int h, bool selected) override;
        void selectedRowsChanged(int row) override;
        void showTopic(int row);
        NoctuaryProcessor& proc;
        NoctuaryEditor& owner;
        juce::ListBox topics;
        juce::TextEditor text;
        juce::String parameters;   // the generated last topic: every parameter with its help
        // The pictures: snapshots of the topic's sections, fresh from the panel with its current
        // values, and a live display of the unit -- the same component the page uses, a second copy.
        std::vector<juce::Image> pics;
        // Pictures of whole tabs. The help page does not draw these -- its picture column
        // holds two or three -- but the manual prints every one of them, with its tab's
        // own name as the caption.
        std::vector<juce::Image> tabPics;
        juce::StringArray        tabNames, tabCaptions, picCaptions;
        juce::String             liveCaption;
        std::vector<juce::StringArray> tabSections;   // per tab picture: the sections under it
        std::vector<juce::Rectangle<int>> picRects;
        std::unique_ptr<juce::Component> live;
        // The signal flow, drawn large enough to read: what the routing map in the header was for.
        struct FlowDiagram : juce::Component {
            explicit FlowDiagram(NoctuaryProcessor& p) : proc(p) { setInterceptsMouseClicks(false, false); }
            void paint(juce::Graphics&) override;
            // The part of this component the drawing actually covers. The diagram is a fixed
            // canvas scaled to fit, so one of the two dimensions is always left over; a picture
            // of the whole component would be a diagram with a field of black under it.
            juce::Rectangle<int> drawn() const;
            static constexpr float kCanvasW = 1000.0f, kCanvasH = 545.0f;
            NoctuaryProcessor& proc;
        };
        FlowDiagram flow;
        int topic = 0;
    };
    // Snapshots for the manual: a section as it stands on the panel (its tab is switched in for
    // the picture and back again), the modulation strip, the browser page.
    juce::Image snapshotSection(const juce::String& name);
    juce::Image snapshotTab(int rowIndex, int page);
    juce::Image snapshotPage(juce::Component* page);
    juce::Image snapshotStrip();
    juce::Image snapshotBrowse();
    juce::Image snapshotPerform();
    juce::Image snapshotHeader();
    juce::Image snapshotBrowseMap();
    juce::Image snapshotBrowseMapZoomed();   // the same view closed in on the current preset, names showing
    juce::Image snapshotStripTab(int tab, bool detail = false);
    void openSourceEnvelope(int slot);   // the envelope tab's SOURCES page, from a click on a source's picture
    juce::StringArray tabSectionNames(int rowIndex, int page) const;
    juce::String tabName(int rowIndex, int page) const;
    // Writes the manual out as pictures and text for Tools/make_manual.py to turn into a PDF.
    // It has to happen here, in a running editor, because the pictures ARE the panel: snapshots
    // of the real sections with their real values, not drawings kept somewhere in step with it.
    void exportManual(const juce::File& dir, std::function<void()> onDone);
    struct ManualJob;
    std::unique_ptr<ManualJob> manual_;
    void runManualStep();
    std::unique_ptr<HelpView> help_;
    std::unique_ptr<juce::TextButton> helpButton_;
    std::unique_ptr<juce::TextButton> aboutButton_;
    std::unique_ptr<juce::TooltipWindow> tooltips_;

    struct Cell {
        std::unique_ptr<juce::Component> comp;
        std::unique_ptr<juce::Label> label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> slider;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> button;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> combo;
        juce::String baseLabel;
        int units = 1;
        int param = -1;    // ParamId or -1 for extra cells
        // The slot's type does not use it: not drawn and not laid out, rather than greyed. A
        // strip is as tall as the type it is set to, not as the union of every type.
        bool unused = false;
    };
    struct Section {
        juce::String name;
        std::vector<int> cells;
        juce::Rectangle<int> bounds;
        int maxUnits = 7;
        int wideUnits = 0;          // the natural width on the tabbed page (decides the columns; the fit may change it)
        int flowUnits = 0;          // the width in Expanded's flows, where a section's shape is its own
        int group = -1;
        bool visible = true;   // false while its tab is not the open one
        // Its switch is off (a type of Off, a level at zero, an Active that is not): only the
        // title and the switch's own cells are shown. Off means closed. Opening it is the
        // player's own action on the switch, so the page never rearranges itself.
        bool collapsed = false;
        // ... unless closing it buys nothing. A section is closed to save room, and where the row
        // has the room anyway, it is laid out open instead: a title beside an empty band is worse
        // than the section itself. Decided by the layout, from the row's own width, and cleared at
        // the start of every pass -- `collapsed` stays what the parameters say, so the timer that
        // watches for a switch being thrown cannot see this and rebuild for ever.
        bool opened = false;
        bool closedNow() const { return collapsed && !opened; }
    };
    struct Group {
        juce::String name;
        juce::Colour colour;
        std::vector<std::vector<juce::String>> rows;   // section names per row
        juce::Rectangle<int> bounds;
        int column = 0;
        std::vector<juce::Component*> displays;        // per row: a display that fills the leftover width, or null
        // A row with no sections in it gets its height from here instead of from them; and if the
        // group is marked stretchy, that row also takes whatever height the taller column has
        // left over, so the two columns end level and the page has no hole in it at any size.
        int  minRowH = 0;
        bool stretch = false;
    };

    // A row of a group whose sections take turns: one page open, the others a tab away. This is
    // what keeps the whole synth on one screen -- the three sources, the two filters, the effect
    // pairs and the conductor's tables are alike enough that seeing one at a time is no loss.
    struct TabRow {
        int group = 0, row = 0;
        std::vector<juce::String> names;                       // one label per page
        std::vector<std::vector<juce::String>> pages;          // section names per page
        std::vector<juce::Component*> displays;                // per page: display for the leftover width, or null; two pages may share one
        int active = 0;
        juce::Rectangle<int> bar;
        std::vector<juce::Rectangle<int>> tabs;
    };
    std::vector<TabRow> tabRows_;
    TabRow* tabRowFor(int group, int row);
    void    setSectionVisible(Section&, bool);
    void    clickTabs(juce::Point<int> contentPos);

    Section* findSection(const juce::String& name);
    // Which cells a section shows, and whether it is closed. Both are functions of the parameter
    // state alone, never of the window, so the layout is reproducible and the manual can be.
    bool cellShown(const Section&, const Cell&) const;
    int  shownCells(const Section&) const;   // how many of its cells are on the page as it stands
    bool sectionCollapsed(const Section&) const;
    int      sectionWidth(const Section&) const;
    int      sectionHeight(const Section&) const;
    void     layoutSection(Section&, int x, int y);

    NoctuaryProcessor& proc_;
    NoctuaryLookAndFeel laf_;
    float scale_ = 1.0f;   // window size / design size
    int   designW_ = 1400, designH_ = 820;   // measured from the layout, not guessed
    int   bodyW_ = 0, bodyH_ = 0;
    void  layoutBody();
    // The tabbed page (Normal and Compact). Every page of every row is FITTED to its column: its
    // sections are given the fewest rows at which they stand side by side in the column's width,
    // each as narrow as that allows, so they come out the same height and the page has no hole
    // under a short one; the display takes the width that is left. A row is as tall as the page
    // that is open on it, not as its tallest page, so a one-row page is one row; the design
    // height is still the sum of the tallest pages, so the window never changes shape on a tab
    // click, and the last group of each column grows into the difference.
    void  layoutTabbed();
    std::vector<Cell> cells_;
    std::vector<Section> sections_;
    std::vector<Group> groups_;
    // The Expanded page: its own table of groups and rows, three columns, no tabs. Rows without
    // a display wrap when their sections, open, would run past kWrapW, so a row of closed
    // sections is one line and a row of open ones is two or three, never a column a screen wide.
    std::vector<Group> expandedGroups_;
    static constexpr int kWrapW = 1400;
    std::map<juce::Component*, int> cellOf_;
    std::unique_ptr<juce::Slider> master_;

    // Live picture of the oscillator: one cycle built from the partial amplitudes the loudest
    // voice is summing right now, plus those partials as a spectrum. It moves because the
    // shimmer and the drift move -- it is the sound, not an illustration of it.
    struct ScopeView : juce::Component, juce::Timer {
        explicit ScopeView(NoctuaryProcessor& p) : proc(p) { setInterceptsMouseClicks(false, false); startTimerHz(20); }
        void paint(juce::Graphics&) override;
        void timerCallback() override { if (isShowing()) repaint(); }
        NoctuaryProcessor& proc;
        float amp[ambient::kMaxPartials] = {};   // smoothed towards the engine's values
        int   count = 0;
        bool  mode = false;                      // false = waveform, true = spectrum
    };
    std::unique_ptr<ScopeView> scope_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> masterAttach_;
    // Everything below the header lives in a scrollable content component.
    struct Content : juce::Component {
        std::function<void(juce::Graphics&)> onPaint;
        std::function<void(const juce::MouseEvent&)> onMouse;
        void paint(juce::Graphics& g) override { if (onPaint) onPaint(g); }
        void mouseDown(const juce::MouseEvent& e) override { if (onMouse) onMouse(e); }
    };
    Content content_;
    juce::Viewport viewport_;
    void paintContent(juce::Graphics&);
    std::unique_ptr<juce::TextButton> saveButton_, loadButton_, recButton_, calibButton_, mapButton_, performButton_;
    // Main brings the panel back from any page; VR holds what only a headset needs (calibration,
    // the gesture table) under one button instead of two on the toolbar.
    std::unique_ptr<juce::TextButton> mainButton_, vrButton_;
    // Perform page: the eight macros as large knobs plus the morph, instead of the editor.
    struct PerformView : juce::Component {
        explicit PerformView(NoctuaryProcessor& p);
        void paint(juce::Graphics&) override;
        void resized() override;
        NoctuaryProcessor& proc;
        std::vector<std::unique_ptr<juce::Slider>> knobs;
        std::vector<std::unique_ptr<juce::Label>> labels;
        std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> attachments;
        juce::Slider morph;
        juce::ToggleButton morphActive;
        juce::Label morphLabel, aLabel, bLabel;
        // set timeline: record everything that moves, play a set back
        juce::TextButton setRec{ "Record set" }, setPlay{ "Play set..." }, setStop{ "Stop set" };
        juce::Label setInfo;
        std::unique_ptr<juce::FileChooser> chooser;
        void updateSetInfo();
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> morphAttach;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> morphActiveAttach;
    };
    std::unique_ptr<PerformView> perform_;
    bool performing_ = false;
    void setPerforming(bool on);

    // Browse page: filterable preset list plus the preset map (points = presets, drag the
    // cursor to blend between neighbours).
    struct BrowseView : juce::Component, juce::ListBoxModel, juce::Timer {
        explicit BrowseView(NoctuaryProcessor& p);
        void paint(juce::Graphics&) override;
        void resized() override;
        void timerCallback() override;
        // list
        int  getNumRows() override { return static_cast<int>(filtered.size()); }
        void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override;
        void listBoxItemClicked(int row, const juce::MouseEvent&) override;
        void applyFilter();
        // map
        struct MapView : juce::Component {
            explicit MapView(BrowseView& o) : owner(o) {}
            void paint(juce::Graphics&) override;
            void mouseDown(const juce::MouseEvent&) override;
            void mouseDrag(const juce::MouseEvent&) override;
            void mouseMove(const juce::MouseEvent&) override;
            void mouseUp(const juce::MouseEvent&) override;
            void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
            void mouseDoubleClick(const juce::MouseEvent&) override;
            int  nearestPreset(juce::Point<float> p, float maxDist) const;
            juce::Point<float> toScreen(float x, float y) const;
            juce::Point<float> toMap(juce::Point<float> s) const;        // clamped to the plane
            juce::Point<float> toMapRaw(juce::Point<float> s) const;     // not clamped: for the zoom's pivot
            void zoomTo(float x, float y, float z);                      // centre the view on a point at a zoom
            BrowseView& owner;
            int hover = -1;
            bool dragging = false;
            // Seven thousand dots on a few hundred pixels are heaps, not presets. The view zooms
            // about the mouse and pans by dragging; the plane itself never changes, only the
            // window onto it, so a cursor position means the same thing at every zoom.
            float zoom = 1.0f;
            juce::Point<float> centre { 0.5f, 0.5f };
            bool panning = false;
            juce::Point<float> panFrom, centreFrom;
            // The cloud itself, drawn once into an image. Eight and a half thousand filled dots,
            // fifteen times a second, were nineteen percentage points of a processor core for a
            // picture that only changes when the window, the zoom, the pan, the filter or the
            // colouring changes. What moves -- the cursor, the ring, the crossing, the names --
            // is drawn live on top of it.
            juce::Image cloud;
            float cloudZoom = -1.0f;
            juce::Point<float> cloudCentre { -1.0f, -1.0f };
            int  cloudW = 0, cloudH = 0, cloudFilter = -1, cloudCount = -1;
            bool cloudGroups = false;
        };
        // classic column browser (Omnisphere / Absynth style): each column narrows the list
        struct Column : juce::ListBoxModel {
            BrowseView* owner = nullptr;
            juce::String title;
            juce::StringArray items;          // items[0] = "All"
            std::vector<uint32_t> tagBits;    // per item: tag mask (0 for family columns / All)
            std::vector<int> familyIdx;       // per item: family index or -1
            std::set<int> chosen;             // chosen rows (empty = All)
            std::vector<int> counts;          // presets per row, rebuilt when the list grows
            int countsFor = -1;               // the numPresets() the counts were taken at
            void updateCounts();
            juce::ListBox box;
            int  getNumRows() override { return items.size(); }
            void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override;
            void listBoxItemClicked(int row, const juce::MouseEvent&) override;
            bool passes(int preset) const;
        };
        Column columns[4];
        void setMode(int m);   // 0 classic columns, 1 map
        int mode = 0;
        juce::TextButton modeClassic{ "Columns" }, modeMap{ "Map" }, star{ "Favourite" };
        juce::ToggleButton onlyFavourites{ "only favourites" };
        juce::ToggleButton favouritesFirst{ "favourites first" };   // the starred ones at the top, whatever the sort
        // Five thousand presets include some that barely move. Their measurements say so, and
        // this hides them: nothing is deleted, the list simply stops offering them.
        juce::ToggleButton hideDull{ "hide the still ones" };
        // A preset change as a journey rather than a cut, and how long it takes.
        juce::ToggleButton morphOnSelect{ "morph into it" };
        juce::Slider morphSeconds;
        // What u-he's browsers put beside the list: what this preset is, how it paces itself,
        // what your hands are wired to in it, and where it is filed. Generated from the
        // measurements, the settings and its own matrix (ambient::presetInfoText).
        struct InfoPanel : juce::Component {
            void paint(juce::Graphics&) override;
            juce::String title, body;
        };
        InfoPanel info2;
        int infoFor = -2;              // which preset the panel currently describes
        void updateInfo();
        // The cloud's colour: the measured groups, or where a preset came from. Groups by
        // default -- what a browser is for is finding a sound, and the pack a sound was written
        // in is a fact about its author.
        juce::ToggleButton colourByGroup{ "groups" };
        // The four macro sliders. Each is a range over one measured descriptor, wide open by
        // default; narrowing one thins the cloud to what is left, and the view closes in on it.
        struct Macro { juce::Slider slider; juce::Label label; float ambient::PresetMeta::* field; };
        Macro macros[4];
        bool  macroActive() const;
        void  fitToFilter();      // zoom the map onto what survived the filter
        bool  fitPending = false;

        NoctuaryProcessor& proc;
        juce::TextEditor search;
        juce::ComboBox family, sort;
        std::vector<std::unique_ptr<juce::ToggleButton>> tagButtons;
        juce::ListBox list;
        MapView map;
        juce::ToggleButton mapActive;
        juce::Slider radius;
        juce::Label info;
        // route strip (map view): preset routes, play/loop/speed, add the cursor as a point, edit the text
        juce::ComboBox routeBox;
        juce::ToggleButton routePlay, routeLoop;
        juce::Slider routeSpeed;
        juce::TextButton routeAdd{ "+ point" }, routeClear{ "Clear" }, routeEdit{ "Route..." };
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> routePlayAttach, routeLoopAttach;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> routeSpeedAttach;
        juce::Component::SafePointer<juce::TextEditor> routeEditor;
        juce::String routeEditText; bool routeEditOpen = false;
        void showRouteEditor();
        juce::TextButton toA{ "-> A" }, toB{ "-> B" }, load{ "Load" };
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> mapActiveAttach;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> radiusAttach;
        std::vector<int> filtered;
        int filterGen = 0;      // bumped by applyFilter, so the map knows its cached cloud is stale
        int selected = -1;
        // "More like this": the list narrowed to the map's nearest neighbours of the selected
        // preset, and the map zoomed onto them. The similarity is the one the map already has.
        juce::ToggleButton similar { "similar" };
        std::vector<int> similarSet;
        int similarOf = -1;
    };
    std::unique_ptr<BrowseView> browse_;
    // The modulation strip along the bottom of the main page, in the shape Pigments uses: a lane
    // of every modulator as a small card with its live curve, a row of tabs, and the full editors
    // for whichever group is open. A modulator you cannot see is a modulator you cannot aim.
    struct ModView : juce::Component, juce::Timer {
        ModView(NoctuaryProcessor& p, NoctuaryEditor& o);
        void paint(juce::Graphics&) override;
        void resized() override;
        void timerCallback() override;
        void mouseDown(const juce::MouseEvent&) override;
        void mouseDrag(const juce::MouseEvent&) override;
        void mouseUp(const juce::MouseEvent&) override;
        void mouseMove(const juce::MouseEvent&) override;

        struct Row {   // one LFO or one envelope: its curve and the controls that shape it
            std::vector<std::unique_ptr<juce::Component>> controls;
            std::vector<std::unique_ptr<juce::Label>> labels;
            std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> sliders;
            std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>> combos;
            juce::Rectangle<int> curve;
            juce::String title;
        };
        Row lfos[ambient::kNumLfos];
        static constexpr int kEnvRows = ambient::kNumModEnvs + ambient::kSlots;   // the six, then the four sources' own
        Row envs[kEnvRows];

        // A card in the lane: one modulation source, drawn with its own live shape.
        struct Card {
            ambient::ModSource source = ambient::ModSource::None;
            juce::String label;
            juce::Colour colour;
            juce::Rectangle<int> bounds;
            int tab = 0;                 // which tab this card belongs to
            int index = 0;               // LFO or envelope number, for the curve
        };
        std::vector<Card> cards;
        int tab = 0;                     // 0 LFO, 1 envelopes, 2 matrix
        int envPage = 0;                 // on the envelope tab: 0 the six, 1 the sources' own
        int hoverCard = -1;
        int dragCard = -1;               // the card being dragged onto a knob
        juce::Point<int> dragPos;
        // Editing an envelope's shape: which one, and which of its breakpoints is under the mouse.
        int dragEnv = -1, dragPoint = -1, hoverEnv = -1, hoverPoint = -1;

        void setTab(int t);
        void setEnvPage(int page);
        bool envVisible(int env) const;                                 // on the page that is showing
        const ambient::ModEnv& shapeOf(int env) const;                  // a row's shape, of the six or a source's
        juce::String envKey(int env, const char* field) const;          // a row's parameter: "env3_time", "src2_env_time"
        void paintLane(juce::Graphics&);
        void paintCard(juce::Graphics&, const Card&, bool hot);
        void paintLfo(juce::Graphics&, int i);
        void paintEnv(juce::Graphics&, int i);
        void mouseDoubleClick(const juce::MouseEvent&) override;
        // The envelope editor. The shapes were always there in the engine -- sixteen breakpoints,
        // a curve on every segment, a sustain point and a loop -- but nothing could reach them
        // except a preset, so in the panel they were pictures of something you could not touch.
        // One row's geometry, asked for by both the drawing and the mouse so they cannot drift.
        struct EnvGeom { float x0 = 0, w = 1, cy = 0, h = 1, len = 1, depth = 1; bool uni = false; };
        EnvGeom envGeom(int env) const;
        int  envAt(juce::Point<int> pos, int* pointOut = nullptr) const;   // -1 if not on a curve
        juce::Point<float> envToXY(int env, float time, float value) const;
        void envFromXY(int env, juce::Point<int> pos, float& time, float& value) const;
        ambient::ModEnv envCopy(int env) const;
        void envCommit(int env, const ambient::ModEnv&, const juce::String& what);
        void envShapeMenu(int env, int point);
        ambient::LfoSpec specOf(int i) const;
        // Where a drag ended: the parameter under the mouse, or none.
        int paramUnder(juce::Point<int> screenPos) const;

        NoctuaryProcessor& proc;
        NoctuaryEditor& owner;
        juce::TextButton tabLfo{ "LFO" }, tabEnv{ "ENVELOPES" }, tabMatrix{ "MATRIX" };
        juce::TextButton pageMod{ "MODULATION 1-6" }, pageSrc{ "SOURCES 1-4" };
        // The matrix page: a table of routes (EditorMatrix.cpp), where a text box used to be.
        std::unique_ptr<edt::RouteTable> table;
        juce::Label matrixInfo, hint;
        juce::Rectangle<int> lane, tabsArea, content;
        ~ModView() override;
        void pullMatrix();
        // Adds a route from a dragged source to a parameter, with a small default depth.
        bool addRoute(ambient::ModSource src, ambient::ParamId target);
    };
    std::unique_ptr<ModView> mod_;
    juce::Viewport modPort_;
    std::unique_ptr<juce::TextButton> modButton_;
public:
    // The cells the modulation strip needs to find a drop target and to mark modulated knobs.
    int cellParamAt(juce::Point<int> screenPos) const;
    juce::Rectangle<int> cellScreenBounds(int cellIndex) const;
private:
    // Displays that live inside the grid, one per section row, filling the room the knobs leave.
    // The grid used to leave that room empty; the displays are what a Pigments-style layout puts
    // there, and each one is drawn from the numbers the engine is using, not from an illustration.

    // The two filters as a frequency response: the state-variable filter in the voice's colour,
    // the z-plane cascade in the accent, and what a note actually meets after both.
    struct FilterView : juce::Component, juce::Timer {
        explicit FilterView(NoctuaryProcessor& p) : proc(p) { setInterceptsMouseClicks(false, false); startTimerHz(15); }
        void paint(juce::Graphics&) override;
        // Nothing but parameters is drawn here, so nothing but a parameter can change it.
        void timerCallback() override
        { const uint32_t g = proc.paramGeneration(); if (isShowing() && g != seen) { seen = g; repaint(); } }
        NoctuaryProcessor& proc;
        uint32_t seen = 0xffffffffu;
    };
    // One source slot: the wavetable frame, the FM cycle, the clip with its grain window, or the
    // colour of the noise -- whichever the slot is set to. A table (Harmonic or Wavetable) is drawn
    // whole and in depth -- every frame a line, the first at the front, the frame at Position lit --
    // and a click turns it into the flat picture of that one frame with its neighbours, and back.
    struct SourceView : juce::Component, juce::Timer {
        SourceView(NoctuaryProcessor& p, int s) : proc(p), slot(s) { setInterceptsMouseClicks(true, false); startTimerHz(15); }
        void paint(juce::Graphics&) override;
        void paintSource(juce::Graphics&);
        // A slot that enters on a shape of its own carries a small picture of it in the corner, with
        // the loudest voice's place in it; a click on the picture opens the shape in the editor.
        bool ownEntrance() const;
        juce::Rectangle<int> entranceBox() const;
        void paintEntrance(juce::Graphics&);
        void mouseDown(const juce::MouseEvent&) override;
        // A wavetable frame, an FM cycle and a noise colour are pictures of parameters and change
        // only when one moves -- or, for a table, when a modulation moves its Position, which is
        // watched here. A clip's grains, the additive bank's partials, a bowed string and a
        // spectral model are what the engine is doing this instant, and are drawn every tick.
        void timerCallback() override
        {
            if (!isShowing()) return;
            const int type = static_cast<int>(std::lround(proc.engine().getParam(ambient::slotParamIds(slot - 1)[0])));
            const bool alive = type == 3 || type == 5 || type == 6 || type == 7 || type == 8 || type >= 10;   // ...and the near sources
            const bool moved = (type == 1 || type == 9) && std::fabs(livePosition() - shownPos) > 0.002f;
            const uint32_t g = proc.paramGeneration();
            if (alive || moved || ownEntrance() || g != seen) { seen = g; repaint(); }
        }
        // Position as it is playing: the knob, the morph or map blend, what the matrix adds -- and
        // the slot's own Pos Drift, which is the one that actually moves. The drift lives inside
        // the voice and never left it, so for a preset whose only movement was Pos Drift the
        // picture stood still while the sound wandered a third of the table. The engine answers
        // with -1 when nothing sounds; then the knob is all there is to draw.
        float livePosition() const
        {
            const float live = proc.engine().displaySlotPosition(slot - 1);
            if (live >= 0.0f) return juce::jlimit(0.0f, 1.0f, live);
            const ambient::ParamId id = ambient::slotParamIds(slot - 1)[6];
            return juce::jlimit(0.0f, 1.0f, proc.engine().effectiveParam(id) + proc.engine().modAmount(id));
        }
        // The table in depth; false when there is nothing to stand in depth (no table, one frame).
        bool paintTable3D(juce::Graphics& g, juce::Rectangle<float> plot, int type, int table, float pos);
        NoctuaryProcessor& proc;
        uint32_t seen = 0xffffffffu;
        int slot;   // 1 .. 4
        float amp[ambient::kTablePartials] = {};   // smoothed live amplitudes for the additive picture
        bool  flat = false;       // the table as one frame (after a click) rather than in depth
        float shownPos = -1.0f;   // the Position last drawn
        // The table sampled for drawing, kTablePoints + 1 points a frame, and its lines drawn once
        // into an image at the display's size: both rebuilt only when the table or the size changes.
        static constexpr int kTablePoints = 96;
        std::vector<float> tableCycles;
        int    tableFrames = 0;
        double tableSig = -1.0;
        juce::Image tableMesh;
        juce::Rectangle<int> meshBounds;
        float  meshScale = 0.0f;
    };
    // The conductor's notes as they happen: a piano roll scrolling left, one column per tick, so
    // the cluster brain's choices can be watched rather than inferred from the keyboard strip.
    struct BrainView : juce::Component, juce::Timer {
        explicit BrainView(NoctuaryProcessor& p) : proc(p) { setInterceptsMouseClicks(false, false); startTimerHz(15); }
        void paint(juce::Graphics&) override;
        void timerCallback() override;
        NoctuaryProcessor& proc;
        static constexpr int kCols = 240;                 // 16 s at 15 Hz
        std::vector<std::array<bool, 128>> hist = std::vector<std::array<bool, 128>>(kCols);
        int head = 0;
        int lo = 36, hi = 84;                             // the note range in view, widened as notes arrive
    };
    // Three rolls: the conductor's, the second conductor's, and one on the Autoplay tab, where
    // watching the chord travel one voice at a time is the whole point of the mode.
    std::unique_ptr<BrainView> brainView_, brainView2_, brainView3_;
    // The stereo stage: every sounding voice as a dot, left-right by its pan, near-far by its
    // plane, size by its envelope -- the spatial model (concept.md) as a picture, moving.
    struct StageView : juce::Component, juce::Timer {
        explicit StageView(NoctuaryProcessor& p) : proc(p) { startTimerHz(15); }
        void paint(juce::Graphics&) override;
        void timerCallback() override { if (isShowing()) repaint(); }
        // The planes are the things on the stage a hand can move: the conductor's, the keys' and
        // the second conductor's depth, each a line across the room. Drag a line and its knob
        // follows (through the host, so it is automated and undone like any knob). The voices
        // themselves are where the conductor put them and stay a picture.
        void mouseMove(const juce::MouseEvent&) override;
        void mouseDown(const juce::MouseEvent&) override;
        void mouseDrag(const juce::MouseEvent&) override;
        void mouseUp(const juce::MouseEvent&) override;
        void mouseExit(const juce::MouseEvent&) override { hoverLine = -1; repaint(); }
        juce::Rectangle<float> plotRect() const;
        int  lineAt(juce::Point<int>) const;
        std::function<void(const juce::String&)> onUndo;
        int hoverLine = -1, dragLine = -1;
        NoctuaryProcessor& proc;
        struct Dot { float x = 0.0f, y = 0.0f, r = 0.0f; int note = -1; bool on = false; };
        Dot dots[ambient::Engine::kMaxVoices];   // smoothed positions, one per voice slot seen
    };
    std::unique_ptr<StageView> stageView_;
    // The Tuning page's display: the timbre's own roughness curve across the octave (Sethares),
    // the chosen scale's degrees on it, the key the conductor has found, the comma and the tide.
    // What the tuning section computes, drawn, instead of only its numbers.
    struct TuningView : juce::Component, juce::Timer {
        explicit TuningView(NoctuaryProcessor& p) : proc(p) { setInterceptsMouseClicks(false, false); startTimerHz(5); }
        void paint(juce::Graphics&) override;
        void timerCallback() override { if (isShowing()) repaint(); }
        NoctuaryProcessor& proc;
        static constexpr int kPoints = 240;   // five cents apart
        float curve[kPoints] = {};
    };
    std::unique_ptr<TuningView> tuningView_;
    // The Coherence page's display: the four Kuramoto phases on a ring, the Lenia field as a
    // grey grid, the six attractor readings as bars -- the living modulators, seen.
    struct CoherenceView : juce::Component, juce::Timer {
        explicit CoherenceView(NoctuaryProcessor& p) : proc(p) { setInterceptsMouseClicks(false, false); startTimerHz(10); }
        void paint(juce::Graphics&) override;
        void timerCallback() override;
        NoctuaryProcessor& proc;
        // Where the two attractors have been: the last forty seconds of their x/y, drawn as orbits.
        std::vector<juce::Point<float>> lorenzTrail, rosslerTrail;
    };
    std::unique_ptr<CoherenceView> coherenceView_;
    // The Cosmos return's spectrum: what the shifter, the resonator, the vowel and the nebula are
    // handing back, on a log-frequency axis, smoothed the way a meter falls.
    struct CosmosView : juce::Component, juce::Timer {
        explicit CosmosView(NoctuaryProcessor& p);
        void paint(juce::Graphics&) override;
        void timerCallback() override;
        NoctuaryProcessor& proc;
        static constexpr int kN = 2048, kBins = 160;
        std::vector<float> re, im, window;
        std::unique_ptr<ambient::Fft> fft;
        float bins[kBins] = {};   // dB per log-spaced bin, smoothed
        bool  silent = true;
    };
    std::unique_ptr<CosmosView> cosmosView_;
    // What the header used to carry was a small spectrum, at 1024 points and 96 bands. The strip
    // along the bottom of the page now does that properly, so this space says the thing a piece
    // of ambient actually has to be judged by: its loudness and, more to the point, how much of
    // its dynamic range is still there. A drone mastered to -9 LUFS has had the movement squeezed
    // out of its reverb tails and cannot get it back.
    struct LoudnessView : juce::Component, juce::Timer {
        explicit LoudnessView(NoctuaryProcessor& p) : proc(p) { startTimerHz(10); }
        void paint(juce::Graphics&) override;
        void timerCallback() override { if (isShowing()) repaint(); }
        void mouseDown(const juce::MouseEvent&) override;   // click to start measuring again
        NoctuaryProcessor& proc;
    };
    std::unique_ptr<LoudnessView> outputView_;
    // The Vector's square, with the point in it: three corners are the source slots, the fourth
    // is all three together. Drag the point; it is the one control here whose value is a place.
    struct VectorView : juce::Component, juce::Timer {
        explicit VectorView(NoctuaryProcessor& p) : proc(p) { startTimerHz(15); }
        void paint(juce::Graphics&) override;
        void timerCallback() override { if (isShowing()) repaint(); }
        void mouseDown(const juce::MouseEvent& e) override { drag(e); }
        void mouseDrag(const juce::MouseEvent& e) override { drag(e); }
        void drag(const juce::MouseEvent&);
        juce::Rectangle<float> square() const;
        NoctuaryProcessor& proc;
        static constexpr int kTrail = 240;      // where the wander has been, at 20 Hz: twelve seconds
        float tx[kTrail] = {}, ty[kTrail] = {};
        int   head = 0, filled = 0;
    };
    std::unique_ptr<VectorView> vectorView_;
    // The wide strip along the bottom of the left column: the same signal as the header's little
    // meter, but with room to say something. A 16384-sample window is 2.9 Hz wide at 48 kHz, so
    // the partials of a low drone are separate lines rather than a hump, and the filter's own
    // response is drawn over them -- what the filter is doing to what is actually there.
    struct SpectrumView : juce::Component, juce::Timer {
        explicit SpectrumView(NoctuaryProcessor& p);
        void paint(juce::Graphics&) override;
        void timerCallback() override;
        void mouseMove(const juce::MouseEvent&) override;
        void mouseExit(const juce::MouseEvent&) override;
        NoctuaryProcessor& proc;
        static constexpr int kN = ambient::kOutTapLen;   // FFT length: the whole tap
        static constexpr int kBands = 480;               // one band per two or three pixels
        static constexpr float kLoHz = 20.0f, kHiHz = 16000.0f;
        // The axis runs a little above zero so the filter, drawn on the same decibels, has
        // somewhere to put a resonance: a flat response is the 0 dB line, not the ceiling.
        static constexpr float kFloorDb = -84.0f, kTopDb = 6.0f;
        static constexpr int kLabelGutter = 26;   // room at the left for the decibel numbers
        std::vector<float> re, im, window;
        std::unique_ptr<ambient::Fft> fft;
        std::vector<float> band, hold;                   // live bands, and a slowly falling trace
        float peakDb = -96.0f;
        int   hoverX = -1;                               // read the frequency under the pointer
    };
    std::unique_ptr<SpectrumView> spectrumView_;
    // The amplitude envelope as a curve, with the loudest voice's level on it.
    struct EnvView : juce::Component, juce::Timer {
        explicit EnvView(NoctuaryProcessor& p) : proc(p) { setInterceptsMouseClicks(false, false); startTimerHz(15); }
        void paint(juce::Graphics&) override;
        void timerCallback() override { if (isShowing()) repaint(); }
        NoctuaryProcessor& proc;
    };
    std::unique_ptr<EnvView> envView_;
    std::unique_ptr<FilterView> filterView_;
    std::unique_ptr<SourceView> source1View_, source2View_, source3View_, source4View_;
    std::unique_ptr<juce::TextButton> browseButton_;
    void setPage(int page);   // 0 edit, 1 perform, 2 browse
    void showMappingEditor();
    juce::Component::SafePointer<juce::TextEditor> mapEditor_;
    juce::String mapText_;
    bool mapOpen_ = false;
    std::unique_ptr<juce::ComboBox> soundBox_;
    juce::ComboBox* cosmosBox_ = nullptr;   // not owned: its cell in the Cosmos section owns it
    juce::ComboBox* morphABox_ = nullptr;   // owned by their cells
    juce::ComboBox* morphBBox_ = nullptr;
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::Rectangle<int> header_, helpLine_, keys_;
    bool sounding_[128] = {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoctuaryEditor)
};
