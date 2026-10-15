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

int numPresetClusters() { return ClusterCount; }

const char* presetClusterName(int cluster)
{
    return (cluster >= 0 && cluster < ClusterCount) ? ClusterNames[cluster] : "";
}

const float* presetClusterCentre(int cluster)
{
    return (cluster >= 0 && cluster < ClusterCount) ? ClusterCentres[cluster] : nullptr;
}

int numPresetPhrases() { return PhraseCount; }

const char* presetPhrase(int index)
{
    return (index >= 0 && index < PhraseCount) ? Phrases[index] : "";
}

/**
 * @param m  the measured metadata of one preset; only its cluster field is read
 * @return   the stored cluster when it is valid, else the nearest centroid, else -1 when no
 *           clusters were generated at all
 */
int presetClusterOf(const PresetMeta& m)
{
    if (ClusterCount <= 0) return -1;
    if (m.m_cluster >= 0 && m.m_cluster < ClusterCount) return m.m_cluster;
    const float d[NumMapDescriptors] = { m.m_bright, m.m_motion, m.m_width, m.m_noisy, m.m_bass,
                                          m.m_density, m.m_evolve, m.m_rough, m.m_wet };
    int best = 0;
    float bestD = 1.0e30f;
    for (int c = 0; c < ClusterCount; ++c) {
        float s = 0.0f;
        for (int k = 0; k < NumMapDescriptors; ++k) {
            const float e = d[k] - ClusterCentres[c][k];
            s += e * e;
        }
        if (s < bestD) { bestD = s; best = c; }
    }
    return best;
}

} // namespace ambient
