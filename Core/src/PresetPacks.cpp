/**
 * @file PresetPacks.cpp
 * @brief Runtime preset packs.
 *
 * The preset list the rest of the program sees is the
 * built-in presets followed by every loaded pack, so the DAW programs, the map, the browser and
 * routes-by-name all pick packs up without knowing they exist.
 *
 * A pack is a text file of presets (the format is written out in Presets.h). This file reads
 * them, keeps them, and answers the preset questions the header declares -- preset(), presetMeta(),
 * presetFilePath(), the family and pack names -- by splicing the packs behind the compiled-in
 * tables of Presets.cpp and PresetMeta.cpp. Everything here is message-thread only: loading
 * allocates and resolves paths, and the views handed out point into the pack strings, which is why
 * the packs are held by pointer and stay put until clearPresetPacks(). The same file also owns the
 * library roots -- where sample files named relative to the library are looked for -- and the near
 * layer's Auto draw, the hash that gives a pack preset its foreground.
 */
#include "ambient/Presets.h"
#include "ambient/PresetMeta.h"
#include "ambient/WavFile.h"   // resolveAudioFile, for the library-relative files of the banks
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <memory>

namespace ambient {

namespace {

/**
 * @brief One preset of a pack, as read from its line: the strings a Preset will point into, and
 *        its measured metadata.
 *
 * The string members are the fields of the pack line in their order; each optional one is empty
 * when the line stopped before it, and the Preset view then carries nullptr for it.
 */
struct PackEntry {
    std::string name,        ///< the preset's name
                settings,    ///< its "key=value;..." settings, format-1 names translated
                texture,     ///< the texture file(s) for the source slots, up to four separated by ';'
                wavetable,   ///< the wavetable file for the User table
                impulse,     ///< the impulse response of the convolution Room
                mod,         ///< the modulation matrix rows
                envs,        ///< the envelope shapes, '~' between them
                impulseB;    ///< the impulse Room Morph goes to
    PresetMeta  meta{ 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0, 0, 0.0f, 0.5f, 0.5f, 0.5f, { -1, -1 }, -1 };   ///< the metadata field, or this neutral centre-of-the-map default when the line has none; the family is filled in by loadPresetPack()
};
/** @brief One loaded pack file. */
struct Pack {
    std::string name;                 ///< the "pack" line's name, else the file's stem; also the family name of its presets
    std::string dir;                  ///< the pack file's folder, for relative sample paths
    std::vector<PackEntry> entries;   ///< its presets, in file order
};

/**
 * @brief The loaded packs, in load order.
 *
 * Held by pointer, not by value: the views below hand out const char* into these strings, and a
 * vector of Packs moves its elements when it grows -- which moves every short string with it
 * (they live inside the object). Rebuilding all the views after every pack was the workaround;
 * this is the reason it was needed.
 *
 * @return  the one list, a function-local static so that it exists before any caller
 */
std::vector<std::unique_ptr<Pack>>& packs() { static std::vector<std::unique_ptr<Pack>> p; return p; }
/**
 * @brief The pack presets as the rest of the program sees them, in the order of the packs and of
 *        their lines; preset(index) for an index past the built-ins reads here.
 *
 * Preset objects handed out point into the pack strings, so they stay valid until clearPresetPacks().
 *
 * @return  the one list
 */
std::vector<Preset>& views() { static std::vector<Preset> v; return v; }
/**
 * @brief The resolved file paths of the pack presets: absolute, kPresetFiles per entry, in the
 *        order of views() (texture, wavetable, impulse, impulse B).
 * @return  the one list
 */
std::vector<std::string>& paths() { static std::vector<std::string> p; return p; }
/**
 * @brief The canonical paths of the pack files already read, so that a file loads once per process.
 * @return  the one list
 */
std::vector<std::string>& loadedPaths() { static std::vector<std::string> p; return p; }
/**
 * @brief One pointer per pack preset, in the order the views are in.
 *
 * presetMeta used to walk the pack
 * list to find out which pack an index belonged to -- and the map's neighbour search asks for
 * every preset's position on every audio block, so with 42 packs that was a hundred and eighty
 * thousand iterations per block to answer eight thousand questions.
 *
 * @return  the one list, pointing into the PackEntry::meta of the packs
 */
std::vector<const PresetMeta*>& metaViews() { static std::vector<const PresetMeta*> v; return v; }
/**
 * @brief The pack each view came from, in the order the views are in (the near layer's Auto asks).
 * @return  the one list of pack indices into packs()
 */
std::vector<int>& viewPacks() { static std::vector<int> v; return v; }

/** @brief One candidate foreground of an artist's near table (NearAuto.inc, compiled from Tools/library/near_by_artist.json). */
struct NearAutoEntry {
    const char* preset;   ///< the near preset's name (nearPreset(i).name)
    float weight;         ///< its share of the draw within the pack's list
    float rateLo,         ///< the lowest factor the near preset's Every is multiplied by
    rateHi;               ///< and the highest; the factor is drawn uniformly between the two
};
/** @brief One pack's near table: which share of its presets get a foreground at all, and where its candidates lie in kNearAutoEntries. */
struct NearAutoPack  {
    const char* pack;   ///< the pack's name as loadPresetPack() reads it
    float share;        ///< 0 .. 1, the fraction of the pack's presets that bring a foreground
    int first,          ///< index of the pack's first entry in kNearAutoEntries
    count;              ///< how many entries follow it there
};
#include "NearAuto.inc"

/**
 * @brief One pack's presets appended to the views.
 *
 * Called once per pack as it is loaded: the whole list
 * used to be rebuilt every time, so loading 42 packs of 200 presets did 176 000 entries' worth of
 * work instead of 8400, three filesystem path resolutions each. That was 678 ms of the 2.1 s
 * before the window appeared, and 0.4 s of every offline render.
 *
 * @param pk  the pack, already in packs() (its index there is what viewPacks() records)
 */
void appendViews(const Pack& pk)
{
    {
        int packIndex = -1;
        for (size_t i = 0; i < packs().size(); ++i) if (packs()[i].get() == &pk) packIndex = static_cast<int>(i);
        for (const PackEntry& e : pk.entries) {
            metaViews().push_back(&e.meta);
            viewPacks().push_back(packIndex);
            views().push_back(Preset{ e.name.c_str(), e.settings.c_str(),
                                      e.texture.empty() ? nullptr : e.texture.c_str(),
                                      e.wavetable.empty() ? nullptr : e.wavetable.c_str(),
                                      e.impulse.empty() ? nullptr : e.impulse.c_str(),
                                      e.mod.empty() ? nullptr : e.mod.c_str(),
                                      e.envs.empty() ? nullptr : e.envs.c_str(),
                                      e.impulseB.empty() ? nullptr : e.impulseB.c_str() });
            const std::filesystem::path dir(pk.dir);
            auto resolveOne = [&dir](const std::string& rel) {
                return rel.empty() ? std::string() : (dir / rel).lexically_normal().string();
            };
            // The texture field may name up to four files separated by ';', one per source slot.
            // Each is resolved against the pack's folder and the separators are kept, so the host
            // can split the result the same way.
            auto resolve = [&resolveOne](const std::string& field) {
                if (field.find(';') == std::string::npos) return resolveOne(field);
                std::string out;
                size_t start = 0;
                while (true) {
                    const size_t semi = field.find(';', start);
                    std::string part = field.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
                    while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) part.erase(part.begin());
                    while (!part.empty() && (part.back() == ' ' || part.back() == '\t')) part.pop_back();
                    out += resolveOne(part);
                    if (semi == std::string::npos) break;
                    out += ';';
                    start = semi + 1;
                }
                return out;
            };
            paths().push_back(resolve(e.texture));
            paths().push_back(resolve(e.wavetable));
            paths().push_back(resolve(e.impulse));
            paths().push_back(resolve(e.impulseB));
        }
    }
}

/** @brief Throws every view away and appends every loaded pack again; what clearPresetPacks() does after emptying the pack list. */
void rebuildViews()
{
    views().clear();
    paths().clear();
    metaViews().clear();
    viewPacks().clear();
    for (const auto& pk : packs()) appendViews(*pk);
}

/**
 * @brief The string without leading and trailing blanks, tabs and carriage returns -- the last for
 *        pack files written on Windows and read elsewhere.
 * @param s  the text as read
 * @return   the trimmed copy
 */
std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

/**
 * @brief Splits a string at every occurrence of a separator, keeping empty fields.
 * @param s    the text
 * @param sep  the separator character ('|' between the fields of a pack line, ' ' inside the
 *             metadata field, ';' between the folders of AMBIENT_PACKS)
 * @return     the fields, one more than there are separators; a single empty string for empty input
 */
std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == sep) { out.push_back(s.substr(start, i - start)); start = i + 1; }
    return out;
}

