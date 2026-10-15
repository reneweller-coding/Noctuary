/**
 * @file Quest/src/main.cpp
 * @brief Noctuary for Meta Quest -- native OpenXR application.
 *
 * @verbatim
 *   hands (XR_EXT_hand_tracking) -> GestureLayer -> Engine parameters
 *   left pinch                   -> HandMenu (presets A/B, morph, record, calibrate)
 *   Oboe output stream           -> Engine::process (+ WavRecorder)
 *   GLES 3 scene                 <- Engine observers (sounding notes, level, distance, root, morph)
 *   optional bridge              -> OSC to a PC running the desktop plugin
 * @endverbatim
 *
 * No game engine: NativeActivity + android_native_app_glue, EGL, OpenXR loader, Oboe, the core.
 * The whole front end lives in this one file, in an anonymous namespace: the small matrix and
 * quaternion helpers the OpenXR views need, a 5x7 dot font for the head-locked panel, the OSC
 * sender of the bridge mode, the Oboe stream that drives Engine::process, the config reader, the
 * GLES point-cloud scene, and App, which owns the OpenXR instance, session, spaces, swapchains
 * and hand trackers and runs the frame loop. android_main at the bottom is the only symbol the
 * NativeActivity glue looks for. Nothing here touches the synthesizer's internals: the engine is
 * driven through the same Engine, GestureLayer, HandMenu and preset calls the desktop plugin uses,
 * so a preset, a route or a calibration means the same thing on both.
 *
 * Threads: the glue thread runs everything below except Audio::onAudioReady, which Oboe calls on
 * its own real-time thread; the engine, the gesture layer and the recorder are built for that split
 * (parameters through atomics, recording through a lock-free ring). Impulses that arrive before the
 * engine is prepared wait in App until the audio has started.
 *
 * @verbatim
 * Files in <externalDataPath>:
 *   ambient.cfg   osc_host=192.168.1.20  osc_port=9000  audio=1  preset=Consonant Hollow  route=Night Descent  rest_zone=0.08
 *   calib.txt     hand calibration, written after the calibration gesture
 *   texture.wav   optional sample for the Texture source slots (assumed recorded at C4)
 *   wavetable.wav optional user wavetable, 2048-sample frames (Table = User)
 *   Packs/*.ambientpack  preset packs loaded at start (see Core/include/ambient/Presets.h)
 *                        a pack preset may name its own texture, wavetable and impulse
 *   rec-*.wav     recordings
 * @endverbatim
 */

#include <android/log.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <jni.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <oboe/Oboe.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <atomic>
#include <memory>

#include "ambient/Engine.h"
#include "ambient/Gesture.h"
#include "ambient/Menu.h"
#include "ambient/Presets.h"
#include "ambient/Recorder.h"
#include "ambient/WavFile.h"
#include "ambient/Sources.h"
#include <dirent.h>

/** @brief Informational line in logcat under the tag "Noctuary"; printf-style arguments. */
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "Noctuary", __VA_ARGS__)
/** @brief Error line in logcat under the tag "Noctuary"; printf-style arguments. */
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "Noctuary", __VA_ARGS__)

using namespace ambient;

namespace {

// ---------------------------------------------------------------- small math

/**
 * @brief A 4x4 matrix in OpenGL's column-major layout: element (row, column) sits at m[column * 4 + row].
 *
 * Only what the two eyes' view-projection needs; there is no general linear algebra here.
 */
struct Mat4 {
    float m_m[16];   ///< the sixteen elements, column after column, as glUniformMatrix4fv reads them
};
/** @brief A point or direction in metres, OpenXR convention: x right, y up, z back (towards the viewer). */
struct Vec3 {
    float m_x,   ///< right, in metres (unitless for a direction)
    m_y,         ///< up
    m_z;         ///< back, towards the viewer
};

/**
 * @brief The identity matrix, the starting point of every matrix built below.
 * @return a Mat4 with ones on the diagonal and zeros elsewhere
 */
Mat4 identity() { Mat4 r{}; r.m_m[0] = r.m_m[5] = r.m_m[10] = r.m_m[15] = 1.0f; return r; }

/**
 * @brief The matrix product a * b, so that (a * b) v applies b first and a second.
 * @param a  the left factor (applied last)
 * @param b  the right factor (applied first)
 * @return   the product, column-major like its factors
 */
Mat4 multiply(const Mat4& a, const Mat4& b)
{
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr)
            r.m_m[c * 4 + rr] = a.m_m[0 * 4 + rr] * b.m_m[c * 4 + 0] + a.m_m[1 * 4 + rr] * b.m_m[c * 4 + 1] + a.m_m[2 * 4 + rr] * b.m_m[c * 4 + 2] + a.m_m[3 * 4 + rr] * b.m_m[c * 4 + 3];
    return r;
}

/**
 * @brief An OpenGL perspective projection from the asymmetric field of view OpenXR reports per eye.
 *
 * The four angles are turned into tangents, so the frustum need not be centred on the view axis --
 * the two eyes of a headset never are. Depth maps to the -1 .. 1 clip range of GLES.
 *
 * @param fov    the eye's half angles in radians, as xrLocateViews gives them (left and down negative)
 * @param nearZ  distance of the near plane in metres (the app uses 0.05)
 * @param farZ   distance of the far plane in metres (the app uses 100)
 * @return       the projection matrix, column-major
 */
Mat4 projectionFromFov(const XrFovf& fov, float nearZ, float farZ)
{
    const float l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight), u = std::tan(fov.angleUp), d = std::tan(fov.angleDown);
    const float w = r - l, h = u - d;
    Mat4 p{};
    p.m_m[0] = 2.0f / w;  p.m_m[8] = (r + l) / w;
    p.m_m[5] = 2.0f / h;  p.m_m[9] = (u + d) / h;
    p.m_m[10] = -(farZ + nearZ) / (farZ - nearZ);
    p.m_m[11] = -1.0f;
    p.m_m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
    return p;
}

/**
 * @brief The rotation matrix of a unit quaternion.
 * @param q  a unit quaternion (OpenXR poses hand out normalised ones; nothing renormalises here)
 * @return   the rotation as a Mat4 with no translation
 */
Mat4 rotationFromQuat(const XrQuaternionf& q)
{
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    Mat4 r = identity();
    r.m_m[0] = 1 - 2 * (y * y + z * z); r.m_m[4] = 2 * (x * y - z * w);     r.m_m[8] = 2 * (x * z + y * w);
    r.m_m[1] = 2 * (x * y + z * w);     r.m_m[5] = 1 - 2 * (x * x + z * z); r.m_m[9] = 2 * (y * z - x * w);
    r.m_m[2] = 2 * (x * z - y * w);     r.m_m[6] = 2 * (y * z + x * w);     r.m_m[10] = 1 - 2 * (x * x + y * y);
    return r;
}

/**
 * @brief Rotates a vector by a quaternion, through its rotation matrix.
 * @param q  the rotation, a unit quaternion
 * @param v  the vector to rotate (a direction: no translation is applied)
 * @return   the rotated vector
 */
Vec3 rotate(const XrQuaternionf& q, Vec3 v)
{
    const Mat4 r = rotationFromQuat(q);
    return { r.m_m[0] * v.m_x + r.m_m[4] * v.m_y + r.m_m[8] * v.m_z, r.m_m[1] * v.m_x + r.m_m[5] * v.m_y + r.m_m[9] * v.m_z, r.m_m[2] * v.m_x + r.m_m[6] * v.m_y + r.m_m[10] * v.m_z };
}

/**
 * @brief The view matrix of an eye: the inverse of its pose in the stage space.
 *
 * Inverse rotation (the transpose, the quaternion being unit) times the negated translation, so a
 * point given in stage coordinates lands in the eye's own frame.
 *
 * @param pose  the eye's pose from xrLocateViews, in the space the scene is drawn in
 * @return      the view matrix, to be multiplied with the projection
 */
Mat4 viewFromPose(const XrPosef& pose)
{
    Mat4 r = rotationFromQuat(pose.orientation);
    Mat4 rt = identity();
    for (int c = 0; c < 3; ++c) for (int rr = 0; rr < 3; ++rr) rt.m_m[c * 4 + rr] = r.m_m[rr * 4 + c];
    Mat4 t = identity();
    t.m_m[12] = -pose.position.x; t.m_m[13] = -pose.position.y; t.m_m[14] = -pose.position.z;
    return multiply(rt, t);
}

/**
 * @brief Yaw, pitch and roll in degrees of a head orientation, for the gesture layer and the engine.
 *
 * OpenXR's y-up frame: yaw is the turn about the vertical axis, pitch the nod about x, roll the
 * tilt about the view axis. The pitch's sine is clamped to -1 .. 1 before the asin so a quaternion
 * slightly off unit length cannot produce a NaN.
 *
 * @param q      the orientation, a unit quaternion
 * @param yaw    out: degrees, 0 looking along -z of the stage, positive turning towards +x (the right)
 * @param pitch  out: degrees, positive looking up
 * @param roll   out: degrees, positive tilting the head to the left
 */
void quatToEulerDeg(const XrQuaternionf& q, float& yaw, float& pitch, float& roll)
{
    const float sinp = 2.0f * (q.w * q.x - q.y * q.z);
    pitch = std::asin(std::fmax(-1.0f, std::fmin(1.0f, sinp))) * 57.29578f;
    yaw = std::atan2(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y)) * 57.29578f;
    roll = std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.x * q.x + q.z * q.z)) * 57.29578f;
}

// ---------------------------------------------------------------- 5x7 point font

/**
 * @brief The 5x7 dot pattern of a character, as Scene::addText draws it on the panel.
 *
 * Rows top to bottom, 5 bits each (bit 4 = left column). Covers A-Z, 0-9 and a few signs.
 * Lower-case letters are folded to upper case; anything the font does not have is the blank glyph,
 * so a preset name with an odd character still takes up its cell.
 *
 * @param c  the character to draw
 * @return   seven row bitmasks, a pointer into the static table (never null)
 */
