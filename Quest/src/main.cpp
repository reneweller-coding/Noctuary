// Noctuary for Meta Quest -- native OpenXR application.
//
//   hands (XR_EXT_hand_tracking) -> GestureLayer -> Engine parameters
//   left pinch                   -> HandMenu (presets A/B, morph, record, calibrate)
//   Oboe output stream           -> Engine::process (+ WavRecorder)
//   GLES 3 scene                 <- Engine observers (sounding notes, level, distance, root, morph)
//   optional bridge              -> OSC to a PC running the desktop plugin
//
// No game engine: NativeActivity + android_native_app_glue, EGL, OpenXR loader, Oboe, the core.
// Files in <externalDataPath>:
//   ambient.cfg   osc_host=192.168.1.20  osc_port=9000  audio=1  preset=Consonant Hollow  route=Night Descent  rest_zone=0.08
//   calib.txt     hand calibration, written after the calibration gesture
//   texture.wav   optional sample for the Texture source slots (assumed recorded at C4)
//   wavetable.wav optional user wavetable, 2048-sample frames (Table = User)
//   Packs/*.ambientpack  preset packs loaded at start (see Core/include/ambient/Presets.h)
//                        a pack preset may name its own texture, wavetable and impulse
//   rec-*.wav     recordings

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

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "Noctuary", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "Noctuary", __VA_ARGS__)

using namespace ambient;

namespace {

// ---------------------------------------------------------------- small math

struct Mat4 { float m[16]; };
struct Vec3 { float x, y, z; };

Mat4 identity() { Mat4 r{}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f; return r; }

Mat4 multiply(const Mat4& a, const Mat4& b)
{
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr)
            r.m[c * 4 + rr] = a.m[0 * 4 + rr] * b.m[c * 4 + 0] + a.m[1 * 4 + rr] * b.m[c * 4 + 1] + a.m[2 * 4 + rr] * b.m[c * 4 + 2] + a.m[3 * 4 + rr] * b.m[c * 4 + 3];
    return r;
}

Mat4 projectionFromFov(const XrFovf& fov, float nearZ, float farZ)
{
    const float l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight), u = std::tan(fov.angleUp), d = std::tan(fov.angleDown);
    const float w = r - l, h = u - d;
    Mat4 p{};
    p.m[0] = 2.0f / w;  p.m[8] = (r + l) / w;
    p.m[5] = 2.0f / h;  p.m[9] = (u + d) / h;
    p.m[10] = -(farZ + nearZ) / (farZ - nearZ);
    p.m[11] = -1.0f;
    p.m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
    return p;
}

Mat4 rotationFromQuat(const XrQuaternionf& q)
{
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    Mat4 r = identity();
    r.m[0] = 1 - 2 * (y * y + z * z); r.m[4] = 2 * (x * y - z * w);     r.m[8] = 2 * (x * z + y * w);
    r.m[1] = 2 * (x * y + z * w);     r.m[5] = 1 - 2 * (x * x + z * z); r.m[9] = 2 * (y * z - x * w);
    r.m[2] = 2 * (x * z - y * w);     r.m[6] = 2 * (y * z + x * w);     r.m[10] = 1 - 2 * (x * x + y * y);
    return r;
}

Vec3 rotate(const XrQuaternionf& q, Vec3 v)
{
    const Mat4 r = rotationFromQuat(q);
    return { r.m[0] * v.x + r.m[4] * v.y + r.m[8] * v.z, r.m[1] * v.x + r.m[5] * v.y + r.m[9] * v.z, r.m[2] * v.x + r.m[6] * v.y + r.m[10] * v.z };
}

Mat4 viewFromPose(const XrPosef& pose)
{
    Mat4 r = rotationFromQuat(pose.orientation);
    Mat4 rt = identity();
    for (int c = 0; c < 3; ++c) for (int rr = 0; rr < 3; ++rr) rt.m[c * 4 + rr] = r.m[rr * 4 + c];
    Mat4 t = identity();
    t.m[12] = -pose.position.x; t.m[13] = -pose.position.y; t.m[14] = -pose.position.z;
    return multiply(rt, t);
}

void quatToEulerDeg(const XrQuaternionf& q, float& yaw, float& pitch, float& roll)
{
    const float sinp = 2.0f * (q.w * q.x - q.y * q.z);
    pitch = std::asin(std::fmax(-1.0f, std::fmin(1.0f, sinp))) * 57.29578f;
    yaw = std::atan2(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y)) * 57.29578f;
    roll = std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.x * q.x + q.z * q.z)) * 57.29578f;
}

// ---------------------------------------------------------------- 5x7 point font

// Rows top to bottom, 5 bits each (bit 4 = left column). Covers A-Z, 0-9 and a few signs.
const unsigned char* glyph(char c)
{
    static const unsigned char kFont[][7] = {
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
    if (c >= 'A' && c <= 'Z') return kFont[c - 'A'];
    if (c >= '0' && c <= '9') return kFont[26 + (c - '0')];
    switch (c) { case ' ': return kFont[36]; case '-': return kFont[37]; case '.': return kFont[38]; case ':': return kFont[39];
                 case '/': return kFont[40]; case '<': return kFont[41]; case '>': return kFont[42]; case '+': return kFont[43]; case '=': return kFont[44]; }
    return kFont[45];
}

// ---------------------------------------------------------------- OSC sender (bridge mode)

class OscOut {
public:
    bool open(const std::string& host, int port)
    {
        close();
        sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ < 0) return false;
        std::memset(&to_, 0, sizeof(to_));
        to_.sin_family = AF_INET;
        to_.sin_port = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET, host.c_str(), &to_.sin_addr) != 1) { close(); return false; }
        return true;
    }
    void close() { if (sock_ >= 0) ::close(sock_); sock_ = -1; }
    bool ok() const { return sock_ >= 0; }
    void send(const char* address, const float* args, int n)
    {
        if (sock_ < 0) return;
        char buf[256]; size_t pos = 0;
        auto putStr = [&](const char* s) { const size_t l = std::strlen(s) + 1; std::memcpy(buf + pos, s, l); pos += l; while (pos & 3) buf[pos++] = 0; };
        putStr(address);
        char tags[16]; tags[0] = ','; for (int i = 0; i < n; ++i) tags[1 + i] = 'f'; tags[1 + n] = 0;
        putStr(tags);
        for (int i = 0; i < n; ++i) { uint32_t u; std::memcpy(&u, &args[i], 4); buf[pos++] = static_cast<char>(u >> 24); buf[pos++] = static_cast<char>(u >> 16); buf[pos++] = static_cast<char>(u >> 8); buf[pos++] = static_cast<char>(u); }
        ::sendto(sock_, buf, pos, 0, reinterpret_cast<sockaddr*>(&to_), sizeof(to_));
    }
