/**
 * @file Gesture.cpp
 * @brief The gesture layer off the audio thread: names, poses, calibration and the mapping table.
 *
 * update(), the per-block part that runs on the audio thread, is a template in Gesture.h. What
 * stands here is everything that runs on the message thread or from the tracker's callback: the
 * input names and their parser, the raw hand and head poses turned into normalised inputs (with the
 * calibration ranges applied and the extremes collected while a calibration runs), the calibration
 * gesture and its text form, the default vocabulary of mappings and the eight macros, and the
 * mapping text format the settings file keeps. Nothing here allocates, and every change to the
 * mapping table publishes its count last, so the audio thread never acts on a half-written entry.
 */
#include "ambient/Gesture.h"
#include <cmath>
#include "ambient/Dsp.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace ambient {

namespace {
/** @brief The inputs' names in GestureInput order: what the mapping file and the OSC receiver use. */
const char* const kInputNames[kNumGestureInputs] = {
    "HandDistance", "LeftHeight", "RightHeight", "LeftForward", "RightForward",
    "LeftTilt", "RightTilt", "LeftPinch", "RightPinch", "HeadYaw", "HeadPitch", "HeadRoll",
    "Custom0", "Custom1", "Custom2", "Custom3", "Custom4", "Custom5", "Custom6", "Custom7",
};
/**
 * @brief A raw value's place in a calibrated range, 0 .. 1.
 *
 * A calibration that collected no range at all leaves lo == hi, and the division then makes a
 * NaN -- which this function used to hand on, straight into the host's parameters, because a
 * comparison against NaN is false and clampv lets it through. An empty range means nothing was
 * measured: the input reads as the bottom of it.
 *
 * @param v   the raw value (a height, a reach or a distance in metres)
 * @param lo  the value that reads as 0
 * @param hi  the value that reads as 1
 * @return    (v - lo) / (hi - lo) clamped to 0 .. 1; 0 when the range is empty
 */
float norm01(float v, float lo, float hi)
{
    const float span = hi - lo;
    if (!(std::fabs(span) > 1.0e-9f)) return 0.0f;
    return clampv((v - lo) / span, 0.0f, 1.0f);
}
}

const char* gestureInputName(GestureInput in)
{
    const int i = static_cast<int>(in);
    return (i >= 0 && i < kNumGestureInputs) ? kInputNames[i] : "none";
}

bool gestureInputFromName(const char* name, GestureInput& out)
{
    if (name == nullptr) return false;
    for (int i = 0; i < kNumGestureInputs; ++i)
        if (std::strcmp(name, kInputNames[i]) == 0) { out = static_cast<GestureInput>(i); return true; }
    if (std::strcmp(name, "none") == 0 || std::strcmp(name, "-") == 0) { out = GestureInput::Count; return true; }
    return false;
}

GestureLayer::GestureLayer()
{
    for (auto& i : inputs_) i.store(0.0f, std::memory_order_relaxed);
    setDefaultMappings();
}

void GestureLayer::setInput(GestureInput in, float normalised)
{
    const int i = static_cast<int>(in);
    if (i < 0 || i >= kNumGestureInputs) return;
    inputs_[i].store(clampv(normalised, 0.0f, 1.0f), std::memory_order_relaxed);
    updates_.fetch_add(1, std::memory_order_relaxed);
}

void GestureLayer::setCalibration(float heightLow, float heightHigh, float reachNear, float reachFar, float distNear, float distFar)
{
    hLow_ = heightLow; hHigh_ = heightHigh; rNear_ = reachNear; rFar_ = reachFar; dNear_ = distNear; dFar_ = distFar;
}

void GestureLayer::startCalibration(float seconds)
{
    calibTotal_ = calibRemaining_ = std::max(seconds, 0.5f);
    calMinY_ = 1e9f; calMaxY_ = -1e9f; calMinD_ = 1e9f; calMaxD_ = -1e9f; calMinR_ = 1e9f; calMaxR_ = -1e9f;
}