const unsigned char* glyph(char c)
{
    static const unsigned char Font[][7] = {
        {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, // A B C
        {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, // D E F
        {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, // G H I
        {0x01,0x01,0x01,0x01,0x11,0x11,0x0E}, {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, // J K L
        {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, // M N O
        {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, // P Q R
        {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, // S T U
        {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, // V W X
        {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},                                        // Y Z
        {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, // 0 1 2
        {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 3 4 5
        {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 6 7 8
        {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},                                                                              // 9
        {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, // space - .
        {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}, {0x01,0x02,0x04,0x08,0x10,0x00,0x00}, {0x02,0x04,0x08,0x04,0x02,0x00,0x00}, // : / <
        {0x08,0x04,0x02,0x04,0x08,0x00,0x00}, {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}, // > + =
        {0x00,0x00,0x00,0x00,0x00,0x00,0x00},                                                                              // unknown
    };
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return Font[c - 'A'];
    if (c >= '0' && c <= '9') return Font[26 + (c - '0')];
    switch (c) { case ' ': return Font[36]; case '-': return Font[37]; case '.': return Font[38]; case ':': return Font[39];
                 case '/': return Font[40]; case '<': return Font[41]; case '>': return Font[42]; case '+': return Font[43]; case '=': return Font[44]; }
    return Font[45];
}

// ---------------------------------------------------------------- OSC sender (bridge mode)

/**
 * @brief The bridge mode's OSC sender: UDP datagrams of float arguments to the desktop plugin.
 *
 * Only what the bridge needs -- an address, a type tag string of `f`s and big-endian floats, no
 * bundles, no other types. With `osc_host` set in ambient.cfg the app sends every frame's hand
 * and head data through it (App::updateHands, App::updateHead) so the plugin on a PC can be played
 * from the headset.
 */
class OscOut {
public:
    /**
     * @brief Opens the UDP socket towards the plugin, closing any earlier one.
     * @param host  the PC's IPv4 address in dotted form (no name resolution)
     * @param port  the plugin's OSC port (9000 by default)
     * @return      false if the socket could not be made or the host is not a dotted IPv4 address
     */
    bool open(const std::string& host, int port)
    {
        close();
        m_sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_sock < 0) return false;
        std::memset(&m_to, 0, sizeof(m_to));
        m_to.sin_family = AF_INET;
        m_to.sin_port = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET, host.c_str(), &m_to.sin_addr) != 1) { close(); return false; }
        return true;
    }
    /** @brief Closes the socket; afterwards ok() is false and send() does nothing. */
    void close() { if (m_sock >= 0) ::close(m_sock); m_sock = -1; }
    /** @brief Whether a socket is open, i.e. the bridge is on. @return true after a successful open() */
    bool ok() const { return m_sock >= 0; }
    /**
     * @brief Sends one OSC message of float arguments; a no-op while the socket is closed.
     *
     * The packet is built in a 256-byte buffer: address and type tags padded to four bytes, then
     * the floats as big-endian 32-bit words. Fire and forget: the return of sendto is not checked.
     *
     * @param address  the OSC address pattern, e.g. "/ambient/hand/L"
     * @param args     the float arguments
     * @param n        how many, at most 14 (the type tag string holds 16 characters)
     */
    void send(const char* address, const float* args, int n)
    {
        if (m_sock < 0) return;
        char buf[256]; size_t pos = 0;
        auto putStr = [&](const char* s) { const size_t l = std::strlen(s) + 1; std::memcpy(buf + pos, s, l); pos += l; while (pos & 3) buf[pos++] = 0; };
        putStr(address);
        char tags[16]; tags[0] = ','; for (int i = 0; i < n; ++i) tags[1 + i] = 'f'; tags[1 + n] = 0;
        putStr(tags);
        for (int i = 0; i < n; ++i) { uint32_t u; std::memcpy(&u, &args[i], 4); buf[pos++] = static_cast<char>(u >> 24); buf[pos++] = static_cast<char>(u >> 16); buf[pos++] = static_cast<char>(u >> 8); buf[pos++] = static_cast<char>(u); }
        ::sendto(m_sock, buf, pos, 0, reinterpret_cast<sockaddr*>(&m_to), sizeof(m_to));
    }
private:
    int m_sock = -1;      ///< the UDP socket, or -1 while closed
    sockaddr_in m_to{};   ///< the plugin's address and port, filled by open()
};

// ---------------------------------------------------------------- audio

/**
 * @brief The Oboe output stream and its callback: the engine renders into it, the recorder listens.
 *
 * An exclusive low-latency float stereo stream is asked for at 48 kHz; whatever rate the device
 * grants is what the engine is prepared at, and what a recording is written at. The callback is the
 * app's audio thread: it renders the engine in chunks, hands the same samples to the recorder's ring
 * and interleaves them into the stream's buffer.
 */
class Audio : public oboe::AudioStreamDataCallback {
public:
    /**
     * @brief Binds the stream to the engine it renders and the recorder that taps it.
     * @param e  the engine, prepared by start() and run by the callback
     * @param r  the recorder that gets every rendered block (it ignores them while not recording)
     */
    Audio(Engine& e, WavRecorder& r) : m_engine(e), m_recorder(r) {}
    /**
     * @brief Opens and starts the stream and prepares the engine at the stream's rate.
     *
     * Buffer size is two bursts. The engine is prepared with a maximum block of 1024 and the
     * scratch buffers hold 8192 frames; the callback works in chunks of at most 4096 frames.
     *
     * @return false if the stream could not be opened or started (the app then runs without sound)
     */
    bool start()
    {
        oboe::AudioStreamBuilder b;
        b.setDirection(oboe::Direction::Output)
         ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
         ->setSharingMode(oboe::SharingMode::Exclusive)
         ->setFormat(oboe::AudioFormat::Float)
         ->setChannelCount(2)
         ->setSampleRate(48000)
         ->setDataCallback(this);
        if (b.openStream(m_stream) != oboe::Result::OK) { LOGE("Oboe: cannot open stream"); return false; }
        m_sampleRate = m_stream->getSampleRate();
        const int burst = m_stream->getFramesPerBurst();
        m_stream->setBufferSizeInFrames(burst * 2);
        m_bufL.assign(8192, 0.0f); m_bufR.assign(8192, 0.0f);
        m_engine.prepare(m_sampleRate, 1024);
        LOGI("Oboe: %d Hz, burst %d", m_sampleRate, burst);
        return m_stream->requestStart() == oboe::Result::OK;
    }
    /** @brief Stops and closes the stream, if one is open; called once at shutdown. */
    void stop() { if (m_stream) { m_stream->requestStop(); m_stream->close(); m_stream.reset(); } }
    /** @brief The stream's rate in Hz (48000 until start() learned the real one); a recording is written at it. @return the sample rate */
    int sampleRate() const { return m_sampleRate; }
    /**
     * @brief Oboe's data callback, the audio thread: renders @p frames stereo frames into @p data.
     *
     * The engine renders into m_bufL / m_bufR in chunks of at most 4096 frames, the recorder is fed
     * the same chunk, and the two channels are interleaved into the stream's float buffer.
     *
     * @param data    the stream's output buffer, interleaved stereo float
     * @param frames  how many frames Oboe wants
     * @return        Continue, always; the stream stops only through stop()
     */
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream*, void* data, int32_t frames) override
    {
        float* out = static_cast<float*>(data);
        int done = 0;
        while (done < frames) {
            const int n = std::min(frames - done, 4096);
            m_engine.process(m_bufL.data(), m_bufR.data(), n);
            m_recorder.write(m_bufL.data(), m_bufR.data(), n);
            for (int i = 0; i < n; ++i) { out[(done + i) * 2] = m_bufL[static_cast<size_t>(i)]; out[(done + i) * 2 + 1] = m_bufR[static_cast<size_t>(i)]; }
            done += n;
        }
        return oboe::DataCallbackResult::Continue;
    }
private:
    Engine& m_engine;                              ///< the synthesizer, owned by App
    WavRecorder& m_recorder;                       ///< the recorder, owned by App; taps every rendered block
    std::shared_ptr<oboe::AudioStream> m_stream;   ///< the open Oboe stream, null before start() and after stop()
    std::vector<float> m_bufL,   ///< left scratch buffer the engine renders into, 8192 frames
                       m_bufR;   ///< right scratch buffer, the same size
    int m_sampleRate = 48000;                      ///< the rate the stream really opened at, in Hz
};

// ---------------------------------------------------------------- config

/**
 * @brief What ambient.cfg in the data folder may set; the defaults are what runs without a file.
 *
 * One `key=value` per line, keys as named in the file header; unknown keys are ignored. Read once
 * at start by readConfig(); nothing rereads it while the app runs.
 */
struct Config {
    std::string m_oscHost;      ///< `osc_host`: IPv4 address of the PC running the desktop plugin; empty = no bridge
    int m_oscPort = 9000;       ///< `osc_port`: the plugin's OSC port
    bool m_audio = true;        ///< `audio`: false (`audio=0`) means no on-device sound, a pure bridge
    std::string m_preset;       ///< `preset`: a preset applied at start, by its exact name; empty = the engine's init state
    std::string m_route;   ///< `route`: route preset name or route text for ROUTE PLAY/STOP
    float m_restZone = 0.08f;   ///< `rest_zone`: both hands below this share of the calibrated height = resting, nothing moves
};

/**
 * @brief Reads `<dir>/ambient.cfg` into a Config; the defaults where the file or a key is missing.
 *
 * Lines without `=` are skipped, trailing CR, LF and spaces are stripped from a line before the
 * split, and numbers are parsed with atoi / atof (a bad number reads as 0).
 *
 * @param dir  the app's external data folder, or null (then the defaults come back untouched)
 * @return     the settings found, over the defaults
 */
Config readConfig(const char* dir)
{
    Config c;
    if (dir == nullptr) return c;
    const std::string path = std::string(dir) + "/ambient.cfg";
    FILE* f = std::fopen(path.c_str(), "r");
    if (f == nullptr) { LOGI("no config at %s", path.c_str()); return c; }
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = s.substr(0, eq), v = s.substr(eq + 1);
        if (k == "osc_host") c.m_oscHost = v;
        else if (k == "osc_port") c.m_oscPort = std::atoi(v.c_str());
        else if (k == "audio") c.m_audio = v != "0";
        else if (k == "preset") c.m_preset = v;
        else if (k == "route") c.m_route = v;
        else if (k == "rest_zone") c.m_restZone = static_cast<float>(std::atof(v.c_str()));
    }
    std::fclose(f);
    return c;
}

// ---------------------------------------------------------------- GL scene

/**
 * @brief GLES 3.0 vertex shader of the point cloud: position, colour and size per vertex, one
 *        view-projection matrix.
 *
 * The point size is divided by the clip-space w (floored at 0.2) so a dot shrinks with distance
 * like a thing in the world rather than staying a fixed number of pixels.
 */
const char* kVertexShader = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aCol;
layout(location = 2) in float aSize;
uniform mat4 uVP;
out vec4 vCol;
void main() {
    vec4 p = uVP * vec4(aPos, 1.0);
    gl_Position = p;
    gl_PointSize = aSize / max(p.w, 0.2);
    vCol = aCol;
})";

/**
 * @brief GLES 3.0 fragment shader of the point cloud: a soft round dot.
 *
 * The radial smoothstep fades the square point sprite to a disc with a wide soft edge; colour and
 * alpha are premultiplied and the output alpha is 1, because Scene::draw blends additively
 * (GL_ONE, GL_ONE) and the colour's alpha is really its brightness.
 */
const char* kFragmentShader = R"(#version 300 es
precision mediump float;
in vec4 vCol;
out vec4 o;
void main() {
    vec2 d = gl_PointCoord - vec2(0.5);
    float r = length(d) * 2.0;
    float a = smoothstep(1.0, 0.15, r);
    o = vec4(vCol.rgb * a * vCol.a, 1.0);
})";

/**
 * @brief One vertex of the point cloud, laid out exactly as Scene::init binds it: position at
 *        offset 0, colour at 12, size at 28.
 */
struct Point {
    float m_x,      ///< position in metres, stage space: x right
    m_y,            ///< y up
    m_z;            ///< z back
    float m_r,      ///< red 0 .. 1
    m_g,            ///< green 0 .. 1
    m_b,            ///< blue 0 .. 1
    m_a;            ///< brightness of the additive dot, 0 .. 1 (not a coverage)
    float m_size;   ///< point size in pixels at clip w = 1, divided by the depth in the shader
};

/**
 * @brief Compiles one shader stage and logs the info log if it fails.
 * @param type  GL_VERTEX_SHADER or GL_FRAGMENT_SHADER
 * @param src   the GLSL ES source
 * @return      the shader name, returned even when compiling failed (the link then fails and Scene::init reports it)
 */
GLuint compile(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof(log), nullptr, log); LOGE("shader: %s", log); }
    return s;
}

/** @brief A head-locked text panel: origin in front of the eyes, axes from the head's yaw only. */
struct Panel {
    Vec3 m_origin,   ///< the panel's top-left corner in stage space
    m_right,         ///< unit axis along the text
    m_up;            ///< unit axis up the lines (world up)
};

/**
 * @brief The picture: a cloud of soft additive points, rebuilt every frame and drawn once per eye.
 *
 * Everything drawn is a Point -- the horizon ring, the sounding notes, the brain's root, the hands,
 * the morph bridge and the panel text. A frame is begin(), addWorld() and the overlay's addText()
 * calls, upload(), then draw() per eye with that eye's view-projection.
 */
class Scene {
public:
    /**
     * @brief Builds the program from the two shaders and the VAO/VBO with the Point layout.
     * @return false if the program did not link (the app then stops at init)
     */
    bool init()
    {
        m_program = glCreateProgram();
        glAttachShader(m_program, compile(GL_VERTEX_SHADER, kVertexShader));
        glAttachShader(m_program, compile(GL_FRAGMENT_SHADER, kFragmentShader));
        glLinkProgram(m_program);
        GLint ok = 0; glGetProgramiv(m_program, GL_LINK_STATUS, &ok);
        if (!ok) { LOGE("program link failed"); return false; }
        m_uVP = glGetUniformLocation(m_program, "uVP");
        glGenBuffers(1, &m_vbo);
        glGenVertexArrays(1, &m_vao);
        glBindVertexArray(m_vao);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Point), reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Point), reinterpret_cast<void*>(12));
        glEnableVertexAttribArray(2); glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(Point), reinterpret_cast<void*>(28));
        glBindVertexArray(0);
        return true;
    }

    /** @brief Starts a new frame's cloud: forgets last frame's points. */
    void begin() { m_points.clear(); }

    /**
     * @brief Adds the world of the current frame from what the engine and the hands report.
     *
     * A horizon ring of 64 dim dots at 2.5 m. Every sounding note (24 .. 107) around the listener by
     * its pitch class, its octave as height (0.35 m per octave around 0.5 m), its distance as radius
     * (1.2 m near, 5.2 m far), warm near and blue far, brightness and size following its envelope
     * level. The brain's root as an orange dot on the floor at 1 m. Each tracked palm as a dot that
     * turns from green to red with the pinch, and between two tracked palms a dotted bridge that
     * lights up as far as the morph position has gone.
     *
     * @param engine      the engine to read (soundingNotes, noteDistance, noteLevel, brainRoot, morphPosition); glue thread, lock-free reads
     * @param palms       the two palm positions in stage space, left then right
     * @param palmsValid  whether each palm was tracked this frame
     * @param pinch       each hand's pinch 0 .. 1
     */
    void addWorld(Engine& engine, const XrVector3f* palms, const bool* palmsValid, const float* pinch)
    {
        for (int i = 0; i < 64; ++i) {   // horizon ring
            const float a = static_cast<float>(i) / 64.0f * 6.2831853f;
            m_points.push_back({ 2.5f * std::sin(a), 0.02f, -2.5f * std::cos(a), 0.25f, 0.3f, 0.4f, 0.35f, 40.0f });
        }
        bool notes[128];
        engine.soundingNotes(notes);
        const int root = engine.brainRoot();
        for (int n = 24; n < 108; ++n) {
            if (!notes[n]) continue;
            const float d = std::fmax(0.0f, engine.noteDistance(n));
            const float lv = engine.noteLevel(n);
            const float ang = static_cast<float>(n % 12) / 12.0f * 6.2831853f;
            const float radius = 1.2f + 4.0f * d;
            const float y = 0.5f + (static_cast<float>(n / 12) - 3.0f) * 0.35f;
            const float warm = 1.0f - d;
            m_points.push_back({ radius * std::sin(ang), y, -radius * std::cos(ang),
                                0.5f + 0.5f * warm, 0.55f + 0.25f * warm, 1.0f - 0.5f * warm, 0.2f + 0.8f * lv, (120.0f + 160.0f * lv) * (1.0f - 0.5f * d) });
        }
        {
            const float ang = static_cast<float>(root % 12) / 12.0f * 6.2831853f;
            m_points.push_back({ 1.0f * std::sin(ang), 0.1f, -1.0f * std::cos(ang), 1.0f, 0.6f, 0.2f, 0.8f, 120.0f });
        }
        for (int h = 0; h < 2; ++h) {
            if (!palmsValid[h]) continue;
            const float p = pinch[h];
            m_points.push_back({ palms[h].x, palms[h].y, palms[h].z, 0.3f + 0.7f * p, 0.9f * (1.0f - p) + 0.2f, 0.4f * (1.0f - p), 0.95f, 90.0f });
        }
        if (palmsValid[0] && palmsValid[1]) {
            const float m = engine.morphPosition();
            for (int i = 1; i < 8; ++i) {
                const float t = static_cast<float>(i) / 8.0f;
                const float lit = (t <= m) ? 0.9f : 0.25f;
                m_points.push_back({ palms[0].x + (palms[1].x - palms[0].x) * t, palms[0].y + (palms[1].y - palms[0].y) * t, palms[0].z + (palms[1].z - palms[0].z) * t,
                                    0.9f, 0.5f, 0.8f, lit, 35.0f });
            }
        }
    }

    /**
     * @brief Text on the panel; (u, v) in metres from the panel origin, cell = dot spacing in metres.
     *
     * One dot per set bit of the 5x7 glyph, a character every six cells; the dots are sized to the
     * cell so the text stays legible whatever the cell.
     *
     * @param p     the panel giving origin and axes (App::headPanel)
     * @param u     start along the panel's right axis, metres
     * @param v     start along the panel's up axis, metres (the glyph hangs down from it)
     * @param cell  dot spacing in metres (0.006 for the menu)
     * @param text  the string; characters the font lacks become blanks
     * @param r     red 0 .. 1
     * @param g     green 0 .. 1
     * @param b     blue 0 .. 1
     * @param a     brightness 0 .. 1 (the additive alpha)
     */
    void addText(const Panel& p, float u, float v, float cell, const char* text, float r, float g, float b, float a)
    {
        for (const char* c = text; *c; ++c) {
            const unsigned char* gl = glyph(*c);
            for (int row = 0; row < 7; ++row)
                for (int col = 0; col < 5; ++col)
                    if (gl[row] & (0x10 >> col)) {
                        const float x = u + static_cast<float>(col) * cell, y = v - static_cast<float>(row) * cell;
                        m_points.push_back({ p.m_origin.m_x + p.m_right.m_x * x + p.m_up.m_x * y, p.m_origin.m_y + p.m_right.m_y * x + p.m_up.m_y * y, p.m_origin.m_z + p.m_right.m_z * x + p.m_up.m_z * y,
                                            r, g, b, a, cell * 900.0f });
                    }
            u += 6.0f * cell;
        }
    }

    /** @brief Sends this frame's points to the VBO (GL_DYNAMIC_DRAW); once per frame, before the eyes are drawn. */
    void upload()
    {
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_points.size() * sizeof(Point)), m_points.data(), GL_DYNAMIC_DRAW);
    }

    /**
     * @brief Draws the uploaded cloud into the bound framebuffer: near-black clear, additive blend,
     *        no depth test.
     * @param vp      the eye's projection * view
     * @param width   the swapchain image's width in pixels
     * @param height  its height in pixels
     */
    void draw(const Mat4& vp, int width, int height)
    {
        glViewport(0, 0, width, height);
        glClearColor(0.02f, 0.02f, 0.04f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glUseProgram(m_program);
        glUniformMatrix4fv(m_uVP, 1, GL_FALSE, vp.m_m);
        glBindVertexArray(m_vao);
        glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(m_points.size()));
        glBindVertexArray(0);
    }
