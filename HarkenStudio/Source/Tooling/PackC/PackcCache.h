// packc build cache: the incremental-rebuild accelerator.
//
// The cache is a JSON file recording, per source, the raw file content
// hash plus the transform identity and decode results (format facts and
// the canonical-blob checksum). On a rebuild, a source whose raw bytes
// and build-relevant parameters are unchanged skips the decode pipeline;
// its canonical blob
// bytes are lifted out of the PREVIOUS output container and verified by
// recomputing the recorded checksum over the lifted bytes before reuse.
// The pack is then assembled through the exact same layout code a clean
// build runs, so a cache hit can only ever reproduce the bytes a clean
// build would produce — the cache is an accelerator, never an output
// influence.
//
// Degradation contract (never a wrong pack): a missing cache file is the
// normal first build; a cache that fails to parse, carries a different
// version, contains unknown keys, or disagrees with the previous output
// container in any way is ignored WHOLE and the build proceeds clean.
// Every ignore reason surfaces as a report warning, never as a refusal.
//
// Matching is keyed by the source path exactly as written in the
// manifest, downmix policy, and downmix algorithm version. A hit also
// requires the raw byte size, raw content hash (FNV-1a 64 over the file
// bytes), and codec to match; loop points deliberately do NOT
// participate — they are TOC fields the assembler
// applies from the manifest, so editing them reuses the blob. Entries
// without an explicit id never hit: a minted id derives from decoded
// content, and identity must never be derived through the cache.
//
// The previous output may be either container generation: a v1 pack
// exposes its TOC directly, a v2 pack exposes the same 68-byte media
// records through its typed directory (logic entries never feed the
// media cache — they are cheap and serialize fresh every build). Both
// parses fill the same PreviousPack form, so blob lookup and
// verification are generation-blind.
//
// The source→blob binding is never taken from cache text alone: the
// entry's explicit id locates exactly one TOC entry in the previous
// output, and the record's blob-side facts must agree with THAT entry
// (then the blob bytes re-hash to the recorded checksum). A record
// spliced from two sources — one entry's source facts next to another's
// blob facts — collides with the id-located entry's real facts and
// misses instead of assembling the wrong sound.
//
// The whole file additionally carries an integrity checksum (FNV-1a 64
// over the file text with the integrity value itself held at a fixed
// all-zero placeholder); a mismatch discards the cache whole. Threat
// model boundary: the cache lives in the same trust domain as the
// source files it describes — integrity targets accidental corruption
// (bit flips, truncation, hand edits), not an adversary who can rewrite
// the checksum along with the record; such an actor could as easily
// rewrite the sources or the output.
#ifndef HARKEN_HOST_PACKC_CACHE_H
#define HARKEN_HOST_PACKC_CACHE_H

#include <harken_studio/ObjectId.h>
#include "../Common/JsonReader.h"
#include "../Common/JsonWriter.h"
#include "../Common/Utf8Path.h"

