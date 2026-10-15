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
 * @author Rene Weller
 */
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Frame.h"
#include "JourneyTimeline.h"
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
 * The one component the host opens. It owns the processor's view of every parameter (m_cells),
 * the layout that arranges them (m_sections, m_groups, m_tabRows, m_expandedGroups), the displays in
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
     * @param[in,out] p  the processor this editor shows
     */
    explicit NoctuaryEditor(NoctuaryProcessor&);
    /** @brief Detaches the look and feel before the members go. */
    ~NoctuaryEditor() override;

    /**
     * @brief Paints the header in design space: the background lift, the title, the help line
     *        (the control under the mouse, its value and what it does), the keyboard strip with
     *        the sounding notes and the brain's root, the status line, the cascade's aura, the
     *        REC time and the Master section's frame. Everything else is a child component.
     * @param[in,out] g  the graphics, in window coordinates until the scale is applied
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
     * @param[in] e  the press; only a popup-menu press on a parameter cell does anything
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
     * @param[in] wavetable  true for a table into the user slot, false for a clip
     * @param[in] slot       slot < 0: the clip goes into every slot
     */
    void chooseSourceFile(bool wavetable, int slot = -1);
    /**
     * @brief Opens a file chooser for an impulse response for the Room.
     * @param[in] second  true for Impulse B (Room Morph fades to it), false for Impulse A
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
     * @brief The index into m_cells of the cell that holds a parameter.
     * @param[in] id  the parameter
     * @return the cell index, or -1 if the parameter has no cell (the master gain, in the header)
     */
    int  cellForParam(ambient::ParamIdE id) const;
    int  m_tableCell = -1,      ///< the Wavetable... button's cell (Source 2); -1 while not built
         m_impulseCell = -1,    ///< the Impulse A... button's cell
         m_impulseBCell = -1,   ///< the Impulse B... button's cell
         m_nearClipCell = -1;   ///< the near source's Clip... button's cell; the labels of all four name the loaded file
    bool m_autoSeen = true;   ///< the near layer's Auto as the timer last saw it (a rising edge draws anew)
    /** @name Journeys: the box of files, the status label, and the files behind the box's ids.
     *  @{ */
    juce::ComboBox* m_journeyBox = nullptr;   ///< the chooser in the Morph section (owned by its cell); item id = index into m_journeyFiles + 1
    juce::Label*    m_journeyStatus = nullptr;   ///< the step that is playing, or how many steps have been written
    juce::Array<juce::File> m_journeyFiles;   ///< the .journey files the box offers, in item order
    /** @brief Rescans the journey folders and refills the box. */
    void fillJourneyBox();
    /** @brief Asks for a name and writes the journey being written into Documents/Noctuary/Journeys. */
    void saveJourneyAs();
    /** @} */
    int  m_textureCell[ambient::Slots] = { -1, -1, -1, -1 };   ///< a Texture... button in every source section
    /**
     * @brief Builds a cell for every parameter, its section on first sight, and the extra cells
     *        (file loaders, section preset banks, morph slots, the journey row).
     */
    void buildCells();
    /** @brief Gives every knob and switch the colour of its section's group. */
    void colourCellsByGroup();
    /**
     * @brief Adds a cell that is not a parameter (a button, a chooser, a label) to a section.
     * @param[in] section  the section's name
     * @param[in] comp     the component, taken over by the cell
     * @param[in] label    the caption under it
     * @param[in] units    its width in cells
     * @return the new cell's index into m_cells, or -1 if there is no such section
     */
    int  addExtraCell(const juce::String& section, std::unique_ptr<juce::Component> comp, const juce::String& label, int units);
    /** @name Help: the line in the header follows the mouse; the page is the manual by topic.
     *  @{ */
    /**
     * @brief A registered control under the mouse: its parameter goes into the help line.
     * @param[in] e  the event; its component is looked up in m_helpParamOf
     */
    void mouseEnter(const juce::MouseEvent&) override;
    /**
     * @brief The mouse left the control the help line was showing: back to the general hint.
     * @param[in] e  the event; its component is looked up in m_helpParamOf
     */
    void mouseExit(const juce::MouseEvent&) override;
    /**
     * @brief F1 opens and closes the help, Esc closes it, Ctrl+Z undoes, Ctrl+Y redoes.
     * @param[in] k  the key
     * @return true if the key was one of these
     */
    bool keyPressed(const juce::KeyPress&) override;
    int  m_hoveredParam = -1;                              ///< what the mouse is over, for the help line
    std::map<juce::Component*, int> m_helpParamOf;         ///< every control that has a parameter behind it
public:
    /**
     * @brief the modulation strip registers its own
     * @param[in,out] c   the control; it also gets this editor as a mouse listener
     * @param[in]     id  the parameter behind it
     */
    void registerHelp(juce::Component*, ambient::ParamIdE);
    /** @} */
private:
    /**
     * @brief Undo, redo and an A/B compare, all in terms of whole parameter snapshots: the instrument
     * has no other state that a knob can destroy, and a snapshot is 320 floats.
     */
    struct Snapshot {
        std::vector<float> m_v;   ///< every parameter's value, in paramTable() order
        juce::String m_what;      ///< what was about to change when it was taken (the undo button's tooltip)
    };
    std::vector<Snapshot> m_undo,   ///< the undo stack, at most 32 deep
                          m_redo;   ///< what undo took back, cleared by the next change
    Snapshot m_slotA,   ///< state A of the A/B compare; empty until the first swap
             m_slotB;   ///< state B
    bool     m_showingB = false;   ///< which of the two the panel shows
    /**
     * @brief The parameters as they stand now.
     * @param[in] what  the name the snapshot carries
     * @return the snapshot
     */
    Snapshot takeSnapshot(const juce::String& what) const;
    /**
     * @brief Writes a snapshot back through the host, parameter by parameter.
     * @param[in] s  the snapshot; ignored if it is not NumParams long
     */
    void     restore(const Snapshot&);
    /**
     * @brief call before changing many parameters at once
     * @param[in] what  what is about to change ("preset", "randomise Cloud", a knob's name)
     * @pre called before changing
     */
    void     pushUndo(const juce::String& what);
    /** @brief Back one snapshot, the current state onto the redo stack. */
    void     doUndo();
    /** @brief Forward one snapshot, the current state onto the undo stack. */
    void     doRedo();
    /** @brief The A/B button: parks the state, then swaps the two. */
    void     swapAB();
    std::unique_ptr<juce::TextButton> m_undoButton,      ///< Undo, in the header
                                      m_redoButton,      ///< Redo
                                      m_abButton,        ///< A | B: its text says which is showing
                                      m_compactButton,   ///< cycles the layout: Normal, Compact, Expanded
                                      m_recallButton;    ///< session recall on or off (standalone only; null in a host)
    /**
     * @brief Undo covered presets, dice and envelopes but not the thing done most: a knob turned.
     *
     * One
     * listener on every parameter control takes a snapshot on the press or the wheel that starts a
     * change, named after the parameter, and throttles the wheel so a scroll is one step back, not
     * forty.
     */
    struct UndoHook : juce::MouseListener {
        NoctuaryEditor* m_editor = nullptr;   ///< where the snapshots go
        std::map<juce::Component*, juce::String> m_names;   ///< the name a control's snapshot carries
        std::map<juce::Component*, double> m_lastWheel;   ///< when each control was last wheeled, ms
        /**
         * @brief A press on a registered control takes a snapshot before the value moves.
         * @param[in] e  the press; its component is looked up in names
         */
        void mouseDown(const juce::MouseEvent& e) override
        {
            auto it = m_names.find(e.eventComponent);
            if (it != m_names.end() && m_editor != nullptr) m_editor->pushUndo(it->second);
        }
        /**
         * @brief A wheel step takes a snapshot only if the last one on this control is a second old.
         * @param[in] e  the wheel event; its component is looked up in names
         */
        void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails&) override
        {
            auto it = m_names.find(e.eventComponent);
            if (it == m_names.end() || m_editor == nullptr) return;
            const double now = juce::Time::getMillisecondCounterHiRes();
            double& last = m_lastWheel[e.eventComponent];
            if (now - last > 1000.0) m_editor->pushUndo(it->second);
            last = now;
        }
    };
    UndoHook m_undoHook;   ///< the one listener, added to every parameter control
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
         * @brief The ring and name on the target knob, the curved lead from the card, the card under the cursor
         *        (EditorDrag.cpp).
         * @param[in,out] g  the graphics
         */
        void paint(juce::Graphics&) override;
        bool m_active = false;   ///< a drag is in flight; nothing is drawn otherwise
        juce::Point<float> m_from,   ///< where the card came from, in the overlay's coordinates
                           m_to;     ///< where the cursor is
        juce::String m_name,         ///< the modulator's label
                     m_targetName;   ///< the parameter under the cursor (empty over nothing)
        juce::Colour m_colour { 0xffffffff };   ///< the modulator's colour
        juce::Rectangle<float> m_target;   ///< the knob it would land on; empty over nothing
    };
    DragOverlay m_dragOverlay;   ///< covers the whole editor, above everything
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
         * @param[in,out] o  the editor, which rewrites the route
         * @param[in]     s  the route's source
         * @param[in]     t  the route's target
         * @param[in]     c  the source's colour
         */
        DepthPopup(NoctuaryEditor& o, ambient::ModSourceE s, ambient::ParamIdE t, juce::Colour c);
        /** @brief Removes the global mouse listener again. */
        ~DepthPopup() override;
        /**
         * @brief The card: source -> target in the source's colour over the slider.
         * @param[in,out] g  the graphics
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
         * @param[in] e  the press, from anywhere on the desktop
         */
        void mouseDown(const juce::MouseEvent& e) override
        {
            if (e.eventComponent == this || isParentOf(e.eventComponent)) return;
            juce::MessageManager::callAsync([w = juce::Component::SafePointer<DepthPopup>(this)] {
                if (w != nullptr) w->m_owner.hideDepthPopup();
            });
        }
        NoctuaryEditor& m_owner;   ///< the editor that made it
        ambient::ModSourceE m_source;   ///< the route's source
        ambient::ParamIdE m_target;   ///< the route's target
        juce::Colour m_colour;   ///< the source's colour
        juce::Slider m_depth;   ///< -1 .. 1, written straight into the route on every change
        juce::TextButton m_remove{ "x" };   ///< takes the route away and closes the popup
    };
    std::unique_ptr<DepthPopup> m_depthPopup;   ///< the one popup, or none
