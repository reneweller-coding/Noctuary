// Noctuary -- a journey: presets in a row, each held for a while, each crossfaded into the
// next, round and round if it is meant to be endless (13.09.2026, Rene's sets).
//
// The score (Score.h) writes parameter ramps down; the map route walks a path between presets by
// morphing; neither says "this preset for five to ten minutes, then that one over a minute, and
// when the last is done, the first again". That is a journey -- a plain text file, one preset a
// line, with the dwell and the crossfade as ranges the player draws from, so the same journey
// never runs the same way twice and an evening on it does not repeat:
//
//   # a comment
//   journey Sleep Concert: First Night
//   cyclic on
//   Somnus Bed    | 5:00-10:00 | 0:30-1:30
//   Somnus Vigil  | 4:00-8:00  | 0:20-1:00 | near=Quiet, Please
//   Glacier Bloom | 6:00       | 1:00      | near=auto
//
// A line is <preset> | <dwell> | <fade> [| near=<near preset, auto or keep>]. Times are m:ss,
// h:mm:ss or plain seconds; a range is two times with a dash between them, one time is a fixed
// value. Without a near field the sound preset's own Auto decides the foreground (or what the
// player pinned stays); near=keep leaves the foreground as it is; a name sets that near preset.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace ambient {

struct JourneyStep {
    std::string preset;
    double dwellLo = 300.0, dwellHi = 600.0;   // seconds the preset is held (drawn between)
    double fadeLo = 30.0, fadeHi = 90.0;       // seconds of the crossfade into it (drawn between)
    std::string nearPreset;                    // "", "auto", "keep", or a near preset's name
};

struct Journey {
    std::string name;
    bool cyclic = true;
    std::vector<JourneyStep> steps;

    static std::string timeText(double s)
    {
        char buf[32];
        const int t = static_cast<int>(s + 0.5);
        if (t >= 3600) std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
        else std::snprintf(buf, sizeof(buf), "%d:%02d", t / 60, t % 60);
        return buf;
    }
    static std::string rangeText(double lo, double hi)
    {
        return lo == hi ? timeText(lo) : timeText(lo) + "-" + timeText(hi);
    }
    // m:ss, h:mm:ss or seconds; false when it is none of them.
    static bool parseTime(const std::string& s, double& out)
    {
        int h = 0, m = 0; double sec = 0.0;
        const int colons = static_cast<int>(std::count(s.begin(), s.end(), ':'));
        if (colons == 2) { if (std::sscanf(s.c_str(), "%d:%d:%lf", &h, &m, &sec) != 3) return false; out = h * 3600.0 + m * 60.0 + sec; return true; }
        if (colons == 1) { if (std::sscanf(s.c_str(), "%d:%lf", &m, &sec) != 2) return false; out = m * 60.0 + sec; return true; }
        char* end = nullptr;
        out = std::strtod(s.c_str(), &end);
        return end != nullptr && end != s.c_str();
    }
    static bool parseRange(std::string s, double& lo, double& hi)
    {
        s = trim(s);
        const size_t dash = s.find('-', 1);   // not a leading minus
        if (dash == std::string::npos) { if (!parseTime(s, lo)) return false; hi = lo; return true; }
        if (!parseTime(trim(s.substr(0, dash)), lo) || !parseTime(trim(s.substr(dash + 1)), hi)) return false;
        if (hi < lo) std::swap(lo, hi);
        return true;
    }
    static std::string trim(const std::string& s)
    {
        size_t a = 0, b = s.size();
        while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
        return s.substr(a, b - a);
    }

