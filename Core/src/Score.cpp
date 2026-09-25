/**
 * @file Score.cpp
 * @brief The score: parsing and writing the timed-ramp text, and the ramp arithmetic.
 *
 * Score.h holds the player (step() is a template and lives there); this file holds everything
 * around it. parseScoreTime() and writeScoreTime() are the clock notation -- h:mm:ss, m:ss or plain
 * seconds -- that the score shares with nothing else in the program. Score::parse() reads the text
 * line by line, tokenises on blanks, strips '#' comments, resolves the parameter by its key
 * (findParam), accepts a choice by name and a switch as on/off, clamps the value into the
 * parameter's range and refuses the whole text on the first bad line, so a piece is either
 * loaded entirely or not at all. Score::write() is the inverse and produces text that parse()
 * reads back to the same events.
 *
 * Score::rampValue() is where a ramp gets its shape: a float travels in the parameter's skewed
 * domain, the one the knob, the morph and the map blend use, so a logarithmic cutoff sweeps the
 * way the hand would sweep it; a choice, a switch or an integer does not travel at all and flips
 * when the ramp is over. load() and save() are the file forms and allocate, so they belong on the
 * message thread; parse() and write() work on memory the caller owns.
 */
#include "ambient/Score.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ambient {

double parseScoreTime(const char* text)
{
    if (text == nullptr || *text == 0) return -1.0;
    // h:mm:ss, m:ss or plain seconds. Anything else is not a time.
    double parts[3] = { 0.0, 0.0, 0.0 };
    int n = 0;
    const char* p = text;
    while (n < 3) {
        char* end = nullptr;
        const double v = std::strtod(p, &end);
        if (end == p || !std::isfinite(v)) return -1.0;
        parts[n++] = v;
        p = end;
        if (*p != ':') break;
        ++p;
    }
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != 0) return -1.0;
    if (n == 1) return parts[0];
    if (n == 2) return parts[0] * 60.0 + parts[1];
    return parts[0] * 3600.0 + parts[1] * 60.0 + parts[2];
}

int writeScoreTime(double seconds, char* buf, size_t cap)
{
    const long total = static_cast<long>(seconds < 0.0 ? 0.0 : seconds);
    const long h = total / 3600, m = (total % 3600) / 60, s = total % 60;
    if (h > 0) return std::snprintf(buf, cap, "%ld:%02ld:%02ld", h, m, s);
    return std::snprintf(buf, cap, "%ld:%02ld", m, s);
}

bool Score::add(const ScoreEvent& e)
{
    if (count_ >= kMaxScoreEvents) return false;
    events_[count_] = e;
    started_[count_] = false;
    from_[count_] = 0.0f;
    ++count_;
    return true;
}

void Score::rewind()
{
    t_ = 0.0;
    for (int i = 0; i < count_; ++i) { started_[i] = false; from_[i] = 0.0f; }
}

double Score::length() const
{
    double last = 0.0;
    for (int i = 0; i < count_; ++i) last = std::max(last, events_[i].at + events_[i].over);
    return last;
}

