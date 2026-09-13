// Noctuary -- framework-free WAV recorder: the audio thread pushes frames into a
// lock-free ring, a background thread writes 32-bit float WAV. For the Quest app
// (the desktop plugin records through JUCE) and for any host without a writer.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace ambient {

class WavRecorder {
public:
    WavRecorder() = default;
    ~WavRecorder() { stop(); }
    bool start(const char* path, int sampleRate, int channels = 2);
    void stop();                                    // finalises the header
    bool recording() const { return recording_.load(std::memory_order_relaxed); }
    // Audio thread: interleaves L/R into the ring. Frames that do not fit are dropped and counted.
    void write(const float* L, const float* R, int n);
    uint64_t framesWritten() const { return framesWritten_.load(std::memory_order_relaxed); }
    uint64_t framesDropped() const { return framesDropped_.load(std::memory_order_relaxed); }
    double seconds() const { return sampleRate_ > 0 ? static_cast<double>(framesWritten()) / sampleRate_ : 0.0; }
private:
    void run();
    void writeHeader(uint32_t dataBytes);
    std::vector<float> ring_;
    std::atomic<uint32_t> head_{ 0 }, tail_{ 0 };     // in floats (frames * channels)
    std::atomic<bool> recording_{ false }, stopRequested_{ false };
    std::atomic<uint64_t> framesWritten_{ 0 }, framesDropped_{ 0 };
    std::thread thread_;
    FILE* file_ = nullptr;
    int sampleRate_ = 0, channels_ = 2;
    uint32_t mask_ = 0;
};

} // namespace ambient