#include "harken_studio_build/PackFormat.h"
#include "harken_studio_build/PackFormatV2.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace harkenstudiojson {
namespace packc {

inline constexpr std::uint32_t kCacheVersion = 2;
inline constexpr std::uint32_t kDownmixAlgorithmVersion = 1;
inline constexpr char kCacheDownmixPreserve[] = "preserve";
inline constexpr char kCacheDownmixMono[] = "mono";

// The integrity member's serialized form: emitted first in the root
// object with this all-zero placeholder, hashed, then patched in place
// (same length, so no other byte moves). Verification reverses the
// patch and recomputes.
inline constexpr char kCacheIntegrityMarker[] = "\"integrity\": \"";
inline constexpr char kCacheIntegrityPlaceholder[] = "0000000000000000";

struct CacheRecord {
    std::string sourcePath; // as written in the manifest
    std::uint64_t sourceBytes = 0;
    std::uint64_t sourceHash = 0; // FNV-1a 64 over the raw file bytes
    std::string codec;
    std::string downmixPolicy;
    std::uint32_t downmixAlgorithmVersion = 0;
    std::uint16_t sourceChannelCount = 0;
    float sourcePeakAbs = 0.0f;
    std::uint32_t sampleRate = 0;
    std::uint16_t channelCount = 0;
    std::uint64_t frameCount = 0;
    std::uint64_t blobBytes = 0;
    std::uint64_t blobChecksum = 0; // FNV-1a 64 over the canonical blob
};

inline bool ReadBinaryFile(const std::string& path,
                           std::vector<std::uint8_t>* out) {
    std::ifstream in(Utf8Path(path), std::ios::binary);
    if (!in) {
        return false;
    }
    out->assign(std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());
    return in.good() || in.eof();
}

inline bool ParseHexU64(const std::string& text, std::size_t digits,
                        std::uint64_t* out) {
    if (text.size() != digits) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        std::uint64_t nibble = 0;
        if (c >= '0' && c <= '9') {
            nibble = static_cast<std::uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            nibble = static_cast<std::uint64_t>(c - 'a' + 10);
        } else {
            return false;
        }
        value = (value << 4) | nibble;
    }
    *out = value;
    return true;
}

inline bool ParseCacheFloat(const JsonValue& value, float* out) {
    if (value.kind != JsonValue::Kind::Number || value.text.empty()) {
        return false;
    }
    const char* begin = value.text.c_str();
    const char* end = begin + value.text.size();
    float parsed = 0.0f;
    const std::from_chars_result result =
        std::from_chars(begin, end, parsed);
    if (result.ec != std::errc() || result.ptr != end ||
        !std::isfinite(parsed) || parsed < 0.0f) {
        return false;
    }
    *out = parsed;
    return true;
}

inline std::string FormatCacheFloat(float value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.9g",
                  static_cast<double>(value));
    return std::string(buffer);
}

