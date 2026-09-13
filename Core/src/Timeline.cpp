#include <cmath>
#include "ambient/Timeline.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ambient {

void SetTimeline::add(const TimelineEvent& e)
{
    // Room was taken in advance (reserve): past it the event is dropped rather than the
    // vector grown, because this is called from the audio thread while a set is recorded.
    if (capacity_ > 0 && events_.size() >= capacity_) return;
    if (events_.empty() || events_.back().t <= e.t) { events_.push_back(e); return; }
    auto it = std::upper_bound(events_.begin(), events_.end(), e, [](const TimelineEvent& a, const TimelineEvent& b) { return a.t < b.t; });
    events_.insert(it, e);
}

void SetTimeline::seek(double t)
{
    cursor_ = 0;
    while (cursor_ < events_.size() && events_[cursor_].t < t) ++cursor_;
}

bool SetTimeline::parse(const char* text)
{
    clear();
    if (text == nullptr) return false;
    const char* s = text;
    while (*s) {
        const char* e = s; while (*e && *e != '\n') ++e;
        char line[256];
        const int len = static_cast<int>(std::min<ptrdiff_t>(e - s, static_cast<ptrdiff_t>(sizeof(line) - 1)));
        std::memcpy(line, s, static_cast<size_t>(len)); line[len] = 0;
        s = (*e == '\n') ? e + 1 : e;
        char* p = line; while (*p == ' ' || *p == '\t' || *p == '\r') ++p;
        if (!*p || *p == '#') continue;
        char* endp = nullptr;
        TimelineEvent ev;
        ev.t = std::strtod(p, &endp);
        // A NaN time compares false with everything: add() would break the ordering and step()
        // would stop on it for good, so everything after it would never play.
        if (endp == p || !std::isfinite(ev.t)) return false;
        p = endp; while (*p == ' ') ++p;
        char kind[16] = {}; int k = 0;
        while (*p && *p != ' ' && k < 15) kind[k++] = *p++;
        while (*p == ' ') ++p;
        if (std::strcmp(kind, "param") == 0) {
            char key[64] = {}; int j = 0;
            while (*p && *p != ' ' && j < 63) key[j++] = *p++;
            const ParamDesc* d = findParam(key);
            if (d == nullptr) return false;
            ev.type = TimelineEvent::Type::Param; ev.a = static_cast<int>(d->id); ev.v = std::strtof(p, nullptr);
            if (!std::isfinite(ev.v)) return false;
        } else if (std::strcmp(kind, "on") == 0) {
            ev.type = TimelineEvent::Type::NoteOn; ev.a = static_cast<int>(std::strtol(p, &endp, 10)); ev.v = std::strtof(endp, nullptr);
        } else if (std::strcmp(kind, "off") == 0) {
            ev.type = TimelineEvent::Type::NoteOff; ev.a = static_cast<int>(std::strtol(p, nullptr, 10));
        } else return false;
        add(ev);
    }
    return true;
}

std::vector<char> SetTimeline::write() const
{
    std::vector<char> out;
    const char* head = "# Noctuary set: seconds, event, arguments\n";
    out.insert(out.end(), head, head + std::strlen(head));
    char line[160];
    for (const TimelineEvent& e : events_) {
        int len = 0;
        switch (e.type) {
        case TimelineEvent::Type::Param:   len = std::snprintf(line, sizeof(line), "%.3f param %s %g\n", e.t, paramDesc(static_cast<ParamId>(e.a)).key, e.v); break;
        case TimelineEvent::Type::NoteOn:  len = std::snprintf(line, sizeof(line), "%.3f on %d %.3f\n", e.t, e.a, e.v); break;
        case TimelineEvent::Type::NoteOff: len = std::snprintf(line, sizeof(line), "%.3f off %d\n", e.t, e.a); break;
        }
        // snprintf returns the length the line WOULD have had; a huge time makes that longer
        // than the buffer, and copying that many bytes reads past it.
        if (len >= static_cast<int>(sizeof(line))) len = static_cast<int>(sizeof(line)) - 1;
        if (len > 0) out.insert(out.end(), line, line + len);
    }
    out.push_back(0);
    return out;
}

bool SetTimeline::save(const char* path) const
{
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const std::vector<char> text = write();
    const bool ok = std::fwrite(text.data(), 1, text.size() - 1, f) == text.size() - 1;
    std::fclose(f);
    return ok;
}

bool SetTimeline::load(const char* path)
{
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::vector<char> text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.insert(text.end(), buf, buf + n);
    std::fclose(f);
    text.push_back(0);
    return parse(text.data());
}

} // namespace ambient