public:
    /**
     * @brief Draws (or moves) the drag in flight: the lead from the card to the cursor, and the
     *        ring on the knob under it (EditorDrag.cpp).
     * @param[in] screenFrom  where the card is, in screen coordinates
     * @param[in] screenTo    where the cursor is, in screen coordinates
     * @param[in] name        the modulator's label on the card
     * @param[in] c           the modulator's colour
     */
    void showModDrag(juce::Point<int> screenFrom, juce::Point<int> screenTo, const juce::String& name, juce::Colour);
    /** @brief The drag ended: the overlay goes blank. */
    void hideModDrag();
    /**
     * @brief Shows the depth popup for a route beside a screen point, inside the panel.
     * @param[in] src       the route's source
     * @param[in] target    the route's target
     * @param[in] c         the source's colour
     * @param[in] screenAt  where the drop happened, in screen coordinates
     */
    void showDepthPopup(ambient::ModSourceE, ambient::ParamIdE, juce::Colour, juce::Point<int> screenAt);
    /** @brief Deletes the depth popup, if one is up. */
    void hideDepthPopup();
    /**
     * @brief Rewrites one route's depth (the matrix travels as text, as everywhere else here).
     * @param[in] src     the route's source
     * @param[in] target  the route's target
     * @param[in] depth   the new depth, -1 .. 1
     * @return true if the engine accepted the rewritten matrix
     */
    bool setRouteDepth(ambient::ModSourceE, ambient::ParamIdE, float depth);
    /**
     * @brief Drops one route from the matrix, the rest written back as they stand.
     * @param[in] src     the route's source
     * @param[in] target  the route's target
     * @return true if the engine accepted the rewritten matrix
     */
    bool removeRoute(ambient::ModSourceE, ambient::ParamIdE);
    /**
     * @brief The depth of a route as the matrix has it.
     * @param[in] src     the route's source
     * @param[in] target  the route's target
     * @return the depth, or 0 if there is no such route
     */
    float routeDepth(ambient::ModSourceE, ambient::ParamIdE) const;
