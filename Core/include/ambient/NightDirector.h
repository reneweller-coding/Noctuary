/**
 * @file NightDirector.h
 * @brief The night director (20.11.2026, plan D3): a night of so many hours as a journey with a form -- falling
 *        asleep, a deep middle in waves of about ninety minutes, a morning -- composed from the library along a curve
 *        of measured stillness and brightness, every crossfade a step to something that measures alike.
 *
 * A journey of the library is twelve presets of one pack, round and round, the same all through the evening. A night
 * has eight hours and a course; Robert Rich's sleep concerts are the model, and Empyrean's director of the same day
 * (its NightDirector.h) the sister this one follows. The director draws the night as a curve over its position x
 * (0 .. 1) and two measured quantities, each as a percentile of the presets it may choose from -- so the same curve
 * serves the whole library, one family of packs or a single pack:
 *
 *  - **falling asleep**, the first 8 % of the night (30 to 45 minutes, at most a quarter of it): the stillness eases
 *    from the 55th percentile down to the 6th, the brightness from the 50th to the 12th (a half cosine each);
 *  - **the middle**: deep, with waves -- as many as fit at about ninety minutes each (round( middle / 90 min ), one at
 *    the least), each rising from the deep and falling back, ((1 - cos 2 pi f) / 2)^2 so most of a wave lies near the
 *    deep, up to 30 points of stillness and 15 of brightness at the crest, the later waves a little higher (0.6 of that
 *    at the first, all of it at the last: sleep grows lighter towards the morning);
 *  - **the morning**, the last 8 % (30 to 45 minutes): from the deep up to the 65th percentile of stillness and the
 *    85th of brightness -- brighter, and moving a little more.
 *
 * What is measured (Tools/make_night.py, NightDirector.inc, from the library's own measurement of a minute of every
 * pack preset, work/library/packs.json, the one the journey templates are made from):
 *  - **stillness**: the mean of three standardised measures of motion -- the spectral flux (the moment) and the
 *    evolution of the level and of the tone over the minute; lower is stiller. Without the brightness, which is the
 *    curve's other axis.
 *  - **brightness**: the spectral centroid (the map's Bright).
 *  - **roughness** (Plomp-Levelt, of the preset's own spectrum): the rougher two fifths of the candidates are kept out
 *    of the falling asleep and of the deep (where the curve wants the stillest fifth or less) -- a grinding spectrum is
 *    the last thing to fall asleep to. In the waves' crests and the morning it counts for nothing.
 *  - The bass share is not used: over the library it mirrors the brightness (Spearman -0.85), so it would add little
 *    but a second name for the same axis.
 *
 * The night is walked step by step. At each, among the presets not yet played, the one with the lowest cost is taken:
 * its distance from the curve's target (in percentiles, the brightness weighing 0.7) plus 0.3 times its distance in
 * sound from the step before -- the journeys' own distance (Tools/make_journeys.py: eighteen standardised descriptors,
 * Euclidean; divided by 5.7, a random pair's median) -- less 0.1 for a preferred preset (a favourite), plus a die of up
 * to 0.04 (the seed's: another seed, another night). A preset excluded (never again) is never taken, nor one twice.
 *
 * A step stays as the journeys' Nights do (make_journeys.py's timings()): by how much the preset moves by itself over
 * the minute -- the third of the candidates that moves least 3-6 minutes with a fade of 0:45-1:30, the third that moves
 * most 10-15 minutes (1:30-2:30), the rest 6-10 (1:00-2:00), never two short or two long ones in a row -- and the target
 * is read at the middle of that stay. When the night is full, every dwell is scaled by the same factor (near one) so
 * the night lasts what was asked at the middle of its ranges. The journey does not cycle: at its end the morning's
 * preset stays.
 *
 * Pure and deterministic: no JUCE, no audio, the same request gives the same night.
 * @author Rene Weller
 */
#pragma once

