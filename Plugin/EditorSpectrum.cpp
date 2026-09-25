/**
 * @file EditorSpectrum.cpp
 * @brief The spectrum strip along the bottom of the left column.
 *
 * The header carries a small spectrum already, at 1024 points and 96 bands: enough to see that
 * something is loud, not enough to see what it is. This one has the width of the whole left
 * column, and the point of the extra room is resolution rather than size. A drone is stationary
 * for seconds at a time, so a long window costs nothing: 16384 samples is 2.9 Hz at 48 kHz, and
 * the partials of a 40 Hz fundamental land in separate bands instead of in one hump. Measured
 * against the 4096 the tap used to hold: the trough between a 40 Hz tone and its octave is 14 dB
 * down at 4096 and 58 dB down at 16384. That is the difference that matters for an instrument
 * tuned in whole-number ratios -- you can see the fifth sitting exactly on the third partial, and
 * see it come apart when Purity Drift loosens the tuning.
 *
 * Four things are drawn over each other, and they answer different questions.
 *   * the bands -- what is coming out right now, fast up and slow down, like a meter
 *   * the trace -- the loudest each band has been recently, falling about a decibel a second, so
 *     a partial that has just faded is still on the screen: the shape of the piece, not the frame
 *   * the filter -- the response from the voice's own arithmetic on the same decibel axis, so a
 *     resonance sitting between two partials is visible as exactly that
 *   * the ticks -- the fundamental of every sounding note, so a line can be tied to a voice
 */
#include "EditorCommon.h"

using namespace ambient;

NoctuaryEditor::SpectrumView::SpectrumView(NoctuaryProcessor& p)
    : proc(p), re(kN), im(kN), window(kN), fft(std::make_unique<ambient::Fft>(kN)),
      band(kBands, kFloorDb), hold(kBands, kFloorDb)
{
    // Hann. A drone's partials sit close together and a rectangular window's skirts would fill
    // the gaps between them with the leakage of the loudest one, which is the opposite of what
    // the long window was for.
    for (int i = 0; i < kN; ++i)
        window[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi * i / kN);
    startTimerHz(12);
}

void NoctuaryEditor::SpectrumView::timerCallback()
{
    if (!isShowing()) return;
    proc.engine().outputTap(re.data(), kN);
    float peak = 0.0f;
    for (int i = 0; i < kN; ++i) {
        peak = juce::jmax(peak, std::fabs(re[static_cast<size_t>(i)]));
        re[static_cast<size_t>(i)] *= window[static_cast<size_t>(i)];
        im[static_cast<size_t>(i)] = 0.0f;
    }
    peakDb = 20.0f * std::log10(peak + 1.0e-6f);
    fft->transform(re.data(), im.data(), false);

    const float sr = static_cast<float>(proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0);
    const float span = std::log(kHiHz / kLoHz);
    // Scaled so a full-scale sine reads 0 dB on the axis: the transform is unscaled and the Hann
    // window halves a bin's amplitude. Without this the numbers beside the grid mean nothing, and
    // a decibel axis whose numbers mean nothing is a decoration.
    const float norm = 4.0f / (static_cast<float>(kN) * static_cast<float>(kN));
    for (int b = 0; b < kBands; ++b) {
        const float f0 = kLoHz * std::exp(span * static_cast<float>(b) / kBands);
        const float f1 = kLoHz * std::exp(span * static_cast<float>(b + 1) / kBands);
        const int k0 = juce::jmax(1, static_cast<int>(f0 / sr * kN));
        const int k1 = juce::jmax(k0 + 1, static_cast<int>(f1 / sr * kN));
        float p = 0.0f;
        for (int k = k0; k < juce::jmin(k1, kN / 2); ++k)
            p = juce::jmax(p, re[static_cast<size_t>(k)] * re[static_cast<size_t>(k)] + im[static_cast<size_t>(k)] * im[static_cast<size_t>(k)]);
        const float db = 10.0f * std::log10(p * norm + 1.0e-13f);
        float& v = band[static_cast<size_t>(b)];
        v = db > v ? db : v + (db - v) * 0.25f;
        float& h = hold[static_cast<size_t>(b)];
        // Falls about a decibel a second at twelve frames: slow enough that a partial which
        // sounded a few seconds ago is still there, quick enough to follow a chord change.
        h = v > h ? v : juce::jmax(kFloorDb, h - 0.09f);
    }
    repaint();
}

void NoctuaryEditor::SpectrumView::mouseMove(const juce::MouseEvent& e)
{
    hoverX = e.x;
    repaint();
}

void NoctuaryEditor::SpectrumView::mouseExit(const juce::MouseEvent&)
{
    hoverX = -1;
    repaint();
}

