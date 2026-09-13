// Noctuary -- a score: what should happen, written down.
//
// The set timeline (Timeline.h) records what you did and plays it back. The map route walks a
// path between presets. Neither lets you write a piece: "start dry, bring the Cosmos in over four
// minutes from the twelfth, open the far reverb at the twentieth, and let it fall away for the
// last ten". That is what this is -- a plain text file of timed ramps, which is the smallest
// thing that makes a composed forty-minute piece repeatable.
//
//   # a comment
//   0:00    brain_density 3
//   2:00    cosmos_send 0.55 over 4:00
//   12:00   far_decay 70 over 6:00
//   30:00   master_gain -18 over 8:00
//
// A line is <time> <parameter key> <value> [over <duration>]. Times are m:ss or h:mm:ss or plain
// seconds. Without "over" the value is set at that moment; with it, the parameter travels there
// from wherever it was when the ramp started -- in the parameter's own skewed domain, so a
// logarithmic knob moves the way the hand would move it.
#pragma once
#include "Params.h"
#include "Dsp.h"   // clampv
#include <cstddef>

namespace ambient {

constexpr int kMaxScoreEvents = 512;

struct ScoreEvent {
    double  at = 0.0;        // seconds from the start of the piece
    double  over = 0.0;      // seconds the ramp takes; 0 = immediately
    ParamId target = ParamId::MasterGain;
    float   value = 0.0f;
};

class Score {
public:
    bool parse(const char* text);            // replaces the score; false on the first bad line
    int  write(char* buf, size_t cap) const; // the text form again
    bool load(const char* path);
    bool save(const char* path) const;

    int  count() const { return count_; }
    const ScoreEvent& event(int i) const { return events_[i]; }
    double length() const;                   // when the last ramp finishes
    void clear() { count_ = 0; }
    bool add(const ScoreEvent& e);

    // Playing. `now` is the seconds since the piece started; call it once per block with the
    // interval that has passed. `valueOf(ParamId) -> float` reads the current value (a ramp needs
    // to know where it starts), `set(ParamId, float)` receives every parameter that moved.
    void  rewind();
    double position() const { return t_; }
    template <class ValueFn, class SetFn>
    void step(double dt, ValueFn&& valueOf, SetFn&& set)
    {
        if (count_ == 0 || dt <= 0.0) return;
        const double t0 = t_, t1 = t_ + dt;
        t_ = t1;
        for (int i = 0; i < count_; ++i) {
            const ScoreEvent& e = events_[i];
            if (t1 < e.at) continue;
            if (t0 >= e.at + e.over && started_[i]) continue;   // finished, and it did run
            if (!started_[i]) { from_[i] = valueOf(e.target); started_[i] = true; }
            if (e.over <= 0.0) { set(e.target, e.value); continue; }
            const double x = clampv((t1 - e.at) / e.over, 0.0, 1.0);
            set(e.target, rampValue(e.target, from_[i], e.value, static_cast<float>(x)));
        }
    }

    // Where a ramp is at fraction x, in the parameter's own perceptual domain (the same one the
    // morph and the map blend use, so a score moves a knob the way a hand would).
    static float rampValue(ParamId id, float from, float to, float x);

private:
    ScoreEvent events_[kMaxScoreEvents];
    int    count_ = 0;
    double t_ = 0.0;
    bool   started_[kMaxScoreEvents] = {};
    float  from_[kMaxScoreEvents] = {};
};

// "3:20" / "1:02:30" / "45" -> seconds; negative if the text is not a time.
double parseScoreTime(const char* text);
int    writeScoreTime(double seconds, char* buf, size_t cap);

} // namespace ambient