// Loads and fully validates a cache file. Returns true with records on
// success. Returns false otherwise: with an empty reason when the file
// simply does not exist (the normal first build), else with the reason
// the cache is being ignored.
inline bool LoadCacheFile(const std::string& path,
                          std::vector<CacheRecord>* outRecords,
                          std::string* outIgnoreReason) {
    outIgnoreReason->clear();
    std::vector<std::uint8_t> bytes;
    {
        std::ifstream probe(Utf8Path(path), std::ios::binary);
        if (!probe) {
            return false; // absent: silently a clean build
        }
    }
    if (!ReadBinaryFile(path, &bytes)) {
        *outIgnoreReason = "cache file is unreadable";
        return false;
    }
    const std::string text(reinterpret_cast<const char*>(bytes.data()),
                           bytes.size());
    JsonValue root;
    std::string error;
    if (!ParseJson(text, &root, &error) ||
        root.kind != JsonValue::Kind::Object) {
        *outIgnoreReason = "cache file is not valid JSON";
        return false;
    }
    for (const auto& member : root.members) {
        if (member.first != "integrity" &&
            member.first != "cacheVersion" &&
            member.first != "sources") {
            *outIgnoreReason =
                "cache file carries unknown key \"" + member.first + "\"";
            return false;
        }
    }
    const JsonValue* version = root.Find("cacheVersion");
    std::uint64_t versionValue = 0;
    if (version == nullptr || !version->AsU64(&versionValue) ||
        versionValue != kCacheVersion) {
        *outIgnoreReason = "cache file version is not supported";
        return false;
    }
    // Whole-file integrity: the recorded value must reproduce when the
    // text is re-hashed with the value slot held at the placeholder.
    // Any failure to even locate the slot counts as corruption.
    {
        const JsonValue* integrity = root.Find("integrity");
        std::uint64_t recorded = 0;
        if (integrity == nullptr ||
            integrity->kind != JsonValue::Kind::String ||
            !ParseHexU64(integrity->text, 16, &recorded)) {
            *outIgnoreReason = "cache file integrity value is malformed";
            return false;
        }
        const std::string marker = kCacheIntegrityMarker;
        const std::size_t markerPos = text.find(marker);
        const std::size_t valuePos =
            markerPos == std::string::npos ? std::string::npos
                                           : markerPos + marker.size();
        if (valuePos == std::string::npos ||
            valuePos + 16 > text.size() ||
            text.compare(valuePos, 16, integrity->text) != 0) {
            *outIgnoreReason =
                "cache file integrity slot cannot be located";
            return false;
        }
        std::string placeheld = text;
        placeheld.replace(valuePos, 16, kCacheIntegrityPlaceholder);
        if (harkenstudiobuild::Fnv1a64(placeheld.data(), placeheld.size()) !=
            recorded) {
            *outIgnoreReason = "cache file integrity check failed";
            return false;
        }
    }
    const JsonValue* sources = root.Find("sources");
    if (sources == nullptr ||
        sources->kind != JsonValue::Kind::Array) {
        *outIgnoreReason = "cache file lacks a \"sources\" array";
        return false;
    }
    for (const JsonValue& item : sources->items) {
        if (item.kind != JsonValue::Kind::Object) {
            *outIgnoreReason = "cache source record is not an object";
            return false;
        }
        CacheRecord record;
        bool sawPath = false;
        bool sawCodec = false;
        bool sawDownmixPolicy = false;
        bool sawDownmixAlgorithmVersion = false;
        bool sawSourceChannelCount = false;
        bool sawSourcePeakAbs = false;
        std::uint64_t u64 = 0;
        for (const auto& member : item.members) {
            const std::string& key = member.first;
            const JsonValue& value = member.second;
            if (key == "path") {
                if (value.kind != JsonValue::Kind::String) {
                    *outIgnoreReason = "cache record path is malformed";
                    return false;
                }
                record.sourcePath = value.text;
                sawPath = true;
            } else if (key == "codec") {
                if (value.kind != JsonValue::Kind::String) {
                    *outIgnoreReason = "cache record codec is malformed";
                    return false;
                }
                record.codec = value.text;
                sawCodec = true;
            } else if (key == "downmix") {
                if (value.kind != JsonValue::Kind::String ||
                    (value.text != kCacheDownmixPreserve &&
                     value.text != kCacheDownmixMono)) {
                    *outIgnoreReason =
                        "cache record downmix policy is malformed";
                    return false;
                }
                record.downmixPolicy = value.text;
                sawDownmixPolicy = true;
            } else if (key == "sourcePeakAbs") {
                if (!ParseCacheFloat(value, &record.sourcePeakAbs)) {
                    *outIgnoreReason =
                        "cache record sourcePeakAbs is malformed";
                    return false;
                }
                sawSourcePeakAbs = true;
            } else if (key == "bytes" || key == "sampleRate" ||
                       key == "channelCount" || key == "frameCount" ||
                       key == "blobBytes" ||
                       key == "downmixAlgorithmVersion" ||
                       key == "sourceChannelCount") {
                if (!value.AsU64(&u64)) {
                    *outIgnoreReason =
                        "cache record \"" + key + "\" is malformed";
                    return false;
                }
                if (key == "bytes") {
                    record.sourceBytes = u64;
                } else if (key == "downmixAlgorithmVersion") {
                    if (u64 > 0xFFFFFFFFull) {
                        *outIgnoreReason =
                            "cache record downmixAlgorithmVersion is "
                            "out of range";
                        return false;
                    }
                    record.downmixAlgorithmVersion =
                        static_cast<std::uint32_t>(u64);
                    sawDownmixAlgorithmVersion = true;
                } else if (key == "sourceChannelCount") {
                    if (u64 == 0 || u64 > 0xFFFFull) {
                        *outIgnoreReason =
                            "cache record sourceChannelCount is out of "
                            "range";
                        return false;
                    }
                    record.sourceChannelCount =
                        static_cast<std::uint16_t>(u64);
                    sawSourceChannelCount = true;
                } else if (key == "sampleRate") {
                    if (u64 > 0xFFFFFFFFull) {
                        *outIgnoreReason =
                            "cache record sampleRate is out of range";
                        return false;
                    }
                    record.sampleRate =
                        static_cast<std::uint32_t>(u64);
                } else if (key == "channelCount") {
                    if (u64 > 0xFFFFull) {
                        *outIgnoreReason =
                            "cache record channelCount is out of range";
                        return false;
                    }
                    record.channelCount =
                        static_cast<std::uint16_t>(u64);
                } else if (key == "frameCount") {
                    record.frameCount = u64;
                } else {
                    record.blobBytes = u64;
                }
            } else if (key == "hash" || key == "blobChecksum") {
                if (value.kind != JsonValue::Kind::String ||
                    !ParseHexU64(value.text, 16, &u64)) {
                    *outIgnoreReason =
                        "cache record \"" + key + "\" is malformed";
                    return false;
                }
                if (key == "hash") {
                    record.sourceHash = u64;
                } else {
                    record.blobChecksum = u64;
                }
            } else {
                *outIgnoreReason =
                    "cache record carries unknown key \"" + key + "\"";
                return false;
            }
        }
        if (!sawPath || !sawCodec || !sawDownmixPolicy ||
            !sawDownmixAlgorithmVersion || !sawSourceChannelCount ||
            !sawSourcePeakAbs) {
            *outIgnoreReason = "cache record is incomplete";
            return false;
        }
        outRecords->push_back(std::move(record));
    }
    return true;
}

