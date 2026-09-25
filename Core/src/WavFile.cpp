/**
 * @file WavFile.cpp
 * @brief The audio file readers: WAV and FLAC into floats, and the wavetable formats.
 *
 * Everything in the program that opens a sample on disk comes through here -- the render tool, the
 * Quest app and the plugin's own texture, impulse and wavetable loading -- and it runs on the
 * message thread or in a batch, never on the audio thread. The WAV reader walks the RIFF chunks
 * itself (PCM 8/16/24/32 and 32-bit float, WAVE_FORMAT_EXTENSIBLE unwrapped, a data length that
 * lies about the file clamped to what is actually there); FLAC is decoded by dr_flac, which this
 * translation unit compiles in. A file is recognised by its first four bytes, not by its name, and
 * a reference to a .wav that ships as .flac is resolved to the FLAC beside it (resolveAudioFile),
 * so the packs and the compiled-in banks can keep naming the files as they lie in the source
 * library.
 *
 * The wavetable reader on top of that finds the cycle length a table file states about itself --
 * Serum's and Vital's "clm " chunk, Surge's "srge" chunk, a "-WT2048" in the file name, the header
 * of Surge's own .wt format -- and falls back to measuring it from the samples (CycleTable.h).
 * On Windows the files are opened with FILE_FLAG_SEQUENTIAL_SCAN so that a batch reading gigabytes
 * of clips does not push everything else out of memory; see openRead().
 */
#include "ambient/WavFile.h"
/** @brief One translation unit defines dr_flac; everything else here is our own. */
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_OGG            ///< no Ogg-FLAC in this library, and it halves the object
#if defined(_MSC_VER)
  #pragma warning(push, 0)        // somebody else's file: our /W4 is not its business
#endif
#include "dr_flac.h"
#if defined(_MSC_VER)
  #pragma warning(pop)
#endif
#include "ambient/CycleTable.h"   // detectCycleLength, for a wavetable file that does not say
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <io.h>
  #include <fcntl.h>
#endif

namespace ambient {

namespace {
/**
 * @brief Opens a file for binary reading, on Windows with a hint that it will be read once, front
 *        to back, and not needed again.
 *
 * A sample is read once and then never again, but the file cache has no way of knowing that: a
 * batch that renders five thousand presets reads eleven gigabytes of clips and impulses, and
 * every byte of it stays resident afterwards. Windows filled its standby list to 38 GB that way
 * and started trimming the working sets of the applications on screen instead.
 * FILE_FLAG_SEQUENTIAL_SCAN tells the cache manager to age these pages out immediately.
 *
 * @param path  the file to open
 * @return      a stdio stream positioned at the start, or nullptr when the file cannot be opened;
 *              on Windows a plain fopen when the flagged open fails for any reason
 */
FILE* openRead(const char* path)
{
#if defined(_WIN32)
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::fopen(path, "rb");   // fall back rather than fail
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_RDONLY | _O_BINARY);
    if (fd < 0) { CloseHandle(h); return std::fopen(path, "rb"); }
    FILE* f = _fdopen(fd, "rb");
    if (f == nullptr) { _close(fd); return std::fopen(path, "rb"); }
    return f;
#else
    return std::fopen(path, "rb");
#endif
}
} // namespace

bool readWavMono(const char* path, std::vector<float>& mono, int& sampleRate)
{
    std::vector<std::vector<float>> ch;
    if (!readWavChannels(path, ch, sampleRate) || ch.empty()) { mono.clear(); return false; }
    mono.assign(ch[0].size(), 0.0f);
    const float inv = 1.0f / static_cast<float>(ch.size());
    for (const auto& c : ch) for (size_t i = 0; i < mono.size() && i < c.size(); ++i) mono[i] += c[i] * inv;
    return true;
}

bool readWavStereo(const char* path, std::vector<float>& left, std::vector<float>& right, int& sampleRate)
{
    left.clear();
    right.clear();
    std::vector<std::vector<float>> ch;
    if (!readWavChannels(path, ch, sampleRate) || ch.empty()) return false;
    if (ch.size() == 1) { left = std::move(ch[0]); return true; }
    if (ch.size() == 2) { left = std::move(ch[0]); right = std::move(ch[1]); return true; }
    // More than two: the odd channels to the left, the even ones to the right. Nothing in the
    // library is like this, but a player's own file might be, and a reader that took only the
    // first two would quietly drop the rest.
    const size_t n = ch[0].size();
    left.assign(n, 0.0f);
    right.assign(n, 0.0f);
    size_t nl = 0, nr = 0;
    for (size_t c = 0; c < ch.size(); ++c) {
        std::vector<float>& into = (c % 2 == 0) ? left : right;
        (c % 2 == 0 ? nl : nr) += 1;
        for (size_t i = 0; i < n && i < ch[c].size(); ++i) into[i] += ch[c][i];
    }
    for (size_t i = 0; i < n; ++i) {
        if (nl > 0) left[i] /= static_cast<float>(nl);
        if (nr > 0) right[i] /= static_cast<float>(nr);
    }
    return true;
}

