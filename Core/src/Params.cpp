/**
 * @file Params.cpp
 * @brief The parameter table: every knob of the instrument, described once.
 *
 * Params.h names the parameters (ParamId) and says what a description is (ParamDesc); this file
 * holds the descriptions. kTable is the one place a parameter's stable key, display name, section,
 * kind, range, default, skew and unit are written down, in ParamId order (the self test checks
 * that), built from four small constexpr factories -- F for a float, I for an integer, B for a
 * switch, C for a choice -- so that a row reads as one line. Everything that treats parameters
 * generically reads it: the host's automation list, the preset reader (applyPreset), the map's
 * blend, the score's ramps, the modulation matrix, the editor's panels. A parameter's position in
 * the table is its identity in a host's saved state, so rows are only ever appended at the end,
 * and the section comments inside the table say when and why each run of rows arrived.
 *
 * Around the table sit the things the table needs and the questions asked of it: the name lists
 * of the choice parameters that have no home elsewhere (kScaleNames, kRootNames, the stack ratios
 * and so on -- the source, filter, clock and modulation names come from their own headers), the
 * slot-to-field map that says which ParamId is field N of source slot S (kSlotIds), the
 * section-name-to-enum table behind sectionOf(), the per-parameter section cache behind
 * sectionTable(), and findParam(), the sorted index that turns a preset's key into a description
 * in nine string comparisons instead of a hundred and forty-six.
 */
#include "ambient/Params.h"
#include "ambient/Filter.h"
#include "ambient/Clock.h"
#include "ambient/Body.h"
#include "ambient/ClusterBrain.h"
#include "ambient/Sources.h"   // choice names of the source slots
#include "ambient/ZPlane.h"    // choice names of the z-plane filter
#include "ambient/Modulation.h"  // choice names of the LFOs and envelopes
#include <cstring>
#include <algorithm>
#include <iterator>
#include <vector>