inline std::string EmitCacheJson(
    const std::vector<CacheRecord>& records) {
    JsonWriter writer;
    writer.BeginObject();
    // First member on purpose: verification finds the value slot at the
    // FIRST marker occurrence in the raw text, and no earlier string
    // value can exist before the first member.
    writer.Key("integrity");
    writer.ValueString(kCacheIntegrityPlaceholder);
    writer.Key("cacheVersion");
    writer.ValueU64(kCacheVersion);
    writer.Key("sources");
    writer.BeginArray();
    for (const CacheRecord& record : records) {
        writer.BeginObject();
        writer.Key("path");
        writer.ValueString(record.sourcePath);
        writer.Key("bytes");
        writer.ValueU64(record.sourceBytes);
        writer.Key("hash");
        writer.ValueString(JsonWriter::HexU64(record.sourceHash));
        writer.Key("codec");
        writer.ValueString(record.codec);
        writer.Key("downmix");
        writer.ValueString(record.downmixPolicy);
        writer.Key("downmixAlgorithmVersion");
        writer.ValueU64(record.downmixAlgorithmVersion);
        writer.Key("sourceChannelCount");
        writer.ValueU64(record.sourceChannelCount);
        writer.Key("sourcePeakAbs");
        writer.ValueRawNumber(FormatCacheFloat(record.sourcePeakAbs));
        writer.Key("sampleRate");
        writer.ValueU64(record.sampleRate);
        writer.Key("channelCount");
        writer.ValueU64(record.channelCount);
        writer.Key("frameCount");
        writer.ValueU64(record.frameCount);
        writer.Key("blobBytes");
        writer.ValueU64(record.blobBytes);
        writer.Key("blobChecksum");
        writer.ValueString(JsonWriter::HexU64(record.blobChecksum));
        writer.EndObject();
    }
    writer.EndArray();
    writer.EndObject();
    std::string text = writer.Finish();
    const std::string marker = kCacheIntegrityMarker;
    const std::size_t valuePos = text.find(marker) + marker.size();
    text.replace(valuePos, 16,
                 JsonWriter::HexU64(
                     harkenstudiobuild::Fnv1a64(text.data(), text.size())));
    return text;
}