private:
    /**
     * @name The two section layers that live inside their own sections rather than in the header:
     * 156 filters and 41 plucks are lists you go looking for, not things you keep on the toolbar.
     * Not owned: the cell owns the component, the editor only needs to read the selection back.
     * @{ */
    juce::ComboBox* m_zPresetBox = nullptr;   ///< the Z-Plane section's filter presets, by family
    juce::ComboBox* m_strikePresetBox = nullptr;   ///< the Strike section's pluck presets, by family
    juce::ComboBox* m_nearPresetBox = nullptr;   ///< the Near Events section's foreground presets, by family
    /** @} */
    /**
     * @brief Compact: the same page in columns CompactFactor as wide, every page refitted into them --
     * narrower and taller, nothing left out. Kept in the plugin state.
     */
    bool m_compact = false;
    static constexpr float CompactFactor = 0.88f;   ///< the column width of Compact, as a fraction of Normal's
    /**
     * @brief Expanded: every page of every tab row placed under one another.
     *
     * No tabs to click, a
     * tall page, and the four sources, the two filters and the conductor's six tables all in
     * sight at once -- for a tall screen, or for reading a preset through.
     */
    bool m_expanded = false;
    /** @brief The manual's pictures: every section open whatever its switch says, and the tabs. */
    bool m_openAll = false;
    /**
     * @brief updates unused/collapsed from the parameters; true if anything changed
     * @return true if a section's collapsed state changed, so the page has to be laid out again
     */
    bool refreshLayoutState();
    /** @brief The die in a section's title: one click randomises that section, shift-click nudges it. */
    std::map<juce::String, juce::Rectangle<int>> m_diceOf;
    /**
     * @brief Draws every parameter of a section again (the die), after a snapshot for undo.
     * @param[in] name    the section
     * @param[in] subtle  true (shift held) keeps every value near where it already is
     */
    void     randomiseSection(const juce::String& name, bool subtle);
    /** @brief Lays the body out again and resizes the window to the new design shape. */
    void     rebuildLayout();
    /**
     * @brief Switches the layout mode, stores it in the processor and rebuilds.
     * @param[in] mode  0 normal, 1 compact, 2 expanded
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
         * @param[in,out] p  the processor the help reads
         * @param[in,out] o  the editor whose sections it photographs
         */
        HelpView(NoctuaryProcessor&, NoctuaryEditor&);
        /**
         * @brief The frame, the topic's pictures in their column, the title and the hint line.
         * @param[in,out] g  the graphics
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
         * @param[in]     row       the topic
         * @param[in,out] g         the graphics
         * @param[in]     w         row width
         * @param[in]     h         row height
         * @param[in]     selected  whether the row is the chosen one
         */
        void paintListBoxItem(int row, juce::Graphics&, int w, int h, bool selected) override;
        /**
         * @brief Puts the topic's text into the editor and shows its pictures.
         * @param[in] row  the topic chosen, -1 for none
         */
        void selectedRowsChanged(int row) override;
        /**
         * @brief Takes the topic's pictures: its sections, its live display, and -- for the manual -- every
         *        tab that belongs to the chapter, from the tab rows themselves.
         * @param[in] row  the topic
         */
        void showTopic(int row);
        NoctuaryProcessor& m_proc;   ///< the processor the help reads
        NoctuaryEditor& m_owner;   ///< the editor whose panel it photographs
        juce::ListBox m_topics;   ///< the topic list
        juce::TextEditor m_text;   ///< the chosen topic's text, read-only
        juce::String m_parameters;   ///< the generated last topic: every parameter with its help
        /**
         * @brief The pictures: snapshots of the topic's sections, fresh from the panel with its current
         * values, and a live display of the unit -- the same component the page uses, a second copy.
         */
        std::vector<juce::Image> m_pics;
        /**
         * @brief Pictures of whole tabs.
         *
         * The help page does not draw these -- its picture column
         * holds two or three -- but the manual prints every one of them, with its tab's
         * own name as the caption.
         */
        std::vector<juce::Image> m_tabPics;
        juce::StringArray        m_tabNames,      ///< per tab picture: the tab's own name
                                 m_tabCaptions,   ///< per tab picture: its caption
                                 m_picCaptions;   ///< per section picture: its caption
        juce::String             m_liveCaption;   ///< the caption of the live display
        std::vector<juce::StringArray> m_tabSections;   ///< per tab picture: the sections under it
        std::vector<juce::Rectangle<int>> m_picRects;   ///< where each of pics is drawn (empty when it does not fit)
        std::unique_ptr<juce::Component> m_live;   ///< the topic's live display, or none
        /** @brief The signal flow, drawn large enough to read: what the routing map in the header was for. */
        struct FlowDiagram : juce::Component {
            /**
             * @brief Catches no clicks; the diagram reads the processor for nothing but its existence.
             * @param[in,out] p  the processor
             */
            explicit FlowDiagram(NoctuaryProcessor& p) : m_proc(p) { setInterceptsMouseClicks(false, false); }
            /**
             * @brief The units as boxes in their group colours, the buses as arrows, on a fixed canvas scaled to fit.
             * @param[in,out] g  the graphics
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
            static constexpr float CanvasW = 1000.0f,   ///< the width of the canvas the diagram is drawn on
                                   CanvasH = 545.0f;    ///< its height
            NoctuaryProcessor& m_proc;   ///< the processor
        };
        FlowDiagram m_flow;   ///< shown on the overview topic only
        int m_topic = 0;   ///< the topic on screen
    };
    /**
     * @name Snapshots for the manual: a section as it stands on the panel (its tab is switched in for
     * the picture and back again), the modulation strip, the browser page.
     * @{ */
    /**
     * @brief A picture of one section, its tab opened for the picture and put back.
     * @param[in] name  the section
     * @return the snapshot, or an invalid image if there is no such section
     */
    juce::Image snapshotSection(const juce::String& name);
    /**
     * @brief A picture of one page of a tab row as it opens: the bar, the sections, the display.
     * @param[in] rowIndex  the tab row
     * @param[in] page      the page on it
     * @return the snapshot, or an invalid image if there is no such page
     */
    juce::Image snapshotTab(int rowIndex, int page);
    /**
     * @brief A picture of a whole page component, made visible for the picture and put back.
     * @param[in,out] page  the page
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
     * @param[in] tab     0 LFO, 1 envelopes, 2 matrix
     * @param[in] detail  true for the left third at twice the size, where it can be read on a page
     * @return the snapshot, or an invalid image without a strip
     */
    juce::Image snapshotStripTab(int tab, bool detail = false);
    /**
     * @brief the envelope tab's SOURCES page, from a click on a source's picture
     * @param[in] slot  which source asked (unused: the page shows all four)
     */
    void openSourceEnvelope(int slot);
    /**
     * @brief The sections on one page of a tab row.
     * @param[in] rowIndex  the tab row
     * @param[in] page      the page on it
     * @return the section names, empty for an unknown row or page
     */
    juce::StringArray tabSectionNames(int rowIndex, int page) const;
    /**
     * @brief What a tab is called on its own bar.
     * @param[in] rowIndex  the tab row
     * @param[in] page      the page on it
     * @return the label, empty for an unknown row or page
     */
    juce::String tabName(int rowIndex, int page) const;
    /** @} */
    /**
     * @brief Writes the manual out as pictures and text for Tools/make_manual.py to turn into a PDF.
     *
     * It has to happen here, in a running editor, because the pictures ARE the panel: snapshots
     * of the real sections with their real values, not drawings kept somewhere in step with it.
     * @param[in] dir     the folder the pictures, manual.json and coverage.txt go into
     * @param[in] onDone  called when the last step has run
     */
    void exportManual(const juce::File& dir, std::function<void()> onDone);
    struct ManualJob;
    std::unique_ptr<ManualJob> m_manual;   ///< the export in progress, or none
    /** @brief Runs the next step of the manual export and schedules the one after it. */
    void runManualStep();
    std::unique_ptr<HelpView> m_help;   ///< the help page
    std::unique_ptr<juce::TextButton> m_helpButton;   ///< Help, in the header
    std::unique_ptr<juce::TextButton> m_aboutButton;   ///< "?": version, build time, what is loaded
    std::unique_ptr<juce::TooltipWindow> m_tooltips;   ///< tooltips after 600 ms

    /** @brief One control on the panel: the component, its label, its host attachment and its width. */
    struct Cell {
        std::unique_ptr<juce::Component> m_comp;   ///< the knob, switch, chooser, button or label
        std::unique_ptr<juce::Label> m_label;   ///< the caption under it (renamed by the timer: CC, learn, file name)
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> m_slider;   ///< the host link of a knob
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> m_button;   ///< the host link of a switch
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> m_combo;   ///< the host link of a chooser
        juce::String m_baseLabel;   ///< the caption before any marking is appended
        int m_units = 1;   ///< width in cells (a chooser is two, a bank three)
        int m_param = -1;    ///< ParamId or -1 for extra cells
        /**
         * @brief The slot's type does not use it: not drawn and not laid out, rather than greyed.
         *
         * A
         * strip is as tall as the type it is set to, not as the union of every type.
         */
        bool m_unused = false;
    };
    /** @brief A titled box of cells: one parameter section, laid out in rows of at most maxUnits. */
    struct Section {
        juce::String m_name;   ///< the section's name, as the parameter table has it
        std::vector<int> m_cells;   ///< indices into m_cells, in the order they are laid out
        juce::Rectangle<int> m_bounds;   ///< where it was last placed, in content coordinates
        int m_maxUnits = 7;   ///< the width in cells a row may reach before it wraps (set by the fit)
        int m_wideUnits = 0;          ///< the natural width on the tabbed page (decides the columns; the fit may change it)
        int m_flowUnits = 0;          ///< the width in Expanded's flows, where a section's shape is its own
        int m_group = -1;   ///< index into m_groups, or -1 (the Master section, in the header)
        bool m_visible = true;   ///< false while its tab is not the open one
        /**
         * @brief Its switch is off (a type of Off, a level at zero, an Active that is not): only the
         * title and the switch's own cells are shown.
         *
         * Off means closed. Opening it is the
         * player's own action on the switch, so the page never rearranges itself.
         */
        bool m_collapsed = false;
        /**
         * @brief ... unless closing it buys nothing.
         *
         * A section is closed to save room, and where the row
         * has the room anyway, it is laid out open instead: a title beside an empty band is worse
         * than the section itself. Decided by the layout, from the row's own width, and cleared at
         * the start of every pass -- `collapsed` stays what the parameters say, so the timer that
         * watches for a switch being thrown cannot see this and rebuild for ever.
         */
        bool m_opened = false;
        /**
         * @brief Whether the section shows only its title and switch as it stands.
         * @return collapsed by its switch and not opened by the layout
         */
        bool closedNow() const { return m_collapsed && !m_opened; }
    };
    /** @brief A column's box of rows: a name, a colour, the sections of each row and the display beside them. */
    struct Group {
        juce::String m_name;   ///< the title over the box
        juce::Colour m_colour;   ///< the group's colour: its bar, its knobs, its tabs
        std::vector<std::vector<juce::String>> m_rows;   ///< section names per row
        juce::Rectangle<int> m_bounds;   ///< where it was last placed, in content coordinates
        int m_column = 0;   ///< which column it stands in
        std::vector<juce::Component*> m_displays;        ///< per row: a display that fills the leftover width, or null
        /**
         * @brief A row with no sections in it gets its height from here instead of from them; and if the
         * group is marked stretchy, that row also takes whatever height the taller column has
         * left over, so the two columns end level and the page has no hole in it at any size.
         */
        int  m_minRowH = 0;
        bool m_stretch = false;   ///< the last group of a column: its last row grows to the body's height
    };

    /**
     * @brief A row of a group whose sections take turns: one page open, the others a tab away.
     *
     * This is
     * what keeps the whole synth on one screen -- the three sources, the two filters, the effect
     * pairs and the conductor's tables are alike enough that seeing one at a time is no loss.
     */
    struct TabRow {
        int m_group = 0,   ///< index into m_groups
            m_row = 0;     ///< the row of that group which pages
        std::vector<juce::String> m_names;                       ///< one label per page
        std::vector<std::vector<juce::String>> m_pages;          ///< section names per page
        std::vector<juce::Component*> m_displays;                ///< per page: display for the leftover width, or null; two pages may share one
        int m_active = 0;   ///< the page that is open
        juce::Rectangle<int> m_bar;   ///< the tab bar, in content coordinates
        std::vector<juce::Rectangle<int>> m_tabs;   ///< one rectangle per tab, for painting and for the click
    };
    std::vector<TabRow> m_tabRows;   ///< the rows that page, filled in the constructor
    /**
     * @brief The tab row of a group's row, if it has one.
     * @param[in] group  index into m_groups
     * @param[in] row    the row in it
     * @return the tab row, or null if that row does not page
     */
    TabRow* tabRowFor(int group, int row);
    /**
     * @brief Shows or hides a section's cells (those its state shows) and records the flag.
     * @param[in,out] s  the section
     * @param[in]     v  visible or not
     */
    void    setSectionVisible(Section&, bool);
    /**
     * @brief A click on the content: if it hits a tab of a paging row, that page opens.
     * @param[in] contentPos  the click, in content coordinates
     */
    void    clickTabs(juce::Point<int> contentPos);

    /**
     * @brief The section of a name.
     * @param[in] name  the section's name
     * @return the section, or null if there is none
     */
    Section* findSection(const juce::String& name);
    /**
     * @name Which cells a section shows, and whether it is closed. Both are functions of the parameter
     * state alone, never of the window, so the layout is reproducible and the manual can be.
     * @{ */
    /**
     * @brief Whether a cell is on the page as the section stands.
     * @param[in] s  the section
     * @param[in] c  one of its cells
     * @return false for a cell the slot's type does not use, and for all but the switch's cells of a closed section
     */
    bool cellShown(const Section&, const Cell&) const;
    /**
     * @brief how many of its cells are on the page as it stands
     * @param[in] s  the section
     * @return the count of cells for which cellShown is true
     */
    int  shownCells(const Section&) const;
    /**
     * @brief Whether the section's switch says closed (never while m_openAll is set).
     * @param[in] s  the section
     * @return true if the section has a closer and its switch is off
     */
    bool sectionCollapsed(const Section&) const;
    /** @} */
    /**
     * @brief The width the section takes at its maxUnits: its widest row of shown cells plus the pads.
     * @param[in] s  the section
     * @return pixels
     */
    int      sectionWidth(const Section&) const;
    /**
     * @brief The height the section takes at its maxUnits: its rows of shown cells, the title and a pad.
     * @param[in] s  the section
     * @return pixels
     */
    int      sectionHeight(const Section&) const;
    /**
     * @brief Places the section and its cells, hiding the cells its state does not show.
     * @param[in,out] s  the section
     * @param[in]     x  left edge, in content coordinates
     * @param[in]     y  top edge, in content coordinates
     */
    void     layoutSection(Section&, int x, int y);

    NoctuaryProcessor& m_proc;   ///< the processor this editor shows
    NoctuaryLookAndFeel m_laf;   ///< the editor's look
    float m_scale = 1.0f;   ///< window size / design size
    int   m_designW = 1400,   ///< the design width the body is laid out at
          m_designH = 820;    ///< measured from the layout, not guessed
    int   m_bodyW = 0,   ///< what the body needs across, from the last layoutBody
          m_bodyH = 0;   ///< what it needs down
    /**
     * @brief Places every group and section and records what the body needs (m_bodyW, m_bodyH):
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
    std::vector<Cell> m_cells;   ///< every control on the panel, indexed by the sections
    std::vector<Section> m_sections;   ///< one per parameter section, in order of first appearance
    std::vector<Group> m_groups;   ///< the tabbed page's groups, two columns
    /**
     * @brief The Expanded page: its own table of groups and rows, three columns, no tabs.
     *
     * Rows without
     * a display wrap when their sections, open, would run past WrapW, so a row of closed
     * sections is one line and a row of open ones is two or three, never a column a screen wide.
     */
    std::vector<Group> m_expandedGroups;
    static constexpr int WrapW = 1400;   ///< the width at which an Expanded row without a display wraps, in pixels
    std::map<juce::Component*, int> m_cellOf;   ///< a parameter control's cell index, for the right-click menu
    std::unique_ptr<juce::Slider> m_master;   ///< the master gain knob in the header

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
         * @param[in,out] p  the processor whose engine it reads
         */
        explicit ScopeView(NoctuaryProcessor& p) : m_proc(p) { setInterceptsMouseClicks(false, false); startTimerHz(20); }
        /**
         * @brief The spectrum as thin bars behind one cycle of the wave the partials make, and the frequency.
         * @param[in,out] g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief Repaints while on screen; the smoothing happens in paint. */
        void timerCallback() override { if (isShowing()) repaint(); }
        NoctuaryProcessor& m_proc;   ///< the processor
        float m_amp[ambient::MaxPartials] = {};   ///< smoothed towards the engine's values
        int   m_count = 0;   ///< how many partials have been seen, the most so far
        bool  m_mode = false;                      ///< false = waveform, true = spectrum
    };
    std::unique_ptr<ScopeView> m_scope;   ///< the strand bank's display on the Expanded page (hidden on the tabbed one)
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> m_masterAttach;   ///< the master knob's host link
    /** @brief Everything below the header lives in a scrollable content component. */
    struct Content : juce::Component {
        std::function<void(juce::Graphics&)> m_onPaint;   ///< the editor paints the groups, tabs and sections
        std::function<void(const juce::MouseEvent&)> m_onMouse;   ///< the editor takes the click: dice and tabs
        /**
         * @brief Hands the painting to the editor.
         * @param[in,out] g  the graphics
         */
        void paint(juce::Graphics& g) override { if (m_onPaint) m_onPaint(g); }
        /**
         * @brief Hands a press to the editor.
         * @param[in] e  the press
         */
        void mouseDown(const juce::MouseEvent& e) override { if (m_onMouse) m_onMouse(e); }
    };
    Content m_content;   ///< the panel below the header, inside m_viewport
    juce::Viewport m_viewport;   ///< holds m_content; scroll bars hidden, the layout never needs them
    /**
     * @brief Paints the content: the group boxes, the tab bars, every visible section's frame, title, die and notes.
     * @param[in,out] g  the content's graphics
     */
    void paintContent(juce::Graphics&);
    std::unique_ptr<frame::IconButton> m_saveButton,     ///< a disk: save a preset file
                                       m_loadButton,     ///< a folder: load a preset file
                                       m_recButton;      ///< a red disc: record the output to WAV (lit while it records)
    std::unique_ptr<juce::TextButton> m_calibButton,     ///< Calibrate the hands (made, but the VR menu holds it)
                                      m_mapButton,       ///< Gestures... (made, but the VR menu holds it)
                                      m_performButton;   ///< the Perform page
    /**
     * @brief Main brings the panel back from any page; VR holds what only a headset needs (calibration,
     * the gesture table) under one button instead of two on the toolbar.
     */
    std::unique_ptr<juce::TextButton> m_mainButton, m_vrButton;   ///< the VR menu: calibrate, gestures
    /**
     * @brief The right of the header as every instrument of the family has it (Frame.h, 01.10.2026): undo, redo, help
     *        and the settings -- the window's layout, the session recall, the headset, the keys, About.
     */
    std::unique_ptr<frame::IconButton> m_undoIcon;
    std::unique_ptr<frame::IconButton> m_redoIcon;       ///< redo (Ctrl+Y)
    std::unique_ptr<frame::IconButton> m_helpIcon;       ///< the help (F1)
    std::unique_ptr<frame::IconButton> m_settingsIcon;   ///< the settings menu
    void showSettings();          ///< the frame's settings menu
    /**
     * @brief The headset's controls are shown: one sends its hands (Auto), or the settings say always (On).
     * @return true while they are shown
     */
    bool headsetShown() const;
    /** @brief Perform page: the eight macros as large knobs plus the morph, instead of the editor. */
    struct PerformView : juce::Component {
        /**
         * @brief Builds the eight knobs with their labels, the morph slider and switch, and the set recorder's buttons.
         * @param[in,out] p  the processor
         */
        explicit PerformView(NoctuaryProcessor& p);
        /**
         * @brief The frame, the title line, and the morph slots' names (refreshed every paint).
         * @param[in,out] g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief The set controls on the title line, the knobs in a 4 x 2 grid, the morph along the bottom. */
        void resized() override;
        NoctuaryProcessor& m_proc;   ///< the processor
        std::vector<std::unique_ptr<juce::Slider>> m_knobs;   ///< the eight macro knobs
        std::vector<std::unique_ptr<juce::Label>> m_labels;   ///< their names
        std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> m_attachments;   ///< their host links
        juce::Slider m_morph;   ///< the morph position
        juce::ToggleButton m_morphActive;   ///< the morph on or off
        juce::Label m_morphLabel,   ///< "A < position > B"
                    m_aLabel,       ///< slot A's name
                    m_bLabel;       ///< slot B's name
        /** @name set timeline: record everything that moves, play a set back
         *  @{ */
        juce::TextButton m_setRec{ "Record set" },     ///< starts recording; pressed again, asks where to save
                         m_setPlay{ "Play set..." },   ///< opens a set file and plays it
                         m_setStop{ "Stop set" };      ///< stops playing or recording
        juce::Label m_setInfo;   ///< recording or playing time, or what a set is
        std::unique_ptr<juce::FileChooser> m_chooser;   ///< the save / open dialog in flight
        /** @brief Refreshes the Rec button's state and the info line from the processor. */
        void updateSetInfo();
        /** @} */
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> m_morphAttach;   ///< the morph slider's host link
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> m_morphActiveAttach;   ///< the morph switch's host link
    };
    std::unique_ptr<PerformView> m_perform;   ///< the Perform page
    bool m_performing = false;   ///< the Perform page is the one showing
    /**
     * @brief Shows the Perform page, or the panel again.
     * @param[in] on  true for the Perform page
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
         * @param[in,out] p  the processor
         */
        explicit BrowseView(NoctuaryProcessor& p);
        /**
         * @brief The background and the column titles.
         * @param[in,out] g  the graphics
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
        int  getNumRows() override { return static_cast<int>(m_filtered.size()); }
        /**
         * @brief One preset: its name in its family's colour, a star if favourite, its measurements as small bars.
         * @param[in]     row       the row of the filtered list
         * @param[in,out] g         the graphics
         * @param[in]     w         row width
         * @param[in]     h         row height
         * @param[in]     selected  whether the row is the chosen one
         */
        void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override;
        /**
         * @brief Selects the preset; a click loads it (morphing into it if that is on) and puts the map cursor on it.
         * @param[in] row  the row of the filtered list
         * @param[in] e    the click
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
             * @param[in,out] o  the browser
             */
            explicit MapView(BrowseView& o) : m_owner(o) {}
            /**
             * @brief The cached cloud, then what moves: favourites, the route, the cursor, the crossing, the names.
             * @param[in,out] g  the graphics
             */
            void paint(juce::Graphics&) override;
            /**
             * @brief On a preset: selects it. Otherwise: starts a blend drag if Map blend is on, a pan if not.
             * @param[in] e  the press
             */
            void mouseDown(const juce::MouseEvent&) override;
            /**
             * @brief Pans the window onto the plane, or moves the blend cursor through the host.
             * @param[in] e  the drag
             */
            void mouseDrag(const juce::MouseEvent&) override;
            /**
             * @brief Tracks the preset under the mouse for the hover ring and name.
             * @param[in] e  the move
             */
            void mouseMove(const juce::MouseEvent&) override;
            /** @brief Ends a drag or a pan. */
            void mouseUp(const juce::MouseEvent&) override;
            /**
             * @brief Zooms about the mouse, 1 .. 40 times.
             * @param[in] e  the event, whose position is the pivot
             * @param[in] w  the wheel step
             */
            void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
            /** @brief Back to the whole plane. */
            void mouseDoubleClick(const juce::MouseEvent&) override;
            /**
             * @brief The preset whose point is nearest a screen position.
             * @param[in] p        the position, in the map's coordinates
             * @param[in] maxDist  how far away a point may be, in pixels
             * @return the preset index, or -1 if none is within reach
             */
            int  nearestPreset(juce::Point<float> p, float maxDist) const;
            /**
             * @brief Where a point of the plane lands on the screen at the current zoom and centre.
             * @param[in] x  0 .. 1 across the plane
             * @param[in] y  0 .. 1 up the plane
             * @return the position in the map's coordinates
             */
            juce::Point<float> toScreen(float x, float y) const;
            /**
             * @brief The plane position under a screen point.
             * @param[in] s  the position in the map's coordinates
             * @return clamped to the plane
             */
            juce::Point<float> toMap(juce::Point<float> s) const;
            /**
             * @brief The plane position under a screen point.
             * @param[in] s  the position in the map's coordinates
             * @return not clamped: for the zoom's pivot
             */
            juce::Point<float> toMapRaw(juce::Point<float> s) const;
            /**
             * @brief centre the view on a point at a zoom
             * @param[in] x  0 .. 1 across the plane
             * @param[in] y  0 .. 1 up the plane
             * @param[in] z  the zoom, clamped to 1 .. 40; at 1 the centre is the plane's
             */
            void zoomTo(float x, float y, float z);
            BrowseView& m_owner;   ///< the browser
            int m_hover = -1;   ///< the preset under the mouse, or -1
            bool m_dragging = false;   ///< a blend drag is in progress
            /**
             * @brief Seven thousand dots on a few hundred pixels are heaps, not presets.
             *
             * The view zooms
             * about the mouse and pans by dragging; the plane itself never changes, only the
             * window onto it, so a cursor position means the same thing at every zoom.
             */
            float m_zoom = 1.0f;
            juce::Point<float> m_centre { 0.5f, 0.5f };   ///< the plane point in the middle of the view
            bool m_panning = false;   ///< a pan drag is in progress
            juce::Point<float> m_panFrom,      ///< where the pan started: the mouse
                               m_centreFrom;   ///< the centre when it started
            /**
             * @brief The cloud itself, drawn once into an image.
             *
             * Eight and a half thousand filled dots,
             * fifteen times a second, were nineteen percentage points of a processor core for a
             * picture that only changes when the window, the zoom, the pan, the filter or the
             * colouring changes. What moves -- the cursor, the ring, the crossing, the names --
             * is drawn live on top of it.
             */
            juce::Image m_cloud;
            float m_cloudZoom = -1.0f;   ///< the zoom the cloud was drawn at
            juce::Point<float> m_cloudCentre { -1.0f, -1.0f };   ///< the centre the cloud was drawn at
            int  m_cloudW = 0,         ///< the width the cloud was drawn at
                 m_cloudH = 0,         ///< the height
                 m_cloudFilter = -1,   ///< the filterGen it was drawn for
                 m_cloudCount = -1;    ///< the preset count it was drawn for
            bool m_cloudGroups = false;   ///< whether the cloud was coloured by group
        };
        /** @} */
        /** @brief classic column browser (Omnisphere / Absynth style): each column narrows the list */
        struct Column : juce::ListBoxModel {
            BrowseView* m_owner = nullptr;   ///< the browser, told to filter again on a click
            juce::String m_title;   ///< drawn over the column
            juce::StringArray m_items;          ///< items[0] = "All"
            std::vector<uint32_t> m_tagBits;    ///< per item: tag mask (0 for family columns / All)
            std::vector<int> m_familyIdx;       ///< per item: family index or -1
            std::set<int> m_chosen;             ///< chosen rows (empty = All)
            std::vector<int> m_counts;          ///< presets per row, rebuilt when the list grows
            int m_countsFor = -1;               ///< the numPresets() the counts were taken at
            /** @brief Counts the presets behind every row, once per library size. */
            void updateCounts();
            juce::ListBox m_box;   ///< the column's list
            /**
             * @brief The rows of the column.
             * @return the size of items
             */
            int  getNumRows() override { return m_items.size(); }
            /**
             * @brief One row: its name, lit when chosen, with its preset count at the right.
             * @param[in]     row       the row
             * @param[in,out] g         the graphics
             * @param[in]     w         row width
             * @param[in]     h         row height
             * @param[in]     selected  unused: chosen is what lights a row
             */
            void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override;
            /**
             * @brief "All" clears; a row toggles alone or, with shift, joins the chosen set.
             * @param[in] row  the row
             * @param[in] e    the click, for its modifiers
             */
            void listBoxItemClicked(int row, const juce::MouseEvent&) override;
            /**
             * @brief Whether a preset passes this column's choice.
             * @param[in] preset  the preset index
             * @return true if nothing is chosen or the preset is in a chosen family or carries a chosen tag
             */
            bool passes(int preset) const;
        };
        Column m_columns[4];   ///< Family, Character, Motion, Features
        /**
         * @brief Switches the view and shows the controls that belong to it.
         * @param[in] m  0 classic columns, 1 map
         */
        void setMode(int m);
        int m_mode = 0;   ///< the view that is on: 0 columns, 1 map
        juce::TextButton m_modeClassic{ "Columns" },   ///< the column view
                         m_modeMap{ "Map" },           ///< the map view
                         m_star{ "Favourite" };        ///< stars or unstars the selected preset
        juce::ToggleButton m_onlyFavourites{ "only favourites" };   ///< the list narrowed to the starred presets
        juce::ToggleButton m_favouritesFirst{ "favourites first" };   ///< the starred ones at the top, whatever the sort
        /**
         * @brief Five thousand presets include some that barely move.
         *
         * Their measurements say so, and
         * this hides them: nothing is deleted, the list simply stops offering them.
         */
        juce::ToggleButton m_hideDull{ "hide the still ones" };
        /** @name A preset change as a journey rather than a cut, and how long it takes.
         *  @{ */
        juce::ToggleButton m_morphOnSelect{ "morph into it" };   ///< choosing a preset morphs into it instead of switching
        juce::Slider m_morphSeconds;   ///< how long that takes, 0.5 .. 600 s
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
             * @param[in,out] g  the graphics
             */
            void paint(juce::Graphics&) override;
            juce::String m_title,   ///< the preset's name
                         m_body;    ///< the text under it
        };
        InfoPanel m_info2;   ///< the panel beside the list
        int m_infoFor = -2;              ///< which preset the panel currently describes
        /** @brief Refreshes the info panel for the selected preset (or the current program) if it changed. */
        void updateInfo();
        /**
         * @brief The cloud's colour: the measured groups, or where a preset came from.
         *
         * Groups by
         * default -- what a browser is for is finding a sound, and the pack a sound was written
         * in is a fact about its author.
         */
        juce::ToggleButton m_colourByGroup{ "groups" };
        /**
         * @brief The four macro sliders.
         *
         * Each is a range over one measured descriptor, wide open by
         * default; narrowing one thins the cloud to what is left, and the view closes in on it.
         */
        struct Macro {
            juce::Slider m_slider;                  ///< a two-value slider, the range 0 .. 1
            juce::Label m_label;                    ///< the axis's name
            float ambient::PresetMeta::* m_field;   ///< the measured descriptor the range applies to
        };
        Macro m_macros[4];   ///< dark - bright, still - moving, smooth - rough, near - far
        /**
         * @brief Whether any macro range is narrower than the whole.
         * @return true if a slider's minimum is above 0 or its maximum below 1
         */
        bool  macroActive() const;
        /** @brief zoom the map onto what survived the filter */
        void  fitToFilter();
        bool  m_fitPending = false;   ///< a macro moved: fit after the next applyFilter

        NoctuaryProcessor& m_proc;   ///< the processor
        juce::TextEditor m_search;   ///< the name search
        juce::ComboBox m_family,   ///< the family filter (map view)
                       m_sort;     ///< the sort order
        std::vector<std::unique_ptr<juce::ToggleButton>> m_tagButtons;   ///< one per preset tag (map view)
        juce::ListBox m_list;   ///< the filtered presets
        MapView m_map;   ///< the map (map view)
        juce::ToggleButton m_mapActive;   ///< Map blend: dragging the cursor blends between neighbours
        juce::Slider m_radius;   ///< the blend radius
        juce::Label m_info;   ///< "n of m presets"
        /** @name route strip (map view): preset routes, play/loop/speed, add the cursor as a point, edit the text
         *  @{ */
        juce::ComboBox m_routeBox;   ///< the route presets
        juce::ToggleButton m_routePlay,   ///< play the route
                           m_routeLoop;   ///< loop it
        juce::Slider m_routeSpeed;   ///< how fast the route travels
        juce::TextButton m_routeAdd{ "+ point" },     ///< append the cursor as a waypoint
                         m_routeClear{ "Clear" },     ///< clear the route
                         m_routeEdit{ "Route..." };   ///< open the text editor
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> m_routePlayAttach,   ///< the play switch's host link
                                                                              m_routeLoopAttach;   ///< the loop switch's host link
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> m_routeSpeedAttach;   ///< the speed's host link
        juce::Component::SafePointer<juce::TextEditor> m_routeEditor;   ///< the text editor while its dialog lives
        juce::String m_routeEditText;                ///< the editor's text as the timer last saw it
                     bool m_routeEditOpen = false;   ///< the dialog is up: mirror the text, apply it when it is gone
        /** @brief Opens the route as text in a dialog: one point per line, applied when the dialog closes. */
        void showRouteEditor();
        /** @} */
        juce::TextButton m_toA{ "-> A" },    ///< the selected preset into morph slot A
                         m_toB{ "-> B" },    ///< into morph slot B
                         m_load{ "Load" };   ///< loads the selected preset
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> m_mapActiveAttach;   ///< the blend switch's host link
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> m_radiusAttach;   ///< the radius's host link
        std::vector<int> m_filtered;   ///< the presets that pass the filter, in sort order
        int m_filterGen = 0;      ///< bumped by applyFilter, so the map knows its cached cloud is stale
        int m_selected = -1;   ///< the preset chosen in the list or on the map, or -1
        /**
         * @name "More like this": the list narrowed to the map's nearest neighbours of the selected
         * preset, and the map zoomed onto them. The similarity is the one the map already has.
         * @{ */
        juce::ToggleButton m_similar { "similar" };   ///< the switch
        std::vector<int> m_similarSet;   ///< the selected preset and its neighbours; empty when off
        int m_similarOf = -1;   ///< the preset the set was taken for
        /** @} */
    };
    std::unique_ptr<BrowseView> m_browse;   ///< the Browse page
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
         * @param[in,out] p  the processor
         * @param[in,out] o  the editor, for undo, help and the drop target
         */
        ModView(NoctuaryProcessor& p, NoctuaryEditor& o);
        /**
         * @brief The lane of cards, then the open tab's panels, then the drag line while a card is in flight.
         * @param[in,out] g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief The lane along the top, the tab bar, and the open tab's rows and knobs (EditorModStrip.cpp). */
        void resized() override;
        /** @brief Repaints while on screen, 15 Hz. */
        void timerCallback() override;
        /**
         * @brief A card: starts a drag or opens its tab. A tab or page button. An envelope point: starts dragging it;
         *        right-click: the shape menu.
         * @param[in] e  the press
         */
        void mouseDown(const juce::MouseEvent&) override;
        /**
         * @brief Moves the envelope point being dragged, or the card in flight (and the editor's overlay).
         * @param[in] e  the drag
         */
        void mouseDrag(const juce::MouseEvent&) override;
        /**
         * @brief Drops the card: on a knob, a route is added and the depth popup shown; the overlay is hidden.
         * @param[in] e  the release, whose screen position is the drop target
         */
        void mouseUp(const juce::MouseEvent&) override;
        /**
         * @brief Tracks the card and the envelope point under the mouse for the highlight.
         * @param[in] e  the move
         */
        void mouseMove(const juce::MouseEvent&) override;

        /** @brief one LFO or one envelope: its curve and the controls that shape it */
        struct Row {
            std::vector<std::unique_ptr<juce::Component>> m_controls;   ///< the knobs and choosers, in the order they are placed
            std::vector<std::unique_ptr<juce::Label>> m_labels;   ///< their names
            std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> m_sliders;   ///< the knobs' host links
            std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>> m_combos;   ///< the choosers' host links
            juce::Rectangle<int> m_curve;   ///< where its curve is drawn
            juce::String m_title;   ///< "LFO 3", "ENV 2", "SOURCE 1"
        };
        Row m_lfos[ambient::NumLfos];   ///< the LFO tab's rows
        static constexpr int EnvRows = ambient::NumModEnvs + ambient::Slots;   ///< the six, then the four sources' own
        Row m_envs[EnvRows];   ///< the envelope tab's rows, both pages

        /** @brief A card in the lane: one modulation source, drawn with its own live shape. */
        struct Card {
            ambient::ModSourceE m_source = ambient::ModSourceE::NONE;   ///< the source it stands for
            juce::String m_label;   ///< "LFO 1", "MACRO A", "KURA 2", "AMP" ...
            juce::Colour m_colour;   ///< the source's colour (edt::sourceColour)
            juce::Rectangle<int> m_bounds;   ///< where it is in the lane
            int m_tab = 0;                 ///< which tab this card belongs to
            int m_index = 0;               ///< LFO or envelope number, for the curve
        };
        std::vector<Card> m_cards;   ///< every source that can drive something, in matrix order
        int m_tab = 0;                     ///< 0 LFO, 1 envelopes, 2 matrix
        int m_envPage = 0;                 ///< on the envelope tab: 0 the six, 1 the sources' own
        int m_hoverCard = -1;   ///< the card under the mouse, or -1
        int m_dragCard = -1;               ///< the card being dragged onto a knob
        juce::Point<int> m_dragPos;   ///< where the dragged card is, in the strip's coordinates
        /** @brief Editing an envelope's shape: which one, and which of its breakpoints is under the mouse. */
        int m_dragEnv = -1,      ///< the row being edited, or -1
            m_dragPoint = -1,    ///< the breakpoint being dragged, or -1
            m_hoverEnv = -1,     ///< the row under the mouse, or -1
            m_hoverPoint = -1;   ///< the breakpoint under the mouse, or -1

        /**
         * @brief Opens a tab: shows its rows and buttons, pulls the matrix for the table.
         * @param[in] t  0 LFO, 1 envelopes, 2 matrix
         */
        void setTab(int t);
        /**
         * @brief Turns the envelope tab's page.
         * @param[in] page  0 the six modulation envelopes, 1 the four sources' own
         */
        void setEnvPage(int page);
        /**
         * @brief on the page that is showing
         * @param[in] env  the row, 0 .. EnvRows - 1
         * @return true if the row belongs to the page that is open
         */
        bool envVisible(int env) const;
        /**
         * @brief a row's shape, of the six or a source's
         * @param[in] env  the row, 0 .. EnvRows - 1
         * @return the engine's envelope shape
         */
        const ambient::ModEnv& shapeOf(int env) const;
        /**
         * @brief a row's parameter: "env3_time", "src2_env_time"
         * @param[in] env    the row, 0 .. EnvRows - 1
         * @param[in] field  "mode", "time", "depth" or "sync"
         * @return the parameter key
         */
        juce::String envKey(int env, const char* field) const;
        /**
         * @brief The lane's background and every card.
         * @param[in,out] g  the graphics
         */
        void paintLane(juce::Graphics&);
        /**
         * @brief One card: its frame, its live curve and value dot, its label.
         * @param[in,out] g    the graphics
         * @param[in]     c    the card
         * @param[in]     hot  under the mouse or in flight
         */
        void paintCard(juce::Graphics&, const Card&, bool hot);
        /**
         * @brief One LFO row's curve: the shape over a cycle, the live phase, the period.
         * @param[in,out] g  the graphics
         * @param[in]     i  the LFO, 0 .. NumLfos - 1
         */
        void paintLfo(juce::Graphics&, int i);
        /**
         * @brief One envelope row's curve: the breakpoints, the sustain, the loop, the live position, the length.
         * @param[in,out] g  the graphics
         * @param[in]     i  the row, 0 .. EnvRows - 1
         */
        void paintEnv(juce::Graphics&, int i);
        /**
         * @brief On a point: removes it; on the curve: puts one there (sixteen at most).
         * @param[in] e  the click
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
            float m_x0 = 0,       ///< the curve's left edge
            m_w = 1,              ///< its width
            m_cy = 0,             ///< its centre line (the bottom for a unipolar shape)
            m_h = 1,              ///< its half height (the full height for a unipolar shape)
            m_len = 1,            ///< the shape's length in seconds
            m_depth = 1;          ///< the Depth that scales it, floored at 0.05 so a flat line stays editable
            bool m_uni = false;   ///< a source's own envelope: unipolar, drawn from the bottom
        };
        /**
         * @brief The geometry of one envelope row.
         * @param[in] env  the row, 0 .. EnvRows - 1
         * @return where its curve is drawn and how it is scaled
         */
        EnvGeom envGeom(int env) const;
        /**
         * @brief Which envelope row, and which breakpoint, a position is on.
         * @param[in]     pos       the position in the strip's coordinates
         * @param[in,out] pointOut  if given, receives the breakpoint index under the mouse, or -1
         * @return -1 if not on a curve
         */
        int  envAt(juce::Point<int> pos, int* pointOut = nullptr) const;
        /**
         * @brief Where a breakpoint of an envelope is drawn.
         * @param[in] env    the row
         * @param[in] time   the point's time, seconds
         * @param[in] value  the point's value, -1 .. 1 (0 .. 1 for a source's own)
         * @return the position in the strip's coordinates
         */
        juce::Point<float> envToXY(int env, float time, float value) const;
        /**
         * @brief The time and value a position stands for, the inverse of envToXY.
         * @param[in]  env    the row
         * @param[in]  pos    the position in the strip's coordinates
         * @param[out] time   receives the time, seconds
         * @param[out] value  receives the value
         */
        void envFromXY(int env, juce::Point<int> pos, float& time, float& value) const;
        /**
         * @brief A copy of a row's shape to edit.
         * @param[in] env  the row
         * @return the shape
         */
        ambient::ModEnv envCopy(int env) const;
        /**
         * @brief Writes a shape into the engine, with a snapshot for undo.
         * @param[in] env   the row
         * @param[in] e     the shape
         * @param[in] what  the undo name, empty for a step in a drag that was already recorded
         */
        void envCommit(int env, const ambient::ModEnv&, const juce::String& what);
        /**
         * @brief The right-click menu of an envelope: sustain and loop points, a segment's curvature, starting shapes.
         * @param[in] env    the row
         * @param[in] point  the breakpoint clicked, or -1
         */
        void envShapeMenu(int env, int point);
        /**
         * @brief An LFO's settings as the engine reads them, for drawing its shape.
         * @param[in] i  the LFO, 0 .. NumLfos - 1
         * @return the spec
         */
        ambient::LfoSpec specOf(int i) const;
        /**
         * @brief Where a drag ended: the parameter under the mouse, or none.
         * @param[in] screenPos  the position in screen coordinates
         * @return the ParamId as an int, or -1
         */
        int paramUnder(juce::Point<int> screenPos) const;

        NoctuaryProcessor& m_proc;   ///< the processor
        NoctuaryEditor& m_owner;   ///< the editor
        juce::TextButton m_tabLfo{ "LFO" },         ///< the LFO tab
                         m_tabEnv{ "ENVELOPES" },   ///< the envelope tab
                         m_tabMatrix{ "MATRIX" };   ///< the matrix tab
        juce::TextButton m_pageMod{ "MODULATION 1-6" },   ///< the envelope tab's first page: the six
                         m_pageSrc{ "SOURCES 1-4" };      ///< its second: the sources' own
        /** @brief The matrix page: a table of routes (EditorMatrix.cpp), where a text box used to be. */
        std::unique_ptr<edt::RouteTable> m_table;
        juce::Label m_matrixInfo,   ///< "n of 32 routes"
                    m_hint;         ///< what a row is, and that a card can be dragged instead
        juce::Rectangle<int> m_lane,       ///< the band of cards along the top
                             m_tabsArea,   ///< the tab bar
                             m_content;    ///< the open tab's panel
        /** @brief Defined in EditorModStrip.cpp, where RouteTable is a complete type. */
        ~ModView() override;
        /** @brief Reads the engine's matrix into the table and the count. */
        void pullMatrix();
        /**
         * @brief Adds a route from a dragged source to a parameter, with a small default depth.
         * @param[in] src     the source
         * @param[in] target  the parameter
         * @return true if the engine accepted the matrix (false when it is full or the text was refused)
         */
        bool addRoute(ambient::ModSourceE src, ambient::ParamIdE target);
    };
    std::unique_ptr<ModView> m_mod;   ///< the strip
    juce::Viewport m_modPort;   ///< unused holder for the strip
    std::unique_ptr<juce::TextButton> m_modButton;   ///< unused: the strip is always on the main page

    /** @name The family's ways around the panel (15.10.2026, Frame.h, from Empyrean)
     *  @{ */
    std::unique_ptr<frame::IconButton> m_searchIcon;   ///< the command palette (Ctrl+K): a magnifier
    std::unique_ptr<frame::IconButton> m_nightIcon;    ///< the night mode: a crescent
    std::unique_ptr<frame::IconButton> m_holdIcon;     ///< held: the preset as it was loaded ("A|B")
    std::unique_ptr<juce::TextButton> m_journeyButton; ///< the journeys over time
    std::unique_ptr<juce::Label> m_star;               ///< "*" beside the Sound box while the knobs have moved from the preset
    std::unique_ptr<frame::CommandPalette> m_palette;  ///< over everything while open
    std::unique_ptr<frame::Tour> m_tour;               ///< the first steps
    std::unique_ptr<JourneyPanel> m_journeyPanel;      ///< the journeys over time, an overlay
    std::unique_ptr<frame::ModulationAtKnob> m_modulation;   ///< a knob's menu: its routes with their depths, "Modulate with"
    frame::WindowKeeper m_window;                      ///< the standalone's window back where it was left
    /** @brief The night's veil: dark and warm over the panel, under the overlays; the mouse goes through it. */
    struct Veil : juce::Component {
        float m_alpha = 0.0f;   ///< how dark
        /**
         * @brief Dark and warm: little blue for eyes used to the dark.
         * @param[in,out] g  where
         */
        void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff0c0703).withAlpha(m_alpha)); }
    };
    Veil m_veil;                                       ///< the night's veil
    bool m_night = false;                              ///< the night mode is on
    Snapshot m_held;                                   ///< the sound while the preset is held
    bool m_holding = false;                            ///< the preset is held
    Snapshot m_presetSnap;                             ///< the parameters as the preset left them (the star compares)
    int m_presetSeen = -2;                             ///< the preset m_presetSnap was taken of
    int m_tourTicks = 0;                               ///< ticks until the first start's tour (-1: considered)
    /** @brief Whether this is an automated run (AMBIENT_SHOT, AMBIENT_MANUAL, AMBIENT_MUTE): no tour, no window state. @return true then */
    static bool automated();
    void openPalette();                               ///< the command palette (Ctrl+K)
    /**
     * what it can do: actions, pages, knobs, presets
     * @return the palette's commands
     */
    std::vector<frame::CommandPalette::Command> paletteCommands();
    /**
     * @brief Shows a parameter's control (the main page, its tab, scrolled to it) and lights it.
     * @param[in] id  the parameter
     * @return false without a cell
     */
    bool revealParam(int id);
    void startTour();                                 ///< the first steps, from the first
    /**
     * @brief The night mode on or off (kept in the instrument's settings).
     * @param[in] on  night
     */
    void setNight(bool on);
    /**
     * @brief The preset as it was loaded while held; the sound again when let go.
     * @param[in] on  held
     */
    void holdPreset(bool on);
    /**
     * @brief The journey chosen in the Morph section's box.
     * @param[out] out  receives it
     * @return false when none is chosen
     */
    bool chosenJourney(ambient::Journey& out) const;
    /**
     * @brief Starts the chosen journey at step @p step (the timeline's click while none runs).
     * @param[in] step  the step
     */
    void startJourneyAt(int step);
    /**
     * @brief A route's depth anew, in the matrix text.
     * @param[in] row    the route
     * @param[in] depth  -1 .. 1
     */
    void setRouteDepth(int row, float depth);
    /**
     * @brief A route taken out of the matrix text.
     * @param[in] row  the route
     */
    void removeRoute(int row);
    /** @} */
