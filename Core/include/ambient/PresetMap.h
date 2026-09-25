/**
 * @file PresetMap.h
 * @brief The preset map: every built-in preset is a point in a plane (PresetMeta);
 *        a cursor anywhere in the plane blends the presets around it, weighted by a Gaussian of
 *        the distance (radius = its sigma).
 *
 * On a point the blend is that preset; between points
 * it is a new sound that has never been saved. The engine runs this at control rate when
 * Map is active (MapX / MapY / MapRadius), so hands, OSC, MIDI or automation can wander
 * the map, and the browser draws it.
 *
 * The blend needs every preset's full parameter vector, which is expensive to build from the
 * text of eight thousand presets (PresetMap.cpp): it is built once by warmup(), published behind
 * an atomic pointer, and until it is there the map simply does not engage.
 */
#pragma once
#include "Params.h"
#include <cstdint>

namespace ambient {

/**
 * @brief The map's blending machinery: a cached table of every preset's parameters and the
 *        Gaussian neighbour weights over it.
 *
 * Everything is static -- there is one map per process, shared by every instrument instance.
 * neighbours() and blend() are real-time safe once ready(); warmup() is not.
 */
class PresetMap {
public:
    static constexpr int kNeighbours = 6;   ///< how many of the closest presets take part in a blend

    /**
     * @brief Builds the cached parameter vectors of all presets (allocates; call before the audio thread
     *        needs blend()).
     *
     * With a library loaded this reads eight thousand presets, so it is NOT done
     * while an instrument is being prepared -- it used to be, and cost 1.75 seconds every time a
     * plugin instance was created or an offline render started. warmupAsync() puts it on a thread
     * of its own and returns at once; until it is ready() the map simply does not engage, which
     * is a knob that answers a moment late rather than an instrument that hangs.
     *
     * Idempotent: a table that already covers numPresets() presets is kept. A rebuild after packs
     * were loaded publishes a new table and retires the old one rather than freeing it, so a reader
     * inside blend() keeps valid memory. Serialised by a mutex; message or worker thread only.
     */
    static void warmup();
    /**
     * @brief Runs warmup() on a thread of its own and returns at once.
     *
     * One build at a time: a second call while one runs does nothing. The thread is kept, not
     * detached, so shutdown() can wait for it before the library is unloaded.
     */
    static void warmupAsync();
    /**
     * @brief Waits for a warmup that is still running. A plugin must call this before its library
     *        can be unloaded, or the thread outlives the code it is executing.
     */
    static void shutdown();
    /** @return whether a table has been published, i.e. blend() can do anything */
    static bool ready();

    /** @brief The result of a neighbour search: which presets, and how much of each. */
    struct Blend {
        int   index[kNeighbours];    ///< preset indices, closest first; count of them are valid
        float weight[kNeighbours];   ///< normalised, descending
        int   count = 0;             ///< how many neighbours were found (0 when there are no presets)
    };
    /**
     * @brief Neighbour weights for a cursor (0..1 plane). Never allocates.
     *
     * Keeps the kNeighbours closest points by squared distance and weights them by a Gaussian of
     * sigma @p radius. In a gap -- the nearest point further away than the radius -- the sigma
     * grows to reach the third neighbour, so the blend never collapses onto one point between two
     * clusters; standing on a point still plays that preset alone. Far from everything the nearest
     * point gets weight 1.
     *
     * @param x       cursor position, 0 .. 1 (MapX)
     * @param y       cursor position, 0 .. 1 (MapY)
     * @param radius  the Gaussian's sigma in plane units (MapRadius), floored at 0.001
     * @return        the neighbours and their weights
     */
    static Blend neighbours(float x, float y, float radius);
    /**
     * @brief Blended parameter vector (kNumParams values): floats in the skew domain, ints
     *        rounded, choices and switches from the strongest neighbour. Never allocates.
     *
     * Audio thread, control rate. Before ready(), or with no neighbours, every parameter is its
     * default; if a rebuild pulls the table away mid-call, @p out is left untouched.
     *
     * @param b    the neighbours, from neighbours()
     * @param out  kNumParams floats, indexed by ParamId, in each parameter's real range
     */
    static void blend(const Blend& b, float* out);
    /**
     * @brief The cached vector of one preset (kNumParams values), or nullptr before warmup.
     * @param index  preset index as numPresets() counts them
     * @return       a pointer into the published table (valid for the life of the process), or
     *               nullptr before warmup or for an index the table does not cover
     */
    static const float* presetValues(int index);
};

} // namespace ambient