private:
    GLuint m_program = 0,   ///< the linked program
           m_vbo = 0,       ///< the vertex buffer the points are uploaded to
           m_vao = 0;       ///< the vertex array with the Point layout
    GLint m_uVP = -1;                           ///< location of the uVP uniform
    std::vector<Point> m_points;                ///< this frame's cloud, cleared by begin(), uploaded by upload()
};

// ---------------------------------------------------------------- the app

/** @brief One eye's render target: its OpenXR swapchain, the GLES textures behind it, and a framebuffer to draw them through. */
struct SwapchainTarget {
    XrSwapchain m_swapchain = XR_NULL_HANDLE;              ///< the eye's colour swapchain
    int m_width = 0,    ///< the swapchain image width in pixels, the runtime's recommended size for the view
        m_height = 0;   ///< its height in pixels
    std::vector<XrSwapchainImageOpenGLESKHR> m_images;     ///< the swapchain's images as GL texture names, indexed by the acquired index
    GLuint m_fbo = 0,     ///< the framebuffer the acquired image is attached to per frame
           m_depth = 0;   ///< its 24-bit depth renderbuffer
};

/**
 * @brief The application: OpenXR setup, the frame loop, the hands and the head into the engine,
 *        the menu, persistence, and the sound.
 *
 * Built once on the heap by android_main. init() reads the data folder (packs, config, calibration,
 * texture, wavetable, impulse), brings up the loader, instance, EGL, session and hand trackers, the
 * scene and the audio, and starts a calibration when there is no calib.txt. run() is the loop:
 * looper events, OpenXR events, one frame while the session runs. Each frame locates the hands and
 * the head, feeds them to the GestureLayer (which writes the engine's parameters) and the HandMenu,
 * steers the preset map when it is on, then renders the scene for both eyes. shutdown() tears it
 * all down in reverse.
 */
