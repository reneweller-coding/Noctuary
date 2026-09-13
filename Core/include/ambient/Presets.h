// Noctuary -- built-in presets. A preset is a name plus "key=value;key=value"
// over the parameter table; unspecified parameters take their defaults.
//
// Two independent layers can be loaded and combined:
//   Sound  = everything except the Cosmos section (voices, space, delays, reverbs, brain, tuning)
//   Cosmos = the Cosmos section only
// The 256 full presets carry both layers (they are the DAW programs); the Cosmos
// bank carries Cosmos-only settings.
#pragma once
#include <string>
#include "Params.h"
#include <cstring>

namespace ambient {

struct Preset {
    const char* name;
    const char* settings;             // "key=value;key=value", choice values may be given by name
    const char* texture = nullptr;    // file the host should load into the Texture slots (pack presets)
    const char* wavetable = nullptr;  // file the host should load into the User wavetable
    const char* impulse = nullptr;    // file the host should load into the convolution Room
    // Modulation is data, not parameters (see Modulation.h): the matrix rows, and the envelope
    // shapes separated by '~' -- the six modulation envelopes, then the four sources' own (for a
    // slot whose Env is Own). Empty, or a list that stops early, means "the defaults" for the rest.
    const char* mod = nullptr;
    const char* envs = nullptr;
    // The impulse Room Morph goes to, for a preset that morphs its room. Appended, so every preset
    // written before it -- and every aggregate that stops at envs -- means "no B".
    const char* impulseB = nullptr;
};

// A preset can be the whole instrument or one section of it. The section scopes are layers: a
// bank that lands on top of whatever sound is loaded, resetting only its own section first.
enum class PresetScope { Full, Sound, Cosmos, ZPlane, Strike, Near };

// The preset list is the 256 built-in presets followed by every loaded pack, so everything that
// walks presets by index (DAW programs, the map, routes, the browser) sees packs automatically.
int numPresets();
// A line of prose about a preset, in the manner of u-he's browsers: what it sounds like (from the
// measured descriptors) and what is in it (from its own settings). Generated, never stored, and
// never a guess -- see Core/src/PresetText.cpp.
std::string presetDescription(int index);
// The whole card, in the manner of u-he's PRESET INFO: the description, how it paces itself, what
// the macros and the wheel are wired to in this preset, and where it is filed. Lines, not a
// paragraph -- the browser draws the headings.
std::string presetInfoText(int index);
const Preset& preset(int index);
int builtinPresetCount();                 // the compiled-in presets (the first ones)
const Preset& builtinPreset(int index);
// The three section layers. Each has its own bank, its own families for the list, and touches
// nothing outside its section.
int numCosmosPresets();                   // Cosmos-only bank
const Preset& cosmosPreset(int index);
int cosmosPresetCategory(int index);      // 255 for the Off entry, which belongs to no family
int numCosmosPresetFamilies();
const char* cosmosPresetFamily(int family);

int numZPresets();                        // Z-plane-only bank: one preset per filter shape
const Preset& zPreset(int index);
int zPresetCategory(int index);           // the shape's family (see ZPlane.h), 255 for Off

int numStrikePresets();                   // Strike-only bank (the Karplus-Strong pluck)
const Preset& strikePreset(int index);
int strikePresetCategory(int index);
int numStrikePresetFamilies();
const char* strikePresetFamily(int family);

// The near layer (13.09.2026): the Near Source and Near Events sections together. Like the
// Cosmos it is kept across sound presets -- a foreground chosen for the night stays while the
// backgrounds change under it -- so a sound preset neither carries it nor clears it.
int numNearPresets();
const Preset& nearPreset(int index);
int nearPresetCategory(int index);        // 255 for the Off entry
int numNearPresetFamilies();
const char* nearPresetFamily(int family);

inline bool isCosmosParam(ParamId id) { return sectionOf(id) == ParamSection::Cosmos; }
inline bool isZPlaneParam(ParamId id) { return sectionOf(id) == ParamSection::ZPlane; }
inline bool isStrikeParam(ParamId id) { return sectionOf(id) == ParamSection::Strike; }
// Auto is the one Near Events control that is not part of a near preset: it decides whether a
// sound preset brings its own foreground, and a near preset chosen by hand must not switch it on.
inline bool isNearParam(ParamId id)   { const ParamSection s = sectionOf(id); return (s == ParamSection::NearSource || s == ParamSection::NearEvents) && id != ParamId::ForeAuto; }
// The layer as a whole, Auto included. `isNearParam` is what a near PRESET carries; this is what
// a sound preset has to leave standing. The two differ by exactly one control, and the difference
// matters: Auto is the player's setting about presets, not a setting a preset may make.
inline bool isNearLayerParam(ParamId id) { const ParamSection s = sectionOf(id); return s == ParamSection::NearSource || s == ParamSection::NearEvents; }
inline bool isMorphParam(ParamId id)  { return sectionOf(id) == ParamSection::Morph; }
inline bool isMacroParam(ParamId id)  { return sectionOf(id) == ParamSection::Macros; }
inline bool isMapParam(ParamId id)    { return sectionOf(id) == ParamSection::Map; }
inline bool isRouteParam(ParamId id)  { return sectionOf(id) == ParamSection::Route; }
inline bool isClockParam(ParamId id)  { return sectionOf(id) == ParamSection::Clock; }
// Morph controls, macros, the map cursor, the route and the clock are performance state, never part of any preset.
inline bool isPerformanceParam(ParamId id) { return isMorphParam(id) || isMacroParam(id) || isMapParam(id) || isRouteParam(id) || isClockParam(id); }
inline bool inScope(ParamId id, PresetScope scope)
{
    if (isPerformanceParam(id)) return false;
    switch (scope) {
        case PresetScope::Cosmos: return isCosmosParam(id);
        case PresetScope::ZPlane: return isZPlaneParam(id);
        case PresetScope::Strike: return isStrikeParam(id);
        case PresetScope::Near:   return isNearParam(id);
        case PresetScope::Sound:  return !isCosmosParam(id) && !isNearParam(id);   // everything a sound preset owns
        case PresetScope::Full:   break;
    }
    return true;
}
// What a preset CLEARS before it is applied, which is not the same question as what it may SET.
//
// Applying a preset begins by putting everything in its scope back to its default, so that what
// the preset does not mention is not left over from whatever played before. For the near layer
// that is wrong, and it was wrong for a fortnight (found 13.09.2026): the layer is a bank of its
// own with its own selector, kept across sound presets on purpose, and no preset in the library
// carries a single fore_* key. So a Full apply -- a DAW program change, a journey step, the
// crossfade between two presets -- reset fore_level to its default of zero and switched the
// foreground off, silently, every time the piece moved on. Twelve minutes of a journey with
// nothing near in them.
//
// It CLEARS less than it may SET on purpose: a preset that does name a fore_* key still sets it,
// because inScope, which decides that, is unchanged. So this cannot swallow a preset's intent --
// only its silence.
inline bool clearedBy(ParamId id, PresetScope scope)
{
    if (scope == PresetScope::Full && isNearLayerParam(id)) return false;
    return inScope(id, scope);
}

// ---------------------------------------------------------------- preset packs
//
// A pack is a UTF-8 text file (.ambientpack) of presets, loaded at runtime instead of compiled
// in, so a library of thousands does not live in the binary:
//
//   # comment
//   pack <pack name>
//   <name>|<settings>|<x y bright motion width noisy bass density tagbits>|<texture>|<wavetable>|
//   <impulse>|<mod matrix>|<env shapes, '~' between them: Env 1..6, then Source 1..4's own>|<impulse B>
//
// Everything after the settings is optional. The metadata field feeds the browser and the map
// (see PresetMeta.h); the file fields name a sample and a wavetable relative to the pack file,
// which the host loads when the preset is applied. Message thread only.
bool loadPresetPack(const char* path);      // false if the file is missing or a line is malformed
int  loadPresetPacksIn(const char* dir);    // every *.ambientpack in a directory; returns how many loaded
// $AMBIENT_PACKS (';'-separated) if it finds anything, else the user's own
// Documents/Noctuary/Packs and the folders an installer writes to (Windows: ProgramData and
// LocalAppData; elsewhere /usr/local/share and /usr/share). A pack found twice loads once.
int  loadDefaultPresetPacks();
void clearPresetPacks();
int  numPresetPacks();
const char* presetPackName(int pack);
// The pack a preset came from, or -1 for a built-in.
int  presetPack(int presetIndex);
// The near layer's Auto (13.09.2026): the foreground a sound preset of a pack brings with it.
// Rene's table per artist (Tools/library/near_by_artist.json, compiled to NearAuto.inc): a share
// of the pack's presets get one at all, drawn by weight from the artist's list, and the near
// preset's Every scaled by a factor of its class (often, now and then, seldom). Deterministic:
// the draw is a hash of the preset's name, so a preset brings the same foreground every time.
// Returns the near preset's index (0 = Near Off for the presets that get none), or -1 when the
// pack has no table; `rateFactor` is what to multiply the near preset's Every by.
int  nearAutoPick(const char* packName, const char* presetName, float& rateFactor);
// The folders the library may lie in, in the order resolveLibraryFile searches them: for anything
// that lists a library folder (the journeys under <root>/Journeys, say) rather than one file.
int  libraryRoots(char* buf, int cap);   // ';'-separated into buf; returns how many
// Absolute path of the file a pack preset names, or an empty string.
// `which`: 0 texture, 1 wavetable, 2 impulse, 3 impulse B.
constexpr int kPresetFiles = 4;
const char* presetFilePath(int presetIndex, int which);
// A file named relative to the library's root ("Archive/NASA/Historical/...flac"), as the
// compiled-in banks name theirs: looked for under $AMBIENT_LIBRARY, beside every loaded pack
// folder (the library is the folder the Packs folder is in), in the user's and the installer's
// Noctuary folders, and last relative to the working directory (the source tree's Library).
// Empty if nowhere. The .flac beside a named .wav counts, as everywhere else.
std::string resolveLibraryFile(const char* relative);

// Parse a value for `d` from text: numbers, "on"/"off", or a choice name.
float paramValueFromText(const ParamDesc& d, const char* text);

// Calls set(ParamId, value) for every parameter in scope: defaults first, then the
// preset's settings. Returns false if the settings string names an unknown parameter.
template <class SetFn>
bool applyPreset(const Preset& p, SetFn&& set, PresetScope scope = PresetScope::Full)
{
    for (const ParamDesc& d : paramTable()) if (clearedBy(d.id, scope)) set(d.id, d.def);
    bool ok = true;
    const char* s = p.settings;
    while (s && *s) {
        const char* eq = s;
        while (*eq && *eq != '=' && *eq != ';') ++eq;
        if (*eq != '=') { ok = false; break; }
        char key[64];
        const int klen = static_cast<int>(eq - s);
        if (klen <= 0 || klen >= 64) { ok = false; break; }
        for (int i = 0; i < klen; ++i) key[i] = s[i];
        key[klen] = 0;
        const char* vs = eq + 1;
        const char* ve = vs;
        while (*ve && *ve != ';') ++ve;
        char val[64];
        const int vlen = static_cast<int>(ve - vs);
        if (vlen >= 64) { ok = false; break; }
        for (int i = 0; i < vlen; ++i) val[i] = vs[i];
        val[vlen] = 0;
        if (const ParamDesc* d = findParam(key)) { if (inScope(d->id, scope)) set(d->id, paramValueFromText(*d, val)); }
        else ok = false;
        s = (*ve == ';') ? ve + 1 : ve;
    }
    return ok;
}

} // namespace ambient
