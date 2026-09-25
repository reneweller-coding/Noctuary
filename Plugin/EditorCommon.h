/**
 * @file EditorCommon.h
 * @brief What the editor's four translation units share: the geometry of the grid, the
 *        palette shorthands, and the small drawing helpers the displays are built from.
 *
 * Internal to the
 * plugin; nothing outside Plugin/ includes it.
 *
 * PluginEditor.cpp lays the panel out in the design space measured here and scales it; the
 * EditorViews.cpp displays, the spectrum strip (EditorSpectrum.cpp), the modulation matrix
 * (EditorMatrix.cpp) and the browser (EditorBrowse.cpp) draw with the frame, the axes and the
 * filter curve below. Everything lives in namespace edt, which the last line opens for the whole
 * editor, so the .cpp files write kPad and displayFrame without a prefix.
 */
#pragma once
#include "PluginEditor.h"
#include "NoctuaryLookAndFeel.h"
#include "ambient/Params.h"
#include "ambient/Help.h"
#include "ambient/Presets.h"
#include "ambient/PresetMeta.h"
#include "ambient/PresetMap.h"
#include "ambient/Route.h"
#include "ambient/Filter.h"
#include <cstdlib>

/** @brief The editor's shared geometry, palette and drawing helpers (Plugin/ only). */
namespace edt {

/**
 * @brief One cell is a knob and its name; the value is drawn inside the knob (see NoctuaryLookAndFeel),
 *        which is what buys the room for a knob this size in the same wall of controls.
 *
 * kCellW is the width of one knob cell in design pixels.
 */
constexpr int kCellW = 68, kCellH = 76, kPad = 11, kTitleH = 22, kGroupTitleH = 26, kHeaderH = 118;   ///< kHeaderH: the header band across the top (title, help line, output view)
/** @var int edt::kCellH
 *  @brief the height of one knob cell: the knob and its name below */
/** @var int edt::kPad
 *  @brief the gap between cells, cards and groups */
/** @var int edt::kTitleH
 *  @brief the height of a section card's title line */
/** @var int edt::kGroupTitleH
 *  @brief the height of a group panel's title line */
/**
 * @brief The editor is laid out once, in this design space, and the whole thing is then scaled to
 *        whatever size the window has.
 *
 * Dragging the corner is the zoom; the aspect ratio is fixed, so
 * nothing ever reflows into a different arrangement -- it only gets bigger or smaller.
 * kMinDesignW is the design width; the height grows with the body when the rows need it.
 */
constexpr int kMinDesignW = 1400, kMinDesignH = 820;   ///< kMinDesignH: the least design height; header + body + strip may make it taller
/**
 * @brief A tab bar above a row that pages, and the modulation strip along the bottom; the page height
 *        follows from these plus the body, so nothing on the main page ever has to scroll.
 *
 * kTabH is the height of the tab bar over a paging row.
 */
constexpr int kTabH = 26, kStripH = 300, kDisplayMinW = 300;   ///< kDisplayMinW: the least width a page's display gets beside its knobs
/** @var int edt::kStripH
 *  @brief the height of the modulation strip along the bottom */
/** @brief The window ground (ui::bg0); the other five follow the same shorthand rule. */
const juce::Colour kBg = ui::bg0, kGroupFill = ui::group, kSectionFill = ui::card, kAccent = ui::accent,
                   kText = ui::text, kDim = ui::dim;   ///< kDim: secondary text (ui::dim)
/** @var const juce::Colour edt::kGroupFill
 *  @brief the group panel behind the cards (ui::group) */
/** @var const juce::Colour edt::kSectionFill
 *  @brief a section card (ui::card) */
/** @var const juce::Colour edt::kAccent
 *  @brief the instrument's own colour (ui::accent) */
/** @var const juce::Colour edt::kText
 *  @brief primary text (ui::text) */
/** @brief The Voice group's hue (ui::voiceCol); one shorthand per signal group follows. */
const juce::Colour kVoice = ui::voiceCol, kFore = ui::foreCol, kBack = ui::backCol, kCosmos = ui::cosmosCol,
                   kConductor = ui::condCol, kMorph = ui::morphCol, kMaster = ui::masterCol;   ///< kMaster: the master section (ui::masterCol)
/** @var const juce::Colour edt::kFore
 *  @brief the near (foreground) layer (ui::foreCol) */
/** @var const juce::Colour edt::kBack
 *  @brief the background layer (ui::backCol) */
/** @var const juce::Colour edt::kCosmos
 *  @brief the Cosmos chain (ui::cosmosCol) */
/** @var const juce::Colour edt::kConductor
 *  @brief the conductor (ui::condCol) */
/** @var const juce::Colour edt::kMorph
 *  @brief Morph (ui::morphCol) */

/**
 * @brief A parameter's raw value by key, for the displays (they read the host's atomics, never the engine's).
 * @param proc  the processor whose parameter tree is read
 * @param key   the parameter's key (ambient::ParamDesc::key)
 * @return the raw value, or 0 for a key the tree does not have
 */
inline float rawParam(NoctuaryProcessor& proc, const char* key)
{
    auto* v = proc.apvts.getRawParameterValue(key);
    return v != nullptr ? v->load() : 0.0f;
}

/**
 * @brief The filter's response, captured once and then asked at any frequency.
 *
 * Two displays draw it --
 * the response curve in the Filter row, and the spectrum strip underneath the panel, where it is
 * laid over what is actually coming out. A second copy of this arithmetic would be a second copy
 * to keep in step with the voice, so there is one.
 */
struct FilterCurve {
    /**
     * @brief Reads the filter's and the Z-plane's parameters from the tree and builds the Z frame
     *        the engine would build at this point (a biquad cascade, or the modal bank in Modal).
     * @param proc        the processor whose parameters are read
     * @param sampleRate  the rate the response is computed at, in Hz
     */
    void capture(NoctuaryProcessor& proc, float sampleRate)
    {
        sr = sampleRate;
        cutoff = rawParam(proc, "cutoff");
        res    = rawParam(proc, "resonance");
        model  = juce::jlimit(0, ambient::kNumFilterModels - 1, static_cast<int>(std::lround(rawParam(proc, "filter_model"))));
        zMode  = static_cast<int>(std::lround(rawParam(proc, "z_mode")));
        zShape = juce::jlimit(0, ambient::kZShapes - 1, static_cast<int>(std::lround(rawParam(proc, "z_shape"))));
        zMix   = rawParam(proc, "z_mix");
        fOn    = rawParam(proc, "filter_on") >= 0.5f && zMode != 2;
        zOn    = zMode != 0;
        parallel = std::lround(rawParam(proc, "z_route")) == 1;
        isModal  = zMode == 3;
        zUsed = 0;
        zNorm = 1.0f;
        if (zOn) {
            // The frame the engine would build at this point, read the same way it reads it: a
            // cascade of biquads in Series and Replace, a parallel bank of resonators in Modal.
            const ambient::ZFrame frame = ambient::zInterpolate(zShape, rawParam(proc, "z_x"),
                                                                rawParam(proc, "z_y"), rawParam(proc, "z_z"));
            if (isModal) {
                modal.prepare(sr);
                modal.set(frame, rawParam(proc, "z_decay"), rawParam(proc, "z_damp"));
                zUsed = modal.used();
            } else {
                zNorm = ambient::zBuildCascade(frame, biquad, sr);
                zUsed = frame.used;
            }
        }
    }

