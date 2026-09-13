// Noctuary -- a sentence about a preset.
//
// Six thousand eight hundred names tell you nothing. u-he's browsers put a line of prose under
// every patch, and the reason it works is that the line says what the thing IS, not what it is
// called. Ours is not written by hand and not invented: the first half comes from what the preset
// measured (the same ranks the map is laid out from), the second from what its settings actually
// switch on. Nothing here is a guess -- if the line says "the Cosmos is open", cosmos_send is up.
#include "ambient/Presets.h"
#include "ambient/PresetMeta.h"
#include "ambient/Params.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ambient {
namespace {

// The settings string is "key=value;key=value". Only a handful of keys are ever asked for, so it
// is walked rather than parsed into a map.
std::string valueOf(const char* settings, const char* key)
{
    if (settings == nullptr || key == nullptr) return {};
    const size_t klen = std::strlen(key);
    for (const char* p = settings; *p != 0; ) {
        const char* eq = std::strchr(p, '=');
        if (eq == nullptr) break;
        const char* end = std::strchr(eq, ';');
        if (end == nullptr) end = eq + std::strlen(eq);
        if (static_cast<size_t>(eq - p) == klen && std::strncmp(p, key, klen) == 0)
            return std::string(eq + 1, static_cast<size_t>(end - eq - 1));
        p = (*end == 0) ? end : end + 1;
    }
    return {};
}

float numberOf(const char* settings, const char* key, float fallback = 0.0f)
{
    const std::string v = valueOf(settings, key);
    if (v.empty()) return fallback;
    return static_cast<float>(std::atof(v.c_str()));
}


// "a, b and c" out of a list -- the last comma is what makes a sentence read as English.
std::string listOf(const std::vector<std::string>& v)
{
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) out += (i + 1 == v.size()) ? " and " : ", ";
        out += v[i];
    }
    return out;
}

const char* sourceWord(const std::string& type)
{
    if (type == "Additive") return "an additive bank";
    if (type == "Table" || type == "Wavetable") return "a wavetable";
    if (type == "Harmonic") return "a harmonic table";
    if (type == "FM") return "an FM pair";
    if (type == "Texture") return "a grain texture";
    if (type == "Noise") return "noise";
    if (type == "Stretch") return "a stretched recording";
    if (type == "Bow") return "a bowed string";
    if (type == "Spectral") return "a spectral model";
    if (type == "Flute") return "a blown flute";
    if (type == "Murmur") return "a murmuring voice";
    if (type == "Bowl") return "a singing bowl";
    if (type == "Ice") return "creaking ice";
    if (type == "Drops") return "water drops";
    if (type == "Clip") return "a recording, played straight";
    if (type == "Whistler") return "a whistler falling out of the sky";
    if (type == "Shaker") return "a seed pod shaken";
    if (type == "Chime") return "struck bronze";
    if (type == "Geiger") return "a Geiger counter";
    if (type == "Tube") return "a fluorescent tube";
    if (type == "Krell") return "the Krell's circuits";
    if (type == "Beacon") return "a beacon's packet";
    if (type == "Morse") return "a number station";
    if (type == "Dial") return "a shortwave dial turned";
    return nullptr;
}

}   // namespace