namespace {

/**
 * @brief Decodes a whole FLAC file into deinterleaved float channels.
 *
 * FLAC, for the sample library. The clips ship as 24-bit FLAC rather than 24-bit WAV: the same
 * samples to the bit, in half the bytes (measured on this library, 39-44 % of the float originals
 * against 75 % for 24-bit PCM). It costs a decode when a preset loads -- milliseconds for a
 * twelve-second clip, and never on the audio thread -- and nothing at all in memory afterwards,
 * because what comes out is the same block of floats either way.
 *
 * @param path         the FLAC file
 * @param channelsOut  receives one vector per channel, every sample scaled to -1 .. 1
 * @param sampleRate   receives the file's sample rate in Hz
 * @return             false when dr_flac cannot open or decode the file, or it holds no audio
 */
bool readFlacChannels(const char* path, std::vector<std::vector<float>>& channelsOut, int& sampleRate)
{
    unsigned int channels = 0, rate = 0;
    drflac_uint64 frames = 0;
    float* pcm = drflac_open_file_and_read_pcm_frames_f32(path, &channels, &rate, &frames, nullptr);
    if (pcm == nullptr) return false;
    if (channels == 0 || frames == 0) { drflac_free(pcm, nullptr); return false; }
    sampleRate = static_cast<int>(rate);
    channelsOut.assign(channels, std::vector<float>(static_cast<size_t>(frames), 0.0f));
    for (drflac_uint64 i = 0; i < frames; ++i)
        for (unsigned int c = 0; c < channels; ++c)
            channelsOut[c][static_cast<size_t>(i)] = pcm[i * channels + c];
    drflac_free(pcm, nullptr);
    return true;
}

/**
 * @brief Whether a file begins with the "fLaC" marker.
 *
 * What a file is, from its first four bytes rather than from its name: a name can be wrong, and a
 * reader that trusts the extension hands back silence without a word.
 *
 * @param path  the file to inspect
 * @return      true only when the file opens and its first four bytes are "fLaC"
 */
bool looksLikeFlac(const char* path)
{
    FILE* f = openRead(path);
    if (f == nullptr) return false;
    char tag[4] = { 0, 0, 0, 0 };
    const bool got = std::fread(tag, 1, 4, f) == 4;
    std::fclose(f);
    return got && std::memcmp(tag, "fLaC", 4) == 0;
}

/**
 * @brief The same path with its extension replaced by .flac (or .flac appended when it has none).
 *
 * A preset names its clip as it lies in the source library -- "../Textures/a_bell.wav" -- while
 * what ships is the same audio as FLAC, at half the download. Rather than rewrite every reference
 * in every pack (and break every pack anybody else has written), the named file is looked for
 * first and the FLAC beside it second. One rule, in the one place that opens audio at all.
 *
 * @param path  the file as the preset names it; nullptr is treated as an empty name
 * @return      the sibling FLAC's path; a dot inside a folder name is not taken for an extension
 */
std::string withFlacExtension(const char* path)
{
    std::string s(path == nullptr ? "" : path);
    const size_t dot = s.find_last_of('.');
    const size_t slash = s.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return s + ".flac";
    return s.substr(0, dot) + ".flac";
}

/**
 * @brief Whether a file can be opened for reading -- the only test of existence that also proves
 *        the caller may read it.
 * @param path  the file to try
 * @return      true when openRead() succeeds; the stream is closed again at once
 */
bool exists(const std::string& path)
{
    FILE* f = openRead(path.c_str());
    if (f == nullptr) return false;
    std::fclose(f);
    return true;
}

} // namespace

std::string resolveAudioFile(const char* path)
{
    if (path == nullptr || *path == 0) return {};
    if (exists(path)) return path;
    const std::string alt = withFlacExtension(path);
    return exists(alt) ? alt : std::string();
}