    // The text form; false on the first bad line (the journey is then what was read up to it).
    bool parse(const char* text)
    {
        name.clear(); cyclic = true; steps.clear();
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            if (line.compare(0, 8, "journey ") == 0) { name = trim(line.substr(8)); continue; }
            if (line.compare(0, 7, "cyclic ") == 0) { const std::string v = trim(line.substr(7)); cyclic = v == "on" || v == "1" || v == "true" || v == "yes"; continue; }
            std::vector<std::string> fields;
            size_t start = 0;
            while (true) {
                const size_t bar = line.find('|', start);
                fields.push_back(trim(line.substr(start, bar == std::string::npos ? std::string::npos : bar - start)));
                if (bar == std::string::npos) break;
                start = bar + 1;
            }
            if (fields.empty() || fields[0].empty()) return false;
            JourneyStep st;
            st.preset = fields[0];
            if (fields.size() > 1 && !fields[1].empty() && !parseRange(fields[1], st.dwellLo, st.dwellHi)) return false;
            if (fields.size() > 2 && !fields[2].empty() && !parseRange(fields[2], st.fadeLo, st.fadeHi)) return false;
            for (size_t k = 3; k < fields.size(); ++k)
                if (fields[k].compare(0, 5, "near=") == 0) st.nearPreset = trim(fields[k].substr(5));
            steps.push_back(st);
        }
        return true;
    }
    std::string text() const
    {
        std::string out = "# Noctuary journey: <preset> | <dwell m:ss or a range> | <fade> [| near=<near preset, auto or keep>]\n";
        out += "journey " + name + "\n";
        out += std::string("cyclic ") + (cyclic ? "on" : "off") + "\n";
        for (const JourneyStep& st : steps) {
            out += st.preset + " | " + rangeText(st.dwellLo, st.dwellHi) + " | " + rangeText(st.fadeLo, st.fadeHi);
            if (!st.nearPreset.empty()) out += " | near=" + st.nearPreset;
            out += "\n";
        }
        return out;
    }
    bool load(const char* path)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::stringstream ss; ss << f.rdbuf();
        return parse(ss.str().c_str());
    }
    bool save(const char* path) const
    {
        std::ofstream f(path, std::ios::binary);
        if (!f) return false;
        const std::string t = text();
        f.write(t.data(), static_cast<std::streamsize>(t.size()));
        return static_cast<bool>(f);
    }
    // The dwell of a whole pass, at the middle of every range.
    double meanLength() const
    {
        double s = 0.0;
        for (const JourneyStep& st : steps) s += 0.5 * (st.dwellLo + st.dwellHi);
        return s;
    }
};

// Plays a journey: which step is on, how long it has left, and what the next one asks for. The
// host advances it with the seconds that passed (a timer, or the render's blocks) and acts on
// the step it is handed -- the preset to bring up, over how many seconds, and what the
// foreground should do. Dwell and fade are drawn from their ranges with the player's own
// generator, so a seed makes a run repeatable and no seed makes an evening.
class JourneyPlayer {
public:
    void start(const Journey& j, uint64_t seed, int startStep = 0)
    {
        journey_ = j;
        rng_ = seed ? seed : 0x9E3779B97F4A7C15ull;
        running_ = !journey_.steps.empty(); left_ = 0.0; passes_ = 0;
        pending_ = running_;
        // The step before the first one asked for: the first advance() moves on to it, and a
        // journey that is not cyclic must not see that first move as its end (it did once).
        const int n = static_cast<int>(journey_.steps.size());
        step_ = running_ ? ((startStep % n + n) % n) - 1 : -1;
    }
    void stop() { running_ = false; pending_ = false; }
    bool running() const { return running_; }
    int  step() const { return step_; }
    int  passes() const { return passes_; }
    double remaining() const { return left_; }
    const Journey& journey() const { return journey_; }

    // Advance by dt. Returns true when a step begins: `step` is it, `fade` the seconds drawn for
    // its crossfade. The first call after start() begins the first step at once.
    bool advance(double dt, JourneyStep& step, double& fade)
    {
        if (!running_) return false;
        if (!pending_) {
            left_ -= dt;
            if (left_ > 0.0) return false;
        }
        pending_ = false;
        const int n = static_cast<int>(journey_.steps.size());
        int next = step_ + 1;
        if (next >= n) {
            if (!journey_.cyclic) { running_ = false; return false; }
            next = 0; ++passes_;
        }
        step_ = next;
        const JourneyStep& st = journey_.steps[static_cast<size_t>(step_)];
        fade = draw(st.fadeLo, st.fadeHi);
        // The dwell counts from now: the crossfade is part of the stay.
        left_ = draw(st.dwellLo, st.dwellHi);
        step = st;
        return true;
    }

private:
    double draw(double lo, double hi)
    {
        rng_ ^= rng_ << 13; rng_ ^= rng_ >> 7; rng_ ^= rng_ << 17;
        const double u = static_cast<double>(rng_ >> 11) * (1.0 / 9007199254740992.0);
        return lo + (hi - lo) * u;
    }
    Journey  journey_;
    uint64_t rng_ = 0x9E3779B97F4A7C15ull;
    int      step_ = -1, passes_ = 0;
    double   left_ = 0.0;
    bool     running_ = false, pending_ = false;
};

} // namespace ambient
