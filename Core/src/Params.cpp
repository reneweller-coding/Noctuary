/**
 * @file Params.cpp
 * @brief The parameter table: every knob of the instrument, described once.
 *
 * Params.h names the parameters (ParamId) and says what a description is (ParamDesc); this file
 * holds the descriptions. Table is the one place a parameter's stable key, display name, section,
 * kind, range, default, skew and unit are written down, in ParamId order (the self test checks
 * that), built from four small constexpr factories -- F for a float, I for an integer, B for a
 * switch, C for a choice -- so that a row reads as one line. Everything that treats parameters
 * generically reads it: the host's automation list, the preset reader (applyPreset), the map's
 * blend, the score's ramps, the modulation matrix, the editor's panels. A parameter's position in
 * the table is its identity in a host's saved state, so rows are only ever appended at the end,
 * and the section comments inside the table say when and why each run of rows arrived.
 *
 * Around the table sit the things the table needs and the questions asked of it: the name lists
 * of the choice parameters that have no home elsewhere (ScaleNames, RootNames, the stack ratios
 * and so on -- the source, filter, clock and modulation names come from their own headers), the
 * slot-to-field map that says which ParamId is field N of source slot S (SlotIds), the
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

const char* const ScaleNames[NumScaleChoices] = {
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

const char* const RootNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
/**
 * Where a new root may come from. Any is the draw the conductor always made; Fifths keeps it to
 * the fourth and the fifth; Diatonic weights the steps the way this music moves (a fifth or a
 * fourth down before anything else); Falling allows only downward steps, the semitone included,
 * which is the dark profiles' walk.
 */
const char* const RootStepNames[4] = { "Any", "Fifths", "Diatonic", "Falling" };
const char* const RootTargetNames[4] = { "Classic", "Modal", "Mediant", "Phrygian" };
const char* const KeyMapNames[2] = { "Snap to 12 keys", "Consecutive degrees" };
const char* const ShimmerPitchNames[NumShimmerPitches] = { "+12", "+7", "+5", "+19", "-12", "+24" };
const char* const SubOctaveNames[2] = { "-1", "-2" };
const char* const SubSourceNames[3] = { "Root", "Difference", "Lowest" };
/**
 * A slot's own amplitude contour: none, one of the six shapes the preset carries for modulation
 * (read with that envelope's own Mode and Time, so Env 3 used here is the same Env 3 the matrix
 * uses), or the slot's own shape. Own is last because it came last: a choice travels as its index
 * in a host's state, and a slot that named Env 3 must go on naming Env 3.
 */
const char* const SlotEnvNames[NumSlotEnvs] = { "Off", "Env 1", "Env 2", "Env 3", "Env 4", "Env 5", "Env 6", "Own" };
/** Between two samples of a clip: a straight line, or a curve through four points (Catmull-Rom). */
const char* const InterpNames[NumInterp] = { "Linear", "Hermite" };
const char* const RoomSourceNames[2] = { "Far", "Near" };
const char* const AirModeNames[2] = { "Band", "Ghost" };
const char* const EnsModeNames[3] = { "Chorus", "Microshift", "Velvet" };
const char* const FarModeNames[4] = { "Classic", "Scattering", "Colourless", "Rotating" };
const char* const CloudResModeNames[2] = { "Band", "Comb" };
const char* const CloudResNoteNames[4] = { "Scale", "Chord", "Fifths", "Octaves" };
const char* const MemLineNames[3] = { "2", "4", "8" };
const char* const ShimmerModeNames[2] = { "Spectral", "Grain" };
const char* const CloudShiftNames[7] = { "Off", "+12", "+7", "+5", "-5", "-7", "-12" };
const float CloudShiftSemitones[7] = { 0.0f, 12.0f, 7.0f, 5.0f, -5.0f, -7.0f, -12.0f };
const char* const KeysFilterNames[2] = { "Classic", "One Euro" };
const char* const TransposeNames[7] = { "None", "Fourth up", "Fifth up", "Octave up", "Fourth down", "Fifth down", "Octave down" };
const char* const BinauralNames[2] = { "Off", "Headphones" };
const char* const BrainModeNames[NumBrainModes] = { "Free", "Chords" };
const char* const StrikeTypeNames[3] = { "String", "Wood", "Metal" };
const char* const StrikeWhoNames[2] = { "Keys", "Keys + Brain" };
const char* const NearKindNames[3] = { "Note", "Phrase", "Sequence" };
const char* const NearPitchNames[5] = { "Consonant", "Highest", "Lowest", "Root", "Cluster" };
const char* const StackNames[NumStacks] = { "Detune", "Octaves", "Fifths", "Major", "Minor", "Seventh", "Harmonics", "Subharmonics" };
/** Strand ratios, ordered so that fewer strands still make sense (2 = root + fifth, 3 = a triad...). */
const double StackRatios[NumStacks][6] = {
    { 1.0, 1.0,       1.0,       1.0,       1.0,       1.0       },   // Detune (classic unison)
    { 1.0, 2.0,       0.5,       4.0,       0.25,      1.0       },   // Octaves
    { 1.0, 1.5,       2.0,       3.0,       0.5,       2.25      },   // Fifths: 1 3/2 2 3 1/2 9/4
    { 1.0, 1.5,       1.25,      2.0,       2.5,       0.5       },   // Major: 1 3/2 5/4 2 5/2 1/2
    { 1.0, 1.5,       1.2,       2.0,       2.4,       0.5       },   // Minor: 1 3/2 6/5 2 12/5 1/2
    { 1.0, 1.5,       1.25,      1.75,      2.0,       0.5       },   // Seventh: 1 3/2 5/4 7/4 2 1/2
    { 1.0, 2.0,       3.0,       4.0,       5.0,       6.0       },   // Harmonics
    { 1.0, 0.5,       1.0 / 3.0, 0.25,      0.2,       1.0 / 6.0 },   // Subharmonics
};
const float ShimmerPitchSemitones[NumShimmerPitches] = { 12.0f, 7.0f, 5.0f, 19.0f, -12.0f, 24.0f };

