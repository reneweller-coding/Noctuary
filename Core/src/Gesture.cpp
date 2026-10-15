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
#include <chrono>
#include <cmath>
#include "ambient/Dsp.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace ambient {

namespace {
/** @brief The inputs' names in GestureInput order: what the mapping file and the OSC receiver use. */
const char* const InputNames[NumGestureInputs] = {
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

const char* gestureInputName(GestureInputE in)
{
    const int i = static_cast<int>(in);
    return (i >= 0 && i < NumGestureInputs) ? InputNames[i] : "none";
}

bool gestureInputFromName(const char* name, GestureInputE& out)
{
    if (name == nullptr) return false;
    for (int i = 0; i < NumGestureInputs; ++i)
        if (std::strcmp(name, InputNames[i]) == 0) { out = static_cast<GestureInputE>(i); return true; }
    if (std::strcmp(name, "none") == 0 || std::strcmp(name, "-") == 0) { out = GestureInputE::COUNT; return true; }
    return false;
}

GestureLayer::GestureLayer()
{
    for (auto& i : m_inputs) i.store(0.0f, std::memory_order_relaxed);
    setDefaultMappings();
}

void GestureLayer::setInput(GestureInputE in, float normalised)
{
    const int i = static_cast<int>(in);
    if (i < 0 || i >= NumGestureInputs) return;
    m_inputs[i].store(clampv(normalised, 0.0f, 1.0f), std::memory_order_relaxed);
    m_updates.fetch_add(1, std::memory_order_relaxed);
}

void GestureLayer::setCalibration(float heightLow, float heightHigh, float reachNear, float reachFar, float distNear, float distFar)
{
    m_hLow = heightLow; m_hHigh = heightHigh; m_rNear = reachNear; m_rFar = reachFar; m_dNear = distNear; m_dFar = distFar;
}

void GestureLayer::startCalibration(float seconds)
{
    m_calibTotal = m_calibRemaining = std::max(seconds, 0.5f);
    m_calMinY = 1e9f; m_calMaxY = -1e9f; m_calMinD = 1e9f; m_calMaxD = -1e9f; m_calMinR = 1e9f; m_calMaxR = -1e9f;
}

void GestureLayer::finishCalibration()
{
    // Only accept what was actually explored; keep sensible minimum spans.
    if (m_calMaxY - m_calMinY > 0.25f) { const float m = 0.05f * (m_calMaxY - m_calMinY); m_hLow = m_calMinY + m; m_hHigh = m_calMaxY - m; }
    if (m_calMaxD - m_calMinD > 0.20f) { const float m = 0.05f * (m_calMaxD - m_calMinD); m_dNear = m_calMinD + m; m_dFar = m_calMaxD - m; }
    if (m_calMaxR - m_calMinR > 0.15f) { const float m = 0.05f * (m_calMaxR - m_calMinR); m_rNear = m_calMinR + m; m_rFar = m_calMaxR - m; }
}

int GestureLayer::writeCalibration(char* out, int capacity) const
{
    return std::snprintf(out, static_cast<size_t>(capacity), "%g %g %g %g %g %g", m_hLow, m_hHigh, m_rNear, m_rFar, m_dNear, m_dFar);
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
    m_handX[h] = x; m_handY[h] = y; m_handZ[h] = z;
    m_handsSeen = true;
    m_handStampMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(),
                       std::memory_order_relaxed);
    if (m_calibRemaining > 0.0f) {
        m_calMinY = std::min(m_calMinY, y); m_calMaxY = std::max(m_calMaxY, y);
        m_calMinR = std::min(m_calMinR, -z); m_calMaxR = std::max(m_calMaxR, -z);
        const float ddx = m_handX[0] - m_handX[1], ddy = m_handY[0] - m_handY[1], ddz = m_handZ[0] - m_handZ[1];
        const float dd = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
        m_calMinD = std::min(m_calMinD, dd); m_calMaxD = std::max(m_calMaxD, dd);
    }
    setInput(h == 0 ? GestureInputE::LEFT_HEIGHT : GestureInputE::RIGHT_HEIGHT, norm01(y, m_hLow, m_hHigh));
    setInput(h == 0 ? GestureInputE::LEFT_FORWARD : GestureInputE::RIGHT_FORWARD, norm01(-z, m_rNear, m_rFar));
    setInput(h == 0 ? GestureInputE::LEFT_PINCH : GestureInputE::RIGHT_PINCH, pinch);
    setInput(h == 0 ? GestureInputE::LEFT_TILT : GestureInputE::RIGHT_TILT, 0.5f + 0.5f * clampv(tilt, -1.0f, 1.0f));
    const float dx = m_handX[0] - m_handX[1], dy = m_handY[0] - m_handY[1], dz = m_handZ[0] - m_handZ[1];
    setInput(GestureInputE::HAND_DISTANCE, norm01(std::sqrt(dx * dx + dy * dy + dz * dz), m_dNear, m_dFar));
}

bool GestureLayer::handsRecently(double seconds) const
{
    const int64_t stamp = m_handStampMs.load(std::memory_order_relaxed);
    if (stamp == 0) return false;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return static_cast<double>(now - stamp) < seconds * 1000.0;
}

void GestureLayer::setHead(float yawDeg, float pitchDeg, float rollDeg)
{
    setInput(GestureInputE::HEAD_YAW,   0.5f + 0.5f * clampv(yawDeg / 90.0f, -1.0f, 1.0f));
    setInput(GestureInputE::HEAD_PITCH, 0.5f + 0.5f * clampv(pitchDeg / 90.0f, -1.0f, 1.0f));
    setInput(GestureInputE::HEAD_ROLL,  0.5f + 0.5f * clampv(rollDeg / 90.0f, -1.0f, 1.0f));
}

bool GestureLayer::addMapping(const GestureMapping& m)
{
    const int n = m_numMappings.load(std::memory_order_relaxed);
    if (n >= MaxMappings) return false;
    m_maps[n] = m;
    m_state[n] = State{};
    m_numMappings.store(n + 1, std::memory_order_release);   // published only once it is written
    return true;
}

void GestureLayer::setDefaultMappings()
{
    // A first vocabulary for a 30-minute set. The right pinch is the clutch:
    // nothing moves unless the right hand is holding the instrument.
    clearMappings();
    const GestureInputE clutch = GestureInputE::RIGHT_PINCH;
    addMapping({ GestureInputE::HAND_DISTANCE, ParamIdE::MORPH_POS,    0.0f, 1.0f, 0.5f,  0.01f, clutch, false });
    addMapping({ GestureInputE::LEFT_HEIGHT,   ParamIdE::DEPTH,       0.0f, 1.0f, 0.5f,  0.01f, clutch, false });
    addMapping({ GestureInputE::RIGHT_HEIGHT,  ParamIdE::BRIGHTNESS,  0.2f, 1.0f, 0.5f,  0.01f, clutch, false });
    addMapping({ GestureInputE::LEFT_TILT,     ParamIdE::COSMOS_SEND,  0.0f, 1.0f, 0.5f,  0.02f, clutch, false });
    addMapping({ GestureInputE::RIGHT_TILT,    ParamIdE::FAR_LEVEL,    0.2f, 1.0f, 0.5f,  0.02f, clutch, false });
    addMapping({ GestureInputE::LEFT_FORWARD,  ParamIdE::CLOUD_SEND,   0.0f, 1.0f, 0.5f,  0.02f, clutch, false });
    addMapping({ GestureInputE::RIGHT_FORWARD, ParamIdE::DELAY_MIX,    0.0f, 0.6f, 0.5f,  0.02f, clutch, false });
    addMapping({ GestureInputE::HEAD_YAW,      ParamIdE::WIDTH,       0.6f, 1.8f, 1.0f,  0.02f, GestureInputE::COUNT, false });
    // Macros (Custom0..7 = Macro A..H): one knob, several parameters, no clutch.
    const GestureInputE none = GestureInputE::COUNT;
    addMapping({ GestureInputE::CUSTOM0, ParamIdE::FAR_LEVEL,     0.3f,   1.0f,    0.3f, 0.002f, none, false });   // A "Space"
    addMapping({ GestureInputE::CUSTOM0, ParamIdE::FAR_DECAY,     4.0f,   60.0f,   0.3f, 0.002f, none, false });
    addMapping({ GestureInputE::CUSTOM0, ParamIdE::DEPTH,        0.2f,   1.0f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInputE::CUSTOM0, ParamIdE::FAR_SIZE,      1.0f,   3.0f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInputE::CUSTOM1, ParamIdE::COSMOS_SEND,   0.0f,   1.0f,    0.3f, 0.002f, none, false });   // B "Alien"
    addMapping({ GestureInputE::CUSTOM1, ParamIdE::COSMOS_NEBULA, 0.0f,   0.8f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInputE::CUSTOM1, ParamIdE::COSMOS_SHIFT,  0.0f,   80.0f,   0.3f, 0.002f, none, false });
    addMapping({ GestureInputE::CUSTOM2, ParamIdE::DRIFT_RATE,    0.02f,  0.5f,    0.3f, 0.002f, none, false });   // C "Motion"
    addMapping({ GestureInputE::CUSTOM2, ParamIdE::SHIMMER_RATE,  0.05f,  1.0f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInputE::CUSTOM2, ParamIdE::PAN_DRIFT,     0.0f,   1.0f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInputE::CUSTOM2, ParamIdE::ENSEMBLE_MIX,  0.2f,   0.8f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInputE::CUSTOM3, ParamIdE::BRIGHTNESS,   0.3f,   1.0f,    0.3f, 0.002f, none, false });   // D "Bloom"
    addMapping({ GestureInputE::CUSTOM3, ParamIdE::AIR,          0.0f,   0.5f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInputE::CUSTOM3, ParamIdE::CUTOFF,       800.0f, 8000.0f, 0.3f, 0.002f, none, false });
    addMapping({ GestureInputE::CUSTOM3, ParamIdE::PRESENCE,     0.0f,   3.0f,    0.3f, 0.002f, none, false });   // the near plane steps forward
    addMapping({ GestureInputE::CUSTOM4, ParamIdE::BRAIN_DENSITY, 2.0f,   10.0f,   0.3f, 0.002f, none, false });   // E "Density": more notes, more grains, more strands
    addMapping({ GestureInputE::CUSTOM4, ParamIdE::CLOUD_SEND,    0.0f,   0.5f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInputE::CUSTOM4, ParamIdE::CLOUD_DENSITY, 6.0f,   40.0f,   0.3f, 0.002f, none, false });
    addMapping({ GestureInputE::CUSTOM4, ParamIdE::UNISON,       2.0f,   5.0f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInputE::CUSTOM5, ParamIdE::DEPTH,        0.2f,   1.0f,    0.3f, 0.002f, none, false });   // F "Distance": everything recedes
    addMapping({ GestureInputE::CUSTOM5, ParamIdE::KEYS_DEPTH,    0.0f,   0.9f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInputE::CUSTOM5, ParamIdE::CUTOFF,       900.0f, 6000.0f, 0.3f, 0.002f, none, true  });
    addMapping({ GestureInputE::CUSTOM5, ParamIdE::FAR_LEVEL,     0.4f,   1.0f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInputE::CUSTOM5, ParamIdE::PRESENCE,     0.0f,   3.0f,    0.3f, 0.002f, none, true  });
    addMapping({ GestureInputE::CUSTOM6, ParamIdE::RATE_WANDER,   0.0f,   1.0f,    0.3f, 0.002f, none, false });   // G "Evolution": everything moves more
    addMapping({ GestureInputE::CUSTOM6, ParamIdE::SRC2_POS_DRIFT, 0.0f,   1.0f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInputE::CUSTOM6, ParamIdE::SRC3_POS_DRIFT, 0.0f,   1.0f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInputE::CUSTOM6, ParamIdE::BREATH,       0.0f,   0.8f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInputE::CUSTOM6, ParamIdE::ARC_AMOUNT,    0.0f,   1.0f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInputE::CUSTOM7, ParamIdE::AIR,          0.0f,   0.6f,    0.3f, 0.002f, none, false });   // H "Air": breath and light on the sides
    addMapping({ GestureInputE::CUSTOM7, ParamIdE::AIR_COLOR,     2.0f,   8.0f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInputE::CUSTOM7, ParamIdE::SIDE_AIR,      0.0f,   6.0f,    0.3f, 0.002f, none, false });
    addMapping({ GestureInputE::CUSTOM7, ParamIdE::FAR_HIGHCUT,   2500.0f, 9000.0f, 0.3f, 0.002f, none, false });
}

bool GestureLayer::parseMappings(const char* text)
{
    if (text == nullptr) return false;
    GestureMapping parsed[MaxMappings];
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
        if (!gestureInputFromName(tok[0], m.m_input) || m.m_input == GestureInputE::COUNT) return false;
        const ParamDesc* d = findParam(tok[1]);
        if (d == nullptr) return false;
        m.m_param = d->m_id;
        m.m_min = static_cast<float>(std::atof(tok[2]));
        m.m_max = static_cast<float>(std::atof(tok[3]));
        if (nt > 4) m.m_smoothSeconds = static_cast<float>(std::atof(tok[4]));
        if (nt > 5) m.m_deadzone = static_cast<float>(std::atof(tok[5]));
        if (nt > 6 && !gestureInputFromName(tok[6], m.m_clutch)) return false;
        if (nt > 7) m.m_invert = std::atoi(tok[7]) != 0;
        if (n >= MaxMappings) return false;
        parsed[n++] = m;
    }
    clearMappings();
    for (int i = 0; i < n; ++i) addMapping(parsed[i]);
    return true;
}

int GestureLayer::writeMappings(char* out, int capacity) const
{
    int pos = 0;
    const int count = m_numMappings.load(std::memory_order_acquire);
    for (int i = 0; i < count && pos < capacity; ++i) {
        const GestureMapping& m = m_maps[i];
        pos += std::snprintf(out + pos, static_cast<size_t>(capacity - pos), "%s %s %g %g %g %g %s %d\n",
                             gestureInputName(m.m_input), paramDesc(m.m_param).m_key, m.m_min, m.m_max, m.m_smoothSeconds, m.m_deadzone,
                             m.m_clutch == GestureInputE::COUNT ? "none" : gestureInputName(m.m_clutch), m.m_invert ? 1 : 0);
    }
    return pos;
}

} // namespace ambient
