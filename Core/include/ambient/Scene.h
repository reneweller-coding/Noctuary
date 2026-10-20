/**
 * @file Scene.h
 * @brief Scene (20.10.2026): the space set as a place -- forest, canyon, desert, sky, ocean.
 *
 * Robert Rich shaped the halls and the sound of What We Left Behind after forests, mountains, deserts, skies and the
 * ocean, so that everything seemed to happen outdoors. A scene sets the space's knobs to such a place -- once, when
 * it is chosen: the knobs then stand where the place put them and turn from there, and a preset keeps them. What each
 * place does (the acoustics after the mixing notes of 20.10.2026):
 *
 *  - Forest: many small scatterings off trunks and leaves, hardly an echo, the highs damped hard -- a short, very
 *    diffuse room, dark, the far plane low.
 *  - Canyon: a few late, distinct echoes off rock walls, each darker -- the echo's two sides at the walls' distances,
 *    85 m and 140 m (there and back at 343 m/s: 496 and 816 ms), damped, a little feedback, out of the tempo.
 *  - Desert: almost no reflections -- nearly dry, distance by level and lost highs alone, one far echo at most.
 *  - Sky: no real room, an image of endless width -- a very long, diffuse reverb without pre-delay, mixed quietly.
 *  - Ocean: damped highs, slow movement -- a long dark reverb, a slow chorus and a little wow.
 *
 * Off changes nothing. The render tools apply a scene where --set scene=... stands, so later --set values win.
 * @author Rene Weller
 */
#pragma once

namespace ambient
{

constexpr int NumScenes = 6;   ///< Off and the five places
extern const char *const SceneNames[NumScenes];   ///< "Off", "Forest", "Canyon", "Desert", "Sky", "Ocean"

/** @brief One knob a scene sets: its key and its value. */
struct SceneSetting
{
    const char *m_key;   ///< the parameter's key
    float       m_value;   ///< the value it is set to
};

/**
 * @brief The knobs a scene sets.
 * @param[in]  scene  the scene, 0 .. NumScenes - 1
 * @param[out] count  receives how many
 * @return the settings, or null for Off (and anything out of range)
 */
const SceneSetting *sceneSettings( int scene, int &count );

}   // namespace ambient
