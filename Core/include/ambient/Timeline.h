// Noctuary -- a set as a timeline: every parameter change and every note with its time,
// recorded while playing (hands, knobs, OSC, MIDI, routes -- everything ends up as parameter
// changes) and played back later, live or offline. A good set becomes reproducible, and
// `ambient_render --set-file` renders it again at any length or sample rate.
//
// Text form (.ambientset), one event per line, time in seconds from the start:
//   12.500 param far_decay 42.0
//   13.020 on 57 0.80
//   40.000 off 57
// Lines starting with '#' are comments. Events are kept sorted by time.
#pragma once
#include "Params.h"
#include <cstddef>
#include <vector>

namespace ambient {

struct TimelineEvent {
    enum class Type : int { Param = 0, NoteOn = 1, NoteOff = 2 };
    double t = 0.0;
    Type   type = Type::Param;
    int    a = 0;       // ParamId or note
    float  v = 0.0f;    // value or velocity
};

class SetTimeline {
public:
    void clear() { events_.clear(); cursor_ = 0; }
    size_t size() const { return events_.size(); }
    double length() const { return events_.empty() ? 0.0 : events_.back().t; }
    const TimelineEvent& event(size_t i) const { return events_[i]; }

    // Recording. Called from the audio thread while a set is being recorded, so it must not
    // allocate: reserve() takes the room first and add() drops what does not fit rather than
    // growing the vector under the reader's feet. An hour of busy playing is a few thousand
    // events; the default room is a quarter of a million.
    void reserve(size_t events) { events_.reserve(events); capacity_ = events; }
    bool full() const { return events_.size() >= capacity_; }
    void add(const TimelineEvent& e);

    // Playback: seek, then step() emits every event with from <= t < to, in order.
    void seek(double t);
    template <class Sink> void step(double from, double to, Sink&& sink)
    {
        while (cursor_ < events_.size() && events_[cursor_].t < to) {
            if (events_[cursor_].t >= from) sink(events_[cursor_]);
            ++cursor_;
        }
    }
    bool finished() const { return cursor_ >= events_.size(); }

    // Text form. save/load allocate (message thread); parse/write work on memory.
    bool parse(const char* text);
    std::vector<char> write() const;   // NUL-terminated
    bool save(const char* path) const;
    bool load(const char* path);

private:
    std::vector<TimelineEvent> events_;
    size_t capacity_ = 0;   // what reserve() promised; add() never goes past it
    size_t cursor_ = 0;
};

} // namespace ambient