// The previous output container, held as bytes plus a parsed TOC, used
// as the blob source for cache hits. valid=false simply means every
// entry decodes fresh.
struct PreviousPack {
    bool valid = false;
    std::vector<std::uint8_t> bytes;
    std::vector<harkenstudiobuild::PackTocEntry> toc;
};

// v2 branch of the previous-output parse: validates the v2 header and
// directory, then collects the media records (the v1 TOC form, offsets
// file-absolute) so FindVerifiedBlob works identically over either
// container generation. Logic entries are skipped — they never feed
// the media cache. Any disagreement leaves the pack invalid with a
// reason, and the build degrades to clean.
inline void LoadPreviousPackV2Media(PreviousPack* out,
                                    std::string* outIgnoreReason) {
    if (out->bytes.size() < harkenstudiobuild::kPackV2HeaderBytes) {
        *outIgnoreReason = "previous output is unreadable or truncated";
        return;
    }
    harkenstudiobuild::PackV2Header header;
    if (!harkenstudiobuild::ParsePackV2Header(out->bytes.data(), &header) ||
        header.formatVersion != harkenstudiobuild::kPackFormatVersion2 ||
        header.headerBytes != harkenstudiobuild::kPackV2HeaderBytes ||
        header.fileBytes != out->bytes.size() ||
        header.directoryOffset != harkenstudiobuild::kPackV2HeaderBytes) {
        *outIgnoreReason = "previous output header does not validate";
        return;
    }
    const std::uint64_t dirBytes =
        static_cast<std::uint64_t>(header.entryCount) *
        harkenstudiobuild::kPackV2DirectoryEntryBytes;
    if (header.directoryOffset + dirBytes > out->bytes.size()) {
        *outIgnoreReason =
            "previous output directory is out of bounds";
        return;
    }
    const std::uint8_t* dir =
        out->bytes.data() + header.directoryOffset;
    if (harkenstudiobuild::Fnv1a64(dir, static_cast<std::size_t>(dirBytes)) !=
        header.directoryChecksum) {
        *outIgnoreReason =
            "previous output directory checksum mismatch";
        return;
    }
    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        const harkenstudiobuild::PackV2DirectoryEntry entry =
            harkenstudiobuild::ParsePackV2DirectoryEntry(
                dir + static_cast<std::size_t>(i) *
                          harkenstudiobuild::kPackV2DirectoryEntryBytes);
        if (entry.kind != harkenstudiobuild::kPackEntryKindMedia) {
            continue;
        }
        if (entry.vintage != harkenstudiobuild::kPackV2SchemaVintage ||
            entry.size != harkenstudiobuild::kPackV2MediaPayloadBytes ||
            entry.offset > out->bytes.size() ||
            entry.size > out->bytes.size() - entry.offset) {
            *outIgnoreReason =
                "previous output media record does not validate";
            out->toc.clear();
            return;
        }
        const std::uint8_t* payload = out->bytes.data() + entry.offset;
        if (harkenstudiobuild::Fnv1a64(payload,
                           static_cast<std::size_t>(entry.size)) !=
            entry.payloadChecksum) {
            *outIgnoreReason =
                "previous output media record checksum mismatch";
            out->toc.clear();
            return;
        }
        const harkenstudiobuild::PackTocEntry toc =
            harkenstudiobuild::ParsePackTocEntry(payload);
        if (toc.blobOffset + toc.blobBytes > out->bytes.size()) {
            *outIgnoreReason =
                "previous output blob range is out of bounds";
            out->toc.clear();
            return;
        }
        out->toc.push_back(toc);
    }
    out->valid = true;
}