class App {
public:
    /**
     * @brief Remembers the glue's android_app; nothing is created until init().
     * @param app  the NativeActivity glue's application object
     */
    explicit App(android_app* app) : m_app(app) {}

    /**
     * @brief Brings everything up, in the order the dependencies demand.
     *
     * Preset packs first, so the config's preset name can be found among them; then the config, the
     * calibration and the folder's source files, then the preset's own files so they win over the
     * folder's; the Room's maximum impulse length before the engine is prepared; the bridge socket;
     * then loader, instance, EGL, session, hands, scene and audio. Once the audio has started the
     * engine is prepared and the impulses that waited go in. Missing hand tracking and a failed
     * audio start are logged and survived; a failed OpenXR or GL step is fatal.
     *
     * @return false when the app cannot run (loader, instance, EGL, session or the GL program failed)
     */
    bool init()
    {
        m_dataDir = m_app->activity->externalDataPath ? m_app->activity->externalDataPath : "";
        // Preset packs from <externalDataPath>/Packs, before anything looks a preset up by name.
        if (!m_dataDir.empty()) {
            const int packs = loadPresetPacksIn((m_dataDir + "/Packs").c_str());
            if (packs > 0) LOGI("Packs: %d pack(s), %d presets in total", packs, numPresets());
        }
        m_config = readConfig(m_dataDir.c_str());
        if (!m_config.m_preset.empty())
            for (int i = 0; i < numPresets(); ++i) if (m_config.m_preset == preset(i).m_name) { m_engine.applyPreset(i); m_presetA = m_presetB = i; }
        loadCalibration();
        loadSourceFiles();
        // After loadSourceFiles, so a pack preset's own sample wins over the folder's texture.wav.
        if (!m_config.m_preset.empty()) loadPresetFiles(m_presetA);
        m_gestures.setRestZone(m_config.m_restZone);
        m_engine.setRoomMaxSeconds(4.0f);   // a 4 s convolution is about a third of one XR2 core; 8 s would be two thirds
        if (!m_config.m_oscHost.empty()) {
            if (m_osc.open(m_config.m_oscHost, m_config.m_oscPort)) LOGI("bridge: OSC to %s:%d", m_config.m_oscHost.c_str(), m_config.m_oscPort);
            else LOGE("bridge: bad host %s", m_config.m_oscHost.c_str());
        }
        if (!initLoader()) return false;
        if (!initInstance()) return false;
        if (!initEgl()) return false;
        if (!initSession()) return false;
        if (!initHands()) LOGE("hand tracking unavailable");
        if (!m_scene.init()) return false;
        if (m_config.m_audio && !m_audio.start()) LOGE("audio failed to start");
        m_audioStarted = true;        // the engine is prepared now; the convolver has its buffers
        applyPendingImpulses();
        if (!m_calibrated) { m_gestures.startCalibration(8.0f); LOGI("no calibration file: calibrating for 8 s"); }
        return true;
    }

    /**
     * @brief A pack preset may bring its own sample, wavetable and impulse; the paths sit next to the pack.
     *
     * The texture goes straight into the engine, the wavetable too; the impulses wait in m_pendingIr
     * / m_pendingIrB until the engine is prepared, or go in at once when the audio already runs.
     * Called from init() for the config's preset and from applyMenu() when A changes without morph.
     *
     * @param index  the preset's index in the preset list (presetFilePath reads its four file fields)
     */
    void loadPresetFiles(int index)
    {
        std::vector<float> mono; int rate = 0;
        const char* tex = presetFilePath(index, 0);
        if (tex != nullptr && *tex) {
            // One path fills every slot; up to four separated by ';' fill one slot each, an empty
            // one meaning that slot has none -- the same reading the plugin and the render tool give.
            const std::string all(tex);
            const bool perSlot = all.find(';') != std::string::npos;
            size_t start = 0;
            for (int slot = 0; slot < ambient::Slots; ++slot) {
                const size_t semi = all.find(';', start);
                std::string one = all.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
                while (!one.empty() && one.front() == ' ') one.erase(one.begin());
                while (!one.empty() && one.back() == ' ') one.pop_back();
                std::vector<float> right;
                if (!one.empty() && readWavStereo(one.c_str(), mono, right, rate)) {
                    const double base = baseHzFromName(one.c_str());
                    const bool seamless = loopFromName(one.c_str());
                    const float* rp = right.empty() ? nullptr : right.data();
                    if (perSlot) m_engine.setTexture(slot, mono.data(), rp, static_cast<int>(mono.size()), rate, base > 0.0 ? base : 261.6256, seamless);
                    else         m_engine.setTexture(mono.data(), rp, static_cast<int>(mono.size()), rate, base > 0.0 ? base : 261.6256, seamless);
                    LOGI("preset texture %d: %s", slot + 1, one.c_str());
                }
                if (semi == std::string::npos) break;
                start = semi + 1;
            }
        }
        const char* tab = presetFilePath(index, 1);
        int cycle = 0;
        if (tab != nullptr && *tab && readWavetableFile(tab, mono, cycle))
            if (!m_engine.loadUserWavetable(mono.data(), static_cast<int>(mono.size()), cycle))
                LOGE("%s: not a usable wavetable", tab);
        const char* imp = presetFilePath(index, 2);
        const char* impB = presetFilePath(index, 3);
        std::vector<std::vector<float>> ir;
        if (imp != nullptr && *imp && readWavChannels(imp, ir, rate) && !ir.empty()) {
            m_pendingIr = ir; m_pendingIrRate = rate;   // set once the engine is prepared
            LOGI("preset impulse %s", imp);
            m_pendingIrB.clear();                      // a preset that names its room and no B plays A alone
            m_clearIrB = true;
        }
        if (impB != nullptr && *impB && readWavChannels(impB, ir, rate) && !ir.empty()) {
            m_pendingIrB = ir; m_pendingIrBRate = rate; m_clearIrB = false;
            LOGI("preset impulse B %s", impB);
        }
        // A preset picked from the menu arrives after the audio started: its rooms go in now. They
        // used to wait for a start that had already happened, and the Room kept the first preset's.
        if (m_audioStarted) applyPendingImpulses();
    }

    /**
     * @brief Message thread, engine prepared: the impulses a preset or the data folder asked for.
     *
     * Hands m_pendingIr and m_pendingIrB to the engine and clears them; when a preset named a room and
     * no B (m_clearIrB), the engine's B impulse is cleared instead so the Room plays A alone.
     */
    void applyPendingImpulses()
    {
        if (!m_pendingIr.empty()) {
            m_engine.setImpulse(m_pendingIr[0].data(), m_pendingIr.size() > 1 ? m_pendingIr[1].data() : nullptr, static_cast<int>(m_pendingIr[0].size()), m_pendingIrRate);
            m_pendingIr.clear();
        }
        if (!m_pendingIrB.empty()) {
            m_engine.setImpulseB(m_pendingIrB[0].data(), m_pendingIrB.size() > 1 ? m_pendingIrB[1].data() : nullptr, static_cast<int>(m_pendingIrB[0].size()), m_pendingIrBRate);
            m_pendingIrB.clear();
        } else if (m_clearIrB) {
            m_engine.clearImpulseB();
        }
        m_clearIrB = false;
    }

