// Noctuary -- what the editor's four translation units share: the geometry of the grid, the
// palette shorthands, and the small drawing helpers the displays are built from. Internal to the
// plugin; nothing outside Plugin/ includes it.
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

namespace edt {

// One cell is a knob and its name; the value is drawn inside the knob (see NoctuaryLookAndFeel),
// which is what buys the room for a knob this size in the same wall of controls.
constexpr int kCellW = 68, kCellH = 76, kPad = 11, kTitleH = 22, kGroupTitleH = 26, kHeaderH = 118;
// The editor is laid out once, in this design space, and the whole thing is then scaled to
// whatever size the window has. Dragging the corner is the zoom; the aspect ratio is fixed, so
// nothing ever reflows into a different arrangement -- it only gets bigger or smaller.
constexpr int kMinDesignW = 1400, kMinDesignH = 820;
// A tab bar above a row that pages, and the modulation strip along the bottom; the page height
// follows from these plus the body, so nothing on the main page ever has to scroll.
constexpr int kTabH = 26, kStripH = 300, kDisplayMinW = 300;
const juce::Colour kBg = ui::bg0, kGroupFill = ui::group, kSectionFill = ui::card, kAccent = ui::accent,
                   kText = ui::text, kDim = ui::dim;
const juce::Colour kVoice = ui::voiceCol, kFore = ui::foreCol, kBack = ui::backCol, kCosmos = ui::cosmosCol,
                   kConductor = ui::condCol, kMorph = ui::morphCol, kMaster = ui::masterCol;

// A parameter's raw value by key, for the displays (they read the host's atomics, never the engine's).
inline float rawParam(NoctuaryProcessor& proc, const char* key)
{
    auto* v = proc.apvts.getRawParameterValue(key);
    return v != nullptr ? v->load() : 0.0f;
}

// The filter's response, captured once and then asked at any frequency. Two displays draw it --
// the response curve in the Filter row, and the spectrum strip underneath the panel, where it is
// laid over what is actually coming out. A second copy of this arithmetic would be a second copy
// to keep in step with the voice, so there is one.
struct FilterCurve {
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

    // Linear magnitude at a frequency, the two branches combined as the voice combines them.
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

    float branchFilter(float hz) const
    {
        return fOn ? ambient::VoiceFilter::magnitude(static_cast<ambient::FilterModel>(model), cutoff, res, hz, sr) : 1.0f;
    }

    float branchZ(float hz) const
    {
        if (zUsed <= 0) return 1.0f;
        if (isModal) return modal.magnitudeAt(hz);
        float m = zNorm;
        const float w = juce::MathConstants<float>::twoPi * hz / sr;
        for (int s = 0; s < zUsed; ++s) m *= biquad[s].magnitudeAt(w);
        return m;
    }

    juce::String legend() const
    {
        juce::String s = fOn ? juce::String(ambient::kFilterModelNames[model]) + "   " + juce::String(cutoff, 0) + " Hz"
                             : juce::String("filter off");
        if (zOn) s += "   z: " + juce::String(ambient::kZShapeNames[zShape]) + (!fOn ? "  (alone)" : parallel ? "  (parallel)" : "  (series)");
        return s;
    }

    ambient::ZBiquad biquad[ambient::kZSections];
    ambient::ZModal  modal;
    float sr = 48000.0f, cutoff = 1000.0f, res = 0.0f, zMix = 0.0f, zNorm = 1.0f;
    int   model = 0, zMode = 0, zShape = 0, zUsed = 0;
    bool  fOn = false, zOn = false, parallel = false, isModal = false;
};

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

// Log-frequency axis with a few labelled lines, 20 Hz .. 20 kHz.
inline float xForHz(juce::Rectangle<float> plot, float hz)
{
    const float t = (std::log(juce::jmax(20.0f, hz)) - std::log(20.0f)) / (std::log(20000.0f) - std::log(20.0f));
    return plot.getX() + juce::jlimit(0.0f, 1.0f, t) * plot.getWidth();
}
inline float yForDb(juce::Rectangle<float> plot, float db)
{
    const float t = (db + 36.0f) / 48.0f;   // -36 .. +12 dB
    return plot.getBottom() - juce::jlimit(0.0f, 1.0f, t) * plot.getHeight();
}
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

// The modulation matrix as a table of routes (EditorMatrix.cpp): one row per route, each a
// source, a target, a depth, an optional via source and the unipolar flag. It hands the whole
// matrix back to the engine as the text the old box used, so nothing else learns a new format.
class RouteTable : public juce::Component {
public:
    RouteTable(NoctuaryProcessor& p, std::function<void()> onChanged);
    ~RouteTable() override;
    void pull();                 // read the engine's matrix into the rows
    int  count() const;
    void resized() override;
    struct Row;
private:
    friend struct Row;
    void push();                 // the rows back to the engine
    void changed();
    void addRow();
    void removeRow(int index);
    NoctuaryProcessor& proc;
    std::function<void()> changedCallback;
    std::vector<std::unique_ptr<Row>> rows;
    juce::Component content;
    juce::Viewport port;
    juce::TextButton add, clear;
    juce::Label headSource, headTarget, headDepth, headVia, headUni;   // one per column, placed like the rows
};

// The colour a modulation source is drawn in, everywhere it appears.
juce::Colour sourceColour(ambient::ModSource s);
// The colour a preset family is drawn in on the map and in the list.
juce::Colour familyColour(int f);

} // namespace edt

using namespace edt;