namespace {
using KT = ParamKindE;   ///< short for the kind column, so that a table row stays on one line
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
constexpr ParamDesc F(ParamIdE id, const char* key, const char* name, const char* sec,
                      float mn, float mx, float def, float skew, const char* unit)
{ return { id, key, name, sec, KT::FLOAT, mn, mx, def, skew, unit, nullptr, 0 }; }
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
constexpr ParamDesc I(ParamIdE id, const char* key, const char* name, const char* sec,
                      float mn, float mx, float def, const char* unit = "")
{ return { id, key, name, sec, KT::INT, mn, mx, def, 1.0f, unit, nullptr, 0 }; }
/**
 * @brief A switch's row: 0 or 1, written "off" / "on" in a preset.
 * @param id    the parameter
 * @param key   the stable identifier
 * @param name  the display name
 * @param sec   the section name
 * @param def   whether the switch is on by default
 * @return      the description, range 0 .. 1
 */
constexpr ParamDesc B(ParamIdE id, const char* key, const char* name, const char* sec, bool def)
{ return { id, key, name, sec, KT::BOOL, 0.0f, 1.0f, def ? 1.0f : 0.0f, 1.0f, "", nullptr, 0 }; }
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
constexpr ParamDesc C(ParamIdE id, const char* key, const char* name, const char* sec,
                      const char* const* choices, int n, int def)
{ return { id, key, name, sec, KT::CHOICE, 0.0f, static_cast<float>(n - 1), static_cast<float>(def), 1.0f, "", choices, n }; }

/**
 * @brief Every parameter's description, in ParamId order -- the table paramTable() hands out.
 *
 * The rows are grouped by section as the panels show them, but the order is the enum's, and the
 * enum only ever grows at the end: the runs of rows after the Clock section are the parameters
 * added since the first release, each run under a comment saying what it brought. The self test
 * checks that row i describes ParamId i.
 */
const std::array<ParamDesc, NumParams> Table = {{
    F(ParamIdE::MASTER_GAIN,  "master_gain",  "Master",        "Master",     -40.f, 12.f,   -6.f,  1.f,  "dB"),

    C(ParamIdE::SRC1_TYPE,    "src1_type",    "Type",          "Source 1", SourceTypeNames, NumSourceTypes, 5),
    F(ParamIdE::OSC_LEVEL,    "osc_level",    "Level",         "Source 1", 0.f,   1.f,    1.f,   1.f,  ""),
    I(ParamIdE::PARTIALS,    "partials",     "Partials",      "Source 1"  , 1.f,   32.f,   16.f),
    F(ParamIdE::TILT,        "tilt",         "Spectral Tilt", "Source 1"  , 0.3f,  3.f,    1.2f,  1.f,  ""),
    F(ParamIdE::BRIGHTNESS,  "brightness",   "Brightness",    "Source 1"  , 0.f,   1.f,    0.7f,  1.f,  ""),
    F(ParamIdE::ODD_EVEN,     "odd_even",     "Odd / Even",    "Source 1"  , -1.f,  1.f,    0.f,   1.f,  ""),
    F(ParamIdE::INHARMONIC,  "inharmonic",   "Inharmonic",    "Source 1"  , 0.f,   1.f,    0.f,   1.f,  ""),
    F(ParamIdE::SHIMMER,     "shimmer",      "Shimmer",       "Source 1"  , 0.f,   1.f,    0.4f,  1.f,  ""),
    F(ParamIdE::SHIMMER_RATE, "shimmer_rate", "Shimmer Rate",  "Source 1"  , 0.01f, 2.f,    0.15f, 0.4f, "Hz"),
    I(ParamIdE::UNISON,      "strands",      "Strands",       "Strands"   , 1.f,   6.f,    3.f),
    F(ParamIdE::DETUNE,      "detune",       "Detune",        "Strands"   , 0.f,   50.f,   8.f,   0.6f, "ct"),
    F(ParamIdE::DRIFT,       "drift",        "Pitch Drift",   "Strands"   , 0.f,   30.f,   4.f,   0.6f, "ct"),
    F(ParamIdE::DRIFT_RATE,   "drift_rate",   "Drift Rate",    "Strands"   , 0.01f, 1.f,    0.08f, 0.4f, "Hz"),
    F(ParamIdE::SPREAD,      "spread",       "Stereo Spread", "Strands"   , 0.f,   1.f,    0.7f,  1.f,  ""),
    F(ParamIdE::BLOOM,       "bloom",        "Bloom",         "Strands"   , 0.f,   1.f,    0.f,   1.f,  ""),
    F(ParamIdE::BLOOM_TIME,   "bloom_time",   "Bloom Time",    "Strands"   , 5.f,   300.f,  60.f,  0.4f, "s"),
    C(ParamIdE::STACK,       "stack",        "Stack",         "Strands"   , StackNames, NumStacks, 0),
    F(ParamIdE::RATE_WANDER,  "rate_wander",  "Rate Wander",   "Strands"   , 0.f,   1.f,    0.3f,  1.f,  ""),

    I(ParamIdE::SRC1_OCTAVE,   "src1_octave",   "Octave",    "Source 1", -2.f,  2.f,    0.f),
    C(ParamIdE::SRC1_RATIO,    "src1_ratio",    "Ratio",     "Source 1", SlotRatioNames, NumSlotRatios, 0),
    F(ParamIdE::SRC1_PAN,      "src1_pan",      "Pan",       "Source 1", -1.f,  1.f,    0.f,    1.f,  ""),
    C(ParamIdE::SRC1_TABLE,    "src1_table",    "Table",     "Source 1", TableNames, NumTables, 0),
    F(ParamIdE::SRC1_POSITION, "src1_pos",      "Position",  "Source 1", 0.f,   1.f,    0.3f,   1.f,  ""),
    F(ParamIdE::SRC1_POS_DRIFT, "src1_pos_drift","Pos Drift", "Source 1", 0.f,   1.f,    0.3f,   1.f,  ""),
    F(ParamIdE::SRC1_FM_RATIO,  "src1_fm_ratio", "FM Ratio",  "Source 1", 0.25f, 8.f,    2.f,    0.5f, "x"),
    F(ParamIdE::SRC1_FM_INDEX,  "src1_fm_index", "FM Index",  "Source 1", 0.f,   8.f,    1.f,    0.6f, ""),
    F(ParamIdE::SRC1_GRAIN,    "src1_grain",    "Grain",     "Source 1", 30.f,  1000.f, 200.f,  0.5f, "ms"),
    F(ParamIdE::SRC1_DENSITY,  "src1_density",  "Density",   "Source 1", 1.f,   200.f,  12.f,   0.4f, "/s"),
    C(ParamIdE::SRC1_DENSITY_SYNC, "src1_density_sync", "Sync", "Source 1", SyncDivNames, NumSyncDivs, 0),
    C(ParamIdE::SRC1_FOLLOW,   "src1_follow",   "Pitch",     "Source 1", FollowNames, 2, 0),
    I(ParamIdE::SRC1_GRAINS,   "src1_grains",   "Grains",    "Source 1", 1.f,   128.f,  16.f),
    F(ParamIdE::SRC1_SPREAD,   "src1_spread",   "Spread",    "Source 1", 0.f,   1.f,    0.03f,  0.5f, ""),
    C(ParamIdE::SRC1_NOISE,    "src1_noise",    "Noise",     "Source 1", NoiseKindNames, NumNoiseKinds, 1),
    F(ParamIdE::SRC1_NOISE_Q,   "src1_noise_q",  "Noise Q",   "Source 1", 0.f,   1.f,    0.4f,   1.f,  ""),
    F(ParamIdE::SRC1_DRIFT,    "src1_drift",    "Drift",     "Source 1", 0.f,   30.f,   0.f,    0.6f, "ct"),

    C(ParamIdE::SRC2_TYPE,     "src2_type",     "Type",      "Source 2", SourceTypeNames, NumSourceTypes, 0),
    F(ParamIdE::SRC2_LEVEL,    "src2_level",    "Level",     "Source 2", 0.f,   1.f,    0.5f,   1.f,  ""),
    I(ParamIdE::SRC2_OCTAVE,   "src2_octave",   "Octave",    "Source 2", -2.f,  2.f,    0.f),
    C(ParamIdE::SRC2_RATIO,    "src2_ratio",    "Ratio",     "Source 2", SlotRatioNames, NumSlotRatios, 0),
    F(ParamIdE::SRC2_PAN,      "src2_pan",      "Pan",       "Source 2", -1.f,  1.f,    -0.3f,  1.f,  ""),
    C(ParamIdE::SRC2_TABLE,    "src2_table",    "Table",     "Source 2", TableNames, NumTables, 0),
    F(ParamIdE::SRC2_POSITION, "src2_pos",      "Position",  "Source 2", 0.f,   1.f,    0.3f,   1.f,  ""),
    F(ParamIdE::SRC2_POS_DRIFT, "src2_pos_drift","Pos Drift", "Source 2", 0.f,   1.f,    0.3f,   1.f,  ""),
    F(ParamIdE::SRC2_FM_RATIO,  "src2_fm_ratio", "FM Ratio",  "Source 2", 0.25f, 8.f,    2.f,    0.5f, "x"),
    F(ParamIdE::SRC2_FM_INDEX,  "src2_fm_index", "FM Index",  "Source 2", 0.f,   8.f,    1.f,    0.6f, ""),
    F(ParamIdE::SRC2_GRAIN,    "src2_grain",    "Grain",     "Source 2", 30.f,  1000.f, 200.f,  0.5f, "ms"),
    F(ParamIdE::SRC2_DENSITY,  "src2_density",  "Density",   "Source 2", 1.f,   200.f,  12.f,   0.4f, "/s"),
    C(ParamIdE::SRC2_DENSITY_SYNC, "src2_density_sync", "Sync", "Source 2", SyncDivNames, NumSyncDivs, 0),
    C(ParamIdE::SRC2_FOLLOW,   "src2_follow",   "Pitch",     "Source 2", FollowNames, 2, 0),
    I(ParamIdE::SRC2_GRAINS,   "src2_grains",   "Grains",    "Source 2", 1.f,   128.f,  16.f),
    F(ParamIdE::SRC2_SPREAD,   "src2_spread",   "Spread",    "Source 2", 0.f,   1.f,    0.03f,  0.5f, ""),
    C(ParamIdE::SRC2_NOISE,    "src2_noise",    "Noise",     "Source 2", NoiseKindNames, NumNoiseKinds, 1),
    F(ParamIdE::SRC2_NOISE_Q,   "src2_noise_q",  "Noise Q",   "Source 2", 0.f,   1.f,    0.4f,   1.f,  ""),
    I(ParamIdE::SRC2_PARTIALS,    "src2_partials",     "Partials",     "Source 2", 1.f,   32.f,   16.f),
    F(ParamIdE::SRC2_TILT,        "src2_tilt",         "Tilt",         "Source 2", 0.3f,  3.f,    1.2f,   1.f,  ""),
    F(ParamIdE::SRC2_BRIGHT,      "src2_bright",       "Bright",       "Source 2", 0.f,   1.f,    0.7f,   1.f,  ""),
    F(ParamIdE::SRC2_ODD_EVEN,     "src2_odd_even",     "Odd / Even",   "Source 2", -1.f,  1.f,    0.f,    1.f,  ""),
    F(ParamIdE::SRC2_INHARM,      "src2_inharmonic",   "Inharmonic",   "Source 2", 0.f,   1.f,    0.f,    1.f,  ""),
    F(ParamIdE::SRC2_SHIMMER,     "src2_shimmer",      "Shimmer",      "Source 2", 0.f,   1.f,    0.4f,   1.f,  ""),
    F(ParamIdE::SRC2_SHIMMER_RATE, "src2_shimmer_rate", "Shimmer Rate", "Source 2", 0.01f, 2.f,    0.15f,  0.4f, "Hz"),
    F(ParamIdE::SRC2_DRIFT,       "src2_drift",        "Drift",        "Source 2", 0.f,   30.f,   0.f,    0.6f, "ct"),

    C(ParamIdE::SRC3_TYPE,     "src3_type",     "Type",      "Source 3", SourceTypeNames, NumSourceTypes, 0),
    F(ParamIdE::SRC3_LEVEL,    "src3_level",    "Level",     "Source 3", 0.f,   1.f,    0.5f,   1.f,  ""),
    I(ParamIdE::SRC3_OCTAVE,   "src3_octave",   "Octave",    "Source 3", -2.f,  2.f,    0.f),
    C(ParamIdE::SRC3_RATIO,    "src3_ratio",    "Ratio",     "Source 3", SlotRatioNames, NumSlotRatios, 0),
    F(ParamIdE::SRC3_PAN,      "src3_pan",      "Pan",       "Source 3", -1.f,  1.f,    0.3f,   1.f,  ""),
    C(ParamIdE::SRC3_TABLE,    "src3_table",    "Table",     "Source 3", TableNames, NumTables, 0),
    F(ParamIdE::SRC3_POSITION, "src3_pos",      "Position",  "Source 3", 0.f,   1.f,    0.3f,   1.f,  ""),
    F(ParamIdE::SRC3_POS_DRIFT, "src3_pos_drift","Pos Drift", "Source 3", 0.f,   1.f,    0.3f,   1.f,  ""),
    F(ParamIdE::SRC3_FM_RATIO,  "src3_fm_ratio", "FM Ratio",  "Source 3", 0.25f, 8.f,    2.f,    0.5f, "x"),
    F(ParamIdE::SRC3_FM_INDEX,  "src3_fm_index", "FM Index",  "Source 3", 0.f,   8.f,    1.f,    0.6f, ""),
    F(ParamIdE::SRC3_GRAIN,    "src3_grain",    "Grain",     "Source 3", 30.f,  1000.f, 200.f,  0.5f, "ms"),
    F(ParamIdE::SRC3_DENSITY,  "src3_density",  "Density",   "Source 3", 1.f,   200.f,  12.f,   0.4f, "/s"),
    C(ParamIdE::SRC3_DENSITY_SYNC, "src3_density_sync", "Sync", "Source 3", SyncDivNames, NumSyncDivs, 0),
    C(ParamIdE::SRC3_FOLLOW,   "src3_follow",   "Pitch",     "Source 3", FollowNames, 2, 0),
    I(ParamIdE::SRC3_GRAINS,   "src3_grains",   "Grains",    "Source 3", 1.f,   128.f,  16.f),
    F(ParamIdE::SRC3_SPREAD,   "src3_spread",   "Spread",    "Source 3", 0.f,   1.f,    0.03f,  0.5f, ""),
    C(ParamIdE::SRC3_NOISE,    "src3_noise",    "Noise",     "Source 3", NoiseKindNames, NumNoiseKinds, 1),
    F(ParamIdE::SRC3_NOISE_Q,   "src3_noise_q",  "Noise Q",   "Source 3", 0.f,   1.f,    0.4f,   1.f,  ""),
    I(ParamIdE::SRC3_PARTIALS,    "src3_partials",     "Partials",     "Source 3", 1.f,   32.f,   16.f),
    F(ParamIdE::SRC3_TILT,        "src3_tilt",         "Tilt",         "Source 3", 0.3f,  3.f,    1.2f,   1.f,  ""),
    F(ParamIdE::SRC3_BRIGHT,      "src3_bright",       "Bright",       "Source 3", 0.f,   1.f,    0.7f,   1.f,  ""),
    F(ParamIdE::SRC3_ODD_EVEN,     "src3_odd_even",     "Odd / Even",   "Source 3", -1.f,  1.f,    0.f,    1.f,  ""),
    F(ParamIdE::SRC3_INHARM,      "src3_inharmonic",   "Inharmonic",   "Source 3", 0.f,   1.f,    0.f,    1.f,  ""),
    F(ParamIdE::SRC3_SHIMMER,     "src3_shimmer",      "Shimmer",      "Source 3", 0.f,   1.f,    0.4f,   1.f,  ""),
    F(ParamIdE::SRC3_SHIMMER_RATE, "src3_shimmer_rate", "Shimmer Rate", "Source 3", 0.01f, 2.f,    0.15f,  0.4f, "Hz"),
    F(ParamIdE::SRC3_DRIFT,       "src3_drift",        "Drift",        "Source 3", 0.f,   30.f,   0.f,    0.6f, "ct"),

    F(ParamIdE::SUB_LEVEL,    "sub_level",    "Level",         "Foundation", 0.f,   1.f,    0.f,   1.f,  ""),
    C(ParamIdE::SUB_OCTAVE,   "sub_octave",   "Octave",        "Foundation", SubOctaveNames, 2, 1),   // -2 by default (25.09.2026): 33-62 Hz under a C3-B3 root, where the guide wants a sub
    F(ParamIdE::SUB_GLIDE,    "sub_glide",    "Glide",         "Foundation", 0.1f,  30.f,   8.f,   0.4f, "s"),
    F(ParamIdE::SUB_BINAURAL, "sub_binaural", "Binaural",      "Foundation", 0.f,   12.f,   0.f,   0.6f, "Hz"),
    F(ParamIdE::SUB_TONE,     "sub_tone",     "Tone",          "Foundation", 0.f,   1.f,    0.2f,  1.f,  ""),
    C(ParamIdE::SUB_SOURCE,   "sub_source",   "Source",        "Foundation", SubSourceNames, 3, 0),
    F(ParamIdE::PAD_LOW_CUT,   "pad_low_cut",  "Pad Low Cut",   "Foundation", 0.f,   300.f,  0.f,   0.6f, "Hz"),

    F(ParamIdE::STRIKE_LEVEL, "strike_level", "Strike",        "Strike",     0.f,   1.f,    0.f,   1.f,  ""),
    C(ParamIdE::STRIKE_TYPE,  "strike_type",  "Type",          "Strike",     StrikeTypeNames, 3, 0),
    F(ParamIdE::STRIKE_DECAY, "strike_decay", "Decay",         "Strike",     0.02f, 3.f,    0.4f,  0.5f, "s"),
    F(ParamIdE::STRIKE_DAMP,  "strike_damp",  "Damp",          "Strike",     0.f,   1.f,    0.5f,  1.f,  ""),
    C(ParamIdE::STRIKE_WHO,   "strike_who",   "Fires",         "Strike",     StrikeWhoNames, 2, 0),

    F(ParamIdE::AIR,         "air",          "Air",           "Air",        0.f,   1.f,    0.15f, 1.f,  ""),
    F(ParamIdE::AIR_COLOR,    "air_color",    "Color",         "Air",        1.f,   16.f,   3.f,   0.5f, "x f0"),
    F(ParamIdE::AIR_Q,        "air_q",        "Q",             "Air",        1.f,   40.f,   10.f,  0.5f, ""),

    F(ParamIdE::ATTACK,      "attack",       "Attack",        "Envelope",   0.01f, 60.f,   6.f,   0.3f, "s"),
    F(ParamIdE::DECAY,       "decay",        "Decay",         "Envelope",   0.01f, 60.f,   4.f,   0.3f, "s"),
    F(ParamIdE::SUSTAIN,     "sustain",      "Sustain",       "Envelope",   0.f,   1.f,    0.8f,  1.f,  ""),
    F(ParamIdE::RELEASE,     "release",      "Release",       "Envelope",   0.05f, 120.f,  12.f,  0.3f, "s"),

    B(ParamIdE::FILTER_ON,    "filter_on",    "On",            "Filter",     true),
    C(ParamIdE::FILTER_MODEL, "filter_model", "Model",         "Filter",     FilterModelNames, NumFilterModels, 1),
    F(ParamIdE::CUTOFF,      "cutoff",       "Cutoff",        "Filter",     40.f,  18000.f, 2500.f, 0.25f, "Hz"),
    F(ParamIdE::RESONANCE,   "resonance",    "Resonance",     "Filter",     0.f,   1.f,    0.15f, 1.f,  ""),
    F(ParamIdE::FILTER_ENV,   "filter_env",   "Env Amount",    "Filter",     -1.f,  1.f,    0.3f,  1.f,  ""),
    F(ParamIdE::FILTER_DRIFT, "filter_drift", "Drift",         "Filter",     0.f,   1.f,    0.3f,  1.f,  ""),
    F(ParamIdE::KEY_TRACK,    "keytrack",     "Key Track",     "Filter",     0.f,   1.f,    0.5f,  1.f,  ""),
    F(ParamIdE::FILTER_DRIVE, "filter_drive", "Drive",         "Filter",     0.f,   1.f,    0.f,   1.f,  ""),

    C(ParamIdE::Z_MODE,       "z_mode",       "Mode",          "Z-Plane",    ZModeNames, 4, 0),
    C(ParamIdE::Z_ROUTE,      "z_route",      "Route",         "Z-Plane",    FilterRouteNames, 2, 0),
    C(ParamIdE::Z_SHAPE,      "z_shape",      "Shape",         "Z-Plane",    ZShapeNames, ZShapes, 0),
    F(ParamIdE::ZX,          "z_x",          "X",             "Z-Plane",    0.f,   1.f,    0.5f,  1.f,  ""),
    F(ParamIdE::ZY,          "z_y",          "Y",             "Z-Plane",    0.f,   1.f,    0.5f,  1.f,  ""),
    F(ParamIdE::ZZ,          "z_z",          "Transform",     "Z-Plane",    0.f,   1.f,    0.f,   1.f,  ""),
    F(ParamIdE::Z_RATE,       "z_rate",       "Rate",          "Z-Plane",    0.005f, 1.f,   0.05f, 0.4f, "Hz"),
    F(ParamIdE::Z_DEPTH,      "z_depth",      "Depth",         "Z-Plane",    0.f,   1.f,    0.5f,  1.f,  ""),
    F(ParamIdE::Z_RESONANCE,  "z_res",        "Resonance",     "Z-Plane",    0.f,   1.f,    0.5f,  1.f,  ""),
    F(ParamIdE::Z_KEY_TRACK,   "z_keytrack",   "Key Track",     "Z-Plane",    0.f,   1.f,    0.f,   1.f,  ""),
    F(ParamIdE::Z_MIX,        "z_mix",        "Mix",           "Z-Plane",    0.f,   1.f,    0.7f,  1.f,  ""),
    F(ParamIdE::Z_DECAY,      "z_decay",      "Decay",         "Z-Plane",    0.02f, 40.f,   2.5f,  0.35f, "s"),
    F(ParamIdE::Z_DAMP,       "z_damp",       "Damping",       "Z-Plane",    0.f,   1.f,    0.6f,  1.f,  ""),

    F(ParamIdE::DEPTH,       "depth",        "Depth",         "Space",      0.f,   1.f,    0.7f,  1.f,  ""),
    F(ParamIdE::KEYS_DEPTH,   "keys_depth",   "Keys Depth",    "Space",      0.f,   1.f,    0.f,   1.f,  ""),
    F(ParamIdE::PAN_DRIFT,    "pan_drift",    "Pan Drift",     "Space",      0.f,   1.f,    0.4f,  1.f,  ""),
    F(ParamIdE::ITD,         "itd",          "Time Width",    "Space",      0.f,   1.f,    0.6f,  1.f,  ""),
    F(ParamIdE::ARC_AMOUNT,   "arc",          "Arc",           "Space",      0.f,   1.f,    0.3f,  1.f,  ""),
    F(ParamIdE::ARC_PERIOD,   "arc_period",   "Arc Period",    "Space",      2.f,   240.f,  40.f,  0.4f, "min"),
    C(ParamIdE::ARC_SYNC, "arc_sync", "Arc Sync", "Space", SyncDivNames, NumSyncDivs, 0),
    F(ParamIdE::PRESENCE,    "presence",     "Presence",      "Space",      0.f,   6.f,    0.f,   1.f,  "dB"),
    F(ParamIdE::BREATH,      "breath",       "Breath",        "Space",      0.f,   1.f,    0.f,   1.f,  ""),
    F(ParamIdE::BREATH_RATE,  "breath_rate",  "Breath Rate",   "Space",      0.005f, 0.2f,  0.03f, 0.5f, "Hz"),
    F(ParamIdE::PHASE_WIDTH,  "phase_width",  "Phase Width",   "Space",      0.f,   1.f,    0.f,   1.f,  ""),
    F(ParamIdE::PHASE_RATE,   "phase_rate",   "Phase Rate",    "Space",      0.005f, 0.5f,  0.03f, 0.5f, "Hz"),
    F(ParamIdE::DOPPLER,     "doppler",      "Doppler",       "Space",      0.f,   1.f,    0.f,   1.f,  ""),
    F(ParamIdE::EXTERNALISE, "externalise",  "Externalise",   "Space",      0.f,   1.f,    0.f,   1.f,  ""),

    F(ParamIdE::PRESS_DISTANCE, "press_distance", "Press Near",  "Expression", 0.f,  1.f,   0.f,  1.f, ""),
    F(ParamIdE::PRESS_BRIGHT,   "press_bright",   "Press Bright","Expression", -1.f, 1.f,   0.f,  1.f, ""),
    F(ParamIdE::PRESS_LEVEL,    "press_level",    "Press Level", "Expression", 0.f,  1.f,   0.f,  1.f, ""),
    F(ParamIdE::SLIDE_CUTOFF,   "slide_cutoff",   "Slide Filter","Expression", -3.f, 3.f,   0.f,  1.f, "oct"),
    F(ParamIdE::SLIDE_Z,        "slide_z",        "Slide Z",     "Expression", -1.f, 1.f,   0.f,  1.f, ""),
    F(ParamIdE::BEND_RANGE,     "bend_range",     "Bend Range",  "Expression", 0.f,  48.f,  2.f,  0.5f, "st"),
    B(ParamIdE::MPE_ON,         "mpe",            "MPE",         "Expression", false),

    F(ParamIdE::ENSEMBLE_MIX,   "ens_mix",   "Mix",   "Ensemble", 0.f,   1.f, 0.4f, 1.f,  ""),
    F(ParamIdE::ENSEMBLE_DEPTH, "ens_depth", "Depth", "Ensemble", 0.f,   1.f, 0.4f, 1.f,  ""),
    F(ParamIdE::ENSEMBLE_RATE,  "ens_rate",  "Rate",  "Ensemble", 0.02f, 2.f, 0.2f, 0.4f, "Hz"),
    C(ParamIdE::ENSEMBLE_SYNC, "ensemble_sync", "Sync", "Ensemble", SyncDivNames, NumSyncDivs, 0),

    F(ParamIdE::DELAY_TIME_L,    "dly_time_l",   "Time L",    "Delay", 0.02f, 4.f,   0.75f, 0.4f, "s"),
    F(ParamIdE::DELAY_TIME_R,    "dly_time_r",   "Time R",    "Delay", 0.02f, 4.f,   1.1f,  0.4f, "s"),
    C(ParamIdE::DELAY_SYNC_L, "dly_sync_l", "Sync L", "Delay", SyncDivNames, NumSyncDivs, 0),
    C(ParamIdE::DELAY_SYNC_R, "dly_sync_r", "Sync R", "Delay", SyncDivNames, NumSyncDivs, 0),
    F(ParamIdE::DELAY_FEEDBACK, "dly_feedback", "Feedback",  "Delay", 0.f,   0.95f, 0.5f,  1.f,  ""),
    F(ParamIdE::DELAY_CROSS,    "dly_cross",    "Cross",     "Delay", 0.f,   1.f,   0.3f,  1.f,  ""),
    F(ParamIdE::DELAY_DAMP,     "dly_damp",     "Damping",   "Delay", 0.f,   1.f,   0.5f,  1.f,  ""),
    F(ParamIdE::DELAY_ABSORB,   "dly_absorb",   "Absorb",    "Delay", 0.f,   1.f,   0.f,   1.f,  ""),
    F(ParamIdE::DELAY_MIX,      "dly_mix",      "Mix",       "Delay", 0.f,   1.f,   0.25f, 1.f,  ""),
    F(ParamIdE::DELAY_TO_FAR,    "dly_to_far",   "To Far",    "Delay", 0.f,   1.f,   0.4f,  1.f,  ""),
    F(ParamIdE::DELAY_DUCK,     "dly_duck",     "Duck",      "Delay", 0.f,   1.f,   0.f,   1.f,  ""),

    F(ParamIdE::DELAY2_TIME_L,    "dly2_time_l",   "Time L",    "Delay 2", 0.02f, 4.f,   1.5f,  0.4f, "s"),
    F(ParamIdE::DELAY2_TIME_R,    "dly2_time_r",   "Time R",    "Delay 2", 0.02f, 4.f,   2.2f,  0.4f, "s"),
    C(ParamIdE::DELAY2_SYNC_L, "dly2_sync_l", "Sync L", "Delay 2", SyncDivNames, NumSyncDivs, 0),
    C(ParamIdE::DELAY2_SYNC_R, "dly2_sync_r", "Sync R", "Delay 2", SyncDivNames, NumSyncDivs, 0),
    F(ParamIdE::DELAY2_FEEDBACK, "dly2_feedback", "Feedback",  "Delay 2", 0.f,   0.95f, 0.4f,  1.f,  ""),
    F(ParamIdE::DELAY2_CROSS,    "dly2_cross",    "Cross",     "Delay 2", 0.f,   1.f,   0.5f,  1.f,  ""),
    F(ParamIdE::DELAY2_DAMP,     "dly2_damp",     "Damping",   "Delay 2", 0.f,   1.f,   0.7f,  1.f,  ""),
    F(ParamIdE::DELAY2_ABSORB,   "dly2_absorb",   "Absorb",    "Delay 2", 0.f,   1.f,   0.f,   1.f,  ""),
    F(ParamIdE::DELAY2_MIX,      "dly2_mix",      "Mix",       "Delay 2", 0.f,   1.f,   0.f,   1.f,  ""),
    F(ParamIdE::DELAY2_TO_FAR,    "dly2_to_far",   "To Far",    "Delay 2", 0.f,   1.f,   0.5f,  1.f,  ""),

    F(ParamIdE::NEAR_MIX,   "near_mix",   "Mix",     "Near Reverb", 0.f,  1.f, 0.2f, 1.f,  ""),
    F(ParamIdE::NEAR_DECAY, "near_decay", "Decay",   "Near Reverb", 0.2f, 6.f, 1.2f, 0.5f, "s"),
    F(ParamIdE::NEAR_DAMP,  "near_damp",  "Damping", "Near Reverb", 0.f,  1.f, 0.3f, 1.f,  ""),
    F(ParamIdE::BLUR_MIX,   "blur_mix",   "Blur",  "Blur", 0.f,  1.f, 0.f,  1.f,  ""),
    F(ParamIdE::BLUR_SMEAR, "blur_smear", "Smear", "Blur", 0.f,  1.f, 0.5f, 1.f,  ""),

    F(ParamIdE::FAR_LEVEL,    "far_level",    "Level",     "Far Reverb", 0.f,   1.f,     0.8f,   1.f,  ""),
    F(ParamIdE::FAR_SIZE,     "far_size",     "Size",      "Far Reverb", 0.5f,  3.f,     2.f,    1.f,  ""),
    F(ParamIdE::FAR_DECAY,    "far_decay",    "Decay",     "Far Reverb", 1.f,   90.f,    25.f,   0.3f, "s"),
    F(ParamIdE::FAR_DAMP,     "far_damp",     "Damping",   "Far Reverb", 0.f,   1.f,     0.5f,   1.f,  ""),
    F(ParamIdE::FAR_PRE_DELAY, "far_predelay", "Pre-Delay", "Far Reverb", 0.f,   500.f,   3.f,    0.5f, "ms"),   // 3 ms by default (25.09.2026): the horizon has no gap; the near source's gap is Depth Gap
    F(ParamIdE::FAR_ASYM,     "far_asym",     "Asymmetry", "Far Reverb", 0.f,   1.f,     0.5f,   1.f,  ""),
    F(ParamIdE::FAR_HIGHCUT,  "far_highcut",  "Tail Cut",  "Far Reverb", 500.f, 16000.f, 3500.f, 0.3f, "Hz"),
    B(ParamIdE::FAR_FREEZE,   "far_freeze",   "Freeze",    "Far Reverb", false),
    F(ParamIdE::FAR_ROTATE,   "far_rotate",   "Rotate",    "Far Reverb", 0.f,  1.f, 0.f,  1.f,  ""),
    F(ParamIdE::FAR_UNMASK,   "far_unmask",   "Unmask",    "Far Reverb", 0.f,  1.f, 0.f,  1.f,  ""),
    F(ParamIdE::FAR_DIFFUSE,  "far_diffuse",  "Diffuse",   "Far Reverb", 0.f,  1.f, 0.f,  1.f,  ""),

    F(ParamIdE::FEEDBACK_BUS,   "fb_bus",   "To Bus",   "Feedback", 0.f,   1.f,     0.f,    1.f,  ""),
    F(ParamIdE::FEEDBACK_FM,    "fb_fm",    "To Pitch", "Feedback", 0.f,   1.f,     0.f,    1.f,  ""),
    F(ParamIdE::FEEDBACK_TONE,  "fb_tone",  "Tone",     "Feedback", 200.f, 8000.f,  1500.f, 0.4f, "Hz"),
    F(ParamIdE::FEEDBACK_DRIVE, "fb_drive", "Drive",    "Feedback", 0.f,   1.f,     0.5f,   1.f,  ""),

    F(ParamIdE::ROOM_LEVEL,    "room_level",    "Level",     "Room", 0.f,   1.f,     0.f,    1.f,  ""),
    C(ParamIdE::ROOM_SOURCE,   "room_source",   "Source",    "Room", RoomSourceNames, 2, 0),
    F(ParamIdE::ROOM_PRE_DELAY, "room_predelay", "Pre-Delay", "Room", 0.f,   300.f,   20.f,   0.5f, "ms"),
    F(ParamIdE::ROOM_HIGHCUT,  "room_highcut",  "Tail Cut",  "Room", 500.f, 16000.f, 5000.f, 0.3f, "Hz"),
    F(ParamIdE::ROOM_MORPH,   "room_morph",  "Morph A/B", "Room", 0.f,  1.f,  0.f,  1.f,  ""),

    F(ParamIdE::COSMOS_SEND,        "cosmos_send",        "Send",        "Cosmos", 0.f,    1.f,    0.f,   1.f,  ""),
    F(ParamIdE::COSMOS_SHIFT,       "cosmos_shift",       "Shift",       "Cosmos", -300.f, 300.f,  0.f,   1.f,  "Hz"),
    F(ParamIdE::COSMOS_SHIFT_DRIFT,  "cosmos_shift_drift", "Shift Drift", "Cosmos", 0.f,    1.f,    0.3f,  1.f,  ""),
    F(ParamIdE::COSMOS_RES,         "cosmos_res",         "Resonator",   "Cosmos", 0.f,    1.f,    0.f,   1.f,  ""),
    F(ParamIdE::COSMOS_RES_PITCH,    "cosmos_res_pitch",   "Res Pitch",   "Cosmos", 0.25f,  8.f,    2.f,   0.5f, "x root"),
    F(ParamIdE::COSMOS_RES_FEEDBACK, "cosmos_res_fb",      "Res Feedback","Cosmos", 0.f,    0.97f,  0.8f,  1.f,  ""),
    F(ParamIdE::COSMOS_VOWEL,       "cosmos_vowel",       "Vowel",       "Cosmos", 0.f,    1.f,    0.f,   1.f,  ""),
    F(ParamIdE::COSMOS_VOWEL_RATE,   "cosmos_vowel_rate",  "Vowel Rate",  "Cosmos", 0.005f, 0.5f,   0.05f, 0.4f, "Hz"),
    F(ParamIdE::COSMOS_NEBULA,      "cosmos_nebula",      "Nebula",      "Cosmos", 0.f,    1.f,    0.f,   1.f,  ""),
    F(ParamIdE::COSMOS_SMEAR,       "cosmos_smear",       "Smear",       "Cosmos", 0.f,    1.f,    0.7f,  1.f,  ""),
    F(ParamIdE::COSMOS_SHIMMER,     "cosmos_shimmer",     "Shimmer",     "Cosmos", 0.f,    1.f,    0.f,   1.f,  ""),
    C(ParamIdE::COSMOS_SHIMMER_PITCH,"cosmos_shimmer_pitch","Shimmer Pitch","Cosmos", ShimmerPitchNames, NumShimmerPitches, 0),
    F(ParamIdE::COSMOS_RETURN,      "cosmos_return",      "Return",      "Cosmos", 0.f,    1.f,    0.5f,  1.f,  ""),
    F(ParamIdE::COSMOS_TO_FAR,       "cosmos_to_far",      "To Far",      "Cosmos", 0.f,    1.f,    0.5f,  1.f,  ""),

    F(ParamIdE::CLOUD_SEND,    "cloud_send",    "Send",    "Cloud", 0.f,   1.f,    0.f,   1.f,  ""),
    F(ParamIdE::CLOUD_DENSITY, "cloud_density", "Density", "Cloud", 1.f,   60.f,   12.f,  0.5f, "/s"),
    C(ParamIdE::CLOUD_SYNC, "cloud_sync", "Sync", "Cloud", SyncDivNames, NumSyncDivs, 0),
    F(ParamIdE::CLOUD_SIZE,    "cloud_size",    "Grain",   "Cloud", 30.f,  800.f,  250.f, 0.5f, "ms"),
    F(ParamIdE::CLOUD_PITCH,   "cloud_pitch",   "Pitch",   "Cloud", 0.f,   1.f,    0.3f,  1.f,  ""),
    F(ParamIdE::CLOUD_SPRAY,   "cloud_spray",   "Spray",   "Cloud", 0.05f, 2.f,    0.8f,  0.5f, "s"),
    // Up to 2, which is what the grain engine has always accepted (GrainCloud::set clamps there):
    // the knob stopped at 1 and half the range was unreachable. Measured before the change, with
    // send, level, density and size all at their tops, the cloud moved the output by 0.6 dB and
    // sat nine decibels under the preset -- "fully up" was inaudible, which is not what a send is
    // for. Everything written before this keeps its value, and 0.7 is still the default.
    F(ParamIdE::CLOUD_LEVEL,   "cloud_level",   "Level",   "Cloud", 0.f,   2.f,    0.7f,  1.f,  ""),

    F(ParamIdE::BODY_LEVEL,    "body_level",    "Body",     "Body",   0.f,   1.f,    0.f,   1.f,  ""),
    C(ParamIdE::BODY_MATERIAL, "body_material", "Material", "Body",   BodyMaterialNames, NumBodyMaterials, 0),
    F(ParamIdE::BODY_PITCH,    "body_pitch",    "Pitch",    "Body",   0.25f, 4.f,    1.f,   0.5f, "x root"),
    F(ParamIdE::BODY_DECAY,    "body_decay",    "Decay",    "Body",   0.1f,  20.f,   4.f,   0.4f, "s"),
    F(ParamIdE::BODY_TONE,     "body_tone",     "Tone",     "Body",   0.f,   1.f,    0.4f,  1.f,  ""),
    F(ParamIdE::BODY_SPREAD,   "body_spread",   "Spread",   "Body",   0.f,   1.f,    0.6f,  1.f,  ""),

    F(ParamIdE::PATINA_AMOUNT, "patina",        "Patina",   "Patina", 0.f,   1.f,    0.f,   1.f,  ""),
    F(ParamIdE::PATINA_WOW,    "patina_wow",    "Wow",      "Patina", 0.f,   1.f,    0.3f,  1.f,  ""),
    F(ParamIdE::PATINA_HISS,   "patina_hiss",   "Hiss",     "Patina", 0.f,   1.f,    0.2f,  1.f,  ""),
    F(ParamIdE::PATINA_AGE,    "patina_age",    "Age",      "Patina", 0.f,   1.f,    0.3f,  1.f,  ""),

    F(ParamIdE::TILT2,        "master_tilt",   "Tilt",     "Master", -6.f,  6.f,    0.f,   1.f,  "dB"),
    F(ParamIdE::TILT_PIVOT,    "tilt_pivot",    "Pivot",    "Master", 100.f, 4000.f, 700.f, 0.4f, "Hz"),

    F(ParamIdE::BASS_MONO, "bass_mono", "Bass Mono", "Master", 40.f, 300.f, 150.f, 0.5f, "Hz"),
    F(ParamIdE::SIDE_AIR,  "side_air",  "Side Air",  "Master", 0.f,  6.f,   2.f,   1.f,  "dB"),
    F(ParamIdE::WIDTH,    "width",     "Width",     "Master", 0.f,  2.f,   1.2f,  1.f,  ""),
    B(ParamIdE::MONO_GUARD,"mono_guard","Mono Safe", "Master", true),

    B(ParamIdE::BRAIN_ON,         "brain_on",         "Active",     "Cluster Brain", true),
    I(ParamIdE::BRAIN_DENSITY,    "brain_density",    "Density",    "Cluster Brain", 1.f,  10.f,  5.f),
    F(ParamIdE::BRAIN_RATE,       "brain_rate",       "Event Rate", "Cluster Brain", 2.f,  300.f, 25.f, 0.4f, "s"),
    C(ParamIdE::BRAIN_SYNC, "brain_sync", "Sync", "Cluster Brain", SyncDivNames, NumSyncDivs, 0),
    F(ParamIdE::BRAIN_HOLD_MIN,    "brain_hold_min",   "Hold Min",   "Cluster Brain", 5.f,  600.f, 30.f, 0.4f, "s"),
    F(ParamIdE::BRAIN_HOLD_MAX,    "brain_hold_max",   "Hold Max",   "Cluster Brain", 5.f,  600.f, 120.f, 0.4f, "s"),
    I(ParamIdE::BRAIN_LOW,        "brain_low",        "Lowest",     "Cluster Brain", 24.f, 96.f,  36.f),
    I(ParamIdE::BRAIN_HIGH,       "brain_high",       "Highest",    "Cluster Brain", 24.f, 108.f, 79.f),
    F(ParamIdE::BRAIN_CONSONANCE, "brain_consonance", "Consonance", "Cluster Brain", 0.f,  1.f,   0.7f, 1.f, ""),
    F(ParamIdE::BRAIN_WANDER,     "brain_wander",     "Wander",     "Cluster Brain", 0.f,  1.f,   0.3f, 1.f, ""),
    C(ParamIdE::BRAIN_QUANTIZE, "brain_quantize", "Quantize", "Cluster Brain", SyncDivNames, NumSyncDivs, 0),

    C(ParamIdE::AUTO_MODE,      "auto_mode",     "Autoplay",  "Autoplay", BrainModeNames, NumBrainModes, 0),
    F(ParamIdE::AUTO_RATE,      "auto_rate",     "Every",     "Autoplay", 2.f,   900.f, 45.f, 0.4f, "s"),
    C(ParamIdE::AUTO_SYNC,      "auto_sync",     "Sync",      "Autoplay", SyncDivNames, NumSyncDivs, 0),
    F(ParamIdE::AUTO_LEAD,      "auto_lead",     "Voice Lead","Autoplay", 1.f,   24.f,  7.f,  0.6f, "st"),
    F(ParamIdE::AUTO_TENSION,   "auto_tension",  "Tension",   "Autoplay", 0.f,   1.f,   0.f,  1.f,  ""),
    F(ParamIdE::AUTO_ROOT_MOVE,  "auto_root_move","Root Move", "Autoplay", 0.f,   1.f,   0.2f, 1.f,  ""),
    B(ParamIdE::AUTO_STEP,      "auto_step",     "Step",      "Autoplay", false),

    B(ParamIdE::BRAIN2_ON,        "brain2_on",       "Active",    "Brain 2", false),
    I(ParamIdE::BRAIN2_DENSITY,   "brain2_density",  "Density",   "Brain 2", 1.f,   12.f,  3.f),
    F(ParamIdE::BRAIN2_RATE,      "brain2_rate",     "Event Rate","Brain 2", 2.f,   600.f, 60.f, 0.4f, "s"),
    F(ParamIdE::BRAIN2_HOLD_MIN,   "brain2_hold_min", "Hold Min",  "Brain 2", 2.f,   600.f, 60.f, 0.4f, "s"),
    F(ParamIdE::BRAIN2_HOLD_MAX,   "brain2_hold_max", "Hold Max",  "Brain 2", 5.f,   1200.f,240.f,0.4f, "s"),
    I(ParamIdE::BRAIN2_LOW,       "brain2_low",      "Lowest",    "Brain 2", 12.f,  84.f,  28.f),
    I(ParamIdE::BRAIN2_HIGH,      "brain2_high",     "Highest",   "Brain 2", 24.f,  108.f, 58.f),
    F(ParamIdE::BRAIN2_DEPTH,     "brain2_depth",    "Plane",     "Brain 2", 0.f,   1.f,   0.9f, 1.f,  ""),
    I(ParamIdE::BRAIN2_INTERVAL,  "brain2_interval", "Interval",  "Brain 2", -24.f, 24.f,  0.f,  "st"),
    F(ParamIdE::BRAIN2_CONSONANCE,"brain2_consonance","Consonance","Brain 2",0.f,   1.f,   0.7f, 1.f,  ""),

    C(ParamIdE::SCALE,     "scale",     "Scale",     "Tuning", ScaleNames, NumScaleChoices, 3),
    C(ParamIdE::KEY_MAP,    "keymap",    "Keys",      "Tuning", KeyMapNames, 2, 0),
    C(ParamIdE::ROOT_NOTE,  "root",      "Root",      "Tuning", RootNames, 12, 2),
    F(ParamIdE::REF_PITCH,  "ref_pitch", "A4",        "Tuning", 415.f, 466.f, 440.f, 1.f, "Hz"),
    I(ParamIdE::SEED,      "seed",      "Seed",      "Tuning", 0.f, 9999.f, 1.f),
    B(ParamIdE::HOLD,      "hold",      "Hold",      "Tuning", false),
    F(ParamIdE::TUNE_PURITY,    "purity",       "Purity",       "Tuning",     0.f,    1.f,   1.f,   1.f,  ""),
    F(ParamIdE::TUNE_DRIFT,     "purity_drift", "Purity Drift", "Tuning",     0.f,    1.f,   0.f,   1.f,  ""),
    F(ParamIdE::TUNE_DRIFT_RATE, "purity_rate",  "Drift Rate",   "Tuning",     0.002f, 0.1f,  0.01f, 0.5f, "Hz"),
    B(ParamIdE::FREEZE,        "freeze",       "Freeze",       "Strands"   , false),
    F(ParamIdE::TIDE,          "tide",         "Tide",         "Tuning",     0.f,   30.f,  0.f,   0.6f, "ct"),
    F(ParamIdE::TIDE_PERIOD,    "tide_period",  "Tide Period",  "Tuning",     1.f,   60.f,  12.f,  0.5f, "min"),
    C(ParamIdE::AIR_MODE,       "air_mode",     "Mode",         "Air",        AirModeNames, 2, 0),
    F(ParamIdE::PORTAMENTO,    "portamento",   "Portamento",   "Tuning",     0.f,   20.f,  0.f,   0.4f, "s"),
    F(ParamIdE::PORTA_GRAVITY,  "porta_gravity","Gravity",      "Tuning",     0.f,   1.f,   0.5f,  1.f,  ""),
    F(ParamIdE::FEEDBACK_TAPE,  "fb_tape",      "Tape",         "Feedback",   0.f,   1.f,   0.f,   1.f,  ""),
    F(ParamIdE::COHERENCE,     "coherence",    "Coherence",    "Coherence",  0.f,   1.f,   0.f,   1.f,  ""),
    F(ParamIdE::COHERENCE_DEPTH,"coherence_depth","Depth",      "Coherence",  0.f,   1.f,   0.f,   1.f,  ""),
    F(ParamIdE::COHERENCE_RATE, "coherence_rate","Rate",        "Coherence",  0.2f,  5.f,   1.f,   0.5f, "x"),
    F(ParamIdE::SYMPATHY,      "sympathy",     "Sympathy",     "Coherence",  0.f,   1.f,   0.f,  1.f,  ""),

    C(ParamIdE::LFO1_SHAPE, "lfo1_shape", "Shape", "LFO 1", LfoShapeNames, NumLfoShapes, 0),
    F(ParamIdE::LFO1_RATE,  "lfo1_rate",  "Rate",  "LFO 1", 0.0008f, 20.f, 0.0300f, 0.25f, "Hz"),
    F(ParamIdE::LFO1_PHASE, "lfo1_phase", "Phase", "LFO 1", 0.f, 1.f, 0.0f, 1.f, ""),
    F(ParamIdE::LFO1_DEPTH, "lfo1_depth", "Depth", "LFO 1", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamIdE::LFO1_MODE,  "lfo1_mode",  "Mode",  "LFO 1", LfoModeNames, NumLfoModes, 0),
    I(ParamIdE::LFO1_TABLE, "lfo1_table", "Table", "LFO 1", 0.f, 63.f, 0.f),
    C(ParamIdE::LFO1_SYNC, "lfo1_sync", "Sync", "LFO 1", SyncDivNames, NumSyncDivs, 0),
    C(ParamIdE::LFO2_SHAPE, "lfo2_shape", "Shape", "LFO 2", LfoShapeNames, NumLfoShapes, 0),
    F(ParamIdE::LFO2_RATE,  "lfo2_rate",  "Rate",  "LFO 2", 0.0008f, 20.f, 0.0485f, 0.25f, "Hz"),
    F(ParamIdE::LFO2_PHASE, "lfo2_phase", "Phase", "LFO 2", 0.f, 1.f, 0.125f, 1.f, ""),
    F(ParamIdE::LFO2_DEPTH, "lfo2_depth", "Depth", "LFO 2", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamIdE::LFO2_MODE,  "lfo2_mode",  "Mode",  "LFO 2", LfoModeNames, NumLfoModes, 0),
    I(ParamIdE::LFO2_TABLE, "lfo2_table", "Table", "LFO 2", 0.f, 63.f, 0.f),
    C(ParamIdE::LFO2_SYNC, "lfo2_sync", "Sync", "LFO 2", SyncDivNames, NumSyncDivs, 0),
    C(ParamIdE::LFO3_SHAPE, "lfo3_shape", "Shape", "LFO 3", LfoShapeNames, NumLfoShapes, 0),
    F(ParamIdE::LFO3_RATE,  "lfo3_rate",  "Rate",  "LFO 3", 0.0008f, 20.f, 0.0785f, 0.25f, "Hz"),
    F(ParamIdE::LFO3_PHASE, "lfo3_phase", "Phase", "LFO 3", 0.f, 1.f, 0.25f, 1.f, ""),
    F(ParamIdE::LFO3_DEPTH, "lfo3_depth", "Depth", "LFO 3", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamIdE::LFO3_MODE,  "lfo3_mode",  "Mode",  "LFO 3", LfoModeNames, NumLfoModes, 0),
    I(ParamIdE::LFO3_TABLE, "lfo3_table", "Table", "LFO 3", 0.f, 63.f, 0.f),
    C(ParamIdE::LFO3_SYNC, "lfo3_sync", "Sync", "LFO 3", SyncDivNames, NumSyncDivs, 0),
    C(ParamIdE::LFO4_SHAPE, "lfo4_shape", "Shape", "LFO 4", LfoShapeNames, NumLfoShapes, 0),
    F(ParamIdE::LFO4_RATE,  "lfo4_rate",  "Rate",  "LFO 4", 0.0008f, 20.f, 0.1271f, 0.25f, "Hz"),
    F(ParamIdE::LFO4_PHASE, "lfo4_phase", "Phase", "LFO 4", 0.f, 1.f, 0.375f, 1.f, ""),
    F(ParamIdE::LFO4_DEPTH, "lfo4_depth", "Depth", "LFO 4", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamIdE::LFO4_MODE,  "lfo4_mode",  "Mode",  "LFO 4", LfoModeNames, NumLfoModes, 0),
    I(ParamIdE::LFO4_TABLE, "lfo4_table", "Table", "LFO 4", 0.f, 63.f, 0.f),
    C(ParamIdE::LFO4_SYNC, "lfo4_sync", "Sync", "LFO 4", SyncDivNames, NumSyncDivs, 0),
    C(ParamIdE::LFO5_SHAPE, "lfo5_shape", "Shape", "LFO 5", LfoShapeNames, NumLfoShapes, 0),
    F(ParamIdE::LFO5_RATE,  "lfo5_rate",  "Rate",  "LFO 5", 0.0008f, 20.f, 0.2056f, 0.25f, "Hz"),
    F(ParamIdE::LFO5_PHASE, "lfo5_phase", "Phase", "LFO 5", 0.f, 1.f, 0.5f, 1.f, ""),
    F(ParamIdE::LFO5_DEPTH, "lfo5_depth", "Depth", "LFO 5", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamIdE::LFO5_MODE,  "lfo5_mode",  "Mode",  "LFO 5", LfoModeNames, NumLfoModes, 0),
    I(ParamIdE::LFO5_TABLE, "lfo5_table", "Table", "LFO 5", 0.f, 63.f, 0.f),
    C(ParamIdE::LFO5_SYNC, "lfo5_sync", "Sync", "LFO 5", SyncDivNames, NumSyncDivs, 0),
    C(ParamIdE::LFO6_SHAPE, "lfo6_shape", "Shape", "LFO 6", LfoShapeNames, NumLfoShapes, 0),
    F(ParamIdE::LFO6_RATE,  "lfo6_rate",  "Rate",  "LFO 6", 0.0008f, 20.f, 0.3327f, 0.25f, "Hz"),
    F(ParamIdE::LFO6_PHASE, "lfo6_phase", "Phase", "LFO 6", 0.f, 1.f, 0.625f, 1.f, ""),
    F(ParamIdE::LFO6_DEPTH, "lfo6_depth", "Depth", "LFO 6", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamIdE::LFO6_MODE,  "lfo6_mode",  "Mode",  "LFO 6", LfoModeNames, NumLfoModes, 0),
    I(ParamIdE::LFO6_TABLE, "lfo6_table", "Table", "LFO 6", 0.f, 63.f, 0.f),
    C(ParamIdE::LFO6_SYNC, "lfo6_sync", "Sync", "LFO 6", SyncDivNames, NumSyncDivs, 0),
    C(ParamIdE::LFO7_SHAPE, "lfo7_shape", "Shape", "LFO 7", LfoShapeNames, NumLfoShapes, 0),
    F(ParamIdE::LFO7_RATE,  "lfo7_rate",  "Rate",  "LFO 7", 0.0008f, 20.f, 0.5383f, 0.25f, "Hz"),
    F(ParamIdE::LFO7_PHASE, "lfo7_phase", "Phase", "LFO 7", 0.f, 1.f, 0.75f, 1.f, ""),
    F(ParamIdE::LFO7_DEPTH, "lfo7_depth", "Depth", "LFO 7", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamIdE::LFO7_MODE,  "lfo7_mode",  "Mode",  "LFO 7", LfoModeNames, NumLfoModes, 0),
    I(ParamIdE::LFO7_TABLE, "lfo7_table", "Table", "LFO 7", 0.f, 63.f, 0.f),
    C(ParamIdE::LFO7_SYNC, "lfo7_sync", "Sync", "LFO 7", SyncDivNames, NumSyncDivs, 0),
    C(ParamIdE::LFO8_SHAPE, "lfo8_shape", "Shape", "LFO 8", LfoShapeNames, NumLfoShapes, 0),
    F(ParamIdE::LFO8_RATE,  "lfo8_rate",  "Rate",  "LFO 8", 0.0008f, 20.f, 0.8710f, 0.25f, "Hz"),
    F(ParamIdE::LFO8_PHASE, "lfo8_phase", "Phase", "LFO 8", 0.f, 1.f, 0.875f, 1.f, ""),
    F(ParamIdE::LFO8_DEPTH, "lfo8_depth", "Depth", "LFO 8", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamIdE::LFO8_MODE,  "lfo8_mode",  "Mode",  "LFO 8", LfoModeNames, NumLfoModes, 0),
    I(ParamIdE::LFO8_TABLE, "lfo8_table", "Table", "LFO 8", 0.f, 63.f, 0.f),
    C(ParamIdE::LFO8_SYNC, "lfo8_sync", "Sync", "LFO 8", SyncDivNames, NumSyncDivs, 0),
    C(ParamIdE::ENV1_MODE,  "env1_mode",  "Mode",  "Env 1", EnvModeNames, NumEnvModes, 0),
    F(ParamIdE::ENV1_TIME,  "env1_time",  "Time",  "Env 1", 0.05f, 20.f, 1.f, 0.4f, "x"),
    F(ParamIdE::ENV1_DEPTH, "env1_depth", "Depth", "Env 1", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamIdE::ENV1_SYNC, "env1_sync", "Sync", "Env 1", SyncDivNames, NumSyncDivs, 0),
    C(ParamIdE::ENV2_MODE,  "env2_mode",  "Mode",  "Env 2", EnvModeNames, NumEnvModes, 0),
    F(ParamIdE::ENV2_TIME,  "env2_time",  "Time",  "Env 2", 0.05f, 20.f, 1.f, 0.4f, "x"),
    F(ParamIdE::ENV2_DEPTH, "env2_depth", "Depth", "Env 2", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamIdE::ENV2_SYNC, "env2_sync", "Sync", "Env 2", SyncDivNames, NumSyncDivs, 0),
    C(ParamIdE::ENV3_MODE,  "env3_mode",  "Mode",  "Env 3", EnvModeNames, NumEnvModes, 0),
    F(ParamIdE::ENV3_TIME,  "env3_time",  "Time",  "Env 3", 0.05f, 20.f, 1.f, 0.4f, "x"),
    F(ParamIdE::ENV3_DEPTH, "env3_depth", "Depth", "Env 3", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamIdE::ENV3_SYNC, "env3_sync", "Sync", "Env 3", SyncDivNames, NumSyncDivs, 0),
    C(ParamIdE::ENV4_MODE,  "env4_mode",  "Mode",  "Env 4", EnvModeNames, NumEnvModes, 0),
    F(ParamIdE::ENV4_TIME,  "env4_time",  "Time",  "Env 4", 0.05f, 20.f, 1.f, 0.4f, "x"),
    F(ParamIdE::ENV4_DEPTH, "env4_depth", "Depth", "Env 4", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamIdE::ENV4_SYNC, "env4_sync", "Sync", "Env 4", SyncDivNames, NumSyncDivs, 0),
    C(ParamIdE::ENV5_MODE,  "env5_mode",  "Mode",  "Env 5", EnvModeNames, NumEnvModes, 0),
    F(ParamIdE::ENV5_TIME,  "env5_time",  "Time",  "Env 5", 0.05f, 20.f, 1.f, 0.4f, "x"),
    F(ParamIdE::ENV5_DEPTH, "env5_depth", "Depth", "Env 5", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamIdE::ENV5_SYNC, "env5_sync", "Sync", "Env 5", SyncDivNames, NumSyncDivs, 0),
    C(ParamIdE::ENV6_MODE,  "env6_mode",  "Mode",  "Env 6", EnvModeNames, NumEnvModes, 0),
    F(ParamIdE::ENV6_TIME,  "env6_time",  "Time",  "Env 6", 0.05f, 20.f, 1.f, 0.4f, "x"),
    F(ParamIdE::ENV6_DEPTH, "env6_depth", "Depth", "Env 6", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamIdE::ENV6_SYNC, "env6_sync", "Sync", "Env 6", SyncDivNames, NumSyncDivs, 0),

    B(ParamIdE::MORPH_ACTIVE, "morph_active", "Active",   "Morph", false),
    F(ParamIdE::MORPH_POS,    "morph",        "Position", "Morph", 0.f, 1.f,   0.f,  1.f,  ""),
    F(ParamIdE::MORPH_GLIDE,  "morph_glide",  "Glide",    "Morph", 0.f, 900.f, 30.f, 0.4f, "s"),

    F(ParamIdE::MACRO_A, "macro_a", "Space",  "Macros", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::MACRO_B, "macro_b", "Alien",  "Macros", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::MACRO_C, "macro_c", "Motion", "Macros", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::MACRO_D, "macro_d", "Bloom",  "Macros", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::MACRO_E, "macro_e", "Density",   "Macros", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::MACRO_F, "macro_f", "Distance",  "Macros", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::MACRO_G, "macro_g", "Evolution", "Macros", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::MACRO_H, "macro_h", "Air",       "Macros", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::INERTIA, "inertia", "Inertia",  "Macros", 0.f, 5.f, 0.f, 0.5f, "s"),

    B(ParamIdE::MAP_ACTIVE, "map_active", "Active", "Map", false),
    F(ParamIdE::MAP_X,      "map_x",      "X",      "Map", 0.f,   1.f,  0.5f,  1.f, ""),
    F(ParamIdE::MAP_Y,      "map_y",      "Y",      "Map", 0.f,   1.f,  0.5f,  1.f, ""),
    F(ParamIdE::MAP_RADIUS, "map_radius", "Radius", "Map", 0.02f, 0.4f, 0.08f, 1.f, ""),

    B(ParamIdE::ROUTE_ACTIVE, "route_active", "Play",  "Route", false),
    F(ParamIdE::ROUTE_SPEED,  "route_speed",  "Speed", "Route", 0.25f, 4.f, 1.f, 0.5f, "x"),
    B(ParamIdE::ROUTE_LOOP,   "route_loop",   "Loop",  "Route", true),

    C(ParamIdE::CLOCK_SOURCE, "clock_source", "Source", "Clock", ClockSourceNames, NumClockSources, 0),
    F(ParamIdE::TEMPO,       "tempo",        "Tempo",  "Clock", 20.f, 300.f, 90.f, 0.6f, "bpm"),
    B(ParamIdE::CLOCK_RUN,    "clock_run",    "Run",    "Clock", true),

    // ---- appended later; see the note in Params.h on why they are at the end
    F(ParamIdE::FAR_LOWCUT,   "far_lowcut",   "Low Cut", "Far Reverb",  20.f, 800.f, 20.f, 0.35f, "Hz"),
    F(ParamIdE::NEAR_LOWCUT,  "near_lowcut",  "Low Cut", "Near Reverb", 20.f, 800.f, 20.f, 0.35f, "Hz"),
    F(ParamIdE::ROOM_LOWCUT,  "room_lowcut",  "Low Cut", "Room",        20.f, 800.f, 20.f, 0.35f, "Hz"),
    F(ParamIdE::SUBSONIC,    "subsonic",     "Subsonic", "Master",      0.f,  40.f,  0.f, 1.f,   "Hz"),
    F(ParamIdE::VEC_AMOUNT,   "vec_amount",   "Amount",  "Vector", 0.f, 1.f, 0.f,   1.f, ""),
    F(ParamIdE::VEC_X,        "vec_x",        "X",       "Vector", 0.f, 1.f, 0.5f,  1.f, ""),
    F(ParamIdE::VEC_Y,        "vec_y",        "Y",       "Vector", 0.f, 1.f, 0.5f,  1.f, ""),
    F(ParamIdE::VEC_WANDER,   "vec_wander",   "Wander",  "Vector", 0.f, 1.f, 0.f,   1.f, ""),
    F(ParamIdE::VEC_RATE,     "vec_rate",     "Rate",    "Vector", 0.002f, 0.5f, 0.02f, 0.35f, "Hz"),
    // ---- Source 4, the same slot as 2 and 3 field for field (the self test checks that)
    C(ParamIdE::SRC4_TYPE,     "src4_type",     "Type",      "Source 4", SourceTypeNames, NumSourceTypes, 0),
    F(ParamIdE::SRC4_LEVEL,    "src4_level",    "Level",     "Source 4", 0.f,   1.f,    0.5f,   1.f,  ""),
    I(ParamIdE::SRC4_OCTAVE,   "src4_octave",   "Octave",    "Source 4", -2.f,  2.f,    0.f),
    C(ParamIdE::SRC4_RATIO,    "src4_ratio",    "Ratio",     "Source 4", SlotRatioNames, NumSlotRatios, 0),
    F(ParamIdE::SRC4_PAN,      "src4_pan",      "Pan",       "Source 4", -1.f,  1.f,    0.3f,   1.f,  ""),
    C(ParamIdE::SRC4_TABLE,    "src4_table",    "Table",     "Source 4", TableNames, NumTables, 0),
    F(ParamIdE::SRC4_POSITION, "src4_pos",      "Position",  "Source 4", 0.f,   1.f,    0.3f,   1.f,  ""),
    F(ParamIdE::SRC4_POS_DRIFT, "src4_pos_drift","Pos Drift", "Source 4", 0.f,   1.f,    0.3f,   1.f,  ""),
    F(ParamIdE::SRC4_FM_RATIO,  "src4_fm_ratio", "FM Ratio",  "Source 4", 0.25f, 8.f,    2.f,    0.5f, "x"),
    F(ParamIdE::SRC4_FM_INDEX,  "src4_fm_index", "FM Index",  "Source 4", 0.f,   8.f,    1.f,    0.6f, ""),
    F(ParamIdE::SRC4_GRAIN,    "src4_grain",    "Grain",     "Source 4", 30.f,  1000.f, 200.f,  0.5f, "ms"),
    F(ParamIdE::SRC4_DENSITY,  "src4_density",  "Density",   "Source 4", 1.f,   200.f,  12.f,   0.4f, "/s"),
    C(ParamIdE::SRC4_DENSITY_SYNC, "src4_density_sync", "Sync", "Source 4", SyncDivNames, NumSyncDivs, 0),
    C(ParamIdE::SRC4_FOLLOW,   "src4_follow",   "Pitch",     "Source 4", FollowNames, 2, 0),
    I(ParamIdE::SRC4_GRAINS,   "src4_grains",   "Grains",    "Source 4", 1.f,   128.f,  16.f),
    F(ParamIdE::SRC4_SPREAD,   "src4_spread",   "Spread",    "Source 4", 0.f,   1.f,    0.03f,  0.5f, ""),
    C(ParamIdE::SRC4_NOISE,    "src4_noise",    "Noise",     "Source 4", NoiseKindNames, NumNoiseKinds, 1),
    F(ParamIdE::SRC4_NOISE_Q,   "src4_noise_q",  "Noise Q",   "Source 4", 0.f,   1.f,    0.4f,   1.f,  ""),
    I(ParamIdE::SRC4_PARTIALS,    "src4_partials",     "Partials",     "Source 4", 1.f,   32.f,   16.f),
    F(ParamIdE::SRC4_TILT,        "src4_tilt",         "Tilt",         "Source 4", 0.3f,  3.f,    1.2f,   1.f,  ""),
    F(ParamIdE::SRC4_BRIGHT,      "src4_bright",       "Bright",       "Source 4", 0.f,   1.f,    0.7f,   1.f,  ""),
    F(ParamIdE::SRC4_ODD_EVEN,     "src4_odd_even",     "Odd / Even",   "Source 4", -1.f,  1.f,    0.f,    1.f,  ""),
    F(ParamIdE::SRC4_INHARM,      "src4_inharmonic",   "Inharmonic",   "Source 4", 0.f,   1.f,    0.f,    1.f,  ""),
    F(ParamIdE::SRC4_SHIMMER,     "src4_shimmer",      "Shimmer",      "Source 4", 0.f,   1.f,    0.4f,   1.f,  ""),
    F(ParamIdE::SRC4_SHIMMER_RATE, "src4_shimmer_rate", "Shimmer Rate", "Source 4", 0.01f, 2.f,    0.15f,  0.4f, "Hz"),
    F(ParamIdE::SRC4_DRIFT,       "src4_drift",        "Drift",        "Source 4", 0.f,   30.f,   0.f,    0.6f, "ct"),
    // ---- Stretch: factor and loop crossfade, one pair per slot
    F(ParamIdE::SRC1_STRETCH, "src1_stretch", "Stretch",   "Source 1", 1.f, 1000.f, 40.f, 0.25f, "x"),
    F(ParamIdE::SRC1_XFADE,   "src1_xfade",   "Loop Fade", "Source 1", 0.f, 1.f,    0.1f, 1.f,   ""),
    F(ParamIdE::SRC2_STRETCH, "src2_stretch", "Stretch",   "Source 2", 1.f, 1000.f, 40.f, 0.25f, "x"),
    F(ParamIdE::SRC2_XFADE,   "src2_xfade",   "Loop Fade", "Source 2", 0.f, 1.f,    0.1f, 1.f,   ""),
    F(ParamIdE::SRC3_STRETCH, "src3_stretch", "Stretch",   "Source 3", 1.f, 1000.f, 40.f, 0.25f, "x"),
    F(ParamIdE::SRC3_XFADE,   "src3_xfade",   "Loop Fade", "Source 3", 0.f, 1.f,    0.1f, 1.f,   ""),
    F(ParamIdE::SRC4_STRETCH, "src4_stretch", "Stretch",   "Source 4", 1.f, 1000.f, 40.f, 0.25f, "x"),
    F(ParamIdE::SRC4_XFADE,   "src4_xfade",   "Loop Fade", "Source 4", 0.f, 1.f,    0.1f, 1.f,   ""),
    // ---- the mixing desk's four, all neutral at their defaults
    F(ParamIdE::FAR_WIDTH,   "far_width",   "Width",     "Far Reverb", 0.f, 1.5f,  1.f,  1.f,  ""),
    C(ParamIdE::ENS_MODE,    "ens_mode",    "Mode",      "Ensemble",   EnsModeNames, 3, 0),
    F(ParamIdE::HAAS,       "haas",        "Haas",      "Space",      0.f, 1.f,   0.f,  1.f,  ""),
    F(ParamIdE::HAAS_TIME,   "haas_time",   "Haas Time", "Space",      6.f, 28.f,  15.f, 1.f,  "ms"),
    F(ParamIdE::FILTER_FOLD, "filter_fold", "Fold",      "Filter",     0.f, 1.f,   0.f,  1.f,  ""),
    // ---- after the classics: all neutral at their defaults
    F(ParamIdE::TUNE_STRETCH,     "stretch",           "Stretch",  "Tuning",     0.f, 30.f, 0.f, 1.f, "ct"),
    C(ParamIdE::FAR_MODE,         "far_mode",          "Mode",     "Far Reverb", FarModeNames, 4, 0),
    F(ParamIdE::FAR_UNMASK_SPREAD, "far_unmask_spread", "Spread",   "Far Reverb", 0.f, 1.f,  0.f, 1.f, ""),
    C(ParamIdE::BINAURAL,        "binaural",          "Binaural", "Space",      BinauralNames, 2, 0),
    F(ParamIdE::BRAIN_TIMBRE,     "brain_timbre",      "Timbre",   "Cluster Brain", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::BRAIN_SPACING,    "brain_spacing",     "Spacing",  "Cluster Brain", -1.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::EARLY_LEVEL,      "early_level",       "Early",    "Early Room", 0.f,  1.f,  0.f,  1.f, ""),
    F(ParamIdE::EARLY_SIZE,       "early_size",        "Size",     "Early Room", 2.f,  40.f, 8.f,  0.5f, "m"),
    F(ParamIdE::EARLY_ABSORB,     "early_absorb",      "Absorb",   "Early Room", 0.f,  1.f,  0.35f, 1.f, ""),
    F(ParamIdE::EARLY_WIDTH,      "early_width",       "Width",    "Early Room", 0.f,  1.5f, 1.f,  1.f, ""),
    // ---- Bow: the pair per slot
    F(ParamIdE::SRC1_BOW_FORCE, "src1_bow_force", "Force", "Source 1", 0.f, 1.f, 0.4f, 1.f, ""),
    F(ParamIdE::SRC1_BOW_SPEED, "src1_bow_speed", "Speed", "Source 1", 0.f, 1.f, 0.3f, 1.f, ""),
    F(ParamIdE::SRC2_BOW_FORCE, "src2_bow_force", "Force", "Source 2", 0.f, 1.f, 0.4f, 1.f, ""),
    F(ParamIdE::SRC2_BOW_SPEED, "src2_bow_speed", "Speed", "Source 2", 0.f, 1.f, 0.3f, 1.f, ""),
    F(ParamIdE::SRC3_BOW_FORCE, "src3_bow_force", "Force", "Source 3", 0.f, 1.f, 0.4f, 1.f, ""),
    F(ParamIdE::SRC3_BOW_SPEED, "src3_bow_speed", "Speed", "Source 3", 0.f, 1.f, 0.3f, 1.f, ""),
    F(ParamIdE::SRC4_BOW_FORCE, "src4_bow_force", "Force", "Source 4", 0.f, 1.f, 0.4f, 1.f, ""),
    F(ParamIdE::SRC4_BOW_SPEED, "src4_bow_speed", "Speed", "Source 4", 0.f, 1.f, 0.3f, 1.f, ""),
    F(ParamIdE::SRC1_SPEC_RATE,   "src1_spec_rate",   "Rate",   "Source 1", 0.f, 4.f, 1.f, 0.6f, "x"),
    F(ParamIdE::SRC1_SPEC_BREATH, "src1_spec_breath", "Breath", "Source 1", -1.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::SRC2_SPEC_RATE,   "src2_spec_rate",   "Rate",   "Source 2", 0.f, 4.f, 1.f, 0.6f, "x"),
    F(ParamIdE::SRC2_SPEC_BREATH, "src2_spec_breath", "Breath", "Source 2", -1.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::SRC3_SPEC_RATE,   "src3_spec_rate",   "Rate",   "Source 3", 0.f, 4.f, 1.f, 0.6f, "x"),
    F(ParamIdE::SRC3_SPEC_BREATH, "src3_spec_breath", "Breath", "Source 3", -1.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::SRC4_SPEC_RATE,   "src4_spec_rate",   "Rate",   "Source 4", 0.f, 4.f, 1.f, 0.6f, "x"),
    F(ParamIdE::SRC4_SPEC_BREATH, "src4_spec_breath", "Breath", "Source 4", -1.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::BRAIN_HARMONIC,  "brain_harmonic",  "Harmonic", "Cluster Brain", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::BRAIN_KEY,       "brain_key",       "Key",      "Cluster Brain", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::BRAIN_EVEN,      "brain_even",      "Even",     "Cluster Brain", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::BRAIN_SMOOTH,    "brain_smooth",    "Smooth",   "Cluster Brain", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::TUNE_GUARD,      "purity_guard",    "Guard",    "Tuning",        -1.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::TUNE_MATCH,      "match",           "Match",    "Tuning",        0.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::BRAIN_BLEND,     "brain_blend",     "Blend",    "Cluster Brain", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::ARC_HARMONY,     "arc_harmony",     "Arc Harmony", "Space",      0.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::ELEV_NEAR,       "elev_near",       "Elev Near",   "Space",      -1.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::ELEV_FAR,        "elev_far",        "Elev Far",    "Space",      -1.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::DEPTH_LAW,       "depth_law",       "Depth Law",   "Space",      0.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::ENVELOP,        "far_envelop",     "Envelop",     "Far Reverb", 0.f, 1.f, 0.f, 1.f, ""),
    B(ParamIdE::ARC_CLOCK,       "arc_clock",       "Arc Clock",   "Space",      false),
    F(ParamIdE::BRAIN_CASCADE,   "brain_cascade",   "Cascade",     "Cluster Brain", 0.f, 1.f, 0.f,  1.f, ""),
    F(ParamIdE::BRAIN_SURPRISE,  "brain_surprise",  "Surprise",    "Cluster Brain", 0.f, 1.f, 0.5f, 1.f, ""),
    F(ParamIdE::BRAIN_HOMEOSTAT, "brain_homeostat", "Homeostat",   "Cluster Brain", 0.f, 1.f, 0.f,  1.f, ""),
    F(ParamIdE::TUNE_ADAPT,      "purity_adapt",    "Adaptive",    "Tuning",        0.f, 1.f, 0.f,  1.f, ""),
    F(ParamIdE::FAR_COMOD,       "far_comod",       "Comodulate",  "Far Reverb",    0.f, 1.f, 0.f,  1.f, ""),
    F(ParamIdE::NEAR_ILD,        "near_ild",        "Near Field",  "Space",         0.f, 1.f, 0.f,  1.f, ""),
    F(ParamIdE::SRC1_TRANSPORT,  "src1_transport",  "Transport",   "Source 1",      0.f, 1.f, 0.f,  1.f, ""),
    F(ParamIdE::SRC2_TRANSPORT,  "src2_transport",  "Transport",   "Source 2",      0.f, 1.f, 0.f,  1.f, ""),
    F(ParamIdE::SRC3_TRANSPORT,  "src3_transport",  "Transport",   "Source 3",      0.f, 1.f, 0.f,  1.f, ""),
    F(ParamIdE::SRC4_TRANSPORT,  "src4_transport",  "Transport",   "Source 4",      0.f, 1.f, 0.f,  1.f, ""),
    F(ParamIdE::SUB_PULSE,       "sub_pulse",       "Pulse",       "Foundation",    0.f, 1.f, 0.f,  1.f, ""),
    F(ParamIdE::FEEDBACK_BIAS,   "fb_bias",         "Bias",        "Feedback",      0.f, 1.f, 0.f,  1.f, ""),
    F(ParamIdE::LENIA_RATE,      "lenia_rate",      "Lenia Rate",  "Coherence",     0.5f, 20.f, 4.f, 0.5f, "Hz"),
    F(ParamIdE::LENIA_GROWTH,    "lenia_growth",    "Lenia Growth","Coherence",     0.08f, 0.3f, 0.15f, 1.f, ""),
    F(ParamIdE::BRAIN_DEJA_VU,    "brain_dejavu",    "Deja Vu",     "Cluster Brain", 0.f, 1.f, 0.f,  1.f, ""),
    F(ParamIdE::BRAIN_LOOP,      "brain_loop",      "Loop",        "Cluster Brain", 1.f, 16.f, 8.f, 1.f, ""),
    F(ParamIdE::BRAIN_SPREAD,    "brain_spread",    "Spread",      "Cluster Brain", 0.f, 1.f, 0.5f, 1.f, ""),
    F(ParamIdE::BRAIN_BIAS,      "brain_bias",      "Bias",        "Cluster Brain", -1.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::CHAOS_PERIOD,    "chaos_period",    "Chaos Period","Coherence",     10.f, 1800.f, 120.f, 0.4f, "s"),
    C(ParamIdE::KEYS_FILTER,     "keys_filter",     "MPE Filter",  "Space",         KeysFilterNames, 2, 0),
    C(ParamIdE::TRANSPOSE,      "transpose",       "Transpose",   "Tuning",        TransposeNames, 7, 0),
    F(ParamIdE::PARTIAL_SPREAD,  "partial_spread",  "Partial Spread", "Strands",    0.f, 1.f, 0.f,  1.f, ""),
    // How often the conductor's notes strike, and whether those strikes cluster with its cascade.
    // Chance 1 is what the instrument always did: every note that may strike, strikes.
    F(ParamIdE::STRIKE_CHANCE,   "strike_chance",   "Chance",      "Strike",        0.f, 1.f, 1.f,  1.f, ""),
    F(ParamIdE::STRIKE_CLUSTER,  "strike_cluster",  "Cluster",     "Strike",        0.f, 1.f, 0.f,  1.f, ""),
    // The Cosmos breathing with the conductor's cascade. On by default: without a cascade the
    // excitation is zero and this changes nothing at all, and where there is one it is what the
    // section wanted anyway -- a parallel world that comes and goes rather than a constant hum.
    F(ParamIdE::COSMOS_SWELL,    "cosmos_swell",    "Swell",       "Cosmos",        0.f, 1.f, 0.5f, 1.f, ""),
    // Each slot's own entrance, per note. Delay 0 and Env Off is what every preset made before
    // these existed, so they are the defaults and nothing already written changes. Delay reaches
    // 30 s because the library's attacks do: a texture that arrives half a minute into a held
    // chord is a second instrument entering, not a slow fade.
    F(ParamIdE::SRC1_DELAY, "src1_delay", "Delay", "Source 1", 0.f, 30.f, 0.f, 0.4f, "s"),
    F(ParamIdE::SRC1_RISE,  "src1_rise",  "Rise",  "Source 1", 0.01f, 30.f, 1.f, 0.4f, "s"),
    C(ParamIdE::SRC1_ENV,   "src1_env",   "Env",   "Source 1", SlotEnvNames, NumSlotEnvs, 0),
    F(ParamIdE::SRC2_DELAY, "src2_delay", "Delay", "Source 2", 0.f, 30.f, 0.f, 0.4f, "s"),
    F(ParamIdE::SRC2_RISE,  "src2_rise",  "Rise",  "Source 2", 0.01f, 30.f, 1.f, 0.4f, "s"),
    C(ParamIdE::SRC2_ENV,   "src2_env",   "Env",   "Source 2", SlotEnvNames, NumSlotEnvs, 0),
    F(ParamIdE::SRC3_DELAY, "src3_delay", "Delay", "Source 3", 0.f, 30.f, 0.f, 0.4f, "s"),
    F(ParamIdE::SRC3_RISE,  "src3_rise",  "Rise",  "Source 3", 0.01f, 30.f, 1.f, 0.4f, "s"),
    C(ParamIdE::SRC3_ENV,   "src3_env",   "Env",   "Source 3", SlotEnvNames, NumSlotEnvs, 0),
    F(ParamIdE::SRC4_DELAY, "src4_delay", "Delay", "Source 4", 0.f, 30.f, 0.f, 0.4f, "s"),
    F(ParamIdE::SRC4_RISE,  "src4_rise",  "Rise",  "Source 4", 0.01f, 30.f, 1.f, 0.4f, "s"),
    C(ParamIdE::SRC4_ENV,   "src4_env",   "Env",   "Source 4", SlotEnvNames, NumSlotEnvs, 0),
    C(ParamIdE::SRC1_INTERP, "src1_interp", "Interp", "Source 1", InterpNames, NumInterp, 0),
    C(ParamIdE::SRC2_INTERP, "src2_interp", "Interp", "Source 2", InterpNames, NumInterp, 0),
    C(ParamIdE::SRC3_INTERP, "src3_interp", "Interp", "Source 3", InterpNames, NumInterp, 0),
    C(ParamIdE::SRC4_INTERP, "src4_interp", "Interp", "Source 4", InterpNames, NumInterp, 0),
    F(ParamIdE::SRC1_ROOT, "src1_root", "Root", "Source 1", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::SRC2_ROOT, "src2_root", "Root", "Source 2", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::SRC3_ROOT, "src3_root", "Root", "Source 3", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::SRC4_ROOT, "src4_root", "Root", "Source 4", 0.f, 1.f, 0.f, 1.f, ""),
    I(ParamIdE::SRC1_UNISON,    "src1_unison",     "Unison",  "Source 1", 1.f, 4.f, 1.f),
    F(ParamIdE::SRC1_UNI_DETUNE, "src1_uni_detune", "Uni Det", "Source 1", 0.f, 40.f, 10.f, 1.f, "ct"),
    F(ParamIdE::SRC1_UNI_WIDTH,  "src1_uni_width",  "Uni Wide","Source 1", 0.f, 1.f, 0.6f, 1.f, ""),
    I(ParamIdE::SRC2_UNISON,    "src2_unison",     "Unison",  "Source 2", 1.f, 4.f, 1.f),
    F(ParamIdE::SRC2_UNI_DETUNE, "src2_uni_detune", "Uni Det", "Source 2", 0.f, 40.f, 10.f, 1.f, "ct"),
    F(ParamIdE::SRC2_UNI_WIDTH,  "src2_uni_width",  "Uni Wide","Source 2", 0.f, 1.f, 0.6f, 1.f, ""),
    I(ParamIdE::SRC3_UNISON,    "src3_unison",     "Unison",  "Source 3", 1.f, 4.f, 1.f),
    F(ParamIdE::SRC3_UNI_DETUNE, "src3_uni_detune", "Uni Det", "Source 3", 0.f, 40.f, 10.f, 1.f, "ct"),
    F(ParamIdE::SRC3_UNI_WIDTH,  "src3_uni_width",  "Uni Wide","Source 3", 0.f, 1.f, 0.6f, 1.f, ""),
    I(ParamIdE::SRC4_UNISON,    "src4_unison",     "Unison",  "Source 4", 1.f, 4.f, 1.f),
    F(ParamIdE::SRC4_UNI_DETUNE, "src4_uni_detune", "Uni Det", "Source 4", 0.f, 40.f, 10.f, 1.f, "ct"),
    F(ParamIdE::SRC4_UNI_WIDTH,  "src4_uni_width",  "Uni Wide","Source 4", 0.f, 1.f, 0.6f, 1.f, ""),
    F(ParamIdE::CLOUD_FEEDBACK,  "cloud_feedback",  "Feedback",  "Cloud", 0.f,   1.f,     0.f,    1.f,  ""),
    F(ParamIdE::CLOUD_TONE,      "cloud_tone",      "Tone",      "Cloud", 300.f, 16000.f, 5000.f, 0.3f, "Hz"),
    I(ParamIdE::CLOUD_TRANSPOSE, "cloud_transpose", "Transpose", "Cloud", -24.f, 24.f,    0.f,    "st"),
    F(ParamIdE::CLOUD_SCATTER,   "cloud_scatter",   "Scatter",   "Cloud", 0.f,   1.f,     0.f,    1.f,  ""),
    F(ParamIdE::CLOUD_SWARM,     "cloud_swarm",     "Swarm",     "Cloud", 0.f,   1.f,     0.f,    1.f,  ""),
    F(ParamIdE::CLOUD_RESONANCE, "cloud_resonance", "Resonance", "Cloud", 0.f,   1.f,     0.f,    1.f,  ""),
    C(ParamIdE::CLOUD_RES_MODE,   "cloud_res_mode",  "Res Mode",  "Cloud", CloudResModeNames, 2, 0),
    C(ParamIdE::CLOUD_RES_NOTES,  "cloud_res_notes", "Res Notes", "Cloud", CloudResNoteNames, 4, 1),
    F(ParamIdE::CLOUD_RES_DECAY,  "cloud_res_decay", "Ring",      "Cloud", 0.2f,  12.f,    2.5f,   0.4f, "s"),
    F(ParamIdE::MEM_SEND,    "mem_send",    "Send",       "Memory", 0.f,  1.f,   0.f,   1.f,  ""),
    F(ParamIdE::MEM_RETURN,  "mem_return",  "Return",     "Memory", 0.f,  1.f,   0.5f,  1.f,  ""),
    F(ParamIdE::MEM_TO_FAR,   "mem_to_far",  "To Far",     "Memory", 0.f,  1.f,   0.4f,  1.f,  ""),
    C(ParamIdE::MEM_LINES,   "mem_lines",   "Lines",      "Memory", MemLineNames, 3, 1),
    F(ParamIdE::MEM_SIZE,    "mem_size",    "Size",       "Memory", 1.f,  40.f,  9.f,   0.4f, "s"),
    F(ParamIdE::MEM_BLUR,    "mem_blur",    "Blur",       "Memory", 0.f,  1.f,   0.25f, 1.f,  ""),
    F(ParamIdE::MEM_DRIFT,   "mem_drift",   "Drift",      "Memory", 0.f,  1.f,   0.3f,  1.f,  ""),
    F(ParamIdE::MEM_HOLD,    "mem_hold",    "Hold",       "Memory", 0.f,  1.f,   0.8f,  1.f,  ""),
    F(ParamIdE::MEM_AGE,     "mem_age",     "Age",        "Memory", 0.f,  1.f,   0.3f,  1.f,  ""),
    F(ParamIdE::MEM_RENEW,   "mem_renew",   "Renew",      "Memory", 0.f,  1.f,   0.f,   1.f,  ""),
    F(ParamIdE::MEM_DRIVE,   "mem_drive",   "Drive",      "Memory", 0.f,  1.f,   0.f,   1.f,  ""),
    F(ParamIdE::MEM_RECALL,  "mem_recall",  "Recall",     "Memory", 0.f,  1.f,   0.f,   1.f,  ""),
    F(ParamIdE::MEM_SEEK,    "mem_seek",    "Seek",       "Memory", 0.f,  1.f,   0.5f,  1.f,  ""),
    F(ParamIdE::MEM_GRAIN,   "mem_grain",   "Grain",      "Memory", 40.f, 800.f, 220.f, 0.5f, "ms"),
    B(ParamIdE::MEM_FREEZE,  "mem_freeze",  "Freeze",     "Memory", false),
    B(ParamIdE::MEM_REVERSE, "mem_reverse", "Reverse",    "Memory", false),
    B(ParamIdE::MEM_HALF,    "mem_half",    "Half Speed", "Memory", false),
    B(ParamIdE::MEM_ERASE,   "mem_erase",   "Erase",      "Memory", false),
    C(ParamIdE::COSMOS_SHIMMER_MODE, "cosmos_shimmer_mode", "Shimmer Mode", "Cosmos", ShimmerModeNames, 2, 0),
    C(ParamIdE::CLOUD_SHIFT,        "cloud_shift",         "FB Shift",     "Cloud",  CloudShiftNames, 7, 0),
    // Each source's own envelope, for a slot whose Env is Own (Voice.cpp). Depth 1 is the whole
    // contour; 0 leaves the source at its written level whatever the shape says.
    C(ParamIdE::SRC1_ENV_MODE,  "src1_env_mode",  "Mode",  "Src Env 1", EnvModeNames, NumEnvModes, 0),
    F(ParamIdE::SRC1_ENV_TIME,  "src1_env_time",  "Time",  "Src Env 1", 0.05f, 20.f, 1.f, 0.4f, "x"),
    F(ParamIdE::SRC1_ENV_DEPTH, "src1_env_depth", "Depth", "Src Env 1", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamIdE::SRC1_ENV_SYNC,  "src1_env_sync",  "Sync",  "Src Env 1", SyncDivNames, NumSyncDivs, 0),
    C(ParamIdE::SRC2_ENV_MODE,  "src2_env_mode",  "Mode",  "Src Env 2", EnvModeNames, NumEnvModes, 0),
    F(ParamIdE::SRC2_ENV_TIME,  "src2_env_time",  "Time",  "Src Env 2", 0.05f, 20.f, 1.f, 0.4f, "x"),
    F(ParamIdE::SRC2_ENV_DEPTH, "src2_env_depth", "Depth", "Src Env 2", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamIdE::SRC2_ENV_SYNC,  "src2_env_sync",  "Sync",  "Src Env 2", SyncDivNames, NumSyncDivs, 0),
    C(ParamIdE::SRC3_ENV_MODE,  "src3_env_mode",  "Mode",  "Src Env 3", EnvModeNames, NumEnvModes, 0),
    F(ParamIdE::SRC3_ENV_TIME,  "src3_env_time",  "Time",  "Src Env 3", 0.05f, 20.f, 1.f, 0.4f, "x"),
    F(ParamIdE::SRC3_ENV_DEPTH, "src3_env_depth", "Depth", "Src Env 3", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamIdE::SRC3_ENV_SYNC,  "src3_env_sync",  "Sync",  "Src Env 3", SyncDivNames, NumSyncDivs, 0),
    C(ParamIdE::SRC4_ENV_MODE,  "src4_env_mode",  "Mode",  "Src Env 4", EnvModeNames, NumEnvModes, 0),
    F(ParamIdE::SRC4_ENV_TIME,  "src4_env_time",  "Time",  "Src Env 4", 0.05f, 20.f, 1.f, 0.4f, "x"),
    F(ParamIdE::SRC4_ENV_DEPTH, "src4_env_depth", "Depth", "Src Env 4", 0.f, 1.f, 1.f, 1.f, ""),
    C(ParamIdE::SRC4_ENV_SYNC,  "src4_env_sync",  "Sync",  "Src Env 4", SyncDivNames, NumSyncDivs, 0),
    // ---- what a self-playing generator needs (12.09.2026). Every default is the behaviour the
    // conductor already had, so no preset written before these changes by a single sample.
    F(ParamIdE::BRAIN_LAYERS,      "brain_layers",       "Layers",       "Cluster Brain", 0.f,  1.f,   0.f,  1.f, ""),
    F(ParamIdE::BRAIN_BASS_HOLD,    "brain_bass_hold",    "Bass Hold",    "Cluster Brain", 1.f,  8.f,   1.f,  0.5f, "x"),
    F(ParamIdE::BRAIN_TOP_SOFT,     "brain_top_soft",     "Top Soft",     "Cluster Brain", 0.f,  1.f,   0.f,  1.f, ""),
    F(ParamIdE::BRAIN_LOW_SPACING,  "brain_low_spacing",  "Low Spacing",  "Cluster Brain", 0.f,  1.f,   0.f,  1.f, ""),
    I(ParamIdE::BRAIN_THIRD_FLOOR,  "brain_third_floor",  "Third Floor",  "Cluster Brain", 0.f,  72.f,  0.f),
    F(ParamIdE::BRAIN_LEADING,     "brain_leading",      "Leading",      "Cluster Brain", 0.f,  1.f,   0.f,  1.f, ""),
    F(ParamIdE::BRAIN_THIRDS,      "brain_thirds",       "Thirds",       "Cluster Brain", -1.f, 1.f,   0.f,  1.f, ""),
    F(ParamIdE::BRAIN_SECONDS,     "brain_seconds",      "Seconds",      "Cluster Brain", -1.f, 1.f,   0.f,  1.f, ""),
    F(ParamIdE::BRAIN_SEVENTH,     "brain_seventh",      "Seventh",      "Cluster Brain", 0.f,  1.f,   0.f,  1.f, ""),
    F(ParamIdE::BRAIN_DEGREE_SWAP,  "brain_degree_swap",  "Degree Swap",  "Cluster Brain", 0.f,  1.f,   0.f,  1.f, ""),
    F(ParamIdE::BRAIN_RATE_BREATH,  "brain_rate_breath",  "Rate Breath",  "Cluster Brain", 0.f,  1.f,   0.f,  1.f, ""),
    F(ParamIdE::BRAIN_BREATH_PERIOD,"brain_breath_period","Breath Period","Cluster Brain", 2.f,  30.f,  10.f, 0.6f, "min"),
    F(ParamIdE::BRAIN_OVERLAP,     "brain_overlap",      "Overlap",      "Cluster Brain", 0.f,  60.f,  0.f,  0.6f, "s"),
    B(ParamIdE::BRAIN_ONSET_GUARD,  "brain_onset_guard",  "Onset Guard",  "Cluster Brain", false),
    F(ParamIdE::BRAIN_RELEASE_GAP,  "brain_release_gap",  "Release Gap",  "Cluster Brain", 0.f,  10.f,  0.f,  0.6f, "s"),
    F(ParamIdE::BRAIN_RETRIGGER,   "brain_retrigger",    "Retrigger",    "Cluster Brain", 0.f,  120.f, 0.f,  0.5f, "s"),
    F(ParamIdE::BRAIN_DENSITY_SLEW, "brain_density_slew", "Density Slew", "Cluster Brain", 0.f,  10.f,  0.f,  0.6f, "min"),
    F(ParamIdE::BRAIN_SILENCE,     "brain_silence",      "Silence",      "Cluster Brain", 0.f,  1.f,   0.f,  1.f, ""),
    F(ParamIdE::BRAIN_SILENCE_LEN,  "brain_silence_len",  "Silence Len",  "Cluster Brain", 5.f,  60.f,  20.f, 0.7f, "s"),
    C(ParamIdE::BRAIN_ROOT_STEPS,   "brain_root_steps",   "Root Steps",   "Cluster Brain", RootStepNames, 4, 0),
    F(ParamIdE::BRAIN_ROOT_DOWN,    "brain_root_down",    "Root Down",    "Cluster Brain", -1.f, 1.f,   0.f,  1.f, ""),
    F(ParamIdE::BRAIN_PIVOT,       "brain_pivot",        "Pivot",        "Cluster Brain", 0.f,  90.f,  0.f,  0.6f, "s"),
    F(ParamIdE::BRAIN_HOME,        "brain_home",         "Home",         "Cluster Brain", 0.f,  1.f,   0.f,  1.f, ""),
    F(ParamIdE::BRAIN_HOME_TIME,    "brain_home_time",    "Home Time",    "Cluster Brain", 10.f, 180.f, 60.f, 0.6f, "min"),
    F(ParamIdE::BRAIN_MEMORY,      "brain_memory",       "Memory",       "Cluster Brain", 0.f,  30.f,  0.f,  0.6f, "min"),
    B(ParamIdE::BRAIN2_GOLDEN,     "brain2_golden",      "Golden",       "Brain 2", false),
    B(ParamIdE::TUNE_HOLD_SOUNDING, "tuning_hold_sounding","Hold Sounding","Tuning", false),
    F(ParamIdE::BEAT_CEILING,      "beat_ceiling",       "Beat Ceiling", "Tuning", 0.f, 4.f, 0.f, 0.7f, "Hz"),
    F(ParamIdE::ENV_VEL_ATTACK,     "env_vel_attack",     "Vel to Attack","Envelope", -1.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::LAYER_DEPTH,       "layer_depth",        "Layer Depth",  "Space", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::STRAND_LOW_DETUNE,  "strand_low_detune",  "Low Detune",   "Source 1", 0.f, 1.f, 0.f, 1.f, ""),
    F(ParamIdE::CLOUD_TO_NEAR,      "cloud_to_near",      "To Near",      "Cloud", 0.f, 1.f, 0.f, 1.f, ""),
    // The near plane (13.09.2026): a role per slot, and the Near Events section.
    C(ParamIdE::SRC1_ROLE,         "src1_role",          "Role",         "Source 1", SlotRoleNames, NumSlotRoles, 0),
    C(ParamIdE::SRC2_ROLE,         "src2_role",          "Role",         "Source 2", SlotRoleNames, NumSlotRoles, 0),
    C(ParamIdE::SRC3_ROLE,         "src3_role",          "Role",         "Source 3", SlotRoleNames, NumSlotRoles, 0),
    C(ParamIdE::SRC4_ROLE,         "src4_role",          "Role",         "Source 4", SlotRoleNames, NumSlotRoles, 0),
    // The near source: a fifth slot, the same fields a source slot has where the near types read
    // them, and its own envelope, filter and strike, so the layer owes the sound preset nothing.
    C(ParamIdE::FORE_TYPE,         "fore_type",          "Type",         "Near Source", SourceTypeNames, NumSourceTypes, 10),
    I(ParamIdE::FORE_OCTAVE,       "fore_octave",        "Octave",       "Near Source", -2.f,  2.f,   0.f),
    C(ParamIdE::FORE_RATIO,        "fore_ratio",         "Ratio",        "Near Source", SlotRatioNames, NumSlotRatios, 0),
    F(ParamIdE::FORE_POSITION,     "fore_pos",           "Position",     "Near Source", 0.f,   1.f,   0.3f,  1.f,  ""),
    F(ParamIdE::FORE_POS_DRIFT,     "fore_pos_drift",     "Pos Drift",    "Near Source", 0.f,   1.f,   0.3f,  1.f,  ""),
    F(ParamIdE::FORE_DENSITY,      "fore_density",       "Density",      "Near Source", 0.05f, 200.f, 4.f,   0.4f, "/s"),
    C(ParamIdE::FORE_FOLLOW,       "fore_follow",        "Pitch",        "Near Source", FollowNames, 2, 1),
    F(ParamIdE::FORE_BRIGHT,       "fore_bright",        "Bright",       "Near Source", 0.f,   1.f,   0.6f,  1.f,  ""),
    F(ParamIdE::FORE_FORCE,        "fore_force",         "Force",        "Near Source", 0.f,   1.f,   0.5f,  1.f,  ""),
    F(ParamIdE::FORE_SPEED,        "fore_speed",         "Speed",        "Near Source", 0.f,   1.f,   0.4f,  1.f,  ""),
    C(ParamIdE::FORE_NOISE,        "fore_noise",         "Noise",        "Near Source", NoiseKindNames, NumNoiseKinds, 1),
    F(ParamIdE::FORE_NOISE_Q,       "fore_noise_q",       "Noise Q",      "Near Source", 0.f,   1.f,   0.4f,  1.f,  ""),
    F(ParamIdE::FORE_FM_RATIO,      "fore_fm_ratio",      "FM Ratio",     "Near Source", 0.25f, 8.f,   2.f,   0.5f, ""),
    F(ParamIdE::FORE_FM_INDEX,      "fore_fm_index",      "FM Index",     "Near Source", 0.f,   8.f,   1.f,   0.6f, ""),
    I(ParamIdE::FORE_PARTIALS,     "fore_partials",      "Partials",     "Near Source", 1.f,   32.f,  12.f),
    F(ParamIdE::FORE_TILT,         "fore_tilt",          "Tilt",         "Near Source", 0.3f,  3.f,   1.2f,  1.f,  ""),
    F(ParamIdE::FORE_INHARM,       "fore_inharmonic",    "Inharmonic",   "Near Source", 0.f,   1.f,   0.f,   1.f,  ""),
    F(ParamIdE::FORE_DRIFT,        "fore_drift",         "Drift",        "Near Source", 0.f,   30.f,  0.f,   0.6f, "ct"),
    C(ParamIdE::FORE_TABLE,        "fore_table",         "Table",        "Near Source", TableNames, NumTables, 0),
    F(ParamIdE::FORE_ATTACK,       "fore_attack",        "Attack",       "Near Source", 0.001f, 10.f, 0.3f,  0.4f, "s"),   // down to a millisecond: a sonar ping is struck, not breathed
    F(ParamIdE::FORE_DECAY,        "fore_decay",         "Decay",        "Near Source", 0.01f, 30.f,  1.f,   0.4f, "s"),
    F(ParamIdE::FORE_SUSTAIN,      "fore_sustain",       "Sustain",      "Near Source", 0.f,   1.f,   1.f,   1.f,  ""),
    F(ParamIdE::FORE_RELEASE,      "fore_release",       "Release",      "Near Source", 0.03f, 30.f,  2.f,   0.4f, "s"),
    F(ParamIdE::FORE_CUTOFF,       "fore_cutoff",        "Cutoff",       "Near Source", 40.f,  18000.f, 6000.f, 0.3f, "Hz"),
    F(ParamIdE::FORE_RESONANCE,    "fore_resonance",     "Resonance",    "Near Source", 0.f,   1.f,   0.15f, 1.f,  ""),
    C(ParamIdE::FORE_FILTER_MODEL,  "fore_filter",        "Filter",       "Near Source", FilterModelNames, NumFilterModels, 1),
    F(ParamIdE::FORE_FILTER_ENV,    "fore_filter_env",    "Filter Env",   "Near Source", 0.f,   1.f,   0.f,   1.f,  ""),
    F(ParamIdE::FORE_STRIKE,       "fore_strike",        "Strike",       "Near Source", 0.f,   1.f,   0.f,   1.f,  ""),
    C(ParamIdE::FORE_STRIKE_TYPE,   "fore_strike_type",   "Strike Type",  "Near Source", StrikeTypeNames, 3, 0),
    F(ParamIdE::FORE_STRIKE_DECAY,  "fore_strike_decay",  "Strike Decay", "Near Source", 0.02f, 3.f,   0.4f,  0.5f, "s"),
    F(ParamIdE::FORE_STRIKE_DAMP,   "fore_strike_damp",   "Strike Damp",  "Near Source", 0.f,   1.f,   0.5f,  1.f,  ""),
    // The events: their clock, their pitch, their place, and the sequence.
    F(ParamIdE::FORE_LEVEL,        "fore_level",         "Level",        "Near Events", 0.f,   1.f,   0.f,   1.f,  ""),
    C(ParamIdE::FORE_KIND,         "fore_kind",          "Kind",         "Near Events", NearKindNames, 3, 0),
    F(ParamIdE::FORE_RATE,         "fore_rate",          "Every",        "Near Events", 10.f,  900.f, 120.f, 0.4f, "s"),
    F(ParamIdE::FORE_CHANCE,       "fore_chance",        "Chance",       "Near Events", 0.f,   1.f,   1.f,   1.f,  ""),
    F(ParamIdE::FORE_CLUSTER,      "fore_cluster",       "Cluster",      "Near Events", 0.f,   1.f,   0.f,   1.f,  ""),
    F(ParamIdE::FORE_LENGTH,       "fore_length",        "Length",       "Near Events", 0.5f,  600.f, 6.f,   0.35f, "s"),
    C(ParamIdE::FORE_PITCH,        "fore_pitch",         "Pitch",        "Near Events", NearPitchNames, 5, 0),
    F(ParamIdE::FORE_SPREAD,       "fore_spread",        "Spread",       "Near Events", 0.f,   1.f,   0.5f,  1.f,  ""),
    F(ParamIdE::FORE_APPROACH,     "fore_approach",      "Approach",     "Near Events", -1.f,  1.f,   0.f,   1.f,  ""),   // negative: the event leaves instead of arriving
    F(ParamIdE::FORE_PROXIMITY,    "fore_proximity",     "Proximity",    "Near Events", 0.f,   1.f,   0.6f,  1.f,  ""),
    B(ParamIdE::FORE_HOLD,         "fore_hold",          "Hold Brain",   "Near Events", true),
    F(ParamIdE::FORE_GLIDE,        "fore_glide",         "Glide",        "Near Events", 0.f,   30.f,  0.f,   0.5f, "s"),
    I(ParamIdE::FORE_STEPS,        "fore_steps",         "Steps",        "Near Events", 3.f,   16.f,  7.f),
    F(ParamIdE::FORE_STEP,         "fore_step",          "Step",         "Near Events", 0.1f,  4.f,   0.4f,  0.5f, "s"),
    C(ParamIdE::FORE_STEP_SYNC,     "fore_step_sync",     "Step Sync",    "Near Events", SyncDivNames, NumSyncDivs, 0),
    F(ParamIdE::FORE_MUTATION,     "fore_mutation",      "Mutation",     "Near Events", 0.f,   1.f,   0.12f, 1.f,  ""),
    F(ParamIdE::FORE_SCATTER,      "fore_scatter",       "Scatter",      "Near Events", 0.f,   1.f,   0.3f,  1.f,  ""),
    F(ParamIdE::FORE_BLOOM,        "fore_bloom",         "Bloom",        "Near Events", 0.f,   1.f,   0.5f,  1.f,  ""),
    F(ParamIdE::FORE_DISTANCE,     "fore_distance",      "Distance",     "Near Events", 0.f,   1.f,   0.f,   1.f,  ""),
    F(ParamIdE::FORE_DRY,          "fore_dry",           "Dry",          "Near Events", 0.f,   1.f,   0.f,   1.f,  ""),
    B(ParamIdE::FORE_AUTO,         "fore_auto",          "Auto",         "Near Events", true),
    F(ParamIdE::FAR_UNMASK_RETURN,  "far_unmask_return",  "Return",       "Far Reverb", 0.2f,  10.f,  0.5f,  0.5f, "s"),   // 0.5 s by default (25.09.2026): the guide's release; it was 1.2
    F(ParamIdE::FORE_DELAY2,       "fore_delay2",        "To Delay 2",   "Near Events", 0.f,   1.f,   0.f,   1.f,  ""),
    F(ParamIdE::FORE_COSMOS,       "fore_cosmos",        "To Cosmos",    "Near Events", 0.f,   1.f,   0.f,   1.f,  ""),
    F(ParamIdE::FORE_GAIN,         "fore_gain",          "Gain",         "Near Events", -24.f, 36.f,  0.f,   1.f,  "dB"),
    // The production guide's depth model (25.09.2026): a source loses Range dB between the ear and
    // the horizon (20 puts a note at Depth 0.7 fourteen decibels under the foreground, the guide's
    // middle plane, and one on the horizon twenty under it, the near end of its far plane); its
    // far send waits Gap ms at the ear and nothing on the horizon; its strands fan out to Near
    // Width of their Spread at the ear and to all of it on the horizon.
    F(ParamIdE::DEPTH_RANGE,       "depth_range",        "Range",        "Space",       6.f,   36.f,  20.f,  1.f,  "dB"),
    F(ParamIdE::DEPTH_PRE_DELAY,    "depth_predelay",     "Gap",          "Space",       0.f,   80.f,  40.f,  1.f,  "ms"),
    F(ParamIdE::DEPTH_WIDTH,       "depth_width",        "Near Width",   "Space",       0.f,   1.f,   0.35f, 1.f,  ""),
    F(ParamIdE::SEND_LOWCUT,       "send_lowcut",        "Send Low Cut", "Far Reverb",  20.f,  400.f, 150.f, 0.5f, "Hz"),
    F(ParamIdE::SUB_HARMONICS,     "sub_harmonics",      "Harmonics",    "Foundation",  0.f,   1.f,   0.6f,  1.f,  ""),
    F(ParamIdE::SUB_BEAT,          "sub_beat",           "Beat",         "Foundation",  0.f,   1.f,   0.f,   1.f,  "Hz"),
    // The second round of the guide (25.09.2026): the serial rooms (10 to 20 per cent of the main
    // room into the far one), the far return's middle high-passed from 300 Hz, and the sub's own
    // ceiling at -6 dBFS -- the guide's numbers as the defaults, like the first round's.
    F(ParamIdE::ROOM_TO_FAR,        "room_to_far",        "To Far",       "Room",        0.f,   0.5f,  0.15f, 1.f,  ""),
    F(ParamIdE::FAR_MID_LOWCUT,     "far_mid_lowcut",     "Mid Low Cut",  "Far Reverb",  20.f,  600.f, 300.f, 0.5f, "Hz"),
    F(ParamIdE::SUB_CEILING,       "sub_ceiling",        "Ceiling",      "Foundation",  -24.f, 0.f,   -6.f,  1.f,  "dBFS"),
    // The review against ambient harmony (25.09.2026): Modal by default, because the whole tone is
    // the genre's step and the six alike were film music; Utonal and Series off, the library sets
    // them by family.
    C(ParamIdE::BRAIN_ROOT_TARGETS, "brain_root_targets", "Root Targets", "Cluster Brain", RootTargetNames, 4, 1),
    F(ParamIdE::BRAIN_UTONAL,      "brain_utonal",       "Utonal",       "Cluster Brain", 0.f,  1.f,   0.f,  1.f, ""),
    F(ParamIdE::BRAIN_SERIES,      "brain_series",       "Series",       "Cluster Brain", 0.f,  1.f,   0.f,  1.f, ""),
    // The circuit models of the voice filter (26.09.2026): the SEM's knob, low pass by default so
    // that the model sounds like a low pass until it is turned.
    F(ParamIdE::FILTER_MORPH,      "filter_morph",       "Morph",        "Filter",      0.f,   1.f,   0.f,   1.f,  ""),
}};
} // namespace