    /**
     * @brief texture.wav (a field recording etc., assumed at C4 for Pitch = Note) and wavetable.wav
     *        (2048-sample frames) in the data folder feed the Texture and User-table source slots.
     *
     * Also impulse.wav, which waits in m_pendingIr for the engine to be prepared. Once at init(),
     * before the config preset's own files, so those take precedence.
     */
    void loadSourceFiles()
    {
        if (m_dataDir.empty()) return;
        std::vector<float> mono; int rate = 0;
        // texture.wav, or any texture*.wav (a "_A3" suffix from TextureGen sets the base pitch).
        std::string texPath;
        if (DIR* d = opendir(m_dataDir.c_str())) {
            while (dirent* e = readdir(d)) {
                const std::string n = e->d_name;
                if (n.rfind("texture", 0) == 0 && n.size() > 4 && n.compare(n.size() - 4, 4, ".wav") == 0) { texPath = m_dataDir + "/" + n; if (n == "texture.wav") break; }
            }
            closedir(d);
        }
        std::vector<float> texRight;
        if (!texPath.empty() && readWavStereo(texPath.c_str(), mono, texRight, rate)) {
            const double base = baseHzFromName(texPath.c_str());
            m_engine.setTexture(mono.data(), texRight.empty() ? nullptr : texRight.data(),
                               static_cast<int>(mono.size()), rate, base > 0.0 ? base : 261.6256,
                               loopFromName(texPath.c_str()));
            LOGI("%s: %.1f s @ %d Hz, base %.1f Hz", texPath.c_str(), mono.size() / static_cast<double>(rate), rate, base > 0.0 ? base : 261.6256);
        }
        std::vector<std::vector<float>> ir;
        if (readWavChannels((m_dataDir + "/impulse.wav").c_str(), ir, rate) && !ir.empty()) {
            m_pendingIr = ir; m_pendingIrRate = rate;   // set after the engine is prepared (audio start)
            LOGI("impulse.wav: %zu ch, %.2f s @ %d Hz", ir.size(), ir[0].size() / static_cast<double>(rate), rate);
        }
        int cycle = 0;
        if (readWavetableFile((m_dataDir + "/wavetable.wav").c_str(), mono, cycle)) {
            if (m_engine.loadUserWavetable(mono.data(), static_cast<int>(mono.size()), cycle)) LOGI("wavetable.wav: cycles of %d samples", cycle);
            else LOGE("wavetable.wav: not a usable wavetable");
        }
    }

    /**
     * @brief The main loop until Android asks to destroy the activity or OpenXR asks to quit.
     *
     * Looper events are polled without blocking while the session runs (or before the window
     * exists) and blocking otherwise, then OpenXR events, then one frame while the session runs.
     */
    void run()
    {
        while (!m_app->destroyRequested) {
            int events; android_poll_source* source;
            const int timeout = (m_sessionRunning || m_app->window == nullptr) ? 0 : -1;
            while (ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
                if (source) source->process(m_app, source);
                if (m_app->destroyRequested) break;
            }
            pollXrEvents();
            if (m_quit) break;
            if (m_sessionRunning) frame();
        }
    }

    /** @brief Stops the recorder and the audio and destroys swapchains, hand trackers, spaces, session, instance and the socket. */
    void shutdown()
    {
        m_recorder.stop();
        m_audio.stop();
        for (auto& t : m_targets) { if (t.m_swapchain != XR_NULL_HANDLE) xrDestroySwapchain(t.m_swapchain); }
        for (int h = 0; h < 2; ++h) if (m_handTracker[h] != XR_NULL_HANDLE && m_pfnDestroyHandTracker) m_pfnDestroyHandTracker(m_handTracker[h]);
        if (m_viewSpace != XR_NULL_HANDLE) xrDestroySpace(m_viewSpace);
        if (m_stageSpace != XR_NULL_HANDLE) xrDestroySpace(m_stageSpace);
        if (m_session != XR_NULL_HANDLE) xrDestroySession(m_session);
        if (m_instance != XR_NULL_HANDLE) xrDestroyInstance(m_instance);
        m_osc.close();
    }

private:
    // ------------------------------------------------ persistence

    /**
     * @brief Reads the first line of calib.txt into the gesture layer; m_calibrated says whether it parsed.
     *
     * Without a file nothing is logged and m_calibrated stays false, which makes init() start the
     * calibration gesture.
     */
    void loadCalibration()
    {
        if (m_dataDir.empty()) return;
        FILE* f = std::fopen((m_dataDir + "/calib.txt").c_str(), "r");
        if (f == nullptr) return;
        char line[256] = {};
        if (std::fgets(line, sizeof(line), f)) m_calibrated = m_gestures.parseCalibration(line);
        std::fclose(f);
        LOGI("calibration %s", m_calibrated ? "loaded" : "invalid");
    }

    /**
     * @brief Writes the gesture layer's ranges to calib.txt and marks the app calibrated.
     *
     * Called from frame() the moment a calibration ends, whether the first one or one from the menu.
     */
    void saveCalibration()
    {
        if (m_dataDir.empty()) return;
        char text[128];
        m_gestures.writeCalibration(text, sizeof(text));
        FILE* f = std::fopen((m_dataDir + "/calib.txt").c_str(), "w");
        if (f) { std::fputs(text, f); std::fclose(f); }
        m_calibrated = true;
        LOGI("calibration saved: %s", text);
    }

    // ------------------------------------------------ menu actions

    /**
     * @brief Carries out an item the hand menu fired.
     *
     * Morph on/off; A = now / B = now (capture the live parameters into a slot); previous/next
     * preset into slot A or B (the preset's values go into the morph slot, and without morph a new A
     * is applied outright, files included); the preset map on/off; the route from ambient.cfg (or the
     * first route preset) playing/stopped; recording start/stop to `rec-YYYYMMDD-HHMMSS.wav` in the
     * data folder; and an 8-second calibration.
     *
     * @param a  the fired item, never None
     */
    void applyMenu(MenuActionE a)
    {
        switch (a) {
        case MenuActionE::TOGGLE_MORPH:
            m_engine.setParam(ParamIdE::MORPH_ACTIVE, m_engine.getParam(ParamIdE::MORPH_ACTIVE) >= 0.5f ? 0.0f : 1.0f);
            break;
        case MenuActionE::CAPTURE_A: m_engine.captureMorphSlot(0); m_slotName[0] = "NOW"; break;
        case MenuActionE::CAPTURE_B: m_engine.captureMorphSlot(1); m_slotName[1] = "NOW"; break;
        case MenuActionE::PRESET_A_PREV: case MenuActionE::PRESET_A_NEXT: case MenuActionE::PRESET_B_PREV: case MenuActionE::PRESET_B_NEXT: {
            const int slot = (a == MenuActionE::PRESET_A_PREV || a == MenuActionE::PRESET_A_NEXT) ? 0 : 1;
            int& idx = slot == 0 ? m_presetA : m_presetB;
            idx = (idx + ((a == MenuActionE::PRESET_A_NEXT || a == MenuActionE::PRESET_B_NEXT) ? 1 : numPresets() - 1)) % numPresets();
            float values[NumParams];
            for (int i = 0; i < NumParams; ++i) values[i] = m_engine.getParam(static_cast<ParamIdE>(i));
            applyPreset(preset(idx), [&](ParamIdE id, float v) { values[static_cast<int>(id)] = v; });
            m_engine.setMorphSlot(slot, values);
            m_slotName[slot] = preset(idx).m_name;
            if (m_engine.getParam(ParamIdE::MORPH_ACTIVE) < 0.5f && slot == 0) { m_engine.applyPreset(idx); loadPresetFiles(idx); }   // without morph, A is what plays
            break;
        }
        case MenuActionE::TOGGLE_MAP: {
            // Map on: the left hand's reach (x) and height (y) steer the cursor over the preset map,
            // the engine glides toward the blend of the presets around it. Map off: keep what the
            // map left behind by copying the gliding values into the live parameters.
            const bool on = m_engine.getParam(ParamIdE::MAP_ACTIVE) >= 0.5f;
            if (on) {
                for (const ParamDesc& d : paramTable())
                    if (!isMapParam(d.m_id) && !isMorphParam(d.m_id) && !isMacroParam(d.m_id)) m_engine.setParam(d.m_id, m_engine.blendValue(d.m_id));
            }
            m_engine.setParam(ParamIdE::MAP_ACTIVE, on ? 0.0f : 1.0f);
            LOGI("preset map %s", on ? "off" : "on");
            break;
        }
        case MenuActionE::TOGGLE_ROUTE: {
            // Plays the route from ambient.cfg (route=<preset name or text>), or the first route preset.
            if (m_engine.routeEdit().count() == 0) {
                const char* text = routePreset(0).m_points;
                for (int r = 0; r < numRoutePresets(); ++r) if (m_config.m_route == routePreset(r).m_name) text = routePreset(r).m_points;
                if (!m_config.m_route.empty() && !m_engine.setRouteText(m_config.m_route.c_str())) m_engine.setRouteText(text);
                else if (m_config.m_route.empty()) m_engine.setRouteText(text);
            }
            const bool on = m_engine.getParam(ParamIdE::ROUTE_ACTIVE) >= 0.5f;
            m_engine.setParam(ParamIdE::ROUTE_ACTIVE, on ? 0.0f : 1.0f);
            LOGI("route %s (%d points)", on ? "stopped" : "playing", m_engine.routeEdit().count());
            break;
        }
        case MenuActionE::TOGGLE_RECORD:
            if (m_recorder.recording()) { m_recorder.stop(); LOGI("recording stopped, %.1f s", m_recorder.seconds()); }
            else if (!m_dataDir.empty()) {
                char name[64]; const std::time_t t = std::time(nullptr); std::strftime(name, sizeof(name), "rec-%Y%m%d-%H%M%S.wav", std::localtime(&t));
                if (m_recorder.start((m_dataDir + "/" + name).c_str(), m_audio.sampleRate(), 2)) LOGI("recording to %s", name);
                else LOGE("cannot start recording");
            }
            break;
        case MenuActionE::CALIBRATE:
            m_gestures.startCalibration(8.0f);
            break;
        default: break;
        }
    }

