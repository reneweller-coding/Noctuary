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
 */
#pragma once
#include "Gesture.h"

namespace ambient {

/**
 * @brief The rows of the hand menu, top to bottom, and what activating one asks the host to do.
 *
 * The value of an item is its row: HandMenu::highlighted() is an index into this order, and the
 * right hand's height is mapped onto 0 .. Count - 1 with the hand high at the top row. None is
 * what HandMenu::update() returns in a frame in which nothing was fired.
 */
enum class MenuAction : int {
    None = -1,   ///< nothing fired this frame
    ToggleMorph, CaptureA, CaptureB, PresetAPrev, PresetANext, PresetBPrev, PresetBNext, ToggleMap, ToggleRoute, ToggleRecord, Calibrate,   ///< the eleven rows: morph on/off, capture the live sound as A or B, step A's or B's preset back or forward, map on/off, route play/stop, record, run the hand calibration
    Count        ///< how many rows the menu draws
};
constexpr int kMenuItems = static_cast<int>(MenuAction::Count);   ///< rows of the menu, the range of HandMenu::highlighted()

/**
 * @brief The caption the host draws for a row, in capitals as the menu shows them.
 * @param a  the item
 * @return   its label ("MORPH ON/OFF", "A = NOW", ...), "" for None, Count or anything else
 */
inline const char* menuLabel(MenuAction a)
{
    switch (a) {
    case MenuAction::ToggleMorph:  return "MORPH ON/OFF";
    case MenuAction::CaptureA:     return "A = NOW";
    case MenuAction::CaptureB:     return "B = NOW";
    case MenuAction::PresetAPrev:  return "A PREV";
    case MenuAction::PresetANext:  return "A NEXT";
    case MenuAction::PresetBPrev:  return "B PREV";
    case MenuAction::PresetBNext:  return "B NEXT";
    case MenuAction::ToggleMap:    return "MAP ON/OFF";
    case MenuAction::ToggleRoute:  return "ROUTE PLAY/STOP";
    case MenuAction::ToggleRecord: return "RECORD";
    case MenuAction::Calibrate:    return "CALIBRATE";
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
class HandMenu {
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
     *
     * @param dt  seconds since the previous frame
     * @param g   the gesture layer: LeftPinch, RightPinch and RightHeight are read, its
     *            suspension is written
     * @return    the row fired this frame, or MenuAction::None
     */
    MenuAction update(double dt, GestureLayer& g)
    {
        const float left = g.input(GestureInput::LeftPinch);
        const float right = g.input(GestureInput::RightPinch);
        const bool pinchNow = right > 0.5f;
        MenuAction fired = MenuAction::None;

        if (!open_ && left > 0.6f) { open_ = true; openTime_ = 0.0; armed_ = !pinchNow; }
        else if (open_ && left < 0.4f) { open_ = false; }

        if (open_) {
            openTime_ += dt;
            const float h = g.input(GestureInput::RightHeight);
            highlight_ = clampv(static_cast<int>((1.0f - h) * kMenuItems), 0, kMenuItems - 1);
            if (!pinchNow) armed_ = true;                    // the right hand must open before it can select
            if (pinchNow && !lastPinch_ && armed_ && openTime_ > 0.25) { fired = static_cast<MenuAction>(highlight_); armed_ = false; }
        } else {
            highlight_ = -1;
        }
        lastPinch_ = pinchNow;
        g.setSuspended(open_ || g.calibrating());
        openness_ += (static_cast<float>(open_ ? 1.0 : 0.0) - openness_) * static_cast<float>(std::min(1.0, dt * 8.0));
        return fired;
    }

    /** @brief Whether the left pinch is holding the menu open. @return true while open */
    bool  isOpen() const { return open_; }
    /** @brief The row the right hand points at. @return 0 .. kMenuItems - 1 while open, -1 while shut */
    int   highlighted() const { return highlight_; }
    /**
     * @brief How far the menu has faded in, for drawing.
     * @return 0..1, eased, for drawing
     */
    float openness() const { return openness_; }

private:
    /** @var bool open_
     *  @brief the menu is open: the left pinch went past 0.6 and has not dropped under 0.4 since */
    /** @var bool lastPinch_
     *  @brief the right pinch's state last frame, for the rising edge that fires a row */
    bool   open_ = false, lastPinch_ = false, armed_ = false;   ///< armed_: the right hand has been open since the menu opened (or last fired), so its next pinch may fire
    int    highlight_ = -1;      ///< the row under the right hand, -1 while shut
    double openTime_ = 0.0;      ///< seconds the menu has been open; nothing fires in the first quarter second
    float  openness_ = 0.0f;     ///< eased 0..1 towards open_ (about 8 per second), what openness() returns
};

} // namespace ambient
