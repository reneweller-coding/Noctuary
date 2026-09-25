/**
 * @file Osc.h
 * @brief OSC over UDP: the one control socket for everything that is not
 *        MIDI (a hand-tracking bridge, a tablet, a script).
 *
 * Framework-free: Winsock on
 * Windows, BSD sockets elsewhere (Linux, macOS, Android).
 *
 * Namespace (all floats unless noted):
 *   /ambient/param/\<key\> f         parameter in its real range (see --list)
 *   /ambient/paramn/\<key\> f        parameter normalised 0..1 along the knob curve
 *   /ambient/gesture/\<Input\> f     gesture input 0..1 (names: HandDistance, LeftHeight, ...)
 *   /ambient/hand/L  x y z pinch tilt   left hand, metres (x right, y up, z back), pinch 0..1, tilt -1..1
 *   /ambient/hand/R  x y z pinch tilt   right hand
 *   /ambient/head    yaw pitch roll     degrees
 *   /ambient/note  i i             note number, velocity (0 = off)
 *   /ambient/preset s|i            full preset by name or index
 *   /ambient/sound s|i, /ambient/cosmos s|i    layer presets
 *   /ambient/morph f               morph position
 * Bundles are unpacked; time tags are ignored (everything is "now").
 *
 * The header holds the packet parser (a template, so a test can feed it bytes without a socket)
 * and the queue that carries notes and preset changes to the audio thread; the server and the
 * address dispatch live in Osc.cpp. Parameter writes go straight into the sink on the OSC thread,
 * as the sink's own atomics allow; everything that must not race the audio thread goes through
 * the EventQueue.
 */
#pragma once
#include "Params.h"
#include "Gesture.h"
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <thread>

namespace ambient {

/** @brief Events that must reach the audio thread (notes, presets). Single producer, single consumer. */
struct ControlEvent {
    /** @brief What the event is: a note on or off (a = note, b = velocity), or a full, sound-layer or Cosmos-layer preset change (a = index). */
    enum class Type : int { NoteOn, NoteOff, Preset, SoundPreset, CosmosPreset };
    Type  type;   ///< which of the five
    int   a;      ///< the note number, or the preset index
    float b;      ///< the velocity 0..1 for NoteOn; unused otherwise
};

/**
 * @brief A lock-free ring of kSize - 1 ControlEvents from one producer (the OSC thread) to one
 *        consumer (the audio thread).
 */
class EventQueue {
public:
    static constexpr int kSize = 256;   ///< ring length, a power of two; one slot is always kept empty
    /**
     * @brief Producer side: appends an event.
     * @param e  the event
     * @return   false when the ring is full (the event is dropped)
     */
    bool push(const ControlEvent& e)
    {
        const int w = write_.load(std::memory_order_relaxed);
        const int next = (w + 1) & (kSize - 1);
        if (next == read_.load(std::memory_order_acquire)) return false;
        buf_[w] = e;
        write_.store(next, std::memory_order_release);
        return true;
    }
    /**
     * @brief Consumer side: takes the oldest event.
     * @param e  receives the event
     * @return   false when the ring is empty (@p e untouched)
     */
    bool pop(ControlEvent& e)
    {
        const int r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) return false;
        e = buf_[r];
        read_.store((r + 1) & (kSize - 1), std::memory_order_release);
        return true;
    }
private:
    ControlEvent buf_[kSize] = {};             ///< the ring
    std::atomic<int> read_{ 0 },    ///< @brief the consumer's position, wrapped with kSize - 1
                     write_{ 0 };   ///< the producer's position, wrapped with kSize - 1
};

/** @brief What the receiver does with messages. Parameter writes may come on the OSC thread. */
struct OscSink {
    /** @brief Virtual so a sink may be held by base pointer. */
    virtual ~OscSink() = default;
    /**
     * @brief A parameter set to a value in its real range (already clamped to the parameter's min .. max).
     * @param id     the parameter
     * @param value  real value
     */
    virtual void setParam(ParamId id, float value) = 0;
    /**
     * @brief A parameter set along its knob curve.
     * @param id    the parameter
     * @param norm  0..1 along the knob curve
     */
    virtual void setParamNormalised(ParamId id, float norm) = 0;
    /**
     * @brief A note or a preset change, to be queued for the audio thread.
     * @param e  the event
     */
    virtual void event(const ControlEvent& e) = 0;
    /**
     * @brief The head's yaw in degrees, from /ambient/head. The gesture layer gets it as well; this is
     *        for the binaural mode, which wants the angle itself rather than a mapping of it.
     *
     * The unnamed argument is the yaw in degrees; the default does nothing with it.
     */
    virtual void setHeadYaw(float) {}
};

/** @brief One parsed OSC message (arguments already decoded). */
struct OscMessage {
    static constexpr int kMaxArgs = 8;    ///< arguments beyond this are ignored
    const char* address = nullptr;        ///< the address pattern, pointing into the packet
    int   numArgs = 0;                    ///< how many of types/floats/strings are valid
    char  types[kMaxArgs] = {};           ///< the type tag per argument: 'f', 'i', 's', 'T' or 'F'
    float floats[kMaxArgs] = {};          ///< 'f' and 'i' (converted) arguments
    const char* strings[kMaxArgs] = {};   ///< 's' arguments
};