// Parses and integrity-checks the previous output. Any disagreement —
// magic, version, bounds, TOC checksum — leaves the pack invalid with a
// reason, and the build degrades to clean.
inline void LoadPreviousPack(const std::string& path, PreviousPack* out,
                             std::string* outIgnoreReason) {
    outIgnoreReason->clear();
    {
        std::ifstream probe(Utf8Path(path), std::ios::binary);
        if (!probe) {
            *outIgnoreReason =
                "previous output is absent — building clean";
            return;
        }
    }
    if (!ReadBinaryFile(path, &out->bytes) ||
        out->bytes.size() < harkenstudiobuild::kPackHeaderBytes) {
        *outIgnoreReason = "previous output is unreadable or truncated";
        return;
    }
    harkenstudiobuild::PackHeader header;
    if (!harkenstudiobuild::ParsePackHeader(out->bytes.data(), &header)) {
        *outIgnoreReason = "previous output header does not validate";
        return;
    }
    if (header.formatVersion == harkenstudiobuild::kPackFormatVersion2) {
        LoadPreviousPackV2Media(out, outIgnoreReason);
        return;
    }
    if (header.formatVersion != harkenstudiobuild::kPackFormatVersion ||
        header.headerBytes != harkenstudiobuild::kPackHeaderBytes ||
        header.fileBytes != out->bytes.size() ||
        header.tocOffset != harkenstudiobuild::kPackHeaderBytes) {
        *outIgnoreReason = "previous output header does not validate";
        return;
    }
    const std::uint64_t tocBytes =
        static_cast<std::uint64_t>(header.entryCount) *
        harkenstudiobuild::kPackTocEntryBytes;
    if (header.tocOffset + tocBytes > out->bytes.size()) {
        *outIgnoreReason = "previous output TOC is out of bounds";
        return;
    }
    const std::uint8_t* tocImage = out->bytes.data() + header.tocOffset;
    if (harkenstudiobuild::Fnv1a64(tocImage, static_cast<std::size_t>(tocBytes)) !=
        header.tocChecksum) {
        *outIgnoreReason = "previous output TOC checksum mismatch";
        return;
    }
    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        harkenstudiobuild::PackTocEntry entry = harkenstudiobuild::ParsePackTocEntry(
            tocImage + static_cast<std::size_t>(i) *
                           harkenstudiobuild::kPackTocEntryBytes);
        if (entry.blobOffset + entry.blobBytes > out->bytes.size()) {
            *outIgnoreReason =
                "previous output blob range is out of bounds";
            out->toc.clear();
            return;
        }
        out->toc.push_back(entry);
    }
    out->valid = true;
}

// Locates the blob for one entry in the previous output BY THE ENTRY'S
// STABLE ID — never by scanning for a record's checksum. The id names
// exactly one TOC entry; the cache record's blob-side facts (checksum,
// size, format facts) must ALL agree with that entry, and the blob
// bytes must re-hash to the recorded checksum. This makes the
// source→blob binding a property of the verified build product rather
// than of cache text: a record spliced from two sources collides with
// the id-located entry's real facts and misses. nullptr = no verified
// match (caller decodes fresh); an id absent from the previous output
// (including an id edit since the last build) is simply a miss.
inline const std::uint8_t* FindVerifiedBlob(const PreviousPack& prev,
                                            harkenstudio::ObjectId soundId,
                                            const CacheRecord& record) {
    if (!prev.valid) {
        return nullptr;
    }
    for (const harkenstudiobuild::PackTocEntry& entry : prev.toc) {
        if (entry.soundId != soundId) {
            continue;
        }
        if (entry.blobChecksum != record.blobChecksum ||
            entry.blobBytes != record.blobBytes ||
            entry.sampleRate != record.sampleRate ||
            entry.channelCount != record.channelCount ||
            entry.frameCount != record.frameCount ||
            entry.sampleFormat != harkenstudiobuild::kPackSampleFormatF32Planar) {
            return nullptr;
        }
        const std::uint8_t* blob = prev.bytes.data() + entry.blobOffset;
        if (harkenstudiobuild::Fnv1a64(blob,
                           static_cast<std::size_t>(record.blobBytes)) ==
            record.blobChecksum) {
            return blob;
        }
        return nullptr;
    }
    return nullptr;
}

} // namespace packc
} // namespace harkenstudiojson

#endif // HARKEN_HOST_PACKC_CACHE_H
