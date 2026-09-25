/**
 * @file Timeline.h
 * @brief A set as a timeline: every parameter change and every note with its time,
 *        recorded while playing (hands, knobs, OSC, MIDI, routes -- everything ends up as parameter
 *        changes) and played back later, live or offline.
 *
 * A good set becomes reproducible, and
 * `ambient_render --set-file` renders it again at any length or sample rate.
 *
 * Text form (.ambientset), one event per line, time in seconds from the start:
 *   12.500 param far_decay 42.0
 *   13.020 on 57 0.80
 *   40.000 off 57
 * Lines starting with '#' are comments. Events are kept sorted by time.
 */
#pragma once
#include "Params.h"
#include <cstddef>
#include <vector>

namespace ambient {

/** @brief One event of a set: a parameter written, a note begun or a note let go, at a time. */
struct TimelineEvent {
    /** @brief What happened: Param is a parameter change (a the ParamId, v the value in its real range), NoteOn a note begun (a the MIDI note, v the velocity 0..1), NoteOff a note let go (a the MIDI note). */
    enum class Type : int { Param = 0, NoteOn = 1, NoteOff = 2 };
    double t = 0.0;     ///< seconds from the start of the set
    Type   type = Type::Param;   ///< which of the three
    int    a = 0;       ///< ParamId or note
    float  v = 0.0f;    ///< value or velocity
};

/**
 * @brief The recorded set: a time-sorted list of events with a playback cursor.
 *
 * Recording (add) happens on the audio thread into room reserved beforehand; playback (seek, step)
 * emits events in order into a sink; the text form goes to and from .ambientset files on the
 * message thread.
 */
class SetTimeline {
public:
    /** @brief Forgets every event and rewinds the cursor; the reserved room stays. */
    void clear() { events_.clear(); cursor_ = 0; }
    /** @return how many events are recorded */
    size_t size() const { return events_.size(); }
    /** @return the time of the last event in seconds, 0 when empty */
    double length() const { return events_.empty() ? 0.0 : events_.back().t; }
    /**
     * @brief One event, in time order.
     * @param i  0 .. size()-1, unchecked
     * @return   the event
     */
    const TimelineEvent& event(size_t i) const { return events_[i]; }

    /**
     * @brief Recording. Called from the audio thread while a set is being recorded, so it must not
     *        allocate: reserve() takes the room first and add() drops what does not fit rather than
     *        growing the vector under the reader's feet.
     *
     * An hour of busy playing is a few thousand
     * events; the default room is a quarter of a million.
     *
     * Message thread: allocates.
     *
     * @param events  how many events to make room for; add() never goes past it
     */
    void reserve(size_t events) { events_.reserve(events); capacity_ = events; }
    /** @return whether the reserved room is used up, so add() would drop the next event */
    bool full() const { return events_.size() >= capacity_; }
    /**
     * @brief Records one event, keeping the list sorted by time.
     *
     * Appends when the event is not earlier than the last (the common case while recording),
     * otherwise inserts at its place. Dropped without a word when the room from reserve() is
     * used up. Audio thread safe within that room.
     *
     * @param e  the event
     */
    void add(const TimelineEvent& e);

    /**
     * @brief Playback: seek, then step() emits every event with from <= t < to, in order.
     *
     * Puts the cursor on the first event at or after @p t.
     *
     * @param t  seconds from the start
     */
    void seek(double t);
    /**
     * @brief Emits every event with from <= t < to, in order, and moves the cursor past them.
     *
     * Events before @p from that the cursor still stands on are skipped without being emitted.
     * Call once per block with the interval the block covers.
     *
     * @tparam Sink  callable as sink(const TimelineEvent&)
     * @param from   start of the interval in seconds (inclusive)
     * @param to     end of the interval in seconds (exclusive)
     * @param sink   receives each event
     */
    template <class Sink> void step(double from, double to, Sink&& sink)
    {
        while (cursor_ < events_.size() && events_[cursor_].t < to) {
            if (events_[cursor_].t >= from) sink(events_[cursor_]);
            ++cursor_;
        }
    }
    /** @return whether the cursor has passed the last event */
    bool finished() const { return cursor_ >= events_.size(); }

    /**
     * @brief Text form. save/load allocate (message thread); parse/write work on memory.
     *
     * Replaces the contents with the events in @p text (see the file comment for the format).
     * Parameters are named by key; a NaN or infinite time or value, an unknown key or an unknown
     * event word refuses the whole text. Events are added through add(), so they end up sorted.
     *
     * @param text  the whole file, NUL-terminated; nullptr is refused
     * @return      true when every line parsed
     */
    bool parse(const char* text);
    /**
     * @brief The text form of every event, with a header comment line.
     * @return the text, NUL-terminated (the terminator is included in the vector's size)
     */
    std::vector<char> write() const;
    /**
     * @brief Writes the text form to a file.
     * @param path  the file to create
     * @return      false when the file could not be opened or written in full
     */
    bool save(const char* path) const;
    /**
     * @brief Reads a file and parses it (see parse()).
     * @param path  the file to read
     * @return      false when the file is missing or a line is malformed
     */
    bool load(const char* path);

private:
    std::vector<TimelineEvent> events_;   ///< the events, sorted by time
    size_t capacity_ = 0;   ///< what reserve() promised; add() never goes past it
    size_t cursor_ = 0;     ///< playback position: the next event step() will look at
};

} // namespace ambient
