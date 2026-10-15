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
    if (m_count >= MaxScoreEvents) return false;
    m_events[m_count] = e;
    m_started[m_count] = false;
    m_from[m_count] = 0.0f;
    ++m_count;
    return true;
}

void Score::rewind()
{
    m_t = 0.0;
    for (int i = 0; i < m_count; ++i) { m_started[i] = false; m_from[i] = 0.0f; }
}

double Score::length() const
{
    double last = 0.0;
    for (int i = 0; i < m_count; ++i) last = std::max(last, m_events[i].m_at + m_events[i].m_over);
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
                e.m_at = parseScoreTime(tok[0].c_str());
                if (e.m_at < 0.0) return false;
                const ParamDesc* d = findParam(tok[1].c_str());
                if (d == nullptr) return false;
                e.m_target = d->m_id;
                // A choice or a switch may be written by name as well as by number.
                bool byName = false;
                if (d->m_kind == ParamKindE::CHOICE)
                    for (int i = 0; i < d->m_numChoices; ++i)
                        if (tok[2] == d->m_choices[i]) { e.m_value = static_cast<float>(i); byName = true; }
                if (!byName && d->m_kind == ParamKindE::BOOL && (tok[2] == "on" || tok[2] == "off")) { e.m_value = tok[2] == "on" ? 1.0f : 0.0f; byName = true; }
                if (!byName) e.m_value = static_cast<float>(std::atof(tok[2].c_str()));
                if (!std::isfinite(e.m_value)) e.m_value = d->m_def;   // clampv lets NaN through
                e.m_value = clampv(e.m_value, d->m_min, d->m_max);
                if (tok.size() >= 5 && tok[3] == "over") {
                    e.m_over = parseScoreTime(tok[4].c_str());
                    if (e.m_over < 0.0) return false;
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
    for (int i = 0; i < m_count; ++i) {
        const ScoreEvent& e = m_events[i];
        const ParamDesc& d = paramDesc(e.m_target);
        writeScoreTime(e.m_at, t, sizeof(t));
        char value[64];
        if (d.m_kind == ParamKindE::CHOICE) std::snprintf(value, sizeof(value), "%s", d.m_choices[clampv(static_cast<int>(std::lround(e.m_value)), 0, d.m_numChoices - 1)]);
        else if (d.m_kind == ParamKindE::BOOL) std::snprintf(value, sizeof(value), "%s", e.m_value >= 0.5f ? "on" : "off");
        else std::snprintf(value, sizeof(value), "%g", static_cast<double>(e.m_value));
        int len;
        if (e.m_over > 0.0) {
            writeScoreTime(e.m_over, o, sizeof(o));
            len = std::snprintf(buf + used, used < cap ? cap - used : 0, "%s %s %s over %s\n", t, d.m_key, value, o);
        } else {
            len = std::snprintf(buf + used, used < cap ? cap - used : 0, "%s %s %s\n", t, d.m_key, value);
        }
        if (len < 0) return -1;
        used += static_cast<size_t>(len);
    }
    if (cap > 0) buf[used < cap ? used : cap - 1] = 0;
    return static_cast<int>(used);
}

float Score::rampValue(ParamIdE id, float from, float to, float x)
{
    const ParamDesc& d = paramDesc(id);
    x = clampv(x, 0.0f, 1.0f);
    if (d.m_kind != ParamKindE::FLOAT) {
        // A choice, a switch or an integer does not travel: it changes when the ramp is over
        // (a half-open switch is not a thing).
        return x >= 1.0f ? to : from;
    }
    const float span = std::max(d.m_max - d.m_min, 1e-9f);
    const float a = std::pow(clampv((from - d.m_min) / span, 0.0f, 1.0f), d.m_skew);
    const float b = std::pow(clampv((to - d.m_min) / span, 0.0f, 1.0f), d.m_skew);
    return d.m_min + span * std::pow(a + (b - a) * x, 1.0f / d.m_skew);
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
    std::vector<char> buf(static_cast<size_t>(m_count) * 96 + 64);
    const int n = write(buf.data(), buf.size());
    if (n < 0) return false;
    FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return false;
    const bool ok = std::fwrite(buf.data(), 1, static_cast<size_t>(n), f) == static_cast<size_t>(n);
    std::fclose(f);
    return ok;
}

} // namespace ambient
