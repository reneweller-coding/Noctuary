/**
 * @file Osc.cpp
 * @brief The OSC receiver: the address dispatch and the UDP server thread behind Osc.h.
 *
 * Two halves. dispatchOsc() is the routing table of the /ambient/ namespace listed in Osc.h: it looks
 * at one already-parsed message, clamps its arguments into the parameter's range or the gesture
 * layer's units, and hands them on to the OscSink and the GestureLayer. It has no socket in it, so a
 * test (or a script) can push OscMessages through it without a port. OscServer owns the socket: one
 * UDP datagram socket bound to every interface with SO_REUSEADDR, a thread that waits on select() in
 * 100 ms slices so that stop() is prompt, and parseOscPacket() from the header to unpack each datagram
 * -- bundles included -- into messages for the dispatcher.
 *
 * Threading: everything here runs on the server thread, never on the audio thread. Osc.h tells the
 * sink so ("Parameter writes may come on the OSC thread"); notes and presets are expected to be
 * handed on through its EventQueue. A message whose address is unknown or whose arguments are too few
 * is dropped silently and not counted in messagesReceived(). The only platform-specific part is the
 * socket handle and how it is closed, kept in the small block above the namespace: Winsock on Windows
 * (started and stopped around the server's life), BSD sockets everywhere else.
 */
#include "ambient/Osc.h"
#include "ambient/Presets.h"
#include "ambient/Dsp.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using SocketHandleT = SOCKET;                                  ///< Winsock's socket handle, an unsigned integer type
  static const SocketHandleT InvalidSocket = INVALID_SOCKET;    ///< what socket() hands back when it fails
  /**
   * @brief Closes a socket the Winsock way.
   * @param s  the handle to close; not used again afterwards
   */
  static void closeSocket(SocketHandleT s) { closesocket(s); }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <sys/select.h>
  #include <sys/time.h>
  using SocketHandleT = int;                                     ///< a BSD socket is a file descriptor
  static const SocketHandleT InvalidSocket = -1;                ///< what socket() hands back when it fails
  /**
   * @brief Closes a socket the POSIX way, as any other file descriptor.
   * @param s  the descriptor to close; not used again afterwards
   */
  static void closeSocket(SocketHandleT s) { ::close(s); }
#endif