/**
 * @brief Translates a format-1 settings string into today's type names.
 *
 * A pack without a "format" line was written before the classic wavetable arrived, and calls the
 * spectral table type "Wavetable". That type is Harmonic now and the name belongs to the classic
 * one, so such a pack is read with the old name translated -- every pack in the library, and every
 * pack anybody wrote, keeps the sound it was voiced with. A pack that says "format 2" means what it
 * says.
 *
 * @param settings  the preset's "key=value;..." settings as the old pack wrote them
 * @return          the same settings with every whole "_type=Wavetable" value turned into
 *                  "_type=Harmonic"
 */
std::string migrateFormat1(std::string settings)
{
    const std::string from = "_type=Wavetable", to = "_type=Harmonic";
    for (size_t at = settings.find(from); at != std::string::npos; at = settings.find(from, at + to.size())) {
        const size_t end = at + from.size();
        if (end < settings.size() && settings[end] != ';') continue;   // a longer word that only starts the same
        settings.replace(at, from.size(), to);
    }
    return settings;
}

} // namespace

bool loadPresetPack(const char* path)
{
    if (path == nullptr) return false;
    // Loading the same file twice is doing nothing, not doing it twice. The default folders have
    // always been documented that way ("a pack found twice loads once") but the guard was only in
    // the folder scan, so a second call with the same path appended every preset again. It is what
    // lets one process render preset after preset (ambient_render --batch) without the library
    // growing under it.
    {
        std::error_code ec;
        std::string key = std::filesystem::weakly_canonical(std::filesystem::path(path), ec).string();
        if (ec) key = path;
        auto& seen = loadedPaths();
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) return false;
    }
    std::ifstream f(path);
    if (!f) return false;
    Pack pack;
    int format = 1;   // no "format" line: written before the classic wavetable (see migrateFormat1)
    pack.dir = std::filesystem::path(path).parent_path().string();
    pack.name = std::filesystem::path(path).stem().string();
    std::string line;
    while (std::getline(f, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t.rfind("pack ", 0) == 0) { pack.name = trim(t.substr(5)); continue; }
        if (t.rfind("format ", 0) == 0 && t.find('|') == std::string::npos) { format = std::atoi(t.substr(7).c_str()); continue; }
        const std::vector<std::string> fields = split(t, '|');
        if (fields.size() < 2) return false;
        PackEntry e;
        e.name = trim(fields[0]);
        e.settings = format < 2 ? migrateFormat1(trim(fields[1])) : trim(fields[1]);
        if (e.name.empty()) return false;
        if (fields.size() > 2) {   // x y bright motion width noisy bass density tagbits
            const std::vector<std::string> m = split(trim(fields[2]), ' ');
            float v[8] = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
            size_t j = 0;
            for (const std::string& tok : m) {
                if (tok.empty()) continue;
                if (j < 8) v[j] = static_cast<float>(std::atof(tok.c_str()));
                else if (j == 8) e.meta.tags = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 0));
                else if (j == 9) e.meta.loudDb = static_cast<float>(std::atof(tok.c_str()));   // optional: older packs have none
                else if (j == 10) e.meta.evolve = static_cast<float>(std::atof(tok.c_str()));  // the drone descriptors, added in 1.11.0
                else if (j == 11) e.meta.rough = static_cast<float>(std::atof(tok.c_str()));
                else if (j == 12) e.meta.wet = static_cast<float>(std::atof(tok.c_str()));
                else if (j == 13) e.meta.phrase[0] = static_cast<short>(std::atoi(tok.c_str()));   // what it sounds like,
                else if (j == 14) e.meta.phrase[1] = static_cast<short>(std::atoi(tok.c_str()));   // chosen by CLAP
                else if (j == 15) e.meta.cluster = static_cast<short>(std::atoi(tok.c_str()));      // the group it was laid out in
                ++j;
            }
            e.meta.x = v[0]; e.meta.y = v[1]; e.meta.bright = v[2]; e.meta.motion = v[3];
            e.meta.width = v[4]; e.meta.noisy = v[5]; e.meta.bass = v[6]; e.meta.density = v[7];
        }
        if (fields.size() > 3) e.texture = trim(fields[3]);
        if (fields.size() > 4) e.wavetable = trim(fields[4]);
        if (fields.size() > 5) e.impulse = trim(fields[5]);
        if (fields.size() > 6) e.mod = trim(fields[6]);
        if (fields.size() > 7) e.envs = trim(fields[7]);
        if (fields.size() > 8) e.impulseB = trim(fields[8]);   // the impulse Room Morph goes to
        pack.entries.push_back(std::move(e));
    }
    if (pack.entries.empty()) return false;
    // The same pack can sit in two of the searched folders at once -- one copy put there by the
    // installer for everybody, one in the user's own Documents. Loading it twice would double
    // every preset in it, so the first one found wins.
    for (const auto& have : packs()) if (have->name == pack.name) return false;
    // Every preset of a pack belongs to that pack's family, appended after the built-in families.
    const int family = builtinPresetFamilyCount() + static_cast<int>(packs().size());
    for (PackEntry& e : pack.entries) e.meta.family = family;
    packs().push_back(std::make_unique<Pack>(std::move(pack)));
    appendViews(*packs().back());
    // Remembered as loaded only now that it is. Remembered at the top, a pack that failed to
    // parse -- or was simply not there yet -- could never be tried again in this process.
    {
        std::error_code ec;
        std::string key = std::filesystem::weakly_canonical(std::filesystem::path(path), ec).string();
        if (ec) key = path;
        loadedPaths().push_back(key);
    }
    return true;
}