bool readWavChannels(const char* path, std::vector<std::vector<float>>& channelsOut, int& sampleRate)
{
    channelsOut.clear();
    const std::string real = resolveAudioFile(path);
    if (real.empty()) return false;
    path = real.c_str();
    if (looksLikeFlac(path)) return readFlacChannels(path, channelsOut, sampleRate);
    FILE* f = openRead(path);
    if (!f) return false;
    auto rd32 = [&](uint32_t& v) { return std::fread(&v, 4, 1, f) == 1; };
    auto rd16 = [&](uint16_t& v) { return std::fread(&v, 2, 1, f) == 1; };
    char tag[4]; uint32_t size = 0;
    if (std::fread(tag, 1, 4, f) != 4 || std::memcmp(tag, "RIFF", 4) != 0 || !rd32(size) ||
        std::fread(tag, 1, 4, f) != 4 || std::memcmp(tag, "WAVE", 4) != 0) { std::fclose(f); return false; }
    uint16_t format = 0, channels = 0, bits = 0; uint32_t rate = 0;
    bool haveFmt = false, ok = false;
    // How long the file really is. A recorder that was stopped by pulling the plug leaves the
    // data chunk's length at 0xFFFFFFFF, and a reader that believes it asks for sixteen
    // gigabytes of floats before it has read a sample.
    const long afterHeader = std::ftell(f);
    std::fseek(f, 0, SEEK_END);
    const long fileLen = std::ftell(f);
    std::fseek(f, afterHeader, SEEK_SET);
    while (std::fread(tag, 1, 4, f) == 4 && rd32(size)) {
        const long next = std::ftell(f) + static_cast<long>(size + (size & 1u));
        if (std::memcmp(tag, "fmt ", 4) == 0) {
            uint16_t blockAlign = 0; uint32_t byteRate = 0;
            if (!rd16(format) || !rd16(channels) || !rd32(rate) || !rd32(byteRate) || !rd16(blockAlign) || !rd16(bits)) break;
            if (format == 0xFFFE && size >= 26) {   // WAVE_FORMAT_EXTENSIBLE: the sub-format GUID starts with the real tag
                uint16_t cbSize = 0, validBits = 0; uint32_t mask = 0; uint16_t sub = 0;
                if (rd16(cbSize) && rd16(validBits) && rd32(mask) && rd16(sub)) format = sub;
            }
            haveFmt = channels > 0 && rate > 0 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
        } else if (std::memcmp(tag, "data", 4) == 0 && haveFmt) {
            const int bytes = bits / 8;
            if (fileLen > 0) {
                const long here = std::ftell(f);
                const long left = here >= 0 && fileLen > here ? fileLen - here : 0;
                if (static_cast<uint64_t>(size) > static_cast<uint64_t>(left)) size = static_cast<uint32_t>(left);
            }
            const uint32_t frames = size / static_cast<uint32_t>(bytes * channels);
            channelsOut.assign(channels, std::vector<float>(frames, 0.0f));
            std::vector<unsigned char> buf(static_cast<size_t>(bytes * channels));
            uint32_t got = frames;
            for (uint32_t i = 0; i < frames; ++i) {
                if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) { got = i; break; }
                for (int c = 0; c < channels; ++c) {
                    const unsigned char* p = buf.data() + c * bytes;
                    float v = 0.0f;
                    if (format == 3 && bits == 32) { float fv; std::memcpy(&fv, p, 4); v = fv; }
                    else if (bits == 8)  v = (static_cast<int>(p[0]) - 128) / 128.0f;
                    else if (bits == 16) v = static_cast<int16_t>(p[0] | (p[1] << 8)) / 32768.0f;
                    else if (bits == 24) { int32_t s = (p[0] << 8) | (p[1] << 16) | (p[2] << 24); v = static_cast<float>(s >> 8) / 8388608.0f; }
                    else if (bits == 32) { int32_t s; std::memcpy(&s, p, 4); v = static_cast<float>(s) / 2147483648.0f; }
                    channelsOut[static_cast<size_t>(c)][i] = v;
                }
            }
            for (auto& c : channelsOut) c.resize(got);
            sampleRate = static_cast<int>(rate);
            ok = got > 0;
            break;
        }
        if (std::fseek(f, next, SEEK_SET) != 0) break;
    }
    std::fclose(f);
    return ok;
}