    // ------------------------------------------------ OpenXR setup

    /**
     * @brief Initialises the OpenXR loader with the JavaVM and the activity, as Android requires before any other call.
     * @return false if xrInitializeLoaderKHR is not there or fails
     */
    bool initLoader()
    {
        PFN_xrInitializeLoaderKHR initLoader = nullptr;
        xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", reinterpret_cast<PFN_xrVoidFunction*>(&initLoader));
        if (initLoader == nullptr) { LOGE("no xrInitializeLoaderKHR"); return false; }
        XrLoaderInitInfoAndroidKHR li{ XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
        li.applicationVM = m_app->activity->vm;
        li.applicationContext = m_app->activity->clazz;
        return XR_SUCCEEDED(initLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&li)));
    }

    /**
     * @brief Creates the instance with the Android, OpenGL ES and hand-tracking extensions, finds the
     *        HMD system and asks whether it tracks hands.
     * @return false if the instance or the system could not be obtained
     */
    bool initInstance()
    {
        const char* exts[] = { XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME, XR_EXT_HAND_TRACKING_EXTENSION_NAME };
        XrInstanceCreateInfoAndroidKHR android{ XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
        android.applicationVM = m_app->activity->vm;
        android.applicationActivity = m_app->activity->clazz;
        XrInstanceCreateInfo ci{ XR_TYPE_INSTANCE_CREATE_INFO };
        ci.next = &android;
        std::strncpy(ci.applicationInfo.applicationName, "Noctuary", XR_MAX_APPLICATION_NAME_SIZE - 1);
        ci.applicationInfo.applicationVersion = 1;
        std::strncpy(ci.applicationInfo.engineName, "NoctuaryCore", XR_MAX_ENGINE_NAME_SIZE - 1);
        ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        ci.enabledExtensionCount = 3;
        ci.enabledExtensionNames = exts;
        if (XR_FAILED(xrCreateInstance(&ci, &m_instance))) { LOGE("xrCreateInstance failed"); return false; }

        XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO };
        sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        if (XR_FAILED(xrGetSystem(m_instance, &sgi, &m_system))) { LOGE("xrGetSystem failed"); return false; }
        XrSystemHandTrackingPropertiesEXT ht{ XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT };
        XrSystemProperties sp{ XR_TYPE_SYSTEM_PROPERTIES, &ht };
        xrGetSystemProperties(m_instance, m_system, &sp);
        m_handsSupported = ht.supportsHandTracking == XR_TRUE;
        LOGI("system: %s, hand tracking %d", sp.systemName, m_handsSupported ? 1 : 0);
        return true;
    }

    /**
     * @brief Brings up EGL: queries the runtime's GLES requirements, initialises the default display,
     *        picks an RGBA8 GLES 3 config and makes a context current on a 16x16 pbuffer.
     *
     * The pbuffer is never shown; all drawing goes into the swapchain images through framebuffers.
     *
     * @return false if any EGL step fails
     */
    bool initEgl()
    {
        PFN_xrGetOpenGLESGraphicsRequirementsKHR getReq = nullptr;
        xrGetInstanceProcAddr(m_instance, "xrGetOpenGLESGraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction*>(&getReq));
        XrGraphicsRequirementsOpenGLESKHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
        if (getReq == nullptr || XR_FAILED(getReq(m_instance, m_system, &req))) { LOGE("GLES requirements failed"); return false; }

        m_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        EGLint major, minor;
        if (!eglInitialize(m_display, &major, &minor)) { LOGE("eglInitialize failed"); return false; }
        const EGLint attribs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                   EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 0, EGL_NONE };
        EGLint count = 0;
        if (!eglChooseConfig(m_display, attribs, &m_config_egl, 1, &count) || count == 0) { LOGE("eglChooseConfig failed"); return false; }
        const EGLint pbuf[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
        m_surface = eglCreatePbufferSurface(m_display, m_config_egl, pbuf);
        const EGLint ctx[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
        m_context = eglCreateContext(m_display, m_config_egl, EGL_NO_CONTEXT, ctx);
        if (m_context == EGL_NO_CONTEXT || !eglMakeCurrent(m_display, m_surface, m_surface, m_context)) { LOGE("EGL context failed"); return false; }
        LOGI("EGL %d.%d, GL %s", major, minor, glGetString(GL_VERSION));
        return true;
    }

    /**
     * @brief Creates the session on the EGL context, the reference spaces, and one swapchain with
     *        framebuffer and depth buffer per stereo view.
     *
     * Stage space is preferred and local space the fallback; a view space is made for the head.
     * The swapchain format is GL_SRGB8_ALPHA8 when the runtime offers it, else its first format.
     *
     * @return false if the session, a reference space or a swapchain could not be created
     */
    bool initSession()
    {
        XrGraphicsBindingOpenGLESAndroidKHR gb{ XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
        gb.display = m_display; gb.config = m_config_egl; gb.context = m_context;
        XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO, &gb };
        sci.systemId = m_system;
        if (XR_FAILED(xrCreateSession(m_instance, &sci, &m_session))) { LOGE("xrCreateSession failed"); return false; }

        XrReferenceSpaceCreateInfo rs{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
        rs.poseInReferenceSpace.orientation.w = 1.0f;
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        if (XR_FAILED(xrCreateReferenceSpace(m_session, &rs, &m_stageSpace))) {
            rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            if (XR_FAILED(xrCreateReferenceSpace(m_session, &rs, &m_stageSpace))) { LOGE("no reference space"); return false; }
        }
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        xrCreateReferenceSpace(m_session, &rs, &m_viewSpace);

        uint32_t viewCount = 0;
        xrEnumerateViewConfigurationViews(m_instance, m_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
        std::vector<XrViewConfigurationView> cfg(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
        xrEnumerateViewConfigurationViews(m_instance, m_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, cfg.data());
        m_views.assign(viewCount, { XR_TYPE_VIEW });

        uint32_t fmtCount = 0;
        xrEnumerateSwapchainFormats(m_session, 0, &fmtCount, nullptr);
        std::vector<int64_t> formats(fmtCount);
        xrEnumerateSwapchainFormats(m_session, fmtCount, &fmtCount, formats.data());
        int64_t format = formats.empty() ? GL_RGBA8 : formats[0];
        for (int64_t f : formats) if (f == GL_SRGB8_ALPHA8) { format = f; break; }

        m_targets.resize(viewCount);
        for (uint32_t v = 0; v < viewCount; ++v) {
            SwapchainTarget& t = m_targets[v];
            XrSwapchainCreateInfo sc{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
            sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            sc.format = format;
            sc.sampleCount = 1;
            sc.width = cfg[v].recommendedImageRectWidth;
            sc.height = cfg[v].recommendedImageRectHeight;
            sc.faceCount = 1; sc.arraySize = 1; sc.mipCount = 1;
            if (XR_FAILED(xrCreateSwapchain(m_session, &sc, &t.m_swapchain))) { LOGE("xrCreateSwapchain failed"); return false; }
            t.m_width = static_cast<int>(sc.width); t.m_height = static_cast<int>(sc.height);
            uint32_t imgCount = 0;
            xrEnumerateSwapchainImages(t.m_swapchain, 0, &imgCount, nullptr);
            t.m_images.assign(imgCount, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR });
            xrEnumerateSwapchainImages(t.m_swapchain, imgCount, &imgCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(t.m_images.data()));
            glGenFramebuffers(1, &t.m_fbo);
            glGenRenderbuffers(1, &t.m_depth);
            glBindRenderbuffer(GL_RENDERBUFFER, t.m_depth);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, t.m_width, t.m_height);
            LOGI("view %u: %dx%d, %u images", v, t.m_width, t.m_height, imgCount);
        }
        return true;
    }

    /**
     * @brief Resolves the XR_EXT_hand_tracking entry points and creates a tracker per hand.
     * @return false if the system does not track hands, the functions are missing or a tracker fails; the app then runs without hands
     */
    bool initHands()
    {
        if (!m_handsSupported) return false;
        xrGetInstanceProcAddr(m_instance, "xrCreateHandTrackerEXT", reinterpret_cast<PFN_xrVoidFunction*>(&m_pfnCreateHandTracker));
        xrGetInstanceProcAddr(m_instance, "xrLocateHandJointsEXT", reinterpret_cast<PFN_xrVoidFunction*>(&m_pfnLocateHandJoints));
        xrGetInstanceProcAddr(m_instance, "xrDestroyHandTrackerEXT", reinterpret_cast<PFN_xrVoidFunction*>(&m_pfnDestroyHandTracker));
        if (!m_pfnCreateHandTracker || !m_pfnLocateHandJoints) return false;
        for (int h = 0; h < 2; ++h) {
            XrHandTrackerCreateInfoEXT hci{ XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT };
            hci.hand = (h == 0) ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
            hci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
            if (XR_FAILED(m_pfnCreateHandTracker(m_session, &hci, &m_handTracker[h]))) return false;
        }
        return true;
    }

    /**
     * @brief Drains the OpenXR event queue and follows the session state.
     *
     * READY begins the session (m_sessionRunning), STOPPING ends it, EXITING and LOSS_PENDING -- as
     * an instance loss -- set m_quit so run() returns.
     */
    void pollXrEvents()
    {
        XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
        while (xrPollEvent(m_instance, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto* sc = reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev);
                m_sessionState = sc->state;
                switch (m_sessionState) {
                case XR_SESSION_STATE_READY: {
                    XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (XR_SUCCEEDED(xrBeginSession(m_session, &bi))) m_sessionRunning = true;
                    break;
                }
                case XR_SESSION_STATE_STOPPING:
                    xrEndSession(m_session);
                    m_sessionRunning = false;
                    break;
                case XR_SESSION_STATE_EXITING:
                case XR_SESSION_STATE_LOSS_PENDING:
                    m_quit = true;
                    break;
                default: break;
                }
            } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                m_quit = true;
            }
            ev = { XR_TYPE_EVENT_DATA_BUFFER };
        }
    }

    // ------------------------------------------------ per frame

    /**
     * @brief Locates both hands' joints and turns them into the gesture layer's inputs.
     *
     * Per hand: the palm's position in stage space, a pinch of 1 when thumb tip and index tip are
     * 15 mm apart or closer falling to 0 at 50 mm, and the palm's tilt (the world y of its sideways
     * axis, -1 .. 1). A hand that is not tracked, not active or without a valid palm pose is marked
     * invalid and not sent. In bridge mode the same five numbers go out as `/ambient/hand/L` and `/R`.
     *
     * @param time  the frame's predicted display time, the instant the joints are located for
     */
    void updateHands(XrTime time)
    {
        for (int h = 0; h < 2; ++h) {
            m_palmValid[h] = false;
            if (m_handTracker[h] == XR_NULL_HANDLE) continue;
            XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT];
            XrHandJointLocationsEXT locs{ XR_TYPE_HAND_JOINT_LOCATIONS_EXT };
            locs.jointCount = XR_HAND_JOINT_COUNT_EXT;
            locs.jointLocations = joints;
            XrHandJointsLocateInfoEXT li{ XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT };
            li.baseSpace = m_stageSpace;
            li.time = time;
            if (XR_FAILED(m_pfnLocateHandJoints(m_handTracker[h], &li, &locs)) || !locs.isActive) continue;
            const XrHandJointLocationEXT& palm = joints[XR_HAND_JOINT_PALM_EXT];
            const XrHandJointLocationEXT& thumb = joints[XR_HAND_JOINT_THUMB_TIP_EXT];
            const XrHandJointLocationEXT& index = joints[XR_HAND_JOINT_INDEX_TIP_EXT];
            const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            if ((palm.locationFlags & need) != need) continue;
            const float dx = thumb.pose.position.x - index.pose.position.x, dy = thumb.pose.position.y - index.pose.position.y, dz = thumb.pose.position.z - index.pose.position.z;
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float pinch = std::fmax(0.0f, std::fmin(1.0f, 1.0f - (dist - 0.015f) / 0.035f));
            const Mat4 r = rotationFromQuat(palm.pose.orientation);
            const float tilt = std::fmax(-1.0f, std::fmin(1.0f, r.m_m[1]));   // world y of the palm's sideways axis
            m_palms[h] = palm.pose.position;
            m_palmValid[h] = true;
            m_pinch[h] = pinch;
            m_gestures.setHand(h, palm.pose.position.x, palm.pose.position.y, palm.pose.position.z, pinch, tilt);
            if (m_osc.ok()) {
                const float args[5] = { palm.pose.position.x, palm.pose.position.y, palm.pose.position.z, pinch, tilt };
                m_osc.send(h == 0 ? "/ambient/hand/L" : "/ambient/hand/R", args, 5);
            }
        }
    }

    /**
     * @brief Locates the head (the view space in the stage space) and passes its yaw, pitch and roll on.
     *
     * The gesture layer gets all three, the engine the yaw for its binaural mode, the bridge the three
     * as `/ambient/head`. m_headPose and m_headValid feed the panel.
     *
     * @param time  the frame's predicted display time
     */
    void updateHead(XrTime time)
    {
        m_headValid = false;
        if (m_viewSpace == XR_NULL_HANDLE) return;
        XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
        if (XR_FAILED(xrLocateSpace(m_viewSpace, m_stageSpace, time, &loc))) return;
        if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return;
        m_headPose = loc.pose;
        m_headValid = true;
        float yaw, pitch, roll;
        quatToEulerDeg(loc.pose.orientation, yaw, pitch, roll);
        m_gestures.setHead(yaw, pitch, roll);
        m_engine.setHeadYaw(yaw);   // the binaural mode turns the field against the head
        if (m_osc.ok()) { const float args[3] = { yaw, pitch, roll }; m_osc.send("/ambient/head", args, 3); }
    }

    /**
     * @brief The panel of this frame: 0.9 m ahead of the head along its yaw, 0.12 m above it and
     *        0.22 m to the left, level with the floor.
     * @return the panel's origin and axes in stage space
     */
    Panel headPanel() const
    {
        // In front of the eyes, following yaw only (a panel that tilts with the head is tiring).
        Panel p;
        const Vec3 fwd = rotate(m_headPose.orientation, { 0.0f, 0.0f, -1.0f });
        const float len = std::fmax(std::sqrt(fwd.m_x * fwd.m_x + fwd.m_z * fwd.m_z), 1e-3f);
        const Vec3 f{ fwd.m_x / len, 0.0f, fwd.m_z / len };
        p.m_right = { -f.m_z, 0.0f, f.m_x };
        p.m_up = { 0.0f, 1.0f, 0.0f };
        p.m_origin = { m_headPose.position.x + f.m_x * 0.9f - p.m_right.m_x * 0.22f, m_headPose.position.y + 0.12f, m_headPose.position.z + f.m_z * 0.9f - p.m_right.m_z * 0.22f };
        return p;
    }

    /**
     * @brief Adds the head-locked text of this frame to the scene.
     *
     * While calibrating: the instructions and the progress in percent. With the menu closed: a
     * `REC m:ss` line while recording, else nothing. With the menu open (its openness fades the
     * text in): every item with its state appended (ON/OFF, PLAYING/STOPPED, START/STOP), the
     * highlighted one brighter, and above them the names in slots A and B. Nothing without a head pose.
     */
    void addOverlay()
    {
        if (!m_headValid) return;
        const Panel p = headPanel();
        const float cell = 0.006f;
        if (m_gestures.calibrating()) {
            m_scene.addText(p, 0.0f, 0.0f, cell, "CALIBRATING", 1.0f, 0.8f, 0.3f, 1.0f);
            m_scene.addText(p, 0.0f, -0.06f, cell, "HANDS TOGETHER AND APART", 0.8f, 0.8f, 0.9f, 0.9f);
            m_scene.addText(p, 0.0f, -0.11f, cell, "LOW AND HIGH  NEAR AND FAR", 0.8f, 0.8f, 0.9f, 0.9f);
            const int pct = static_cast<int>(m_gestures.calibrationProgress() * 100.0f);
            char line[32]; std::snprintf(line, sizeof(line), "%d", pct);
            m_scene.addText(p, 0.0f, -0.17f, cell, line, 1.0f, 0.8f, 0.3f, 1.0f);
            return;
        }
        const float o = m_menu.openness();
        if (o < 0.02f) {
            if (m_recorder.recording()) {
                char line[32]; const int s = static_cast<int>(m_recorder.seconds());
                std::snprintf(line, sizeof(line), "REC %d:%02d", s / 60, s % 60);
                m_scene.addText(p, 0.0f, 0.0f, cell, line, 1.0f, 0.3f, 0.3f, 0.9f);
            }
            return;
        }
        const int hi = m_menu.highlighted();
        for (int i = 0; i < MenuItems; ++i) {
            const bool sel = (i == hi);
            const MenuActionE a = static_cast<MenuActionE>(i);
            std::string label = menuLabel(a);
            if (a == MenuActionE::TOGGLE_MORPH) label += m_engine.getParam(ParamIdE::MORPH_ACTIVE) >= 0.5f ? "  ON" : "  OFF";
            if (a == MenuActionE::TOGGLE_MAP) label += m_engine.getParam(ParamIdE::MAP_ACTIVE) >= 0.5f ? "  ON" : "  OFF";
            if (a == MenuActionE::TOGGLE_ROUTE) label += m_engine.getParam(ParamIdE::ROUTE_ACTIVE) >= 0.5f ? "  PLAYING" : "  STOPPED";
            if (a == MenuActionE::TOGGLE_RECORD) label += m_recorder.recording() ? "  STOP" : "  START";
            m_scene.addText(p, 0.0f, -static_cast<float>(i) * 0.05f, cell, label.c_str(), sel ? 1.0f : 0.5f, sel ? 0.9f : 0.55f, sel ? 0.6f : 0.7f, o * (sel ? 1.0f : 0.6f));
        }
        std::string ab = "A:" + m_slotName[0].substr(0, 18) + "  B:" + m_slotName[1].substr(0, 18);
        m_scene.addText(p, 0.0f, 0.06f, cell * 0.8f, ab.c_str(), 0.9f, 0.6f, 0.9f, o);
    }

    /**
     * @brief One OpenXR frame: wait, begin, update, render both eyes, end.
     *
     * dt comes from the predicted display times (1/72 s for the very first frame). Hands and head
     * are located, the menu is updated and its action applied, the gesture layer writes the engine's
     * parameters, the route steps; when no route runs and the preset map is on (menu closed, not
     * calibrating, hands not resting) the left hand's reach and height set the map cursor and the
     * right hand's height its blend radius. A calibration that ended this frame is saved. Then, if
     * the runtime wants a frame and the views are valid, the scene is built once and drawn into each
     * eye's acquired swapchain image, and the projection layer is submitted (or no layer at all).
     */
    void frame()
    {
        XrFrameWaitInfo wi{ XR_TYPE_FRAME_WAIT_INFO };
        XrFrameState fs{ XR_TYPE_FRAME_STATE };
        if (XR_FAILED(xrWaitFrame(m_session, &wi, &fs))) return;
        XrFrameBeginInfo bi{ XR_TYPE_FRAME_BEGIN_INFO };
        xrBeginFrame(m_session, &bi);

        const double dt = m_lastTime == 0 ? 1.0 / 72.0 : static_cast<double>(fs.predictedDisplayTime - m_lastTime) * 1e-9;
        m_lastTime = fs.predictedDisplayTime;
        updateHands(fs.predictedDisplayTime);
        updateHead(fs.predictedDisplayTime);
        const bool wasCalibrating = m_gestures.calibrating();
        const MenuActionE action = m_menu.update(dt, m_gestures);
        if (action != MenuActionE::NONE) applyMenu(action);
        m_gestures.update(dt, [this](ParamIdE id, float v) { m_engine.setParam(id, v); });
        {
            float rx, ry, rr;
            const bool routing = m_engine.routeStep(dt, rx, ry, rr);
            if (!routing && m_engine.getParam(ParamIdE::MAP_ACTIVE) >= 0.5f && !m_menu.isOpen() && !m_gestures.calibrating() && !m_gestures.resting()) {
                // left hand: cursor (reach = x, height = y); right hand height: blend radius (sharp low, blurred high)
                m_engine.setParam(ParamIdE::MAP_X, clampv(m_gestures.input(GestureInputE::LEFT_FORWARD), 0.0f, 1.0f));
                m_engine.setParam(ParamIdE::MAP_Y, clampv(m_gestures.input(GestureInputE::LEFT_HEIGHT), 0.0f, 1.0f));
                m_engine.setParam(ParamIdE::MAP_RADIUS, 0.02f + 0.38f * clampv(m_gestures.input(GestureInputE::RIGHT_HEIGHT), 0.0f, 1.0f));
            }
        }
        if (wasCalibrating && !m_gestures.calibrating()) saveCalibration();

        std::vector<XrCompositionLayerProjectionView> projViews;
        XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        const XrCompositionLayerBaseHeader* layers[1] = { reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer) };
        uint32_t layerCount = 0;

        if (fs.shouldRender) {
            XrViewLocateInfo vli{ XR_TYPE_VIEW_LOCATE_INFO };
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = fs.predictedDisplayTime;
            vli.space = m_stageSpace;
            XrViewState vs{ XR_TYPE_VIEW_STATE };
            uint32_t viewCount = 0;
            xrLocateViews(m_session, &vli, &vs, static_cast<uint32_t>(m_views.size()), &viewCount, m_views.data());
            if ((vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
                m_scene.begin();
                m_scene.addWorld(m_engine, m_palms, m_palmValid, m_pinch);
                addOverlay();
                m_scene.upload();
                projViews.resize(viewCount, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW });
                for (uint32_t v = 0; v < viewCount; ++v) {
                    SwapchainTarget& t = m_targets[v];
                    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
                    uint32_t index = 0;
                    xrAcquireSwapchainImage(t.m_swapchain, &ai, &index);
                    XrSwapchainImageWaitInfo wi2{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                    wi2.timeout = XR_INFINITE_DURATION;
                    xrWaitSwapchainImage(t.m_swapchain, &wi2);

                    glBindFramebuffer(GL_FRAMEBUFFER, t.m_fbo);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.m_images[index].image, 0);
                    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t.m_depth);
                    const Mat4 proj = projectionFromFov(m_views[v].fov, 0.05f, 100.0f);
                    const Mat4 view = viewFromPose(m_views[v].pose);
                    m_scene.draw(multiply(proj, view), t.m_width, t.m_height);
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);

                    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                    xrReleaseSwapchainImage(t.m_swapchain, &ri);

                    projViews[v].pose = m_views[v].pose;
                    projViews[v].fov = m_views[v].fov;
                    projViews[v].subImage.swapchain = t.m_swapchain;
                    projViews[v].subImage.imageRect = { { 0, 0 }, { t.m_width, t.m_height } };
                    projViews[v].subImage.imageArrayIndex = 0;
                }
                layer.space = m_stageSpace;
                layer.viewCount = viewCount;
                layer.views = projViews.data();
                layerCount = 1;
            }
        }

        XrFrameEndInfo ei{ XR_TYPE_FRAME_END_INFO };
        ei.displayTime = fs.predictedDisplayTime;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        ei.layerCount = layerCount;
        ei.layers = layers;
        xrEndFrame(m_session, &ei);
    }

    android_app* m_app;                                                     ///< the NativeActivity glue: activity, window, looper, destroyRequested
    std::string m_dataDir;                                                  ///< the external data folder (config, calibration, samples, packs, recordings), or empty
    std::vector<std::vector<float>> m_pendingIr;                   ///< impulse A waiting for the engine to be prepared, one vector per channel
                                    int m_pendingIrRate = 48000;   ///< its sample rate in Hz
    std::vector<std::vector<float>> m_pendingIrB;                   ///< impulse B waiting likewise, one vector per channel
                                    int m_pendingIrBRate = 48000;   ///< its sample rate in Hz
    bool m_clearIrB = false,       ///< the pending preset named a room and no B, so the engine's B impulse is to be cleared
         m_audioStarted = false;   ///< the engine is prepared, impulses may go in at once
    Config m_config;                                                        ///< ambient.cfg as read at init()
    Engine m_engine;                                                        ///< the synthesizer; rendered by m_audio, parameters written by m_gestures and the menu
    GestureLayer m_gestures;                                                ///< hands and head in, engine parameters out; holds the calibration
    HandMenu m_menu;                                                        ///< the left-pinch menu, updated once per frame
    WavRecorder m_recorder;                                                 ///< records the audio callback's output to rec-*.wav on a background thread
    Audio m_audio{ m_engine, m_recorder };                                    ///< the Oboe stream; started at init() unless m_config.audio is off
    OscOut m_osc;                                                           ///< the bridge to the desktop plugin; closed unless m_config.oscHost is set
    Scene m_scene;                                                          ///< the point-cloud renderer
    int m_presetA = 0,   ///< preset index in morph slot A, moved by the menu
        m_presetB = 0;   ///< preset index in morph slot B
    std::string m_slotName[2] = { "INIT", "INIT" };                         ///< what the panel shows for A and B: a preset name, "NOW" after a capture, "INIT" at start
    bool m_calibrated = false;                                              ///< a calibration was loaded or saved; false makes init() start one

    XrInstance m_instance = XR_NULL_HANDLE;                                 ///< the OpenXR instance
    XrSystemId m_system = XR_NULL_SYSTEM_ID;                                ///< the head-mounted display system
    XrSession m_session = XR_NULL_HANDLE;                                   ///< the session on the EGL context
    XrSpace m_stageSpace = XR_NULL_HANDLE,   ///< the space the scene lives in: stage, or local as fallback
            m_viewSpace = XR_NULL_HANDLE;    ///< the head's space, located in the stage each frame
    XrSessionState m_sessionState = XR_SESSION_STATE_UNKNOWN;               ///< the last state the runtime reported
    bool m_sessionRunning = false,   ///< between xrBeginSession and xrEndSession: frames are rendered
         m_quit = false,             ///< the runtime asked to leave; run() returns
         m_handsSupported = false;   ///< the system reports hand tracking
    std::vector<XrView> m_views;                                            ///< the stereo views, located each frame
    std::vector<SwapchainTarget> m_targets;                                 ///< one render target per view
    XrTime m_lastTime = 0;                                                  ///< the previous frame's predicted display time, for dt; 0 before the first frame
    XrPosef m_headPose{};                                                   ///< the head in stage space, from updateHead()
    bool m_headValid = false;                                               ///< whether m_headPose was located this frame (the panel needs it)

    EGLDisplay m_display = EGL_NO_DISPLAY;                                  ///< the default EGL display
    EGLConfig m_config_egl = nullptr;                                       ///< the RGBA8 GLES 3 config chosen at initEgl()
    EGLSurface m_surface = EGL_NO_SURFACE;                                  ///< a 16x16 pbuffer, only so the context can be made current
    EGLContext m_context = EGL_NO_CONTEXT;                                  ///< the GLES 3 context the session and the scene draw with

    PFN_xrCreateHandTrackerEXT m_pfnCreateHandTracker = nullptr;            ///< XR_EXT_hand_tracking entry point, resolved by initHands()
    PFN_xrLocateHandJointsEXT m_pfnLocateHandJoints = nullptr;              ///< XR_EXT_hand_tracking entry point, resolved by initHands()
    PFN_xrDestroyHandTrackerEXT m_pfnDestroyHandTracker = nullptr;          ///< XR_EXT_hand_tracking entry point, resolved by initHands()
    XrHandTrackerEXT m_handTracker[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE }; ///< the trackers, left then right; null when hands are unavailable
    XrVector3f m_palms[2] = {};                                             ///< this frame's palm positions in stage space, left then right
    bool m_palmValid[2] = { false, false };                                 ///< whether each palm was tracked this frame
    float m_pinch[2] = { 0.0f, 0.0f };                                      ///< each hand's pinch 0 .. 1 this frame (for the picture; the gesture layer has its own copy)
};

/**
 * @brief The glue's app-command handler: nothing to do, the loop watches destroyRequested and the
 *        OpenXR session state itself.
 *
 * Both parameters (the android_app and the command) are unused.
 */
void handleCmd(android_app*, int32_t) {}

} // namespace

/**
 * @brief The entry point the NativeActivity glue calls on its own thread.
 *
 * Attaches the thread to the JavaVM (the OpenXR loader needs a JNIEnv on it), runs the App through
 * init(), run() and shutdown(), and detaches. Errors in init() are logged and the activity ends.
 *
 * @param app  the glue's application object for this activity
 */
void android_main(android_app* app)
{
    app->onAppCmd = handleCmd;
    JNIEnv* env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, nullptr);
    {
        // On the heap: the engine inside is a few hundred KB, the glue thread's stack is 1 MB.
        auto a = std::make_unique<App>(app);
        if (a->init()) a->run();
        else LOGE("init failed");
        a->shutdown();
    }
    app->activity->vm->DetachCurrentThread();
}
