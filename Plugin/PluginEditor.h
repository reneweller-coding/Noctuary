/**
 * @file PluginEditor.h
 * @brief The Noctuary editor: one class, its panel of cells and sections, and the displays,
 *        pages and popups that live inside it.
 *
 * Everything the player sees is NoctuaryEditor. The panel is built from the parameter table
 * (ambient/Params.h): every parameter becomes a Cell (a knob, a switch or a chooser with its
 * label), every parameter's section a Section, and the sections are arranged in Groups that
 * follow the signal flow -- VOICE, MORPH, FOREGROUND, BACKGROUND, COSMOS, CONDUCTOR, ANALYSIS.
 * Rows whose sections are of a kind (the four sources, the two filters, the effect pairs, the
 * conductor's tables) page through TabRows, and the leftover width of every row is a display
 * drawn from the numbers the engine is using: the source's table, the filter's response, the
 * stage, the note roll, the spectrum. Nothing here knows a pixel position in advance: the layout
 * is measured (layoutBody), the design size follows from it, and the whole editor is scaled to
 * the window, so dragging the corner is the zoom and nothing ever reflows.
 *
 * Three layout modes share the same tables: Normal (one page, tabs), Compact (the same page in
 * narrower columns, every page refitted taller) and Expanded (every page of every tab row under
 * one another, no tabs). Four pages share the window: the panel, Perform (the eight macros and
 * the morph, large), Browse (the preset list, the column browser and the map of every preset)
 * and Help (the manual by topic, its pictures snapshots of the panel itself). Along the bottom
 * of the panel runs the modulation strip (ModView): a card per modulator, dragged onto a knob to
 * route it, with the drag drawn over the whole panel (DragOverlay) and the new route's depth
 * offered where it landed (DepthPopup).
 *
 * The state a knob can destroy is only the parameters, so undo, redo and A/B are whole
 * snapshots of them (Snapshot), taken by a mouse listener on every control (UndoHook). The
 * manual is exported from a running editor (ManualJob, `AMBIENT_MANUAL=<folder>`), because its
 * pictures are the real sections with their real values; the other `AMBIENT_*` environment
 * variables read in the constructor exist for that and for photographing the panel.
 *
 * The class is split over several translation units: PluginEditor.cpp (construction, the cells,
 * the layout, undo, the help page and the manual, the timer, the header's painting),
 * EditorViews.cpp (the displays), EditorBrowse.cpp (Browse and Perform), EditorModStrip.cpp
 * (the strip and the envelope editor), EditorDrag.cpp (the drag overlay and the depth popup),
 * EditorMatrix.cpp (the route table) and EditorSpectrum.cpp (the spectrum strip); what they
 * share -- the grid's geometry and the palette -- is EditorCommon.h.
 */
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

/** @brief The editor's own helpers (EditorCommon.h); RouteTable, the matrix page's table of routes (EditorMatrix.cpp), is only named here for ModView::table. */
namespace edt { class RouteTable; }   ///< EditorCommon.h / EditorMatrix.cpp

/**
 * @brief The editor groups the sections the way the signal flows:
 *   VOICE (Oscillator, Air, Envelope, Filter, Space) -> FOREGROUND (Ensemble, Delay, Delay 2,
 *   Near Reverb) -> out; FOREGROUND -> COSMOS (parallel, returns) ; FOREGROUND -> BACKGROUND
 *   (Cloud, Far Reverb) -> out ; CONDUCTOR (Cluster Brain, Tuning) drives the voices;
 *   MORPH blends two full presets. A routing map in the header draws exactly this.
 *
 * The one component the host opens. It owns the processor's view of every parameter (cells_),
 * the layout that arranges them (sections_, groups_, tabRows_, expandedGroups_), the displays in
 * the grid, the four pages, the modulation strip and the header with the master section, the
 * help line, the keyboard strip and the loudness meter. A juce::Timer at 12 Hz keeps the panel
 * in step with the processor: the section boxes, the labels' MIDI markings, the cells a slot's
 * type uses, the live arcs on the knobs. Everything runs on the message thread; the engine is
 * only ever read through NoctuaryProcessor and the APVTS.
 */
class NoctuaryEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    /**
     * @brief Builds the whole panel from the parameter table and sizes the window.
     *
     * Fills the group and tab tables, builds every cell (buildCells), the header's buttons and
     * boxes, the displays, the pages and the strip; measures the body once for the design size,
     * fixes the aspect ratio and opens at the largest fraction of the screen that fits. Then
     * reads the `AMBIENT_*` environment variables (layout, page, preset, tabs, manual export,
     * screenshot, wavetable, parameter overrides, matrix, route) that exist for photographing
     * the panel, and starts the timer.
     * @param p  the processor this editor shows
     */
    explicit NoctuaryEditor(NoctuaryProcessor&);
    /** @brief Detaches the look and feel before the members go. */
    ~NoctuaryEditor() override;

    /**
     * @brief Paints the header in design space: the background lift, the title, the help line
     *        (the control under the mouse, its value and what it does), the keyboard strip with
     *        the sounding notes and the brain's root, the status line, the cascade's aura, the
     *        REC time and the Master section's frame. Everything else is a child component.
     * @param g  the graphics, in window coordinates until the scale is applied
     */
    void paint(juce::Graphics&) override;
    /**
     * @brief Measures the body, derives the design size and the scale, transforms every child
     *        (except JUCE's corner resizer) and places the header's controls, the pages, the
     *        strip, the viewport and the drag overlay.
     */
    void resized() override;
    /**
     * @brief The right-click menu of a knob: MIDI learn, clear the CC, and every route that
     *        modulates it, each removable. Reached through the mouse listener every cell adds.
     * @param e  the press; only a popup-menu press on a parameter cell does anything
     */
    void mouseDown(const juce::MouseEvent&) override;
    /**
     * @brief The standalone window is JUCE's own, and it is built with a minimise and a close button and
     * nothing else. The editor is the first thing that can reach it, and only once it has been put
     * inside it -- hence here rather than in the constructor.
     */
    void parentHierarchyChanged() override;

private:
    /**
     * @brief The 12 Hz refresh: the near layer's Auto edge, the journey status, the modulation
     *        and live arcs on every knob, the gesture editor's text, the four preset boxes
     *        following the processor, the labels' learn / CC marks, the cells a type uses, and
     *        a repaint of the header and the Morph section.
     */
    void timerCallback() override;
    /** @brief Opens a file chooser for a Scala .scl scale and loads it as the user scale. */
    void chooseScalaFile();
    /**
     * @brief Opens a file chooser for a wavetable or a texture clip and loads what was chosen.
     * @param wavetable  true for a table into the user slot, false for a clip
     * @param slot       slot < 0: the clip goes into every slot
     */
    void chooseSourceFile(bool wavetable, int slot = -1);
    /**
     * @brief Opens a file chooser for an impulse response for the Room.
     * @param second  true for Impulse B (Room Morph fades to it), false for Impulse A
     */
    void chooseImpulseFile(bool second = false);
    /** @brief the near source's own recording (the near layer's Clip type) */
    void chooseNearClipFile();
    /** @brief greys the cells a slot's type does not use, names the loaded files */
    void updateSourceCells();
    /**
     * @brief the same for the Near Source's type; true if a cell changed
     * @return true if a cell's `unused` flag changed, so the page has to be laid out again
     */
    bool updateNearCells();
    /**
     * @brief The index into cells_ of the cell that holds a parameter.
     * @param id  the parameter
     * @return the cell index, or -1 if the parameter has no cell (the master gain, in the header)
     */
    int  cellForParam(ambient::ParamId id) const;
    int  tableCell_ = -1,      ///< the Wavetable... button's cell (Source 2); -1 while not built
         impulseCell_ = -1,    ///< the Impulse A... button's cell
         impulseBCell_ = -1,   ///< the Impulse B... button's cell
         nearClipCell_ = -1;   ///< the near source's Clip... button's cell; the labels of all four name the loaded file
    bool autoSeen_ = true;   ///< the near layer's Auto as the timer last saw it (a rising edge draws anew)
    /** @name Journeys: the box of files, the status label, and the files behind the box's ids.
     *  @{ */
    juce::ComboBox* journeyBox_ = nullptr;   ///< the chooser in the Morph section (owned by its cell); item id = index into journeyFiles_ + 1
    juce::Label*    journeyStatus_ = nullptr;   ///< the step that is playing, or how many steps have been written
    juce::Array<juce::File> journeyFiles_;   ///< the .journey files the box offers, in item order
    /** @brief Rescans the journey folders and refills the box. */
    void fillJourneyBox();
    /** @brief Asks for a name and writes the journey being written into Documents/Noctuary/Journeys. */
    void saveJourneyAs();
    /** @} */
    int  textureCell_[ambient::kSlots] = { -1, -1, -1, -1 };   ///< a Texture... button in every source section
    /**
     * @brief Builds a cell for every parameter, its section on first sight, and the extra cells
     *        (file loaders, section preset banks, morph slots, the journey row).
     */
    void buildCells();
    /** @brief Gives every knob and switch the colour of its section's group. */
    void colourCellsByGroup();
    /**
     * @brief Adds a cell that is not a parameter (a button, a chooser, a label) to a section.
     * @param section  the section's name
     * @param comp     the component, taken over by the cell
     * @param label    the caption under it
     * @param units    its width in cells
     * @return the new cell's index into cells_, or -1 if there is no such section
     */
    int  addExtraCell(const juce::String& section, std::unique_ptr<juce::Component> comp, const juce::String& label, int units);
    /** @name Help: the line in the header follows the mouse; the page is the manual by topic.
     *  @{ */
    /**
     * @brief A registered control under the mouse: its parameter goes into the help line.
     * @param e  the event; its component is looked up in helpParamOf_
     */
    void mouseEnter(const juce::MouseEvent&) override;
    /**
     * @brief The mouse left the control the help line was showing: back to the general hint.
     * @param e  the event; its component is looked up in helpParamOf_
     */
    void mouseExit(const juce::MouseEvent&) override;
    /**
     * @brief F1 opens and closes the help, Esc closes it, Ctrl+Z undoes, Ctrl+Y redoes.
     * @param k  the key
     * @return true if the key was one of these
     */
    bool keyPressed(const juce::KeyPress&) override;
    int  hoveredParam_ = -1;                              ///< what the mouse is over, for the help line
    std::map<juce::Component*, int> helpParamOf_;         ///< every control that has a parameter behind it
public:
    /**
     * @brief the modulation strip registers its own
     * @param c   the control; it also gets this editor as a mouse listener
     * @param id  the parameter behind it
     */
    void registerHelp(juce::Component*, ambient::ParamId);
    /** @} */