namespace ambient {

namespace {

/**
 * @brief Whether @p s begins with @p prefix: how the address is taken apart one segment at a time.
 * @param s       the address, or what is left of it after the segments already matched
 * @param prefix  the segment to test for, including its trailing slash where one is expected
 * @return        true when the first strlen(prefix) characters of @p s are the prefix
 */
bool startsWith(const char* s, const char* prefix) { return std::strncmp(s, prefix, std::strlen(prefix)) == 0; }

/**
 * @brief The preset a /ambient/preset, /ambient/sound or /ambient/cosmos message names.
 *
 * A string argument is matched against the preset names, an 'i' or 'f' argument is rounded to an
 * index; either way the result is checked against the table the message addresses.
 * @param m       the message; its first argument is the name or the index
 * @param cosmos  true to look in the cosmos layer presets (Presets.h), false in the full and sound
 *                presets, which share one table
 * @return        the index into that table, or -1 when there is no argument or nothing matches
 */
int presetIndexFromArg(const OscMessage& m, bool cosmos)
{
    if (m.m_numArgs < 1) return -1;
    const int count = cosmos ? numCosmosPresets() : numPresets();
    if (m.m_types[0] == 's') {
        for (int i = 0; i < count; ++i) if (std::strcmp(m.m_strings[0], (cosmos ? cosmosPreset(i) : preset(i)).m_name) == 0) return i;
        return -1;
    }
    const int i = static_cast<int>(std::lround(m.m_floats[0]));
    return (i >= 0 && i < count) ? i : -1;
}

} // namespace

bool dispatchOsc(const OscMessage& m, OscSink& sink, GestureLayer& gestures)
{
    const char* a = m.m_address;
    if (!startsWith(a, "/ambient/")) return false;
    a += 9;
    if (startsWith(a, "param/") || startsWith(a, "paramn/")) {
        const bool normalised = a[5] == 'n';
        const char* key = a + (normalised ? 7 : 6);
        const ParamDesc* d = findParam(key);
        if (d == nullptr || m.m_numArgs < 1) return false;
        if (m.m_types[0] == 's') { sink.setParam(d->m_id, paramValueFromText(*d, m.m_strings[0])); return true; }
        if (normalised) sink.setParamNormalised(d->m_id, clampv(m.m_floats[0], 0.0f, 1.0f));
        else sink.setParam(d->m_id, clampv(m.m_floats[0], d->m_min, d->m_max));
        return true;
    }
    if (startsWith(a, "gesture/")) {
        GestureInputE in;
        if (!gestureInputFromName(a + 8, in) || in == GestureInputE::COUNT || m.m_numArgs < 1) return false;
        gestures.setInput(in, m.m_floats[0]);
        return true;
    }
    if (startsWith(a, "hand/")) {
        const char h = a[5];
        if ((h != 'L' && h != 'R') || m.m_numArgs < 3) return false;
        gestures.setHand(h == 'L' ? 0 : 1, m.m_floats[0], m.m_floats[1], m.m_floats[2],
                         m.m_numArgs > 3 ? m.m_floats[3] : 0.0f, m.m_numArgs > 4 ? m.m_floats[4] : 0.0f);
        return true;
    }
    if (std::strcmp(a, "head") == 0) {
        if (m.m_numArgs < 1) return false;
        gestures.setHead(m.m_floats[0], m.m_numArgs > 1 ? m.m_floats[1] : 0.0f, m.m_numArgs > 2 ? m.m_floats[2] : 0.0f);
        sink.setHeadYaw(m.m_floats[0]);
        return true;
    }
    if (std::strcmp(a, "calibrate") == 0) {
        gestures.startCalibration(m.m_numArgs >= 1 ? m.m_floats[0] : 6.0f);
        return true;
    }
    if (std::strcmp(a, "morph") == 0) {
        if (m.m_numArgs < 1) return false;
        sink.setParam(ParamIdE::MORPH_POS, clampv(m.m_floats[0], 0.0f, 1.0f));
        return true;
    }
    if (std::strcmp(a, "note") == 0) {
        if (m.m_numArgs < 1) return false;
        const int note = static_cast<int>(std::lround(m.m_floats[0]));
        const float vel = m.m_numArgs > 1 ? m.m_floats[1] : 1.0f;
        const float v = vel > 1.0f ? vel / 127.0f : vel;
        sink.event(ControlEvent{ v > 0.0f ? ControlEvent::TypeE::NOTE_ON : ControlEvent::TypeE::NOTE_OFF, note, v });
        return true;
    }
    if (std::strcmp(a, "preset") == 0 || std::strcmp(a, "sound") == 0 || std::strcmp(a, "cosmos") == 0) {
        const bool cosmos = a[0] == 'c';
        const int idx = presetIndexFromArg(m, cosmos);
        if (idx < 0) return false;
        const ControlEvent::TypeE t = cosmos ? ControlEvent::TypeE::COSMOS_PRESET : (a[0] == 's' ? ControlEvent::TypeE::SOUND_PRESET : ControlEvent::TypeE::PRESET);
        sink.event(ControlEvent{ t, idx, 0.0f });
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- server

bool OscServer::start(int port, OscSink& sink, GestureLayer& gestures)
{
    stop();
    m_sink = &sink;
    m_gestures = &gestures;
    m_port = port;
    m_error[0] = 0;
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { std::snprintf(m_error, sizeof(m_error), "WSAStartup failed"); return false; }
#endif
    SocketHandleT s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == InvalidSocket) { std::snprintf(m_error, sizeof(m_error), "socket() failed"); return false; }
    int reuse = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<unsigned short>(port));
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::snprintf(m_error, sizeof(m_error), "port %d is in use", port);
        closeSocket(s);
        return false;
    }
    m_socket = static_cast<long long>(s);
    m_stopRequested.store(false);
    m_running.store(true);
    m_thread = std::thread([this] { run(); });
    return true;
}

void OscServer::stop()
{
    if (!m_running.load()) return;
    m_stopRequested.store(true);
    if (m_thread.joinable()) m_thread.join();
    if (m_socket >= 0) { closeSocket(static_cast<SocketHandleT>(m_socket)); m_socket = -1; }
    m_running.store(false);
#if defined(_WIN32)
    WSACleanup();
#endif
}

void OscServer::run()
{
    char buffer[4096];
    const SocketHandleT s = static_cast<SocketHandleT>(m_socket);
    while (!m_stopRequested.load(std::memory_order_relaxed)) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(s, &set);
        timeval tv{ 0, 100000 };   // 100 ms so stop() is prompt
        const int ready = ::select(static_cast<int>(s) + 1, &set, nullptr, nullptr, &tv);
        if (ready <= 0) continue;
        sockaddr_in from{};
#if defined(_WIN32)
        int fromLen = sizeof(from);
#else
        socklen_t fromLen = sizeof(from);
#endif
        const int n = static_cast<int>(::recvfrom(s, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &fromLen));
        if (n <= 0) continue;
        const int count = parseOscPacket(buffer, static_cast<size_t>(n), [this](const OscMessage& m) {
            if (dispatchOsc(m, *m_sink, *m_gestures)) m_received.fetch_add(1, std::memory_order_relaxed);
        });
        (void)count;
    }
}

} // namespace ambient
