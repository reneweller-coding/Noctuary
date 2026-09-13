// Noctuary -- OSC over UDP: the one control socket for everything that is not
// MIDI (a hand-tracking bridge, a tablet, a script). Framework-free: Winsock on
// Windows, BSD sockets elsewhere (Linux, macOS, Android).
//
// Namespace (all floats unless noted):
//   /ambient/param/<key> f         parameter in its real range (see --list)
//   /ambient/paramn/<key> f        parameter normalised 0..1 along the knob curve
//   /ambient/gesture/<Input> f     gesture input 0..1 (names: HandDistance, LeftHeight, ...)
//   /ambient/hand/L  x y z pinch tilt   left hand, metres (x right, y up, z back), pinch 0..1, tilt -1..1
//   /ambient/hand/R  x y z pinch tilt   right hand
//   /ambient/head    yaw pitch roll     degrees
//   /ambient/note  i i             note number, velocity (0 = off)
//   /ambient/preset s|i            full preset by name or index
//   /ambient/sound s|i, /ambient/cosmos s|i    layer presets
//   /ambient/morph f               morph position
// Bundles are unpacked; time tags are ignored (everything is "now").
#pragma once
#include "Params.h"
#include "Gesture.h"
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <thread>

namespace ambient {

// Events that must reach the audio thread (notes, presets). Single producer, single consumer.
struct ControlEvent {
    enum class Type : int { NoteOn, NoteOff, Preset, SoundPreset, CosmosPreset };
    Type  type;
    int   a;
    float b;
};

class EventQueue {
public:
    static constexpr int kSize = 256;
    bool push(const ControlEvent& e)
    {
        const int w = write_.load(std::memory_order_relaxed);
        const int next = (w + 1) & (kSize - 1);
        if (next == read_.load(std::memory_order_acquire)) return false;
        buf_[w] = e;
        write_.store(next, std::memory_order_release);
        return true;
    }
    bool pop(ControlEvent& e)
    {
        const int r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) return false;
        e = buf_[r];
        read_.store((r + 1) & (kSize - 1), std::memory_order_release);
        return true;
    }
private:
    ControlEvent buf_[kSize] = {};
    std::atomic<int> read_{ 0 }, write_{ 0 };
};

// What the receiver does with messages. Parameter writes may come on the OSC thread.
struct OscSink {
    virtual ~OscSink() = default;
    virtual void setParam(ParamId id, float value) = 0;          // real value
    virtual void setParamNormalised(ParamId id, float norm) = 0; // 0..1 along the knob curve
    virtual void event(const ControlEvent& e) = 0;
    // The head's yaw in degrees, from /ambient/head. The gesture layer gets it as well; this is
    // for the binaural mode, which wants the angle itself rather than a mapping of it.
    virtual void setHeadYaw(float) {}
};

// One parsed OSC message (arguments already decoded).
struct OscMessage {
    static constexpr int kMaxArgs = 8;
    const char* address = nullptr;
    int   numArgs = 0;
    char  types[kMaxArgs] = {};
    float floats[kMaxArgs] = {};          // 'f' and 'i' (converted) arguments
    const char* strings[kMaxArgs] = {};   // 's' arguments
};

namespace osc_detail {
inline int32_t readInt(const char* p)
{
    return static_cast<int32_t>((static_cast<uint32_t>(static_cast<uint8_t>(p[0])) << 24)
                              | (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 16)
                              | (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 8)
                              |  static_cast<uint32_t>(static_cast<uint8_t>(p[3])));
}
inline float readFloat(const char* p)
{
    const uint32_t u = static_cast<uint32_t>(readInt(p));
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
inline size_t padded(size_t n) { return (n + 4) & ~static_cast<size_t>(3); }   // string length incl. terminator, padded to 4

template <class Handler>
int parseOne(const char* data, size_t size, Handler& handler)
{
    if (size < 4) return -1;
    if (size >= 8 && std::memcmp(data, "#bundle", 8) == 0) {
        size_t pos = 16;   // "#bundle\0" + 8-byte time tag
        int count = 0;
        while (pos + 4 <= size) {
            const int32_t len = readInt(data + pos);
            pos += 4;
            if (len < 0 || pos + static_cast<size_t>(len) > size) return -1;
            const int r = parseOne(data + pos, static_cast<size_t>(len), handler);
            if (r < 0) return -1;
            count += r;
            pos += static_cast<size_t>(len);
        }
        return count;
    }
    if (data[0] != '/') return -1;
    const size_t addrLen = ::strnlen(data, size);
    if (addrLen >= size) return -1;
    size_t pos = padded(addrLen);
    OscMessage m;
    m.address = data;
    if (pos >= size) { handler(m); return 1; }   // no type tags: message without arguments
    if (data[pos] != ',') return -1;
    const size_t tagsLen = ::strnlen(data + pos, size - pos);
    const char* tags = data + pos + 1;
    const size_t numTags = tagsLen - 1;
    pos += padded(tagsLen);
    for (size_t t = 0; t < numTags && m.numArgs < OscMessage::kMaxArgs; ++t) {
        const char tag = tags[t];
        const int k = m.numArgs;
        if (tag == 'f') { if (pos + 4 > size) return -1; m.floats[k] = readFloat(data + pos); m.types[k] = 'f'; pos += 4; }
        else if (tag == 'i') { if (pos + 4 > size) return -1; m.floats[k] = static_cast<float>(readInt(data + pos)); m.types[k] = 'i'; pos += 4; }
        else if (tag == 's') {
            const size_t sl = ::strnlen(data + pos, size - pos);
            if (pos + sl >= size) return -1;
            m.strings[k] = data + pos; m.types[k] = 's'; pos += padded(sl);
        }
        else if (tag == 'T') { m.floats[k] = 1.0f; m.types[k] = 'T'; }
        else if (tag == 'F') { m.floats[k] = 0.0f; m.types[k] = 'F'; }
        else return -1;   // blobs, doubles etc. are not needed here
        ++m.numArgs;
    }
    handler(m);
    return 1;
}
} // namespace osc_detail

// Parse a packet (message or bundle); calls handler(const OscMessage&) per message.
// Returns the number of messages, or -1 on malformed data.
template <class Handler>
int parseOscPacket(const char* data, size_t size, Handler&& handler)
{
    return osc_detail::parseOne(data, size, handler);
}

// Route one message into the sink / gesture layer. Returns false if the address is unknown.
bool dispatchOsc(const OscMessage& m, OscSink& sink, GestureLayer& gestures);

class OscServer {
public:
    OscServer() = default;
    ~OscServer() { stop(); }
    bool start(int port, OscSink& sink, GestureLayer& gestures);
    void stop();
    bool running() const { return running_.load(std::memory_order_relaxed); }
    int  port() const { return port_; }
    uint64_t messagesReceived() const { return received_.load(std::memory_order_relaxed); }
    const char* lastError() const { return error_; }
private:
    void run();
    std::thread thread_;
    std::atomic<bool> running_{ false }, stopRequested_{ false };
    std::atomic<uint64_t> received_{ 0 };
    OscSink* sink_ = nullptr;
    GestureLayer* gestures_ = nullptr;
    int port_ = 0;
    long long socket_ = -1;
    char error_[128] = "";
};

} // namespace ambient