#include "Journey.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ambient
{

/** @brief What the director is asked for. */
struct NightRequest
{
    double        m_hours = 8.0;   ///< how long the night is, 1 .. 14 hours
    int           m_choice = 0;    ///< the presets it draws from: 0 .. numNightChoices()-1 (0: every pack)
    std::uint64_t m_seed = 1;      ///< the dice; another seed, another night
    std::vector<std::string> m_prefer;    ///< presets preferred (the favourites), by name
    std::vector<std::string> m_exclude;   ///< presets never taken (never again), by name
    /// Whether a preset can be played here (the instrument: whether the library it loaded has it); empty: every
    /// preset of the table can.
    std::function<bool( const std::string & )> m_available;
    std::string m_name;   ///< the journey's name; empty: "Night 8 h"
};

/** @brief One step of a composed night, with what the director saw (the tool, the plot and the selftest read it). */
struct NightStep
{
    int    m_row = -1;              ///< the preset's row in the director's table (nightRowName)
    double m_at = 0.0;              ///< seconds into the night it begins, at the middle of the ranges
    double m_stay = 0.0;            ///< seconds it stays, at the middle of its range (its crossfade included)
    double m_targetStill = 0.0;     ///< the curve's stillness at the middle of the stay, a percentile 0 .. 1
    double m_targetBright = 0.0;    ///< the curve's brightness there
    double m_still = 0.0;           ///< the preset's stillness, a percentile of the candidates (0 the stillest)
    double m_bright = 0.0;          ///< its brightness, a percentile of the candidates (0 the darkest)
    double m_rough = 0.0;           ///< its roughness, a percentile of the candidates (0 the smoothest)
    double m_distance = 0.0;        ///< its distance in sound from the step before (0 for the first), in spreads
    int    m_class = 1;             ///< 0 a short stay, 1 a middle one, 2 a long one
    bool   m_preferred = false;     ///< one of the preferred
};

/** @brief A composed night: the journey and how it came about. */
struct NightPlan
{
    Journey                m_journey;          ///< not cyclic: once through, the last preset stays
    std::vector<NightStep> m_steps;            ///< one per step of the journey
    int                    m_candidates = 0;   ///< presets it could choose from
    double                 m_scale = 1.0;      ///< the factor every dwell was scaled by to fill the night
};

/** @brief How many choices of presets there are: every pack, then the families of packs, then each pack. @return the count */
int numNightChoices();
/**
 * @brief A choice's name, for a box.
 * @param[in] choice  0 .. numNightChoices()-1
 * @return "Every pack", "Family: Sleep", ..., "Pack: Arctic Loop", ...; "" out of range
 */
std::string nightChoiceName( int choice );

/**
 * @brief The lengths of the night's first and last phase and the middle's waves.
 * @param[in]  hours    the night's length
 * @param[out] asleep   receives the falling asleep's share of the night, 0 .. 1
 * @param[out] morning  receives the morning's share
 * @param[out] waves    receives the middle's waves
 */
void nightPhases( double hours, double &asleep, double &morning, int &waves );

/**
 * @brief The curve: where the night wants to be at a point of it (see the file comment).
 * @param[in]  x       the position in the night, 0 .. 1
 * @param[in]  hours   the night's length
 * @param[out] still   receives the stillness as a percentile, 0 the stillest
 * @param[out] bright  receives the brightness as a percentile, 0 the darkest
 */
void nightCurve( double x, double hours, double &still, double &bright );

/** @brief How many presets the director's table holds (every measured pack preset). @return the count */
int nightRowCount();
/**
 * @brief A row's preset name.
 * @param[in] row  0 .. nightRowCount()-1
 * @return the name, "" out of range
 */
const char *nightRowName( int row );
/**
 * @brief A row's pack.
 * @param[in] row  0 .. nightRowCount()-1
 * @return the pack's title, "" out of range
 */
const char *nightRowPack( int row );
/**
 * @brief The row of a preset by its name.
 * @param[in] name  the preset's name
 * @return its row, -1 when the table has none of that name
 */
int nightRowOf( const std::string &name );
/**
 * @brief A row's measures as the director reads them (z-scores over the library).
 * @param[in]  row     0 .. nightRowCount()-1
 * @param[out] still   receives its stillness (lower is stiller)
 * @param[out] bright  receives its brightness
 * @param[out] rough   receives its roughness
 * @param[out] moving  receives how much it moves by itself
 * @return false out of range
 */
bool nightMeasures( int row, float &still, float &bright, float &rough, float &moving );
/**
 * @brief How unalike two presets measure: the journeys' distance (make_journeys.py, eighteen descriptors in spreads of
 *        the library, Euclidean).
 * @param[in] a  a row
 * @param[in] b  another
 * @return the distance; 0 when either is out of range
 */
float nightDistance( int a, int b );

/**
 * @brief Composes a night.
 * @param[in] request  what is asked
 * @return the night; its journey is empty when no preset could be chosen
 */
NightPlan composeNight( const NightRequest &request );

}   // namespace ambient
