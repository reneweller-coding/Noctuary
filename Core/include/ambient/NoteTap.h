/**
 * @file NoteTap.h
 * @brief The notes the conductors played in one process() call, for the plugin's MIDI out (02.10.2026; after the
 *        family's generators).
 *
 * The engine writes every note the conductors start or release -- the first conductor (the Cluster Brain) on channel
 * 1, the second, background one on channel 2, the near events on channel 3 -- with its place in the block (the chunk it
 * was played in: the conductors run at the chunk's rate). A key played on a keyboard is not written: it came in as MIDI
 * already. When the voices are released all at once (allNotesOff: a preset leaving, a panic) every note a conductor
 * held gets its off. Fixed capacity, no allocation: the audio thread fills it and the same thread empties it.
 */
#pragma once
#include <cstdint>

namespace ambient {

/** @brief A fixed-capacity list of played notes; clear() before a block, read after it. */
struct NoteTap {
    /** @brief One note-on or note-off as a conductor played it. */
    struct Note {
        int offset;         ///< samples into the block (the start of the chunk it was played in)
        uint8_t channel;    ///< MIDI channel 1..16 (1 the first conductor, 2 the second, 3 the near events)
        uint8_t pitch;      ///< MIDI note number
        uint8_t velocity;   ///< 1..127 for an on, 0 for an off
    };
    static constexpr int kCapacity = 1024;   ///< more notes than any block plays
    Note notes[kCapacity];                   ///< the notes, in the order they were played
    int count = 0;                           ///< how many of notes[] are valid
    /** @brief Forgets the notes of the last block. */
    void clear() { count = 0; }
    /** @brief Adds a note (dropped when full). */
    void add(int offset, int channel, int pitch, float velocity, bool on)
    {
        if (count >= kCapacity || pitch < 0 || pitch > 127) return;
        const int v = on ? (velocity <= 0.0f ? 1 : velocity >= 1.0f ? 127 : 1 + static_cast<int>(velocity * 126.0f + 0.5f)) : 0;
        notes[count++] = Note{ offset < 0 ? 0 : offset, static_cast<uint8_t>(channel), static_cast<uint8_t>(pitch), static_cast<uint8_t>(v) };
    }
};

} // namespace ambient
