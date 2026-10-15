/**
 * @file Menu.h
 * @brief Hand menu: a small performance menu driven by the hands alone.
 *
 * @code{.unparsed}
 *   open      hold the LEFT pinch
 *   choose    RIGHT hand height (top item = hand high)
 *   activate  RIGHT pinch while the menu is open
 * @endcode
 * While the menu is open the clutched parameter mappings are suspended, so
 * choosing an item never moves the sound. Framework-free: the host draws the
 * menu and executes the actions (presets, morph, recording, calibration).
 * @author Rene Weller
 */
#pragma once
#include "Gesture.h"

namespace ambient
{

/**
 * @brief The rows of the hand menu, top to bottom, and what activating one asks the host to do.
 *
 * The value of an item is its row: HandMenu::highlighted() is an index into this order, and the
 * right hand's height is mapped onto 0 .. Count - 1 with the hand high at the top row. None is
 * what HandMenu::update() returns in a frame in which nothing was fired.
 */
enum class MenuActionE : int
{
    NONE = -1,   ///< nothing fired this frame
    TOGGLE_MORPH,
    CAPTURE_A,
    CAPTURE_B,
    PRESET_A_PREV,
    PRESET_A_NEXT,
    PRESET_B_PREV,
    PRESET_B_NEXT,
    TOGGLE_MAP,
    TOGGLE_ROUTE,
    TOGGLE_RECORD,
    CALIBRATE,   ///< the eleven rows: morph on/off, capture the live sound as A or B, step A's or B's preset back or forward, map on/off, route play/stop, record, run the hand calibration
    COUNT        ///< how many rows the menu draws
};
constexpr int MenuItems =
    static_cast<int>( MenuActionE::COUNT );   ///< rows of the menu, the range of HandMenu::highlighted()

/**
 * @brief The caption the host draws for a row, in capitals as the menu shows them.
 * @param[in] a  the item
 * @return its label ("MORPH ON/OFF", "A = NOW", ...), "" for None, Count or anything else
 */
inline const char *menuLabel( MenuActionE a )
{
    switch ( a )
    {
    case MenuActionE::TOGGLE_MORPH: return "MORPH ON/OFF";
    case MenuActionE::CAPTURE_A: return "A = NOW";
    case MenuActionE::CAPTURE_B: return "B = NOW";
    case MenuActionE::PRESET_A_PREV: return "A PREV";
    case MenuActionE::PRESET_A_NEXT: return "A NEXT";
    case MenuActionE::PRESET_B_PREV: return "B PREV";
    case MenuActionE::PRESET_B_NEXT: return "B NEXT";
    case MenuActionE::TOGGLE_MAP: return "MAP ON/OFF";
    case MenuActionE::TOGGLE_ROUTE: return "ROUTE PLAY/STOP";
    case MenuActionE::TOGGLE_RECORD: return "RECORD";
    case MenuActionE::CALIBRATE: return "CALIBRATE";
    default: return "";
    }
}

/**
 * @brief The menu's state: open or shut, which row the right hand points at, and whether the
 *        right pinch is armed to fire it.
 *
 * Owned by the host (the Quest app, the OSC desktop), which calls update() once per frame with
 * its GestureLayer, draws the menu from isOpen(), highlighted() and openness(), and carries out
 * the MenuAction it gets back. The menu itself never writes a parameter; all it does to the sound
 * is to suspend the layer's clutched mappings while it is open (or the layer is calibrating), so
 * a hand reaching for a row cannot move anything.
 */
class HandMenu
{
public:
    /**
     * @brief Call once per frame.
     *
     * Returns the activated item, or None. The left pinch opens the menu above 0.6 and shuts it
     * below 0.4, so a trembling pinch does not flicker. While it is open the right hand's height
     * picks the row (hand high = top row), and a right pinch fires that row when it begins while
     * the menu is open, at least a quarter of a second after it opened, and after the right hand
     * has been seen open since the menu opened or last fired -- it must open again before it can
     * fire a second time. The layer's suspension and the eased openness for drawing are updated
     * here as well.
     * @param[in]     dt  seconds since the previous frame
     * @param[in,out] g   the gesture layer: LeftPinch, RightPinch and RightHeight are read, its suspension is written
     * @return the row fired this frame, or MenuAction::None
     */
    MenuActionE update( double dt, GestureLayer &g )
    {
        const float left = g.input( GestureInputE::LEFT_PINCH );
        const float right = g.input( GestureInputE::RIGHT_PINCH );
        const bool  pinchNow = right > 0.5f;
        MenuActionE fired = MenuActionE::NONE;

        if ( !m_open && left > 0.6f )
        {
            m_open = true;
            m_openTime = 0.0;
            m_armed = !pinchNow;
        }
        else if ( m_open && left < 0.4f )
        {
            m_open = false;
        }

        if ( m_open )
        {
            m_openTime += dt;
            const float h = g.input( GestureInputE::RIGHT_HEIGHT );
            m_highlight = clampv( static_cast<int>( ( 1.0f - h ) * MenuItems ), 0, MenuItems - 1 );
            if ( !pinchNow ) m_armed = true;   // the right hand must open before it can select
            if ( pinchNow && !m_lastPinch && m_armed && m_openTime > 0.25 )
            {
                fired = static_cast<MenuActionE>( m_highlight );
                m_armed = false;
            }
        }
        else
        {
            m_highlight = -1;
        }
        m_lastPinch = pinchNow;
        g.setSuspended( m_open || g.calibrating() );
        m_openness +=
            ( static_cast<float>( m_open ? 1.0 : 0.0 ) - m_openness ) * static_cast<float>( std::min( 1.0, dt * 8.0 ) );
        return fired;
    }

    /** @brief Whether the left pinch is holding the menu open. @return true while open */
    bool isOpen() const { return m_open; }
    /** @brief The row the right hand points at. @return 0 .. MenuItems - 1 while open, -1 while shut */
    int highlighted() const { return m_highlight; }
    /**
     * @brief How far the menu has faded in, for drawing.
     * @return 0..1, eased, for drawing
     */
    float openness() const { return m_openness; }

private:
    /** @var bool m_open
     *  @brief the menu is open: the left pinch went past 0.6 and has not dropped under 0.4 since */
    /** @var bool m_lastPinch
     *  @brief the right pinch's state last frame, for the rising edge that fires a row */
    bool
           m_open = false,
           m_lastPinch = false,
           m_armed =
               false;   ///< m_armed: the right hand has been open since the menu opened (or last fired), so its next pinch may fire
    int    m_highlight = -1;    ///< the row under the right hand, -1 while shut
    double m_openTime = 0.0;    ///< seconds the menu has been open; nothing fires in the first quarter second
    float  m_openness = 0.0f;   ///< eased 0..1 towards m_open (about 8 per second), what openness() returns
};

}   // namespace ambient