    /**
     * @brief Linear magnitude at a frequency, the two branches combined as the voice combines them.
     * @param hz  the frequency in Hz
     * @return the linear gain: filter times Z in series, a mix of the two in parallel, 1 with both off
     */
    float magnitude(float hz) const
    {
        const float hs = fOn ? ambient::VoiceFilter::magnitude(static_cast<ambient::FilterModel>(model), cutoff, res, hz, sr) : 1.0f;
        float hzm = 1.0f;
        if (zUsed > 0) {
            if (isModal) hzm = modal.magnitudeAt(hz);        // the modes add, they do not multiply
            else {
                hzm = zNorm;
                const float w = juce::MathConstants<float>::twoPi * hz / sr;
                for (int s = 0; s < zUsed; ++s) hzm *= biquad[s].magnitudeAt(w);
            }
        }
        if (fOn && zOn) return parallel ? (1.0f - zMix) * hs + zMix * hzm : hs * ((1.0f - zMix) + zMix * hzm);
        if (zOn) return (1.0f - zMix) + zMix * hzm;
        return hs;
    }

    /**
     * @brief The voice filter alone, for the response display's first curve.
     * @param hz  the frequency in Hz
     * @return its linear gain, 1 while the filter is off
     */
    float branchFilter(float hz) const
    {
        return fOn ? ambient::VoiceFilter::magnitude(static_cast<ambient::FilterModel>(model), cutoff, res, hz, sr) : 1.0f;
    }