int loadPresetPacksIn(const char* dir)
{
    if (dir == nullptr) return 0;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return 0;
    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        if (entry.is_regular_file(ec) && entry.path().extension() == ".ambientpack") files.push_back(entry.path().string());
    std::sort(files.begin(), files.end());   // packs load in a stable order, so preset indices are reproducible
    int n = 0;
    for (const std::string& f : files) if (loadPresetPack(f.c_str())) ++n;
    return n;
}

namespace {
/**
 * @brief The folders the library may lie in, in the order they are searched.
 *
 * $AMBIENT_LIBRARY first when it is set; then the parent of every loaded pack's folder (the
 * library is the folder the Packs folder is in); then the user's Documents/Noctuary and the
 * folders an installer writes to (ProgramData and LocalAppData on Windows, /usr/local/share and
 * /usr/share elsewhere); and last "Library" and "." relative to the working directory, which is
 * where the source tree keeps it. A folder is listed once even if two packs point at it.
 *
 * @return  the candidate roots, each an absolute or working-directory-relative path
 */
std::vector<std::string> rootsOfTheLibrary()
{
    std::vector<std::string> roots;
    if (const char* env = std::getenv("AMBIENT_LIBRARY")) roots.push_back(env);
    for (const std::string& packFile : loadedPaths()) {
        const std::string root = std::filesystem::path(packFile).parent_path().parent_path().string();
        if (!root.empty() && std::find(roots.begin(), roots.end(), root) == roots.end()) roots.push_back(root);
    }
    const char* home = std::getenv("USERPROFILE");
    if (home == nullptr) home = std::getenv("HOME");
    if (home != nullptr) roots.push_back((std::filesystem::path(home) / "Documents" / "Noctuary").string());
#if defined(_WIN32)
    if (const char* shared = std::getenv("ProgramData")) roots.push_back((std::filesystem::path(shared) / "Noctuary").string());
    if (const char* local = std::getenv("LOCALAPPDATA")) roots.push_back((std::filesystem::path(local) / "Noctuary").string());
#else
    roots.push_back("/usr/local/share/Noctuary");
    roots.push_back("/usr/share/Noctuary");
#endif
    roots.push_back("Library");
    roots.push_back(".");
    return roots;
}
} // namespace

