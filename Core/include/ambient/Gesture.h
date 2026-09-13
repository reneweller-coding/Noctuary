// Noctuary -- gesture layer: a few continuous inputs (hands, head, or anything
// a controller sends) mapped onto parameters with range, smoothing, jitter
// dead-zone and a clutch. Framework-free: the same code runs behind an OpenXR
// hand tracker on the Quest and behind the OSC receiver on the desktop.
//
// Inputs are normalised to 0..1 (tilts and head angles are mapped from -1..1).
// The layer never writes parameters itself; update() hands (ParamId, value) to a
// sink so each host can route it into its own parameter system.
#pragma once
#include "Params.h"
#include <atomic>
#include <cstdint>
#include <cmath>

namespace ambient {

enum class GestureInput : int {
    HandDistance,     // distance between the hands, 0.1 .. 0.8 m
    LeftHeight, RightHeight,     // hand height, 0.9 .. 1.7 m
    LeftForward, RightForward,   // hand reach in front of the head, 0.2 .. 0.7 m
    LeftTilt, RightTilt,         // palm roll, -1 .. 1
    LeftPinch, RightPinch,       // pinch strength 0 .. 1
    HeadYaw, HeadPitch, HeadRoll,   // -1 .. 1 (= -90 .. 90 degrees)
    Custom0, Custom1, Custom2, Custom3, Custom4, Custom5, Custom6, Custom7,
    Count
};
constexpr int kNumGestureInputs = static_cast<int>(GestureInput::Count);
const char* gestureInputName(GestureInput);
bool gestureInputFromName(const char* name, GestureInput& out);

struct GestureMapping {
    GestureInput input = GestureInput::HandDistance;
    ParamId      param = ParamId::MorphPos;
    float        min = 0.0f, max = 1.0f;      // parameter value at input 0 and 1
    float        smoothSeconds = 0.3f;        // one-pole glide of the parameter
    float        deadzone = 0.01f;            // ignore input changes smaller than this
    GestureInput clutch = GestureInput::Count; // Count = always engaged; else engaged while clutch > 0.5
    bool         invert = false;
};

class GestureLayer {
public:
    static constexpr int kMaxMappings = 32;

    GestureLayer();

    // Inputs, any thread.
    void setInput(GestureInput in, float normalised);
    float input(GestureInput in) const { return inputs_[static_cast<int>(in)].load(std::memory_order_relaxed); }
    // Raw poses in metres (OpenXR convention: x right, y up, z back), relative to the stage.
    // Derives distance, heights, reach; tilt and pinch are passed through.
    void setHand(int hand, float x, float y, float z, float pinch, float tilt);
    void setHead(float yawDeg, float pitchDeg, float rollDeg);
    void setCalibration(float heightLow, float heightHigh, float reachNear, float reachFar, float distNear, float distFar);

    // Calibration gesture: for `seconds` the layer watches the raw hand data (hands
    // together and apart, low and high, near and far) and then sets the ranges from
    // the extremes seen, with a small margin. Progress 0..1 for a display.
    void  startCalibration(float seconds);
    bool  calibrating() const { return calibRemaining_ > 0.0f; }
    float calibrationProgress() const { return calibTotal_ > 0.0f ? 1.0f - calibRemaining_ / calibTotal_ : 1.0f; }
    // "hLow hHigh rNear rFar dNear dFar" for persistence.
    int   writeCalibration(char* out, int capacity) const;
    bool  parseCalibration(const char* text);
    float heightLow() const { return hLow_; }  float heightHigh() const { return hHigh_; }
    float distNear() const { return dNear_; }  float distFar() const { return dFar_; }

    // While suspended (a menu is open), clutched mappings hold their values.
    void setSuspended(bool s) { suspended_.store(s, std::memory_order_relaxed); }
    bool suspended() const { return suspended_.load(std::memory_order_relaxed); }
    // Rest zone: share of the calibrated height below which both hands count as resting (0 = off).
    void  setRestZone(float share) { restZone_ = share; }
    float restZone() const { return restZone_; }
    bool  resting() const { return resting_; }