private:
    int sock_ = -1;
    sockaddr_in to_{};
};

// ---------------------------------------------------------------- audio

class Audio : public oboe::AudioStreamDataCallback {
public:
    Audio(Engine& e, WavRecorder& r) : engine_(e), recorder_(r) {}
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
        if (b.openStream(stream_) != oboe::Result::OK) { LOGE("Oboe: cannot open stream"); return false; }
        sampleRate_ = stream_->getSampleRate();
        const int burst = stream_->getFramesPerBurst();
        stream_->setBufferSizeInFrames(burst * 2);
        bufL_.assign(8192, 0.0f); bufR_.assign(8192, 0.0f);
        engine_.prepare(sampleRate_, 1024);
        LOGI("Oboe: %d Hz, burst %d", sampleRate_, burst);
        return stream_->requestStart() == oboe::Result::OK;
    }
    void stop() { if (stream_) { stream_->requestStop(); stream_->close(); stream_.reset(); } }
    int sampleRate() const { return sampleRate_; }
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream*, void* data, int32_t frames) override
    {
        float* out = static_cast<float*>(data);
        int done = 0;
        while (done < frames) {
            const int n = std::min(frames - done, 4096);
            engine_.process(bufL_.data(), bufR_.data(), n);
            recorder_.write(bufL_.data(), bufR_.data(), n);
            for (int i = 0; i < n; ++i) { out[(done + i) * 2] = bufL_[static_cast<size_t>(i)]; out[(done + i) * 2 + 1] = bufR_[static_cast<size_t>(i)]; }
            done += n;
        }
        return oboe::DataCallbackResult::Continue;
    }
private:
    Engine& engine_;
    WavRecorder& recorder_;
    std::shared_ptr<oboe::AudioStream> stream_;
    std::vector<float> bufL_, bufR_;
    int sampleRate_ = 48000;
};

// ---------------------------------------------------------------- config

struct Config {
    std::string oscHost;
    int oscPort = 9000;
    bool audio = true;
    std::string preset;
    std::string route;   // route preset name or route text for ROUTE PLAY/STOP
    float restZone = 0.08f;   // both hands below this share of the calibrated height = resting, nothing moves
};

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
        if (k == "osc_host") c.oscHost = v;
        else if (k == "osc_port") c.oscPort = std::atoi(v.c_str());
        else if (k == "audio") c.audio = v != "0";
        else if (k == "preset") c.preset = v;
        else if (k == "route") c.route = v;
        else if (k == "rest_zone") c.restZone = static_cast<float>(std::atof(v.c_str()));
    }
    std::fclose(f);
    return c;
}

// ---------------------------------------------------------------- GL scene

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

struct Point { float x, y, z; float r, g, b, a; float size; };

GLuint compile(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof(log), nullptr, log); LOGE("shader: %s", log); }
    return s;
}

// A head-locked text panel: origin in front of the eyes, axes from the head's yaw only.
struct Panel { Vec3 origin, right, up; };

class Scene {
public:
    bool init()
    {
        program_ = glCreateProgram();
        glAttachShader(program_, compile(GL_VERTEX_SHADER, kVertexShader));
        glAttachShader(program_, compile(GL_FRAGMENT_SHADER, kFragmentShader));
        glLinkProgram(program_);
        GLint ok = 0; glGetProgramiv(program_, GL_LINK_STATUS, &ok);
        if (!ok) { LOGE("program link failed"); return false; }
        uVP_ = glGetUniformLocation(program_, "uVP");
        glGenBuffers(1, &vbo_);
        glGenVertexArrays(1, &vao_);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Point), reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Point), reinterpret_cast<void*>(12));
        glEnableVertexAttribArray(2); glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(Point), reinterpret_cast<void*>(28));
        glBindVertexArray(0);
        return true;
    }

    void begin() { points_.clear(); }

    void addWorld(Engine& engine, const XrVector3f* palms, const bool* palmsValid, const float* pinch)
    {
        for (int i = 0; i < 64; ++i) {   // horizon ring
            const float a = static_cast<float>(i) / 64.0f * 6.2831853f;
            points_.push_back({ 2.5f * std::sin(a), 0.02f, -2.5f * std::cos(a), 0.25f, 0.3f, 0.4f, 0.35f, 40.0f });
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
            points_.push_back({ radius * std::sin(ang), y, -radius * std::cos(ang),
                                0.5f + 0.5f * warm, 0.55f + 0.25f * warm, 1.0f - 0.5f * warm, 0.2f + 0.8f * lv, (120.0f + 160.0f * lv) * (1.0f - 0.5f * d) });
        }
        {
            const float ang = static_cast<float>(root % 12) / 12.0f * 6.2831853f;
            points_.push_back({ 1.0f * std::sin(ang), 0.1f, -1.0f * std::cos(ang), 1.0f, 0.6f, 0.2f, 0.8f, 120.0f });
        }
        for (int h = 0; h < 2; ++h) {
            if (!palmsValid[h]) continue;
            const float p = pinch[h];
            points_.push_back({ palms[h].x, palms[h].y, palms[h].z, 0.3f + 0.7f * p, 0.9f * (1.0f - p) + 0.2f, 0.4f * (1.0f - p), 0.95f, 90.0f });
        }
        if (palmsValid[0] && palmsValid[1]) {
            const float m = engine.morphPosition();
            for (int i = 1; i < 8; ++i) {
                const float t = static_cast<float>(i) / 8.0f;
                const float lit = (t <= m) ? 0.9f : 0.25f;
                points_.push_back({ palms[0].x + (palms[1].x - palms[0].x) * t, palms[0].y + (palms[1].y - palms[0].y) * t, palms[0].z + (palms[1].z - palms[0].z) * t,
                                    0.9f, 0.5f, 0.8f, lit, 35.0f });
            }
        }
    }

    // Text on the panel; (u, v) in metres from the panel origin, cell = dot spacing in metres.
    void addText(const Panel& p, float u, float v, float cell, const char* text, float r, float g, float b, float a)
    {
        for (const char* c = text; *c; ++c) {
            const unsigned char* gl = glyph(*c);
            for (int row = 0; row < 7; ++row)
                for (int col = 0; col < 5; ++col)
                    if (gl[row] & (0x10 >> col)) {
                        const float x = u + static_cast<float>(col) * cell, y = v - static_cast<float>(row) * cell;
                        points_.push_back({ p.origin.x + p.right.x * x + p.up.x * y, p.origin.y + p.right.y * x + p.up.y * y, p.origin.z + p.right.z * x + p.up.z * y,
                                            r, g, b, a, cell * 900.0f });
                    }
            u += 6.0f * cell;
        }
    }

    void upload()
    {
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(points_.size() * sizeof(Point)), points_.data(), GL_DYNAMIC_DRAW);
    }

    void draw(const Mat4& vp, int width, int height)
    {
        glViewport(0, 0, width, height);
        glClearColor(0.02f, 0.02f, 0.04f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glUseProgram(program_);
        glUniformMatrix4fv(uVP_, 1, GL_FALSE, vp.m);
        glBindVertexArray(vao_);
        glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(points_.size()));
        glBindVertexArray(0);
    }