    /**
     * @brief The Z-plane alone, for the response display's second curve.
     * @param hz  the frequency in Hz
     * @return its linear gain (the cascade with its normalisation, or the modal bank), 1 with no sections in use
     */
    float branchZ(float hz) const
    {
        if (zUsed <= 0) return 1.0f;
        if (isModal) return modal.magnitudeAt(hz);
        float m = zNorm;
        const float w = juce::MathConstants<float>::twoPi * hz / sr;
        for (int s = 0; s < zUsed; ++s) m *= biquad[s].magnitudeAt(w);
        return m;
    }

    /**
     * @brief The line the displays print over the curve.
     * @return the filter model and cutoff (or "filter off"), and the Z shape with how it is routed
     */
    juce::String legend() const
    {
        juce::String s = fOn ? juce::String(ambient::kFilterModelNames[model]) + "   " + juce::String(cutoff, 0) + " Hz"
                             : juce::String("filter off");
        if (zOn) s += "   z: " + juce::String(ambient::kZShapeNames[zShape]) + (!fOn ? "  (alone)" : parallel ? "  (parallel)" : "  (series)");
        return s;
    }

    ambient::ZBiquad biquad[ambient::kZSections];   ///< the Z cascade as capture() built it (Series and Replace)
    ambient::ZModal  modal;                         ///< the modal bank as capture() built it (Modal)
    /** @brief The sample rate the response is computed at, in Hz. */
    float sr = 48000.0f, cutoff = 1000.0f, res = 0.0f, zMix = 0.0f, zNorm = 1.0f;   ///< zNorm: the cascade's normalisation from zBuildCascade
    /** @var float edt::FilterCurve::cutoff
     *  @brief the voice filter's cutoff in Hz */
    /** @var float edt::FilterCurve::res
     *  @brief the voice filter's resonance, 0 .. 1 */
    /** @var float edt::FilterCurve::zMix
     *  @brief the Z-plane's mix, 0 .. 1 */
    /** @brief The voice filter model (ambient::FilterModel as an index). */
    int   model = 0, zMode = 0, zShape = 0, zUsed = 0;   ///< zUsed: sections (or modes) in use, 0 for none
    /** @var int edt::FilterCurve::zMode
     *  @brief the Z-plane mode: 0 off, 1 series, 2 replace, 3 modal */
    /** @var int edt::FilterCurve::zShape
     *  @brief the Z shape index, 0 .. ambient::kZShapes - 1 */
    /** @brief Whether the voice filter is in the path (on, and not replaced by Z). */
    bool  fOn = false, zOn = false, parallel = false, isModal = false;   ///< isModal: the Z-plane runs as the modal bank
    /** @var bool edt::FilterCurve::zOn
     *  @brief whether the Z-plane is on at all */
    /** @var bool edt::FilterCurve::parallel
     *  @brief whether Z runs beside the filter (z_route 1) rather than after it */
};

/**
 * @brief The rounded, translucent frame every display sits in, with its title in the corner.
 * @param g      the graphics context
 * @param r      the display's bounds
 * @param title  drawn small in the top left, in the group's hue
 * @param c      the group's hue (the title's colour)
 */
inline void displayFrame(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title, juce::Colour c)
{
    g.setColour(ui::bg0.withAlpha(0.55f));
    g.fillRoundedRectangle(r.toFloat(), 6.0f);
    g.setColour(ui::cardEdge);
    g.drawRoundedRectangle(r.toFloat().reduced(0.5f), 6.0f, 1.0f);
    g.setColour(c.withAlpha(0.8f));
    g.setFont(ui::title(10.5f));
    g.drawText(title, r.reduced(9, 5), juce::Justification::topLeft, false);
}

/**
 * @name Log-frequency axis with a few labelled lines, 20 Hz .. 20 kHz.
 * @{ */
/**
 * @brief The x of a frequency on the log axis.
 * @param plot  the plot area
 * @param hz    the frequency in Hz, clamped to 20 .. 20000
 * @return the x in the plot's coordinates
 */
inline float xForHz(juce::Rectangle<float> plot, float hz)
{
    const float t = (std::log(juce::jmax(20.0f, hz)) - std::log(20.0f)) / (std::log(20000.0f) - std::log(20.0f));
    return plot.getX() + juce::jlimit(0.0f, 1.0f, t) * plot.getWidth();
}
/**
 * @brief The y of a level on the linear dB axis, -36 dB at the bottom and +12 dB at the top.
 * @param plot  the plot area
 * @param db    the level in dB, clamped to the axis
 * @return the y in the plot's coordinates
 */
inline float yForDb(juce::Rectangle<float> plot, float db)
{
    const float t = (db + 36.0f) / 48.0f;   // -36 .. +12 dB
    return plot.getBottom() - juce::jlimit(0.0f, 1.0f, t) * plot.getHeight();
}
/**
 * @brief Draws the grid: faint lines at the decades' 1-2-5 frequencies, the 0 dB line, and the
 *        labels 100, 1k and 10k along the bottom.
 * @param g     the graphics context
 * @param plot  the plot area
 */
inline void drawAxes(juce::Graphics& g, juce::Rectangle<float> plot)
{
    g.setColour(ui::track.withAlpha(0.55f));
    for (float hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
        g.drawVerticalLine(juce::roundToInt(xForHz(plot, hz)), plot.getY(), plot.getBottom());
    g.setColour(ui::track.withAlpha(0.9f));
    g.drawHorizontalLine(juce::roundToInt(yForDb(plot, 0.0f)), plot.getX(), plot.getRight());
    g.setColour(ui::faint);
    g.setFont(ui::body(9.0f));
    for (float hz : { 100.0f, 1000.0f, 10000.0f })
        g.drawText(hz >= 1000.0f ? juce::String(hz / 1000.0f, 0) + "k" : juce::String(hz, 0),
                   juce::roundToInt(xForHz(plot, hz)) - 14, juce::roundToInt(plot.getBottom()) - 11, 28, 10,
                   juce::Justification::centred, false);
}
/** @} */

/**
 * @brief The modulation matrix as a table of routes (EditorMatrix.cpp): one row per route, each a
 *        source, a target, a depth, an optional via source and the unipolar flag.
 *
 * It hands the whole
 * matrix back to the engine as the text the old box used, so nothing else learns a new format.
 */
class RouteTable : public juce::Component {
public:
    /**
     * @brief Builds the table: the header labels, the scrolling content, the add and clear buttons,
     *        and pulls the engine's matrix.
     * @param p          the processor whose engine holds the matrix
     * @param onChanged  called after every edit the table pushed to the engine
     */
    RouteTable(NoctuaryProcessor& p, std::function<void()> onChanged);
    ~RouteTable() override;
    void pull();                 ///< read the engine's matrix into the rows
    /** @return how many routes the table holds */
    int  count() const;
    /** @brief Lays out the header, the viewport and the buttons; the rows follow the content's width. */
    void resized() override;
    struct Row;
private:
    friend struct Row;
    void push();                 ///< the rows back to the engine
    /** @brief A row was edited: push() and tell the owner. */
    void changed();
    /** @brief Appends an empty route and pushes. */
    void addRow();
    /**
     * @brief Removes one route and pushes.
     * @param index  the row, 0-based
     */
    void removeRow(int index);
    NoctuaryProcessor& proc;   ///< the processor whose engine the matrix lives in
    std::function<void()> changedCallback;   ///< the owner's notification after a push
    std::vector<std::unique_ptr<Row>> rows;   ///< the routes, one component each
    juce::Component content;   ///< holds the rows, scrolled by port
    juce::Viewport port;       ///< the scrolling view over content
    juce::TextButton add, clear;   ///< clear: empties the matrix
    /** @var juce::TextButton edt::RouteTable::add
     *  @brief appends a route */
    juce::Label headSource, headTarget, headDepth, headVia, headUni;   ///< one per column, placed like the rows
    /** @var juce::Label edt::RouteTable::headSource
     *  @brief the header of the source column */
    /** @var juce::Label edt::RouteTable::headTarget
     *  @brief the header of the target column */
    /** @var juce::Label edt::RouteTable::headDepth
     *  @brief the header of the depth column */
    /** @var juce::Label edt::RouteTable::headVia
     *  @brief the header of the via column */
};

/**
 * @brief The colour a modulation source is drawn in, everywhere it appears.
 * @param s  the source
 * @return its colour (EditorModStrip.cpp)
 */
juce::Colour sourceColour(ambient::ModSource s);
/**
 * @brief The colour a preset family is drawn in on the map and in the list.
 * @param f  the family index; the built-in families have fixed colours, later ones a golden-angle hue
 * @return its colour (EditorBrowse.cpp)
 */
juce::Colour familyColour(int f);

} // namespace edt

using namespace edt;