int libraryRoots(char* buf, int cap)
{
    const std::vector<std::string> roots = rootsOfTheLibrary();
    std::string joined;
    for (const std::string& r : roots) { if (!joined.empty()) joined += ';'; joined += r; }
    if (buf != nullptr && cap > 0) { std::strncpy(buf, joined.c_str(), static_cast<size_t>(cap - 1)); buf[cap - 1] = 0; }
    return static_cast<int>(roots.size());
}

std::string resolveLibraryFile(const char* relative)
{
    if (relative == nullptr || *relative == 0) return {};
    const std::vector<std::string> roots = rootsOfTheLibrary();
    // A name ending in a slash is a folder of recordings (the near layer plays one of them at
    // random per event), and is resolved to the folder itself rather than to a file in it.
    const std::string rel(relative);
    const bool folder = rel.back() == '/' || rel.back() == '\\';
    for (const std::string& root : roots) {
        const std::string candidate = (std::filesystem::path(root) / rel).string();
        if (folder) {
            std::error_code ec;
            if (std::filesystem::is_directory(candidate, ec)) return candidate;
            continue;
        }
        const std::string got = resolveAudioFile(candidate.c_str());
        if (!got.empty()) return got;
    }
    return {};
}

int loadDefaultPresetPacks()
{
    // AMBIENT_PACKS wins (a folder, or several separated by ';'). Otherwise two places are read,
    // the user's own first: Documents/Noctuary/Packs, and the shared folder an installer can
    // write to for everybody on the machine. A pack that is in both loads once (see above), and
    // nothing anywhere simply means no packs.
    if (const char* env = std::getenv("AMBIENT_PACKS")) {
        int n = 0;
        for (const std::string& dir : split(env, ';')) if (!dir.empty()) n += loadPresetPacksIn(dir.c_str());
        // Final, whatever it found. It used to fall through to the default folders when nothing
        // was loaded -- which is exactly the case on the SECOND call in one process (a batch
        // render), because everything in the environment folder is already loaded by then. The
        // batch then read the installed copies out of ProgramData on top: an older library
        // under a measurement that had asked, by setting the variable, for this one only.
        return n;
    }
    int n = 0;
    const char* home = std::getenv("USERPROFILE");
    if (home == nullptr) home = std::getenv("HOME");
    if (home != nullptr)
        n += loadPresetPacksIn((std::filesystem::path(home) / "Documents" / "Noctuary" / "Packs").string().c_str());
#if defined(_WIN32)
    // Where an installer puts them: ProgramData when it ran for everybody, LocalAppData when it
    // ran for one user without administrator rights.
    if (const char* shared = std::getenv("ProgramData"))
        n += loadPresetPacksIn((std::filesystem::path(shared) / "Noctuary" / "Packs").string().c_str());
    if (const char* local = std::getenv("LOCALAPPDATA"))
        n += loadPresetPacksIn((std::filesystem::path(local) / "Noctuary" / "Packs").string().c_str());
#else
    n += loadPresetPacksIn("/usr/local/share/Noctuary/Packs");
    n += loadPresetPacksIn("/usr/share/Noctuary/Packs");
#endif
    return n;
}