    // Mappings, message thread (the audio thread reads them; keep changes rare).
    int  numMappings() const { return numMappings_.load(std::memory_order_acquire); }
    const GestureMapping& mapping(int i) const { return maps_[i]; }
    // Zero FIRST, so the audio thread stops reading before the table underneath it is rewritten.
    void clearMappings() { numMappings_.store(0, std::memory_order_release); }
    bool addMapping(const GestureMapping& m);
    void setDefaultMappings();
    // Text form, one mapping per line:
    //   input param min max smooth deadzone clutch invert
    // e.g. "HandDistance morph 0 1 0.5 0.01 RightPinch 0"
    bool parseMappings(const char* text);
    int  writeMappings(char* out, int capacity) const;

    // Audio thread, once per block. Calls sink(ParamId, float) for every mapping
    // whose target changed. Returns the number of parameters written.
    template <class Sink>
    int update(double dt, Sink&& sink)
    {
        if (calibRemaining_ > 0.0f) {
            calibRemaining_ -= static_cast<float>(dt);
            if (calibRemaining_ <= 0.0f) { calibRemaining_ = 0.0f; finishCalibration(); }
            return 0;   // nothing moves while calibrating
        }
        // Rest zone: both hands hanging low (below restZone_ of the calibrated height) means
        // "I am not playing" -- nothing is written, so the arms can drop without touching the sound.
        resting_ = restZone_ > 0.0f && handsSeen_ && input(GestureInput::LeftHeight) < restZone_ && input(GestureInput::RightHeight) < restZone_;
        if (resting_) return 0;
        const bool susp = suspended();
        int written = 0;
        const int count = numMappings_.load(std::memory_order_acquire);
        for (int i = 0; i < count; ++i) {
            const GestureMapping& m = maps_[i];
            State& s = state_[i];
            float x = input(m.input);
            const bool engaged = !(susp && m.clutch != GestureInput::Count) && ((m.clutch == GestureInput::Count) || input(m.clutch) > 0.5f);
            if (!s.seen) {   // a mapping only acts once its input has moved; the glide starts from the rest position
                s.seen = true; s.lastInput = x;
                s.value = m.min + (m.max - m.min) * (m.invert ? 1.0f - x : x); s.primed = true;
            }
            if (engaged && std::fabs(x - s.lastInput) >= m.deadzone) { s.lastInput = x; s.hasTarget = true; }
            if (!s.hasTarget) continue;
            float u = s.lastInput;
            if (m.invert) u = 1.0f - u;
            const float target = m.min + (m.max - m.min) * u;
            const float coef = m.smoothSeconds <= 1e-4 ? 1.0f : 1.0f - static_cast<float>(std::exp(-dt / m.smoothSeconds));
            const float next = s.value + (target - s.value) * coef;
            if (std::fabs(next - s.value) > 1e-6f || !s.sent) {
                s.value = next; s.sent = true;
                sink(m.param, next);
                ++written;
            }
        }
        return written;
    }

    uint64_t inputUpdates() const { return updates_.load(std::memory_order_relaxed); }

private:
    struct State { float lastInput = 0.0f, value = 0.0f; bool seen = false, hasTarget = false, primed = false, sent = false; };
    std::atomic<float> inputs_[kNumGestureInputs];
    std::atomic<uint64_t> updates_{ 0 };
    GestureMapping maps_[kMaxMappings];
    State state_[kMaxMappings];
    // Written by the message thread when the mapping text is edited, read by the audio thread
    // every block. Published after the entry it counts has been written, so a half-written
    // mapping is never acted on.
    std::atomic<int> numMappings_ { 0 };
    void finishCalibration();
    float handX_[2] = {}, handY_[2] = {}, handZ_[2] = {};
    float hLow_ = 0.9f, hHigh_ = 1.7f, rNear_ = 0.2f, rFar_ = 0.7f, dNear_ = 0.1f, dFar_ = 0.8f;
    std::atomic<bool> suspended_{ false };
    float restZone_ = 0.08f;
    bool  resting_ = false;
    bool  handsSeen_ = false;   // set once real hand data arrived (setHand); knobs and OSC gestures alone never "rest"
    float calibRemaining_ = 0.0f, calibTotal_ = 0.0f;
    float calMinY_ = 1e9f, calMaxY_ = -1e9f, calMinD_ = 1e9f, calMaxD_ = -1e9f, calMinR_ = 1e9f, calMaxR_ = -1e9f;
};

} // namespace ambient