public:
    /** @name The cells the modulation strip needs to find a drop target and to mark modulated knobs.
     *  @{ */
    /**
     * @brief Which parameter sits under a screen point.
     * @param[in] screenPos  the position in screen coordinates
     * @return the ParamId as an int, or -1 if no parameter cell is showing there
     */
    int cellParamAt(juce::Point<int> screenPos) const;
    /**
     * @brief Where a cell's control is on the screen.
     * @param[in] cellIndex  index into m_cells
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
         * @param[in,out] p  the processor
         */
        explicit FilterView(NoctuaryProcessor& p) : m_proc(p) { setInterceptsMouseClicks(false, false); startTimerHz(15); }
        /**
         * @brief The axes, the two branches and their product, and the legend (EditorViews.cpp).
         * @param[in,out] g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief Nothing but parameters is drawn here, so nothing but a parameter can change it. */
        void timerCallback() override
        { const uint32_t g = m_proc.paramGeneration(); if (isShowing() && g != m_seen) { m_seen = g; repaint(); } }
        NoctuaryProcessor& m_proc;   ///< the processor
        uint32_t m_seen = 0xffffffffu;   ///< the parameter generation last drawn
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
         * @param[in,out] p  the processor
         * @param[in]     s  the slot, 1 .. 4
         */
        SourceView(NoctuaryProcessor& p, int s) : m_proc(p), m_slot(s) { setInterceptsMouseClicks(true, false); startTimerHz(15); }
        /**
         * @brief The source's picture, then the entrance in the corner.
         * @param[in,out] g  the graphics
         */
        void paint(juce::Graphics&) override;
        /**
         * @brief The picture of whatever the slot is set to: table, FM, clip, noise, partials, string, model.
         * @param[in,out] g  the graphics
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
         * @param[in,out] g  the graphics
         */
        void paintEntrance(juce::Graphics&);
        /**
         * @brief On the entrance: opens the source's envelope in the strip. On a table: flips between depth and flat.
         * @param[in] e  the press
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
            const int type = static_cast<int>(std::lround(m_proc.engine().getParam(ambient::slotParamIds(m_slot - 1)[0])));
            const bool alive = type == 3 || type == 5 || type == 6 || type == 7 || type == 8 || type >= 10;   // ...and the near sources
            const bool moved = (type == 1 || type == 9) && std::fabs(livePosition() - m_shownPos) > 0.002f;
            const uint32_t g = m_proc.paramGeneration();
            if (alive || moved || ownEntrance() || g != m_seen) { m_seen = g; repaint(); }
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
            const float live = m_proc.engine().displaySlotPosition(m_slot - 1);
            if (live >= 0.0f) return juce::jlimit(0.0f, 1.0f, live);
            const ambient::ParamIdE id = ambient::slotParamIds(m_slot - 1)[6];
            return juce::jlimit(0.0f, 1.0f, m_proc.engine().effectiveParam(id) + m_proc.engine().modAmount(id));
        }
        /**
         * @brief The table in depth; false when there is nothing to stand in depth (no table, one frame).
         * @param[in,out] g      the graphics
         * @param[in]     plot   the area to draw in
         * @param[in]     type   the slot's type (Harmonic or Wavetable)
         * @param[in]     table  which table
         * @param[in]     pos    the Position, 0 .. 1, whose frame is lit
         * @return true if the table was drawn
         */
        bool paintTable3D(juce::Graphics& g, juce::Rectangle<float> plot, int type, int table, float pos);
        NoctuaryProcessor& m_proc;   ///< the processor
        uint32_t m_seen = 0xffffffffu;   ///< the parameter generation last drawn
        int m_slot;   ///< 1 .. 4
        float m_amp[ambient::TablePartials] = {};   ///< smoothed live amplitudes for the additive picture
        bool  m_flat = false;       ///< the table as one frame (after a click) rather than in depth
        float m_shownPos = -1.0f;   ///< the Position last drawn
        /**
         * @brief The table sampled for drawing, TablePoints + 1 points a frame, and its lines drawn once
         * into an image at the display's size: both rebuilt only when the table or the size changes.
         */
        static constexpr int TablePoints = 96;
        std::vector<float> m_tableCycles;   ///< the sampled frames, TablePoints + 1 floats each
        int    m_tableFrames = 0;   ///< how many frames are sampled
        double m_tableSig = -1.0;   ///< a signature of the table the samples came from
        juce::Image m_tableMesh;   ///< the lines, drawn once
        juce::Rectangle<int> m_meshBounds;   ///< the plot the mesh was drawn for
        float  m_meshScale = 0.0f;   ///< the scale the mesh was drawn at
    };
    /**
     * @brief The conductor's notes as they happen: a piano roll scrolling left, one column per tick, so
     * the cluster brain's choices can be watched rather than inferred from the keyboard strip.
     */
    struct BrainView : juce::Component, juce::Timer {
        /**
         * @brief Catches no clicks and ticks at 15 Hz.
         * @param[in,out] p  the processor
         */
        explicit BrainView(NoctuaryProcessor& p) : m_proc(p) { setInterceptsMouseClicks(false, false); startTimerHz(15); }
        /**
         * @brief The roll: octave lines, a bar per held note fading with age, the range labels, the cascade's aura.
         * @param[in,out] g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief Writes the sounding notes into the newest column, widens the range, advances the head. */
        void timerCallback() override;
        NoctuaryProcessor& m_proc;   ///< the processor
        static constexpr int Cols = 240;                 ///< 16 s at 15 Hz
        std::vector<std::array<bool, 128>> m_hist = std::vector<std::array<bool, 128>>(Cols);   ///< the ring of columns: which notes sounded at each tick
        int m_head = 0;   ///< the column written next
        int m_lo = 36,   ///< the bottom of the note range in view
            m_hi = 84;   ///< the note range in view, widened as notes arrive
    };
    /**
     * @brief Three rolls: the conductor's, the second conductor's, and one on the Autoplay tab, where
     * watching the chord travel one voice at a time is the whole point of the mode.
     */
    std::unique_ptr<BrainView> m_brainView,    ///< the Cluster Brain tab's roll
                               m_brainView2,   ///< Brain 2's roll
                               m_brainView3;   ///< the Autoplay tab's roll
    /**
     * @brief The stereo stage: every sounding voice as a dot, left-right by its pan, near-far by its
     * plane, size by its envelope -- the spatial model (concept.md) as a picture, moving.
     */
    struct StageView : juce::Component, juce::Timer {
        /**
         * @brief Ticks at 15 Hz; takes the mouse for the planes.
         * @param[in,out] p  the processor
         */
        explicit StageView(NoctuaryProcessor& p) : m_proc(p) { startTimerHz(15); }
        /**
         * @brief The room, the three planes as lines, every voice as a smoothed dot, the count.
         * @param[in,out] g  the graphics
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
         * @param[in] e  the move; the plane under it is highlighted and the cursor changes
         */
        void mouseMove(const juce::MouseEvent&) override;
        /**
         * @brief Grabs the plane under the mouse: a snapshot for undo, then the host's change gesture begins.
         * @param[in] e  the press
         */
        void mouseDown(const juce::MouseEvent&) override;
        /**
         * @brief Moves the grabbed plane's depth parameter to where the mouse is.
         * @param[in] e  the drag
         */
        void mouseDrag(const juce::MouseEvent&) override;
        /** @brief Ends the host's change gesture and lets the plane go. */
        void mouseUp(const juce::MouseEvent&) override;
        /** @brief No plane is hovered once the mouse has left. */
        void mouseExit(const juce::MouseEvent&) override { m_hoverLine = -1; repaint(); }
        /**
         * @brief The room the dots and planes are drawn in.
         * @return the plot rectangle inside the frame
         */
        juce::Rectangle<float> plotRect() const;
        /**
         * @brief Which plane's line a position is near.
         * @param[in] pos  the position in the view's coordinates
         * @return 0 conductor, 1 keys, 2 conductor 2, or -1 if none is within reach
         */
        int  lineAt(juce::Point<int>) const;
        std::function<void(const juce::String&)> m_onUndo;   ///< called with a name before a plane is moved, for the editor's undo
        int m_hoverLine = -1,   ///< the plane under the mouse, -1 for none
            m_dragLine = -1;    ///< the plane being dragged, -1 for none
        NoctuaryProcessor& m_proc;   ///< the processor
        /** @brief One voice's dot. */
        struct Dot {
            float m_x = 0.0f,    ///< its smoothed x in the plot
            m_y = 0.0f,          ///< its smoothed y
            m_r = 0.0f;          ///< its smoothed radius
            int m_note = -1;     ///< the note it stands for, -1 once it is gone
            bool m_on = false;   ///< whether the voice sounded at the last paint
        };
        Dot m_dots[ambient::Engine::MaxVoices];   ///< smoothed positions, one per voice slot seen
    };
    std::unique_ptr<StageView> m_stageView;   ///< the Space / Foundation row's display
    /**
     * @brief The Tuning page's display: the timbre's own roughness curve across the octave (Sethares),
     * the chosen scale's degrees on it, the key the conductor has found, the comma and the tide.
     *
     * What the tuning section computes, drawn, instead of only its numbers.
     */
    struct TuningView : juce::Component, juce::Timer {
        /**
         * @brief Catches no clicks and repaints at 5 Hz: the curve is slow to compute and slow to change.
         * @param[in,out] p  the processor
         */
        explicit TuningView(NoctuaryProcessor& p) : m_proc(p) { setInterceptsMouseClicks(false, false); startTimerHz(5); }
        /**
         * @brief The roughness curve, the scale's degrees on it, the key, the comma, the tide and the scale's name.
         * @param[in,out] g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief Repaints while on screen. */
        void timerCallback() override { if (isShowing()) repaint(); }
        NoctuaryProcessor& m_proc;   ///< the processor
        static constexpr int Points = 240;   ///< five cents apart
        float m_curve[Points] = {};   ///< the roughness across the octave, recomputed in paint
    };
    std::unique_ptr<TuningView> m_tuningView;   ///< the Tuning tab's display
    /**
     * @brief The Coherence page's display: the four Kuramoto phases on a ring, the Lenia field as a
     * grey grid, the six attractor readings as bars -- the living modulators, seen.
     */
    struct CoherenceView : juce::Component, juce::Timer {
        /**
         * @brief Catches no clicks and ticks at 10 Hz.
         * @param[in,out] p  the processor
         */
        explicit CoherenceView(NoctuaryProcessor& p) : m_proc(p) { setInterceptsMouseClicks(false, false); startTimerHz(10); }
        /**
         * @brief The ring of phases, the Lenia grid, the attractor bars and the two orbits.
         * @param[in,out] g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief Appends the attractors' current x/y to the trails (forty seconds kept) and repaints. */
        void timerCallback() override;
        NoctuaryProcessor& m_proc;   ///< the processor
        /** @brief Where the two attractors have been: the last forty seconds of their x/y, drawn as orbits. */
        std::vector<juce::Point<float>> m_lorenzTrail, m_rosslerTrail;   ///< the Roessler attractor's trail
    };
    std::unique_ptr<CoherenceView> m_coherenceView;   ///< the Coherence + Clock tab's display
    /**
     * @brief The Cosmos return's spectrum: what the shifter, the resonator, the vowel and the nebula are
     * handing back, on a log-frequency axis, smoothed the way a meter falls.
     */
    struct CosmosView : juce::Component, juce::Timer {
        /**
         * @brief Sizes the FFT buffers, builds the Hann window, starts at 15 Hz.
         * @param[in,out] p  the processor
         */
        explicit CosmosView(NoctuaryProcessor& p);
        /**
         * @brief The bins as a filled curve on the log axis, and a legend of which stages are on.
         * @param[in,out] g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief Reads the return tap, transforms it, folds the bins into Bins log-spaced bands with a falling hold. */
        void timerCallback() override;
        NoctuaryProcessor& m_proc;   ///< the processor
        static constexpr int N = 2048,     ///< FFT length
                             Bins = 160;   ///< the log-spaced bands drawn
        std::vector<float> m_re,       ///< the FFT's real part
                           m_im,       ///< its imaginary part
                           m_window;   ///< the Hann window
        std::unique_ptr<ambient::Fft> m_fft;   ///< the transform
        float m_bins[Bins] = {};   ///< dB per log-spaced bin, smoothed
        bool  m_silent = true;   ///< nothing came back on the last tick
    };
    std::unique_ptr<CosmosView> m_cosmosView;   ///< the Cosmos tab's display
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
         * @param[in,out] p  the processor
         */
        explicit LoudnessView(NoctuaryProcessor& p) : m_proc(p) { startTimerHz(10); }
        /**
         * @brief Momentary and integrated loudness, the range, the peak, as bars and figures.
         * @param[in,out] g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief Repaints while on screen. */
        void timerCallback() override { if (isShowing()) repaint(); }
        /** @brief click to start measuring again */
        void mouseDown(const juce::MouseEvent&) override;
        NoctuaryProcessor& m_proc;   ///< the processor
    };
    std::unique_ptr<LoudnessView> m_outputView;   ///< in the header, right half
    /**
     * @brief The Vector's square, with the point in it: three corners are the source slots, the fourth
     * is all three together.
     *
     * Drag the point; it is the one control here whose value is a place.
     */
    struct VectorView : juce::Component, juce::Timer {
        /**
         * @brief Ticks at 15 Hz; takes the mouse for the point.
         * @param[in,out] p  the processor
         */
        explicit VectorView(NoctuaryProcessor& p) : m_proc(p) { startTimerHz(15); }
        /**
         * @brief The square with its corners named, the wander's trail, the point, and a hint.
         * @param[in,out] g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief Repaints while on screen. */
        void timerCallback() override { if (isShowing()) repaint(); }
        /**
         * @brief A press moves the point.
         * @param[in] e  the press
         */
        void mouseDown(const juce::MouseEvent& e) override { drag(e); }
        /**
         * @brief A drag moves the point.
         * @param[in] e  the drag
         */
        void mouseDrag(const juce::MouseEvent& e) override { drag(e); }
        /**
         * @brief Writes the mouse position, clamped to the square, into VecX and VecY through the host.
         * @param[in] e  the press or drag
         */
        void drag(const juce::MouseEvent&);
        /**
         * @brief The square the point moves in.
         * @return the largest square that fits under the title
         */
        juce::Rectangle<float> square() const;
        NoctuaryProcessor& m_proc;   ///< the processor
        static constexpr int Trail = 240;      ///< where the wander has been, at 20 Hz: twelve seconds
        float m_tx[Trail] = {},   ///< the trail's x positions, a ring
              m_ty[Trail] = {};   ///< the trail's y positions
        int   m_head = 0,     ///< the ring's write position
              m_filled = 0;   ///< how much of the ring is filled
    };
    std::unique_ptr<VectorView> m_vectorView;   ///< the MORPH row's display
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
         * @param[in,out] p  the processor
         */
        explicit SpectrumView(NoctuaryProcessor& p);
        /**
         * @brief The axes, the hold trace, the live bands, the filter's response over them, the note ticks and the
         *        frequency under the pointer.
         * @param[in,out] g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief Reads the output tap, transforms it, folds it into the bands and lets the hold fall. */
        void timerCallback() override;
        /**
         * @brief Remembers the pointer's x, so paint can name the frequency under it.
         * @param[in] e  the move
         */
        void mouseMove(const juce::MouseEvent&) override;
        /** @brief Forgets the pointer. */
        void mouseExit(const juce::MouseEvent&) override;
        NoctuaryProcessor& m_proc;   ///< the processor
        static constexpr int N = ambient::OutTapLen;   ///< FFT length: the whole tap
        static constexpr int Bands = 480;               ///< one band per two or three pixels
        static constexpr float LoHz = 20.0f,      ///< the left end of the frequency axis
                               HiHz = 16000.0f;   ///< its right end
        /**
         * @brief The axis runs a little above zero so the filter, drawn on the same decibels, has
         * somewhere to put a resonance: a flat response is the 0 dB line, not the ceiling.
         */
        static constexpr float FloorDb = -84.0f, TopDb = 6.0f;   ///< the top of the decibel axis
        static constexpr int LabelGutter = 26;   ///< room at the left for the decibel numbers
        std::vector<float> m_re,       ///< the FFT's real part
                           m_im,       ///< its imaginary part
                           m_window;   ///< the Hann window
        std::unique_ptr<ambient::Fft> m_fft;   ///< the transform
        std::vector<float> m_band,   ///< the live bands, dB
                           m_hold;   ///< live bands, and a slowly falling trace
        float m_peakDb = -96.0f;   ///< the loudest band of the last tick, dB
        int   m_hoverX = -1;                               ///< read the frequency under the pointer
    };
    std::unique_ptr<SpectrumView> m_spectrumView;   ///< the ANALYSIS group's display
    /** @brief The amplitude envelope as a curve, with the loudest voice's level on it. */
    struct EnvView : juce::Component, juce::Timer {
        /**
         * @brief Catches no clicks and repaints at 15 Hz.
         * @param[in,out] p  the processor
         */
        explicit EnvView(NoctuaryProcessor& p) : m_proc(p) { setInterceptsMouseClicks(false, false); startTimerHz(15); }
        /**
         * @brief The ADSR as a curve with its four times labelled, and the loudest voice's level on it.
         * @param[in,out] g  the graphics
         */
        void paint(juce::Graphics&) override;
        /** @brief Repaints while on screen. */
        void timerCallback() override { if (isShowing()) repaint(); }
        NoctuaryProcessor& m_proc;   ///< the processor
    };
    std::unique_ptr<EnvView> m_envView;   ///< the ENVELOPE + EXPRESSION tab's display
    std::unique_ptr<FilterView> m_filterView;   ///< the display of both filter tabs
    std::unique_ptr<SourceView> m_source1View,   ///< Source 1's display
                                m_source2View,   ///< Source 2's display
                                m_source3View,   ///< Source 3's display
                                m_source4View;   ///< Source 4's display
    /** @} */
    std::unique_ptr<juce::TextButton> m_browseButton;   ///< Browse, in the header
    /**
     * @brief Shows one page and hides the others; the header buttons follow.
     * @param[in] page  0 edit, 1 perform, 2 browse, 3 help
     */
    void setPage(int page);
    /** @brief Opens the gesture / macro mapping table as text in a dialog; the timer applies it when the dialog closes. */
    void showMappingEditor();
    juce::Component::SafePointer<juce::TextEditor> m_mapEditor;   ///< the mapping editor while its dialog lives
    juce::String m_mapText;   ///< its text as the timer last saw it
    bool m_mapOpen = false;   ///< the dialog is up: mirror the text, apply it when it is gone
    std::unique_ptr<juce::ComboBox> m_soundBox;   ///< the sound presets, in the header, one submenu per family
    juce::ComboBox* m_cosmosBox = nullptr;   ///< not owned: its cell in the Cosmos section owns it
    juce::ComboBox* m_morphABox = nullptr;   ///< owned by their cells
    juce::ComboBox* m_morphBBox = nullptr;   ///< the preset chooser of morph slot B
    std::unique_ptr<juce::FileChooser> m_chooser;   ///< the file dialog in flight (record, save, load, tables, clips, impulses, scales)
    juce::Rectangle<int> m_header,     ///< the header, in design coordinates
                         m_helpLine,   ///< the help line's card
                         m_keys;       ///< the keyboard strip
    bool m_sounding[128] = {};   ///< which notes sound, read from the engine every tick for the keyboard strip

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoctuaryEditor)
};