const std::array<ParamDesc, NumParams>& paramTable() { return Table; }

namespace {
/**
 * @brief Which ParamId is field f of source slot s, for the SlotFields fields every slot has.
 *
 * The four rows are identical field for field except for the ids: Source 1's level and additive
 * spectrum are the classic Oscillator parameters (OscLevel, Partials, Tilt ...), the other slots
 * have their own. slotParamIds() hands a row out; the engine and the editor both index through it
 * instead of each keeping a copy, and the self test checks the rows against Sources.h.
 */
const ParamIdE SlotIds[SourceSlots][SlotFields] = {
    // Source 1: its type and its slot fields, then the Oscillator parameters that are its level
    // and its additive spectrum, then its grain-density sync and its pitch drift.
    { ParamIdE::SRC1_TYPE, ParamIdE::OSC_LEVEL, ParamIdE::SRC1_OCTAVE, ParamIdE::SRC1_RATIO, ParamIdE::SRC1_PAN, ParamIdE::SRC1_TABLE,
      ParamIdE::SRC1_POSITION, ParamIdE::SRC1_POS_DRIFT, ParamIdE::SRC1_FM_RATIO, ParamIdE::SRC1_FM_INDEX, ParamIdE::SRC1_GRAIN, ParamIdE::SRC1_DENSITY,
      ParamIdE::SRC1_FOLLOW, ParamIdE::SRC1_GRAINS, ParamIdE::SRC1_SPREAD, ParamIdE::SRC1_NOISE, ParamIdE::SRC1_NOISE_Q,
      ParamIdE::PARTIALS, ParamIdE::TILT, ParamIdE::BRIGHTNESS, ParamIdE::ODD_EVEN, ParamIdE::INHARMONIC, ParamIdE::SHIMMER, ParamIdE::SHIMMER_RATE,
      ParamIdE::SRC1_DENSITY_SYNC, ParamIdE::SRC1_DRIFT, ParamIdE::SRC1_STRETCH, ParamIdE::SRC1_XFADE,
      ParamIdE::SRC1_BOW_FORCE, ParamIdE::SRC1_BOW_SPEED,
      ParamIdE::SRC1_SPEC_RATE, ParamIdE::SRC1_SPEC_BREATH, ParamIdE::SRC1_TRANSPORT,
      ParamIdE::SRC1_DELAY, ParamIdE::SRC1_RISE, ParamIdE::SRC1_ENV, ParamIdE::SRC1_INTERP,
      ParamIdE::SRC1_UNISON, ParamIdE::SRC1_UNI_DETUNE, ParamIdE::SRC1_UNI_WIDTH, ParamIdE::SRC1_ROOT, ParamIdE::SRC1_ROLE },
    { ParamIdE::SRC2_TYPE, ParamIdE::SRC2_LEVEL, ParamIdE::SRC2_OCTAVE, ParamIdE::SRC2_RATIO, ParamIdE::SRC2_PAN, ParamIdE::SRC2_TABLE,
      ParamIdE::SRC2_POSITION, ParamIdE::SRC2_POS_DRIFT, ParamIdE::SRC2_FM_RATIO, ParamIdE::SRC2_FM_INDEX, ParamIdE::SRC2_GRAIN, ParamIdE::SRC2_DENSITY,
      ParamIdE::SRC2_FOLLOW, ParamIdE::SRC2_GRAINS, ParamIdE::SRC2_SPREAD, ParamIdE::SRC2_NOISE, ParamIdE::SRC2_NOISE_Q,
      ParamIdE::SRC2_PARTIALS, ParamIdE::SRC2_TILT, ParamIdE::SRC2_BRIGHT, ParamIdE::SRC2_ODD_EVEN, ParamIdE::SRC2_INHARM, ParamIdE::SRC2_SHIMMER, ParamIdE::SRC2_SHIMMER_RATE,
      ParamIdE::SRC2_DENSITY_SYNC, ParamIdE::SRC2_DRIFT, ParamIdE::SRC2_STRETCH, ParamIdE::SRC2_XFADE,
      ParamIdE::SRC2_BOW_FORCE, ParamIdE::SRC2_BOW_SPEED,
      ParamIdE::SRC2_SPEC_RATE, ParamIdE::SRC2_SPEC_BREATH, ParamIdE::SRC2_TRANSPORT,
      ParamIdE::SRC2_DELAY, ParamIdE::SRC2_RISE, ParamIdE::SRC2_ENV, ParamIdE::SRC2_INTERP,
      ParamIdE::SRC2_UNISON, ParamIdE::SRC2_UNI_DETUNE, ParamIdE::SRC2_UNI_WIDTH, ParamIdE::SRC2_ROOT, ParamIdE::SRC2_ROLE },
    { ParamIdE::SRC3_TYPE, ParamIdE::SRC3_LEVEL, ParamIdE::SRC3_OCTAVE, ParamIdE::SRC3_RATIO, ParamIdE::SRC3_PAN, ParamIdE::SRC3_TABLE,
      ParamIdE::SRC3_POSITION, ParamIdE::SRC3_POS_DRIFT, ParamIdE::SRC3_FM_RATIO, ParamIdE::SRC3_FM_INDEX, ParamIdE::SRC3_GRAIN, ParamIdE::SRC3_DENSITY,
      ParamIdE::SRC3_FOLLOW, ParamIdE::SRC3_GRAINS, ParamIdE::SRC3_SPREAD, ParamIdE::SRC3_NOISE, ParamIdE::SRC3_NOISE_Q,
      ParamIdE::SRC3_PARTIALS, ParamIdE::SRC3_TILT, ParamIdE::SRC3_BRIGHT, ParamIdE::SRC3_ODD_EVEN, ParamIdE::SRC3_INHARM, ParamIdE::SRC3_SHIMMER, ParamIdE::SRC3_SHIMMER_RATE,
      ParamIdE::SRC3_DENSITY_SYNC, ParamIdE::SRC3_DRIFT, ParamIdE::SRC3_STRETCH, ParamIdE::SRC3_XFADE,
      ParamIdE::SRC3_BOW_FORCE, ParamIdE::SRC3_BOW_SPEED,
      ParamIdE::SRC3_SPEC_RATE, ParamIdE::SRC3_SPEC_BREATH, ParamIdE::SRC3_TRANSPORT,
      ParamIdE::SRC3_DELAY, ParamIdE::SRC3_RISE, ParamIdE::SRC3_ENV, ParamIdE::SRC3_INTERP,
      ParamIdE::SRC3_UNISON, ParamIdE::SRC3_UNI_DETUNE, ParamIdE::SRC3_UNI_WIDTH, ParamIdE::SRC3_ROOT, ParamIdE::SRC3_ROLE },
    { ParamIdE::SRC4_TYPE, ParamIdE::SRC4_LEVEL, ParamIdE::SRC4_OCTAVE, ParamIdE::SRC4_RATIO, ParamIdE::SRC4_PAN, ParamIdE::SRC4_TABLE,
      ParamIdE::SRC4_POSITION, ParamIdE::SRC4_POS_DRIFT, ParamIdE::SRC4_FM_RATIO, ParamIdE::SRC4_FM_INDEX, ParamIdE::SRC4_GRAIN, ParamIdE::SRC4_DENSITY,
      ParamIdE::SRC4_FOLLOW, ParamIdE::SRC4_GRAINS, ParamIdE::SRC4_SPREAD, ParamIdE::SRC4_NOISE, ParamIdE::SRC4_NOISE_Q,
      ParamIdE::SRC4_PARTIALS, ParamIdE::SRC4_TILT, ParamIdE::SRC4_BRIGHT, ParamIdE::SRC4_ODD_EVEN, ParamIdE::SRC4_INHARM, ParamIdE::SRC4_SHIMMER, ParamIdE::SRC4_SHIMMER_RATE,
      ParamIdE::SRC4_DENSITY_SYNC, ParamIdE::SRC4_DRIFT, ParamIdE::SRC4_STRETCH, ParamIdE::SRC4_XFADE,
      ParamIdE::SRC4_BOW_FORCE, ParamIdE::SRC4_BOW_SPEED,
      ParamIdE::SRC4_SPEC_RATE, ParamIdE::SRC4_SPEC_BREATH, ParamIdE::SRC4_TRANSPORT,
      ParamIdE::SRC4_DELAY, ParamIdE::SRC4_RISE, ParamIdE::SRC4_ENV, ParamIdE::SRC4_INTERP,
      ParamIdE::SRC4_UNISON, ParamIdE::SRC4_UNI_DETUNE, ParamIdE::SRC4_UNI_WIDTH, ParamIdE::SRC4_ROOT, ParamIdE::SRC4_ROLE },
};

/** @brief One section as the table spells it, and the enum value sectionOf() answers for it. */
struct SectionName {
    const char* m_name;       ///< the section string of the table rows, e.g. "Far Reverb"
    ParamSectionE m_section;   ///< the ParamSection it stands for
};
/**
 * @brief The section names of the table and their ParamSection, for sectionOf(const char*).
 *
 * The numbered sections -- "LFO 3", "Env 5", "Src Env 2" -- are not listed; sectionOf() matches
 * them by prefix. "Early Room" maps onto Room, since the early reflections belong to that panel.
 * A name the table uses that is missing here comes out as Unknown, which the self test catches.
 */
const SectionName Sections[] = {
    { "Master", ParamSectionE::MASTER }, { "Source 1", ParamSectionE::SOURCE1 }, { "Strands", ParamSectionE::STRANDS },
    { "Vector", ParamSectionE::VECTOR },
    { "Source 2", ParamSectionE::SOURCE2 }, { "Source 3", ParamSectionE::SOURCE3 }, { "Source 4", ParamSectionE::SOURCE4 },
    { "Strike", ParamSectionE::STRIKE },
    { "Foundation", ParamSectionE::FOUNDATION }, { "Air", ParamSectionE::AIR }, { "Envelope", ParamSectionE::ENVELOPE },
    { "Filter", ParamSectionE::FILTER }, { "Z-Plane", ParamSectionE::Z_PLANE }, { "Expression", ParamSectionE::EXPRESSION },
    { "Space", ParamSectionE::SPACE }, { "Ensemble", ParamSectionE::ENSEMBLE }, { "Delay", ParamSectionE::DELAY },
    { "Delay 2", ParamSectionE::DELAY2 }, { "Near Reverb", ParamSectionE::NEAR_REVERB }, { "Far Reverb", ParamSectionE::FAR_REVERB },
    { "Blur", ParamSectionE::BLUR }, { "Feedback", ParamSectionE::FEEDBACK }, { "Room", ParamSectionE::ROOM }, { "Early Room", ParamSectionE::ROOM },
    { "Body", ParamSectionE::BODY }, { "Patina", ParamSectionE::PATINA }, { "Cosmos", ParamSectionE::COSMOS },
    { "Cloud", ParamSectionE::CLOUD }, { "Cluster Brain", ParamSectionE::CLUSTER_BRAIN }, { "Brain 2", ParamSectionE::BRAIN2 },
    { "Autoplay", ParamSectionE::AUTOPLAY },
    { "Tuning", ParamSectionE::TUNING }, { "Coherence", ParamSectionE::COHERENCE }, { "Clock", ParamSectionE::CLOCK },
    { "Morph", ParamSectionE::MORPH }, { "Macros", ParamSectionE::MACROS }, { "Map", ParamSectionE::MAP },
    { "Route", ParamSectionE::ROUTE }, { "Memory", ParamSectionE::MEMORY },
    { "Near Source", ParamSectionE::NEAR_SOURCE }, { "Near Events", ParamSectionE::NEAR_EVENTS },
};
} // namespace