private:
    GLuint program_ = 0, vbo_ = 0, vao_ = 0;
    GLint uVP_ = -1;
    std::vector<Point> points_;
};

// ---------------------------------------------------------------- the app

struct SwapchainTarget {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int width = 0, height = 0;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    GLuint fbo = 0, depth = 0;
};

class App {
public:
    explicit App(android_app* app) : app_(app) {}

    bool init()
    {
        dataDir_ = app_->activity->externalDataPath ? app_->activity->externalDataPath : "";
        // Preset packs from <externalDataPath>/Packs, before anything looks a preset up by name.
        if (!dataDir_.empty()) {
            const int packs = loadPresetPacksIn((dataDir_ + "/Packs").c_str());
            if (packs > 0) LOGI("Packs: %d pack(s), %d presets in total", packs, numPresets());
        }
        config_ = readConfig(dataDir_.c_str());
        if (!config_.preset.empty())
            for (int i = 0; i < numPresets(); ++i) if (config_.preset == preset(i).name) { engine_.applyPreset(i); presetA_ = presetB_ = i; }
        loadCalibration();
        loadSourceFiles();
        // After loadSourceFiles, so a pack preset's own sample wins over the folder's texture.wav.
        if (!config_.preset.empty()) loadPresetFiles(presetA_);
        gestures_.setRestZone(config_.restZone);
        engine_.setRoomMaxSeconds(4.0f);   // a 4 s convolution is about a third of one XR2 core; 8 s would be two thirds
        if (!config_.oscHost.empty()) {
            if (osc_.open(config_.oscHost, config_.oscPort)) LOGI("bridge: OSC to %s:%d", config_.oscHost.c_str(), config_.oscPort);
            else LOGE("bridge: bad host %s", config_.oscHost.c_str());
        }
        if (!initLoader()) return false;
        if (!initInstance()) return false;
        if (!initEgl()) return false;
        if (!initSession()) return false;
        if (!initHands()) LOGE("hand tracking unavailable");
        if (!scene_.init()) return false;
        if (config_.audio && !audio_.start()) LOGE("audio failed to start");
        audioStarted_ = true;        // the engine is prepared now; the convolver has its buffers
        applyPendingImpulses();
        if (!calibrated_) { gestures_.startCalibration(8.0f); LOGI("no calibration file: calibrating for 8 s"); }
        return true;
    }