void GestureLayer::finishCalibration()
{
    // Only accept what was actually explored; keep sensible minimum spans.
    if (calMaxY_ - calMinY_ > 0.25f) { const float m = 0.05f * (calMaxY_ - calMinY_); hLow_ = calMinY_ + m; hHigh_ = calMaxY_ - m; }
    if (calMaxD_ - calMinD_ > 0.20f) { const float m = 0.05f * (calMaxD_ - calMinD_); dNear_ = calMinD_ + m; dFar_ = calMaxD_ - m; }
    if (calMaxR_ - calMinR_ > 0.15f) { const float m = 0.05f * (calMaxR_ - calMinR_); rNear_ = calMinR_ + m; rFar_ = calMaxR_ - m; }
}

int GestureLayer::writeCalibration(char* out, int capacity) const
{
    return std::snprintf(out, static_cast<size_t>(capacity), "%g %g %g %g %g %g", hLow_, hHigh_, rNear_, rFar_, dNear_, dFar_);
}

bool GestureLayer::parseCalibration(const char* text)
{
    if (text == nullptr) return false;
    float v[6];
    if (std::sscanf(text, "%f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    if (!(v[1] > v[0] && v[3] > v[2] && v[5] > v[4])) return false;
    setCalibration(v[0], v[1], v[2], v[3], v[4], v[5]);
    return true;
}

void GestureLayer::setHand(int hand, float x, float y, float z, float pinch, float tilt)
{
    const int h = hand & 1;
    handX_[h] = x; handY_[h] = y; handZ_[h] = z;
    handsSeen_ = true;
    if (calibRemaining_ > 0.0f) {
        calMinY_ = std::min(calMinY_, y); calMaxY_ = std::max(calMaxY_, y);
        calMinR_ = std::min(calMinR_, -z); calMaxR_ = std::max(calMaxR_, -z);
        const float ddx = handX_[0] - handX_[1], ddy = handY_[0] - handY_[1], ddz = handZ_[0] - handZ_[1];
        const float dd = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
        calMinD_ = std::min(calMinD_, dd); calMaxD_ = std::max(calMaxD_, dd);
    }
    setInput(h == 0 ? GestureInput::LeftHeight : GestureInput::RightHeight, norm01(y, hLow_, hHigh_));
    setInput(h == 0 ? GestureInput::LeftForward : GestureInput::RightForward, norm01(-z, rNear_, rFar_));
    setInput(h == 0 ? GestureInput::LeftPinch : GestureInput::RightPinch, pinch);
    setInput(h == 0 ? GestureInput::LeftTilt : GestureInput::RightTilt, 0.5f + 0.5f * clampv(tilt, -1.0f, 1.0f));
    const float dx = handX_[0] - handX_[1], dy = handY_[0] - handY_[1], dz = handZ_[0] - handZ_[1];
    setInput(GestureInput::HandDistance, norm01(std::sqrt(dx * dx + dy * dy + dz * dz), dNear_, dFar_));
}

void GestureLayer::setHead(float yawDeg, float pitchDeg, float rollDeg)
{
    setInput(GestureInput::HeadYaw,   0.5f + 0.5f * clampv(yawDeg / 90.0f, -1.0f, 1.0f));
    setInput(GestureInput::HeadPitch, 0.5f + 0.5f * clampv(pitchDeg / 90.0f, -1.0f, 1.0f));
    setInput(GestureInput::HeadRoll,  0.5f + 0.5f * clampv(rollDeg / 90.0f, -1.0f, 1.0f));
}

bool GestureLayer::addMapping(const GestureMapping& m)
{
    const int n = numMappings_.load(std::memory_order_relaxed);
    if (n >= kMaxMappings) return false;
    maps_[n] = m;
    state_[n] = State{};
    numMappings_.store(n + 1, std::memory_order_release);   // published only once it is written
    return true;
}

void GestureLayer::setDefaultMappings()
{
    // A first vocabulary for a 30-minute set. The right pinch is the clutch:
    // nothing moves unless the right hand is holding the instrument.
    clearMappings();
    const GestureInput clutch = GestureInput::RightPinch;
    addMapping({ GestureInput::HandDistance, ParamId::MorphPos,    0.0f, 1.0f, 0.5f,  0.01f, clutch, false });
    addMapping({ GestureInput::LeftHeight,   ParamId::Depth,       0.0f, 1.0f, 0.5f,  0.01f, clutch, false });
    addMapping({ GestureInput::RightHeight,  ParamId::Brightness,  0.2f, 1.0f, 0.5f,  0.01f, clutch, false });
    addMapping({ GestureInput::LeftTilt,     ParamId::CosmosSend,  0.0f, 1.0f, 0.5f,  0.02f, clutch, false });
    addMapping({ GestureInput::RightTilt,    ParamId::FarLevel,    0.2f, 1.0f, 0.5f,  0.02f, clutch, false });
    addMapping({ GestureInput::LeftForward,  ParamId::CloudSend,   0.0f, 1.0f, 0.5f,  0.02f, clutch, false });
    addMapping({ GestureInput::RightForward, ParamId::DelayMix,    0.0f, 0.6f, 0.5f,  0.02f, clutch, false });
    addMapping({ GestureInput::HeadYaw,      ParamId::Width,       0.6f, 1.8f, 1.0f,  0.02f, GestureInput::Count, false });
    // Macros (Custom0..7 = Macro A..H): one knob, several parameters, no clutch.
    const GestureInput none = GestureInput::Count;
    addMapping({ GestureInput::Custom0, ParamId::FarLevel,     0.3f,   1.0f,    0.3f, 0.002f, none, false });   // A "Space"
    addMapping({ GestureInput::Custom0, ParamId::FarDecay,     4.0f,   60.0f,   0.3f, 0.002f, none, false });
    addMapping({ GestureInput::Custom0, ParamId::Depth,        0.2f,   1.0f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInput::Custom0, ParamId::FarSize,      1.0f,   3.0f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInput::Custom1, ParamId::CosmosSend,   0.0f,   1.0f,    0.3f, 0.002f, none, false });   // B "Alien"
    addMapping({ GestureInput::Custom1, ParamId::CosmosNebula, 0.0f,   0.8f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInput::Custom1, ParamId::CosmosShift,  0.0f,   80.0f,   0.3f, 0.002f, none, false });
    addMapping({ GestureInput::Custom2, ParamId::DriftRate,    0.02f,  0.5f,    0.3f, 0.002f, none, false });   // C "Motion"
    addMapping({ GestureInput::Custom2, ParamId::ShimmerRate,  0.05f,  1.0f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInput::Custom2, ParamId::PanDrift,     0.0f,   1.0f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInput::Custom2, ParamId::EnsembleMix,  0.2f,   0.8f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInput::Custom3, ParamId::Brightness,   0.3f,   1.0f,    0.3f, 0.002f, none, false });   // D "Bloom"
    addMapping({ GestureInput::Custom3, ParamId::Air,          0.0f,   0.5f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInput::Custom3, ParamId::Cutoff,       800.0f, 8000.0f, 0.3f, 0.002f, none, false });
    addMapping({ GestureInput::Custom3, ParamId::Presence,     0.0f,   3.0f,    0.3f, 0.002f, none, false });   // the near plane steps forward
    addMapping({ GestureInput::Custom4, ParamId::BrainDensity, 2.0f,   10.0f,   0.3f, 0.002f, none, false });   // E "Density": more notes, more grains, more strands
    addMapping({ GestureInput::Custom4, ParamId::CloudSend,    0.0f,   0.5f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInput::Custom4, ParamId::CloudDensity, 6.0f,   40.0f,   0.3f, 0.002f, none, false });
    addMapping({ GestureInput::Custom4, ParamId::Unison,       2.0f,   5.0f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInput::Custom5, ParamId::Depth,        0.2f,   1.0f,    0.3f, 0.002f, none, false });   // F "Distance": everything recedes
    addMapping({ GestureInput::Custom5, ParamId::KeysDepth,    0.0f,   0.9f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInput::Custom5, ParamId::Cutoff,       900.0f, 6000.0f, 0.3f, 0.002f, none, true  });
    addMapping({ GestureInput::Custom5, ParamId::FarLevel,     0.4f,   1.0f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInput::Custom5, ParamId::Presence,     0.0f,   3.0f,    0.3f, 0.002f, none, true  });
    addMapping({ GestureInput::Custom6, ParamId::RateWander,   0.0f,   1.0f,    0.3f, 0.002f, none, false });   // G "Evolution": everything moves more
    addMapping({ GestureInput::Custom6, ParamId::Src2PosDrift, 0.0f,   1.0f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInput::Custom6, ParamId::Src3PosDrift, 0.0f,   1.0f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInput::Custom6, ParamId::Breath,       0.0f,   0.8f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInput::Custom6, ParamId::ArcAmount,    0.0f,   1.0f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInput::Custom7, ParamId::Air,          0.0f,   0.6f,    0.3f, 0.002f, none, false });   // H "Air": breath and light on the sides
    addMapping({ GestureInput::Custom7, ParamId::AirColor,     2.0f,   8.0f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInput::Custom7, ParamId::SideAir,      0.0f,   6.0f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInput::Custom7, ParamId::FarHighcut,   2500.0f, 9000.0f, 0.3f, 0.002f, none, false });
}

bool GestureLayer::parseMappings(const char* text)
{
    if (text == nullptr) return false;
    GestureMapping parsed[kMaxMappings];
    int n = 0;
    const char* p = text;
    while (*p) {
        const char* end = p;
        while (*end && *end != '\n') ++end;
        char line[256];
        size_t len = static_cast<size_t>(end - p);
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        std::memcpy(line, p, len);
        line[len] = 0;
        p = (*end) ? end + 1 : end;
        // tokens
        char* tok[8] = {};
        int nt = 0;
        char* cur = line;
        while (nt < 8) {
            while (*cur == ' ' || *cur == '\t' || *cur == '\r') ++cur;
            if (*cur == 0 || *cur == '#') break;
            tok[nt++] = cur;
            while (*cur && *cur != ' ' && *cur != '\t' && *cur != '\r') ++cur;
            if (*cur) { *cur = 0; ++cur; }
        }
        if (nt == 0) continue;
        if (nt < 4) return false;
        GestureMapping m;
        if (!gestureInputFromName(tok[0], m.input) || m.input == GestureInput::Count) return false;
        const ParamDesc* d = findParam(tok[1]);
        if (d == nullptr) return false;
        m.param = d->id;
        m.min = static_cast<float>(std::atof(tok[2]));
        m.max = static_cast<float>(std::atof(tok[3]));
        if (nt > 4) m.smoothSeconds = static_cast<float>(std::atof(tok[4]));
        if (nt > 5) m.deadzone = static_cast<float>(std::atof(tok[5]));
        if (nt > 6 && !gestureInputFromName(tok[6], m.clutch)) return false;
        if (nt > 7) m.invert = std::atoi(tok[7]) != 0;
        if (n >= kMaxMappings) return false;
        parsed[n++] = m;
    }
    clearMappings();
    for (int i = 0; i < n; ++i) addMapping(parsed[i]);
    return true;
}

int GestureLayer::writeMappings(char* out, int capacity) const
{
    int pos = 0;
    const int count = numMappings_.load(std::memory_order_acquire);
    for (int i = 0; i < count && pos < capacity; ++i) {
        const GestureMapping& m = maps_[i];
        pos += std::snprintf(out + pos, static_cast<size_t>(capacity - pos), "%s %s %g %g %g %g %s %d\n",
                             gestureInputName(m.input), paramDesc(m.param).key, m.min, m.max, m.smoothSeconds, m.deadzone,
                             m.clutch == GestureInput::Count ? "none" : gestureInputName(m.clutch), m.invert ? 1 : 0);
    }
    return pos;
}

} // namespace ambient