const ParamIdE* slotParamIds(int slot)
{
    return (slot >= 0 && slot < SourceSlots) ? SlotIds[slot] : nullptr;
}

ParamSectionE sectionOf(const char* name)
{
    if (name == nullptr) return ParamSectionE::UNKNOWN;
    // The eight LFOs and six envelopes each have their own numbered section ("LFO 3", "Env 5"), and
    // so do the four sources' own envelopes ("Src Env 2"), which are envelopes like the six.
    if (std::strncmp(name, "LFO ", 4) == 0) return ParamSectionE::LFO;
    if (std::strncmp(name, "Env ", 4) == 0) return ParamSectionE::MOD_ENVELOPE;
    if (std::strncmp(name, "Src Env ", 8) == 0) return ParamSectionE::MOD_ENVELOPE;
    for (const SectionName& s : Sections) if (std::strcmp(s.m_name, name) == 0) return s.m_section;
    return ParamSectionE::UNKNOWN;
}

const ParamSectionE* sectionTable()
{
    static const std::vector<ParamSectionE> t = [] {
        std::vector<ParamSectionE> v;
        v.reserve(std::size(Table));
        for (const auto& d : Table) v.push_back(sectionOf(d.m_section));
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
        v.reserve(std::size(Table));
        for (const auto& d : Table) v.push_back(&d);
        std::sort(v.begin(), v.end(), [](const ParamDesc* a, const ParamDesc* b) { return std::strcmp(a->m_key, b->m_key) < 0; });
        return v;
    }();
    auto it = std::lower_bound(sorted.begin(), sorted.end(), key,
                               [](const ParamDesc* d, const char* k) { return std::strcmp(d->m_key, k) < 0; });
    return (it != sorted.end() && std::strcmp((*it)->m_key, key) == 0) ? *it : nullptr;
}

} // namespace ambient
