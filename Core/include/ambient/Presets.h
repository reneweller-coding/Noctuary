/**
 * @file Presets.h
 * @brief Built-in presets. A preset is a name plus "key=value;key=value"
 *        over the parameter table; unspecified parameters take their defaults.
 *
 * Two independent layers can be loaded and combined:
 *   Sound  = everything except the Cosmos section (voices, space, delays, reverbs, brain, tuning)
 *   Cosmos = the Cosmos section only
 * The 256 full presets carry both layers (they are the DAW programs); the Cosmos
 * bank carries Cosmos-only settings.
 *
 * Since then the section layers have grown to four (Cosmos, Z-plane, Strike and the near layer,
 * PresetScope) and the preset list to the built-ins followed by every loaded pack (numPresets,
 * PresetPacks.cpp). The compiled-in tables live in Presets.cpp and the .inc files beside it; the
 * prose about a preset comes from PresetText.cpp. Applying a preset is applyPreset(), a template
 * so that the engine, the host layer and the map's warmup can each receive the values their own
 * way; what it clears and what it may set are two different questions (clearedBy, inScope), and
 * the difference is what keeps a foreground chosen for the night standing while the backgrounds
 * change under it.
 */
#pragma once
#include <string>
#include "Params.h"
#include <cstring>

namespace ambient {

/** @brief One preset: its name, its settings text and the files and data blobs it may carry. */
struct Preset {
    const char* name;                 ///< the name shown in lists and stored by routes and OSC
    const char* settings;             ///< "key=value;key=value", choice values may be given by name
    const char* texture = nullptr;    ///< file the host should load into the Texture slots (pack presets)
    const char* wavetable = nullptr;  ///< file the host should load into the User wavetable
    const char* impulse = nullptr;    ///< file the host should load into the convolution Room
    /// Modulation is data, not parameters (see Modulation.h): the matrix rows, and the envelope
    /// shapes separated by '~' -- the six modulation envelopes, then the four sources' own (for a
    /// slot whose Env is Own). Empty, or a list that stops early, means "the defaults" for the rest.
    const char* mod = nullptr;        ///< the modulation matrix rows, in Modulation.h's text form
    const char* envs = nullptr;       ///< the envelope shapes, '~' between them: Env 1..6, then Source 1..4's own
    /// The impulse Room Morph goes to, for a preset that morphs its room. Appended, so every preset
    /// written before it -- and every aggregate that stops at envs -- means "no B".
    const char* impulseB = nullptr;   ///< the second impulse, or nullptr for no B
};

/**
 * @brief A preset can be the whole instrument or one section of it. The section scopes are layers: a
 *        bank that lands on top of whatever sound is loaded, resetting only its own section first.
 */
enum class PresetScope {
    Full,     ///< @brief every parameter that is not performance state
    Sound,    ///< @brief everything a sound preset owns: all but the Cosmos and the near layer
    Cosmos,   ///< @brief the Cosmos section only
    ZPlane,   ///< @brief the Z-plane filter section only
    Strike,   ///< @brief the Strike section only
    Near      ///< @brief the Near Source and Near Events sections, Auto excepted
};

/**
 * @brief The preset list is the 256 built-in presets followed by every loaded pack, so everything that
 *        walks presets by index (DAW programs, the map, routes, the browser) sees packs automatically.
 * @return how many presets there are right now, built-ins plus loaded packs
 */
int numPresets();
/**
 * @brief A line of prose about a preset, in the manner of u-he's browsers: what it sounds like (from the
 *        measured descriptors) and what is in it (from its own settings).
 *
 * Generated, never stored, and
 * never a guess -- see Core/src/PresetText.cpp.
 *
 * Message thread: allocates.
 *
 * @param index  0 .. numPresets()-1
 * @return       one or two sentences
 */
std::string presetDescription(int index);
/**
 * @brief The whole card, in the manner of u-he's PRESET INFO: the description, how it paces itself, what
 *        the macros and the wheel are wired to in this preset, and where it is filed.
 *
 * Lines, not a
 * paragraph -- the browser draws the headings.
 *
 * Message thread: allocates.
 *
 * @param index  0 .. numPresets()-1
 * @return       the card's lines, newline-separated
 */
std::string presetInfoText(int index);
/**
 * @brief One preset of the list, built-in or pack.
 * @param index  0 .. numPresets()-1; an index past the packs gives built-in 0
 * @return       the preset; a pack preset's pointers stay valid until clearPresetPacks()
 */
const Preset& preset(int index);
/** @return the compiled-in presets (the first ones) */
int builtinPresetCount();
/**
 * @brief One compiled-in preset.
 * @param index  0 .. builtinPresetCount()-1; anything else gives preset 0
 * @return       the preset
 */
const Preset& builtinPreset(int index);
/**
 * @brief The three section layers. Each has its own bank, its own families for the list, and touches
 *        nothing outside its section.
 * @return how many presets the Cosmos-only bank holds
 */
int numCosmosPresets();
/**
 * @brief One preset of the Cosmos bank.
 * @param index  0 .. numCosmosPresets()-1; anything else gives entry 0
 * @return       the preset (its settings name Cosmos parameters only)
 */
const Preset& cosmosPreset(int index);
/**
 * @brief The family a Cosmos preset is filed under.
 * @param index  0 .. numCosmosPresets()-1
 * @return       the family index for cosmosPresetFamily(), or 255 for the Off entry, which belongs
 *               to no family, and for any index out of range
 */
int cosmosPresetCategory(int index);
/** @return how many families the Cosmos bank is sorted into */
int numCosmosPresetFamilies();
/**
 * @brief The name of a Cosmos family: the heading of one family of the bank, as the browser shows it.
 * @param family  0 .. numCosmosPresetFamilies()-1
 * @return        the name, "" when out of range
 */
const char* cosmosPresetFamily(int family);

/** @return how many presets are in the Z-plane-only bank: one preset per filter shape */
int numZPresets();
/**
 * @brief One preset of the Z-plane bank.
 * @param index  0 .. numZPresets()-1; anything else gives entry 0
 * @return       the preset
 */
const Preset& zPreset(int index);
/**
 * @brief The family of a Z-plane preset: the shape's own family.
 * @param index  0 .. numZPresets()-1
 * @return       the shape's family (see ZPlane.h), 255 for Off and for any index out of range --
 *               255 belongs to no family
 */
int zPresetCategory(int index);

/** @return how many presets the Strike-only bank (the Karplus-Strong pluck) holds */
int numStrikePresets();
/**
 * @brief One preset of the Strike bank.
 * @param index  0 .. numStrikePresets()-1; anything else gives entry 0
 * @return       the preset
 */
const Preset& strikePreset(int index);
/**
 * @brief The family a Strike preset is filed under.
 * @param index  0 .. numStrikePresets()-1
 * @return       the family index for strikePresetFamily(), or 255 for the Off entry and for any
 *               index out of range -- 255 belongs to no family
 */
int strikePresetCategory(int index);
/** @return how many families the Strike bank is sorted into */
int numStrikePresetFamilies();
/**
 * @brief The name of a Strike family: the heading of one family of the bank, as the browser shows it.
 * @param family  0 .. numStrikePresetFamilies()-1
 * @return        the name, "" when out of range
 */
const char* strikePresetFamily(int family);

/**
 * @brief The near layer (13.09.2026): the Near Source and Near Events sections together.
 *
 * Like the
 * Cosmos it is kept across sound presets -- a foreground chosen for the night stays while the
 * backgrounds change under it -- so a sound preset neither carries it nor clears it.
 *
 * @return how many presets the near bank holds (entry 0 is Near Off)
 */
int numNearPresets();
/**
 * @brief One preset of the near bank.
 * @param index  0 .. numNearPresets()-1; anything else gives entry 0
 * @return       the preset
 */
const Preset& nearPreset(int index);
/**
 * @brief The family a near preset is filed under.
 * @param index  0 .. numNearPresets()-1
 * @return       the family index for nearPresetFamily(), or 255 for the Off entry and for any
 *               index out of range -- 255 belongs to no family
 */
int nearPresetCategory(int index);
/** @return how many families the near bank is sorted into */
int numNearPresetFamilies();
/**
 * @brief The name of a near family: the heading of one family of the bank, as the browser shows it.
 * @param family  0 .. numNearPresetFamilies()-1
 * @return        the name, "" when out of range
 */
const char* nearPresetFamily(int family);

/**
 * @brief Whether a parameter belongs to the Cosmos section.
 * @param id  the parameter
 * @return    true for the Cosmos layer's parameters
 */
inline bool isCosmosParam(ParamId id) { return sectionOf(id) == ParamSection::Cosmos; }
/**
 * @brief Whether a parameter belongs to the Z-plane section.
 * @param id  the parameter
 * @return    true for the Z-plane layer's parameters
 */
inline bool isZPlaneParam(ParamId id) { return sectionOf(id) == ParamSection::ZPlane; }
/**
 * @brief Whether a parameter belongs to the Strike section.
 * @param id  the parameter
 * @return    true for the Strike layer's parameters
 */
inline bool isStrikeParam(ParamId id) { return sectionOf(id) == ParamSection::Strike; }
/**
 * @brief What a near PRESET carries: the Near Source and Near Events sections, Auto excepted.
 *
 * Auto is the one Near Events control that is not part of a near preset: it decides whether a
 * sound preset brings its own foreground, and a near preset chosen by hand must not switch it on.
 *
 * @param id  the parameter
 * @return    true for the near layer's parameters other than ForeAuto
 */
inline bool isNearParam(ParamId id)   { const ParamSection s = sectionOf(id); return (s == ParamSection::NearSource || s == ParamSection::NearEvents) && id != ParamId::ForeAuto; }
/**
 * @brief The layer as a whole, Auto included.
 *
 * `isNearParam` is what a near PRESET carries; this is what
 * a sound preset has to leave standing. The two differ by exactly one control, and the difference
 * matters: Auto is the player's setting about presets, not a setting a preset may make.
 *
 * @param id  the parameter
 * @return    true for every Near Source and Near Events parameter
 */
inline bool isNearLayerParam(ParamId id) { const ParamSection s = sectionOf(id); return s == ParamSection::NearSource || s == ParamSection::NearEvents; }
/**
 * @brief Whether a parameter is one of the morph controls (performance state).
 * @param id  the parameter
 * @return    true for the Morph section
 */
inline bool isMorphParam(ParamId id)  { return sectionOf(id) == ParamSection::Morph; }
/**
 * @brief Whether a parameter is one of the eight macros (performance state).
 * @param id  the parameter
 * @return    true for the Macros section
 */
inline bool isMacroParam(ParamId id)  { return sectionOf(id) == ParamSection::Macros; }
/**
 * @brief Whether a parameter is the map cursor or its switch (performance state).
 * @param id  the parameter
 * @return    true for the Map section
 */
inline bool isMapParam(ParamId id)    { return sectionOf(id) == ParamSection::Map; }
/**
 * @brief Whether a parameter is one of the route controls (performance state).
 * @param id  the parameter
 * @return    true for the Route section
 */
inline bool isRouteParam(ParamId id)  { return sectionOf(id) == ParamSection::Route; }
/**
 * @brief Whether a parameter is one of the clock controls (performance state).
 * @param id  the parameter
 * @return    true for the Clock section
 */
inline bool isClockParam(ParamId id)  { return sectionOf(id) == ParamSection::Clock; }
/**
 * @brief Morph controls, macros, the map cursor, the route and the clock are performance state, never part of any preset.
 * @param id  the parameter
 * @return    true when no preset, of any scope, may set or clear it
 */
inline bool isPerformanceParam(ParamId id) { return isMorphParam(id) || isMacroParam(id) || isMapParam(id) || isRouteParam(id) || isClockParam(id); }
/**
 * @brief Whether a preset of the given scope may SET a parameter.
 *
 * Performance state never; the section scopes their own section; Sound everything that is not
 * the Cosmos or the near layer; Full everything else.
 *
 * @param id     the parameter
 * @param scope  the preset's scope
 * @return       true when a key naming @p id in a preset of that scope is applied
 */
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
/**
 * @brief What a preset CLEARS before it is applied, which is not the same question as what it may SET.
 *
 * Applying a preset begins by putting everything in its scope back to its default, so that what
 * the preset does not mention is not left over from whatever played before. For the near layer
 * that is wrong, and it was wrong for a fortnight (found 13.09.2026): the layer is a bank of its
 * own with its own selector, kept across sound presets on purpose, and no preset in the library
 * carries a single fore_* key. So a Full apply -- a DAW program change, a journey step, the
 * crossfade between two presets -- reset fore_level to its default of zero and switched the
 * foreground off, silently, every time the piece moved on. Twelve minutes of a journey with
 * nothing near in them.
 *
 * It CLEARS less than it may SET on purpose: a preset that does name a fore_* key still sets it,
 * because inScope, which decides that, is unchanged. So this cannot swallow a preset's intent --
 * only its silence.
 *
 * @param id     the parameter
 * @param scope  the preset's scope
 * @return       true when applying a preset of that scope first resets @p id to its default
 */
inline bool clearedBy(ParamId id, PresetScope scope)
{
    if (scope == PresetScope::Full && isNearLayerParam(id)) return false;
    return inScope(id, scope);
}

// ---------------------------------------------------------------- preset packs
//
/**
 * @name Preset packs
 *
 * A pack is a UTF-8 text file (.ambientpack) of presets, loaded at runtime instead of compiled
 * in, so a library of thousands does not live in the binary:
 *
 *   # comment
 *   pack \<pack name\>
 *   \<name\>|\<settings\>|\<x y bright motion width noisy bass density tagbits\>|\<texture\>|\<wavetable\>|
 *   \<impulse\>|\<mod matrix\>|\<env shapes, '~' between them: Env 1..6, then Source 1..4's own\>|\<impulse B\>
 *
 * Everything after the settings is optional. The metadata field feeds the browser and the map
 * (see PresetMeta.h); the file fields name a sample and a wavetable relative to the pack file,
 * which the host loads when the preset is applied. Message thread only.
 * @{ */
/**
 * @brief Loads one pack file and appends its presets to the list.
 *
 * A file already loaded in this process, or a pack whose name is already loaded from another
 * folder, is skipped (false). A pack without a "format" line predates the classic wavetable and
 * has its "Wavetable" type read as Harmonic. Every preset of the pack is filed under a family of
 * its own, appended after the built-in families.
 *
 * @param path  the .ambientpack file
 * @return      false if the file is missing or a line is malformed
 */
bool loadPresetPack(const char* path);
/**
 * @brief Loads every *.ambientpack in a directory, in sorted order so preset indices are reproducible.
 * @param dir  the folder; a missing folder loads nothing
 * @return     every *.ambientpack in a directory; returns how many loaded
 */
int  loadPresetPacksIn(const char* dir);
/**
 * @brief Loads the packs from the default places.
 *
 * $AMBIENT_PACKS (';'-separated) if it finds anything, else the user's own
 * Documents/Noctuary/Packs and the folders an installer writes to (Windows: ProgramData and
 * LocalAppData; elsewhere /usr/local/share and /usr/share). A pack found twice loads once.
 *
 * With $AMBIENT_PACKS set its folders are final, even when they yield nothing new -- a second
 * call in one process (a batch render) must not fall through to the installed copies.
 *
 * @return how many packs were loaded by this call
 */
int  loadDefaultPresetPacks();
/** @brief Unloads every pack: the list shrinks back to the built-ins, and every pack Preset's pointers die. */
void clearPresetPacks();
/** @return how many packs are loaded */
int  numPresetPacks();
/**
 * @brief The name of a loaded pack (its "pack" line, else the file's stem).
 * @param pack  0 .. numPresetPacks()-1
 * @return      the name, "" when out of range
 */
const char* presetPackName(int pack);
/**
 * @brief The pack a preset came from, or -1 for a built-in.
 * @param presetIndex  0 .. numPresets()-1
 * @return             the pack index, or -1
 */
int  presetPack(int presetIndex);
/**
 * @brief The near layer's Auto (13.09.2026): the foreground a sound preset of a pack brings with it.
 *
 * Rene's table per artist (Tools/library/near_by_artist.json, compiled to NearAuto.inc): a share
 * of the pack's presets get one at all, drawn by weight from the artist's list, and the near
 * preset's Every scaled by a factor of its class (often, now and then, seldom). Deterministic:
 * the draw is a hash of the preset's name, so a preset brings the same foreground every time.
 * Returns the near preset's index (0 = Near Off for the presets that get none), or -1 when the
 * pack has no table; `rateFactor` is what to multiply the near preset's Every by.
 *
 * @param packName    the pack's name, as presetPackName() gives it
 * @param presetName  the preset's name
 * @param rateFactor  receives the factor on the near preset's Every (1 when there is no table)
 * @return            the near preset's index, 0 for none, -1 when the pack has no table
 */
int  nearAutoPick(const char* packName, const char* presetName, float& rateFactor);
/**
 * @brief The folders the library may lie in, in the order resolveLibraryFile searches them: for anything
 *        that lists a library folder (the journeys under \<root\>/Journeys, say) rather than one file.
 * @param buf  receives the roots, NUL-terminated
 * @param cap  size of @p buf; roots that do not fit are left out
 * @return     ';'-separated into buf; returns how many roots were written
 */
int  libraryRoots(char* buf, int cap);
constexpr int kPresetFiles = 4;   ///< how many files a pack preset may name: texture, wavetable, impulse, impulse B
/**
 * @brief Absolute path of the file a pack preset names, or an empty string.
 *
 * `which`: 0 texture, 1 wavetable, 2 impulse, 3 impulse B.
 *
 * Resolved against the pack's folder when the pack was loaded; the texture field may hold up to
 * four ';'-separated files, one per slot, and the separators are kept.
 *
 * @param presetIndex  0 .. numPresets()-1; a built-in has no files
 * @param which        0 .. kPresetFiles-1
 * @return             the path, or "" for a built-in, an unnamed file or an index out of range
 */
const char* presetFilePath(int presetIndex, int which);
/**
 * @brief Finds a file of the library by its library-relative name.
 *
 * A file named relative to the library's root ("Archive/NASA/Historical/...flac"), as the
 * compiled-in banks name theirs: looked for under $AMBIENT_LIBRARY, beside every loaded pack
 * folder (the library is the folder the Packs folder is in), in the user's and the installer's
 * Noctuary folders, and last relative to the working directory (the source tree's Library).
 * Empty if nowhere. The .flac beside a named .wav counts, as everywhere else.
 *
 * @param relative  the path as the bank names it
 * @return          the first existing absolute path, or ""
 */
std::string resolveLibraryFile(const char* relative);
/** @} */

/**
 * @brief Parse a value for `d` from text: numbers, "on"/"off", or a choice name.
 *
 * A choice name is looked up first; a switch also accepts true/false and yes/no; anything else
 * goes through atof and is clamped to the parameter's range, and "nan" (or no number at all)
 * gives the default.
 *
 * @param d     the parameter's description, for its kind, range, default and choice names
 * @param text  the value as written in a preset, a score or an OSC string
 * @return      the value in the parameter's real range
 */
float paramValueFromText(const ParamDesc& d, const char* text);

/**
 * @brief Calls set(ParamId, value) for every parameter in scope: defaults first, then the
 *        preset's settings. Returns false if the settings string names an unknown parameter.
 *
 * Defaults go to every parameter clearedBy() the scope, then each "key=value" of the settings to
 * its parameter if inScope(). Parsing stops at the first malformed item (a key over 63 characters,
 * a value over 63, a missing '='); an unknown key is skipped and remembered in the result. No
 * allocation, so the engine may apply on the audio thread.
 *
 * @tparam SetFn  callable as set(ParamId, float)
 * @param p       the preset
 * @param set     receives every parameter that is cleared or set
 * @param scope   which layer this apply is
 * @return        true when every key was known and every item well formed
 */
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