/** @brief The parser's innards: big-endian readers and the recursive packet walk. */
namespace osc_detail {
/**
 * @brief Reads a big-endian 32-bit integer.
 * @param p  four bytes
 * @return   the value
 */
inline int32_t readInt(const char* p)
{
    return static_cast<int32_t>((static_cast<uint32_t>(static_cast<uint8_t>(p[0])) << 24)
                              | (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 16)
                              | (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 8)
                              |  static_cast<uint32_t>(static_cast<uint8_t>(p[3])));
}
/**
 * @brief Reads a big-endian IEEE float.
 * @param p  four bytes
 * @return   the value
 */
inline float readFloat(const char* p)
{
    const uint32_t u = static_cast<uint32_t>(readInt(p));
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
/**
 * @brief How many bytes an OSC string of @p n characters occupies.
 * @param n  the string's length without its terminator
 * @return   string length incl. terminator, padded to 4
 */
inline size_t padded(size_t n) { return (n + 4) & ~static_cast<size_t>(3); }

/**
 * @brief Parses one packet element: a bundle (recursing into its elements, time tag ignored) or a
 *        message, whose arguments are decoded into an OscMessage handed to @p handler.
 *
 * Understands 'f', 'i', 's', 'T' and 'F'; a message without type tags is delivered with no
 * arguments; any other tag, or a length that runs past the packet, is malformed.
 *
 * @tparam Handler  callable as handler(const OscMessage&)
 * @param data      the bytes
 * @param size      how many
 * @param handler   receives each message
 * @return          the number of messages delivered, or -1 on malformed data
 */
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

/**
 * @brief Parse a packet (message or bundle); calls handler(const OscMessage&) per message.
 *        Returns the number of messages, or -1 on malformed data.
 *
 * @tparam Handler  callable as handler(const OscMessage&)
 * @param data      the packet's bytes, as received from the socket
 * @param size      how many
 * @param handler   receives each message; the OscMessage points into @p data and is valid only during the call
 * @return          the number of messages, or -1 on malformed data
 */
template <class Handler>
int parseOscPacket(const char* data, size_t size, Handler&& handler)
{
    return osc_detail::parseOne(data, size, handler);
}

/**
 * @brief Route one message into the sink / gesture layer. Returns false if the address is unknown.
 *
 * Implements the namespace in the file comment (plus /ambient/calibrate, which starts the gesture
 * layer's calibration). A parameter given as a string is parsed with paramValueFromText, so a
 * choice may be sent by name; a preset given as a string is looked up by name; a note velocity
 * above 1 is taken as MIDI 0..127. Runs on the OSC thread.
 *
 * @param m         the parsed message
 * @param sink      receives parameters, events and the head yaw
 * @param gestures  receives gesture inputs, hands and head
 * @return          false if the address is unknown or the arguments do not fit it
 */
bool dispatchOsc(const OscMessage& m, OscSink& sink, GestureLayer& gestures);

/**
 * @brief The UDP server: a socket bound to a port and a thread that receives, parses and dispatches.
 *
 * start() and stop() are message-thread calls; the thread wakes every 100 ms to see whether it
 * should leave, so stop() returns promptly.
 */
class OscServer {
public:
    /** @brief An idle server; nothing is opened until start(). */
    OscServer() = default;
    /** @brief Stops the server if it is running (see stop()). */
    ~OscServer() { stop(); }
    /**
     * @brief Binds a UDP socket to @p port on every interface and starts the receiving thread.
     *
     * A server already running is stopped first. On Windows this also initialises Winsock.
     *
     * @param port      the UDP port to listen on
     * @param sink      where parameters, events and the head yaw go; must outlive the server
     * @param gestures  where hands, head and gesture inputs go; must outlive the server
     * @return          false when the socket could not be made or the port is in use (see lastError())
     */
    bool start(int port, OscSink& sink, GestureLayer& gestures);
    /** @brief Asks the thread to leave, joins it, closes the socket. Safe when not running. */
    void stop();
    /** @return whether the server is listening */
    bool running() const { return running_.load(std::memory_order_relaxed); }
    /** @return the port start() was given (0 before any start) */
    int  port() const { return port_; }
    /** @return how many messages were dispatched to a known address since start() */
    uint64_t messagesReceived() const { return received_.load(std::memory_order_relaxed); }
    /** @return why the last start() failed, "" when it did not */
    const char* lastError() const { return error_; }
private:
    /** @brief The thread's loop: select with a 100 ms timeout, recvfrom, parseOscPacket, dispatchOsc, until stop() asks. */
    void run();
    std::thread thread_;                                            ///< the receiving thread
    std::atomic<bool> running_{ false },         ///< @brief listening
                      stopRequested_{ false };   ///< told to leave
    std::atomic<uint64_t> received_{ 0 };                           ///< dispatched messages, behind messagesReceived()
    OscSink* sink_ = nullptr;                                       ///< where messages go (not owned)
    GestureLayer* gestures_ = nullptr;                              ///< where hands and head go (not owned)
    int port_ = 0;                                                  ///< the bound port
    long long socket_ = -1;                                         ///< the socket handle, -1 when closed (wide enough for Winsock's SOCKET)
    char error_[128] = "";                                          ///< the last start() failure, for lastError()
};

} // namespace ambient