// One or two sentences: what it sounds like, then what is in it.
std::string presetDescription(int index)
{
    if (index < 0 || index >= numPresets()) return {};
    const Preset& p = preset(index);
    const char* st = p.settings;
    std::string character;
    if (numPresetMeta() == numPresets()) {
        const PresetMeta& m = presetMeta(index);
        // The three ranks furthest from the middle, said in words -- the same vocabulary the map's
        // groups are named with, so a preset's line and the group it sits in agree.
        struct Axis { float v; const char* hi; const char* lo; };
        const Axis ax[] = {
            { m.bright, "bright", "dark" }, { m.motion, "moving", "calm" },
            { m.width, "wide", "narrow" },  { m.noisy, "noisy", "tonal" },
            { m.bass, "deep", "light" },    { m.density, "dense", "sparse" },
            { m.evolve, "evolving", "still" }, { m.rough, "rough", "smooth" },
            { m.wet, "distant", "close" },
        };
        std::vector<std::pair<float, std::string>> picked;
        for (const Axis& a : ax) {
            const float d = a.v - 0.5f;
            if (std::abs(d) > 0.22f) picked.push_back({ -std::abs(d), std::string(d > 0.0f ? a.hi : a.lo) });
        }
        std::sort(picked.begin(), picked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<std::string> words;
        for (size_t i = 0; i < picked.size() && i < 3; ++i) words.push_back(picked[i].second);
        character = listOf(words);
        if (!character.empty()) character[0] = static_cast<char>(std::toupper(character[0]));
    }

    // What is in it. Only what the settings actually say. Three groups, because a list that mixes
    // "an additive bank" with "conducted" reads like a machine wrote it: what makes the sound,
    // how it is played, and what it is put through.
    std::vector<std::string> parts, extras;
    {
        std::vector<std::string> src;
        const char* const slotKeys[4] = { "src1_type", "src2_type", "src3_type", "src4_type" };
        for (int k = 0; k < 4; ++k) {
            std::string t = valueOf(st, slotKeys[k]);
            if (k == 0 && t.empty()) t = "Additive";          // slot one's default
            if (t.empty() || t == "Off") continue;
            if (const char* w = sourceWord(t)) {
                bool already = false;
                for (const auto& s : src) if (s == w) already = true;
                if (!already) src.push_back(w);
            }
        }
        if (numberOf(st, "strands", 1.0f) > 1.5f && !src.empty() && src[0] == "an additive bank")
            src[0] = "a strand bank";
        if (!src.empty()) parts.push_back(listOf(src));
    }
    {
        const std::string scale = valueOf(st, "scale");
        if (!scale.empty() && scale != "Equal") parts.push_back("tuned " + scale);
    }
    parts.push_back(valueOf(st, "brain_on") == "off" ? "played from the keys" : "conducted");
    if (numberOf(st, "cosmos_send") > 0.05f) extras.push_back("the Cosmos open");
    if (numberOf(st, "fb_bus") > 0.02f || numberOf(st, "fb_fm") > 0.02f) extras.push_back("a feedback loop into the sources");
    if (numberOf(st, "strike_level") > 0.02f) extras.push_back("a struck attack");
    if (numberOf(st, "cloud_send") > 0.05f) {
        if (numberOf(st, "cloud_resonance") > 0.3f) extras.push_back("a grain cloud ringing on the scale's notes");
        else if (numberOf(st, "cloud_feedback") > 0.5f) extras.push_back("a grain cloud feeding on itself");
        else extras.push_back("a grain cloud");
    }
    if (numberOf(st, "mem_send") > 0.05f) extras.push_back("a drifting memory");
    if (!valueOf(st, "z_mode").empty() && valueOf(st, "z_mode") != "Off") extras.push_back("the z-plane filter in the path");
    {
        const float decay = numberOf(st, "far_decay", 0.0f);
        if (decay > 55.0f) extras.push_back("a reverb a minute long");
        else if (decay > 25.0f) extras.push_back("a long far reverb");
    }
    if (numberOf(st, "sub_level") > 0.05f) extras.push_back("a sub under it");

    std::string out;
    if (!character.empty()) { out = character; out += ". "; }
    std::string body = listOf(parts);
    if (!body.empty()) {
        body[0] = static_cast<char>(std::toupper(body[0]));
        out += body;
        out += ".";
    }
    if (!extras.empty()) {
        out += " With ";
        out += listOf(extras);
        out += ".";
    }
    return out;
}

// The whole card, in the manner of u-he's PRESET INFO: what it is, what it sounds like, what it
// is filed under, and last what your hands do in it. That order is not taste: the panel paints
// until it runs out of height, and on the map side it has 240 pixels, so what identifies a preset
// stands above the list of its routes. Longer than the one-line description, and put together from the same
// three sources -- the measurements, the settings, and the preset's own modulation matrix.
std::string presetInfoText(int index)
{
    if (index < 0 || index >= numPresets()) return {};
    const Preset& p = preset(index);
    const char* st = p.settings != nullptr ? p.settings : "";
    std::string out = presetDescription(index);

    // A sentence about how it moves: the conductor's own pace, and how long a note stands. These
    // are the numbers that decide whether a drone is a bed or a piece, and they are never in the
    // name.
    {
        std::vector<std::string> pace;
        const float rate = numberOf(st, "brain_rate", 0.0f);
        const float holdMin = numberOf(st, "brain_hold_min", 0.0f), holdMax = numberOf(st, "brain_hold_max", 0.0f);
        const int voices = static_cast<int>(numberOf(st, "brain_density", 0.0f));
        if (valueOf(st, "brain_on") != "off" && rate > 0.0f) {
            std::string s = "a note every " + std::to_string(static_cast<int>(rate + 0.5f)) + " s";
            if (voices > 0) s += ", " + std::to_string(voices) + " voices at a time";
            pace.push_back(s);
        }
        if (holdMin > 0.0f && holdMax > 0.0f)
            pace.push_back("held " + std::to_string(static_cast<int>(holdMin + 0.5f)) + " to "
                           + std::to_string(static_cast<int>(holdMax + 0.5f)) + " s");
        const float arc = numberOf(st, "arc_period", 0.0f);
        if (numberOf(st, "arc", 0.0f) > 0.05f && arc > 0.0f)
            pace.push_back("an arc over " + std::to_string(static_cast<int>(arc + 0.5f)) + " minutes");
        const float attack = numberOf(st, "attack", 0.0f), release = numberOf(st, "release", 0.0f);
        if (attack >= 8.0f || release >= 20.0f)
            pace.push_back("envelopes of " + std::to_string(static_cast<int>(attack + 0.5f)) + " s in and "
                           + std::to_string(static_cast<int>(release + 0.5f)) + " s out");
        if (!pace.empty()) { out += "\nIts pace: " + listOf(pace) + "."; }
    }

    // Where it is filed: the family it came from, the group it measured into, and its tags.
    if (numPresetMeta() == numPresets()) {
        const PresetMeta& m = presetMeta(index);
        // What a model that listened to the render says. The sentence above was written from the
        // settings -- it knows what is switched on; this line knows only the sound, which is why
        // the two are kept apart and never blended into one paragraph.
        if (numPresetPhrases() > 0 && m.phrase[0] >= 0) {
            out += "\n\nSOUNDS LIKE\n";
            out += presetPhrase(m.phrase[0]);
            if (m.phrase[1] >= 0 && m.phrase[1] != m.phrase[0]) { out += ", "; out += presetPhrase(m.phrase[1]); }
            out += "\n";
        }
        out += "\n\nFILED UNDER\n";
        out += presetFamilyName(m.family);
        const int c = presetClusterOf(m);
        if (c >= 0 && *presetClusterName(c) != 0) { out += "   -   group: "; out += presetClusterName(c); }
        out += "\n";
        std::string tags;
        for (int t = 0; t < kNumPresetTags; ++t)
            if (m.tags & (1u << t)) { if (!tags.empty()) tags += "  "; tags += presetTagName(t); }
        if (!tags.empty()) out += tags + "\n";
        if (m.loudDb < -0.5f) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "measured at %.1f dBFS\n", static_cast<double>(m.loudDb));
            out += buf;
        }
    }
    // What your hands do here. Only the routes a player can reach -- the macros, the wheel, the
    // pressure and the slide; the LFOs and the fields are the instrument moving by itself.
    if (p.mod != nullptr && *p.mod != 0) {
        std::vector<std::string> hands, itself;
        std::string t(p.mod);
        size_t at = 0;
        while (at < t.size()) {
            const size_t semi = t.find(';', at);
            std::string row = t.substr(at, semi == std::string::npos ? std::string::npos : semi - at);
            at = (semi == std::string::npos) ? t.size() : semi + 1;
            const size_t gt = row.find('>');
            const size_t colon = row.find(':', gt == std::string::npos ? 0 : gt);
            if (gt == std::string::npos || colon == std::string::npos) continue;
            const std::string src = row.substr(0, gt);
            const std::string tgt = row.substr(gt + 1, colon - gt - 1);
            const ParamDesc* d = findParam(tgt.c_str());
            if (d == nullptr) continue;
            std::string pretty = src;
            for (char& c : pretty) if (c == '_') c = ' ';
            if (!pretty.empty()) pretty[0] = static_cast<char>(std::toupper(pretty[0]));
            const bool byHand = src.rfind("macro", 0) == 0 || src == "wheel" || src == "pressure" || src == "slide";
            (byHand ? hands : itself).push_back(pretty + "  ->  " + d->name);
        }
        if (!hands.empty()) {
            out += "\n\nUNDER YOUR HANDS\n";
            for (const auto& h : hands) out += h + "\n";
        }
        if (!itself.empty()) {
            out += hands.empty() ? "\n\nIT MOVES ITSELF\n" : "\nIT MOVES ITSELF\n";
            for (size_t i = 0; i < itself.size() && i < 8; ++i) out += itself[i] + "\n";
            if (itself.size() > 8) out += "and " + std::to_string(itself.size() - 8) + " more routes\n";
        }
    }

    return out;
}

}   // namespace ambient