void clearPresetPacks() { packs().clear(); loadedPaths().clear(); rebuildViews(); }
int  numPresetPacks() { return static_cast<int>(packs().size()); }
const char* presetPackName(int pack) { return (pack >= 0 && pack < numPresetPacks()) ? packs()[static_cast<size_t>(pack)]->name.c_str() : ""; }

int numPresets() { return builtinPresetCount() + static_cast<int>(views().size()); }

int presetPack(int presetIndex)
{
    const int i = presetIndex - builtinPresetCount();
    return i >= 0 && i < static_cast<int>(viewPacks().size()) ? viewPacks()[static_cast<size_t>(i)] : -1;
}

namespace {
/**
 * @brief A number in [0, 1) that depends on a text and a salt and on nothing else.
 *
 * The draw is a hash, not a random number: FNV-1a over the preset's name (and a salt per
 * question), folded to [0, 1). A preset asks three questions -- whether it gets a foreground at
 * all, which one, and how often it speaks -- and gets the same three answers every time.
 *
 * @param text  the preset's name, hashed byte by byte
 * @param salt  which question is being asked (1, 2 or 3 in nearAutoPick), mixed into the seed
 * @return      the top 53 bits of the finalised hash as a double in [0, 1)
 */
double hashUnit(const char* text, unsigned salt)
{
    uint64_t h = 1469598103934665603ull ^ (static_cast<uint64_t>(salt) * 0x9E3779B97F4A7C15ull);
    for (const unsigned char* c = reinterpret_cast<const unsigned char*>(text); *c; ++c) { h ^= *c; h *= 1099511628211ull; }
    h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
    return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
}
} // namespace

