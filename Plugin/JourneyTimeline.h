/**
 * @file JourneyTimeline.h
 * @brief The journeys over time (15.10.2026, from Empyrean's Journey page): an overlay with the journey chosen (or
 *        under way) as a band of its steps -- each as wide as it lasts on average, its crossfade a ramp into it, the
 *        step under way lit and filling as its time goes, a click on a step goes there -- and under it the journey being
 *        written, to change by hand: a step's right edge for how long it stays, its ramp for how long it fades in, the
 *        block for its place in the row, a right-click to remove it. "Edit a copy" takes the journey above to change.
 * @author Rene Weller
 */
#pragma once

#include "PluginProcessor.h"

#include <functional>

/** @brief The journeys' timeline (see the file comment). */
class JourneyTimeline final : public juce::Component, private juce::Timer
{
public:
    /**
     * @brief The timeline of @p proc's journeys.
     * @param[in,out] proc     the processor (the player, the journey being written)
     * @param[in]     chosen   the journey chosen in the editor's box, when none runs (false: none)
     * @param[in]     startAt  starts the chosen journey at a step (a click on it while none runs)
     */
    JourneyTimeline( NoctuaryProcessor &proc, std::function<bool( ambient::Journey & )> chosen,
                     std::function<void( int )> startAt );
    /**
     * @brief The bands, the steps, where the journey is.
     * @param[in,out] g  where
     */
    void paint( juce::Graphics &g ) override;
    /** @brief The copy button. */
    void resized() override;
    /**
     * @brief A step clicked (go there), or a stroke on the one being written begins.
     * @param[in] e  the press
     */
    void mouseDown( const juce::MouseEvent &e ) override;
    /**
     * @brief The edge, the ramp or the block follows.
     * @param[in] e  the drag
     */
    void mouseDrag( const juce::MouseEvent &e ) override;
    /**
     * @brief A moved block lands where it was let go.
     * @param[in] e  the release
     */
    void mouseUp( const juce::MouseEvent &e ) override;
    /**
     * @brief The cursor says what a drag would do.
     * @param[in] e  the move
     */
    void mouseMove( const juce::MouseEvent &e ) override;

private:
    /** @brief One step's place in a band: where its crossfade begins, where it stands, where it ends. */
    struct Block
    {
        float m_x0 = 0.0f;   ///< the crossfade's start
        float m_xg = 0.0f;   ///< the step's own start
        float m_x1 = 0.0f;   ///< its end
    };
    /** @brief What a drag on the journey being written does. */
    enum class DragE
    {
        NONE,
        DWELL,
        FADE,
        MOVE
    };
    /** @brief Twice a second while it is seen (ten while a drag is on). */
    void timerCallback() override;
    /**
     * @brief The journey above: the one under way, else the one chosen.
     * @param[out] out  receives it
     * @return false when there is none
     */
    bool upper( ambient::Journey &out ) const;
    /** @brief Whether the journey being written is shown (it has steps and is not merely the one above). @return true then */
    bool hasDraft() const;
    /**
     * @brief A band's rectangle.
     * @param[in] which  0 the journey above, 1 the one being written
     * @return pixels
     */
    juce::Rectangle<float> band( int which ) const;
    /**
     * @brief The blocks of a journey in a band, on its own time scale.
     * @param[in] j  the journey
     * @param[in] r  the band
     * @return them
     */
    static std::vector<Block> blocks( const ambient::Journey &j, juce::Rectangle<float> r );
    /**
     * @brief Draws a journey in a band.
     * @param[in,out] g      where
     * @param[in]     j      the journey
     * @param[in]     r      the band
     * @param[in]     lit    the step lit, -1 none
     * @param[in]     done   how far it has gone, 0 .. 1
     * @param[in]     faint  drawn faint
     */
    void drawBand( juce::Graphics &g, const ambient::Journey &j, juce::Rectangle<float> r, int lit, double done,
                   bool faint ) const;
    /**
     * @brief The block of the one being written under a point.
     * @param[in]  p     the point
     * @param[out] drag  receives what a drag there does
     * @return its index, -1 none
     */
    int draftAt( juce::Point<float> p, DragE &drag ) const;

    NoctuaryProcessor                        &m_proc;      ///< whose journeys
    std::function<bool( ambient::Journey & )> m_chosen;    ///< the journey chosen in the box
    std::function<void( int )>                m_startAt;   ///< starts it at a step
    juce::TextButton     m_copy{ "Edit a copy" };          ///< the journey above into the one being written
    DragE                m_drag = DragE::NONE;             ///< the drag under way
    int                  m_dragStep = -1;                  ///< its step
    float                m_dragX = 0.0f;                   ///< where it began
    double               m_perPixel = 1.0;   ///< seconds a pixel when it began (the scale held while dragging)
    ambient::JourneyStep m_before;           ///< the step as it was when it began
    int                  m_dropAt = -1;      ///< where a moved block would land, -1 nowhere
};