    // A pack preset may bring its own sample, wavetable and impulse; the paths sit next to the pack.
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
            for (int slot = 0; slot < ambient::kSlots; ++slot) {
                const size_t semi = all.find(';', start);
                std::string one = all.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
                while (!one.empty() && one.front() == ' ') one.erase(one.begin());
                while (!one.empty() && one.back() == ' ') one.pop_back();
                std::vector<float> right;
                if (!one.empty() && readWavStereo(one.c_str(), mono, right, rate)) {
                    const double base = baseHzFromName(one.c_str());
                    const bool seamless = loopFromName(one.c_str());
                    const float* rp = right.empty() ? nullptr : right.data();
                    if (perSlot) engine_.setTexture(slot, mono.data(), rp, static_cast<int>(mono.size()), rate, base > 0.0 ? base : 261.6256, seamless);
                    else         engine_.setTexture(mono.data(), rp, static_cast<int>(mono.size()), rate, base > 0.0 ? base : 261.6256, seamless);
                    LOGI("preset texture %d: %s", slot + 1, one.c_str());
                }
                if (semi == std::string::npos) break;
                start = semi + 1;
            }
        }
        const char* tab = presetFilePath(index, 1);
        int cycle = 0;
        if (tab != nullptr && *tab && readWavetableFile(tab, mono, cycle))
            if (!engine_.loadUserWavetable(mono.data(), static_cast<int>(mono.size()), cycle))
                LOGE("%s: not a usable wavetable", tab);
        const char* imp = presetFilePath(index, 2);
        const char* impB = presetFilePath(index, 3);
        std::vector<std::vector<float>> ir;
        if (imp != nullptr && *imp && readWavChannels(imp, ir, rate) && !ir.empty()) {
            pendingIr_ = ir; pendingIrRate_ = rate;   // set once the engine is prepared
            LOGI("preset impulse %s", imp);
            pendingIrB_.clear();                      // a preset that names its room and no B plays A alone
            clearIrB_ = true;
        }
        if (impB != nullptr && *impB && readWavChannels(impB, ir, rate) && !ir.empty()) {
            pendingIrB_ = ir; pendingIrBRate_ = rate; clearIrB_ = false;
            LOGI("preset impulse B %s", impB);
        }
        // A preset picked from the menu arrives after the audio started: its rooms go in now. They
        // used to wait for a start that had already happened, and the Room kept the first preset's.
        if (audioStarted_) applyPendingImpulses();
    }

    // Message thread, engine prepared: the impulses a preset or the data folder asked for.
    void applyPendingImpulses()
    {
        if (!pendingIr_.empty()) {
            engine_.setImpulse(pendingIr_[0].data(), pendingIr_.size() > 1 ? pendingIr_[1].data() : nullptr, static_cast<int>(pendingIr_[0].size()), pendingIrRate_);
            pendingIr_.clear();
        }
        if (!pendingIrB_.empty()) {
            engine_.setImpulseB(pendingIrB_[0].data(), pendingIrB_.size() > 1 ? pendingIrB_[1].data() : nullptr, static_cast<int>(pendingIrB_[0].size()), pendingIrBRate_);
            pendingIrB_.clear();
        } else if (clearIrB_) {
            engine_.clearImpulseB();
        }
        clearIrB_ = false;
    }

    // texture.wav (a field recording etc., assumed at C4 for Pitch = Note) and wavetable.wav
    // (2048-sample frames) in the data folder feed the Texture and User-table source slots.
    void loadSourceFiles()
    {
        if (dataDir_.empty()) return;
        std::vector<float> mono; int rate = 0;
        // texture.wav, or any texture*.wav (a "_A3" suffix from TextureGen sets the base pitch).
        std::string texPath;
        if (DIR* d = opendir(dataDir_.c_str())) {
            while (dirent* e = readdir(d)) {
                const std::string n = e->d_name;
                if (n.rfind("texture", 0) == 0 && n.size() > 4 && n.compare(n.size() - 4, 4, ".wav") == 0) { texPath = dataDir_ + "/" + n; if (n == "texture.wav") break; }
            }
            closedir(d);
        }
        std::vector<float> texRight;
        if (!texPath.empty() && readWavStereo(texPath.c_str(), mono, texRight, rate)) {
            const double base = baseHzFromName(texPath.c_str());
            engine_.setTexture(mono.data(), texRight.empty() ? nullptr : texRight.data(),
                               static_cast<int>(mono.size()), rate, base > 0.0 ? base : 261.6256,
                               loopFromName(texPath.c_str()));
            LOGI("%s: %.1f s @ %d Hz, base %.1f Hz", texPath.c_str(), mono.size() / static_cast<double>(rate), rate, base > 0.0 ? base : 261.6256);
        }
        std::vector<std::vector<float>> ir;
        if (readWavChannels((dataDir_ + "/impulse.wav").c_str(), ir, rate) && !ir.empty()) {
            pendingIr_ = ir; pendingIrRate_ = rate;   // set after the engine is prepared (audio start)
            LOGI("impulse.wav: %zu ch, %.2f s @ %d Hz", ir.size(), ir[0].size() / static_cast<double>(rate), rate);
        }
        int cycle = 0;
        if (readWavetableFile((dataDir_ + "/wavetable.wav").c_str(), mono, cycle)) {
            if (engine_.loadUserWavetable(mono.data(), static_cast<int>(mono.size()), cycle)) LOGI("wavetable.wav: cycles of %d samples", cycle);
            else LOGE("wavetable.wav: not a usable wavetable");
        }
    }

    void run()
    {
        while (!app_->destroyRequested) {
            int events; android_poll_source* source;
            const int timeout = (sessionRunning_ || app_->window == nullptr) ? 0 : -1;
            while (ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
                if (source) source->process(app_, source);
                if (app_->destroyRequested) break;
            }
            pollXrEvents();
            if (quit_) break;
            if (sessionRunning_) frame();
        }
    }

    void shutdown()
    {
        recorder_.stop();
        audio_.stop();
        for (auto& t : targets_) { if (t.swapchain != XR_NULL_HANDLE) xrDestroySwapchain(t.swapchain); }
        for (int h = 0; h < 2; ++h) if (handTracker_[h] != XR_NULL_HANDLE && pfnDestroyHandTracker_) pfnDestroyHandTracker_(handTracker_[h]);
        if (viewSpace_ != XR_NULL_HANDLE) xrDestroySpace(viewSpace_);
        if (stageSpace_ != XR_NULL_HANDLE) xrDestroySpace(stageSpace_);
        if (session_ != XR_NULL_HANDLE) xrDestroySession(session_);
        if (instance_ != XR_NULL_HANDLE) xrDestroyInstance(instance_);
        osc_.close();
    }