namespace ambient {

const char* const kScaleNames[kNumScaleChoices] = {
    "12-TET",
    "JI Major (Ptolemy)",
    "JI Minor",
    "JI 7-limit",
    "Pythagorean",
    "JI Pentatonic",
    "Harmonic 8-16",
    "Subharmonic 16-8",
    "Slendro (JI)",
    "Bohlen-Pierce (JI)",
    "Otonality 1-11",
    "User (Scala)",
    "Timbre (Sethares)",
};

const char* const kRootNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
/**
 * Where a new root may come from. Any is the draw the conductor always made; Fifths keeps it to
 * the fourth and the fifth; Diatonic weights the steps the way this music moves (a fifth or a
 * fourth down before anything else); Falling allows only downward steps, the semitone included,
 * which is the dark profiles' walk.
 */
const char* const kRootStepNames[4] = { "Any", "Fifths", "Diatonic", "Falling" };
const char* const kKeyMapNames[2] = { "Snap to 12 keys", "Consecutive degrees" };
const char* const kShimmerPitchNames[kNumShimmerPitches] = { "+12", "+7", "+5", "+19", "-12", "+24" };
const char* const kSubOctaveNames[2] = { "-1", "-2" };
const char* const kSubSourceNames[3] = { "Root", "Difference", "Lowest" };
/**
 * A slot's own amplitude contour: none, one of the six shapes the preset carries for modulation
 * (read with that envelope's own Mode and Time, so Env 3 used here is the same Env 3 the matrix
 * uses), or the slot's own shape. Own is last because it came last: a choice travels as its index
 * in a host's state, and a slot that named Env 3 must go on naming Env 3.
 */
const char* const kSlotEnvNames[kNumSlotEnvs] = { "Off", "Env 1", "Env 2", "Env 3", "Env 4", "Env 5", "Env 6", "Own" };
/** Between two samples of a clip: a straight line, or a curve through four points (Catmull-Rom). */
const char* const kInterpNames[kNumInterp] = { "Linear", "Hermite" };
const char* const kRoomSourceNames[2] = { "Far", "Near" };
const char* const kAirModeNames[2] = { "Band", "Ghost" };
const char* const kEnsModeNames[3] = { "Chorus", "Microshift", "Velvet" };
const char* const kFarModeNames[4] = { "Classic", "Scattering", "Colourless", "Rotating" };
const char* const kCloudResModeNames[2] = { "Band", "Comb" };
const char* const kCloudResNoteNames[4] = { "Scale", "Chord", "Fifths", "Octaves" };
const char* const kMemLineNames[3] = { "2", "4", "8" };
const char* const kShimmerModeNames[2] = { "Spectral", "Grain" };
const char* const kCloudShiftNames[7] = { "Off", "+12", "+7", "+5", "-5", "-7", "-12" };
const float kCloudShiftSemitones[7] = { 0.0f, 12.0f, 7.0f, 5.0f, -5.0f, -7.0f, -12.0f };
const char* const kKeysFilterNames[2] = { "Classic", "One Euro" };
const char* const kTransposeNames[7] = { "None", "Fourth up", "Fifth up", "Octave up", "Fourth down", "Fifth down", "Octave down" };
const char* const kBinauralNames[2] = { "Off", "Headphones" };
const char* const kBrainModeNames[kNumBrainModes] = { "Free", "Chords" };
const char* const kStrikeTypeNames[3] = { "String", "Wood", "Metal" };
const char* const kStrikeWhoNames[2] = { "Keys", "Keys + Brain" };
const char* const kNearKindNames[3] = { "Note", "Phrase", "Sequence" };
const char* const kNearPitchNames[5] = { "Consonant", "Highest", "Lowest", "Root", "Cluster" };
const char* const kStackNames[kNumStacks] = { "Detune", "Octaves", "Fifths", "Major", "Minor", "Seventh", "Harmonics", "Subharmonics" };
/** Strand ratios, ordered so that fewer strands still make sense (2 = root + fifth, 3 = a triad...). */
const double kStackRatios[kNumStacks][6] = {
    { 1.0, 1.0,       1.0,       1.0,       1.0,       1.0       },   // Detune (classic unison)
    { 1.0, 2.0,       0.5,       4.0,       0.25,      1.0       },   // Octaves
    { 1.0, 1.5,       2.0,       3.0,       0.5,       2.25      },   // Fifths: 1 3/2 2 3 1/2 9/4
    { 1.0, 1.5,       1.25,      2.0,       2.5,       0.5       },   // Major: 1 3/2 5/4 2 5/2 1/2
    { 1.0, 1.5,       1.2,       2.0,       2.4,       0.5       },   // Minor: 1 3/2 6/5 2 12/5 1/2
    { 1.0, 1.5,       1.25,      1.75,      2.0,       0.5       },   // Seventh: 1 3/2 5/4 7/4 2 1/2
    { 1.0, 2.0,       3.0,       4.0,       5.0,       6.0       },   // Harmonics
    { 1.0, 0.5,       1.0 / 3.0, 0.25,      0.2,       1.0 / 6.0 },   // Subharmonics
};
const float kShimmerPitchSemitones[kNumShimmerPitches] = { 12.0f, 7.0f, 5.0f, 19.0f, -12.0f, 24.0f };

namespace {
using K = ParamKind;   ///< short for the kind column, so that a table row stays on one line
/**
 * @brief A float parameter's row of the table.
 * @param id    the parameter
 * @param key   the stable identifier presets and automation use (never renamed)
 * @param name  the display name on the panel
 * @param sec   the section name the editor groups it under (sectionOf() turns it into a ParamSection)
 * @param mn    the lowest value, in the parameter's unit
 * @param mx    the highest value
 * @param def   the value a new instance and an unmentioning preset get
 * @param skew  1 for a linear knob, below 1 for more resolution at the low end (the domain the
 *              morph, the map blend and the score ramp in)
 * @param unit  the unit shown after the value ("Hz", "s", "dB", "ct", "" for a plain 0 .. 1)
 * @return      the description, with no choices
 */
constexpr ParamDesc F(ParamId id, const char* key, const char* name, const char* sec,
                      float mn, float mx, float def, float skew, const char* unit)
{ return { id, key, name, sec, K::Float, mn, mx, def, skew, unit, nullptr, 0 }; }
/**
 * @brief An integer parameter's row: linear, stepped, rounded by whoever reads it.
 * @param id    the parameter
 * @param key   the stable identifier presets and automation use
 * @param name  the display name
 * @param sec   the section name
 * @param mn    the lowest value
 * @param mx    the highest value
 * @param def   the default
 * @param unit  the unit shown after the value; most integers have none
 * @return      the description, skew 1 and no choices
 */
constexpr ParamDesc I(ParamId id, const char* key, const char* name, const char* sec,
                      float mn, float mx, float def, const char* unit = "")
{ return { id, key, name, sec, K::Int, mn, mx, def, 1.0f, unit, nullptr, 0 }; }
/**
 * @brief A switch's row: 0 or 1, written "off" / "on" in a preset.
 * @param id    the parameter
 * @param key   the stable identifier
 * @param name  the display name
 * @param sec   the section name
 * @param def   whether the switch is on by default
 * @return      the description, range 0 .. 1
 */
constexpr ParamDesc B(ParamId id, const char* key, const char* name, const char* sec, bool def)
{ return { id, key, name, sec, K::Bool, 0.0f, 1.0f, def ? 1.0f : 0.0f, 1.0f, "", nullptr, 0 }; }
/**
 * @brief A choice's row: an index into a list of names, which a preset may write by name.
 * @param id       the parameter
 * @param key      the stable identifier
 * @param name     the display name
 * @param sec      the section name
 * @param choices  the names, one per index; the pointer is kept, so the list must be static
 * @param n        how many names there are; the range becomes 0 .. n - 1
 * @param def      the default index
 * @return         the description, with the choices attached
 */
constexpr ParamDesc C(ParamId id, const char* key, const char* name, const char* sec,
                      const char* const* choices, int n, int def)
{ return { id, key, name, sec, K::Choice, 0.0f, static_cast<float>(n - 1), static_cast<float>(def), 1.0f, "", choices, n }; }

/**
 * @brief Every parameter's description, in ParamId order -- the table paramTable() hands out.
 *
 * The rows are grouped by section as the panels show them, but the order is the enum's, and the
 * enum only ever grows at the end: the runs of rows after the Clock section are the parameters
 * added since the first release, each run under a comment saying what it brought. The self test
 * checks that row i describes ParamId i.
 */
const std::array<ParamDesc, kNumParams> kTable = {{
    F(ParamId::MasterGain,  "master_gain",  "Master",        "Master",     -40.f, 12.f,   -6.f,  1.f,  "dB"),

    C(ParamId::Src1Type,    "src1_type",    "Type",          "Source 1", kSourceTypeNames, kNumSourceTypes, 5),
    F(ParamId::OscLevel,    "osc_level",    "Level",         "Source 1", 0.f,   1.f,    1.f,   1.f,  ""),
    I(ParamId::Partials,    "partials",     "Partials",      "Source 1"  , 1.f,   32.f,   16.f),
    F(ParamId::Tilt,        "tilt",         "Spectral Tilt", "Source 1"  , 0.3f,  3.f,    1.2f,  1.f,  ""),
    F(ParamId::Brightness,  "brightness",   "Brightness",    "Source 1"  , 0.f,   1.f,    0.7f,  1.f,  ""),
    F(ParamId::OddEven,     "odd_even",     "Odd / Even",    "Source 1"  , -1.f,  1.f,    0.f,   1.f,  ""),
    F(ParamId::Inharmonic,  "inharmonic",   "Inharmonic",    "Source 1"  , 0.f,   1.f,    0.f,   1.f,  ""),
    F(ParamId::Shimmer,     "shimmer",      "Shimmer",       "Source 1"  , 0.f,   1.f,    0.4f,  1.f,  ""),
    F(ParamId::ShimmerRate, "shimmer_rate", "Shimmer Rate",  "Source 1"  , 0.01f, 2.f,    0.15f, 0.4f, "Hz"),
    I(ParamId::Unison,      "strands",      "Strands",       "Strands"   , 1.f,   6.f,    3.f),
    F(ParamId::Detune,      "detune",       "Detune",        "Strands"   , 0.f,   50.f,   8.f,   0.6f, "ct"),
    F(ParamId::Drift,       "drift",        "Pitch Drift",   "Strands"   , 0.f,   30.f,   4.f,   0.6f, "ct"),
    F(ParamId::DriftRate,   "drift_rate",   "Drift Rate",    "Strands"   , 0.01f, 1.f,    0.08f, 0.4f, "Hz"),
    F(ParamId::Spread,      "spread",       "Stereo Spread", "Strands"   , 0.f,   1.f,    0.7f,  1.f,  ""),
    F(ParamId::Bloom,       "bloom",        "Bloom",         "Strands"   , 0.f,   1.f,    0.f,   1.f,  ""),
    F(ParamId::BloomTime,   "bloom_time",   "Bloom Time",    "Strands"   , 5.f,   300.f,  60.f,  0.4f, "s"),
    C(ParamId::Stack,       "stack",        "Stack",         "Strands"   , kStackNames, kNumStacks, 0),
    F(ParamId::RateWander,  "rate_wander",  "Rate Wander",   "Strands"   , 0.f,   1.f,    0.3f,  1.f,  ""),

    I(ParamId::Src1Octave,   "src1_octave",   "Octave",    "Source 1", -2.f,  2.f,    0.f),
    C(ParamId::Src1Ratio,    "src1_ratio",    "Ratio",     "Source 1", kSlotRatioNames, kNumSlotRatios, 0),
    F(ParamId::Src1Pan,      "src1_pan",      "Pan",       "Source 1", -1.f,  1.f,    0.f,    1.f,  ""),
    C(ParamId::Src1Table,    "src1_table",    "Table",     "Source 1", kTableNames, kNumTables, 0),
    F(ParamId::Src1Position, "src1_pos",      "Position",  "Source 1", 0.f,   1.f,    0.3f,   1.f,  ""),
    F(ParamId::Src1PosDrift, "src1_pos_drift","Pos Drift", "Source 1", 0.f,   1.f,    0.3f,   1.f,  ""),
    F(ParamId::Src1FmRatio,  "src1_fm_ratio", "FM Ratio",  "Source 1", 0.25f, 8.f,    2.f,    0.5f, "x"),
    F(ParamId::Src1FmIndex,  "src1_fm_index", "FM Index",  "Source 1", 0.f,   8.f,    1.f,    0.6f, ""),
    F(ParamId::Src1Grain,    "src1_grain",    "Grain",     "Source 1", 30.f,  1000.f, 200.f,  0.5f, "ms"),
    F(ParamId::Src1Density,  "src1_density",  "Density",   "Source 1", 1.f,   200.f,  12.f,   0.4f, "/s"),
    C(ParamId::Src1DensitySync, "src1_density_sync", "Sync", "Source 1", kSyncDivNames, kNumSyncDivs, 0),
    C(ParamId::Src1Follow,   "src1_follow",   "Pitch",     "Source 1", kFollowNames, 2, 0),
    I(ParamId::Src1Grains,   "src1_grains",   "Grains",    "Source 1", 1.f,   128.f,  16.f),
    F(ParamId::Src1Spread,   "src1_spread",   "Spread",    "Source 1", 0.f,   1.f,    0.03f,  0.5f, ""),
    C(ParamId::Src1Noise,    "src1_noise",    "Noise",     "Source 1", kNoiseKindNames, kNumNoiseKinds, 1),
    F(ParamId::Src1NoiseQ,   "src1_noise_q",  "Noise Q",   "Source 1", 0.f,   1.f,    0.4f,   1.f,  ""),
    F(ParamId::Src1Drift,    "src1_drift",    "Drift",     "Source 1", 0.f,   30.f,   0.f,    0.6f, "ct"),

    C(ParamId::Src2Type,     "src2_type",     "Type",      "Source 2", kSourceTypeNames, kNumSourceTypes, 0),
    F(ParamId::Src2Level,    "src2_level",    "Level",     "Source 2", 0.f,   1.f,    0.5f,   1.f,  ""),
    I(ParamId::Src2Octave,   "src2_octave",   "Octave",    "Source 2", -2.f,  2.f,    0.f),
    C(ParamId::Src2Ratio,    "src2_ratio",    "Ratio",     "Source 2", kSlotRatioNames, kNumSlotRatios, 0),
    F(ParamId::Src2Pan,      "src2_pan",      "Pan",       "Source 2", -1.f,  1.f,    -0.3f,  1.f,  ""),
    C(ParamId::Src2Table,    "src2_table",    "Table",     "Source 2", kTableNames, kNumTables, 0),
    F(ParamId::Src2Position, "src2_pos",      "Position",  "Source 2", 0.f,   1.f,    0.3f,   1.f,  ""),
    F(ParamId::Src2PosDrift, "src2_pos_drift","Pos Drift", "Source 2", 0.f,   1.f,    0.3f,   1.f,  ""),
    F(ParamId::Src2FmRatio,  "src2_fm_ratio", "FM Ratio",  "Source 2", 0.25f, 8.f,    2.f,    0.5f, "x"),
    F(ParamId::Src2FmIndex,  "src2_fm_index", "FM Index",  "Source 2", 0.f,   8.f,    1.f,    0.6f, ""),
    F(ParamId::Src2Grain,    "src2_grain",    "Grain",     "Source 2", 30.f,  1000.f, 200.f,  0.5f, "ms"),
    F(ParamId::Src2Density,  "src2_density",  "Density",   "Source 2", 1.f,   200.f,  12.f,   0.4f, "/s"),
    C(ParamId::Src2DensitySync, "src2_density_sync", "Sync", "Source 2", kSyncDivNames, kNumSyncDivs, 0),
    C(ParamId::Src2Follow,   "src2_follow",   "Pitch",     "Source 2", kFollowNames, 2, 0),
    I(ParamId::Src2Grains,   "src2_grains",   "Grains",    "Source 2", 1.f,   128.f,  16.f),
    F(ParamId::Src2Spread,   "src2_spread",   "Spread",    "Source 2", 0.f,   1.f,    0.03f,  0.5f, ""),
    C(ParamId::Src2Noise,    "src2_noise",    "Noise",     "Source 2", kNoiseKindNames, kNumNoiseKinds, 1),
    F(ParamId::Src2NoiseQ,   "src2_noise_q",  "Noise Q",   "Source 2", 0.f,   1.f,    0.4f,   1.f,  ""),
    I(ParamId::Src2Partials,    "src2_partials",     "Partials",     "Source 2", 1.f,   32.f,   16.f),
    F(ParamId::Src2Tilt,        "src2_tilt",         "Tilt",         "Source 2", 0.3f,  3.f,    1.2f,   1.f,  ""),
    F(ParamId::Src2Bright,      "src2_bright",       "Bright",       "Source 2", 0.f,   1.f,    0.7f,   1.f,  ""),
    F(ParamId::Src2OddEven,     "src2_odd_even",     "Odd / Even",   "Source 2", -1.f,  1.f,    0.f,    1.f,  ""),
    F(ParamId::Src2Inharm,      "src2_inharmonic",   "Inharmonic",   "Source 2", 0.f,   1.f,    0.f,    1.f,  ""),
    F(ParamId::Src2Shimmer,     "src2_shimmer",      "Shimmer",      "Source 2", 0.f,   1.f,    0.4f,   1.f,  ""),
    F(ParamId::Src2ShimmerRate, "src2_shimmer_rate", "Shimmer Rate", "Source 2", 0.01f, 2.f,    0.15f,  0.4f, "Hz"),
    F(ParamId::Src2Drift,       "src2_drift",        "Drift",        "Source 2", 0.f,   30.f,   0.f,    0.6f, "ct"),

    C(ParamId::Src3Type,     "src3_type",     "Type",      "Source 3", kSourceTypeNames, kNumSourceTypes, 0),
    F(ParamId::Src3Level,    "src3_level",    "Level",     "Source 3", 0.f,   1.f,    0.5f,   1.f,  ""),
    I(ParamId::Src3Octave,   "src3_octave",   "Octave",    "Source 3", -2.f,  2.f,    0.f),
    C(ParamId::Src3Ratio,    "src3_ratio",    "Ratio",     "Source 3", kSlotRatioNames, kNumSlotRatios, 0),
    F(ParamId::Src3Pan,      "src3_pan",      "Pan",       "Source 3", -1.f,  1.f,    0.3f,   1.f,  ""),
    C(ParamId::Src3Table,    "src3_table",    "Table",     "Source 3", kTableNames, kNumTables, 0),
    F(ParamId::Src3Position, "src3_pos",      "Position",  "Source 3", 0.f,   1.f,    0.3f,   1.f,  ""),
    F(ParamId::Src3PosDrift, "src3_pos_drift","Pos Drift", "Source 3", 0.f,   1.f,    0.3f,   1.f,  ""),
    F(ParamId::Src3FmRatio,  "src3_fm_ratio", "FM Ratio",  "Source 3", 0.25f, 8.f,    2.f,    0.5f, "x"),
    F(ParamId::Src3FmIndex,  "src3_fm_index", "FM Index",  "Source 3", 0.f,   8.f,    1.f,    0.6f, ""),
    F(ParamId::Src3Grain,    "src3_grain",    "Grain",     "Source 3", 30.f,  1000.f, 200.f,  0.5f, "ms"),
    F(ParamId::Src3Density,  "src3_density",  "Density",   "Source 3", 1.f,   200.f,  12.f,   0.4f, "/s"),
    C(ParamId::Src3DensitySync, "src3_density_sync", "Sync", "Source 3", kSyncDivNames, kNumSyncDivs, 0),
    C(ParamId::Src3Follow,   "src3_follow",   "Pitch",     "Source 3", kFollowNames, 2, 0),
    I(ParamId::Src3Grains,   "src3_grains",   "Grains",    "Source 3", 1.f,   128.f,  16.f),
    F(ParamId::Src3Spread,   "src3_spread",   "Spread",    "Source 3", 0.f,   1.f,    0.03f,  0.5f, ""),
    C(ParamId::Src3Noise,    "src3_noise",    "Noise",     "Source 3", kNoiseKindNames, kNumNoiseKinds, 1),
    F(ParamId::Src3NoiseQ,   "src3_noise_q",  "Noise Q",   "Source 3", 0.f,   1.f,    0.4f,   1.f,  ""),
    I(ParamId::Src3Partials,    "src3_partials",     "Partials",     "Source 3", 1.f,   32.f,   16.f),
    F(ParamId::Src3Tilt,        "src3_tilt",         "Tilt",         "Source 3", 0.3f,  3.f,    1.2f,   1.f,  ""),
    F(ParamId::Src3Bright,      "src3_bright",       "Bright",       "Source 3", 0.f,   1.f,    0.7f,   1.f,  ""),
    F(ParamId::Src3OddEven,     "src3_odd_even",     "Odd / Even",   "Source 3", -1.f,  1.f,    0.f,    1.f,  ""),
    F(ParamId::Src3Inharm,      "src3_inharmonic",   "Inharmonic",   "Source 3", 0.f,   1.f,    0.f,    1.f,  ""),
    F(ParamId::Src3Shimmer,     "src3_shimmer",      "Shimmer",      "Source 3", 0.f,   1.f,    0.4f,   1.f,  ""),
    F(ParamId::Src3ShimmerRate, "src3_shimmer_rate", "Shimmer Rate", "Source 3", 0.01f, 2.f,    0.15f,  0.4f, "Hz"),
    F(ParamId::Src3Drift,       "src3_drift",        "Drift",        "Source 3", 0.f,   30.f,   0.f,    0.6f, "ct"),

    F(ParamId::SubLevel,    "sub_level",    "Level",         "Foundation", 0.f,   1.f,    0.f,   1.f,  ""),
    C(ParamId::SubOctave,   "sub_octave",   "Octave",        "Foundation", kSubOctaveNames, 2, 1),   // -2 by default (25.09.2026): 33-62 Hz under a C3-B3 root, where the guide wants a sub
    F(ParamId::SubGlide,    "sub_glide",    "Glide",         "Foundation", 0.1f,  30.f,   8.f,   0.4f, "s"),
    F(ParamId::SubBinaural, "sub_binaural", "Binaural",      "Foundation", 0.f,   12.f,   0.f,   0.6f, "Hz"),
    F(ParamId::SubTone,     "sub_tone",     "Tone",          "Foundation", 0.f,   1.f,    0.2f,  1.f,  ""),
    C(ParamId::SubSource,   "sub_source",   "Source",        "Foundation", kSubSourceNames, 3, 0),
    F(ParamId::PadLowCut,   "pad_low_cut",  "Pad Low Cut",   "Foundation", 0.f,   300.f,  0.f,   0.6f, "Hz"),

    F(ParamId::StrikeLevel, "strike_level", "Strike",        "Strike",     0.f,   1.f,    0.f,   1.f,  ""),
    C(ParamId::StrikeType,  "strike_type",  "Type",          "Strike",     kStrikeTypeNames, 3, 0),
    F(ParamId::StrikeDecay, "strike_decay", "Decay",         "Strike",     0.02f, 3.f,    0.4f,  0.5f, "s"),
    F(ParamId::StrikeDamp,  "strike_damp",  "Damp",          "Strike",     0.f,   1.f,    0.5f,  1.f,  ""),
    C(ParamId::StrikeWho,   "strike_who",   "Fires",         "Strike",     kStrikeWhoNames, 2, 0),

    F(ParamId::Air,         "air",          "Air",           "Air",        0.f,   1.f,    0.15f, 1.f,  ""),
    F(ParamId::AirColor,    "air_color",    "Color",         "Air",        1.f,   16.f,   3.f,   0.5f, "x f0"),
    F(ParamId::AirQ,        "air_q",        "Q",             "Air",        1.f,   40.f,   10.f,  0.5f, ""),

    F(ParamId::Attack,      "attack",       "Attack",        "Envelope",   0.01f, 60.f,   6.f,   0.3f, "s"),
    F(ParamId::Decay,       "decay",        "Decay",         "Envelope",   0.01f, 60.f,   4.f,   0.3f, "s"),
    F(ParamId::Sustain,     "sustain",      "Sustain",       "Envelope",   0.f,   1.f,    0.8f,  1.f,  ""),
    F(ParamId::Release,     "release",      "Release",       "Envelope",   0.05f, 120.f,  12.f,  0.3f, "s"),

    B(ParamId::FilterOn,    "filter_on",    "On",            "Filter",     true),
    C(ParamId::FilterModel, "filter_model", "Model",         "Filter",     kFilterModelNames, kNumFilterModels, 1),
    F(ParamId::Cutoff,      "cutoff",       "Cutoff",        "Filter",     40.f,  18000.f, 2500.f, 0.25f, "Hz"),
    F(ParamId::Resonance,   "resonance",    "Resonance",     "Filter",     0.f,   1.f,    0.15f, 1.f,  ""),
    F(ParamId::FilterEnv,   "filter_env",   "Env Amount",    "Filter",     -1.f,  1.f,    0.3f,  1.f,  ""),
    F(ParamId::FilterDrift, "filter_drift", "Drift",         "Filter",     0.f,   1.f,    0.3f,  1.f,  ""),
    F(ParamId::KeyTrack,    "keytrack",     "Key Track",     "Filter",     0.f,   1.f,    0.5f,  1.f,  ""),
    F(ParamId::FilterDrive, "filter_drive", "Drive",         "Filter",     0.f,   1.f,    0.f,   1.f,  ""),

    C(ParamId::ZMode,       "z_mode",       "Mode",          "Z-Plane",    kZModeNames, 4, 0),
    C(ParamId::ZRoute,      "z_route",      "Route",         "Z-Plane",    kFilterRouteNames, 2, 0),
    C(ParamId::ZShape,      "z_shape",      "Shape",         "Z-Plane",    kZShapeNames, kZShapes, 0),
    F(ParamId::ZX,          "z_x",          "X",             "Z-Plane",    0.f,   1.f,    0.5f,  1.f,  ""),
    F(ParamId::ZY,          "z_y",          "Y",             "Z-Plane",    0.f,   1.f,    0.5f,  1.f,  ""),
    F(ParamId::ZZ,          "z_z",          "Transform",     "Z-Plane",    0.f,   1.f,    0.f,   1.f,  ""),
    F(ParamId::ZRate,       "z_rate",       "Rate",          "Z-Plane",    0.005f, 1.f,   0.05f, 0.4f, "Hz"),
    F(ParamId::ZDepth,      "z_depth",      "Depth",         "Z-Plane",    0.f,   1.f,    0.5f,  1.f,  ""),
    F(ParamId::ZResonance,  "z_res",        "Resonance",     "Z-Plane",    0.f,   1.f,    0.5f,  1.f,  ""),
    F(ParamId::ZKeyTrack,   "z_keytrack",   "Key Track",     "Z-Plane",    0.f,   1.f,    0.f,   1.f,  ""),
    F(ParamId::ZMix,        "z_mix",        "Mix",           "Z-Plane",    0.f,   1.f,    0.7f,  1.f,  ""),
    F(ParamId::ZDecay,      "z_decay",      "Decay",         "Z-Plane",    0.02f, 40.f,   2.5f,  0.35f, "s"),
    F(ParamId::ZDamp,       "z_damp",       "Damping",       "Z-Plane",    0.f,   1.f,    0.6f,  1.f,  ""),

    F(ParamId::Depth,       "depth",        "Depth",         "Space",      0.f,   1.f,    0.7f,  1.f,  ""),
    F(ParamId::KeysDepth,   "keys_depth",   "Keys Depth",    "Space",      0.f,   1.f,    0.f,   1.f,  ""),
    F(ParamId::PanDrift,    "pan_drift",    "Pan Drift",     "Space",      0.f,   1.f,    0.4f,  1.f,  ""),
    F(ParamId::Itd,         "itd",          "Time Width",    "Space",      0.f,   1.f,    0.6f,  1.f,  ""),
    F(ParamId::ArcAmount,   "arc",          "Arc",           "Space",      0.f,   1.f,    0.3f,  1.f,  ""),
    F(ParamId::ArcPeriod,   "arc_period",   "Arc Period",    "Space",      2.f,   240.f,  40.f,  0.4f, "min"),
    C(ParamId::ArcSync, "arc_sync", "Arc Sync", "Space", kSyncDivNames, kNumSyncDivs, 0),
    F(ParamId::Presence,    "presence",     "Presence",      "Space",      0.f,   6.f,    0.f,   1.f,  "dB"),
    F(ParamId::Breath,      "breath",       "Breath",        "Space",      0.f,   1.f,    0.f,   1.f,  ""),
    F(ParamId::BreathRate,  "breath_rate",  "Breath Rate",   "Space",      0.005f, 0.2f,  0.03f, 0.5f, "Hz"),
    F(ParamId::PhaseWidth,  "phase_width",  "Phase Width",   "Space",      0.f,   1.f,    0.f,   1.f,  ""),
    F(ParamId::PhaseRate,   "phase_rate",   "Phase Rate",    "Space",      0.005f, 0.5f,  0.03f, 0.5f, "Hz"),
    F(ParamId::Doppler,     "doppler",      "Doppler",       "Space",      0.f,   1.f,    0.f,   1.f,  ""),
    F(ParamId::Externalise, "externalise",  "Externalise",   "Space",      0.f,   1.f,    0.f,   1.f,  ""),

    F(ParamId::PressDistance, "press_distance", "Press Near",  "Expression", 0.f,  1.f,   0.f,  1.f, ""),
    F(ParamId::PressBright,   "press_bright",   "Press Bright","Expression", -1.f, 1.f,   0.f,  1.f, ""),
    F(ParamId::PressLevel,    "press_level",    "Press Level", "Expression", 0.f,  1.f,   0.f,  1.f, ""),
    F(ParamId::SlideCutoff,   "slide_cutoff",   "Slide Filter","Expression", -3.f, 3.f,   0.f,  1.f, "oct"),
    F(ParamId::SlideZ,        "slide_z",        "Slide Z",     "Expression", -1.f, 1.f,   0.f,  1.f, ""),
    F(ParamId::BendRange,     "bend_range",     "Bend Range",  "Expression", 0.f,  48.f,  2.f,  0.5f, "st"),
    B(ParamId::MpeOn,         "mpe",            "MPE",         "Expression", false),

    F(ParamId::EnsembleMix,   "ens_mix",   "Mix",   "Ensemble", 0.f,   1.f, 0.4f, 1.f,  ""),
    F(ParamId::EnsembleDepth, "ens_depth", "Depth", "Ensemble", 0.f,   1.f, 0.4f, 1.f,  ""),
    F(ParamId::EnsembleRate,  "ens_rate",  "Rate",  "Ensemble", 0.02f, 2.f, 0.2f, 0.4f, "Hz"),
    C(ParamId::EnsembleSync, "ensemble_sync", "Sync", "Ensemble", kSyncDivNames, kNumSyncDivs, 0),

    F(ParamId::DelayTimeL,    "dly_time_l",   "Time L",    "Delay", 0.02f, 4.f,   0.75f, 0.4f, "s"),
    F(ParamId::DelayTimeR,    "dly_time_r",   "Time R",    "Delay", 0.02f, 4.f,   1.1f,  0.4f, "s"),
    C(ParamId::DelaySyncL, "dly_sync_l", "Sync L", "Delay", kSyncDivNames, kNumSyncDivs, 0),
    C(ParamId::DelaySyncR, "dly_sync_r", "Sync R", "Delay", kSyncDivNames, kNumSyncDivs, 0),
    F(ParamId::DelayFeedback, "dly_feedback", "Feedback",  "Delay", 0.f,   0.95f, 0.5f,  1.f,  ""),
    F(ParamId::DelayCross,    "dly_cross",    "Cross",     "Delay", 0.f,   1.f,   0.3f,  1.f,  ""),
    F(ParamId::DelayDamp,     "dly_damp",     "Damping",   "Delay", 0.f,   1.f,   0.5f,  1.f,  ""),
    F(ParamId::DelayAbsorb,   "dly_absorb",   "Absorb",    "Delay", 0.f,   1.f,   0.f,   1.f,  ""),
    F(ParamId::DelayMix,      "dly_mix",      "Mix",       "Delay", 0.f,   1.f,   0.25f, 1.f,  ""),
    F(ParamId::DelayToFar,    "dly_to_far",   "To Far",    "Delay", 0.f,   1.f,   0.4f,  1.f,  ""),
    F(ParamId::DelayDuck,     "dly_duck",     "Duck",      "Delay", 0.f,   1.f,   0.f,   1.f,  ""),

    F(ParamId::Delay2TimeL,    "dly2_time_l",   "Time L",    "Delay 2", 0.02f, 4.f,   1.5f,  0.4f, "s"),
    F(ParamId::Delay2TimeR,    "dly2_time_r",   "Time R",    "Delay 2", 0.02f, 4.f,   2.2f,  0.4f, "s"),
    C(ParamId::Delay2SyncL, "dly2_sync_l", "Sync L", "Delay 2", kSyncDivNames, kNumSyncDivs, 0),
    C(ParamId::Delay2SyncR, "dly2_sync_r", "Sync R", "Delay 2", kSyncDivNames, kNumSyncDivs, 0),
    F(ParamId::Delay2Feedback, "dly2_feedback", "Feedback",  "Delay 2", 0.f,   0.95f, 0.4f,  1.f,  ""),
    F(ParamId::Delay2Cross,    "dly2_cross",    "Cross",     "Delay 2", 0.f,   1.f,   0.5f,  1.f,  ""),
    F(ParamId::Delay2Damp,     "dly2_damp",     "Damping",   "Delay 2", 0.f,   1.f,   0.7f,  1.f,  ""),
    F(ParamId::Delay2Absorb,   "dly2_absorb",   "Absorb",    "Delay 2", 0.f,   1.f,   0.f,   1.f,  ""),
    F(ParamId::Delay2Mix,      "dly2_mix",      "Mix",       "Delay 2", 0.f,   1.f,   0.f,   1.f,  ""),
    F(ParamId::Delay2ToFar,    "dly2_to_far",   "To Far",    "Delay 2", 0.f,   1.f,   0.5f,  1.f,  ""),

    F(ParamId::NearMix,   "near_mix",   "Mix",     "Near Reverb", 0.f,  1.f, 0.2f, 1.f,  ""),
    F(ParamId::NearDecay, "near_decay", "Decay",   "Near Reverb", 0.2f, 6.f, 1.2f, 0.5f, "s"),
    F(ParamId::NearDamp,  "near_damp",  "Damping", "Near Reverb", 0.f,  1.f, 0.3f, 1.f,  ""),
    F(ParamId::BlurMix,   "blur_mix",   "Blur",  "Blur", 0.f,  1.f, 0.f,  1.f,  ""),
    F(ParamId::BlurSmear, "blur_smear", "Smear", "Blur", 0.f,  1.f, 0.5f, 1.f,  ""),

    F(ParamId::FarLevel,    "far_level",    "Level",     "Far Reverb", 0.f,   1.f,     0.8f,   1.f,  ""),
    F(ParamId::FarSize,     "far_size",     "Size",      "Far Reverb", 0.5f,  3.f,     2.f,    1.f,  ""),
    F(ParamId::FarDecay,    "far_decay",    "Decay",     "Far Reverb", 1.f,   90.f,    25.f,   0.3f, "s"),
    F(ParamId::FarDamp,     "far_damp",     "Damping",   "Far Reverb", 0.f,   1.f,     0.5f,   1.f,  ""),
    F(ParamId::FarPreDelay, "far_predelay", "Pre-Delay", "Far Reverb", 0.f,   500.f,   3.f,    0.5f, "ms"),   // 3 ms by default (25.09.2026): the horizon has no gap; the near source's gap is Depth Gap
    F(ParamId::FarAsym,     "far_asym",     "Asymmetry", "Far Reverb", 0.f,   1.f,     0.5f,   1.f,  ""),
    F(ParamId::FarHighcut,  "far_highcut",  "Tail Cut",  "Far Reverb", 500.f, 16000.f, 3500.f, 0.3f, "Hz"),
    B(ParamId::FarFreeze,   "far_freeze",   "Freeze",    "Far Reverb", false),
    F(ParamId::FarRotate,   "far_rotate",   "Rotate",    "Far Reverb", 0.f,  1.f, 0.f,  1.f,  ""),
    F(ParamId::FarUnmask,   "far_unmask",   "Unmask",    "Far Reverb", 0.f,  1.f, 0.f,  1.f,  ""),
    F(ParamId::FarDiffuse,  "far_diffuse",  "Diffuse",   "Far Reverb", 0.f,  1.f, 0.f,  1.f,  ""),

    F(ParamId::FeedbackBus,   "fb_bus",   "To Bus",   "Feedback", 0.f,   1.f,     0.f,    1.f,  ""),
    F(ParamId::FeedbackFm,    "fb_fm",    "To Pitch", "Feedback", 0.f,   1.f,     0.f,    1.f,  ""),
    F(ParamId::FeedbackTone,  "fb_tone",  "Tone",     "Feedback", 200.f, 8000.f,  1500.f, 0.4f, "Hz"),
    F(ParamId::FeedbackDrive, "fb_drive", "Drive",    "Feedback", 0.f,   1.f,     0.5f,   1.f,  ""),

    F(ParamId::RoomLevel,    "room_level",    "Level",     "Room", 0.f,   1.f,     0.f,    1.f,  ""),
    C(ParamId::RoomSource,   "room_source",   "Source",    "Room", kRoomSourceNames, 2, 0),
    F(ParamId::RoomPreDelay, "room_predelay", "Pre-Delay", "Room", 0.f,   300.f,   20.f,   0.5f, "ms"),
    F(ParamId::RoomHighcut,  "room_highcut",  "Tail Cut",  "Room", 500.f, 16000.f, 5000.f, 0.3f, "Hz"),
    F(ParamId::RoomMorph,   "room_morph",  "Morph A/B", "Room", 0.f,  1.f,  0.f,  1.f,  ""),

    F(ParamId::CosmosSend,        "cosmos_send",        "Send",        "Cosmos", 0.f,    1.f,    0.f,   1.f,  ""),
    F(ParamId::CosmosShift,       "cosmos_shift",       "Shift",       "Cosmos", -300.f, 300.f,  0.f,   1.f,  "Hz"),
    F(ParamId::CosmosShiftDrift,  "cosmos_shift_drift", "Shift Drift", "Cosmos", 0.f,    1.f,    0.3f,  1.f,  ""),
    F(ParamId::CosmosRes,         "cosmos_res",         "Resonator",   "Cosmos", 0.f,    1.f,    0.f,   1.f,  ""),
    F(ParamId::CosmosResPitch,    "cosmos_res_pitch",   "Res Pitch",   "Cosmos", 0.25f,  8.f,    2.f,   0.5f, "x root"),
    F(ParamId::CosmosResFeedback, "cosmos_res_fb",      "Res Feedback","Cosmos", 0.f,    0.97f,  0.8f,  1.f,  ""),
    F(ParamId::CosmosVowel,       "cosmos_vowel",       "Vowel",       "Cosmos", 0.f,    1.f,    0.f,   1.f,  ""),
    F(ParamId::CosmosVowelRate,   "cosmos_vowel_rate",  "Vowel Rate",  "Cosmos", 0.005f, 0.5f,   0.05f, 0.4f, "Hz"),
    F(ParamId::CosmosNebula,      "cosmos_nebula",      "Nebula",      "Cosmos", 0.f,    1.f,    0.f,   1.f,  ""),
    F(ParamId::CosmosSmear,       "cosmos_smear",       "Smear",       "Cosmos", 0.f,    1.f,    0.7f,  1.f,  ""),
    F(ParamId::CosmosShimmer,     "cosmos_shimmer",     "Shimmer",     "Cosmos", 0.f,    1.f,    0.f,   1.f,  ""),
    C(ParamId::CosmosShimmerPitch,"cosmos_shimmer_pitch","Shimmer Pitch","Cosmos", kShimmerPitchNames, kNumShimmerPitches, 0),
    F(ParamId::CosmosReturn,      "cosmos_return",      "Return",      "Cosmos", 0.f,    1.f,    0.5f,  1.f,  ""),
    F(ParamId::CosmosToFar,       "cosmos_to_far",      "To Far",      "Cosmos", 0.f,    1.f,    0.5f,  1.f,  ""),

    F(ParamId::CloudSend,    "cloud_send",    "Send",    "Cloud", 0.f,   1.f,    0.f,   1.f,  ""),
    F(ParamId::CloudDensity, "cloud_density", "Density", "Cloud", 1.f,   60.f,   12.f,  0.5f, "/s"),
    C(ParamId::CloudSync, "cloud_sync", "Sync", "Cloud", kSyncDivNames, kNumSyncDivs, 0),
    F(ParamId::CloudSize,    "cloud_size",    "Grain",   "Cloud", 30.f,  800.f,  250.f, 0.5f, "ms"),
    F(ParamId::CloudPitch,   "cloud_pitch",   "Pitch",   "Cloud", 0.f,   1.f,    0.3f,  1.f,  ""),
    F(ParamId::CloudSpray,   "cloud_spray",   "Spray",   "Cloud", 0.05f, 2.f,    0.8f,  0.5f, "s"),
    // Up to 2, which is what the grain engine has always accepted (GrainCloud::set clamps there):
    // the knob stopped at 1 and half the range was unreachable. Measured before the change, with
    // send, level, density and size all at their tops, the cloud moved the output by 0.6 dB and
    // sat nine decibels under the preset -- "fully up" was inaudible, which is not what a send is
    // for. Everything written before this keeps its value, and 0.7 is still the default.
    F(ParamId::CloudLevel,   "cloud_level",   "Level",   "Cloud", 0.f,   2.f,    0.7f,  1.f,  ""),

    F(ParamId::BodyLevel,    "body_level",    "Body",     "Body",   0.f,   1.f,    0.f,   1.f,  ""),
    C(ParamId::BodyMaterial, "body_material", "Material", "Body",   kBodyMaterialNames, kNumBodyMaterials, 0),
    F(ParamId::BodyPitch,    "body_pitch",    "Pitch",    "Body",   0.25f, 4.f,    1.f,   0.5f, "x root"),
    F(ParamId::BodyDecay,    "body_decay",    "Decay",    "Body",   0.1f,  20.f,   4.f,   0.4f, "s"),
    F(ParamId::BodyTone,     "body_tone",     "Tone",     "Body",   0.f,   1.f,    0.4f,  1.f,  ""),
    F(ParamId::BodySpread,   "body_spread",   "Spread",   "Body",   0.f,   1.f,    0.6f,  1.f,  ""),

    F(ParamId::PatinaAmount, "patina",        "Patina",   "Patina", 0.f,   1.f,    0.f,   1.f,  ""),
    F(ParamId::PatinaWow,    "patina_wow",    "Wow",      "Patina", 0.f,   1.f,    0.3f,  1.f,  ""),
    F(ParamId::PatinaHiss,   "patina_hiss",   "Hiss",     "Patina", 0.f,   1.f,    0.2f,  1.f,  ""),
    F(ParamId::PatinaAge,    "patina_age",    "Age",      "Patina", 0.f,   1.f,    0.3f,  1.f,  ""),

    F(ParamId::Tilt2,        "master_tilt",   "Tilt",     "Master", -6.f,  6.f,    0.f,   1.f,  "dB"),
    F(ParamId::TiltPivot,    "tilt_pivot",    "Pivot",    "Master", 100.f, 4000.f, 700.f, 0.4f, "Hz"),

    F(ParamId::BassMono, "bass_mono", "Bass Mono", "Master", 40.f, 300.f, 150.f, 0.5f, "Hz"),
    F(ParamId::SideAir,  "side_air",  "Side Air",  "Master", 0.f,  6.f,   2.f,   1.f,  "dB"),
    F(ParamId::Width,    "width",     "Width",     "Master", 0.f,  2.f,   1.2f,  1.f,  ""),
    B(ParamId::MonoGuard,"mono_guard","Mono Safe", "Master", true),

    B(ParamId::BrainOn,         "brain_on",         "Active",     "Cluster Brain", true),
    I(ParamId::BrainDensity,    "brain_density",    "Density",    "Cluster Brain", 1.f,  10.f,  5.f),
    F(ParamId::BrainRate,       "brain_rate",       "Event Rate", "Cluster Brain", 2.f,  300.f, 25.f, 0.4f, "s"),
    C(ParamId::BrainSync, "brain_sync", "Sync", "Cluster Brain", kSyncDivNames, kNumSyncDivs, 0),
    F(ParamId::BrainHoldMin,    "brain_hold_min",   "Hold Min",   "Cluster Brain", 5.f,  600.f, 30.f, 0.4f, "s"),
    F(ParamId::BrainHoldMax,    "brain_hold_max",   "Hold Max",   "Cluster Brain", 5.f,  600.f, 120.f, 0.4f, "s"),
    I(ParamId::BrainLow,        "brain_low",        "Lowest",     "Cluster Brain", 24.f, 96.f,  36.f),
    I(ParamId::BrainHigh,       "brain_high",       "Highest",    "Cluster Brain", 24.f, 108.f, 79.f),
    F(ParamId::BrainConsonance, "brain_consonance", "Consonance", "Cluster Brain", 0.f,  1.f,   0.7f, 1.f, ""),
    F(ParamId::BrainWander,     "brain_wander",     "Wander",     "Cluster Brain", 0.f,  1.f,   0.3f, 1.f, ""),
    C(ParamId::BrainQuantize, "brain_quantize", "Quantize", "Cluster Brain", kSyncDivNames, kNumSyncDivs, 0),

    C(ParamId::AutoMode,      "auto_mode",     "Autoplay",  "Autoplay", kBrainModeNames, kNumBrainModes, 0),
    F(ParamId::AutoRate,      "auto_rate",     "Every",     "Autoplay", 2.f,   900.f, 45.f, 0.4f, "s"),
    C(ParamId::AutoSync,      "auto_sync",     "Sync",      "Autoplay", kSyncDivNames, kNumSyncDivs, 0),
    F(ParamId::AutoLead,      "auto_lead",     "Voice Lead","Autoplay", 1.f,   24.f,  7.f,  0.6f, "st"),
    F(ParamId::AutoTension,   "auto_tension",  "Tension",   "Autoplay", 0.f,   1.f,   0.f,  1.f,  ""),
    F(ParamId::AutoRootMove,  "auto_root_move","Root Move", "Autoplay", 0.f,   1.f,   0.2f, 1.f,  ""),
    B(ParamId::AutoStep,      "auto_step",     "Step",      "Autoplay", false),

    B(ParamId::Brain2On,        "brain2_on",       "Active",    "Brain 2", false),
    I(ParamId::Brain2Density,   "brain2_density",  "Density",   "Brain 2", 1.f,   12.f,  3.f),
    F(ParamId::Brain2Rate,      "brain2_rate",     "Event Rate","Brain 2", 2.f,   600.f, 60.f, 0.4f, "s"),
    F(ParamId::Brain2HoldMin,   "brain2_hold_min", "Hold Min",  "Brain 2", 2.f,   600.f, 60.f, 0.4f, "s"),
    F(ParamId::Brain2HoldMax,   "brain2_hold_max", "Hold Max",  "Brain 2", 5.f,   1200.f,240.f,0.4f, "s"),
    I(ParamId::Brain2Low,       "brain2_low",      "Lowest",    "Brain 2", 12.f,  84.f,  28.f),
    I(ParamId::Brain2High,      "brain2_high",     "Highest",   "Brain 2", 24.f,  108.f, 58.f),
    F(ParamId::Brain2Depth,     "brain2_depth",    "Plane",     "Brain 2", 0.f,   1.f,   0.9f, 1.f,  ""),
    I(ParamId::Brain2Interval,  "brain2_interval", "Interval",  "Brain 2", -24.f, 24.f,  0.f,  "st"),
    F(ParamId::Brain2Consonance,"brain2_consonance","Consonance","Brain 2",0.f,   1.f,   0.7f, 1.f,  ""),

    C(ParamId::Scale,     "scale",     "Scale",     "Tuning", kScaleNames, kNumScaleChoices, 3),
    C(ParamId::KeyMap,    "keymap",    "Keys",      "Tuning", kKeyMapNames, 2, 0),
    C(ParamId::RootNote,  "root",      "Root",      "Tuning", kRootNames, 12, 2),
    F(ParamId::RefPitch,  "ref_pitch", "A4",        "Tuning", 415.f, 466.f, 440.f, 1.f, "Hz"),
    I(ParamId::Seed,      "seed",      "Seed",      "Tuning", 0.f, 9999.f, 1.f),
    B(ParamId::Hold,      "hold",      "Hold",      "Tuning", false),
    F(ParamId::TunePurity,    "purity",       "Purity",       "Tuning",     0.f,    1.f,   1.f,   1.f,  ""),
    F(ParamId::TuneDrift,     "purity_drift", "Purity Drift", "Tuning",     0.f,    1.f,   0.f,   1.f,  ""),
    F(ParamId::TuneDriftRate, "purity_rate",  "Drift Rate",   "Tuning",     0.002f, 0.1f,  0.01f, 0.5f, "Hz"),
    B(ParamId::Freeze,        "freeze",       "Freeze",       "Strands"   , false),
    F(ParamId::Tide,          "tide",         "Tide",         "Tuning",     0.f,   30.f,  0.f,   0.6f, "ct"),
    F(ParamId::TidePeriod,    "tide_period",  "Tide Period",  "Tuning",     1.f,   60.f,  12.f,  0.5f, "min"),
    C(ParamId::AirMode,       "air_mode",     "Mode",         "Air",        kAirModeNames, 2, 0),
    F(ParamId::Portamento,    "portamento",   "Portamento",   "Tuning",     0.f,   20.f,  0.f,   0.4f, "s"),
    F(ParamId::PortaGravity,  "porta_gravity","Gravity",      "Tuning",     0.f,   1.f,   0.5f,  1.f,  ""),
    F(ParamId::FeedbackTape,  "fb_tape",      "Tape",         "Feedback",   0.f,   1.f,   0.f,   1.f,  ""),
    F(ParamId::Coherence,     "coherence",    "Coherence",    "Coherence",  0.f,   1.f,   0.f,   1.f,  ""),
    F(ParamId::CoherenceDepth,"coherence_depth","Depth",      "Coherence",  0.f,   1.f,   0.f,   1.f,  ""),
    F(ParamId::CoherenceRate, "coherence_rate","Rate",        "Coherence",  0.2f,  5.f,   1.f,   0.5f, "x"),
    F(ParamId::Sympathy,      "sympathy",     "Sympathy",     "Coherence",  0.f,   1.f,   0.f,  1.f,  ""),

    C(ParamId::Lfo1Shape, "lfo1_shape", "Shape", "LFO 1", kLfoShapeNames, kNumLfoShapes, 0),
    F(ParamId::Lfo1Rate,  "lfo1_rate",  "Rate",  "LFO 1", 0.0008f, 20.f, 0.0300f, 0.25f, "Hz"),
    F(ParamId::Lfo1Phase, "lfo1_phase", "Phase", "LFO 1", 0.f, 1.f, 0.0f, 1.f, ""),
    F(ParamId::Lfo1Depth, "lfo1_depth", "Depth", "LFO 1", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamId::Lfo1Mode,  "lfo1_mode",  "Mode",  "LFO 1", kLfoModeNames, kNumLfoModes, 0),
    I(ParamId::Lfo1Table, "lfo1_table", "Table", "LFO 1", 0.f, 63.f, 0.f),
    C(ParamId::Lfo1Sync, "lfo1_sync", "Sync", "LFO 1", kSyncDivNames, kNumSyncDivs, 0),
    C(ParamId::Lfo2Shape, "lfo2_shape", "Shape", "LFO 2", kLfoShapeNames, kNumLfoShapes, 0),
    F(ParamId::Lfo2Rate,  "lfo2_rate",  "Rate",  "LFO 2", 0.0008f, 20.f, 0.0485f, 0.25f, "Hz"),
    F(ParamId::Lfo2Phase, "lfo2_phase", "Phase", "LFO 2", 0.f, 1.f, 0.125f, 1.f, ""),
    F(ParamId::Lfo2Depth, "lfo2_depth", "Depth", "LFO 2", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamId::Lfo2Mode,  "lfo2_mode",  "Mode",  "LFO 2", kLfoModeNames, kNumLfoModes, 0),
    I(ParamId::Lfo2Table, "lfo2_table", "Table", "LFO 2", 0.f, 63.f, 0.f),
    C(ParamId::Lfo2Sync, "lfo2_sync", "Sync", "LFO 2", kSyncDivNames, kNumSyncDivs, 0),
    C(ParamId::Lfo3Shape, "lfo3_shape", "Shape", "LFO 3", kLfoShapeNames, kNumLfoShapes, 0),
    F(ParamId::Lfo3Rate,  "lfo3_rate",  "Rate",  "LFO 3", 0.0008f, 20.f, 0.0785f, 0.25f, "Hz"),
    F(ParamId::Lfo3Phase, "lfo3_phase", "Phase", "LFO 3", 0.f, 1.f, 0.25f, 1.f, ""),
    F(ParamId::Lfo3Depth, "lfo3_depth", "Depth", "LFO 3", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamId::Lfo3Mode,  "lfo3_mode",  "Mode",  "LFO 3", kLfoModeNames, kNumLfoModes, 0),
    I(ParamId::Lfo3Table, "lfo3_table", "Table", "LFO 3", 0.f, 63.f, 0.f),
    C(ParamId::Lfo3Sync, "lfo3_sync", "Sync", "LFO 3", kSyncDivNames, kNumSyncDivs, 0),
    C(ParamId::Lfo4Shape, "lfo4_shape", "Shape", "LFO 4", kLfoShapeNames, kNumLfoShapes, 0),
    F(ParamId::Lfo4Rate,  "lfo4_rate",  "Rate",  "LFO 4", 0.0008f, 20.f, 0.1271f, 0.25f, "Hz"),
    F(ParamId::Lfo4Phase, "lfo4_phase", "Phase", "LFO 4", 0.f, 1.f, 0.375f, 1.f, ""),
    F(ParamId::Lfo4Depth, "lfo4_depth", "Depth", "LFO 4", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamId::Lfo4Mode,  "lfo4_mode",  "Mode",  "LFO 4", kLfoModeNames, kNumLfoModes, 0),
    I(ParamId::Lfo4Table, "lfo4_table", "Table", "LFO 4", 0.f, 63.f, 0.f),
    C(ParamId::Lfo4Sync, "lfo4_sync", "Sync", "LFO 4", kSyncDivNames, kNumSyncDivs, 0),
    C(ParamId::Lfo5Shape, "lfo5_shape", "Shape", "LFO 5", kLfoShapeNames, kNumLfoShapes, 0),
    F(ParamId::Lfo5Rate,  "lfo5_rate",  "Rate",  "LFO 5", 0.0008f, 20.f, 0.2056f, 0.25f, "Hz"),
    F(ParamId::Lfo5Phase, "lfo5_phase", "Phase", "LFO 5", 0.f, 1.f, 0.5f, 1.f, ""),
    F(ParamId::Lfo5Depth, "lfo5_depth", "Depth", "LFO 5", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamId::Lfo5Mode,  "lfo5_mode",  "Mode",  "LFO 5", kLfoModeNames, kNumLfoModes, 0),
    I(ParamId::Lfo5Table, "lfo5_table", "Table", "LFO 5", 0.f, 63.f, 0.f),
    C(ParamId::Lfo5Sync, "lfo5_sync", "Sync", "LFO 5", kSyncDivNames, kNumSyncDivs, 0),
    C(ParamId::Lfo6Shape, "lfo6_shape", "Shape", "LFO 6", kLfoShapeNames, kNumLfoShapes, 0),
    F(ParamId::Lfo6Rate,  "lfo6_rate",  "Rate",  "LFO 6", 0.0008f, 20.f, 0.3327f, 0.25f, "Hz"),
    F(ParamId::Lfo6Phase, "lfo6_phase", "Phase", "LFO 6", 0.f, 1.f, 0.625f, 1.f, ""),
    F(ParamId::Lfo6Depth, "lfo6_depth", "Depth", "LFO 6", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamId::Lfo6Mode,  "lfo6_mode",  "Mode",  "LFO 6", kLfoModeNames, kNumLfoModes, 0),
    I(ParamId::Lfo6Table, "lfo6_table", "Table", "LFO 6", 0.f, 63.f, 0.f),
    C(ParamId::Lfo6Sync, "lfo6_sync", "Sync", "LFO 6", kSyncDivNames, kNumSyncDivs, 0),
    C(ParamId::Lfo7Shape, "lfo7_shape", "Shape", "LFO 7", kLfoShapeNames, kNumLfoShapes, 0),
    F(ParamId::Lfo7Rate,  "lfo7_rate",  "Rate",  "LFO 7", 0.0008f, 20.f, 0.5383f, 0.25f, "Hz"),
    F(ParamId::Lfo7Phase, "lfo7_phase", "Phase", "LFO 7", 0.f, 1.f, 0.75f, 1.f, ""),
    F(ParamId::Lfo7Depth, "lfo7_depth", "Depth", "LFO 7", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamId::Lfo7Mode,  "lfo7_mode",  "Mode",  "LFO 7", kLfoModeNames, kNumLfoModes, 0),
    I(ParamId::Lfo7Table, "lfo7_table", "Table", "LFO 7", 0.f, 63.f, 0.f),
    C(ParamId::Lfo7Sync, "lfo7_sync", "Sync", "LFO 7", kSyncDivNames, kNumSyncDivs, 0),
    C(ParamId::Lfo8Shape, "lfo8_shape", "Shape", "LFO 8", kLfoShapeNames, kNumLfoShapes, 0),
    F(ParamId::Lfo8Rate,  "lfo8_rate",  "Rate",  "LFO 8", 0.0008f, 20.f, 0.8710f, 0.25f, "Hz"),
    F(ParamId::Lfo8Phase, "lfo8_phase", "Phase", "LFO 8", 0.f, 1.f, 0.875f, 1.f, ""),
    F(ParamId::Lfo8Depth, "lfo8_depth", "Depth", "LFO 8", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamId::Lfo8Mode,  "lfo8_mode",  "Mode",  "LFO 8", kLfoModeNames, kNumLfoModes, 0),
    I(ParamId::Lfo8Table, "lfo8_table", "Table", "LFO 8", 0.f, 63.f, 0.f),
    C(ParamId::Lfo8Sync, "lfo8_sync", "Sync", "LFO 8", kSyncDivNames, kNumSyncDivs, 0),
    C(ParamId::Env1Mode,  "env1_mode",  "Mode",  "Env 1", kEnvModeNames, kNumEnvModes, 0),
    F(ParamId::Env1Time,  "env1_time",  "Time",  "Env 1", 0.05f, 20.f, 1.f, 0.4f, "x"),
    F(ParamId::Env1Depth, "env1_depth", "Depth", "Env 1", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamId::Env1Sync, "env1_sync", "Sync", "Env 1", kSyncDivNames, kNumSyncDivs, 0),
    C(ParamId::Env2Mode,  "env2_mode",  "Mode",  "Env 2", kEnvModeNames, kNumEnvModes, 0),
    F(ParamId::Env2Time,  "env2_time",  "Time",  "Env 2", 0.05f, 20.f, 1.f, 0.4f, "x"),
    F(ParamId::Env2Depth, "env2_depth", "Depth", "Env 2", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamId::Env2Sync, "env2_sync", "Sync", "Env 2", kSyncDivNames, kNumSyncDivs, 0),
    C(ParamId::Env3Mode,  "env3_mode",  "Mode",  "Env 3", kEnvModeNames, kNumEnvModes, 0),
    F(ParamId::Env3Time,  "env3_time",  "Time",  "Env 3", 0.05f, 20.f, 1.f, 0.4f, "x"),
    F(ParamId::Env3Depth, "env3_depth", "Depth", "Env 3", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamId::Env3Sync, "env3_sync", "Sync", "Env 3", kSyncDivNames, kNumSyncDivs, 0),
    C(ParamId::Env4Mode,  "env4_mode",  "Mode",  "Env 4", kEnvModeNames, kNumEnvModes, 0),
    F(ParamId::Env4Time,  "env4_time",  "Time",  "Env 4", 0.05f, 20.f, 1.f, 0.4f, "x"),
    F(ParamId::Env4Depth, "env4_depth", "Depth", "Env 4", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamId::Env4Sync, "env4_sync", "Sync", "Env 4", kSyncDivNames, kNumSyncDivs, 0),
    C(ParamId::Env5Mode,  "env5_mode",  "Mode",  "Env 5", kEnvModeNames, kNumEnvModes, 0),
    F(ParamId::Env5Time,  "env5_time",  "Time",  "Env 5", 0.05f, 20.f, 1.f, 0.4f, "x"),
    F(ParamId::Env5Depth, "env5_depth", "Depth", "Env 5", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamId::Env5Sync, "env5_sync", "Sync", "Env 5", kSyncDivNames, kNumSyncDivs, 0),
    C(ParamId::Env6Mode,  "env6_mode",  "Mode",  "Env 6", kEnvModeNames, kNumEnvModes, 0),
    F(ParamId::Env6Time,  "env6_time",  "Time",  "Env 6", 0.05f, 20.f, 1.f, 0.4f, "x"),
    F(ParamId::Env6Depth, "env6_depth", "Depth", "Env 6", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamId::Env6Sync, "env6_sync", "Sync", "Env 6", kSyncDivNames, kNumSyncDivs, 0),

    B(ParamId::MorphActive, "morph_active", "Active",   "Morph", false),
    F(ParamId::MorphPos,    "morph",        "Position", "Morph", 0.f, 1.f,   0.f,  1.f,  ""),
    F(ParamId::MorphGlide,  "morph_glide",  "Glide",    "Morph", 0.f, 900.f, 30.f, 0.4f, "s"),

    F(ParamId::MacroA, "macro_a", "Space",  "Macros", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::MacroB, "macro_b", "Alien",  "Macros", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::MacroC, "macro_c", "Motion", "Macros", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::MacroD, "macro_d", "Bloom",  "Macros", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::MacroE, "macro_e", "Density",   "Macros", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::MacroF, "macro_f", "Distance",  "Macros", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::MacroG, "macro_g", "Evolution", "Macros", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::MacroH, "macro_h", "Air",       "Macros", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::Inertia, "inertia", "Inertia",  "Macros", 0.f, 5.f, 0.f, 0.5f, "s"),

    B(ParamId::MapActive, "map_active", "Active", "Map", false),
    F(ParamId::MapX,      "map_x",      "X",      "Map", 0.f,   1.f,  0.5f,  1.f, ""),
    F(ParamId::MapY,      "map_y",      "Y",      "Map", 0.f,   1.f,  0.5f,  1.f, ""),
    F(ParamId::MapRadius, "map_radius", "Radius", "Map", 0.02f, 0.4f, 0.08f, 1.f, ""),

    B(ParamId::RouteActive, "route_active", "Play",  "Route", false),
    F(ParamId::RouteSpeed,  "route_speed",  "Speed", "Route", 0.25f, 4.f, 1.f, 0.5f, "x"),
    B(ParamId::RouteLoop,   "route_loop",   "Loop",  "Route", true),

    C(ParamId::ClockSource, "clock_source", "Source", "Clock", kClockSourceNames, kNumClockSources, 0),
    F(ParamId::Tempo,       "tempo",        "Tempo",  "Clock", 20.f, 300.f, 90.f, 0.6f, "bpm"),
    B(ParamId::ClockRun,    "clock_run",    "Run",    "Clock", true),

    // ---- appended later; see the note in Params.h on why they are at the end
    F(ParamId::FarLowcut,   "far_lowcut",   "Low Cut", "Far Reverb",  20.f, 800.f, 20.f, 0.35f, "Hz"),
    F(ParamId::NearLowcut,  "near_lowcut",  "Low Cut", "Near Reverb", 20.f, 800.f, 20.f, 0.35f, "Hz"),
    F(ParamId::RoomLowcut,  "room_lowcut",  "Low Cut", "Room",        20.f, 800.f, 20.f, 0.35f, "Hz"),
    F(ParamId::Subsonic,    "subsonic",     "Subsonic", "Master",      0.f,  40.f,  0.f, 1.f,   "Hz"),
    F(ParamId::VecAmount,   "vec_amount",   "Amount",  "Vector", 0.f, 1.f, 0.f,   1.f, ""),
    F(ParamId::VecX,        "vec_x",        "X",       "Vector", 0.f, 1.f, 0.5f,  1.f, ""),
    F(ParamId::VecY,        "vec_y",        "Y",       "Vector", 0.f, 1.f, 0.5f,  1.f, ""),
    F(ParamId::VecWander,   "vec_wander",   "Wander",  "Vector", 0.f, 1.f, 0.f,   1.f, ""),
    F(ParamId::VecRate,     "vec_rate",     "Rate",    "Vector", 0.002f, 0.5f, 0.02f, 0.35f, "Hz"),
    // ---- Source 4, the same slot as 2 and 3 field for field (the self test checks that)
    C(ParamId::Src4Type,     "src4_type",     "Type",      "Source 4", kSourceTypeNames, kNumSourceTypes, 0),
    F(ParamId::Src4Level,    "src4_level",    "Level",     "Source 4", 0.f,   1.f,    0.5f,   1.f,  ""),
    I(ParamId::Src4Octave,   "src4_octave",   "Octave",    "Source 4", -2.f,  2.f,    0.f),
    C(ParamId::Src4Ratio,    "src4_ratio",    "Ratio",     "Source 4", kSlotRatioNames, kNumSlotRatios, 0),
    F(ParamId::Src4Pan,      "src4_pan",      "Pan",       "Source 4", -1.f,  1.f,    0.3f,   1.f,  ""),
    C(ParamId::Src4Table,    "src4_table",    "Table",     "Source 4", kTableNames, kNumTables, 0),
    F(ParamId::Src4Position, "src4_pos",      "Position",  "Source 4", 0.f,   1.f,    0.3f,   1.f,  ""),
    F(ParamId::Src4PosDrift, "src4_pos_drift","Pos Drift", "Source 4", 0.f,   1.f,    0.3f,   1.f,  ""),
    F(ParamId::Src4FmRatio,  "src4_fm_ratio", "FM Ratio",  "Source 4", 0.25f, 8.f,    2.f,    0.5f, "x"),
    F(ParamId::Src4FmIndex,  "src4_fm_index", "FM Index",  "Source 4", 0.f,   8.f,    1.f,    0.6f, ""),
    F(ParamId::Src4Grain,    "src4_grain",    "Grain",     "Source 4", 30.f,  1000.f, 200.f,  0.5f, "ms"),
    F(ParamId::Src4Density,  "src4_density",  "Density",   "Source 4", 1.f,   200.f,  12.f,   0.4f, "/s"),
    C(ParamId::Src4DensitySync, "src4_density_sync", "Sync", "Source 4", kSyncDivNames, kNumSyncDivs, 0),
    C(ParamId::Src4Follow,   "src4_follow",   "Pitch",     "Source 4", kFollowNames, 2, 0),
    I(ParamId::Src4Grains,   "src4_grains",   "Grains",    "Source 4", 1.f,   128.f,  16.f),
    F(ParamId::Src4Spread,   "src4_spread",   "Spread",    "Source 4", 0.f,   1.f,    0.03f,  0.5f, ""),
    C(ParamId::Src4Noise,    "src4_noise",    "Noise",     "Source 4", kNoiseKindNames, kNumNoiseKinds, 1),
    F(ParamId::Src4NoiseQ,   "src4_noise_q",  "Noise Q",   "Source 4", 0.f,   1.f,    0.4f,   1.f,  ""),
    I(ParamId::Src4Partials,    "src4_partials",     "Partials",     "Source 4", 1.f,   32.f,   16.f),
    F(ParamId::Src4Tilt,        "src4_tilt",         "Tilt",         "Source 4", 0.3f,  3.f,    1.2f,   1.f,  ""),
    F(ParamId::Src4Bright,      "src4_bright",       "Bright",       "Source 4", 0.f,   1.f,    0.7f,   1.f,  ""),
    F(ParamId::Src4OddEven,     "src4_odd_even",     "Odd / Even",   "Source 4", -1.f,  1.f,    0.f,    1.f,  ""),
    F(ParamId::Src4Inharm,      "src4_inharmonic",   "Inharmonic",   "Source 4", 0.f,   1.f,    0.f,    1.f,  ""),
    F(ParamId::Src4Shimmer,     "src4_shimmer",      "Shimmer",      "Source 4", 0.f,   1.f,    0.4f,   1.f,  ""),
    F(ParamId::Src4ShimmerRate, "src4_shimmer_rate", "Shimmer Rate", "Source 4", 0.01f, 2.f,    0.15f,  0.4f, "Hz"),
    F(ParamId::Src4Drift,       "src4_drift",        "Drift",        "Source 4", 0.f,   30.f,   0.f,    0.6f, "ct"),
    // ---- Stretch: factor and loop crossfade, one pair per slot
    F(ParamId::Src1Stretch, "src1_stretch", "Stretch",   "Source 1", 1.f, 1000.f, 40.f, 0.25f, "x"),
    F(ParamId::Src1Xfade,   "src1_xfade",   "Loop Fade", "Source 1", 0.f, 1.f,    0.1f, 1.f,   ""),
    F(ParamId::Src2Stretch, "src2_stretch", "Stretch",   "Source 2", 1.f, 1000.f, 40.f, 0.25f, "x"),
    F(ParamId::Src2Xfade,   "src2_xfade",   "Loop Fade", "Source 2", 0.f, 1.f,    0.1f, 1.f,   ""),
    F(ParamId::Src3Stretch, "src3_stretch", "Stretch",   "Source 3", 1.f, 1000.f, 40.f, 0.25f, "x"),
    F(ParamId::Src3Xfade,   "src3_xfade",   "Loop Fade", "Source 3", 0.f, 1.f,    0.1f, 1.f,   ""),
    F(ParamId::Src4Stretch, "src4_stretch", "Stretch",   "Source 4", 1.f, 1000.f, 40.f, 0.25f, "x"),
    F(ParamId::Src4Xfade,   "src4_xfade",   "Loop Fade", "Source 4", 0.f, 1.f,    0.1f, 1.f,   ""),
    // ---- the mixing desk's four, all neutral at their defaults
    F(ParamId::FarWidth,   "far_width",   "Width",     "Far Reverb", 0.f, 1.5f,  1.f,  1.f,  ""),
    C(ParamId::EnsMode,    "ens_mode",    "Mode",      "Ensemble",   kEnsModeNames, 3, 0),
    F(ParamId::Haas,       "haas",        "Haas",      "Space",      0.f, 1.f,   0.f,  1.f,  ""),
    F(ParamId::HaasTime,   "haas_time",   "Haas Time", "Space",      6.f, 28.f,  15.f, 1.f,  "ms"),
    F(ParamId::FilterFold, "filter_fold", "Fold",      "Filter",     0.f, 1.f,   0.f,  1.f,  ""),
    // ---- after the classics: all neutral at their defaults
    F(ParamId::TuneStretch,     "stretch",           "Stretch",  "Tuning",     0.f, 30.f, 0.f, 1.f, "ct"),
    C(ParamId::FarMode,         "far_mode",          "Mode",     "Far Reverb", kFarModeNames, 4, 0),
    F(ParamId::FarUnmaskSpread, "far_unmask_spread", "Spread",   "Far Reverb", 0.f, 1.f,  0.f, 1.f, ""),
    C(ParamId::Binaural,        "binaural",          "Binaural", "Space",      kBinauralNames, 2, 0),
    F(ParamId::BrainTimbre,     "brain_timbre",      "Timbre",   "Cluster Brain", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::BrainSpacing,    "brain_spacing",     "Spacing",  "Cluster Brain", -1.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::EarlyLevel,      "early_level",       "Early",    "Early Room", 0.f,  1.f,  0.f,  1.f, ""),
    F(ParamId::EarlySize,       "early_size",        "Size",     "Early Room", 2.f,  40.f, 8.f,  0.5f, "m"),
    F(ParamId::EarlyAbsorb,     "early_absorb",      "Absorb",   "Early Room", 0.f,  1.f,  0.35f, 1.f, ""),
    F(ParamId::EarlyWidth,      "early_width",       "Width",    "Early Room", 0.f,  1.5f, 1.f,  1.f, ""),
    // ---- Bow: the pair per slot
    F(ParamId::Src1BowForce, "src1_bow_force", "Force", "Source 1", 0.f, 1.f, 0.4f, 1.f, ""),
    F(ParamId::Src1BowSpeed, "src1_bow_speed", "Speed", "Source 1", 0.f, 1.f, 0.3f, 1.f, ""),
    F(ParamId::Src2BowForce, "src2_bow_force", "Force", "Source 2", 0.f, 1.f, 0.4f, 1.f, ""),
    F(ParamId::Src2BowSpeed, "src2_bow_speed", "Speed", "Source 2", 0.f, 1.f, 0.3f, 1.f, ""),
    F(ParamId::Src3BowForce, "src3_bow_force", "Force", "Source 3", 0.f, 1.f, 0.4f, 1.f, ""),
    F(ParamId::Src3BowSpeed, "src3_bow_speed", "Speed", "Source 3", 0.f, 1.f, 0.3f, 1.f, ""),
    F(ParamId::Src4BowForce, "src4_bow_force", "Force", "Source 4", 0.f, 1.f, 0.4f, 1.f, ""),
    F(ParamId::Src4BowSpeed, "src4_bow_speed", "Speed", "Source 4", 0.f, 1.f, 0.3f, 1.f, ""),
    F(ParamId::Src1SpecRate,   "src1_spec_rate",   "Rate",   "Source 1", 0.f, 4.f, 1.f, 0.6f, "x"),
    F(ParamId::Src1SpecBreath, "src1_spec_breath", "Breath", "Source 1", -1.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::Src2SpecRate,   "src2_spec_rate",   "Rate",   "Source 2", 0.f, 4.f, 1.f, 0.6f, "x"),
    F(ParamId::Src2SpecBreath, "src2_spec_breath", "Breath", "Source 2", -1.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::Src3SpecRate,   "src3_spec_rate",   "Rate",   "Source 3", 0.f, 4.f, 1.f, 0.6f, "x"),
    F(ParamId::Src3SpecBreath, "src3_spec_breath", "Breath", "Source 3", -1.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::Src4SpecRate,   "src4_spec_rate",   "Rate",   "Source 4", 0.f, 4.f, 1.f, 0.6f, "x"),
    F(ParamId::Src4SpecBreath, "src4_spec_breath", "Breath", "Source 4", -1.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::BrainHarmonic,  "brain_harmonic",  "Harmonic", "Cluster Brain", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::BrainKey,       "brain_key",       "Key",      "Cluster Brain", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::BrainEven,      "brain_even",      "Even",     "Cluster Brain", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::BrainSmooth,    "brain_smooth",    "Smooth",   "Cluster Brain", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::TuneGuard,      "purity_guard",    "Guard",    "Tuning",        -1.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::TuneMatch,      "match",           "Match",    "Tuning",        0.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::BrainBlend,     "brain_blend",     "Blend",    "Cluster Brain", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::ArcHarmony,     "arc_harmony",     "Arc Harmony", "Space",      0.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::ElevNear,       "elev_near",       "Elev Near",   "Space",      -1.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::ElevFar,        "elev_far",        "Elev Far",    "Space",      -1.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::DepthLaw,       "depth_law",       "Depth Law",   "Space",      0.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::Envelop,        "far_envelop",     "Envelop",     "Far Reverb", 0.f, 1.f, 0.f, 1.f, ""),
    B(ParamId::ArcClock,       "arc_clock",       "Arc Clock",   "Space",      false),
    F(ParamId::BrainCascade,   "brain_cascade",   "Cascade",     "Cluster Brain", 0.f, 1.f, 0.f,  1.f, ""),
    F(ParamId::BrainSurprise,  "brain_surprise",  "Surprise",    "Cluster Brain", 0.f, 1.f, 0.5f, 1.f, ""),
    F(ParamId::BrainHomeostat, "brain_homeostat", "Homeostat",   "Cluster Brain", 0.f, 1.f, 0.f,  1.f, ""),
    F(ParamId::TuneAdapt,      "purity_adapt",    "Adaptive",    "Tuning",        0.f, 1.f, 0.f,  1.f, ""),
    F(ParamId::FarComod,       "far_comod",       "Comodulate",  "Far Reverb",    0.f, 1.f, 0.f,  1.f, ""),
    F(ParamId::NearIld,        "near_ild",        "Near Field",  "Space",         0.f, 1.f, 0.f,  1.f, ""),
    F(ParamId::Src1Transport,  "src1_transport",  "Transport",   "Source 1",      0.f, 1.f, 0.f,  1.f, ""),
    F(ParamId::Src2Transport,  "src2_transport",  "Transport",   "Source 2",      0.f, 1.f, 0.f,  1.f, ""),
    F(ParamId::Src3Transport,  "src3_transport",  "Transport",   "Source 3",      0.f, 1.f, 0.f,  1.f, ""),
    F(ParamId::Src4Transport,  "src4_transport",  "Transport",   "Source 4",      0.f, 1.f, 0.f,  1.f, ""),
    F(ParamId::SubPulse,       "sub_pulse",       "Pulse",       "Foundation",    0.f, 1.f, 0.f,  1.f, ""),
    F(ParamId::FeedbackBias,   "fb_bias",         "Bias",        "Feedback",      0.f, 1.f, 0.f,  1.f, ""),
    F(ParamId::LeniaRate,      "lenia_rate",      "Lenia Rate",  "Coherence",     0.5f, 20.f, 4.f, 0.5f, "Hz"),
    F(ParamId::LeniaGrowth,    "lenia_growth",    "Lenia Growth","Coherence",     0.08f, 0.3f, 0.15f, 1.f, ""),
    F(ParamId::BrainDejaVu,    "brain_dejavu",    "Deja Vu",     "Cluster Brain", 0.f, 1.f, 0.f,  1.f, ""),
    F(ParamId::BrainLoop,      "brain_loop",      "Loop",        "Cluster Brain", 1.f, 16.f, 8.f, 1.f, ""),
    F(ParamId::BrainSpread,    "brain_spread",    "Spread",      "Cluster Brain", 0.f, 1.f, 0.5f, 1.f, ""),
    F(ParamId::BrainBias,      "brain_bias",      "Bias",        "Cluster Brain", -1.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::ChaosPeriod,    "chaos_period",    "Chaos Period","Coherence",     10.f, 1800.f, 120.f, 0.4f, "s"),
    C(ParamId::KeysFilter,     "keys_filter",     "MPE Filter",  "Space",         kKeysFilterNames, 2, 0),
    C(ParamId::Transpose,      "transpose",       "Transpose",   "Tuning",        kTransposeNames, 7, 0),
    F(ParamId::PartialSpread,  "partial_spread",  "Partial Spread", "Strands",    0.f, 1.f, 0.f,  1.f, ""),
    // How often the conductor's notes strike, and whether those strikes cluster with its cascade.
    // Chance 1 is what the instrument always did: every note that may strike, strikes.
    F(ParamId::StrikeChance,   "strike_chance",   "Chance",      "Strike",        0.f, 1.f, 1.f,  1.f, ""),
    F(ParamId::StrikeCluster,  "strike_cluster",  "Cluster",     "Strike",        0.f, 1.f, 0.f,  1.f, ""),
    // The Cosmos breathing with the conductor's cascade. On by default: without a cascade the
    // excitation is zero and this changes nothing at all, and where there is one it is what the
    // section wanted anyway -- a parallel world that comes and goes rather than a constant hum.
    F(ParamId::CosmosSwell,    "cosmos_swell",    "Swell",       "Cosmos",        0.f, 1.f, 0.5f, 1.f, ""),
    // Each slot's own entrance, per note. Delay 0 and Env Off is what every preset made before
    // these existed, so they are the defaults and nothing already written changes. Delay reaches
    // 30 s because the library's attacks do: a texture that arrives half a minute into a held
    // chord is a second instrument entering, not a slow fade.
    F(ParamId::Src1Delay, "src1_delay", "Delay", "Source 1", 0.f, 30.f, 0.f, 0.4f, "s"),
    F(ParamId::Src1Rise,  "src1_rise",  "Rise",  "Source 1", 0.01f, 30.f, 1.f, 0.4f, "s"),
    C(ParamId::Src1Env,   "src1_env",   "Env",   "Source 1", kSlotEnvNames, kNumSlotEnvs, 0),
    F(ParamId::Src2Delay, "src2_delay", "Delay", "Source 2", 0.f, 30.f, 0.f, 0.4f, "s"),
    F(ParamId::Src2Rise,  "src2_rise",  "Rise",  "Source 2", 0.01f, 30.f, 1.f, 0.4f, "s"),
    C(ParamId::Src2Env,   "src2_env",   "Env",   "Source 2", kSlotEnvNames, kNumSlotEnvs, 0),
    F(ParamId::Src3Delay, "src3_delay", "Delay", "Source 3", 0.f, 30.f, 0.f, 0.4f, "s"),
    F(ParamId::Src3Rise,  "src3_rise",  "Rise",  "Source 3", 0.01f, 30.f, 1.f, 0.4f, "s"),
    C(ParamId::Src3Env,   "src3_env",   "Env",   "Source 3", kSlotEnvNames, kNumSlotEnvs, 0),
    F(ParamId::Src4Delay, "src4_delay", "Delay", "Source 4", 0.f, 30.f, 0.f, 0.4f, "s"),
    F(ParamId::Src4Rise,  "src4_rise",  "Rise",  "Source 4", 0.01f, 30.f, 1.f, 0.4f, "s"),
    C(ParamId::Src4Env,   "src4_env",   "Env",   "Source 4", kSlotEnvNames, kNumSlotEnvs, 0),
    C(ParamId::Src1Interp, "src1_interp", "Interp", "Source 1", kInterpNames, kNumInterp, 0),
    C(ParamId::Src2Interp, "src2_interp", "Interp", "Source 2", kInterpNames, kNumInterp, 0),
    C(ParamId::Src3Interp, "src3_interp", "Interp", "Source 3", kInterpNames, kNumInterp, 0),
    C(ParamId::Src4Interp, "src4_interp", "Interp", "Source 4", kInterpNames, kNumInterp, 0),
    F(ParamId::Src1Root, "src1_root", "Root", "Source 1", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::Src2Root, "src2_root", "Root", "Source 2", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::Src3Root, "src3_root", "Root", "Source 3", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::Src4Root, "src4_root", "Root", "Source 4", 0.f, 1.f, 0.f, 1.f, ""),
    I(ParamId::Src1Unison,    "src1_unison",     "Unison",  "Source 1", 1.f, 4.f, 1.f),
    F(ParamId::Src1UniDetune, "src1_uni_detune", "Uni Det", "Source 1", 0.f, 40.f, 10.f, 1.f, "ct"),
    F(ParamId::Src1UniWidth,  "src1_uni_width",  "Uni Wide","Source 1", 0.f, 1.f, 0.6f, 1.f, ""),
    I(ParamId::Src2Unison,    "src2_unison",     "Unison",  "Source 2", 1.f, 4.f, 1.f),
    F(ParamId::Src2UniDetune, "src2_uni_detune", "Uni Det", "Source 2", 0.f, 40.f, 10.f, 1.f, "ct"),
    F(ParamId::Src2UniWidth,  "src2_uni_width",  "Uni Wide","Source 2", 0.f, 1.f, 0.6f, 1.f, ""),
    I(ParamId::Src3Unison,    "src3_unison",     "Unison",  "Source 3", 1.f, 4.f, 1.f),
    F(ParamId::Src3UniDetune, "src3_uni_detune", "Uni Det", "Source 3", 0.f, 40.f, 10.f, 1.f, "ct"),
    F(ParamId::Src3UniWidth,  "src3_uni_width",  "Uni Wide","Source 3", 0.f, 1.f, 0.6f, 1.f, ""),
    I(ParamId::Src4Unison,    "src4_unison",     "Unison",  "Source 4", 1.f, 4.f, 1.f),
    F(ParamId::Src4UniDetune, "src4_uni_detune", "Uni Det", "Source 4", 0.f, 40.f, 10.f, 1.f, "ct"),
    F(ParamId::Src4UniWidth,  "src4_uni_width",  "Uni Wide","Source 4", 0.f, 1.f, 0.6f, 1.f, ""),
    F(ParamId::CloudFeedback,  "cloud_feedback",  "Feedback",  "Cloud", 0.f,   1.f,     0.f,    1.f,  ""),
    F(ParamId::CloudTone,      "cloud_tone",      "Tone",      "Cloud", 300.f, 16000.f, 5000.f, 0.3f, "Hz"),
    I(ParamId::CloudTranspose, "cloud_transpose", "Transpose", "Cloud", -24.f, 24.f,    0.f,    "st"),
    F(ParamId::CloudScatter,   "cloud_scatter",   "Scatter",   "Cloud", 0.f,   1.f,     0.f,    1.f,  ""),
    F(ParamId::CloudSwarm,     "cloud_swarm",     "Swarm",     "Cloud", 0.f,   1.f,     0.f,    1.f,  ""),
    F(ParamId::CloudResonance, "cloud_resonance", "Resonance", "Cloud", 0.f,   1.f,     0.f,    1.f,  ""),
    C(ParamId::CloudResMode,   "cloud_res_mode",  "Res Mode",  "Cloud", kCloudResModeNames, 2, 0),
    C(ParamId::CloudResNotes,  "cloud_res_notes", "Res Notes", "Cloud", kCloudResNoteNames, 4, 1),
    F(ParamId::CloudResDecay,  "cloud_res_decay", "Ring",      "Cloud", 0.2f,  12.f,    2.5f,   0.4f, "s"),
    F(ParamId::MemSend,    "mem_send",    "Send",       "Memory", 0.f,  1.f,   0.f,   1.f,  ""),
    F(ParamId::MemReturn,  "mem_return",  "Return",     "Memory", 0.f,  1.f,   0.5f,  1.f,  ""),
    F(ParamId::MemToFar,   "mem_to_far",  "To Far",     "Memory", 0.f,  1.f,   0.4f,  1.f,  ""),
    C(ParamId::MemLines,   "mem_lines",   "Lines",      "Memory", kMemLineNames, 3, 1),
    F(ParamId::MemSize,    "mem_size",    "Size",       "Memory", 1.f,  40.f,  9.f,   0.4f, "s"),
    F(ParamId::MemBlur,    "mem_blur",    "Blur",       "Memory", 0.f,  1.f,   0.25f, 1.f,  ""),
    F(ParamId::MemDrift,   "mem_drift",   "Drift",      "Memory", 0.f,  1.f,   0.3f,  1.f,  ""),
    F(ParamId::MemHold,    "mem_hold",    "Hold",       "Memory", 0.f,  1.f,   0.8f,  1.f,  ""),
    F(ParamId::MemAge,     "mem_age",     "Age",        "Memory", 0.f,  1.f,   0.3f,  1.f,  ""),
    F(ParamId::MemRenew,   "mem_renew",   "Renew",      "Memory", 0.f,  1.f,   0.f,   1.f,  ""),
    F(ParamId::MemDrive,   "mem_drive",   "Drive",      "Memory", 0.f,  1.f,   0.f,   1.f,  ""),
    F(ParamId::MemRecall,  "mem_recall",  "Recall",     "Memory", 0.f,  1.f,   0.f,   1.f,  ""),
    F(ParamId::MemSeek,    "mem_seek",    "Seek",       "Memory", 0.f,  1.f,   0.5f,  1.f,  ""),
    F(ParamId::MemGrain,   "mem_grain",   "Grain",      "Memory", 40.f, 800.f, 220.f, 0.5f, "ms"),
    B(ParamId::MemFreeze,  "mem_freeze",  "Freeze",     "Memory", false),
    B(ParamId::MemReverse, "mem_reverse", "Reverse",    "Memory", false),
    B(ParamId::MemHalf,    "mem_half",    "Half Speed", "Memory", false),
    B(ParamId::MemErase,   "mem_erase",   "Erase",      "Memory", false),
    C(ParamId::CosmosShimmerMode, "cosmos_shimmer_mode", "Shimmer Mode", "Cosmos", kShimmerModeNames, 2, 0),
    C(ParamId::CloudShift,        "cloud_shift",         "FB Shift",     "Cloud",  kCloudShiftNames, 7, 0),
    // Each source's own envelope, for a slot whose Env is Own (Voice.cpp). Depth 1 is the whole
    // contour; 0 leaves the source at its written level whatever the shape says.
    C(ParamId::Src1EnvMode,  "src1_env_mode",  "Mode",  "Src Env 1", kEnvModeNames, kNumEnvModes, 0),
    F(ParamId::Src1EnvTime,  "src1_env_time",  "Time",  "Src Env 1", 0.05f, 20.f, 1.f, 0.4f, "x"),
    F(ParamId::Src1EnvDepth, "src1_env_depth", "Depth", "Src Env 1", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamId::Src1EnvSync,  "src1_env_sync",  "Sync",  "Src Env 1", kSyncDivNames, kNumSyncDivs, 0),
    C(ParamId::Src2EnvMode,  "src2_env_mode",  "Mode",  "Src Env 2", kEnvModeNames, kNumEnvModes, 0),
    F(ParamId::Src2EnvTime,  "src2_env_time",  "Time",  "Src Env 2", 0.05f, 20.f, 1.f, 0.4f, "x"),
    F(ParamId::Src2EnvDepth, "src2_env_depth", "Depth", "Src Env 2", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamId::Src2EnvSync,  "src2_env_sync",  "Sync",  "Src Env 2", kSyncDivNames, kNumSyncDivs, 0),
    C(ParamId::Src3EnvMode,  "src3_env_mode",  "Mode",  "Src Env 3", kEnvModeNames, kNumEnvModes, 0),
    F(ParamId::Src3EnvTime,  "src3_env_time",  "Time",  "Src Env 3", 0.05f, 20.f, 1.f, 0.4f, "x"),
    F(ParamId::Src3EnvDepth, "src3_env_depth", "Depth", "Src Env 3", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamId::Src3EnvSync,  "src3_env_sync",  "Sync",  "Src Env 3", kSyncDivNames, kNumSyncDivs, 0),
    C(ParamId::Src4EnvMode,  "src4_env_mode",  "Mode",  "Src Env 4", kEnvModeNames, kNumEnvModes, 0),
    F(ParamId::Src4EnvTime,  "src4_env_time",  "Time",  "Src Env 4", 0.05f, 20.f, 1.f, 0.4f, "x"),
    F(ParamId::Src4EnvDepth, "src4_env_depth", "Depth", "Src Env 4", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamId::Src4EnvSync,  "src4_env_sync",  "Sync",  "Src Env 4", kSyncDivNames, kNumSyncDivs, 0),
    // ---- what a self-playing generator needs (12.09.2026). Every default is the behaviour the
    // conductor already had, so no preset written before these changes by a single sample.
    F(ParamId::BrainLayers,      "brain_layers",       "Layers",       "Cluster Brain", 0.f,  1.f,   0.f,  1.f, ""),
    F(ParamId::BrainBassHold,    "brain_bass_hold",    "Bass Hold",    "Cluster Brain", 1.f,  8.f,   1.f,  0.5f, "x"),
    F(ParamId::BrainTopSoft,     "brain_top_soft",     "Top Soft",     "Cluster Brain", 0.f,  1.f,   0.f,  1.f, ""),
    F(ParamId::BrainLowSpacing,  "brain_low_spacing",  "Low Spacing",  "Cluster Brain", 0.f,  1.f,   0.f,  1.f, ""),
    I(ParamId::BrainThirdFloor,  "brain_third_floor",  "Third Floor",  "Cluster Brain", 0.f,  72.f,  0.f),
    F(ParamId::BrainLeading,     "brain_leading",      "Leading",      "Cluster Brain", 0.f,  1.f,   0.f,  1.f, ""),
    F(ParamId::BrainThirds,      "brain_thirds",       "Thirds",       "Cluster Brain", -1.f, 1.f,   0.f,  1.f, ""),
    F(ParamId::BrainSeconds,     "brain_seconds",      "Seconds",      "Cluster Brain", -1.f, 1.f,   0.f,  1.f, ""),
    F(ParamId::BrainSeventh,     "brain_seventh",      "Seventh",      "Cluster Brain", 0.f,  1.f,   0.f,  1.f, ""),
    F(ParamId::BrainDegreeSwap,  "brain_degree_swap",  "Degree Swap",  "Cluster Brain", 0.f,  1.f,   0.f,  1.f, ""),
    F(ParamId::BrainRateBreath,  "brain_rate_breath",  "Rate Breath",  "Cluster Brain", 0.f,  1.f,   0.f,  1.f, ""),
    F(ParamId::BrainBreathPeriod,"brain_breath_period","Breath Period","Cluster Brain", 2.f,  30.f,  10.f, 0.6f, "min"),
    F(ParamId::BrainOverlap,     "brain_overlap",      "Overlap",      "Cluster Brain", 0.f,  60.f,  0.f,  0.6f, "s"),
    B(ParamId::BrainOnsetGuard,  "brain_onset_guard",  "Onset Guard",  "Cluster Brain", false),
    F(ParamId::BrainReleaseGap,  "brain_release_gap",  "Release Gap",  "Cluster Brain", 0.f,  10.f,  0.f,  0.6f, "s"),
    F(ParamId::BrainRetrigger,   "brain_retrigger",    "Retrigger",    "Cluster Brain", 0.f,  120.f, 0.f,  0.5f, "s"),
    F(ParamId::BrainDensitySlew, "brain_density_slew", "Density Slew", "Cluster Brain", 0.f,  10.f,  0.f,  0.6f, "min"),
    F(ParamId::BrainSilence,     "brain_silence",      "Silence",      "Cluster Brain", 0.f,  1.f,   0.f,  1.f, ""),
    F(ParamId::BrainSilenceLen,  "brain_silence_len",  "Silence Len",  "Cluster Brain", 5.f,  60.f,  20.f, 0.7f, "s"),
    C(ParamId::BrainRootSteps,   "brain_root_steps",   "Root Steps",   "Cluster Brain", kRootStepNames, 4, 0),
    F(ParamId::BrainRootDown,    "brain_root_down",    "Root Down",    "Cluster Brain", -1.f, 1.f,   0.f,  1.f, ""),
    F(ParamId::BrainPivot,       "brain_pivot",        "Pivot",        "Cluster Brain", 0.f,  90.f,  0.f,  0.6f, "s"),
    F(ParamId::BrainHome,        "brain_home",         "Home",         "Cluster Brain", 0.f,  1.f,   0.f,  1.f, ""),
    F(ParamId::BrainHomeTime,    "brain_home_time",    "Home Time",    "Cluster Brain", 10.f, 180.f, 60.f, 0.6f, "min"),
    F(ParamId::BrainMemory,      "brain_memory",       "Memory",       "Cluster Brain", 0.f,  30.f,  0.f,  0.6f, "min"),
    B(ParamId::Brain2Golden,     "brain2_golden",      "Golden",       "Brain 2", false),
    B(ParamId::TuneHoldSounding, "tuning_hold_sounding","Hold Sounding","Tuning", false),
    F(ParamId::BeatCeiling,      "beat_ceiling",       "Beat Ceiling", "Tuning", 0.f, 4.f, 0.f, 0.7f, "Hz"),
    F(ParamId::EnvVelAttack,     "env_vel_attack",     "Vel to Attack","Envelope", -1.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::LayerDepth,       "layer_depth",        "Layer Depth",  "Space", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::StrandLowDetune,  "strand_low_detune",  "Low Detune",   "Source 1", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamId::CloudToNear,      "cloud_to_near",      "To Near",      "Cloud", 0.f, 1.f, 0.f, 1.f, ""),
    // The near plane (13.09.2026): a role per slot, and the Near Events section.
    C(ParamId::Src1Role,         "src1_role",          "Role",         "Source 1", kSlotRoleNames, kNumSlotRoles, 0),
    C(ParamId::Src2Role,         "src2_role",          "Role",         "Source 2", kSlotRoleNames, kNumSlotRoles, 0),
    C(ParamId::Src3Role,         "src3_role",          "Role",         "Source 3", kSlotRoleNames, kNumSlotRoles, 0),
    C(ParamId::Src4Role,         "src4_role",          "Role",         "Source 4", kSlotRoleNames, kNumSlotRoles, 0),
    // The near source: a fifth slot, the same fields a source slot has where the near types read
    // them, and its own envelope, filter and strike, so the layer owes the sound preset nothing.
    C(ParamId::ForeType,         "fore_type",          "Type",         "Near Source", kSourceTypeNames, kNumSourceTypes, 10),
    I(ParamId::ForeOctave,       "fore_octave",        "Octave",       "Near Source", -2.f,  2.f,   0.f),
    C(ParamId::ForeRatio,        "fore_ratio",         "Ratio",        "Near Source", kSlotRatioNames, kNumSlotRatios, 0),
    F(ParamId::ForePosition,     "fore_pos",           "Position",     "Near Source", 0.f,   1.f,   0.3f,  1.f,  ""),
    F(ParamId::ForePosDrift,     "fore_pos_drift",     "Pos Drift",    "Near Source", 0.f,   1.f,   0.3f,  1.f,  ""),
    F(ParamId::ForeDensity,      "fore_density",       "Density",      "Near Source", 0.05f, 200.f, 4.f,   0.4f, "/s"),
    C(ParamId::ForeFollow,       "fore_follow",        "Pitch",        "Near Source", kFollowNames, 2, 1),
    F(ParamId::ForeBright,       "fore_bright",        "Bright",       "Near Source", 0.f,   1.f,   0.6f,  1.f,  ""),
    F(ParamId::ForeForce,        "fore_force",         "Force",        "Near Source", 0.f,   1.f,   0.5f,  1.f,  ""),
    F(ParamId::ForeSpeed,        "fore_speed",         "Speed",        "Near Source", 0.f,   1.f,   0.4f,  1.f,  ""),
    C(ParamId::ForeNoise,        "fore_noise",         "Noise",        "Near Source", kNoiseKindNames, kNumNoiseKinds, 1),
    F(ParamId::ForeNoiseQ,       "fore_noise_q",       "Noise Q",      "Near Source", 0.f,   1.f,   0.4f,  1.f,  ""),
    F(ParamId::ForeFmRatio,      "fore_fm_ratio",      "FM Ratio",     "Near Source", 0.25f, 8.f,   2.f,   0.5f, ""),
    F(ParamId::ForeFmIndex,      "fore_fm_index",      "FM Index",     "Near Source", 0.f,   8.f,   1.f,   0.6f, ""),
    I(ParamId::ForePartials,     "fore_partials",      "Partials",     "Near Source", 1.f,   32.f,  12.f),
    F(ParamId::ForeTilt,         "fore_tilt",          "Tilt",         "Near Source", 0.3f,  3.f,   1.2f,  1.f,  ""),
    F(ParamId::ForeInharm,       "fore_inharmonic",    "Inharmonic",   "Near Source", 0.f,   1.f,   0.f,   1.f,  ""),
    F(ParamId::ForeDrift,        "fore_drift",         "Drift",        "Near Source", 0.f,   30.f,  0.f,   0.6f, "ct"),
    C(ParamId::ForeTable,        "fore_table",         "Table",        "Near Source", kTableNames, kNumTables, 0),
    F(ParamId::ForeAttack,       "fore_attack",        "Attack",       "Near Source", 0.001f, 10.f, 0.3f,  0.4f, "s"),   // down to a millisecond: a sonar ping is struck, not breathed
    F(ParamId::ForeDecay,        "fore_decay",         "Decay",        "Near Source", 0.01f, 30.f,  1.f,   0.4f, "s"),
    F(ParamId::ForeSustain,      "fore_sustain",       "Sustain",      "Near Source", 0.f,   1.f,   1.f,   1.f,  ""),
    F(ParamId::ForeRelease,      "fore_release",       "Release",      "Near Source", 0.03f, 30.f,  2.f,   0.4f, "s"),
    F(ParamId::ForeCutoff,       "fore_cutoff",        "Cutoff",       "Near Source", 40.f,  18000.f, 6000.f, 0.3f, "Hz"),
    F(ParamId::ForeResonance,    "fore_resonance",     "Resonance",    "Near Source", 0.f,   1.f,   0.15f, 1.f,  ""),
    C(ParamId::ForeFilterModel,  "fore_filter",        "Filter",       "Near Source", kFilterModelNames, kNumFilterModels, 1),
    F(ParamId::ForeFilterEnv,    "fore_filter_env",    "Filter Env",   "Near Source", 0.f,   1.f,   0.f,   1.f,  ""),
    F(ParamId::ForeStrike,       "fore_strike",        "Strike",       "Near Source", 0.f,   1.f,   0.f,   1.f,  ""),
    C(ParamId::ForeStrikeType,   "fore_strike_type",   "Strike Type",  "Near Source", kStrikeTypeNames, 3, 0),
    F(ParamId::ForeStrikeDecay,  "fore_strike_decay",  "Strike Decay", "Near Source", 0.02f, 3.f,   0.4f,  0.5f, "s"),
    F(ParamId::ForeStrikeDamp,   "fore_strike_damp",   "Strike Damp",  "Near Source", 0.f,   1.f,   0.5f,  1.f,  ""),
    // The events: their clock, their pitch, their place, and the sequence.
    F(ParamId::ForeLevel,        "fore_level",         "Level",        "Near Events", 0.f,   1.f,   0.f,   1.f,  ""),
    C(ParamId::ForeKind,         "fore_kind",          "Kind",         "Near Events", kNearKindNames, 3, 0),
    F(ParamId::ForeRate,         "fore_rate",          "Every",        "Near Events", 10.f,  900.f, 120.f, 0.4f, "s"),
    F(ParamId::ForeChance,       "fore_chance",        "Chance",       "Near Events", 0.f,   1.f,   1.f,   1.f,  ""),
    F(ParamId::ForeCluster,      "fore_cluster",       "Cluster",      "Near Events", 0.f,   1.f,   0.f,   1.f,  ""),
    F(ParamId::ForeLength,       "fore_length",        "Length",       "Near Events", 0.5f,  600.f, 6.f,   0.35f, "s"),
    C(ParamId::ForePitch,        "fore_pitch",         "Pitch",        "Near Events", kNearPitchNames, 5, 0),
    F(ParamId::ForeSpread,       "fore_spread",        "Spread",       "Near Events", 0.f,   1.f,   0.5f,  1.f,  ""),
    F(ParamId::ForeApproach,     "fore_approach",      "Approach",     "Near Events", -1.f,  1.f,   0.f,   1.f,  ""),   // negative: the event leaves instead of arriving
    F(ParamId::ForeProximity,    "fore_proximity",     "Proximity",    "Near Events", 0.f,   1.f,   0.6f,  1.f,  ""),
    B(ParamId::ForeHold,         "fore_hold",          "Hold Brain",   "Near Events", true),
    F(ParamId::ForeGlide,        "fore_glide",         "Glide",        "Near Events", 0.f,   30.f,  0.f,   0.5f, "s"),
    I(ParamId::ForeSteps,        "fore_steps",         "Steps",        "Near Events", 3.f,   16.f,  7.f),
    F(ParamId::ForeStep,         "fore_step",          "Step",         "Near Events", 0.1f,  4.f,   0.4f,  0.5f, "s"),
    C(ParamId::ForeStepSync,     "fore_step_sync",     "Step Sync",    "Near Events", kSyncDivNames, kNumSyncDivs, 0),
    F(ParamId::ForeMutation,     "fore_mutation",      "Mutation",     "Near Events", 0.f,   1.f,   0.12f, 1.f,  ""),
    F(ParamId::ForeScatter,      "fore_scatter",       "Scatter",      "Near Events", 0.f,   1.f,   0.3f,  1.f,  ""),
    F(ParamId::ForeBloom,        "fore_bloom",         "Bloom",        "Near Events", 0.f,   1.f,   0.5f,  1.f,  ""),
    F(ParamId::ForeDistance,     "fore_distance",      "Distance",     "Near Events", 0.f,   1.f,   0.f,   1.f,  ""),
    F(ParamId::ForeDry,          "fore_dry",           "Dry",          "Near Events", 0.f,   1.f,   0.f,   1.f,  ""),
    B(ParamId::ForeAuto,         "fore_auto",          "Auto",         "Near Events", true),
    F(ParamId::FarUnmaskReturn,  "far_unmask_return",  "Return",       "Far Reverb", 0.2f,  10.f,  1.2f,  0.5f, "s"),
    F(ParamId::ForeDelay2,       "fore_delay2",        "To Delay 2",   "Near Events", 0.f,   1.f,   0.f,   1.f,  ""),
    F(ParamId::ForeCosmos,       "fore_cosmos",        "To Cosmos",    "Near Events", 0.f,   1.f,   0.f,   1.f,  ""),
    F(ParamId::ForeGain,         "fore_gain",          "Gain",         "Near Events", -24.f, 36.f,  0.f,   1.f,  "dB"),
    // The production guide's depth model (25.09.2026): a source loses Range dB between the ear and
    // the horizon (20 puts a note at Depth 0.7 fourteen decibels under the foreground, the guide's
    // middle plane, and one on the horizon twenty under it, the near end of its far plane); its
    // far send waits Gap ms at the ear and nothing on the horizon; its strands fan out to Near
    // Width of their Spread at the ear and to all of it on the horizon.
    F(ParamId::DepthRange,       "depth_range",        "Range",        "Space",       6.f,   36.f,  20.f,  1.f,  "dB"),
    F(ParamId::DepthPreDelay,    "depth_predelay",     "Gap",          "Space",       0.f,   80.f,  40.f,  1.f,  "ms"),
    F(ParamId::DepthWidth,       "depth_width",        "Near Width",   "Space",       0.f,   1.f,   0.35f, 1.f,  ""),
    F(ParamId::SendLowcut,       "send_lowcut",        "Send Low Cut", "Far Reverb",  20.f,  400.f, 150.f, 0.5f, "Hz"),
    F(ParamId::SubHarmonics,     "sub_harmonics",      "Harmonics",    "Foundation",  0.f,   1.f,   0.6f,  1.f,  ""),
    F(ParamId::SubBeat,          "sub_beat",           "Beat",         "Foundation",  0.f,   1.f,   0.f,   1.f,  "Hz"),
}};
} // namespace

const std::array<ParamDesc, kNumParams>& paramTable() { return kTable; }

namespace {
/**
 * @brief Which ParamId is field f of source slot s, for the kSlotFields fields every slot has.
 *
 * The four rows are identical field for field except for the ids: Source 1's level and additive
 * spectrum are the classic Oscillator parameters (OscLevel, Partials, Tilt ...), the other slots
 * have their own. slotParamIds() hands a row out; the engine and the editor both index through it
 * instead of each keeping a copy, and the self test checks the rows against Sources.h.
 */
const ParamId kSlotIds[kSourceSlots][kSlotFields] = {
    // Source 1: its type and its slot fields, then the Oscillator parameters that are its level
    // and its additive spectrum, then its grain-density sync and its pitch drift.
    { ParamId::Src1Type, ParamId::OscLevel, ParamId::Src1Octave, ParamId::Src1Ratio, ParamId::Src1Pan, ParamId::Src1Table,
      ParamId::Src1Position, ParamId::Src1PosDrift, ParamId::Src1FmRatio, ParamId::Src1FmIndex, ParamId::Src1Grain, ParamId::Src1Density,
      ParamId::Src1Follow, ParamId::Src1Grains, ParamId::Src1Spread, ParamId::Src1Noise, ParamId::Src1NoiseQ,
      ParamId::Partials, ParamId::Tilt, ParamId::Brightness, ParamId::OddEven, ParamId::Inharmonic, ParamId::Shimmer, ParamId::ShimmerRate,
      ParamId::Src1DensitySync, ParamId::Src1Drift, ParamId::Src1Stretch, ParamId::Src1Xfade,
      ParamId::Src1BowForce, ParamId::Src1BowSpeed,
      ParamId::Src1SpecRate, ParamId::Src1SpecBreath, ParamId::Src1Transport,
      ParamId::Src1Delay, ParamId::Src1Rise, ParamId::Src1Env, ParamId::Src1Interp,
      ParamId::Src1Unison, ParamId::Src1UniDetune, ParamId::Src1UniWidth, ParamId::Src1Root, ParamId::Src1Role },
    { ParamId::Src2Type, ParamId::Src2Level, ParamId::Src2Octave, ParamId::Src2Ratio, ParamId::Src2Pan, ParamId::Src2Table,
      ParamId::Src2Position, ParamId::Src2PosDrift, ParamId::Src2FmRatio, ParamId::Src2FmIndex, ParamId::Src2Grain, ParamId::Src2Density,
      ParamId::Src2Follow, ParamId::Src2Grains, ParamId::Src2Spread, ParamId::Src2Noise, ParamId::Src2NoiseQ,
      ParamId::Src2Partials, ParamId::Src2Tilt, ParamId::Src2Bright, ParamId::Src2OddEven, ParamId::Src2Inharm, ParamId::Src2Shimmer, ParamId::Src2ShimmerRate,
      ParamId::Src2DensitySync, ParamId::Src2Drift, ParamId::Src2Stretch, ParamId::Src2Xfade,
      ParamId::Src2BowForce, ParamId::Src2BowSpeed,
      ParamId::Src2SpecRate, ParamId::Src2SpecBreath, ParamId::Src2Transport,
      ParamId::Src2Delay, ParamId::Src2Rise, ParamId::Src2Env, ParamId::Src2Interp,
      ParamId::Src2Unison, ParamId::Src2UniDetune, ParamId::Src2UniWidth, ParamId::Src2Root, ParamId::Src2Role },
    { ParamId::Src3Type, ParamId::Src3Level, ParamId::Src3Octave, ParamId::Src3Ratio, ParamId::Src3Pan, ParamId::Src3Table,
      ParamId::Src3Position, ParamId::Src3PosDrift, ParamId::Src3FmRatio, ParamId::Src3FmIndex, ParamId::Src3Grain, ParamId::Src3Density,
      ParamId::Src3Follow, ParamId::Src3Grains, ParamId::Src3Spread, ParamId::Src3Noise, ParamId::Src3NoiseQ,
      ParamId::Src3Partials, ParamId::Src3Tilt, ParamId::Src3Bright, ParamId::Src3OddEven, ParamId::Src3Inharm, ParamId::Src3Shimmer, ParamId::Src3ShimmerRate,
      ParamId::Src3DensitySync, ParamId::Src3Drift, ParamId::Src3Stretch, ParamId::Src3Xfade,
      ParamId::Src3BowForce, ParamId::Src3BowSpeed,
      ParamId::Src3SpecRate, ParamId::Src3SpecBreath, ParamId::Src3Transport,
      ParamId::Src3Delay, ParamId::Src3Rise, ParamId::Src3Env, ParamId::Src3Interp,
      ParamId::Src3Unison, ParamId::Src3UniDetune, ParamId::Src3UniWidth, ParamId::Src3Root, ParamId::Src3Role },
    { ParamId::Src4Type, ParamId::Src4Level, ParamId::Src4Octave, ParamId::Src4Ratio, ParamId::Src4Pan, ParamId::Src4Table,
      ParamId::Src4Position, ParamId::Src4PosDrift, ParamId::Src4FmRatio, ParamId::Src4FmIndex, ParamId::Src4Grain, ParamId::Src4Density,
      ParamId::Src4Follow, ParamId::Src4Grains, ParamId::Src4Spread, ParamId::Src4Noise, ParamId::Src4NoiseQ,
      ParamId::Src4Partials, ParamId::Src4Tilt, ParamId::Src4Bright, ParamId::Src4OddEven, ParamId::Src4Inharm, ParamId::Src4Shimmer, ParamId::Src4ShimmerRate,
      ParamId::Src4DensitySync, ParamId::Src4Drift, ParamId::Src4Stretch, ParamId::Src4Xfade,
      ParamId::Src4BowForce, ParamId::Src4BowSpeed,
      ParamId::Src4SpecRate, ParamId::Src4SpecBreath, ParamId::Src4Transport,
      ParamId::Src4Delay, ParamId::Src4Rise, ParamId::Src4Env, ParamId::Src4Interp,
      ParamId::Src4Unison, ParamId::Src4UniDetune, ParamId::Src4UniWidth, ParamId::Src4Root, ParamId::Src4Role },
};

/** @brief One section as the table spells it, and the enum value sectionOf() answers for it. */
struct SectionName {
    const char* name;       ///< the section string of the table rows, e.g. "Far Reverb"
    ParamSection section;   ///< the ParamSection it stands for
};
/**
 * @brief The section names of the table and their ParamSection, for sectionOf(const char*).
 *
 * The numbered sections -- "LFO 3", "Env 5", "Src Env 2" -- are not listed; sectionOf() matches
 * them by prefix. "Early Room" maps onto Room, since the early reflections belong to that panel.
 * A name the table uses that is missing here comes out as Unknown, which the self test catches.
 */
const SectionName kSections[] = {
    { "Master", ParamSection::Master }, { "Source 1", ParamSection::Source1 }, { "Strands", ParamSection::Strands },
    { "Vector", ParamSection::Vector },
    { "Source 2", ParamSection::Source2 }, { "Source 3", ParamSection::Source3 }, { "Source 4", ParamSection::Source4 },
    { "Strike", ParamSection::Strike },
    { "Foundation", ParamSection::Foundation }, { "Air", ParamSection::Air }, { "Envelope", ParamSection::Envelope },
    { "Filter", ParamSection::Filter }, { "Z-Plane", ParamSection::ZPlane }, { "Expression", ParamSection::Expression },
    { "Space", ParamSection::Space }, { "Ensemble", ParamSection::Ensemble }, { "Delay", ParamSection::Delay },
    { "Delay 2", ParamSection::Delay2 }, { "Near Reverb", ParamSection::NearReverb }, { "Far Reverb", ParamSection::FarReverb },
    { "Blur", ParamSection::Blur }, { "Feedback", ParamSection::Feedback }, { "Room", ParamSection::Room }, { "Early Room", ParamSection::Room },
    { "Body", ParamSection::Body }, { "Patina", ParamSection::Patina }, { "Cosmos", ParamSection::Cosmos },
    { "Cloud", ParamSection::Cloud }, { "Cluster Brain", ParamSection::ClusterBrain }, { "Brain 2", ParamSection::Brain2 },
    { "Autoplay", ParamSection::Autoplay },
    { "Tuning", ParamSection::Tuning }, { "Coherence", ParamSection::Coherence }, { "Clock", ParamSection::Clock },
    { "Morph", ParamSection::Morph }, { "Macros", ParamSection::Macros }, { "Map", ParamSection::Map },
    { "Route", ParamSection::Route }, { "Memory", ParamSection::Memory },
    { "Near Source", ParamSection::NearSource }, { "Near Events", ParamSection::NearEvents },
};
} // namespace

const ParamId* slotParamIds(int slot)
{
    return (slot >= 0 && slot < kSourceSlots) ? kSlotIds[slot] : nullptr;
}

ParamSection sectionOf(const char* name)
{
    if (name == nullptr) return ParamSection::Unknown;
    // The eight LFOs and six envelopes each have their own numbered section ("LFO 3", "Env 5"), and
    // so do the four sources' own envelopes ("Src Env 2"), which are envelopes like the six.
    if (std::strncmp(name, "LFO ", 4) == 0) return ParamSection::Lfo;
    if (std::strncmp(name, "Env ", 4) == 0) return ParamSection::ModEnvelope;
    if (std::strncmp(name, "Src Env ", 8) == 0) return ParamSection::ModEnvelope;
    for (const SectionName& s : kSections) if (std::strcmp(s.name, name) == 0) return s.section;
    return ParamSection::Unknown;
}

const ParamSection* sectionTable()
{
    static const std::vector<ParamSection> t = [] {
        std::vector<ParamSection> v;
        v.reserve(std::size(kTable));
        for (const auto& d : kTable) v.push_back(sectionOf(d.section));
        return v;
    }();
    return t.data();
}

const ParamDesc* findParam(const char* key)
{
    // Binary search over a sorted index, built once. It used to be a linear strcmp over the whole
    // table, and everything that reads a preset goes through here: warming the map up for the
    // library meant eight thousand presets times a hundred and thirty settings times a hundred
    // and forty-six string comparisons -- a hundred and sixty million of them, 1.75 seconds, paid
    // in full every time an engine was prepared. Nine comparisons instead of a hundred and forty-six.
    if (key == nullptr) return nullptr;
    static const std::vector<const ParamDesc*> sorted = [] {
        std::vector<const ParamDesc*> v;
        v.reserve(std::size(kTable));
        for (const auto& d : kTable) v.push_back(&d);
        std::sort(v.begin(), v.end(), [](const ParamDesc* a, const ParamDesc* b) { return std::strcmp(a->key, b->key) < 0; });
        return v;
    }();
    auto it = std::lower_bound(sorted.begin(), sorted.end(), key,
                               [](const ParamDesc* d, const char* k) { return std::strcmp(d->key, k) < 0; });
    return (it != sorted.end() && std::strcmp((*it)->key, key) == 0) ? *it : nullptr;
}

} // namespace ambient
