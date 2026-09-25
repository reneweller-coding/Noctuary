/**
 * @file PresetMap.cpp
 * @brief The preset map's engine room: the cached parameter table, the neighbour search and the
 *        blend.
 *
 * The map needs every preset's full parameter vector at hand, because a cursor between presets is
 * a weighted mean of its neighbours' parameters. Those vectors are not stored anywhere: a preset is
 * a "key=value" string over the defaults, so warmup() applies every preset once (applyPreset over
 * paramTable) into one flat table of numPresets() x kNumParams floats. With a library of eight
 * thousand presets that is the 1.75 s that used to be paid at every prepare(), which is why it now
 * runs on a thread of its own (warmupAsync) and why the map simply does nothing until ready().
 *
 * The table is handed to the audio thread through an atomic pointer, and a table that has been
 * replaced is retired rather than freed, so a reader that is halfway through blend() when a pack
 * load rebuilds the table keeps reading valid memory. neighbours() and blend() run at control
 * rate on the audio thread and allocate nothing: the six nearest presets are kept in a small
 * sorted list, the Gaussian radius follows the local density of the map, and the blend of every
 * parameter respects its kind -- floats in the skew domain, integers rounded, choices and
 * switches taken from the strongest neighbour.
 */
#include "ambient/PresetMap.h"
#include "ambient/PresetMeta.h"
#include "ambient/Presets.h"
#include "ambient/Dsp.h"
#include <vector>
#include <cmath>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace ambient {

namespace {
/**
 * @brief Every table ever built, the published one included, so that a replaced table is kept
 *        rather than freed.
 *
 * The table the audio thread reads, published as a pointer: a rebuild fills a new one and swaps
 * it in, and the one it replaces is kept rather than freed. Keeping it costs a few megabytes at
 * most (a rebuild happens when packs are loaded, which is once) and it is the only way a reader
 * that is already inside blend() cannot have the ground taken from under it.
 */
std::vector<std::unique_ptr<std::vector<float>>> g_retired;
std::atomic<const float*> g_table { nullptr };  ///< the published table (numPresets() x kNumParams floats), nullptr until the first warmup
std::atomic<int>          g_count { 0 };       ///< presets in the published table
std::atomic<bool>         g_building { false };   ///< set while a warmupAsync() thread is running, so only one runs at a time
std::thread               g_thread;             ///< the warmup thread of warmupAsync(); joined by shutdown() and before a new one starts
std::mutex                g_threadLock;         ///< guards g_thread between warmupAsync() and shutdown()
std::mutex                g_buildLock;          ///< serialises warmup() itself, whichever thread calls it
}

void PresetMap::warmup()
{
    std::lock_guard<std::mutex> lock(g_buildLock);
    const int n = numPresets();
    if (g_count.load(std::memory_order_acquire) == n && g_table.load(std::memory_order_acquire) != nullptr) return;
    auto built = std::make_unique<std::vector<float>>(static_cast<size_t>(n) * kNumParams, 0.0f);
    for (int p = 0; p < n; ++p) {
        float* v = built->data() + static_cast<size_t>(p) * kNumParams;
        for (const ParamDesc& d : paramTable()) v[static_cast<int>(d.id)] = d.def;
        applyPreset(preset(p), [&](ParamId id, float val) { v[static_cast<int>(id)] = val; });
    }
    const float* data = built->data();
    g_retired.push_back(std::move(built));
    g_count.store(n, std::memory_order_release);
    g_table.store(data, std::memory_order_release);
}

void PresetMap::warmupAsync()
{
    if (g_building.exchange(true, std::memory_order_acq_rel)) return;   // one at a time
    // NOT detached. A host scanning plugins builds an instance, destroys it and unloads the
    // library within milliseconds, and a detached thread then runs code that is no longer mapped
    // -- an access violation during the scan, and a blacklisted plugin. The thread is kept so it
    // can be waited for; shutdown() does that, and the host layer calls it when it goes away.
    std::lock_guard<std::mutex> lock(g_threadLock);
    if (g_thread.joinable()) g_thread.join();
    g_thread = std::thread([] { warmup(); g_building.store(false, std::memory_order_release); });
}

void PresetMap::shutdown()
{
    std::lock_guard<std::mutex> lock(g_threadLock);
    if (g_thread.joinable()) g_thread.join();
}

