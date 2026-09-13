// Noctuary -- hand menu: a small performance menu driven by the hands alone.
//   open      hold the LEFT pinch
//   choose    RIGHT hand height (top item = hand high)
//   activate  RIGHT pinch while the menu is open
// While the menu is open the clutched parameter mappings are suspended, so
// choosing an item never moves the sound. Framework-free: the host draws the
// menu and executes the actions (presets, morph, recording, calibration).
#pragma once
#include "Gesture.h"

namespace ambient {

enum class MenuAction : int {
    None = -1,
    ToggleMorph, CaptureA, CaptureB, PresetAPrev, PresetANext, PresetBPrev, PresetBNext, ToggleMap, ToggleRoute, ToggleRecord, Calibrate,
    Count
};
constexpr int kMenuItems = static_cast<int>(MenuAction::Count);

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

class HandMenu {
public:
    // Call once per frame. Returns the activated item, or None.
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

    bool  isOpen() const { return open_; }
    int   highlighted() const { return highlight_; }
    float openness() const { return openness_; }   // 0..1, eased, for drawing

private:
    bool   open_ = false, lastPinch_ = false, armed_ = false;
    int    highlight_ = -1;
    double openTime_ = 0.0;
    float  openness_ = 0.0f;
};

} // namespace ambient
