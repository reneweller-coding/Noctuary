/**
 * @file Route.h
 * @brief A route over the preset map: waypoints (a map position and blend radius)
 *        with a travel time to reach each one and a hold time to stay there.
 *
 * The engine walks the
 * route and moves the map cursor (MapX / MapY / MapRadius), so a whole set becomes a path
 * that plays itself: smoothstep travel between points, no jumps, optional loop.
 *
 * Text form (route presets, plugin state, OSC):
 *   point;point;...   point = \<preset name\>|travel|hold[|radius]   or   x,y|travel|hold[|radius]
 * A preset name resolves to that preset's map position (PresetMeta), so routes survive a
 * re-measurement of the map. Times in seconds.
 */
#pragma once
#include <cstddef>

namespace ambient {

/** @brief One point of a route: where the cursor goes, how long it takes to get there and how long it stays. */
struct Waypoint {
    float x = 0.5f,         ///< @brief map position, 0 .. 1
          y = 0.5f,         ///< @brief map position, 0 .. 1
          radius = 0.08f;   ///< blend radius (0.02 .. 0.4) at this point
    float travel = 60.0f;   ///< seconds to glide here from the previous point (or from where the cursor was)
    float hold = 60.0f;     ///< seconds to stay
    int   preset = -1;      ///< resolved preset index when the point was given by name, else -1
};

/** @brief A built-in route: its name and its text form (see the file comment), parsed by Route::parse. */
struct RoutePreset {
    const char* name;     ///< @brief the name shown in the list
    const char* points;   ///< @brief the route in the text form (see the file comment)
};
/** @return how many built-in routes there are (twelve, Route.cpp) */
int numRoutePresets();
/**
 * @brief One built-in route.
 * @param index  0 .. numRoutePresets()-1, clamped
 * @return       the route's name and text
 */
const RoutePreset& routePreset(int index);

/**
 * @brief The route itself: up to kMaxPoints waypoints and the walk along them.
 *
 * No allocation anywhere, so parse() may replace a route while the engine is running and
 * update() runs at control rate on the audio thread.
 */
class Route {
public:
    static constexpr int kMaxPoints = 32;   ///< the most waypoints a route may hold

    /** @return how many waypoints the route has */
    int  count() const { return count_; }
    /**
     * @brief One waypoint.
     * @param i  0 .. count()-1, unchecked
     * @return   the point
     */
    const Waypoint& point(int i) const { return points_[i]; }
    /** @brief Removes every waypoint and stops the walk. */
    void clear() { count_ = 0; stop(); }
    /**
     * @brief Appends a waypoint.
     * @param w  the point
     * @return   false when the route is full (kMaxPoints)
     */
    bool add(const Waypoint& w) { if (count_ >= kMaxPoints) return false; points_[count_++] = w; return true; }
    /**
     * @brief Parses the text form (see the file comment): replaces the route; false if a point is malformed or a name unknown.
     *
     * Stops the walk. Preset names are looked up in the current preset list (packs included) and resolved to their
     * measured map position; coordinates are clamped to 0 .. 1, times floored at 0, the optional
     * radius clamped to 0.02 .. 0.4. Nothing is changed unless the whole text parses.
     *
     * @param text  points separated by ';', fields by '|'; nullptr is refused
     * @return      true when the whole text parsed and the route was replaced
     */
    bool parse(const char* text);
    /**
     * @brief Writes the text form: a point by preset name where it was given by one, else as x,y.
     * @param buf  receives the NUL-terminated text
     * @param cap  size of @p buf
     * @return     text form, returns length (0 if it did not fit)
     */
    int  write(char* buf, size_t cap) const;

    /**
     * @brief Start walking from the cursor's current position; the first point is reached after its travel time.
     *
     * Does nothing on an empty route.
     *
     * @param fromX       where the map cursor is now, 0 .. 1
     * @param fromY       where the map cursor is now, 0 .. 1
     * @param fromRadius  the blend radius now
     * @param loop        whether to start over at the first point after the last one's hold
     */
    void start(float fromX, float fromY, float fromRadius, bool loop);
    /** @brief Stops the walk; the cursor stays where it is. */
    void stop() { running_ = false; }
    /** @return whether the route is being walked */
    bool running() const { return running_; }
    /** @return index of the point being approached or held */
    int  segment() const { return seg_; }
    /** @return 0..1 within the current travel or hold */
    float progress() const { return prog_; }

    /**
     * @brief Advances by dt (already multiplied by the speed). Writes the cursor; returns false when the
     *        route has ended (not looping) or is not running.
     *
     * Travel is a smoothstep from where the cursor was to the point (position and radius alike);
     * the hold then sits on the point. After the last point's hold the walk either loops or stops.
     * When not running the last cursor position is still written.
     *
     * @param dt      seconds since the last call, times the route speed; negative counts as 0
     * @param x       receives the cursor's x (MapX)
     * @param y       receives the cursor's y (MapY)
     * @param radius  receives the blend radius (MapRadius)
     * @return        whether the route is still running after this step
     */
    bool update(float dt, float& x, float& y, float& radius);

private:
    Waypoint points_[kMaxPoints];                  ///< the route
    int   count_ = 0;                              ///< how many of points_ are in use
    bool  running_ = false,   ///< @brief the route is being walked
          loop_ = false,      ///< @brief start over after the last point
          holding_ = false;   ///< in the hold of seg_ rather than travelling to it
    int   seg_ = 0;                                ///< the waypoint being approached or held
    float prog_ = 0.0f,      ///< @brief progress 0..1 of the current travel or hold
          elapsed_ = 0.0f;   ///< seconds spent in the current travel or hold
    float fromX_ = 0.5f,    ///< @brief x where the current travel started (the previous point, or the cursor at start())
          fromY_ = 0.5f,    ///< @brief y where the current travel started
          fromR_ = 0.08f;   ///< radius where the current travel started
    float curX_ = 0.5f,    ///< @brief the cursor's x now, as update() last wrote it
          curY_ = 0.5f,    ///< @brief the cursor's y now
          curR_ = 0.08f;   ///< the blend radius now
};

} // namespace ambient