namespace {

/**
 * @brief How long a cycle is, as a WAV says it.
 *
 * Serum writes a "clm " chunk whose text begins "<!>2048"
 * and Vital writes the same; Surge writes "srge", a version and the size as two 32-bit integers.
 * 0 when the file says nothing.
 *
 * @param path  the WAV file (the real file, after resolveAudioFile)
 * @return      the stated cycle length in samples when it lies between 8 and 65536, else 0
 */
int statedCycleLength(const char* path)
{
    FILE* f = openRead(path);
    if (f == nullptr) return 0;
    auto rd32 = [&](uint32_t& v) { return std::fread(&v, 4, 1, f) == 1; };
    char tag[4];
    uint32_t size = 0;
    int found = 0;
    if (std::fread(tag, 1, 4, f) == 4 && std::memcmp(tag, "RIFF", 4) == 0 && rd32(size)
        && std::fread(tag, 1, 4, f) == 4 && std::memcmp(tag, "WAVE", 4) == 0) {
        while (found == 0 && std::fread(tag, 1, 4, f) == 4 && rd32(size)) {
            const long next = std::ftell(f) + static_cast<long>(size + (size & 1u));
            if (std::memcmp(tag, "clm ", 4) == 0) {
                char text[64] = {};
                const size_t want = std::min<size_t>(size, sizeof(text) - 1);
                if (std::fread(text, 1, want, f) == want && std::strncmp(text, "<!>", 3) == 0) found = std::atoi(text + 3);
            } else if (std::memcmp(tag, "srge", 4) == 0 && size >= 8) {
                uint32_t version = 0, len = 0;
                if (rd32(version) && rd32(len)) found = static_cast<int>(len);
            }
            if (std::fseek(f, next, SEEK_SET) != 0) break;
        }
    }
    std::fclose(f);
    return (found >= 8 && found <= 65536) ? found : 0;
}

/**
 * @brief A frame length written into the file name, a convention some tools use: "-WT512", "_wt1024".
 *
 * A power of two from 16 to 8192, or 0 when the name says nothing.
 *
 * @param path  the file's path; only the part after the last slash is searched
 * @return      the number after the first "-WT", "_WT" or " WT" (any case) that is a power of two
 *              in 16 .. 8192, else 0
 */
int namedCycleLength(const char* path)
{
    const char* base = path;
    for (const char* p = path; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
    for (const char* p = base; p[0] != 0 && p[1] != 0 && p[2] != 0; ++p) {
        if ((p[0] != '-' && p[0] != '_' && p[0] != ' ') || (p[1] != 'W' && p[1] != 'w') || (p[2] != 'T' && p[2] != 't')) continue;
        int v = 0, digits = 0;
        for (const char* d = p + 3; *d >= '0' && *d <= '9' && digits < 6; ++d, ++digits) v = v * 10 + (*d - '0');
        if (digits > 0 && v >= 16 && v <= 8192 && (v & (v - 1)) == 0) return v;
    }
    return 0;
}

/**
 * @brief Reads a Surge .wt wavetable file.
 *
 * Surge's own format: "vawt", the size of one wave, how many waves, flags, then the waves -- 16-bit
 * when flag 4 is set (full scale at 32768 with flag 8, at 16384 without it), 32-bit float otherwise.
 *
 * @param path      the file to read
 * @param mono      receives every wave in turn, waveSize * count samples scaled to -1 .. 1; cleared
 *                  when the read fails
 * @param cycleLen  receives the size of one wave, or 0 when the read fails
 * @return          false when the file is not a .wt (no "vawt" tag, a wave size outside 8 .. 65536,
 *                  no waves) or is cut short
 */
bool readSurgeWt(const char* path, std::vector<float>& mono, int& cycleLen)
{
    FILE* f = openRead(path);
    if (f == nullptr) return false;
    char tag[4];
    uint32_t waveSize = 0;
    uint16_t count = 0, flags = 0;
    bool ok = std::fread(tag, 1, 4, f) == 4 && std::memcmp(tag, "vawt", 4) == 0
           && std::fread(&waveSize, 4, 1, f) == 1 && std::fread(&count, 2, 1, f) == 1 && std::fread(&flags, 2, 1, f) == 1
           && waveSize >= 8 && waveSize <= 65536 && count >= 1;
    if (ok) {
        const size_t total = static_cast<size_t>(waveSize) * count;
        mono.assign(total, 0.0f);
        if (flags & 4u) {
            std::vector<int16_t> raw(total);
            ok = std::fread(raw.data(), 2, total, f) == total;
            const float scale = (flags & 8u) ? 1.0f / 32768.0f : 1.0f / 16384.0f;
            for (size_t i = 0; ok && i < total; ++i) mono[i] = static_cast<float>(raw[i]) * scale;
        } else {
            ok = std::fread(mono.data(), 4, total, f) == total;
        }
        cycleLen = static_cast<int>(waveSize);
    }
    std::fclose(f);
    if (!ok) { mono.clear(); cycleLen = 0; }
    return ok;
}

} // namespace

bool readWavetableFile(const char* path, std::vector<float>& mono, int& cycleLen)
{
    mono.clear();
    cycleLen = 0;
    if (path == nullptr || *path == 0) return false;
    if (readSurgeWt(path, mono, cycleLen)) return true;
    int rate = 0;
    if (!readWavMono(path, mono, rate) || mono.empty()) return false;
    const std::string real = resolveAudioFile(path);
    const int n = static_cast<int>(mono.size());
    int stated = real.empty() ? 0 : statedCycleLength(real.c_str());
    if (stated <= 0 || n < stated) stated = namedCycleLength(path);
    cycleLen = (stated > 0 && n >= stated) ? stated : detectCycleLength(mono.data(), n);
    return cycleLen > 0;
}

} // namespace ambient
