// Noctuary -- a route over the preset map: waypoints (a map position and blend radius)
// with a travel time to reach each one and a hold time to stay there. The engine walks the
// route and moves the map cursor (MapX / MapY / MapRadius), so a whole set becomes a path
// that plays itself: smoothstep travel between points, no jumps, optional loop.
//
// Text form (route presets, plugin state, OSC):
//   point;point;...   point = <preset name>|travel|hold[|radius]   or   x,y|travel|hold[|radius]
// A preset name resolves to that preset's map position (PresetMeta), so routes survive a
// re-measurement of the map. Times in seconds.
#pragma once
#include <cstddef>

namespace ambient {

struct Waypoint {
    float x = 0.5f, y = 0.5f, radius = 0.08f;
    float travel = 60.0f;   // seconds to glide here from the previous point (or from where the cursor was)
    float hold = 60.0f;     // seconds to stay
    int   preset = -1;      // resolved preset index when the point was given by name, else -1
};

struct RoutePreset { const char* name; const char* points; };
int numRoutePresets();
const RoutePreset& routePreset(int index);

class Route {
public:
    static constexpr int kMaxPoints = 32;

    int  count() const { return count_; }
    const Waypoint& point(int i) const { return points_[i]; }
    void clear() { count_ = 0; stop(); }
    bool add(const Waypoint& w) { if (count_ >= kMaxPoints) return false; points_[count_++] = w; return true; }
    bool parse(const char* text);                 // replaces the route; false if a point is malformed or a name unknown
    int  write(char* buf, size_t cap) const;      // text form, returns length (0 if it did not fit)

    // Start walking from the cursor's current position; the first point is reached after its travel time.
    void start(float fromX, float fromY, float fromRadius, bool loop);
    void stop() { running_ = false; }
    bool running() const { return running_; }
    int  segment() const { return seg_; }          // index of the point being approached or held
    float progress() const { return prog_; }       // 0..1 within the current travel or hold

    // Advances by dt (already multiplied by the speed). Writes the cursor; returns false when the
    // route has ended (not looping) or is not running.
    bool update(float dt, float& x, float& y, float& radius);

private:
    Waypoint points_[kMaxPoints];
    int   count_ = 0;
    bool  running_ = false, loop_ = false, holding_ = false;
    int   seg_ = 0;
    float prog_ = 0.0f, elapsed_ = 0.0f;
    float fromX_ = 0.5f, fromY_ = 0.5f, fromR_ = 0.08f;
    float curX_ = 0.5f, curY_ = 0.5f, curR_ = 0.08f;
};

} // namespace ambient