private:
    // ------------------------------------------------ persistence

    void loadCalibration()
    {
        if (dataDir_.empty()) return;
        FILE* f = std::fopen((dataDir_ + "/calib.txt").c_str(), "r");
        if (f == nullptr) return;
        char line[256] = {};
        if (std::fgets(line, sizeof(line), f)) calibrated_ = gestures_.parseCalibration(line);
        std::fclose(f);
        LOGI("calibration %s", calibrated_ ? "loaded" : "invalid");
    }

    void saveCalibration()
    {
        if (dataDir_.empty()) return;
        char text[128];
        gestures_.writeCalibration(text, sizeof(text));
        FILE* f = std::fopen((dataDir_ + "/calib.txt").c_str(), "w");
        if (f) { std::fputs(text, f); std::fclose(f); }
        calibrated_ = true;
        LOGI("calibration saved: %s", text);
    }

    // ------------------------------------------------ menu actions

    void applyMenu(MenuAction a)
    {
        switch (a) {
        case MenuAction::ToggleMorph:
            engine_.setParam(ParamId::MorphActive, engine_.getParam(ParamId::MorphActive) >= 0.5f ? 0.0f : 1.0f);
            break;
        case MenuAction::CaptureA: engine_.captureMorphSlot(0); slotName_[0] = "NOW"; break;
        case MenuAction::CaptureB: engine_.captureMorphSlot(1); slotName_[1] = "NOW"; break;
        case MenuAction::PresetAPrev: case MenuAction::PresetANext: case MenuAction::PresetBPrev: case MenuAction::PresetBNext: {
            const int slot = (a == MenuAction::PresetAPrev || a == MenuAction::PresetANext) ? 0 : 1;
            int& idx = slot == 0 ? presetA_ : presetB_;
            idx = (idx + ((a == MenuAction::PresetANext || a == MenuAction::PresetBNext) ? 1 : numPresets() - 1)) % numPresets();
            float values[kNumParams];
            for (int i = 0; i < kNumParams; ++i) values[i] = engine_.getParam(static_cast<ParamId>(i));
            applyPreset(preset(idx), [&](ParamId id, float v) { values[static_cast<int>(id)] = v; });
            engine_.setMorphSlot(slot, values);
            slotName_[slot] = preset(idx).name;
            if (engine_.getParam(ParamId::MorphActive) < 0.5f && slot == 0) { engine_.applyPreset(idx); loadPresetFiles(idx); }   // without morph, A is what plays
            break;
        }
        case MenuAction::ToggleMap: {
            // Map on: the left hand's reach (x) and height (y) steer the cursor over the preset map,
            // the engine glides toward the blend of the presets around it. Map off: keep what the
            // map left behind by copying the gliding values into the live parameters.
            const bool on = engine_.getParam(ParamId::MapActive) >= 0.5f;
            if (on) {
                for (const ParamDesc& d : paramTable())
                    if (!isMapParam(d.id) && !isMorphParam(d.id) && !isMacroParam(d.id)) engine_.setParam(d.id, engine_.blendValue(d.id));
            }
            engine_.setParam(ParamId::MapActive, on ? 0.0f : 1.0f);
            LOGI("preset map %s", on ? "off" : "on");
            break;
        }
        case MenuAction::ToggleRoute: {
            // Plays the route from ambient.cfg (route=<preset name or text>), or the first route preset.
            if (engine_.routeEdit().count() == 0) {
                const char* text = routePreset(0).points;
                for (int r = 0; r < numRoutePresets(); ++r) if (config_.route == routePreset(r).name) text = routePreset(r).points;
                if (!config_.route.empty() && !engine_.setRouteText(config_.route.c_str())) engine_.setRouteText(text);
                else if (config_.route.empty()) engine_.setRouteText(text);
            }
            const bool on = engine_.getParam(ParamId::RouteActive) >= 0.5f;
            engine_.setParam(ParamId::RouteActive, on ? 0.0f : 1.0f);
            LOGI("route %s (%d points)", on ? "stopped" : "playing", engine_.routeEdit().count());
            break;
        }
        case MenuAction::ToggleRecord:
            if (recorder_.recording()) { recorder_.stop(); LOGI("recording stopped, %.1f s", recorder_.seconds()); }
            else if (!dataDir_.empty()) {
                char name[64]; const std::time_t t = std::time(nullptr); std::strftime(name, sizeof(name), "rec-%Y%m%d-%H%M%S.wav", std::localtime(&t));
                if (recorder_.start((dataDir_ + "/" + name).c_str(), audio_.sampleRate(), 2)) LOGI("recording to %s", name);
                else LOGE("cannot start recording");
            }
            break;
        case MenuAction::Calibrate:
            gestures_.startCalibration(8.0f);
            break;
        default: break;
        }
    }

    // ------------------------------------------------ OpenXR setup

    bool initLoader()
    {
        PFN_xrInitializeLoaderKHR initLoader = nullptr;
        xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", reinterpret_cast<PFN_xrVoidFunction*>(&initLoader));
        if (initLoader == nullptr) { LOGE("no xrInitializeLoaderKHR"); return false; }
        XrLoaderInitInfoAndroidKHR li{ XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
        li.applicationVM = app_->activity->vm;
        li.applicationContext = app_->activity->clazz;
        return XR_SUCCEEDED(initLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&li)));
    }

    bool initInstance()
    {
        const char* exts[] = { XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME, XR_EXT_HAND_TRACKING_EXTENSION_NAME };
        XrInstanceCreateInfoAndroidKHR android{ XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
        android.applicationVM = app_->activity->vm;
        android.applicationActivity = app_->activity->clazz;
        XrInstanceCreateInfo ci{ XR_TYPE_INSTANCE_CREATE_INFO };
        ci.next = &android;
        std::strncpy(ci.applicationInfo.applicationName, "Noctuary", XR_MAX_APPLICATION_NAME_SIZE - 1);
        ci.applicationInfo.applicationVersion = 1;
        std::strncpy(ci.applicationInfo.engineName, "NoctuaryCore", XR_MAX_ENGINE_NAME_SIZE - 1);
        ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        ci.enabledExtensionCount = 3;
        ci.enabledExtensionNames = exts;
        if (XR_FAILED(xrCreateInstance(&ci, &instance_))) { LOGE("xrCreateInstance failed"); return false; }

        XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO };
        sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        if (XR_FAILED(xrGetSystem(instance_, &sgi, &system_))) { LOGE("xrGetSystem failed"); return false; }
        XrSystemHandTrackingPropertiesEXT ht{ XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT };
        XrSystemProperties sp{ XR_TYPE_SYSTEM_PROPERTIES, &ht };
        xrGetSystemProperties(instance_, system_, &sp);
        handsSupported_ = ht.supportsHandTracking == XR_TRUE;
        LOGI("system: %s, hand tracking %d", sp.systemName, handsSupported_ ? 1 : 0);
        return true;
    }

    bool initEgl()
    {
        PFN_xrGetOpenGLESGraphicsRequirementsKHR getReq = nullptr;
        xrGetInstanceProcAddr(instance_, "xrGetOpenGLESGraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction*>(&getReq));
        XrGraphicsRequirementsOpenGLESKHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
        if (getReq == nullptr || XR_FAILED(getReq(instance_, system_, &req))) { LOGE("GLES requirements failed"); return false; }

        display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        EGLint major, minor;
        if (!eglInitialize(display_, &major, &minor)) { LOGE("eglInitialize failed"); return false; }
        const EGLint attribs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                   EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 0, EGL_NONE };
        EGLint count = 0;
        if (!eglChooseConfig(display_, attribs, &config_egl_, 1, &count) || count == 0) { LOGE("eglChooseConfig failed"); return false; }
        const EGLint pbuf[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
        surface_ = eglCreatePbufferSurface(display_, config_egl_, pbuf);
        const EGLint ctx[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
        context_ = eglCreateContext(display_, config_egl_, EGL_NO_CONTEXT, ctx);
        if (context_ == EGL_NO_CONTEXT || !eglMakeCurrent(display_, surface_, surface_, context_)) { LOGE("EGL context failed"); return false; }
        LOGI("EGL %d.%d, GL %s", major, minor, glGetString(GL_VERSION));
        return true;
    }

    bool initSession()
    {
        XrGraphicsBindingOpenGLESAndroidKHR gb{ XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
        gb.display = display_; gb.config = config_egl_; gb.context = context_;
        XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO, &gb };
        sci.systemId = system_;
        if (XR_FAILED(xrCreateSession(instance_, &sci, &session_))) { LOGE("xrCreateSession failed"); return false; }

        XrReferenceSpaceCreateInfo rs{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
        rs.poseInReferenceSpace.orientation.w = 1.0f;
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        if (XR_FAILED(xrCreateReferenceSpace(session_, &rs, &stageSpace_))) {
            rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            if (XR_FAILED(xrCreateReferenceSpace(session_, &rs, &stageSpace_))) { LOGE("no reference space"); return false; }
        }
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        xrCreateReferenceSpace(session_, &rs, &viewSpace_);

        uint32_t viewCount = 0;
        xrEnumerateViewConfigurationViews(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
        std::vector<XrViewConfigurationView> cfg(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
        xrEnumerateViewConfigurationViews(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, cfg.data());
        views_.assign(viewCount, { XR_TYPE_VIEW });

        uint32_t fmtCount = 0;
        xrEnumerateSwapchainFormats(session_, 0, &fmtCount, nullptr);
        std::vector<int64_t> formats(fmtCount);
        xrEnumerateSwapchainFormats(session_, fmtCount, &fmtCount, formats.data());
        int64_t format = formats.empty() ? GL_RGBA8 : formats[0];
        for (int64_t f : formats) if (f == GL_SRGB8_ALPHA8) { format = f; break; }

        targets_.resize(viewCount);
        for (uint32_t v = 0; v < viewCount; ++v) {
            SwapchainTarget& t = targets_[v];
            XrSwapchainCreateInfo sc{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
            sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            sc.format = format;
            sc.sampleCount = 1;
            sc.width = cfg[v].recommendedImageRectWidth;
            sc.height = cfg[v].recommendedImageRectHeight;
            sc.faceCount = 1; sc.arraySize = 1; sc.mipCount = 1;
            if (XR_FAILED(xrCreateSwapchain(session_, &sc, &t.swapchain))) { LOGE("xrCreateSwapchain failed"); return false; }
            t.width = static_cast<int>(sc.width); t.height = static_cast<int>(sc.height);
            uint32_t imgCount = 0;
            xrEnumerateSwapchainImages(t.swapchain, 0, &imgCount, nullptr);
            t.images.assign(imgCount, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR });
            xrEnumerateSwapchainImages(t.swapchain, imgCount, &imgCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(t.images.data()));
            glGenFramebuffers(1, &t.fbo);
            glGenRenderbuffers(1, &t.depth);
            glBindRenderbuffer(GL_RENDERBUFFER, t.depth);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, t.width, t.height);
            LOGI("view %u: %dx%d, %u images", v, t.width, t.height, imgCount);
        }
        return true;
    }

    bool initHands()
    {
        if (!handsSupported_) return false;
        xrGetInstanceProcAddr(instance_, "xrCreateHandTrackerEXT", reinterpret_cast<PFN_xrVoidFunction*>(&pfnCreateHandTracker_));
        xrGetInstanceProcAddr(instance_, "xrLocateHandJointsEXT", reinterpret_cast<PFN_xrVoidFunction*>(&pfnLocateHandJoints_));
        xrGetInstanceProcAddr(instance_, "xrDestroyHandTrackerEXT", reinterpret_cast<PFN_xrVoidFunction*>(&pfnDestroyHandTracker_));
        if (!pfnCreateHandTracker_ || !pfnLocateHandJoints_) return false;
        for (int h = 0; h < 2; ++h) {
            XrHandTrackerCreateInfoEXT hci{ XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT };
            hci.hand = (h == 0) ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
            hci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
            if (XR_FAILED(pfnCreateHandTracker_(session_, &hci, &handTracker_[h]))) return false;
        }
        return true;
    }

    void pollXrEvents()
    {
        XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
        while (xrPollEvent(instance_, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto* sc = reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev);
                sessionState_ = sc->state;
                switch (sessionState_) {
                case XR_SESSION_STATE_READY: {
                    XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (XR_SUCCEEDED(xrBeginSession(session_, &bi))) sessionRunning_ = true;
                    break;
                }
                case XR_SESSION_STATE_STOPPING:
                    xrEndSession(session_);
                    sessionRunning_ = false;
                    break;
                case XR_SESSION_STATE_EXITING:
                case XR_SESSION_STATE_LOSS_PENDING:
                    quit_ = true;
                    break;
                default: break;
                }
            } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                quit_ = true;
            }
            ev = { XR_TYPE_EVENT_DATA_BUFFER };
        }
    }

    // ------------------------------------------------ per frame

    void updateHands(XrTime time)
    {
        for (int h = 0; h < 2; ++h) {
            palmValid_[h] = false;
            if (handTracker_[h] == XR_NULL_HANDLE) continue;
            XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT];
            XrHandJointLocationsEXT locs{ XR_TYPE_HAND_JOINT_LOCATIONS_EXT };
            locs.jointCount = XR_HAND_JOINT_COUNT_EXT;
            locs.jointLocations = joints;
            XrHandJointsLocateInfoEXT li{ XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT };
            li.baseSpace = stageSpace_;
            li.time = time;
            if (XR_FAILED(pfnLocateHandJoints_(handTracker_[h], &li, &locs)) || !locs.isActive) continue;
            const XrHandJointLocationEXT& palm = joints[XR_HAND_JOINT_PALM_EXT];
            const XrHandJointLocationEXT& thumb = joints[XR_HAND_JOINT_THUMB_TIP_EXT];
            const XrHandJointLocationEXT& index = joints[XR_HAND_JOINT_INDEX_TIP_EXT];
            const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            if ((palm.locationFlags & need) != need) continue;
            const float dx = thumb.pose.position.x - index.pose.position.x, dy = thumb.pose.position.y - index.pose.position.y, dz = thumb.pose.position.z - index.pose.position.z;
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float pinch = std::fmax(0.0f, std::fmin(1.0f, 1.0f - (dist - 0.015f) / 0.035f));
            const Mat4 r = rotationFromQuat(palm.pose.orientation);
            const float tilt = std::fmax(-1.0f, std::fmin(1.0f, r.m[1]));   // world y of the palm's sideways axis
            palms_[h] = palm.pose.position;
            palmValid_[h] = true;
            pinch_[h] = pinch;
            gestures_.setHand(h, palm.pose.position.x, palm.pose.position.y, palm.pose.position.z, pinch, tilt);
            if (osc_.ok()) {
                const float args[5] = { palm.pose.position.x, palm.pose.position.y, palm.pose.position.z, pinch, tilt };
                osc_.send(h == 0 ? "/ambient/hand/L" : "/ambient/hand/R", args, 5);
            }
        }
    }

    void updateHead(XrTime time)
    {
        headValid_ = false;
        if (viewSpace_ == XR_NULL_HANDLE) return;
        XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
        if (XR_FAILED(xrLocateSpace(viewSpace_, stageSpace_, time, &loc))) return;
        if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return;
        headPose_ = loc.pose;
        headValid_ = true;
        float yaw, pitch, roll;
        quatToEulerDeg(loc.pose.orientation, yaw, pitch, roll);
        gestures_.setHead(yaw, pitch, roll);
        engine_.setHeadYaw(yaw);   // the binaural mode turns the field against the head
        if (osc_.ok()) { const float args[3] = { yaw, pitch, roll }; osc_.send("/ambient/head", args, 3); }
    }

    Panel headPanel() const
    {
        // In front of the eyes, following yaw only (a panel that tilts with the head is tiring).
        Panel p;
        const Vec3 fwd = rotate(headPose_.orientation, { 0.0f, 0.0f, -1.0f });
        const float len = std::fmax(std::sqrt(fwd.x * fwd.x + fwd.z * fwd.z), 1e-3f);
        const Vec3 f{ fwd.x / len, 0.0f, fwd.z / len };
        p.right = { -f.z, 0.0f, f.x };
        p.up = { 0.0f, 1.0f, 0.0f };
        p.origin = { headPose_.position.x + f.x * 0.9f - p.right.x * 0.22f, headPose_.position.y + 0.12f, headPose_.position.z + f.z * 0.9f - p.right.z * 0.22f };
        return p;
    }

    void addOverlay()
    {
        if (!headValid_) return;
        const Panel p = headPanel();
        const float cell = 0.006f;
        if (gestures_.calibrating()) {
            scene_.addText(p, 0.0f, 0.0f, cell, "CALIBRATING", 1.0f, 0.8f, 0.3f, 1.0f);
            scene_.addText(p, 0.0f, -0.06f, cell, "HANDS TOGETHER AND APART", 0.8f, 0.8f, 0.9f, 0.9f);
            scene_.addText(p, 0.0f, -0.11f, cell, "LOW AND HIGH  NEAR AND FAR", 0.8f, 0.8f, 0.9f, 0.9f);
            const int pct = static_cast<int>(gestures_.calibrationProgress() * 100.0f);
            char line[32]; std::snprintf(line, sizeof(line), "%d", pct);
            scene_.addText(p, 0.0f, -0.17f, cell, line, 1.0f, 0.8f, 0.3f, 1.0f);
            return;
        }
        const float o = menu_.openness();
        if (o < 0.02f) {
            if (recorder_.recording()) {
                char line[32]; const int s = static_cast<int>(recorder_.seconds());
                std::snprintf(line, sizeof(line), "REC %d:%02d", s / 60, s % 60);
                scene_.addText(p, 0.0f, 0.0f, cell, line, 1.0f, 0.3f, 0.3f, 0.9f);
            }
            return;
        }
        const int hi = menu_.highlighted();
        for (int i = 0; i < kMenuItems; ++i) {
            const bool sel = (i == hi);
            const MenuAction a = static_cast<MenuAction>(i);
            std::string label = menuLabel(a);
            if (a == MenuAction::ToggleMorph) label += engine_.getParam(ParamId::MorphActive) >= 0.5f ? "  ON" : "  OFF";
            if (a == MenuAction::ToggleMap) label += engine_.getParam(ParamId::MapActive) >= 0.5f ? "  ON" : "  OFF";
            if (a == MenuAction::ToggleRoute) label += engine_.getParam(ParamId::RouteActive) >= 0.5f ? "  PLAYING" : "  STOPPED";
            if (a == MenuAction::ToggleRecord) label += recorder_.recording() ? "  STOP" : "  START";
            scene_.addText(p, 0.0f, -static_cast<float>(i) * 0.05f, cell, label.c_str(), sel ? 1.0f : 0.5f, sel ? 0.9f : 0.55f, sel ? 0.6f : 0.7f, o * (sel ? 1.0f : 0.6f));
        }
        std::string ab = "A:" + slotName_[0].substr(0, 18) + "  B:" + slotName_[1].substr(0, 18);
        scene_.addText(p, 0.0f, 0.06f, cell * 0.8f, ab.c_str(), 0.9f, 0.6f, 0.9f, o);
    }

    void frame()
    {
        XrFrameWaitInfo wi{ XR_TYPE_FRAME_WAIT_INFO };
        XrFrameState fs{ XR_TYPE_FRAME_STATE };
        if (XR_FAILED(xrWaitFrame(session_, &wi, &fs))) return;
        XrFrameBeginInfo bi{ XR_TYPE_FRAME_BEGIN_INFO };
        xrBeginFrame(session_, &bi);

        const double dt = lastTime_ == 0 ? 1.0 / 72.0 : static_cast<double>(fs.predictedDisplayTime - lastTime_) * 1e-9;
        lastTime_ = fs.predictedDisplayTime;
        updateHands(fs.predictedDisplayTime);
        updateHead(fs.predictedDisplayTime);
        const bool wasCalibrating = gestures_.calibrating();
        const MenuAction action = menu_.update(dt, gestures_);
        if (action != MenuAction::None) applyMenu(action);
        gestures_.update(dt, [this](ParamId id, float v) { engine_.setParam(id, v); });
        {
            float rx, ry, rr;
            const bool routing = engine_.routeStep(dt, rx, ry, rr);
            if (!routing && engine_.getParam(ParamId::MapActive) >= 0.5f && !menu_.isOpen() && !gestures_.calibrating() && !gestures_.resting()) {
                // left hand: cursor (reach = x, height = y); right hand height: blend radius (sharp low, blurred high)
                engine_.setParam(ParamId::MapX, clampv(gestures_.input(GestureInput::LeftForward), 0.0f, 1.0f));
                engine_.setParam(ParamId::MapY, clampv(gestures_.input(GestureInput::LeftHeight), 0.0f, 1.0f));
                engine_.setParam(ParamId::MapRadius, 0.02f + 0.38f * clampv(gestures_.input(GestureInput::RightHeight), 0.0f, 1.0f));
            }
        }
        if (wasCalibrating && !gestures_.calibrating()) saveCalibration();

        std::vector<XrCompositionLayerProjectionView> projViews;
        XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        const XrCompositionLayerBaseHeader* layers[1] = { reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer) };
        uint32_t layerCount = 0;

        if (fs.shouldRender) {
            XrViewLocateInfo vli{ XR_TYPE_VIEW_LOCATE_INFO };
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = fs.predictedDisplayTime;
            vli.space = stageSpace_;
            XrViewState vs{ XR_TYPE_VIEW_STATE };
            uint32_t viewCount = 0;
            xrLocateViews(session_, &vli, &vs, static_cast<uint32_t>(views_.size()), &viewCount, views_.data());
            if ((vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
                scene_.begin();
                scene_.addWorld(engine_, palms_, palmValid_, pinch_);
                addOverlay();
                scene_.upload();
                projViews.resize(viewCount, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW });
                for (uint32_t v = 0; v < viewCount; ++v) {
                    SwapchainTarget& t = targets_[v];
                    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
                    uint32_t index = 0;
                    xrAcquireSwapchainImage(t.swapchain, &ai, &index);
                    XrSwapchainImageWaitInfo wi2{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                    wi2.timeout = XR_INFINITE_DURATION;
                    xrWaitSwapchainImage(t.swapchain, &wi2);

                    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.images[index].image, 0);
                    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t.depth);
                    const Mat4 proj = projectionFromFov(views_[v].fov, 0.05f, 100.0f);
                    const Mat4 view = viewFromPose(views_[v].pose);
                    scene_.draw(multiply(proj, view), t.width, t.height);
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);

                    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                    xrReleaseSwapchainImage(t.swapchain, &ri);

                    projViews[v].pose = views_[v].pose;
                    projViews[v].fov = views_[v].fov;
                    projViews[v].subImage.swapchain = t.swapchain;
                    projViews[v].subImage.imageRect = { { 0, 0 }, { t.width, t.height } };
                    projViews[v].subImage.imageArrayIndex = 0;
                }
                layer.space = stageSpace_;
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
        xrEndFrame(session_, &ei);
    }

    android_app* app_;
    std::string dataDir_;
    std::vector<std::vector<float>> pendingIr_; int pendingIrRate_ = 48000;
    std::vector<std::vector<float>> pendingIrB_; int pendingIrBRate_ = 48000;
    bool clearIrB_ = false, audioStarted_ = false;
    Config config_;
    Engine engine_;
    GestureLayer gestures_;
    HandMenu menu_;
    WavRecorder recorder_;
    Audio audio_{ engine_, recorder_ };
    OscOut osc_;
    Scene scene_;
    int presetA_ = 0, presetB_ = 0;
    std::string slotName_[2] = { "INIT", "INIT" };
    bool calibrated_ = false;

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId system_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace stageSpace_ = XR_NULL_HANDLE, viewSpace_ = XR_NULL_HANDLE;
    XrSessionState sessionState_ = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning_ = false, quit_ = false, handsSupported_ = false;
    std::vector<XrView> views_;
    std::vector<SwapchainTarget> targets_;
    XrTime lastTime_ = 0;
    XrPosef headPose_{};
    bool headValid_ = false;

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig config_egl_ = nullptr;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;

    PFN_xrCreateHandTrackerEXT pfnCreateHandTracker_ = nullptr;
    PFN_xrLocateHandJointsEXT pfnLocateHandJoints_ = nullptr;
    PFN_xrDestroyHandTrackerEXT pfnDestroyHandTracker_ = nullptr;
    XrHandTrackerEXT handTracker_[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
    XrVector3f palms_[2] = {};
    bool palmValid_[2] = { false, false };
    float pinch_[2] = { 0.0f, 0.0f };
};

void handleCmd(android_app*, int32_t) {}

} // namespace

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