int nearAutoPick(const char* packName, const char* presetName, float& rateFactor)
{
    rateFactor = 1.0f;
    if (packName == nullptr || presetName == nullptr) return -1;
    const NearAutoPack* pk = nullptr;
    for (int i = 0; i < kNumNearAutoPacks; ++i) if (std::strcmp(kNearAutoPacks[i].pack, packName) == 0) { pk = &kNearAutoPacks[i]; break; }
    if (pk == nullptr || pk->count <= 0) return -1;
    if (hashUnit(presetName, 1u) >= static_cast<double>(pk->share)) return 0;   // no foreground for this one: Near Off
    double total = 0.0;
    for (int i = 0; i < pk->count; ++i) total += kNearAutoEntries[pk->first + i].weight;
    double u = hashUnit(presetName, 2u) * total;
    const NearAutoEntry* pick = &kNearAutoEntries[pk->first + pk->count - 1];
    for (int i = 0; i < pk->count; ++i) {
        const NearAutoEntry& e = kNearAutoEntries[pk->first + i];
        if (u < e.weight) { pick = &e; break; }
        u -= e.weight;
    }
    rateFactor = pick->rateLo + (pick->rateHi - pick->rateLo) * static_cast<float>(hashUnit(presetName, 3u));
    for (int i = 0; i < numNearPresets(); ++i) if (std::strcmp(nearPreset(i).name, pick->preset) == 0) return i;
    return -1;
}

const Preset& preset(int index)
{
    const int b = builtinPresetCount();
    if (index < b) return builtinPreset(index);
    const size_t i = static_cast<size_t>(index - b);
    return i < views().size() ? views()[i] : builtinPreset(0);
}

const char* presetFilePath(int presetIndex, int which)
{
    const int b = builtinPresetCount();
    if (which < 0 || which >= kPresetFiles) return "";
    const size_t i = static_cast<size_t>(presetIndex - b) * kPresetFiles + static_cast<size_t>(which);
    return (presetIndex >= b && i < paths().size()) ? paths()[i].c_str() : "";
}

// ---------------------------------------------------------------- metadata

int numPresetMeta() { return builtinPresetMetaCount() > 0 ? numPresets() : 0; }

const PresetMeta& presetMeta(int index)
{
    static const PresetMeta none = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0, 0, 0.0f, 0.5f, 0.5f, 0.5f, { -1, -1 }, -1 };
    const int b = builtinPresetCount();
    if (index < b) return builtinPresetMeta(index);
    const size_t i = static_cast<size_t>(index - b);
    return i < metaViews().size() ? *metaViews()[i] : none;
}

int numPresetFamilies() { return builtinPresetFamilyCount() + numPresetPacks(); }

const char* presetFamilyName(int family)
{
    const int b = builtinPresetFamilyCount();
    return family < b ? builtinPresetFamilyName(family) : presetPackName(family - b);
}

} // namespace ambient
