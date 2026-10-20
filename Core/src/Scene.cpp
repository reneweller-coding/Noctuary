/**
 * @file Scene.cpp
 * @brief The scenes' knobs (Scene.h), Noctuary's.
 * @author Rene Weller
 */
#include "ambient/Scene.h"

namespace ambient
{

const char *const SceneNames[NumScenes] = { "Off", "Forest", "Canyon", "Desert", "Sky", "Ocean" };

namespace
{
/** @brief Forest: a short, very diffuse, dark room; the far plane low and short. */
const SceneSetting Forest[] = {
    { "near_mix", 0.35f },   { "near_decay", 1.2f },  { "near_damp", 0.75f },   { "early_level", 0.2f },
    { "early_size", 5.0f },  { "early_absorb", 0.8f }, { "far_level", 0.3f },   { "far_size", 1.0f },
    { "far_decay", 5.0f },   { "far_damp", 0.8f },     { "far_diffuse", 0.9f }, { "far_predelay", 8.0f },
    { "dly_mix", 0.0f },   { "dly2_mix", 0.0f },   { "far_highcut", 4000.0f }, { "far_lowcut", 300.0f },
    { "near_lowcut", 250.0f },
};
/** @brief Canyon: walls at 85 m and 140 m -- 496 and 816 ms there and back, damped, a little feedback, out of the tempo. */
const SceneSetting Canyon[] = {
    { "dly_mix", 0.25f },     { "dly_time_l", 0.496f }, { "dly_time_r", 0.816f }, { "dly_feedback", 0.15f },
    { "dly_cross", 0.0f },    { "dly_damp", 0.55f },    { "dly_absorb", 0.3f },   { "dly_to_far", 0.25f },
    { "dly2_mix", 0.0f },     { "near_mix", 0.1f },       { "early_level", 0.0f },    { "far_level", 0.45f },
    { "far_decay", 4.0f },      { "far_damp", 0.6f },       { "far_predelay", 40.0f },  { "far_diffuse", 0.5f },
    { "dly_sync_l", 0.0f },     { "dly_sync_r", 0.0f },     { "far_highcut", 5000.0f }, { "far_lowcut", 250.0f },
};
/** @brief Desert: nearly dry, one far echo. */
const SceneSetting Desert[] = {
    { "near_mix", 0.05f },      { "early_level", 0.0f },    { "far_level", 0.12f },     { "far_decay", 3.0f },
    { "far_damp", 0.7f },       { "far_predelay", 60.0f },  { "dly_mix", 0.0f },      { "dly2_mix", 0.12f },
    { "dly2_time_l", 1.1f },  { "dly2_time_r", 1.3f },  { "dly2_feedback", 0.0f }, { "dly2_damp", 0.8f },
    { "dly2_to_far", 0.0f },    { "dly2_sync_l", 0.0f },    { "dly2_sync_r", 0.0f },
};
/** @brief Sky: a very long, diffuse reverb without pre-delay, mixed quietly. */
const SceneSetting Sky[] = {
    { "far_level", 0.65f }, { "far_size", 3.0f },  { "far_decay", 12.0f },  { "far_damp", 0.35f },
    { "far_predelay", 0.0f }, { "far_diffuse", 1.0f }, { "near_mix", 0.05f }, { "early_level", 0.0f },
    { "dly_mix", 0.0f },  { "dly2_mix", 0.0f }, { "far_highcut", 7000.0f }, { "far_lowcut", 200.0f },
};
/** @brief Ocean: a long dark reverb, a slow chorus and a little wow. */
const SceneSetting Ocean[] = {
    { "far_level", 0.6f }, { "far_size", 2.5f },  { "far_decay", 10.0f }, { "far_damp", 0.85f },
    { "far_predelay", 5.0f }, { "far_diffuse", 0.9f }, { "near_mix", 0.1f },  { "early_level", 0.0f },
    { "dly_mix", 0.0f }, { "dly2_mix", 0.0f }, { "far_highcut", 1500.0f }, { "far_lowcut", 150.0f },
    { "ens_mix", 0.5f },   { "ens_rate", 0.07f },  { "ens_depth", 0.6f },   { "patina", 0.25f },
    { "patina_wow", 0.6f },
};

/**
 * @brief A recipe and its length.
 * @tparam N  how many settings
 * @param[in]  s      the recipe
 * @param[out] count  receives N
 * @return the recipe
 */
template <int N> const SceneSetting *give( const SceneSetting ( &s )[N], int &count )
{
    count = N;
    return s;
}
}   // namespace

const SceneSetting *sceneSettings( int scene, int &count )
{
    count = 0;
    switch ( scene )
    {
    case 1: return give( Forest, count );
    case 2: return give( Canyon, count );
    case 3: return give( Desert, count );
    case 4: return give( Sky, count );
    case 5: return give( Ocean, count );
    default: return nullptr;
    }
}

}   // namespace ambient
