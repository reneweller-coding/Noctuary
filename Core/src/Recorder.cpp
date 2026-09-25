/**
 * @file Recorder.cpp
 * @brief The framework-free WAV recorder: a lock-free ring fed by the audio thread, drained to disk
 *        by a thread of its own.
 *
 * Two threads meet here and never wait for each other. The audio thread calls write() once per
 * block and only ever touches the ring and its head index; if the ring is full the block is
 * counted as dropped and the thread moves on, because a stall there is a click in the room. The
 * writer thread started by start() sleeps in 20 ms steps, copies whatever has arrived into an
 * 8192-float chunk and hands it to fwrite, and runs until stop() asks it to and the ring is empty.
 * The ring holds half a million floats, about 5.4 s of stereo at 48 kHz, which is far more than any
 * disk hiccup the recorder has met.
 *
 * The file is 32-bit float WAV (format tag 3). The RIFF header is written with a zero data length
 * when the file is opened and patched in stop(), once the frame count is known; a recording that
 * outgrows the 32-bit length fields is capped at 0xFFFFFFFF there, which is what every other WAV
 * writer does and what readWavChannels (WavFile.cpp) knows to expect. The Quest app and any host
 * without a writer of its own record through this; the desktop plugin records through JUCE.
 */
#include "ambient/Recorder.h"
#include <chrono>
#include <cstring>

namespace ambient {

bool WavRecorder::start(const char* path, int sampleRate, int channels)
{
    stop();
    file_ = std::fopen(path, "wb");
    if (file_ == nullptr) return false;
    sampleRate_ = sampleRate;
    channels_ = channels;
    const uint32_t size = 1u << 19;   // 512k floats = ~5.4 s of stereo at 48 kHz
    ring_.assign(size, 0.0f);
    mask_ = size - 1;
    head_.store(0); tail_.store(0);
    framesWritten_.store(0); framesDropped_.store(0);
    writeHeader(0);
    stopRequested_.store(false);
    recording_.store(true);
    thread_ = std::thread([this] { run(); });
    return true;
}

void WavRecorder::stop()
{
    if (!recording_.load() && file_ == nullptr) return;
    recording_.store(false);
    stopRequested_.store(true);
    if (thread_.joinable()) thread_.join();
    if (file_) {
        const uint64_t bytes = framesWritten_.load() * static_cast<uint64_t>(channels_) * 4u;
        writeHeader(static_cast<uint32_t>(bytes > 0xFFFFFFFFull ? 0xFFFFFFFFull : bytes));
        std::fclose(file_);
        file_ = nullptr;
    }
}

void WavRecorder::write(const float* L, const float* R, int n)
{
    if (!recording_.load(std::memory_order_relaxed)) return;
    const uint32_t head = head_.load(std::memory_order_relaxed);
    const uint32_t tail = tail_.load(std::memory_order_acquire);
    const uint32_t free = mask_ + 1 - (head - tail);
    const uint32_t need = static_cast<uint32_t>(n) * 2u;
    if (need > free) { framesDropped_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed); return; }
    float* r = ring_.data();
    for (int i = 0; i < n; ++i) {
        r[(head + static_cast<uint32_t>(i) * 2u) & mask_] = L[i];
        r[(head + static_cast<uint32_t>(i) * 2u + 1u) & mask_] = R[i];
    }
    head_.store(head + need, std::memory_order_release);
}

void WavRecorder::run()
{
    std::vector<float> chunk(8192);
    for (;;) {
        const uint32_t head = head_.load(std::memory_order_acquire);
        uint32_t tail = tail_.load(std::memory_order_relaxed);
        uint32_t avail = head - tail;
        if (avail == 0) {
            if (stopRequested_.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        while (avail > 0) {
            const uint32_t take = std::min<uint32_t>(avail, static_cast<uint32_t>(chunk.size()));
            for (uint32_t i = 0; i < take; ++i) chunk[i] = ring_[(tail + i) & mask_];
            std::fwrite(chunk.data(), sizeof(float), take, file_);
            framesWritten_.fetch_add(take / 2u, std::memory_order_relaxed);
            tail += take; avail -= take;
        }
        tail_.store(tail, std::memory_order_release);
    }
    std::fflush(file_);
}

void WavRecorder::writeHeader(uint32_t dataBytes)
{
    if (file_ == nullptr) return;
    std::fseek(file_, 0, SEEK_SET);
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, file_); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, file_); };
    std::fwrite("RIFF", 1, 4, file_); u32(36 + dataBytes); std::fwrite("WAVE", 1, 4, file_);
    std::fwrite("fmt ", 1, 4, file_); u32(16); u16(3); u16(static_cast<uint16_t>(channels_));
    u32(static_cast<uint32_t>(sampleRate_)); u32(static_cast<uint32_t>(sampleRate_ * channels_ * 4));
    u16(static_cast<uint16_t>(channels_ * 4)); u16(32);
    std::fwrite("data", 1, 4, file_); u32(dataBytes);
    std::fseek(file_, 0, SEEK_END);
}

} // namespace ambient