/**
 * @brief The night director's controls (20.11.2026, plan D3; Empyrean's NightPanel the model): how long the night is
 *        (until a wake time from now, or so many hours), which presets it draws from (every pack, a family of packs, one
 *        pack), whether the favourites are preferred, the dice, Make; under them what came of it and its curve -- the
 *        stillness and brightness the night wants (dashed) and the presets chosen (solid). Make writes the night into the
 *        journey being written (the timeline below shows it); Save ... and Play are the player's, nothing starts by
 *        itself.
 */
class NightBar final : public juce::Component
{
public:
    /**
     * @brief The controls for @p proc's night director.
     * @param[in,out] proc  the processor (ambient::composeNight through makeNight)
     */
    explicit NightBar( NoctuaryProcessor &proc );
    /** @brief The lengths offered again (the wake times are counted from now). */
    void refreshLengths();
    /**
     * @brief Makes a night now.
     * @param[in] hours  how long; 0: what the length box says
     * @param[in] seed   the dice; 0: the bar's own
     */
    void make( double hours = 0.0, uint64_t seed = 0 );
    std::function<void()> m_onSave;   ///< Save ...: the editor's "Save journey" dialog
    /**
     * @brief The frame, the curve and the chosen presets.
     * @param[in,out] g  where
     */
    void paint( juce::Graphics &g ) override;
    /** @brief One row of controls, the summary, the curve. */
    void resized() override;

private:
    /** @brief The curve's rectangle. @return pixels */
    juce::Rectangle<float> curveArea() const;
    NoctuaryProcessor  &m_proc;                                  ///< whose director
    juce::ComboBox      m_length;                                ///< until a wake time, or so many hours
    std::vector<double> m_lengthHours;                           ///< the hours behind each item of m_length (id - 1)
    juce::ComboBox      m_choice;                                ///< every pack, a family, a pack
    juce::ToggleButton  m_favourites{ "Prefer my favourites" };  ///< the stars count
    juce::TextButton    m_dice{ "New dice" };                    ///< another seed, and made at once
    juce::TextButton    m_make{ "Make" };                        ///< composes the night
    juce::TextButton    m_save{ "Save ..." };                    ///< writes the journey being written
    juce::TextButton    m_play{ "Play" };                        ///< starts it
    juce::Label         m_summary;                               ///< what came of it
    uint64_t            m_seed = 1;                              ///< the dice
    double              m_startHour = 23.0;                      ///< the clock at the night's start (now, when made)
    ambient::NightPlan  m_plan;                                  ///< the night last made
    double              m_hours = 0.0;                           ///< its length
};

/** @brief The timeline as an overlay over the panel: a title, the timeline, a close button; Esc or a click beside it closes it. */
class JourneyPanel final : public juce::Component
{
public:
    /**
     * @brief The panel of @p proc's journeys.
     * @param[in,out] proc     the processor
     * @param[in]     chosen   the box's journey
     * @param[in]     startAt  starts it at a step
     */
    JourneyPanel( NoctuaryProcessor &proc, std::function<bool( ambient::Journey & )> chosen,
                  std::function<void( int )> startAt );
    /** @brief Opens it over its parent. */
    void open();
    /**
     * @brief Opens it with the night director's controls shown.
     * @param[in] makeHours  when above 0, a night of that many hours is made at once (seed 1; the screenshots)
     */
    void openNight( double makeHours = 0.0 );
    /** @return the night director's controls (the editor gives them its Save dialog) */
    NightBar &nightBar() { return m_night; }
    /**
     * @brief The dimmed window and the panel's frame.
     * @param[in,out] g  where
     */
    void paint( juce::Graphics &g ) override;
    /** @brief The title, the timeline, the close button. */
    void resized() override;
    /**
     * @brief A click beside the panel closes it.
     * @param[in] e  the press
     */
    void mouseDown( const juce::MouseEvent &e ) override;
    /**
     * @brief Esc closes it.
     * @param[in] key  the key
     * @return true when it was Esc
     */
    bool keyPressed( const juce::KeyPress &key ) override;

private:
    /** @brief The panel's rectangle. @return pixels */
    juce::Rectangle<int> panel() const;
    /**
     * @brief Shows or hides the night director's controls.
     * @param[in] on  shown
     */
    void showNight( bool on );
    JourneyTimeline  m_timeline;                     ///< the bands
    NightBar         m_night;                        ///< the night director's controls, hidden until "Night ..."
    juce::TextButton m_nightButton{ "Night ..." };   ///< shows or hides them
    juce::TextButton m_close{ "Close" };             ///< closes the overlay
};
