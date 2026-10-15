/**
 * @file JourneyTimeline.h
 * @brief The journeys over time (15.10.2026, from Empyrean's Journey page): an overlay with the journey chosen (or
 *        under way) as a band of its steps -- each as wide as it lasts on average, its crossfade a ramp into it, the
 *        step under way lit and filling as its time goes, a click on a step goes there -- and under it the journey being
 *        written, to change by hand: a step's right edge for how long it stays, its ramp for how long it fades in, the
 *        block for its place in the row, a right-click to remove it. "Edit a copy" takes the journey above to change.
 */
#pragma once

#include "PluginProcessor.h"

#include <functional>

/** @brief The journeys' timeline (see the file comment). */
class JourneyTimeline final : public juce::Component, private juce::Timer {
public:
    /**
     * @brief The timeline of @p proc's journeys.
     * @param proc     the processor (the player, the journey being written)
     * @param chosen   the journey chosen in the editor's box, when none runs (false: none)
     * @param startAt  starts the chosen journey at a step (a click on it while none runs)
     */
    JourneyTimeline(NoctuaryProcessor& proc, std::function<bool(ambient::Journey&)> chosen, std::function<void(int)> startAt);
    /** @brief The bands, the steps, where the journey is. @param g where */
    void paint(juce::Graphics& g) override;
    /** @brief The copy button. */
    void resized() override;
    /** @brief A step clicked (go there), or a stroke on the one being written begins. @param e the press */
    void mouseDown(const juce::MouseEvent& e) override;
    /** @brief The edge, the ramp or the block follows. @param e the drag */
    void mouseDrag(const juce::MouseEvent& e) override;
    /** @brief A moved block lands where it was let go. @param e the release */
    void mouseUp(const juce::MouseEvent& e) override;
    /** @brief The cursor says what a drag would do. @param e the move */
    void mouseMove(const juce::MouseEvent& e) override;

private:
    /** @brief One step's place in a band: where its crossfade begins, where it stands, where it ends. */
    struct Block {
        float x0 = 0.0f;   ///< the crossfade's start
        float xg = 0.0f;   ///< the step's own start
        float x1 = 0.0f;   ///< its end
    };
    /** @brief What a drag on the journey being written does. */
    enum class Drag { None, Dwell, Fade, Move };
    /** @brief Twice a second while it is seen (ten while a drag is on). */
    void timerCallback() override;
    /** @brief The journey above: the one under way, else the one chosen. @param out receives it @return false when there is none */
    bool upper(ambient::Journey& out) const;
    /** @brief Whether the journey being written is shown (it has steps and is not merely the one above). @return true then */
    bool hasDraft() const;
    /** @brief A band's rectangle. @param which 0 the journey above, 1 the one being written @return pixels */
    juce::Rectangle<float> band(int which) const;
    /** @brief The blocks of a journey in a band, on its own time scale. @param j the journey @param r the band @return them */
    static std::vector<Block> blocks(const ambient::Journey& j, juce::Rectangle<float> r);
    /**
     * @brief Draws a journey in a band.
     * @param g     where
     * @param j     the journey
     * @param r     the band
     * @param lit   the step lit, -1 none
     * @param done  how far it has gone, 0 .. 1
     * @param faint drawn faint
     */
    void drawBand(juce::Graphics& g, const ambient::Journey& j, juce::Rectangle<float> r, int lit, double done, bool faint) const;
    /** @brief The block of the one being written under a point. @param p the point @param drag receives what a drag there does @return its index, -1 none */
    int draftAt(juce::Point<float> p, Drag& drag) const;

    NoctuaryProcessor& proc_;                            ///< whose journeys
    std::function<bool(ambient::Journey&)> chosen_;      ///< the journey chosen in the box
    std::function<void(int)> startAt_;                   ///< starts it at a step
    juce::TextButton copy_{ "Edit a copy" };             ///< the journey above into the one being written
    Drag drag_ = Drag::None;                             ///< the drag under way
    int dragStep_ = -1;                                  ///< its step
    float dragX_ = 0.0f;                                 ///< where it began
    double perPixel_ = 1.0;                              ///< seconds a pixel when it began (the scale held while dragging)
    ambient::JourneyStep before_;                        ///< the step as it was when it began
    int dropAt_ = -1;                                    ///< where a moved block would land, -1 nowhere
};

/** @brief The timeline as an overlay over the panel: a title, the timeline, a close button; Esc or a click beside it closes it. */
class JourneyPanel final : public juce::Component {
public:
    /** @brief The panel of @p proc's journeys. @param proc the processor @param chosen the box's journey @param startAt starts it at a step */
    JourneyPanel(NoctuaryProcessor& proc, std::function<bool(ambient::Journey&)> chosen, std::function<void(int)> startAt);
    /** @brief Opens it over its parent. */
    void open();
    /** @brief The dimmed window and the panel's frame. @param g where */
    void paint(juce::Graphics& g) override;
    /** @brief The title, the timeline, the close button. */
    void resized() override;
    /** @brief A click beside the panel closes it. @param e the press */
    void mouseDown(const juce::MouseEvent& e) override;
    /** @brief Esc closes it. @param key the key @return true when it was Esc */
    bool keyPressed(const juce::KeyPress& key) override;

private:
    /** @brief The panel's rectangle. @return pixels */
    juce::Rectangle<int> panel() const;
    JourneyTimeline timeline_;                   ///< the bands
    juce::TextButton close_{ "Close" };          ///< closes the overlay
};
