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
 * @author Rene Weller
 */
#pragma once
#include <string>
#include <vector>
#include "Params.h"
#include <cstring>

namespace ambient
{

/** @brief One preset: its name, its settings text and the files and data blobs it may carry. */
struct Preset
{
    const char *m_name;                  ///< the name shown in lists and stored by routes and OSC
    const char *m_settings;              ///< "key=value;key=value", choice values may be given by name
    const char *m_texture = nullptr;     ///< file the host should load into the Texture slots (pack presets)
    const char *m_wavetable = nullptr;   ///< file the host should load into the User wavetable
    const char *m_impulse = nullptr;     ///< file the host should load into the convolution Room
    /// Modulation is data, not parameters (see Modulation.h): the matrix rows, and the envelope
    /// shapes separated by '~' -- the six modulation envelopes, then the four sources' own (for a
    /// slot whose Env is Own). Empty, or a list that stops early, means "the defaults" for the rest.
    const char *m_mod = nullptr;    ///< the modulation matrix rows, in Modulation.h's text form
    const char *m_envs = nullptr;   ///< the envelope shapes, '~' between them: Env 1..6, then Source 1..4's own
    /// The impulse Room Morph goes to, for a preset that morphs its room. Appended, so every preset
    /// written before it -- and every aggregate that stops at envs -- means "no B".
    const char *m_impulseB = nullptr;   ///< the second impulse, or nullptr for no B
};

/**
 * @brief A preset can be the whole instrument or one section of it. The section scopes are layers: a
 *        bank that lands on top of whatever sound is loaded, resetting only its own section first.
 */
enum class PresetScopeE
{
    FULL,        ///< @brief every parameter that is not performance state
    SOUND,       ///< @brief everything a sound preset owns: all but the Cosmos and the near layer
    COSMOS,      ///< @brief the Cosmos section only
    Z_PLANE,     ///< @brief the Z-plane filter section only
    STRIKE,      ///< @brief the Strike section only
    NEAR_LAYER   ///< @brief the Near Source and Near Events sections, Auto excepted
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
 * @param[in] index  0 .. numPresets()-1
 * @return       one or two sentences
 */
std::string presetDescription( int index );
/**
 * @brief The whole card, in the manner of u-he's PRESET INFO: the description, how it paces itself, what
 *        the macros and the wheel are wired to in this preset, and where it is filed.
 *
 * Lines, not a
 * paragraph -- the browser draws the headings.
 *
 * Message thread: allocates.
 *
 * @param[in] index  0 .. numPresets()-1
 * @return       the card's lines, newline-separated
 */
std::string presetInfoText( int index );
/**
 * @brief One preset of the list, built-in or pack.
 * @param[in] index  0 .. numPresets()-1; an index past the packs gives built-in 0
 * @return the preset; a pack preset's pointers stay valid until clearPresetPacks()
 */
const Preset &preset( int index );
/** @return the compiled-in presets (the first ones) */
int builtinPresetCount();
/**
 * @brief One compiled-in preset.
 * @param[in] index  0 .. builtinPresetCount()-1; anything else gives preset 0
 * @return       the preset
 */
const Preset &builtinPreset( int index );
/**
 * @brief The three section layers. Each has its own bank, its own families for the list, and touches
 *        nothing outside its section.
 * @return how many presets the Cosmos-only bank holds
 */
int numCosmosPresets();
/**
 * @brief One preset of the Cosmos bank.
 * @param[in] index  0 .. numCosmosPresets()-1; anything else gives entry 0
 * @return       the preset (its settings name Cosmos parameters only)
 */
const Preset &cosmosPreset( int index );
/**
 * @brief The family a Cosmos preset is filed under.
 * @param[in] index  0 .. numCosmosPresets()-1
 * @return the family index for cosmosPresetFamily(), or 255 for the Off entry, which belongs to no family, and for any
 *         index out of range
 */
int cosmosPresetCategory( int index );
/** @return how many families the Cosmos bank is sorted into */
int numCosmosPresetFamilies();
/**
 * @brief The name of a Cosmos family: the heading of one family of the bank, as the browser shows it.
 * @param[in] family  0 .. numCosmosPresetFamilies()-1
 * @return the name, "" when out of range
 */
const char *cosmosPresetFamily( int family );

/** @return how many presets are in the Z-plane-only bank: one preset per filter shape */
int numZPresets();
/**
 * @brief One preset of the Z-plane bank.
 * @param[in] index  0 .. numZPresets()-1; anything else gives entry 0
 * @return       the preset
 */
const Preset &zPreset( int index );
/**
 * @brief The family of a Z-plane preset: the shape's own family.
 * @param[in] index  0 .. numZPresets()-1
 * @return the shape's family (see ZPlane.h), 255 for Off and for any index out of range -- 255 belongs to no family
 */
int zPresetCategory( int index );

/** @return how many presets the Strike-only bank (the Karplus-Strong pluck) holds */
int numStrikePresets();
/**
 * @brief One preset of the Strike bank.
 * @param[in] index  0 .. numStrikePresets()-1; anything else gives entry 0
 * @return       the preset
 */
const Preset &strikePreset( int index );
/**
 * @brief The family a Strike preset is filed under.
 * @param[in] index  0 .. numStrikePresets()-1
 * @return the family index for strikePresetFamily(), or 255 for the Off entry and for any index out of range -- 255
 *         belongs to no family
 */
int strikePresetCategory( int index );
/** @return how many families the Strike bank is sorted into */
int numStrikePresetFamilies();
/**
 * @brief The name of a Strike family: the heading of one family of the bank, as the browser shows it.
 * @param[in] family  0 .. numStrikePresetFamilies()-1
 * @return the name, "" when out of range
 */
const char *strikePresetFamily( int family );

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
 * @param[in] index  0 .. numNearPresets()-1; anything else gives entry 0
 * @return       the preset
 */
const Preset &nearPreset( int index );
/**
 * @brief The night guard of one near preset (20.11.2026, plan C3): what an event of it gets when the near layer's
 *        Night is on -- a fade-in over the event's first seconds and a gain -- so that it rises over the background
 *        no faster, and stands out of its own mean no further, than the bank's limits (Tools/library/near_night.py).
 */
struct NearGuard
{
    float m_fade = 0.0f;     ///< seconds of the fade-in from the event's beginning (0: none)
    float m_gainDb = 0.0f;   ///< the event's gain in dB (0 or less)
    /** @return whether this changes anything at all */
    bool any() const { return m_fade > 0.0f || m_gainDb < 0.0f; }
};
/**
 * @brief The night guard measured for a preset of the near bank (Core/src/NearNight.inc).
 * @param[in] index  0 .. numNearPresets()-1
 * @return its row; none (no fade, 0 dB) for Near Off, an index out of range, or a row that does not name the preset
 */
NearGuard nearGuardOf( int index );
/**
 * @brief The family a near preset is filed under.
 * @param[in] index  0 .. numNearPresets()-1
 * @return the family index for nearPresetFamily(), or 255 for the Off entry and for any index out of range -- 255
 *         belongs to no family
 */
int nearPresetCategory( int index );
/** @return how many families the near bank is sorted into */
int numNearPresetFamilies();
/**
 * @brief The name of a near family: the heading of one family of the bank, as the browser shows it.
 * @param[in] family  0 .. numNearPresetFamilies()-1
 * @return the name, "" when out of range
 */
const char *nearPresetFamily( int family );

/**
 * @brief Whether a parameter belongs to the Cosmos section.
 * @param[in] id  the parameter
 * @return true for the Cosmos layer's parameters
 */
inline bool isCosmosParam( ParamIdE id )
{
    return sectionOf( id ) == ParamSectionE::COSMOS;
}
/**
 * @brief Whether a parameter belongs to the Z-plane section.
 * @param[in] id  the parameter
 * @return true for the Z-plane layer's parameters
 */
inline bool isZPlaneParam( ParamIdE id )
{
    return sectionOf( id ) == ParamSectionE::Z_PLANE;
}
/**
 * @brief Whether a parameter belongs to the Strike section.
 * @param[in] id  the parameter
 * @return true for the Strike layer's parameters
 */
inline bool isStrikeParam( ParamIdE id )
{
    return sectionOf( id ) == ParamSectionE::STRIKE;
}
/**
 * @brief What a near PRESET carries: the Near Source and Near Events sections, Auto and Night excepted.
 *
 * Auto is a Near Events control that is not part of a near preset: it decides whether a
 * sound preset brings its own foreground, and a near preset chosen by hand must not switch it on.
 * Night (20.11.2026) is the player's guard for the night, which no preset may switch either way.
 * @param[in] id  the parameter
 * @return true for the near layer's parameters other than ForeAuto and ForeNight
 */
inline bool isNearParam( ParamIdE id )
{
    const ParamSectionE s = sectionOf( id );
    return ( s == ParamSectionE::NEAR_SOURCE || s == ParamSectionE::NEAR_EVENTS ) && id != ParamIdE::FORE_AUTO &&
           id != ParamIdE::FORE_NIGHT;
}
/**
 * @brief The layer as a whole, Auto included.
 *
 * `isNearParam` is what a near PRESET carries; this is what
 * a sound preset has to leave standing. The two differ by exactly one control, and the difference
 * matters: Auto is the player's setting about presets, not a setting a preset may make.
 * @param[in] id  the parameter
 * @return true for every Near Source and Near Events parameter
 */
inline bool isNearLayerParam( ParamIdE id )
{
    const ParamSectionE s = sectionOf( id );
    return s == ParamSectionE::NEAR_SOURCE || s == ParamSectionE::NEAR_EVENTS;
}
/**
 * @brief Whether a parameter is one of the morph controls (performance state).
 * @param[in] id  the parameter
 * @return true for the Morph section
 */
inline bool isMorphParam( ParamIdE id )
{
    return sectionOf( id ) == ParamSectionE::MORPH;
}
/**
 * @brief Whether a parameter is one of the eight macros (performance state).
 * @param[in] id  the parameter
 * @return true for the Macros section
 */
inline bool isMacroParam( ParamIdE id )
{
    return sectionOf( id ) == ParamSectionE::MACROS;
}
/**
 * @brief Whether a parameter is the map cursor or its switch (performance state).
 * @param[in] id  the parameter
 * @return true for the Map section
 */
inline bool isMapParam( ParamIdE id )
{
    return sectionOf( id ) == ParamSectionE::MAP;
}
/**
 * @brief Whether a parameter is one of the route controls (performance state).
 * @param[in] id  the parameter
 * @return true for the Route section
 */
inline bool isRouteParam( ParamIdE id )
{
    return sectionOf( id ) == ParamSectionE::ROUTE;
}
/**
 * @brief Whether a parameter is one of the clock controls (performance state).
 * @param[in] id  the parameter
 * @return true for the Clock section
 */
inline bool isClockParam( ParamIdE id )
{
    return sectionOf( id ) == ParamSectionE::CLOCK;
}
/**
 * @brief Morph controls, macros, the map cursor, the route and the clock are performance state, never part of any
 *        preset.
 * @param[in] id  the parameter
 * @return true when no preset, of any scope, may set or clear it
 */
inline bool isPerformanceParam( ParamIdE id )
{
    return isMorphParam( id ) || isMacroParam( id ) || isMapParam( id ) || isRouteParam( id ) || isClockParam( id );
}
/**
 * @brief Whether a preset of the given scope may SET a parameter.
 *
 * Performance state never, nor the near layer's Night (the player's guard for the night, 20.11.2026: a sound preset
 * applied as a whole would otherwise put it back to off at every journey step); the section scopes their own section;
 * Sound everything that is not the Cosmos or the near layer; Full everything else.
 * @param[in] id     the parameter
 * @param[in] scope  the preset's scope
 * @return true when a key naming @p id in a preset of that scope is applied
 */
inline bool inScope( ParamIdE id, PresetScopeE scope )
{
    if ( isPerformanceParam( id ) || id == ParamIdE::FORE_NIGHT ) return false;
    switch ( scope )
    {
    case PresetScopeE::COSMOS: return isCosmosParam( id );
    case PresetScopeE::Z_PLANE: return isZPlaneParam( id );
    case PresetScopeE::STRIKE: return isStrikeParam( id );
    case PresetScopeE::NEAR_LAYER: return isNearParam( id );
    case PresetScopeE::SOUND: return !isCosmosParam( id ) && !isNearParam( id );   // everything a sound preset owns
    case PresetScopeE::FULL: break;
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
 * @param[in] id     the parameter
 * @param[in] scope  the preset's scope
 * @return true when applying a preset of that scope first resets @p id to its default
 */
inline bool clearedBy( ParamIdE id, PresetScopeE scope )
{
    if ( scope == PresetScopeE::FULL && isNearLayerParam( id ) ) return false;
    return inScope( id, scope );
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
 * @param[in] path  the .ambientpack file
 * @return false if the file is missing or a line is malformed
 */
bool loadPresetPack( const char *path );
/**
 * @brief Loads every *.ambientpack in a directory, in sorted order so preset indices are reproducible.
 * @param[in] dir  the folder; a missing folder loads nothing
 * @return every *.ambientpack in a directory; returns how many loaded
 */
int loadPresetPacksIn( const char *dir );
/**
 * @brief Loads the packs from the default places.
 *
 * $AMBIENT_PACKS (';'-separated) when it is set; else, for a program that lies in a source tree's
 * bin folder, that tree's Library/Packs (repoLibraryFor(), 20.11.2026); else the user's own
 * Documents/Noctuary/Packs and the folders an installer writes to (Windows: ProgramData and
 * LocalAppData; elsewhere /usr/local/share and /usr/share). A pack found twice loads once. Each
 * folder's stamp is kept for libraryStatus().
 *
 * With $AMBIENT_PACKS set its folders are final, even when they yield nothing new -- a second
 * call in one process (a batch render) must not fall through to the installed copies. The
 * source tree's folder is final in the same way.
 *
 * @return how many packs were loaded by this call
 */
int loadDefaultPresetPacks();
/**
 * @brief A library's stamp: when its packs last changed and a hash of them (20.11.2026).
 *
 * The build keeps `library.stamp` in the source tree's Library/Packs (cmake/LibraryStamp.cmake) and compiles the
 * same stamp into the program; an installer copies the file with the packs. A folder without one predates the stamp.
 */
struct LibraryStampS
{
    std::string m_date;   ///< "YYYY-MM-DD hh:mm" (UTC), empty when there is no stamp
    std::string m_hash;   ///< the hash of the packs, empty when there is no stamp
};
/**
 * @brief Reads `library.stamp` in a packs folder.
 * @param[in] folder  the packs folder
 * @return its stamp; empty fields when the folder has none
 */
LibraryStampS readLibraryStamp( const char *folder );
/** @return the stamp of the library this program was built with (empty when the tree it was built in had none) */
LibraryStampS builtLibraryStamp();
/**
 * @brief Whether a loaded library is older than the one the program was built with.
 *
 * Not older when the program carries no stamp, or when the two hashes agree; older when the loaded one has no stamp
 * (it predates them) or an earlier date.
 * @param[in] loaded  the loaded folder's stamp
 * @param[in] built   the program's (builtLibraryStamp())
 * @return true when it is older
 */
bool libraryIsOlder( const LibraryStampS &loaded, const LibraryStampS &built );
/**
 * @brief The source tree's packs folder for a program that lies in that tree's bin folder.
 *
 * Walks up from the program's folder; where a folder named `bin` sits beside a `Library/Packs` that holds packs, that
 * is the answer. loadDefaultPresetPacks() reads it, and only it, when AMBIENT_PACKS is not set.
 * @param[in] programPath  the program's (or plug-in's) file, as programPath() gives it
 * @return the Library/Packs folder, or "" when the program is not in a source tree
 */
std::string repoLibraryFor( const char *programPath );
/** @return the file of the program or plug-in this code is linked into (Windows and Linux), or "" */
std::string programPath();
/** @brief Where the presets came from, against the library the program was built with. */
struct LibraryStatusS
{
    std::string   m_folder;          ///< the packs folder named: the first that gave packs, or the first older one
    std::string   m_how;             ///< why it was read: "AMBIENT_PACKS", "the source tree", "the default folders"
    LibraryStampS m_stamp;           ///< its stamp
    LibraryStampS m_built;           ///< the program's
    bool          m_older = false;   ///< libraryIsOlder( m_stamp, m_built )
};
/** @return the status of the packs loaded so far (an empty folder when none were) */
LibraryStatusS libraryStatus();
/** @return one line for the status line when the loaded library is older than the program's, else "" */
std::string libraryNotice();
/** @brief Unloads every pack: the list shrinks back to the built-ins, and every pack Preset's pointers die. */
void clearPresetPacks();
/** @return how many packs are loaded */
int numPresetPacks();
/**
 * @brief The name of a loaded pack (its "pack" line, else the file's stem).
 * @param[in] pack  0 .. numPresetPacks()-1
 * @return the name, "" when out of range
 */
const char *presetPackName( int pack );
/**
 * @brief The pack a preset came from, or -1 for a built-in.
 * @param[in] presetIndex  0 .. numPresets()-1
 * @return the pack index, or -1
 */
int presetPack( int presetIndex );
/**
 * @brief The near layer's Auto (13.09.2026): the foreground a sound preset of a pack brings with it.
 *
 * Rene's table per artist (Tools/library/near_by_artist.json, compiled to NearAuto.inc): a share
 * of the pack's presets get one at all, drawn by weight from the artist's list, and the near
 * preset's Every scaled by a factor of its class (often, now and then, seldom). Deterministic:
 * the draw is a hash of the preset's name, so a preset brings the same foreground every time.
 * Returns the near preset's index (0 = Near Off for the presets that get none), or -1 when the
 * pack has no table; `rateFactor` is what to multiply the near preset's Every by.
 * @param[in]  packName    the pack's name, as presetPackName() gives it
 * @param[in]  presetName  the preset's name
 * @param[out] rateFactor  receives the factor on the near preset's Every (1 when there is no table)
 * @return the near preset's index, 0 for none, -1 when the pack has no table
 */
int nearAutoPick( const char *packName, const char *presetName, float &rateFactor );

/**
 * @name The near bank learns the player's taste (20.11.2026, plan C2)
 *
 * Rene rated fifteen near presets by ear on 19.11.2026; the thirteen he liked were written into the generator by hand
 * (LIKED in Tools/make_layer_presets.py). Now the instrument asks: "like" and "never again" beside the near preset box,
 * kept by name in the player's own file (Documents/Noctuary/near_taste.txt, beside the favourites), read by the
 * generator as well (`--taste`). In the instrument a "never again" preset is not drawn by Auto (nearAutoPick) any
 * more, at once; a liked one is drawn NearLikedFactor times as often as its place in the artist's table says.
 *
 * The file is plain text, one entry a line, for a hand to edit: `like <name>` or `never <name>`; `#` begins a comment,
 * anything else is ignored, and a name given twice counts as its last line says.
 * @{ */
/** @brief A player's verdict on a near preset. */
enum class NearTasteE : unsigned char
{
    NONE = 0,    ///< no verdict: Auto draws it at its table weight
    LIKED = 1,   ///< liked: drawn NearLikedFactor times as often
    NEVER = 2    ///< never again: Auto does not draw it
};
/**
 * @brief How much more often Auto draws a liked near preset. Twice: an artist's list holds about a hundred and thirty
 *        entries in groups Rene weighted per artist, so a handful of likes at two times stays a lean inside his
 *        table rather than replacing it (ten likes of average weight move from about 8 % of the draws to about
 *        14 %); and it is the factor the generator gives its liked presets, which get twice the variants.
 */
constexpr float NearLikedFactor = 2.0f;
/** @brief The two lists of the taste file, by name, in the file's order; names the bank does not have are kept. */
struct NearTasteLists
{
    std::vector<std::string> m_liked;   ///< `like` lines
    std::vector<std::string> m_never;   ///< `never` lines
};
/**
 * @brief Reads the taste file's text. Comments, empty lines and lines it does not understand are skipped.
 * @param[in] text  the file's content
 * @return the lists (a name in both ends up in the one of its last line)
 */
NearTasteLists parseNearTaste( const std::string &text );
/**
 * @brief The taste file's text: a comment saying what it is, then the liked names, then the never-again ones.
 * @param[in] lists  the lists
 * @return the text, `\n` line ends
 */
std::string nearTasteText( const NearTasteLists &lists );
/**
 * @brief Reads a taste file. A missing or unreadable file gives empty lists.
 * @param[in] path  the file
 * @return the lists
 */
NearTasteLists readNearTasteFile( const std::string &path );
/**
 * @brief Writes a taste file atomically: into a file beside it, then renamed over it, so a crash never leaves half a file.
 * @param[in] path   the file (its folder must exist)
 * @param[in] lists  the lists
 * @return whether it was written
 */
bool writeNearTasteFile( const std::string &path, const NearTasteLists &lists );
/**
 * @brief Gives the instrument the lists: each near preset of the bank named in them gets its verdict, every other one
 *        none. Any thread; nearAutoPick reads the verdicts lock-free (one atomic byte per near preset).
 * @param[in] lists  the lists
 */
void useNearTaste( const NearTasteLists &lists );
/**
 * @brief The verdict on a near preset of the bank.
 * @param[in] nearIndex  0 .. numNearPresets()-1
 * @return its verdict, NONE out of range
 */
NearTasteE nearTaste( int nearIndex );
/** @} */
/**
 * @brief The folders the library may lie in, in the order resolveLibraryFile searches them: for anything
 *        that lists a library folder (the journeys under \<root\>/Journeys, say) rather than one file.
 * @param[out] buf  receives the roots, NUL-terminated
 * @param[in]  cap  size of @p buf; roots that do not fit are left out
 * @return ';'-separated into buf; returns how many roots were written
 */
int libraryRoots( char *buf, int cap );
constexpr int PresetFiles = 4;   ///< how many files a pack preset may name: texture, wavetable, impulse, impulse B
/**
 * @brief Absolute path of the file a pack preset names, or an empty string.
 *
 * `which`: 0 texture, 1 wavetable, 2 impulse, 3 impulse B.
 *
 * Resolved against the pack's folder when the pack was loaded; the texture field may hold up to
 * four ';'-separated files, one per slot, and the separators are kept.
 * @param[in] presetIndex  0 .. numPresets()-1; a built-in has no files
 * @param[in] which        0 .. PresetFiles-1
 * @return the path, or "" for a built-in, an unnamed file or an index out of range
 */
const char *presetFilePath( int presetIndex, int which );
/**
 * @brief Finds a file of the library by its library-relative name.
 *
 * A file named relative to the library's root ("Archive/NASA/Historical/...flac"), as the
 * compiled-in banks name theirs: looked for under $AMBIENT_LIBRARY, beside every loaded pack
 * folder (the library is the folder the Packs folder is in), in the user's and the installer's
 * Noctuary folders, and last relative to the working directory (the source tree's Library).
 * Empty if nowhere. The .flac beside a named .wav counts, as everywhere else.
 * @param[in] relative  the path as the bank names it
 * @return the first existing absolute path, or ""
 */
std::string resolveLibraryFile( const char *relative );
/** @} */

/**
 * @brief Parse a value for `d` from text: numbers, "on"/"off", or a choice name.
 *
 * A choice name is looked up first; a switch also accepts true/false and yes/no; anything else
 * goes through atof and is clamped to the parameter's range, and "nan" (or no number at all)
 * gives the default.
 *
 * @param[in] d     the parameter's description, for its kind, range, default and choice names
 * @param[in] text  the value as written in a preset, a score or an OSC string
 * @return      the value in the parameter's real range
 */
float paramValueFromText( const ParamDesc &d, const char *text );

/**
 * @brief Calls set(ParamId, value) for every parameter in scope: defaults first, then the
 *        preset's settings. Returns false if the settings string names an unknown parameter.
 *
 * Defaults go to every parameter clearedBy() the scope, then each "key=value" of the settings to
 * its parameter if inScope(). Parsing stops at the first malformed item (a key over 63 characters,
 * a value over 63, a missing '='); an unknown key is skipped and remembered in the result. No
 * allocation, so the engine may apply on the audio thread.
 * @param[in] p      the preset
 * @param[in] set    receives every parameter that is cleared or set
 * @param[in] scope  which layer this apply is
 * @return true when every key was known and every item well formed
 * @tparam SetFn  callable as set(ParamId, float)
 */
template <class SetFnT>
bool applyPreset( const Preset &p, SetFnT &&set, PresetScopeE scope = PresetScopeE::FULL )
{
    for ( const ParamDesc &d : paramTable() )
        if ( clearedBy( d.m_id, scope ) ) set( d.m_id, d.m_def );
    bool        ok = true;
    const char *s = p.m_settings;
    while ( s && *s )
    {
        const char *eq = s;
        while ( *eq && *eq != '=' && *eq != ';' ) ++eq;
        if ( *eq != '=' )
        {
            ok = false;
            break;
        }
        char      key[64];
        const int klen = static_cast<int>( eq - s );
        if ( klen <= 0 || klen >= 64 )
        {
            ok = false;
            break;
        }
        for ( int i = 0; i < klen; ++i ) key[i] = s[i];
        key[klen] = 0;
        const char *vs = eq + 1;
        const char *ve = vs;
        while ( *ve && *ve != ';' ) ++ve;
        char      val[64];
        const int vlen = static_cast<int>( ve - vs );
        if ( vlen >= 64 )
        {
            ok = false;
            break;
        }
        for ( int i = 0; i < vlen; ++i ) val[i] = vs[i];
        val[vlen] = 0;
        if ( const ParamDesc *d = findParam( key ) )
        {
            if ( inScope( d->m_id, scope ) ) set( d->m_id, paramValueFromText( *d, val ) );
        }
        else
            ok = false;
        s = ( *ve == ';' ) ? ve + 1 : ve;
    }
    return ok;
}

}   // namespace ambient
