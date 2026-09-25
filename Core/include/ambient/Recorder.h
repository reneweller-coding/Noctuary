/**
 * @file Recorder.h
 * @brief Framework-free WAV recorder: the audio thread pushes frames into a
 *        lock-free ring, a background thread writes 32-bit float WAV.
 *
 * For the Quest app
 * (the desktop plugin records through JUCE) and for any host without a writer.
 *
 * The ring holds about five seconds of stereo at 48 kHz (Recorder.cpp), the writer wakes every
 * 20 ms and drains it in 8192-float chunks, and the RIFF header is written with a zero data length
 * at start() and rewritten with the real one at stop(), so a file whose recording was interrupted
 * is still readable up to where the writer got. Frames the ring cannot take are dropped -- never
 * blocked on -- and counted, so the audio thread never waits for the disk.
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace ambient {

/**
 * @brief The recorder: one file at a time, written by its own thread from a ring the audio thread fills.
 *
 * start() and stop() belong to the message thread; write() is the only method meant for the audio
 * thread, and the counters may be read from anywhere. Single producer, single consumer.
 */
class WavRecorder {
public:
    /** @brief An idle recorder: no file, no thread, until start(). */
    WavRecorder() = default;
    /** @brief Stops a recording still running, finalising the header (see stop()). */
    ~WavRecorder() { stop(); }
    /**
     * @brief Opens the file, writes a provisional header and starts the writer thread.
     *
     * Any recording still running is stopped first. Allocates the ring (512k floats), so message
     * thread only.
     *
     * @param path        the file to create (truncated if it exists)
     * @param sampleRate  the rate the frames given to write() are at, in Hz, written into the header
     * @param channels    channels written into the header; write() itself always interleaves two
     * @return            false when the file could not be opened
     */
    bool start(const char* path, int sampleRate, int channels = 2);
    /**
     * @brief Stops recording and finalises the header.
     *
     * Joins the writer thread (which first drains what is left in the ring), rewrites the RIFF
     * and data lengths from the frames actually written (clamped to 32 bits) and closes the file.
     * Safe to call when nothing is recording.
     */
    void stop();
    /** @return whether a recording is in progress (true between start() and stop()) */
    bool recording() const { return recording_.load(std::memory_order_relaxed); }
    /**
     * @brief Audio thread: interleaves L/R into the ring. Frames that do not fit are dropped and counted.
     *
     * Lock-free and allocation-free; does nothing when not recording. A block is taken whole or not
     * at all, so a dropped block never leaves a half frame in the ring.
     *
     * @param L  left samples
     * @param R  right samples
     * @param n  frames in both
     */
    void write(const float* L, const float* R, int n);
    /** @return frames the writer thread has put into the file so far */
    uint64_t framesWritten() const { return framesWritten_.load(std::memory_order_relaxed); }
    /** @return frames write() had to drop because the ring was full (the disk fell behind) */
    uint64_t framesDropped() const { return framesDropped_.load(std::memory_order_relaxed); }
    /** @return the length of the recording written so far, in seconds (0 before start()) */
    double seconds() const { return sampleRate_ > 0 ? static_cast<double>(framesWritten()) / sampleRate_ : 0.0; }
private:
    /** @brief The writer thread's loop: drains the ring to the file in chunks until stop() asks and the ring is empty. */
    void run();
    /**
     * @brief Writes the 44-byte RIFF/WAVE header (format 3, 32-bit float) at the start of the file.
     * @param dataBytes  length of the data chunk to write into the header; 0 while recording
     */
    void writeHeader(uint32_t dataBytes);
    std::vector<float> ring_;                         ///< the lock-free ring, a power of two of floats
    std::atomic<uint32_t> head_{ 0 },   ///< @brief write position, in floats, advanced by the audio thread
                          tail_{ 0 };   ///< in floats (frames * channels): the read position, advanced by the writer thread
    std::atomic<bool> recording_{ false },       ///< @brief gates write(): true between start() and stop()
                      stopRequested_{ false };   ///< tells run() to drain what is left and leave
    std::atomic<uint64_t> framesWritten_{ 0 },   ///< @brief frames the writer put into the file (framesWritten())
                          framesDropped_{ 0 };   ///< frames write() dropped for lack of room (framesDropped())
    std::thread thread_;                              ///< the writer thread, joined in stop()
    FILE* file_ = nullptr;                            ///< the open file, nullptr when idle
    int sampleRate_ = 0,   ///< @brief the rate start() was told, for the header and seconds()
        channels_ = 2;     ///< the channel count start() was told, for the header
    uint32_t mask_ = 0;                               ///< ring size minus one, for wrapping indices
};

} // namespace ambient