void NoctuaryEditor::SpectrumView::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds();
    displayFrame(g, r, "OUTPUT SPECTRUM", ui::voiceCol);
    // A gutter at the left for the decibel numbers: they used to be drawn on top of the
    // spectrum, where the one thing they label is the thing they cover.
    const auto plot = r.toFloat().reduced(10.0f, 8.0f).withTrimmedTop(14.0f)
                       .withTrimmedBottom(11.0f).withTrimmedLeft(static_cast<float>(kLabelGutter));
    if (plot.getWidth() < 40.0f || plot.getHeight() < 20.0f) return;
    const float span = std::log(kHiHz / kLoHz);
    auto xOf = [&](float hz) { return plot.getX() + plot.getWidth() * std::log(juce::jmax(hz, kLoHz) / kLoHz) / span; };
    auto yOf = [&](float db) {
        const float t = juce::jlimit(0.0f, 1.0f, (db - kFloorDb) / (kTopDb - kFloorDb));
        return plot.getBottom() - t * plot.getHeight();
    };

    // The grid, faint: it is for reading off, not for looking at.
    g.setFont(ui::body(9.0f));
    for (float hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f }) {
        const float x = xOf(hz);
        const bool decade = hz == 100.0f || hz == 1000.0f || hz == 10000.0f;
        g.setColour(ui::track.withAlpha(decade ? 0.55f : 0.28f));
        g.drawVerticalLine(juce::roundToInt(x), plot.getY(), plot.getBottom());
        g.setColour(ui::faint);
        g.drawText(hz >= 1000.0f ? juce::String(hz / 1000.0f, 0) + "k" : juce::String(hz, 0),
                   x - 18.0f, plot.getBottom() + 1.0f, 36.0f, 10.0f, juce::Justification::centred, false);
    }
    for (float db : { 0.0f, -20.0f, -40.0f, -60.0f, -80.0f }) {
        g.setColour(ui::track.withAlpha(db == 0.0f ? 0.5f : 0.25f));
        g.drawHorizontalLine(juce::roundToInt(yOf(db)), plot.getX(), plot.getRight());
        g.setColour(ui::faint);
        g.drawText(juce::String(static_cast<int>(db)), plot.getX() - kLabelGutter, yOf(db) - 5.0f,
                   kLabelGutter - 3.0f, 10.0f, juce::Justification::right, false);
    }

    // The fundamental of everything sounding, as a tick along the bottom edge.
    bool sounding[128] = {};
    proc.engine().soundingNotes(sounding);
    g.setColour(ui::condCol.withAlpha(0.55f));
    for (int n = 0; n < 128; ++n) {
        if (!sounding[n]) continue;
        const float hz = static_cast<float>(proc.engine().frequencyOf(n));
        if (hz < kLoHz || hz > kHiHz) continue;
        g.drawVerticalLine(juce::roundToInt(xOf(hz)), plot.getBottom() - 6.0f, plot.getBottom());
    }

    const float bw = plot.getWidth() / static_cast<float>(kBands);
    // The trace first, so the live bands stand in front of it.
    juce::Path trace;
    for (int b = 0; b < kBands; ++b) {
        const float x = plot.getX() + (b + 0.5f) * bw, y = yOf(hold[static_cast<size_t>(b)]);
        if (b == 0) trace.startNewSubPath(x, y);
        else        trace.lineTo(x, y);
    }
    g.setColour(ui::dim.withAlpha(0.45f));
    g.strokePath(trace, juce::PathStrokeType(1.0f));

    for (int b = 0; b < kBands; ++b) {
        const float db = band[static_cast<size_t>(b)];
        if (db <= kFloorDb + 0.5f) continue;
        const float y = yOf(db);
        const float t = juce::jlimit(0.0f, 1.0f, (db - kFloorDb) / (kTopDb - kFloorDb));
        g.setColour(ui::accent.withAlpha(0.28f + 0.55f * t));
        g.fillRect(plot.getX() + b * bw, y, juce::jmax(1.0f, bw - 0.4f), plot.getBottom() - y);
    }

    // The filter, on the same axis. Six decibels down from the top, so a flat response is a line
    // across the upper part of the picture and a resonance rises towards 0 rather than off it.
    FilterCurve fc;
    fc.capture(proc, static_cast<float>(juce::jmax(8000.0, proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0)));
    if (fc.fOn || fc.zOn) {
        juce::Path curve;
        const int steps = juce::jmax(96, static_cast<int>(plot.getWidth() / 2.0f));
        for (int i = 0; i <= steps; ++i) {
            const float hz = kLoHz * std::exp(span * static_cast<float>(i) / steps);
            // Its own decibels, on the same axis: flat is the 0 dB line and a resonance rises
            // above it, which is how a filter plot is read anywhere else.
            const float db = 20.0f * std::log10(juce::jmax(fc.magnitude(hz), 1.0e-5f));
            const float y = yOf(juce::jlimit(kFloorDb, kTopDb, db));
            if (i == 0) curve.startNewSubPath(xOf(hz), y);
            else        curve.lineTo(xOf(hz), y);
        }
        g.setColour(ui::bg0.withAlpha(0.5f));
        g.strokePath(curve, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved));
        g.setColour(ui::text.withAlpha(0.8f));
        g.strokePath(curve, juce::PathStrokeType(1.3f, juce::PathStrokeType::curved));
    }

    // Under the pointer: the frequency, the note nearest it, and what that band is reading.
    g.setFont(ui::body(9.5f));
    if (hoverX > 0 && plot.contains(static_cast<float>(hoverX), plot.getCentreY())) {
        const float t = (hoverX - plot.getX()) / plot.getWidth();
        const float hz = kLoHz * std::exp(span * t);
        const int b = juce::jlimit(0, kBands - 1, static_cast<int>(t * kBands));
        g.setColour(ui::text.withAlpha(0.35f));
        g.drawVerticalLine(hoverX, plot.getY(), plot.getBottom());
        const int midi = juce::jlimit(0, 127, static_cast<int>(std::lround(69.0 + 12.0 * std::log2(hz / 440.0))));
        g.setColour(ui::text);
        g.drawText(juce::String(hz, hz < 1000.0f ? 1 : 0) + " Hz   " + juce::MidiMessage::getMidiNoteName(midi, true, true, 4)
                       + "   " + juce::String(band[static_cast<size_t>(b)], 1) + " dB",
                   r.reduced(9, 4), juce::Justification::topRight, false);
    } else {
        g.setColour(peakDb > -0.5f ? juce::Colour(0xffe06060) : ui::dim);
        g.drawText(fc.legend() + "      " + juce::String(peakDb, 1) + " dB peak",
                   r.reduced(9, 4), juce::Justification::topRight, false);
    }
}