bool Score::parse(const char* text)
{
    clear();
    if (text == nullptr) return false;
    std::string line;
    const char* p = text;
    while (true) {
        const char c = *p;
        if (c != 0 && c != '\n' && c != '\r') { line.push_back(c); ++p; continue; }
        // one line collected
        {
            // strip a comment and the surrounding space
            const size_t hash = line.find('#');
            if (hash != std::string::npos) line.resize(hash);
            std::vector<std::string> tok;
            std::string cur;
            for (char ch : line) {
                if (ch == ' ' || ch == '\t') { if (!cur.empty()) { tok.push_back(cur); cur.clear(); } }
                else cur.push_back(ch);
            }
            if (!cur.empty()) tok.push_back(cur);
            if (!tok.empty()) {
                if (tok.size() < 3) return false;
                ScoreEvent e;
                e.at = parseScoreTime(tok[0].c_str());
                if (e.at < 0.0) return false;
                const ParamDesc* d = findParam(tok[1].c_str());
                if (d == nullptr) return false;
                e.target = d->id;
                // A choice or a switch may be written by name as well as by number.
                bool byName = false;
                if (d->kind == ParamKind::Choice)
                    for (int i = 0; i < d->numChoices; ++i)
                        if (tok[2] == d->choices[i]) { e.value = static_cast<float>(i); byName = true; }
                if (!byName && d->kind == ParamKind::Bool && (tok[2] == "on" || tok[2] == "off")) { e.value = tok[2] == "on" ? 1.0f : 0.0f; byName = true; }
                if (!byName) e.value = static_cast<float>(std::atof(tok[2].c_str()));
                if (!std::isfinite(e.value)) e.value = d->def;   // clampv lets NaN through
                e.value = clampv(e.value, d->min, d->max);
                if (tok.size() >= 5 && tok[3] == "over") {
                    e.over = parseScoreTime(tok[4].c_str());
                    if (e.over < 0.0) return false;
                } else if (tok.size() != 3) {
                    return false;
                }
                if (!add(e)) return false;
            }
        }
        line.clear();
        if (c == 0) break;
        ++p;
        if (c == '\r' && *p == '\n') ++p;
    }
    rewind();
    return true;
}

int Score::write(char* buf, size_t cap) const
{
    size_t used = 0;
    char t[32], o[32];
    for (int i = 0; i < count_; ++i) {
        const ScoreEvent& e = events_[i];
        const ParamDesc& d = paramDesc(e.target);
        writeScoreTime(e.at, t, sizeof(t));
        char value[64];
        if (d.kind == ParamKind::Choice) std::snprintf(value, sizeof(value), "%s", d.choices[clampv(static_cast<int>(std::lround(e.value)), 0, d.numChoices - 1)]);
        else if (d.kind == ParamKind::Bool) std::snprintf(value, sizeof(value), "%s", e.value >= 0.5f ? "on" : "off");
        else std::snprintf(value, sizeof(value), "%g", static_cast<double>(e.value));
        int len;
        if (e.over > 0.0) {
            writeScoreTime(e.over, o, sizeof(o));
            len = std::snprintf(buf + used, used < cap ? cap - used : 0, "%s %s %s over %s\n", t, d.key, value, o);
        } else {
            len = std::snprintf(buf + used, used < cap ? cap - used : 0, "%s %s %s\n", t, d.key, value);
        }
        if (len < 0) return -1;
        used += static_cast<size_t>(len);
    }
    if (cap > 0) buf[used < cap ? used : cap - 1] = 0;
    return static_cast<int>(used);
}

float Score::rampValue(ParamId id, float from, float to, float x)
{
    const ParamDesc& d = paramDesc(id);
    x = clampv(x, 0.0f, 1.0f);
    if (d.kind != ParamKind::Float) {
        // A choice, a switch or an integer does not travel: it changes when the ramp is over
        // (a half-open switch is not a thing).
        return x >= 1.0f ? to : from;
    }
    const float span = std::max(d.max - d.min, 1e-9f);
    const float a = std::pow(clampv((from - d.min) / span, 0.0f, 1.0f), d.skew);
    const float b = std::pow(clampv((to - d.min) / span, 0.0f, 1.0f), d.skew);
    return d.min + span * std::pow(a + (b - a) * x, 1.0f / d.skew);
}

bool Score::load(const char* path)
{
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return false;
    std::string text;
    char chunk[4096];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) text.append(chunk, got);
    std::fclose(f);
    return parse(text.c_str());
}

bool Score::save(const char* path) const
{
    std::vector<char> buf(static_cast<size_t>(count_) * 96 + 64);
    const int n = write(buf.data(), buf.size());
    if (n < 0) return false;
    FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return false;
    const bool ok = std::fwrite(buf.data(), 1, static_cast<size_t>(n), f) == static_cast<size_t>(n);
    std::fclose(f);
    return ok;
}

} // namespace ambient