private:
    /**
     * @brief Undo, redo and an A/B compare, all in terms of whole parameter snapshots: the instrument
     * has no other state that a knob can destroy, and a snapshot is 320 floats.
     */
    struct Snapshot {
        std::vector<float> v;   ///< every parameter's value, in paramTable() order
        juce::String what;      ///< what was about to change when it was taken (the undo button's tooltip)
    };
    std::vector<Snapshot> undo_,   ///< the undo stack, at most 32 deep
                          redo_;   ///< what undo took back, cleared by the next change
    Snapshot slotA_,   ///< state A of the A/B compare; empty until the first swap
             slotB_;   ///< state B
    bool     showingB_ = false;   ///< which of the two the panel shows
    /**
     * @brief The parameters as they stand now.
     * @param what  the name the snapshot carries
     * @return the snapshot
     */
    Snapshot takeSnapshot(const juce::String& what) const;
    /**
     * @brief Writes a snapshot back through the host, parameter by parameter.
     * @param s  the snapshot; ignored if it is not kNumParams long
     */
    void     restore(const Snapshot&);
    /**
     * @brief call before changing many parameters at once
     * @param what  what is about to change ("preset", "randomise Cloud", a knob's name)
     */
    void     pushUndo(const juce::String& what);
    /** @brief Back one snapshot, the current state onto the redo stack. */
    void     doUndo();
    /** @brief Forward one snapshot, the current state onto the undo stack. */
    void     doRedo();
    /** @brief The A/B button: parks the state, then swaps the two. */
    void     swapAB();
    std::unique_ptr<juce::TextButton> undoButton_,      ///< Undo, in the header
                                      redoButton_,      ///< Redo
                                      abButton_,        ///< A | B: its text says which is showing
                                      compactButton_,   ///< cycles the layout: Normal, Compact, Expanded
                                      recallButton_;    ///< session recall on or off (standalone only; null in a host)
    /**
     * @brief Undo covered presets, dice and envelopes but not the thing done most: a knob turned.
     *
     * One
     * listener on every parameter control takes a snapshot on the press or the wheel that starts a
     * change, named after the parameter, and throttles the wheel so a scroll is one step back, not
     * forty.
     */
    struct UndoHook : juce::MouseListener {
        NoctuaryEditor* editor = nullptr;   ///< where the snapshots go
        std::map<juce::Component*, juce::String> names;   ///< the name a control's snapshot carries
        std::map<juce::Component*, double> lastWheel;   ///< when each control was last wheeled, ms
        /**
         * @brief A press on a registered control takes a snapshot before the value moves.
         * @param e  the press; its component is looked up in names
         */
        void mouseDown(const juce::MouseEvent& e) override
        {
            auto it = names.find(e.eventComponent);
            if (it != names.end() && editor != nullptr) editor->pushUndo(it->second);
        }
        /**
         * @brief A wheel step takes a snapshot only if the last one on this control is a second old.
         * @param e  the wheel event; its component is looked up in names
         */
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
    UndoHook undoHook_;   ///< the one listener, added to every parameter control
    /**
     * @brief Dragging a modulation card onto a knob.
     *
     * The gesture crosses half the panel, and the strip
     * could only draw inside itself, so as soon as the hand left the lane there was nothing to
     * see: no line, no card, no sign of which modulator was in flight. This is drawn on top of
     * everything instead -- the line from the card, the card under the cursor, and a ring around
     * the knob it would land on, named.
     */
    struct DragOverlay : juce::Component {
        /** @brief Catches no clicks and stays on top: it is only ever drawn on. */
        DragOverlay() { setInterceptsMouseClicks(false, false); setAlwaysOnTop(true); }
        /**
         * @brief The ring and name on the target knob, the curved lead from the card, the card under the cursor (EditorDrag.cpp).
         * @param g  the graphics
         */
        void paint(juce::Graphics&) override;
        bool active = false;   ///< a drag is in flight; nothing is drawn otherwise
        juce::Point<float> from,   ///< where the card came from, in the overlay's coordinates
                           to;     ///< where the cursor is
        juce::String name,         ///< the modulator's label
                     targetName;   ///< the parameter under the cursor (empty over nothing)
        juce::Colour colour { 0xffffffff };   ///< the modulator's colour
        juce::Rectangle<float> target;   ///< the knob it would land on; empty over nothing
    };
    DragOverlay dragOverlay_;   ///< covers the whole editor, above everything
    /**
     * @brief Pigments' little knob: after the drop, the depth of the route that was just made, in the
     * modulator's own colour and named after both ends of it.
     *
     * The next click anywhere else puts
     * it away.
     * @note a Component is already a MouseListener: inheriting it twice is ambiguous
     */
    struct DepthPopup : juce::Component {
        /**
         * @brief Builds the slider at the route's current depth and registers as a global mouse listener.
         * @param o  the editor, which rewrites the route
         * @param s  the route's source
         * @param t  the route's target
         * @param c  the source's colour
         */
        DepthPopup(NoctuaryEditor& o, ambient::ModSource s, ambient::ParamId t, juce::Colour c);
        /** @brief Removes the global mouse listener again. */
        ~DepthPopup() override;
        /**
         * @brief The card: source -> target in the source's colour over the slider.
         * @param g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief The slider across, the remove button at the right. */
        void resized() override;
        /**
         * @brief One callback serves both roles: the component's own presses and, because this is
         * registered as a global listener while it lives, everybody else's.
         *
         * A press inside keeps
         * it; a press anywhere else puts it away -- posted, since a component may not be deleted
         * from inside its own mouse callback.
         * @param e  the press, from anywhere on the desktop
         */
        void mouseDown(const juce::MouseEvent& e) override
        {
            if (e.eventComponent == this || isParentOf(e.eventComponent)) return;
            juce::MessageManager::callAsync([w = juce::Component::SafePointer<DepthPopup>(this)] {
                if (w != nullptr) w->owner.hideDepthPopup();
            });
        }
        NoctuaryEditor& owner;   ///< the editor that made it
        ambient::ModSource source;   ///< the route's source
        ambient::ParamId target;   ///< the route's target
        juce::Colour colour;   ///< the source's colour
        juce::Slider depth;   ///< -1 .. 1, written straight into the route on every change
        juce::TextButton remove{ "x" };   ///< takes the route away and closes the popup
    };
    std::unique_ptr<DepthPopup> depthPopup_;   ///< the one popup, or none
public:
    /**
     * @brief Draws (or moves) the drag in flight: the lead from the card to the cursor, and the
     *        ring on the knob under it (EditorDrag.cpp).
     * @param screenFrom  where the card is, in screen coordinates
     * @param screenTo    where the cursor is, in screen coordinates
     * @param name        the modulator's label on the card
     * @param c           the modulator's colour
     */
    void showModDrag(juce::Point<int> screenFrom, juce::Point<int> screenTo, const juce::String& name, juce::Colour);
    /** @brief The drag ended: the overlay goes blank. */
    void hideModDrag();
    /**
     * @brief Shows the depth popup for a route beside a screen point, inside the panel.
     * @param src       the route's source
     * @param target    the route's target
     * @param c         the source's colour
     * @param screenAt  where the drop happened, in screen coordinates
     */
    void showDepthPopup(ambient::ModSource, ambient::ParamId, juce::Colour, juce::Point<int> screenAt);
    /** @brief Deletes the depth popup, if one is up. */
    void hideDepthPopup();
    /**
     * @brief Rewrites one route's depth (the matrix travels as text, as everywhere else here).
     * @param src     the route's source
     * @param target  the route's target
     * @param depth   the new depth, -1 .. 1
     * @return true if the engine accepted the rewritten matrix
     */
    bool setRouteDepth(ambient::ModSource, ambient::ParamId, float depth);
    /**
     * @brief Drops one route from the matrix, the rest written back as they stand.
     * @param src     the route's source
     * @param target  the route's target
     * @return true if the engine accepted the rewritten matrix
     */
    bool removeRoute(ambient::ModSource, ambient::ParamId);
    /**
     * @brief The depth of a route as the matrix has it.
     * @param src     the route's source
     * @param target  the route's target
     * @return the depth, or 0 if there is no such route
     */
    float routeDepth(ambient::ModSource, ambient::ParamId) const;
private:
    /**
     * @name The two section layers that live inside their own sections rather than in the header:
     * 156 filters and 41 plucks are lists you go looking for, not things you keep on the toolbar.
     * Not owned: the cell owns the component, the editor only needs to read the selection back.
     * @{ */
    juce::ComboBox* zPresetBox_ = nullptr;   ///< the Z-Plane section's filter presets, by family
    juce::ComboBox* strikePresetBox_ = nullptr;   ///< the Strike section's pluck presets, by family
    juce::ComboBox* nearPresetBox_ = nullptr;   ///< the Near Events section's foreground presets, by family
    /** @} */
    /**
     * @brief Compact: the same page in columns kCompactFactor as wide, every page refitted into them --
     * narrower and taller, nothing left out. Kept in the plugin state.
     */
    bool compact_ = false;
    static constexpr float kCompactFactor = 0.88f;   ///< the column width of Compact, as a fraction of Normal's
    /**
     * @brief Expanded: every page of every tab row placed under one another.
     *
     * No tabs to click, a
     * tall page, and the four sources, the two filters and the conductor's six tables all in
     * sight at once -- for a tall screen, or for reading a preset through.
     */
    bool expanded_ = false;
    /** @brief The manual's pictures: every section open whatever its switch says, and the tabs. */
    bool openAll_ = false;
    /**
     * @brief updates unused/collapsed from the parameters; true if anything changed
     * @return true if a section's collapsed state changed, so the page has to be laid out again
     */
    bool refreshLayoutState();
    /** @brief The die in a section's title: one click randomises that section, shift-click nudges it. */
    std::map<juce::String, juce::Rectangle<int>> diceOf_;
    /**
     * @brief Draws every parameter of a section again (the die), after a snapshot for undo.
     * @param name    the section
     * @param subtle  true (shift held) keeps every value near where it already is
     */
    void     randomiseSection(const juce::String& name, bool subtle);
    /** @brief Lays the body out again and resizes the window to the new design shape. */
    void     rebuildLayout();
    /**
     * @brief Switches the layout mode, stores it in the processor and rebuilds.
     * @param mode  0 normal, 1 compact, 2 expanded
     */
    void     applyLayoutMode(int mode);
    /**
     * @brief The help page: the manual's topics in a list, the chosen topic's text, its pictures
     *        (snapshots of the panel's own sections), a live copy of the topic's display and,
     *        on the first topic, the signal-flow diagram. Also the source of the manual's pictures.
     */
    struct HelpView : juce::Component, juce::ListBoxModel {
        /**
         * @brief Builds the list and the text, and generates the last topic (every parameter with its help).
         * @param p  the processor the help reads
         * @param o  the editor whose sections it photographs
         */
        HelpView(NoctuaryProcessor&, NoctuaryEditor&);
        /**
         * @brief The frame, the topic's pictures in their column, the title and the hint line.
         * @param g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief The topics on the left, the text at a readable width, the pictures and the live display in the column right of it. */
        void resized() override;
        /**
         * @brief The manual's topics plus the generated parameter topic.
         * @return ambient::numHelpTopics() + 1
         */
        int  getNumRows() override;
        /**
         * @brief One topic title, highlighted when selected.
         * @param row       the topic
         * @param g         the graphics
         * @param w         row width
         * @param h         row height
         * @param selected  whether the row is the chosen one
         */
        void paintListBoxItem(int row, juce::Graphics&, int w, int h, bool selected) override;
        /**
         * @brief Puts the topic's text into the editor and shows its pictures.
         * @param row  the topic chosen, -1 for none
         */
        void selectedRowsChanged(int row) override;
        /**
         * @brief Takes the topic's pictures: its sections, its live display, and -- for the manual -- every
         *        tab that belongs to the chapter, from the tab rows themselves.
         * @param row  the topic
         */
        void showTopic(int row);
        NoctuaryProcessor& proc;   ///< the processor the help reads
        NoctuaryEditor& owner;   ///< the editor whose panel it photographs
        juce::ListBox topics;   ///< the topic list
        juce::TextEditor text;   ///< the chosen topic's text, read-only
        juce::String parameters;   ///< the generated last topic: every parameter with its help
        /**
         * @brief The pictures: snapshots of the topic's sections, fresh from the panel with its current
         * values, and a live display of the unit -- the same component the page uses, a second copy.
         */
        std::vector<juce::Image> pics;
        /**
         * @brief Pictures of whole tabs.
         *
         * The help page does not draw these -- its picture column
         * holds two or three -- but the manual prints every one of them, with its tab's
         * own name as the caption.
         */
        std::vector<juce::Image> tabPics;
        juce::StringArray        tabNames,      ///< per tab picture: the tab's own name
                                 tabCaptions,   ///< per tab picture: its caption
                                 picCaptions;   ///< per section picture: its caption
        juce::String             liveCaption;   ///< the caption of the live display
        std::vector<juce::StringArray> tabSections;   ///< per tab picture: the sections under it
        std::vector<juce::Rectangle<int>> picRects;   ///< where each of pics is drawn (empty when it does not fit)
        std::unique_ptr<juce::Component> live;   ///< the topic's live display, or none
        /** @brief The signal flow, drawn large enough to read: what the routing map in the header was for. */
        struct FlowDiagram : juce::Component {
            /**
             * @brief Catches no clicks; the diagram reads the processor for nothing but its existence.
             * @param p  the processor
             */
            explicit FlowDiagram(NoctuaryProcessor& p) : proc(p) { setInterceptsMouseClicks(false, false); }
            /**
             * @brief The units as boxes in their group colours, the buses as arrows, on a fixed canvas scaled to fit.
             * @param g  the graphics
             */
            void paint(juce::Graphics&) override;
            /**
             * @brief The part of this component the drawing actually covers.
             *
             * The diagram is a fixed
             * canvas scaled to fit, so one of the two dimensions is always left over; a picture
             * of the whole component would be a diagram with a field of black under it.
             * @return the covered rectangle, from the top left
             */
            juce::Rectangle<int> drawn() const;
            static constexpr float kCanvasW = 1000.0f,   ///< the width of the canvas the diagram is drawn on
                                   kCanvasH = 545.0f;    ///< its height
            NoctuaryProcessor& proc;   ///< the processor
        };
        FlowDiagram flow;   ///< shown on the overview topic only
        int topic = 0;   ///< the topic on screen
    };
    /**
     * @name Snapshots for the manual: a section as it stands on the panel (its tab is switched in for
     * the picture and back again), the modulation strip, the browser page.
     * @{ */
    /**
     * @brief A picture of one section, its tab opened for the picture and put back.
     * @param name  the section
     * @return the snapshot, or an invalid image if there is no such section
     */
    juce::Image snapshotSection(const juce::String& name);
    /**
     * @brief A picture of one page of a tab row as it opens: the bar, the sections, the display.
     * @param rowIndex  the tab row
     * @param page      the page on it
     * @return the snapshot, or an invalid image if there is no such page
     */
    juce::Image snapshotTab(int rowIndex, int page);
    /**
     * @brief A picture of a whole page component, made visible for the picture and put back.
     * @param page  the page
     * @return the snapshot, or an invalid image if the page has no size
     */
    juce::Image snapshotPage(juce::Component* page);
    /**
     * @brief The modulation strip as it stands.
     * @return the snapshot, or an invalid image without a strip
     */
    juce::Image snapshotStrip();
    /**
     * @brief The Browse page in whatever view it is in.
     * @return the snapshot, or an invalid image without a browser
     */
    juce::Image snapshotBrowse();
    /**
     * @brief The Perform page.
     * @return the snapshot
     */
    juce::Image snapshotPerform();
    /**
     * @brief The master's corner of the header: the Master section, the meter and the note roll.
     * @return the snapshot, or an invalid image if nothing of it is laid out
     */
    juce::Image snapshotHeader();
    /**
     * @brief The Browse page switched to its map view, the view put back afterwards.
     * @return the snapshot, or an invalid image without a browser
     */
    juce::Image snapshotBrowseMap();
    /**
     * @brief the same view closed in on the current preset, names showing
     * @return the snapshot, or an invalid image without a browser
     */
    juce::Image snapshotBrowseMapZoomed();
    /**
     * @brief One tab of the modulation strip.
     * @param tab     0 LFO, 1 envelopes, 2 matrix
     * @param detail  true for the left third at twice the size, where it can be read on a page
     * @return the snapshot, or an invalid image without a strip
     */
    juce::Image snapshotStripTab(int tab, bool detail = false);
    /**
     * @brief the envelope tab's SOURCES page, from a click on a source's picture
     * @param slot  which source asked (unused: the page shows all four)
     */
    void openSourceEnvelope(int slot);
    /**
     * @brief The sections on one page of a tab row.
     * @param rowIndex  the tab row
     * @param page      the page on it
     * @return the section names, empty for an unknown row or page
     */
    juce::StringArray tabSectionNames(int rowIndex, int page) const;
    /**
     * @brief What a tab is called on its own bar.
     * @param rowIndex  the tab row
     * @param page      the page on it
     * @return the label, empty for an unknown row or page
     */
    juce::String tabName(int rowIndex, int page) const;
    /** @} */
    /**
     * @brief Writes the manual out as pictures and text for Tools/make_manual.py to turn into a PDF.
     *
     * It has to happen here, in a running editor, because the pictures ARE the panel: snapshots
     * of the real sections with their real values, not drawings kept somewhere in step with it.
     * @param dir     the folder the pictures, manual.json and coverage.txt go into
     * @param onDone  called when the last step has run
     */
    void exportManual(const juce::File& dir, std::function<void()> onDone);
    struct ManualJob;
    std::unique_ptr<ManualJob> manual_;   ///< the export in progress, or none
    /** @brief Runs the next step of the manual export and schedules the one after it. */
    void runManualStep();
    std::unique_ptr<HelpView> help_;   ///< the help page
    std::unique_ptr<juce::TextButton> helpButton_;   ///< Help, in the header
    std::unique_ptr<juce::TextButton> aboutButton_;   ///< "?": version, build time, what is loaded
    std::unique_ptr<juce::TooltipWindow> tooltips_;   ///< tooltips after 600 ms

    /** @brief One control on the panel: the component, its label, its host attachment and its width. */
    struct Cell {
        std::unique_ptr<juce::Component> comp;   ///< the knob, switch, chooser, button or label
        std::unique_ptr<juce::Label> label;   ///< the caption under it (renamed by the timer: CC, learn, file name)
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> slider;   ///< the host link of a knob
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> button;   ///< the host link of a switch
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> combo;   ///< the host link of a chooser
        juce::String baseLabel;   ///< the caption before any marking is appended
        int units = 1;   ///< width in cells (a chooser is two, a bank three)
        int param = -1;    ///< ParamId or -1 for extra cells
        /**
         * @brief The slot's type does not use it: not drawn and not laid out, rather than greyed.
         *
         * A
         * strip is as tall as the type it is set to, not as the union of every type.
         */
        bool unused = false;
    };
    /** @brief A titled box of cells: one parameter section, laid out in rows of at most maxUnits. */
    struct Section {
        juce::String name;   ///< the section's name, as the parameter table has it
        std::vector<int> cells;   ///< indices into cells_, in the order they are laid out
        juce::Rectangle<int> bounds;   ///< where it was last placed, in content coordinates
        int maxUnits = 7;   ///< the width in cells a row may reach before it wraps (set by the fit)
        int wideUnits = 0;          ///< the natural width on the tabbed page (decides the columns; the fit may change it)
        int flowUnits = 0;          ///< the width in Expanded's flows, where a section's shape is its own
        int group = -1;   ///< index into groups_, or -1 (the Master section, in the header)
        bool visible = true;   ///< false while its tab is not the open one
        /**
         * @brief Its switch is off (a type of Off, a level at zero, an Active that is not): only the
         * title and the switch's own cells are shown.
         *
         * Off means closed. Opening it is the
         * player's own action on the switch, so the page never rearranges itself.
         */
        bool collapsed = false;
        /**
         * @brief ... unless closing it buys nothing.
         *
         * A section is closed to save room, and where the row
         * has the room anyway, it is laid out open instead: a title beside an empty band is worse
         * than the section itself. Decided by the layout, from the row's own width, and cleared at
         * the start of every pass -- `collapsed` stays what the parameters say, so the timer that
         * watches for a switch being thrown cannot see this and rebuild for ever.
         */
        bool opened = false;
        /**
         * @brief Whether the section shows only its title and switch as it stands.
         * @return collapsed by its switch and not opened by the layout
         */
        bool closedNow() const { return collapsed && !opened; }
    };
    /** @brief A column's box of rows: a name, a colour, the sections of each row and the display beside them. */
    struct Group {
        juce::String name;   ///< the title over the box
        juce::Colour colour;   ///< the group's colour: its bar, its knobs, its tabs
        std::vector<std::vector<juce::String>> rows;   ///< section names per row
        juce::Rectangle<int> bounds;   ///< where it was last placed, in content coordinates
        int column = 0;   ///< which column it stands in
        std::vector<juce::Component*> displays;        ///< per row: a display that fills the leftover width, or null
        /**
         * @brief A row with no sections in it gets its height from here instead of from them; and if the
         * group is marked stretchy, that row also takes whatever height the taller column has
         * left over, so the two columns end level and the page has no hole in it at any size.
         */
        int  minRowH = 0;
        bool stretch = false;   ///< the last group of a column: its last row grows to the body's height
    };

    /**
     * @brief A row of a group whose sections take turns: one page open, the others a tab away.
     *
     * This is
     * what keeps the whole synth on one screen -- the three sources, the two filters, the effect
     * pairs and the conductor's tables are alike enough that seeing one at a time is no loss.
     */
    struct TabRow {
        int group = 0,   ///< index into groups_
            row = 0;     ///< the row of that group which pages
        std::vector<juce::String> names;                       ///< one label per page
        std::vector<std::vector<juce::String>> pages;          ///< section names per page
        std::vector<juce::Component*> displays;                ///< per page: display for the leftover width, or null; two pages may share one
        int active = 0;   ///< the page that is open
        juce::Rectangle<int> bar;   ///< the tab bar, in content coordinates
        std::vector<juce::Rectangle<int>> tabs;   ///< one rectangle per tab, for painting and for the click
    };
    std::vector<TabRow> tabRows_;   ///< the rows that page, filled in the constructor
    /**
     * @brief The tab row of a group's row, if it has one.
     * @param group  index into groups_
     * @param row    the row in it
     * @return the tab row, or null if that row does not page
     */
    TabRow* tabRowFor(int group, int row);
    /**
     * @brief Shows or hides a section's cells (those its state shows) and records the flag.
     * @param s  the section
     * @param v  visible or not
     */
    void    setSectionVisible(Section&, bool);
    /**
     * @brief A click on the content: if it hits a tab of a paging row, that page opens.
     * @param contentPos  the click, in content coordinates
     */
    void    clickTabs(juce::Point<int> contentPos);

    /**
     * @brief The section of a name.
     * @param name  the section's name
     * @return the section, or null if there is none
     */
    Section* findSection(const juce::String& name);
    /**
     * @name Which cells a section shows, and whether it is closed. Both are functions of the parameter
     * state alone, never of the window, so the layout is reproducible and the manual can be.
     * @{ */
    /**
     * @brief Whether a cell is on the page as the section stands.
     * @param s  the section
     * @param c  one of its cells
     * @return false for a cell the slot's type does not use, and for all but the switch's cells of a closed section
     */
    bool cellShown(const Section&, const Cell&) const;
    /**
     * @brief how many of its cells are on the page as it stands
     * @param s  the section
     * @return the count of cells for which cellShown is true
     */
    int  shownCells(const Section&) const;
    /**
     * @brief Whether the section's switch says closed (never while openAll_ is set).
     * @param s  the section
     * @return true if the section has a closer and its switch is off
     */
    bool sectionCollapsed(const Section&) const;
    /** @} */
    /**
     * @brief The width the section takes at its maxUnits: its widest row of shown cells plus the pads.
     * @param s  the section
     * @return pixels
     */
    int      sectionWidth(const Section&) const;
    /**
     * @brief The height the section takes at its maxUnits: its rows of shown cells, the title and a pad.
     * @param s  the section
     * @return pixels
     */
    int      sectionHeight(const Section&) const;
    /**
     * @brief Places the section and its cells, hiding the cells its state does not show.
     * @param s  the section
     * @param x  left edge, in content coordinates
     * @param y  top edge, in content coordinates
     */
    void     layoutSection(Section&, int x, int y);

    NoctuaryProcessor& proc_;   ///< the processor this editor shows
    NoctuaryLookAndFeel laf_;   ///< the editor's look
    float scale_ = 1.0f;   ///< window size / design size
    int   designW_ = 1400,   ///< the design width the body is laid out at
          designH_ = 820;    ///< measured from the layout, not guessed
    int   bodyW_ = 0,   ///< what the body needs across, from the last layoutBody
          bodyH_ = 0;   ///< what it needs down
    /**
     * @brief Places every group and section and records what the body needs (bodyW_, bodyH_):
     *        the tabbed page, or the Expanded page with its wrapping rows and three columns.
     */
    void  layoutBody();
    /**
     * @brief The tabbed page (Normal and Compact).
     *
     * Every page of every row is FITTED to its column: its
     * sections are given the fewest rows at which they stand side by side in the column's width,
     * each as narrow as that allows, so they come out the same height and the page has no hole
     * under a short one; the display takes the width that is left. A row is as tall as the page
     * that is open on it, not as its tallest page, so a one-row page is one row; the design
     * height is still the sum of the tallest pages, so the window never changes shape on a tab
     * click, and the last group of each column grows into the difference.
     */
    void  layoutTabbed();
    std::vector<Cell> cells_;   ///< every control on the panel, indexed by the sections
    std::vector<Section> sections_;   ///< one per parameter section, in order of first appearance
    std::vector<Group> groups_;   ///< the tabbed page's groups, two columns
    /**
     * @brief The Expanded page: its own table of groups and rows, three columns, no tabs.
     *
     * Rows without
     * a display wrap when their sections, open, would run past kWrapW, so a row of closed
     * sections is one line and a row of open ones is two or three, never a column a screen wide.
     */
    std::vector<Group> expandedGroups_;
    static constexpr int kWrapW = 1400;   ///< the width at which an Expanded row without a display wraps, in pixels
    std::map<juce::Component*, int> cellOf_;   ///< a parameter control's cell index, for the right-click menu
    std::unique_ptr<juce::Slider> master_;   ///< the master gain knob in the header

    /**
     * @brief Live picture of the oscillator: one cycle built from the partial amplitudes the loudest
     * voice is summing right now, plus those partials as a spectrum.
     *
     * It moves because the
     * shimmer and the drift move -- it is the sound, not an illustration of it.
     */
    struct ScopeView : juce::Component, juce::Timer {
        /**
         * @brief Catches no clicks and repaints at 20 Hz.
         * @param p  the processor whose engine it reads
         */
        explicit ScopeView(NoctuaryProcessor& p) : proc(p) { setInterceptsMouseClicks(false, false); startTimerHz(20); }
        /**
         * @brief The spectrum as thin bars behind one cycle of the wave the partials make, and the frequency.
         * @param g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief Repaints while on screen; the smoothing happens in paint. */
        void timerCallback() override { if (isShowing()) repaint(); }
        NoctuaryProcessor& proc;   ///< the processor
        float amp[ambient::kMaxPartials] = {};   ///< smoothed towards the engine's values
        int   count = 0;   ///< how many partials have been seen, the most so far
        bool  mode = false;                      ///< false = waveform, true = spectrum
    };
    std::unique_ptr<ScopeView> scope_;   ///< the strand bank's display on the Expanded page (hidden on the tabbed one)
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> masterAttach_;   ///< the master knob's host link
    /** @brief Everything below the header lives in a scrollable content component. */
    struct Content : juce::Component {
        std::function<void(juce::Graphics&)> onPaint;   ///< the editor paints the groups, tabs and sections
        std::function<void(const juce::MouseEvent&)> onMouse;   ///< the editor takes the click: dice and tabs
        /**
         * @brief Hands the painting to the editor.
         * @param g  the graphics
         */
        void paint(juce::Graphics& g) override { if (onPaint) onPaint(g); }
        /**
         * @brief Hands a press to the editor.
         * @param e  the press
         */
        void mouseDown(const juce::MouseEvent& e) override { if (onMouse) onMouse(e); }
    };
    Content content_;   ///< the panel below the header, inside viewport_
    juce::Viewport viewport_;   ///< holds content_; scroll bars hidden, the layout never needs them
    /**
     * @brief Paints the content: the group boxes, the tab bars, every visible section's frame, title, die and notes.
     * @param g  the content's graphics
     */
    void paintContent(juce::Graphics&);
    std::unique_ptr<juce::TextButton> saveButton_,      ///< Save...: a preset file
                                      loadButton_,      ///< Load...: a preset file
                                      recButton_,       ///< Rec: record the output to WAV
                                      calibButton_,     ///< Calibrate the hands (made, but the VR menu holds it)
                                      mapButton_,       ///< Gestures... (made, but the VR menu holds it)
                                      performButton_;   ///< the Perform page
    /**
     * @brief Main brings the panel back from any page; VR holds what only a headset needs (calibration,
     * the gesture table) under one button instead of two on the toolbar.
     */
    std::unique_ptr<juce::TextButton> mainButton_, vrButton_;   ///< the VR menu: calibrate, gestures
    /** @brief Perform page: the eight macros as large knobs plus the morph, instead of the editor. */
    struct PerformView : juce::Component {
        /**
         * @brief Builds the eight knobs with their labels, the morph slider and switch, and the set recorder's buttons.
         * @param p  the processor
         */
        explicit PerformView(NoctuaryProcessor& p);
        /**
         * @brief The frame, the title line, and the morph slots' names (refreshed every paint).
         * @param g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief The set controls on the title line, the knobs in a 4 x 2 grid, the morph along the bottom. */
        void resized() override;
        NoctuaryProcessor& proc;   ///< the processor
        std::vector<std::unique_ptr<juce::Slider>> knobs;   ///< the eight macro knobs
        std::vector<std::unique_ptr<juce::Label>> labels;   ///< their names
        std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> attachments;   ///< their host links
        juce::Slider morph;   ///< the morph position
        juce::ToggleButton morphActive;   ///< the morph on or off
        juce::Label morphLabel,   ///< "A < position > B"
                    aLabel,       ///< slot A's name
                    bLabel;       ///< slot B's name
        /** @name set timeline: record everything that moves, play a set back
         *  @{ */
        juce::TextButton setRec{ "Record set" },     ///< starts recording; pressed again, asks where to save
                         setPlay{ "Play set..." },   ///< opens a set file and plays it
                         setStop{ "Stop set" };      ///< stops playing or recording
        juce::Label setInfo;   ///< recording or playing time, or what a set is
        std::unique_ptr<juce::FileChooser> chooser;   ///< the save / open dialog in flight
        /** @brief Refreshes the Rec button's state and the info line from the processor. */
        void updateSetInfo();
        /** @} */
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> morphAttach;   ///< the morph slider's host link
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> morphActiveAttach;   ///< the morph switch's host link
    };
    std::unique_ptr<PerformView> perform_;   ///< the Perform page
    bool performing_ = false;   ///< the Perform page is the one showing
    /**
     * @brief Shows the Perform page, or the panel again.
     * @param on  true for the Perform page
     */
    void setPerforming(bool on);

    /**
     * @brief Browse page: filterable preset list plus the preset map (points = presets, drag the
     * cursor to blend between neighbours).
     */
    struct BrowseView : juce::Component, juce::ListBoxModel, juce::Timer {
        /**
         * @brief Builds the search, the family and sort boxes, the tag buttons, the list, the map with its
         *        macros and route strip, the four columns and the mode buttons; starts in the column view.
         * @param p  the processor
         */
        explicit BrowseView(NoctuaryProcessor& p);
        /**
         * @brief The background and the column titles.
         * @param g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief Places the controls of the view that is on (columns or map) and hides the other's. */
        void resized() override;
        /** @brief Repaints the map and the list, refreshes the info panel, and mirrors the route editor's text while it is open, applying it when it closes. */
        void timerCallback() override;
        /** @name list
         *  @{ */
        /**
         * @brief How many presets pass the filter.
         * @return the size of filtered
         */
        int  getNumRows() override { return static_cast<int>(filtered.size()); }
        /**
         * @brief One preset: its name in its family's colour, a star if favourite, its measurements as small bars.
         * @param row       the row of the filtered list
         * @param g         the graphics
         * @param w         row width
         * @param h         row height
         * @param selected  whether the row is the chosen one
         */
        void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override;
        /**
         * @brief Selects the preset; a click loads it (morphing into it if that is on) and puts the map cursor on it.
         * @param row  the row of the filtered list
         * @param e    the click
         */
        void listBoxItemClicked(int row, const juce::MouseEvent&) override;
        /** @brief Rebuilds filtered from the search, the columns or the tags and macros, the favourites and the sort; bumps filterGen. */
        void applyFilter();
        /** @} */
        /** @name map
         *  @{ */
        /**
         * @brief The plane of every preset, coloured by group or family, with the cursor, its radius, the
         *        route and the names; zoomed about the mouse and panned by dragging.
         */
        struct MapView : juce::Component {
            /**
             * @brief Remembers the browser it belongs to.
             * @param o  the browser
             */
            explicit MapView(BrowseView& o) : owner(o) {}
            /**
             * @brief The cached cloud, then what moves: favourites, the route, the cursor, the crossing, the names.
             * @param g  the graphics
             */
            void paint(juce::Graphics&) override;
            /**
             * @brief On a preset: selects it. Otherwise: starts a blend drag if Map blend is on, a pan if not.
             * @param e  the press
             */
            void mouseDown(const juce::MouseEvent&) override;
            /**
             * @brief Pans the window onto the plane, or moves the blend cursor through the host.
             * @param e  the drag
             */
            void mouseDrag(const juce::MouseEvent&) override;
            /**
             * @brief Tracks the preset under the mouse for the hover ring and name.
             * @param e  the move
             */
            void mouseMove(const juce::MouseEvent&) override;
            /** @brief Ends a drag or a pan. */
            void mouseUp(const juce::MouseEvent&) override;
            /**
             * @brief Zooms about the mouse, 1 .. 40 times.
             * @param e  the event, whose position is the pivot
             * @param w  the wheel step
             */
            void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
            /** @brief Back to the whole plane. */
            void mouseDoubleClick(const juce::MouseEvent&) override;
            /**
             * @brief The preset whose point is nearest a screen position.
             * @param p        the position, in the map's coordinates
             * @param maxDist  how far away a point may be, in pixels
             * @return the preset index, or -1 if none is within reach
             */
            int  nearestPreset(juce::Point<float> p, float maxDist) const;
            /**
             * @brief Where a point of the plane lands on the screen at the current zoom and centre.
             * @param x  0 .. 1 across the plane
             * @param y  0 .. 1 up the plane
             * @return the position in the map's coordinates
             */
            juce::Point<float> toScreen(float x, float y) const;
            /**
             * @brief The plane position under a screen point.
             * @param s  the position in the map's coordinates
             * @return clamped to the plane
             */
            juce::Point<float> toMap(juce::Point<float> s) const;
            /**
             * @brief The plane position under a screen point.
             * @param s  the position in the map's coordinates
             * @return not clamped: for the zoom's pivot
             */
            juce::Point<float> toMapRaw(juce::Point<float> s) const;
            /**
             * @brief centre the view on a point at a zoom
             * @param x  0 .. 1 across the plane
             * @param y  0 .. 1 up the plane
             * @param z  the zoom, clamped to 1 .. 40; at 1 the centre is the plane's
             */
            void zoomTo(float x, float y, float z);
            BrowseView& owner;   ///< the browser
            int hover = -1;   ///< the preset under the mouse, or -1
            bool dragging = false;   ///< a blend drag is in progress
            /**
             * @brief Seven thousand dots on a few hundred pixels are heaps, not presets.
             *
             * The view zooms
             * about the mouse and pans by dragging; the plane itself never changes, only the
             * window onto it, so a cursor position means the same thing at every zoom.
             */
            float zoom = 1.0f;
            juce::Point<float> centre { 0.5f, 0.5f };   ///< the plane point in the middle of the view
            bool panning = false;   ///< a pan drag is in progress
            juce::Point<float> panFrom,      ///< where the pan started: the mouse
                               centreFrom;   ///< the centre when it started
            /**
             * @brief The cloud itself, drawn once into an image.
             *
             * Eight and a half thousand filled dots,
             * fifteen times a second, were nineteen percentage points of a processor core for a
             * picture that only changes when the window, the zoom, the pan, the filter or the
             * colouring changes. What moves -- the cursor, the ring, the crossing, the names --
             * is drawn live on top of it.
             */
            juce::Image cloud;
            float cloudZoom = -1.0f;   ///< the zoom the cloud was drawn at
            juce::Point<float> cloudCentre { -1.0f, -1.0f };   ///< the centre the cloud was drawn at
            int  cloudW = 0,         ///< the width the cloud was drawn at
                 cloudH = 0,         ///< the height
                 cloudFilter = -1,   ///< the filterGen it was drawn for
                 cloudCount = -1;    ///< the preset count it was drawn for
            bool cloudGroups = false;   ///< whether the cloud was coloured by group
        };
        /** @} */
        /** @brief classic column browser (Omnisphere / Absynth style): each column narrows the list */
        struct Column : juce::ListBoxModel {
            BrowseView* owner = nullptr;   ///< the browser, told to filter again on a click
            juce::String title;   ///< drawn over the column
            juce::StringArray items;          ///< items[0] = "All"
            std::vector<uint32_t> tagBits;    ///< per item: tag mask (0 for family columns / All)
            std::vector<int> familyIdx;       ///< per item: family index or -1
            std::set<int> chosen;             ///< chosen rows (empty = All)
            std::vector<int> counts;          ///< presets per row, rebuilt when the list grows
            int countsFor = -1;               ///< the numPresets() the counts were taken at
            /** @brief Counts the presets behind every row, once per library size. */
            void updateCounts();
            juce::ListBox box;   ///< the column's list
            /**
             * @brief The rows of the column.
             * @return the size of items
             */
            int  getNumRows() override { return items.size(); }
            /**
             * @brief One row: its name, lit when chosen, with its preset count at the right.
             * @param row       the row
             * @param g         the graphics
             * @param w         row width
             * @param h         row height
             * @param selected  unused: chosen is what lights a row
             */
            void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override;
            /**
             * @brief "All" clears; a row toggles alone or, with shift, joins the chosen set.
             * @param row  the row
             * @param e    the click, for its modifiers
             */
            void listBoxItemClicked(int row, const juce::MouseEvent&) override;
            /**
             * @brief Whether a preset passes this column's choice.
             * @param preset  the preset index
             * @return true if nothing is chosen or the preset is in a chosen family or carries a chosen tag
             */
            bool passes(int preset) const;
        };
        Column columns[4];   ///< Family, Character, Motion, Features
        /**
         * @brief Switches the view and shows the controls that belong to it.
         * @param m  0 classic columns, 1 map
         */
        void setMode(int m);
        int mode = 0;   ///< the view that is on: 0 columns, 1 map
        juce::TextButton modeClassic{ "Columns" },   ///< the column view
                         modeMap{ "Map" },           ///< the map view
                         star{ "Favourite" };        ///< stars or unstars the selected preset
        juce::ToggleButton onlyFavourites{ "only favourites" };   ///< the list narrowed to the starred presets
        juce::ToggleButton favouritesFirst{ "favourites first" };   ///< the starred ones at the top, whatever the sort
        /**
         * @brief Five thousand presets include some that barely move.
         *
         * Their measurements say so, and
         * this hides them: nothing is deleted, the list simply stops offering them.
         */
        juce::ToggleButton hideDull{ "hide the still ones" };
        /** @name A preset change as a journey rather than a cut, and how long it takes.
         *  @{ */
        juce::ToggleButton morphOnSelect{ "morph into it" };   ///< choosing a preset morphs into it instead of switching
        juce::Slider morphSeconds;   ///< how long that takes, 0.5 .. 600 s
        /** @} */
        /**
         * @brief What u-he's browsers put beside the list: what this preset is, how it paces itself,
         * what your hands are wired to in it, and where it is filed.
         *
         * Generated from the
         * measurements, the settings and its own matrix (ambient::presetInfoText).
         */
        struct InfoPanel : juce::Component {
            /**
             * @brief The title, then the body's headings in the accent colour with their prose wrapped under them.
             * @param g  the graphics
             */
            void paint(juce::Graphics&) override;
            juce::String title,   ///< the preset's name
                         body;    ///< the text under it
        };
        InfoPanel info2;   ///< the panel beside the list
        int infoFor = -2;              ///< which preset the panel currently describes
        /** @brief Refreshes the info panel for the selected preset (or the current program) if it changed. */
        void updateInfo();
        /**
         * @brief The cloud's colour: the measured groups, or where a preset came from.
         *
         * Groups by
         * default -- what a browser is for is finding a sound, and the pack a sound was written
         * in is a fact about its author.
         */
        juce::ToggleButton colourByGroup{ "groups" };
        /**
         * @brief The four macro sliders.
         *
         * Each is a range over one measured descriptor, wide open by
         * default; narrowing one thins the cloud to what is left, and the view closes in on it.
         */
        struct Macro {
            juce::Slider slider;                  ///< a two-value slider, the range 0 .. 1
            juce::Label label;                    ///< the axis's name
            float ambient::PresetMeta::* field;   ///< the measured descriptor the range applies to
        };
        Macro macros[4];   ///< dark - bright, still - moving, smooth - rough, near - far
        /**
         * @brief Whether any macro range is narrower than the whole.
         * @return true if a slider's minimum is above 0 or its maximum below 1
         */
        bool  macroActive() const;
        /** @brief zoom the map onto what survived the filter */
        void  fitToFilter();
        bool  fitPending = false;   ///< a macro moved: fit after the next applyFilter

        NoctuaryProcessor& proc;   ///< the processor
        juce::TextEditor search;   ///< the name search
        juce::ComboBox family,   ///< the family filter (map view)
                       sort;     ///< the sort order
        std::vector<std::unique_ptr<juce::ToggleButton>> tagButtons;   ///< one per preset tag (map view)
        juce::ListBox list;   ///< the filtered presets
        MapView map;   ///< the map (map view)
        juce::ToggleButton mapActive;   ///< Map blend: dragging the cursor blends between neighbours
        juce::Slider radius;   ///< the blend radius
        juce::Label info;   ///< "n of m presets"
        /** @name route strip (map view): preset routes, play/loop/speed, add the cursor as a point, edit the text
         *  @{ */
        juce::ComboBox routeBox;   ///< the route presets
        juce::ToggleButton routePlay,   ///< play the route
                           routeLoop;   ///< loop it
        juce::Slider routeSpeed;   ///< how fast the route travels
        juce::TextButton routeAdd{ "+ point" },     ///< append the cursor as a waypoint
                         routeClear{ "Clear" },     ///< clear the route
                         routeEdit{ "Route..." };   ///< open the text editor
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> routePlayAttach,   ///< the play switch's host link
                                                                              routeLoopAttach;   ///< the loop switch's host link
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> routeSpeedAttach;   ///< the speed's host link
        juce::Component::SafePointer<juce::TextEditor> routeEditor;   ///< the text editor while its dialog lives
        juce::String routeEditText;                ///< the editor's text as the timer last saw it
                     bool routeEditOpen = false;   ///< the dialog is up: mirror the text, apply it when it is gone
        /** @brief Opens the route as text in a dialog: one point per line, applied when the dialog closes. */
        void showRouteEditor();
        /** @} */
        juce::TextButton toA{ "-> A" },    ///< the selected preset into morph slot A
                         toB{ "-> B" },    ///< into morph slot B
                         load{ "Load" };   ///< loads the selected preset
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> mapActiveAttach;   ///< the blend switch's host link
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> radiusAttach;   ///< the radius's host link
        std::vector<int> filtered;   ///< the presets that pass the filter, in sort order
        int filterGen = 0;      ///< bumped by applyFilter, so the map knows its cached cloud is stale
        int selected = -1;   ///< the preset chosen in the list or on the map, or -1
        /**
         * @name "More like this": the list narrowed to the map's nearest neighbours of the selected
         * preset, and the map zoomed onto them. The similarity is the one the map already has.
         * @{ */
        juce::ToggleButton similar { "similar" };   ///< the switch
        std::vector<int> similarSet;   ///< the selected preset and its neighbours; empty when off
        int similarOf = -1;   ///< the preset the set was taken for
        /** @} */
    };
    std::unique_ptr<BrowseView> browse_;   ///< the Browse page
    /**
     * @brief The modulation strip along the bottom of the main page, in the shape Pigments uses: a lane
     * of every modulator as a small card with its live curve, a row of tabs, and the full editors
     * for whichever group is open.
     *
     * A modulator you cannot see is a modulator you cannot aim.
     */
    struct ModView : juce::Component, juce::Timer {
        /**
         * @brief Builds the LFO and envelope rows with their knobs, the cards of every source, the tab and
         *        page buttons and the route table; opens on the LFO tab at 15 Hz.
         * @param p  the processor
         * @param o  the editor, for undo, help and the drop target
         */
        ModView(NoctuaryProcessor& p, NoctuaryEditor& o);
        /**
         * @brief The lane of cards, then the open tab's panels, then the drag line while a card is in flight.
         * @param g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief The lane along the top, the tab bar, and the open tab's rows and knobs (EditorModStrip.cpp). */
        void resized() override;
        /** @brief Repaints while on screen, 15 Hz. */
        void timerCallback() override;
        /**
         * @brief A card: starts a drag or opens its tab. A tab or page button. An envelope point: starts dragging it; right-click: the shape menu.
         * @param e  the press
         */
        void mouseDown(const juce::MouseEvent&) override;
        /**
         * @brief Moves the envelope point being dragged, or the card in flight (and the editor's overlay).
         * @param e  the drag
         */
        void mouseDrag(const juce::MouseEvent&) override;
        /**
         * @brief Drops the card: on a knob, a route is added and the depth popup shown; the overlay is hidden.
         * @param e  the release, whose screen position is the drop target
         */
        void mouseUp(const juce::MouseEvent&) override;
        /**
         * @brief Tracks the card and the envelope point under the mouse for the highlight.
         * @param e  the move
         */
        void mouseMove(const juce::MouseEvent&) override;

        /** @brief one LFO or one envelope: its curve and the controls that shape it */
        struct Row {
            std::vector<std::unique_ptr<juce::Component>> controls;   ///< the knobs and choosers, in the order they are placed
            std::vector<std::unique_ptr<juce::Label>> labels;   ///< their names
            std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> sliders;   ///< the knobs' host links
            std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>> combos;   ///< the choosers' host links
            juce::Rectangle<int> curve;   ///< where its curve is drawn
            juce::String title;   ///< "LFO 3", "ENV 2", "SOURCE 1"
        };
        Row lfos[ambient::kNumLfos];   ///< the LFO tab's rows
        static constexpr int kEnvRows = ambient::kNumModEnvs + ambient::kSlots;   ///< the six, then the four sources' own
        Row envs[kEnvRows];   ///< the envelope tab's rows, both pages

        /** @brief A card in the lane: one modulation source, drawn with its own live shape. */
        struct Card {
            ambient::ModSource source = ambient::ModSource::None;   ///< the source it stands for
            juce::String label;   ///< "LFO 1", "MACRO A", "KURA 2", "AMP" ...
            juce::Colour colour;   ///< the source's colour (edt::sourceColour)
            juce::Rectangle<int> bounds;   ///< where it is in the lane
            int tab = 0;                 ///< which tab this card belongs to
            int index = 0;               ///< LFO or envelope number, for the curve
        };
        std::vector<Card> cards;   ///< every source that can drive something, in matrix order
        int tab = 0;                     ///< 0 LFO, 1 envelopes, 2 matrix
        int envPage = 0;                 ///< on the envelope tab: 0 the six, 1 the sources' own
        int hoverCard = -1;   ///< the card under the mouse, or -1
        int dragCard = -1;               ///< the card being dragged onto a knob
        juce::Point<int> dragPos;   ///< where the dragged card is, in the strip's coordinates
        /** @brief Editing an envelope's shape: which one, and which of its breakpoints is under the mouse. */
        int dragEnv = -1,      ///< the row being edited, or -1
            dragPoint = -1,    ///< the breakpoint being dragged, or -1
            hoverEnv = -1,     ///< the row under the mouse, or -1
            hoverPoint = -1;   ///< the breakpoint under the mouse, or -1

        /**
         * @brief Opens a tab: shows its rows and buttons, pulls the matrix for the table.
         * @param t  0 LFO, 1 envelopes, 2 matrix
         */
        void setTab(int t);
        /**
         * @brief Turns the envelope tab's page.
         * @param page  0 the six modulation envelopes, 1 the four sources' own
         */
        void setEnvPage(int page);
        /**
         * @brief on the page that is showing
         * @param env  the row, 0 .. kEnvRows - 1
         * @return true if the row belongs to the page that is open
         */
        bool envVisible(int env) const;
        /**
         * @brief a row's shape, of the six or a source's
         * @param env  the row, 0 .. kEnvRows - 1
         * @return the engine's envelope shape
         */
        const ambient::ModEnv& shapeOf(int env) const;
        /**
         * @brief a row's parameter: "env3_time", "src2_env_time"
         * @param env    the row, 0 .. kEnvRows - 1
         * @param field  "mode", "time", "depth" or "sync"
         * @return the parameter key
         */
        juce::String envKey(int env, const char* field) const;
        /**
         * @brief The lane's background and every card.
         * @param g  the graphics
         */
        void paintLane(juce::Graphics&);
        /**
         * @brief One card: its frame, its live curve and value dot, its label.
         * @param g    the graphics
         * @param c    the card
         * @param hot  under the mouse or in flight
         */
        void paintCard(juce::Graphics&, const Card&, bool hot);
        /**
         * @brief One LFO row's curve: the shape over a cycle, the live phase, the period.
         * @param g  the graphics
         * @param i  the LFO, 0 .. kNumLfos - 1
         */
        void paintLfo(juce::Graphics&, int i);
        /**
         * @brief One envelope row's curve: the breakpoints, the sustain, the loop, the live position, the length.
         * @param g  the graphics
         * @param i  the row, 0 .. kEnvRows - 1
         */
        void paintEnv(juce::Graphics&, int i);
        /**
         * @brief On a point: removes it; on the curve: puts one there (sixteen at most).
         * @param e  the click
         */
        void mouseDoubleClick(const juce::MouseEvent&) override;
        /**
         * @brief The envelope editor.
         *
         * The shapes were always there in the engine -- sixteen breakpoints,
         * a curve on every segment, a sustain point and a loop -- but nothing could reach them
         * except a preset, so in the panel they were pictures of something you could not touch.
         * One row's geometry, asked for by both the drawing and the mouse so they cannot drift.
         */
        struct EnvGeom {
            float x0 = 0,       ///< the curve's left edge
            w = 1,              ///< its width
            cy = 0,             ///< its centre line (the bottom for a unipolar shape)
            h = 1,              ///< its half height (the full height for a unipolar shape)
            len = 1,            ///< the shape's length in seconds
            depth = 1;          ///< the Depth that scales it, floored at 0.05 so a flat line stays editable
            bool uni = false;   ///< a source's own envelope: unipolar, drawn from the bottom
        };
        /**
         * @brief The geometry of one envelope row.
         * @param env  the row, 0 .. kEnvRows - 1
         * @return where its curve is drawn and how it is scaled
         */
        EnvGeom envGeom(int env) const;
        /**
         * @brief Which envelope row, and which breakpoint, a position is on.
         * @param pos       the position in the strip's coordinates
         * @param pointOut  if given, receives the breakpoint index under the mouse, or -1
         * @return -1 if not on a curve
         */
        int  envAt(juce::Point<int> pos, int* pointOut = nullptr) const;
        /**
         * @brief Where a breakpoint of an envelope is drawn.
         * @param env    the row
         * @param time   the point's time, seconds
         * @param value  the point's value, -1 .. 1 (0 .. 1 for a source's own)
         * @return the position in the strip's coordinates
         */
        juce::Point<float> envToXY(int env, float time, float value) const;
        /**
         * @brief The time and value a position stands for, the inverse of envToXY.
         * @param env    the row
         * @param pos    the position in the strip's coordinates
         * @param time   receives the time, seconds
         * @param value  receives the value
         */
        void envFromXY(int env, juce::Point<int> pos, float& time, float& value) const;
        /**
         * @brief A copy of a row's shape to edit.
         * @param env  the row
         * @return the shape
         */
        ambient::ModEnv envCopy(int env) const;
        /**
         * @brief Writes a shape into the engine, with a snapshot for undo.
         * @param env   the row
         * @param e     the shape
         * @param what  the undo name, empty for a step in a drag that was already recorded
         */
        void envCommit(int env, const ambient::ModEnv&, const juce::String& what);
        /**
         * @brief The right-click menu of an envelope: sustain and loop points, a segment's curvature, starting shapes.
         * @param env    the row
         * @param point  the breakpoint clicked, or -1
         */
        void envShapeMenu(int env, int point);
        /**
         * @brief An LFO's settings as the engine reads them, for drawing its shape.
         * @param i  the LFO, 0 .. kNumLfos - 1
         * @return the spec
         */
        ambient::LfoSpec specOf(int i) const;
        /**
         * @brief Where a drag ended: the parameter under the mouse, or none.
         * @param screenPos  the position in screen coordinates
         * @return the ParamId as an int, or -1
         */
        int paramUnder(juce::Point<int> screenPos) const;

        NoctuaryProcessor& proc;   ///< the processor
        NoctuaryEditor& owner;   ///< the editor
        juce::TextButton tabLfo{ "LFO" },         ///< the LFO tab
                         tabEnv{ "ENVELOPES" },   ///< the envelope tab
                         tabMatrix{ "MATRIX" };   ///< the matrix tab
        juce::TextButton pageMod{ "MODULATION 1-6" },   ///< the envelope tab's first page: the six
                         pageSrc{ "SOURCES 1-4" };      ///< its second: the sources' own
        /** @brief The matrix page: a table of routes (EditorMatrix.cpp), where a text box used to be. */
        std::unique_ptr<edt::RouteTable> table;
        juce::Label matrixInfo,   ///< "n of 32 routes"
                    hint;         ///< what a row is, and that a card can be dragged instead
        juce::Rectangle<int> lane,       ///< the band of cards along the top
                             tabsArea,   ///< the tab bar
                             content;    ///< the open tab's panel
        /** @brief Defined in EditorModStrip.cpp, where RouteTable is a complete type. */
        ~ModView() override;
        /** @brief Reads the engine's matrix into the table and the count. */
        void pullMatrix();
        /**
         * @brief Adds a route from a dragged source to a parameter, with a small default depth.
         * @param src     the source
         * @param target  the parameter
         * @return true if the engine accepted the matrix (false when it is full or the text was refused)
         */
        bool addRoute(ambient::ModSource src, ambient::ParamId target);
    };
    std::unique_ptr<ModView> mod_;   ///< the strip
    juce::Viewport modPort_;   ///< unused holder for the strip
    std::unique_ptr<juce::TextButton> modButton_;   ///< unused: the strip is always on the main page
public:
    /** @name The cells the modulation strip needs to find a drop target and to mark modulated knobs.
     *  @{ */
    /**
     * @brief Which parameter sits under a screen point.
     * @param screenPos  the position in screen coordinates
     * @return the ParamId as an int, or -1 if no parameter cell is showing there
     */
    int cellParamAt(juce::Point<int> screenPos) const;
    /**
     * @brief Where a cell's control is on the screen.
     * @param cellIndex  index into cells_
     * @return the bounds in screen coordinates, empty for a bad index or a cell without a component
     */
    juce::Rectangle<int> cellScreenBounds(int cellIndex) const;
    /** @} */
private:
    /**
     * @name Displays that live inside the grid, one per section row, filling the room the knobs leave.
     * The grid used to leave that room empty; the displays are what a Pigments-style layout puts
     * there, and each one is drawn from the numbers the engine is using, not from an illustration.
     * @{ */

    /**
     * @brief The two filters as a frequency response: the state-variable filter in the voice's colour,
     * the z-plane cascade in the accent, and what a note actually meets after both.
     */
    struct FilterView : juce::Component, juce::Timer {
        /**
         * @brief Catches no clicks and polls the parameters at 15 Hz.
         * @param p  the processor
         */
        explicit FilterView(NoctuaryProcessor& p) : proc(p) { setInterceptsMouseClicks(false, false); startTimerHz(15); }
        /**
         * @brief The axes, the two branches and their product, and the legend (EditorViews.cpp).
         * @param g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief Nothing but parameters is drawn here, so nothing but a parameter can change it. */
        void timerCallback() override
        { const uint32_t g = proc.paramGeneration(); if (isShowing() && g != seen) { seen = g; repaint(); } }
        NoctuaryProcessor& proc;   ///< the processor
        uint32_t seen = 0xffffffffu;   ///< the parameter generation last drawn
    };
    /**
     * @brief One source slot: the wavetable frame, the FM cycle, the clip with its grain window, or the
     * colour of the noise -- whichever the slot is set to.
     *
     * A table (Harmonic or Wavetable) is drawn
     * whole and in depth -- every frame a line, the first at the front, the frame at Position lit --
     * and a click turns it into the flat picture of that one frame with its neighbours, and back.
     */
    struct SourceView : juce::Component, juce::Timer {
        /**
         * @brief Takes clicks (the table flips, the entrance opens) and polls at 15 Hz.
         * @param p  the processor
         * @param s  the slot, 1 .. 4
         */
        SourceView(NoctuaryProcessor& p, int s) : proc(p), slot(s) { setInterceptsMouseClicks(true, false); startTimerHz(15); }
        /**
         * @brief The source's picture, then the entrance in the corner.
         * @param g  the graphics
         */
        void paint(juce::Graphics&) override;
        /**
         * @brief The picture of whatever the slot is set to: table, FM, clip, noise, partials, string, model.
         * @param g  the graphics
         */
        void paintSource(juce::Graphics&);
        /**
         * @brief A slot that enters on a shape of its own carries a small picture of it in the corner, with
         * the loudest voice's place in it; a click on the picture opens the shape in the editor.
         * @return true if the slot is on and its Env is set to Own
         */
        bool ownEntrance() const;
        /**
         * @brief Where the entrance picture sits.
         * @return the box in the top right corner
         */
        juce::Rectangle<int> entranceBox() const;
        /**
         * @brief The own envelope's shape with the loudest voice's gain on it, and "ENV".
         * @param g  the graphics
         */
        void paintEntrance(juce::Graphics&);
        /**
         * @brief On the entrance: opens the source's envelope in the strip. On a table: flips between depth and flat.
         * @param e  the press
         */
        void mouseDown(const juce::MouseEvent&) override;
        /**
         * @brief A wavetable frame, an FM cycle and a noise colour are pictures of parameters and change
         * only when one moves -- or, for a table, when a modulation moves its Position, which is
         * watched here.
         *
         * A clip's grains, the additive bank's partials, a bowed string and a
         * spectral model are what the engine is doing this instant, and are drawn every tick.
         */
        void timerCallback() override
        {
            if (!isShowing()) return;
            const int type = static_cast<int>(std::lround(proc.engine().getParam(ambient::slotParamIds(slot - 1)[0])));
            const bool alive = type == 3 || type == 5 || type == 6 || type == 7 || type == 8 || type >= 10;   // ...and the near sources
            const bool moved = (type == 1 || type == 9) && std::fabs(livePosition() - shownPos) > 0.002f;
            const uint32_t g = proc.paramGeneration();
            if (alive || moved || ownEntrance() || g != seen) { seen = g; repaint(); }
        }
        /**
         * @brief Position as it is playing: the knob, the morph or map blend, what the matrix adds -- and
         * the slot's own Pos Drift, which is the one that actually moves.
         *
         * The drift lives inside
         * the voice and never left it, so for a preset whose only movement was Pos Drift the
         * picture stood still while the sound wandered a third of the table. The engine answers
         * with -1 when nothing sounds; then the knob is all there is to draw.
         * @return the position, 0 .. 1
         */
        float livePosition() const
        {
            const float live = proc.engine().displaySlotPosition(slot - 1);
            if (live >= 0.0f) return juce::jlimit(0.0f, 1.0f, live);
            const ambient::ParamId id = ambient::slotParamIds(slot - 1)[6];
            return juce::jlimit(0.0f, 1.0f, proc.engine().effectiveParam(id) + proc.engine().modAmount(id));
        }
        /**
         * @brief The table in depth; false when there is nothing to stand in depth (no table, one frame).
         * @param g      the graphics
         * @param plot   the area to draw in
         * @param type   the slot's type (Harmonic or Wavetable)
         * @param table  which table
         * @param pos    the Position, 0 .. 1, whose frame is lit
         * @return true if the table was drawn
         */
        bool paintTable3D(juce::Graphics& g, juce::Rectangle<float> plot, int type, int table, float pos);
        NoctuaryProcessor& proc;   ///< the processor
        uint32_t seen = 0xffffffffu;   ///< the parameter generation last drawn
        int slot;   ///< 1 .. 4
        float amp[ambient::kTablePartials] = {};   ///< smoothed live amplitudes for the additive picture
        bool  flat = false;       ///< the table as one frame (after a click) rather than in depth
        float shownPos = -1.0f;   ///< the Position last drawn
        /**
         * @brief The table sampled for drawing, kTablePoints + 1 points a frame, and its lines drawn once
         * into an image at the display's size: both rebuilt only when the table or the size changes.
         */
        static constexpr int kTablePoints = 96;
        std::vector<float> tableCycles;   ///< the sampled frames, kTablePoints + 1 floats each
        int    tableFrames = 0;   ///< how many frames are sampled
        double tableSig = -1.0;   ///< a signature of the table the samples came from
        juce::Image tableMesh;   ///< the lines, drawn once
        juce::Rectangle<int> meshBounds;   ///< the plot the mesh was drawn for
        float  meshScale = 0.0f;   ///< the scale the mesh was drawn at
    };
    /**
     * @brief The conductor's notes as they happen: a piano roll scrolling left, one column per tick, so
     * the cluster brain's choices can be watched rather than inferred from the keyboard strip.
     */
    struct BrainView : juce::Component, juce::Timer {
        /**
         * @brief Catches no clicks and ticks at 15 Hz.
         * @param p  the processor
         */
        explicit BrainView(NoctuaryProcessor& p) : proc(p) { setInterceptsMouseClicks(false, false); startTimerHz(15); }
        /**
         * @brief The roll: octave lines, a bar per held note fading with age, the range labels, the cascade's aura.
         * @param g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief Writes the sounding notes into the newest column, widens the range, advances the head. */
        void timerCallback() override;
        NoctuaryProcessor& proc;   ///< the processor
        static constexpr int kCols = 240;                 ///< 16 s at 15 Hz
        std::vector<std::array<bool, 128>> hist = std::vector<std::array<bool, 128>>(kCols);   ///< the ring of columns: which notes sounded at each tick
        int head = 0;   ///< the column written next
        int lo = 36,   ///< the bottom of the note range in view
            hi = 84;   ///< the note range in view, widened as notes arrive
    };
    /**
     * @brief Three rolls: the conductor's, the second conductor's, and one on the Autoplay tab, where
     * watching the chord travel one voice at a time is the whole point of the mode.
     */
    std::unique_ptr<BrainView> brainView_,    ///< the Cluster Brain tab's roll
                               brainView2_,   ///< Brain 2's roll
                               brainView3_;   ///< the Autoplay tab's roll
    /**
     * @brief The stereo stage: every sounding voice as a dot, left-right by its pan, near-far by its
     * plane, size by its envelope -- the spatial model (concept.md) as a picture, moving.
     */
    struct StageView : juce::Component, juce::Timer {
        /**
         * @brief Ticks at 15 Hz; takes the mouse for the planes.
         * @param p  the processor
         */
        explicit StageView(NoctuaryProcessor& p) : proc(p) { startTimerHz(15); }
        /**
         * @brief The room, the three planes as lines, every voice as a smoothed dot, the count.
         * @param g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief Repaints while on screen. */
        void timerCallback() override { if (isShowing()) repaint(); }
        /**
         * @brief The planes are the things on the stage a hand can move: the conductor's, the keys' and
         * the second conductor's depth, each a line across the room.
         *
         * Drag a line and its knob
         * follows (through the host, so it is automated and undone like any knob). The voices
         * themselves are where the conductor put them and stay a picture.
         * @param e  the move; the plane under it is highlighted and the cursor changes
         */
        void mouseMove(const juce::MouseEvent&) override;
        /**
         * @brief Grabs the plane under the mouse: a snapshot for undo, then the host's change gesture begins.
         * @param e  the press
         */
        void mouseDown(const juce::MouseEvent&) override;
        /**
         * @brief Moves the grabbed plane's depth parameter to where the mouse is.
         * @param e  the drag
         */
        void mouseDrag(const juce::MouseEvent&) override;
        /** @brief Ends the host's change gesture and lets the plane go. */
        void mouseUp(const juce::MouseEvent&) override;
        /** @brief No plane is hovered once the mouse has left. */
        void mouseExit(const juce::MouseEvent&) override { hoverLine = -1; repaint(); }
        /**
         * @brief The room the dots and planes are drawn in.
         * @return the plot rectangle inside the frame
         */
        juce::Rectangle<float> plotRect() const;
        /**
         * @brief Which plane's line a position is near.
         * @param pos  the position in the view's coordinates
         * @return 0 conductor, 1 keys, 2 conductor 2, or -1 if none is within reach
         */
        int  lineAt(juce::Point<int>) const;
        std::function<void(const juce::String&)> onUndo;   ///< called with a name before a plane is moved, for the editor's undo
        int hoverLine = -1,   ///< the plane under the mouse, -1 for none
            dragLine = -1;    ///< the plane being dragged, -1 for none
        NoctuaryProcessor& proc;   ///< the processor
        /** @brief One voice's dot. */
        struct Dot {
            float x = 0.0f,    ///< its smoothed x in the plot
            y = 0.0f,          ///< its smoothed y
            r = 0.0f;          ///< its smoothed radius
            int note = -1;     ///< the note it stands for, -1 once it is gone
            bool on = false;   ///< whether the voice sounded at the last paint
        };
        Dot dots[ambient::Engine::kMaxVoices];   ///< smoothed positions, one per voice slot seen
    };
    std::unique_ptr<StageView> stageView_;   ///< the Space / Foundation row's display
    /**
     * @brief The Tuning page's display: the timbre's own roughness curve across the octave (Sethares),
     * the chosen scale's degrees on it, the key the conductor has found, the comma and the tide.
     *
     * What the tuning section computes, drawn, instead of only its numbers.
     */
    struct TuningView : juce::Component, juce::Timer {
        /**
         * @brief Catches no clicks and repaints at 5 Hz: the curve is slow to compute and slow to change.
         * @param p  the processor
         */
        explicit TuningView(NoctuaryProcessor& p) : proc(p) { setInterceptsMouseClicks(false, false); startTimerHz(5); }
        /**
         * @brief The roughness curve, the scale's degrees on it, the key, the comma, the tide and the scale's name.
         * @param g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief Repaints while on screen. */
        void timerCallback() override { if (isShowing()) repaint(); }
        NoctuaryProcessor& proc;   ///< the processor
        static constexpr int kPoints = 240;   ///< five cents apart
        float curve[kPoints] = {};   ///< the roughness across the octave, recomputed in paint
    };
    std::unique_ptr<TuningView> tuningView_;   ///< the Tuning tab's display
    /**
     * @brief The Coherence page's display: the four Kuramoto phases on a ring, the Lenia field as a
     * grey grid, the six attractor readings as bars -- the living modulators, seen.
     */
    struct CoherenceView : juce::Component, juce::Timer {
        /**
         * @brief Catches no clicks and ticks at 10 Hz.
         * @param p  the processor
         */
        explicit CoherenceView(NoctuaryProcessor& p) : proc(p) { setInterceptsMouseClicks(false, false); startTimerHz(10); }
        /**
         * @brief The ring of phases, the Lenia grid, the attractor bars and the two orbits.
         * @param g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief Appends the attractors' current x/y to the trails (forty seconds kept) and repaints. */
        void timerCallback() override;
        NoctuaryProcessor& proc;   ///< the processor
        /** @brief Where the two attractors have been: the last forty seconds of their x/y, drawn as orbits. */
        std::vector<juce::Point<float>> lorenzTrail, rosslerTrail;   ///< the Roessler attractor's trail
    };
    std::unique_ptr<CoherenceView> coherenceView_;   ///< the Coherence + Clock tab's display
    /**
     * @brief The Cosmos return's spectrum: what the shifter, the resonator, the vowel and the nebula are
     * handing back, on a log-frequency axis, smoothed the way a meter falls.
     */
    struct CosmosView : juce::Component, juce::Timer {
        /**
         * @brief Sizes the FFT buffers, builds the Hann window, starts at 15 Hz.
         * @param p  the processor
         */
        explicit CosmosView(NoctuaryProcessor& p);
        /**
         * @brief The bins as a filled curve on the log axis, and a legend of which stages are on.
         * @param g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief Reads the return tap, transforms it, folds the bins into kBins log-spaced bands with a falling hold. */
        void timerCallback() override;
        NoctuaryProcessor& proc;   ///< the processor
        static constexpr int kN = 2048,     ///< FFT length
                             kBins = 160;   ///< the log-spaced bands drawn
        std::vector<float> re,       ///< the FFT's real part
                           im,       ///< its imaginary part
                           window;   ///< the Hann window
        std::unique_ptr<ambient::Fft> fft;   ///< the transform
        float bins[kBins] = {};   ///< dB per log-spaced bin, smoothed
        bool  silent = true;   ///< nothing came back on the last tick
    };
    std::unique_ptr<CosmosView> cosmosView_;   ///< the Cosmos tab's display
    /**
     * @brief What the header used to carry was a small spectrum, at 1024 points and 96 bands.
     *
     * The strip
     * along the bottom of the page now does that properly, so this space says the thing a piece
     * of ambient actually has to be judged by: its loudness and, more to the point, how much of
     * its dynamic range is still there. A drone mastered to -9 LUFS has had the movement squeezed
     * out of its reverb tails and cannot get it back.
     */
    struct LoudnessView : juce::Component, juce::Timer {
        /**
         * @brief Repaints at 10 Hz; takes the click that resets the measurement.
         * @param p  the processor
         */
        explicit LoudnessView(NoctuaryProcessor& p) : proc(p) { startTimerHz(10); }
        /**
         * @brief Momentary and integrated loudness, the range, the peak, as bars and figures.
         * @param g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief Repaints while on screen. */
        void timerCallback() override { if (isShowing()) repaint(); }
        /** @brief click to start measuring again */
        void mouseDown(const juce::MouseEvent&) override;
        NoctuaryProcessor& proc;   ///< the processor
    };
    std::unique_ptr<LoudnessView> outputView_;   ///< in the header, right half
    /**
     * @brief The Vector's square, with the point in it: three corners are the source slots, the fourth
     * is all three together.
     *
     * Drag the point; it is the one control here whose value is a place.
     */
    struct VectorView : juce::Component, juce::Timer {
        /**
         * @brief Ticks at 15 Hz; takes the mouse for the point.
         * @param p  the processor
         */
        explicit VectorView(NoctuaryProcessor& p) : proc(p) { startTimerHz(15); }
        /**
         * @brief The square with its corners named, the wander's trail, the point, and a hint.
         * @param g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief Repaints while on screen. */
        void timerCallback() override { if (isShowing()) repaint(); }
        /**
         * @brief A press moves the point.
         * @param e  the press
         */
        void mouseDown(const juce::MouseEvent& e) override { drag(e); }
        /**
         * @brief A drag moves the point.
         * @param e  the drag
         */
        void mouseDrag(const juce::MouseEvent& e) override { drag(e); }
        /**
         * @brief Writes the mouse position, clamped to the square, into VecX and VecY through the host.
         * @param e  the press or drag
         */
        void drag(const juce::MouseEvent&);
        /**
         * @brief The square the point moves in.
         * @return the largest square that fits under the title
         */
        juce::Rectangle<float> square() const;
        NoctuaryProcessor& proc;   ///< the processor
        static constexpr int kTrail = 240;      ///< where the wander has been, at 20 Hz: twelve seconds
        float tx[kTrail] = {},   ///< the trail's x positions, a ring
              ty[kTrail] = {};   ///< the trail's y positions
        int   head = 0,     ///< the ring's write position
              filled = 0;   ///< how much of the ring is filled
    };
    std::unique_ptr<VectorView> vectorView_;   ///< the MORPH row's display
    /**
     * @brief The wide strip along the bottom of the left column: the same signal as the header's little
     * meter, but with room to say something.
     *
     * A 16384-sample window is 2.9 Hz wide at 48 kHz, so
     * the partials of a low drone are separate lines rather than a hump, and the filter's own
     * response is drawn over them -- what the filter is doing to what is actually there.
     */
    struct SpectrumView : juce::Component, juce::Timer {
        /**
         * @brief Sizes the FFT buffers, builds the Hann window, starts at 12 Hz (EditorSpectrum.cpp).
         * @param p  the processor
         */
        explicit SpectrumView(NoctuaryProcessor& p);
        /**
         * @brief The axes, the hold trace, the live bands, the filter's response over them, the note ticks and the frequency under the pointer.
         * @param g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief Reads the output tap, transforms it, folds it into the bands and lets the hold fall. */
        void timerCallback() override;
        /**
         * @brief Remembers the pointer's x, so paint can name the frequency under it.
         * @param e  the move
         */
        void mouseMove(const juce::MouseEvent&) override;
        /** @brief Forgets the pointer. */
        void mouseExit(const juce::MouseEvent&) override;
        NoctuaryProcessor& proc;   ///< the processor
        static constexpr int kN = ambient::kOutTapLen;   ///< FFT length: the whole tap
        static constexpr int kBands = 480;               ///< one band per two or three pixels
        static constexpr float kLoHz = 20.0f,      ///< the left end of the frequency axis
                               kHiHz = 16000.0f;   ///< its right end
        /**
         * @brief The axis runs a little above zero so the filter, drawn on the same decibels, has
         * somewhere to put a resonance: a flat response is the 0 dB line, not the ceiling.
         */
        static constexpr float kFloorDb = -84.0f, kTopDb = 6.0f;   ///< the top of the decibel axis
        static constexpr int kLabelGutter = 26;   ///< room at the left for the decibel numbers
        std::vector<float> re,       ///< the FFT's real part
                           im,       ///< its imaginary part
                           window;   ///< the Hann window
        std::unique_ptr<ambient::Fft> fft;   ///< the transform
        std::vector<float> band,   ///< the live bands, dB
                           hold;   ///< live bands, and a slowly falling trace
        float peakDb = -96.0f;   ///< the loudest band of the last tick, dB
        int   hoverX = -1;                               ///< read the frequency under the pointer
    };
    std::unique_ptr<SpectrumView> spectrumView_;   ///< the ANALYSIS group's display
    /** @brief The amplitude envelope as a curve, with the loudest voice's level on it. */
    struct EnvView : juce::Component, juce::Timer {
        /**
         * @brief Catches no clicks and repaints at 15 Hz.
         * @param p  the processor
         */
        explicit EnvView(NoctuaryProcessor& p) : proc(p) { setInterceptsMouseClicks(false, false); startTimerHz(15); }
        /**
         * @brief The ADSR as a curve with its four times labelled, and the loudest voice's level on it.
         * @param g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief Repaints while on screen. */
        void timerCallback() override { if (isShowing()) repaint(); }
        NoctuaryProcessor& proc;   ///< the processor
    };
    std::unique_ptr<EnvView> envView_;   ///< the ENVELOPE + EXPRESSION tab's display
    std::unique_ptr<FilterView> filterView_;   ///< the display of both filter tabs
    std::unique_ptr<SourceView> source1View_,   ///< Source 1's display
                                source2View_,   ///< Source 2's display
                                source3View_,   ///< Source 3's display
                                source4View_;   ///< Source 4's display
    /** @} */
    std::unique_ptr<juce::TextButton> browseButton_;   ///< Browse, in the header
    /**
     * @brief Shows one page and hides the others; the header buttons follow.
     * @param page  0 edit, 1 perform, 2 browse, 3 help
     */
    void setPage(int page);
    /** @brief Opens the gesture / macro mapping table as text in a dialog; the timer applies it when the dialog closes. */
    void showMappingEditor();
    juce::Component::SafePointer<juce::TextEditor> mapEditor_;   ///< the mapping editor while its dialog lives
    juce::String mapText_;   ///< its text as the timer last saw it
    bool mapOpen_ = false;   ///< the dialog is up: mirror the text, apply it when it is gone
    std::unique_ptr<juce::ComboBox> soundBox_;   ///< the sound presets, in the header, one submenu per family
    juce::ComboBox* cosmosBox_ = nullptr;   ///< not owned: its cell in the Cosmos section owns it
    juce::ComboBox* morphABox_ = nullptr;   ///< owned by their cells
    juce::ComboBox* morphBBox_ = nullptr;   ///< the preset chooser of morph slot B
    std::unique_ptr<juce::FileChooser> chooser_;   ///< the file dialog in flight (record, save, load, tables, clips, impulses, scales)
    juce::Rectangle<int> header_,     ///< the header, in design coordinates
                         helpLine_,   ///< the help line's card
                         keys_;       ///< the keyboard strip
    bool sounding_[128] = {};   ///< which notes sound, read from the engine every tick for the keyboard strip

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoctuaryEditor)
};
