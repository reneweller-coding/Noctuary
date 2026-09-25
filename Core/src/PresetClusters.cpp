/**
 * @file PresetClusters.cpp
 * @brief The measured groups of the preset library.
 *
 * The centroids are generated (PresetClusters.inc);
 * which group a preset belongs to is worked out here from its own descriptors, so a built-in
 * measured by one tool and a pack preset measured by another are judged by the same yardstick.
 *
 * The same translation unit carries the phrase vocabulary (PresetPhrases.inc): the written list
 * CLAP chose a preset's "sounds like" phrases from (PresetMeta::phrase), handed out by index to the
 * browser's info card (PresetText.cpp). Both tables are written by Tools/library/map_all.py and
 * never by hand; this file only answers the questions PresetMeta.h declares over them.
 */
#include "ambient/PresetMeta.h"
#include <cmath>

namespace ambient {
namespace {
#include "PresetClusters.inc"
#include "PresetPhrases.inc"
}   // namespace

int numPresetClusters() { return kClusterCount; }

const char* presetClusterName(int cluster)
{
    return (cluster >= 0 && cluster < kClusterCount) ? kClusterNames[cluster] : "";
}

const float* presetClusterCentre(int cluster)
{
    return (cluster >= 0 && cluster < kClusterCount) ? kClusterCentres[cluster] : nullptr;
}

int numPresetPhrases() { return kPhraseCount; }

const char* presetPhrase(int index)
{
    return (index >= 0 && index < kPhraseCount) ? kPhrases[index] : "";
}

/**
 * @param m  the measured metadata of one preset; only its cluster field is read
 * @return   the stored cluster when it is valid, else the nearest centroid, else -1 when no
 *           clusters were generated at all
 */
int presetClusterOf(const PresetMeta& m)
{
    if (kClusterCount <= 0) return -1;
    if (m.cluster >= 0 && m.cluster < kClusterCount) return m.cluster;
    const float d[kNumMapDescriptors] = { m.bright, m.motion, m.width, m.noisy, m.bass,
                                          m.density, m.evolve, m.rough, m.wet };
    int best = 0;
    float bestD = 1.0e30f;
    for (int c = 0; c < kClusterCount; ++c) {
        float s = 0.0f;
        for (int k = 0; k < kNumMapDescriptors; ++k) {
            const float e = d[k] - kClusterCentres[c][k];
            s += e * e;
        }
        if (s < bestD) { bestD = s; best = c; }
    }
    return best;
}

} // namespace ambient