bool PresetMap::ready() { return g_table.load(std::memory_order_acquire) != nullptr; }

const float* PresetMap::presetValues(int index)
{
    const float* t = g_table.load(std::memory_order_acquire);
    if (t == nullptr || index < 0 || index >= g_count.load(std::memory_order_acquire)) return nullptr;
    return t + static_cast<size_t>(index) * kNumParams;
}

PresetMap::Blend PresetMap::neighbours(float x, float y, float radius)
{
    Blend b;
    const int n = std::min(numPresetMeta(), numPresets());
    const float sigma = std::max(radius, 1e-3f);
    // Keep the kNeighbours closest points (insertion into a small sorted list).
    float dist[kNeighbours];
    for (int i = 0; i < n; ++i) {
        const PresetMeta& m = presetMeta(i);
        const float dx = m.x - x, dy = m.y - y;
        const float d = dx * dx + dy * dy;
        int pos = b.count;
        while (pos > 0 && dist[pos - 1] > d) --pos;
        if (pos >= kNeighbours) continue;
        const int last = std::min(b.count, kNeighbours - 1);
        for (int k = last; k > pos; --k) { dist[k] = dist[k - 1]; b.index[k] = b.index[k - 1]; }
        dist[pos] = d; b.index[pos] = i;
        if (b.count < kNeighbours) ++b.count;
    }
    // The radius follows the local density. On a map laid out as a free cloud the presets are
    // not evenly spread any more: in a ball of forty near-identical sounds a fixed radius takes
    // all of them, and in the empty space between two clusters it takes nothing and the blend
    // collapses onto whichever point is nearest. Never smaller than the knob says, and never so
    // small that the third neighbour is already out of reach.
    // Only in a gap, though: standing on a preset's own point still plays that preset and nothing
    // else, whatever the neighbours are doing. The radius grows when the nearest point is already
    // further away than the radius itself -- which is what "the cursor is in the empty space"
    // means -- and not one pixel sooner.
    const float d0 = b.count > 0 ? std::sqrt(dist[0]) : 0.0f;
    const float d3 = std::sqrt(dist[b.count >= 3 ? 2 : 0]);
    const float sig = (b.count > 0 && d0 > sigma) ? std::max(sigma, 0.7f * d3) : sigma;
    float sum = 0.0f;
    for (int k = 0; k < b.count; ++k) { b.weight[k] = std::exp(-dist[k] / (2.0f * sig * sig)); sum += b.weight[k]; }
    if (sum <= 1e-12f) {   // far from everything: the nearest point alone
        for (int k = 0; k < b.count; ++k) b.weight[k] = (k == 0) ? 1.0f : 0.0f;
    } else {
        for (int k = 0; k < b.count; ++k) b.weight[k] /= sum;
    }
    return b;
}

void PresetMap::blend(const Blend& b, float* out)
{
    // The neighbours' vectors are looked up once, not once per parameter per neighbour: this runs
    // at control rate on the audio thread and the table is behind an atomic now.
    const float* row[kNeighbours] = {};
    const bool have = ready() && b.count > 0;
    if (have)
        for (int k = 0; k < b.count; ++k)
            if ((row[k] = presetValues(b.index[k])) == nullptr) return;   // a rebuild caught us: leave the sound alone
    for (const ParamDesc& d : paramTable()) {
        const int i = static_cast<int>(d.id);
        if (!have) { out[i] = d.def; continue; }
        switch (d.kind) {
        case ParamKind::Float: {
            const float span = std::max(d.max - d.min, 1e-9f);
            float p = 0.0f;
            for (int k = 0; k < b.count; ++k)
                p += b.weight[k] * std::pow(clampv((row[k][i] - d.min) / span, 0.0f, 1.0f), d.skew);
            out[i] = d.min + span * std::pow(clampv(p, 0.0f, 1.0f), 1.0f / d.skew);
            break;
        }
        case ParamKind::Int: {
            float v = 0.0f;
            for (int k = 0; k < b.count; ++k) v += b.weight[k] * row[k][i];
            out[i] = static_cast<float>(std::lround(v));
            break;
        }
        default:
            out[i] = row[0][i];
        }
    }
}

} // namespace ambient
