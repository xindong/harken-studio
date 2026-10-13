// packc validation report: the machine-readable build-feedback surface.
// Every build — either manifest generation, pack or stream, built or
// refused — emits one JSON report; on a refusal the report names the
// reason the exit code cannot carry. Schema (reportVersion 1):
//
//   {
//     "reportVersion": 1,
//     "tool": "packc",
//     "status": "built" | "refused",
//     "refusal": { "message": "...", "manifest": "..." },  // refused only
//     "warnings": [ { "kind": "...", ... } ],              // cross-pack
//     "packs": [
//       {
//         "manifest": "...", "manifestVersion": 1|2,
//         "output": "...", "container": "pack" | "stream",
//         "packKind": "resident" | "content",   // v2 containers only
//         "logicEntries": { "buses": 2, ... },  // v2 containers only;
//                          // per-section counts, present sections
//                          // only, fixed alphabetical key order
//         "entries": [
//           { "id": "42", "source": "click.wav", "frames": 4800,
//             "rate": 48000, "channels": 1,
//             "checksum": "<16 hex>", "peakAbs": 0.5,
//             "silent": false, "clipped": false,
//             "skippedByCache": false } ],
//         "totals": { "entryCount": 1, "fileBytes": 19332,
//                     "tocChecksum": "<16 hex>",
//                     "cacheHits": 0, "cacheMisses": 1 },
//         "warnings": [ { "kind": "silent-entry", ... } ]
//       } ]
//   }
//
// Warning kinds: per-pack "silent-entry" (peak at or below the
// repository's silence floor), "clipped-source" (peak above unity), and
// "downmix-cancellation" (a stereo-to-mono transform loses at least
// 20 dB of peak level);
// cross-pack "duplicate-id-across-manifests" (the same id declared by
// more than one manifest in this invocation — legal per pack, but the
// packs cannot co-load into one engine) and "cache-ignored" (a present
// cache could not be used; the build degraded to clean). Warnings never
// change the exit code.
//
// Number encoding: u64-valued identity and checksum fields travel as
// strings (ids in decimal, checksums as 16-digit lowercase hex) because
// a general JSON consumer only guarantees 53-bit integer precision;
// counts and format facts stay plain numbers. peakAbs is a %.9g float
// literal — nine significant digits round-trip an f32 exactly. The
// report contains nothing derived from time, absolute machine state, or
// unordered iteration: identical inputs produce an identical report,
// modulo the cache-hit fields that honestly describe how this
// particular build ran.
#ifndef HARKEN_HOST_PACKC_REPORT_H
#define HARKEN_HOST_PACKC_REPORT_H

#include "../Common/JsonWriter.h"
#include <harken_studio/ObjectId.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace harkenstudiojson {
namespace packc {

inline constexpr std::uint32_t kReportVersion = 2;

// The repository-wide silence-floor discipline: a signal whose absolute
// peak does not exceed 1e-4 counts as silent (the same floor the host
// demo verdicts and the test support assert against).
inline constexpr float kSilenceFloorPeak = 1e-4f;

// Full scale for the canonical float pipeline; a source peaking above
// it would clip once rendered through any unity path.
inline constexpr float kClipPeak = 1.0f;

// 20 * log10(0.1) = -20 dB. Comparing the peak ratio avoids a log and
// includes the exact 20 dB boundary.
inline constexpr double kDownmixCancellationPeakRatio = 0.1;

inline std::string FormatFloat(float value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.9g",
                  static_cast<double>(value));
    return std::string(buffer);
}

struct ReportEntry {
    harkenstudio::ObjectId id{};
    std::string source; // as written in the manifest
    std::uint64_t frames = 0;
    std::uint32_t rate = 0;
    std::uint16_t channels = 0;
    std::uint64_t checksum = 0; // FNV-1a 64 over the canonical blob
    float peakAbs = 0.0f;
    bool silent = false;
    bool clipped = false;
    bool skippedByCache = false;
};

struct ReportWarning {
    std::string kind;
    // Optional facts, emitted only when set.
    bool hasId = false;
    harkenstudio::ObjectId id{};
    std::string source;
    std::string reason;
    bool hasPeakAbs = false;
    float peakAbs = 0.0f;
    bool hasSourcePeakAbs = false;
    float sourcePeakAbs = 0.0f;
    std::vector<std::string> manifests;
};

// One "logicEntries" row: a section name and its entry count.
struct ReportLogicCount {
    std::string section;
    std::uint64_t count = 0;
};

struct ReportPack {
    std::string manifest;
    int manifestVersion = 1;
    std::string output;
    std::string container = "pack"; // or "stream"
    std::string packKind; // "" = v1 container (members not emitted)
    std::vector<ReportLogicCount> logicEntries;
    std::vector<ReportEntry> entries;
    std::uint64_t fileBytes = 0;
    std::uint64_t tocChecksum = 0;
    std::uint32_t cacheHits = 0;
    std::uint32_t cacheMisses = 0;
    std::vector<ReportWarning> warnings;
};

struct Report {
    bool refused = false;
    std::string refusalMessage;
    std::string refusalManifest;
    std::vector<ReportWarning> warnings; // cross-pack
    std::vector<ReportPack> packs;
};

inline void EmitWarning(JsonWriter* writer,
                        const ReportWarning& warning) {
    writer->BeginObject();
    writer->Key("kind");
    writer->ValueString(warning.kind);
    if (warning.hasId) {
        writer->Key("id");
        writer->ValueString(harkenstudio::ObjectIdString(warning.id));
    }
    if (!warning.source.empty()) {
        writer->Key("source");
        writer->ValueString(warning.source);
    }
    if (warning.hasPeakAbs) {
        writer->Key("peakAbs");
        writer->ValueRawNumber(FormatFloat(warning.peakAbs));
    }
    if (warning.hasSourcePeakAbs) {
        writer->Key("sourcePeakAbs");
        writer->ValueRawNumber(FormatFloat(warning.sourcePeakAbs));
    }
    if (!warning.reason.empty()) {
        writer->Key("reason");
        writer->ValueString(warning.reason);
    }
    if (!warning.manifests.empty()) {
        writer->Key("manifests");
        writer->BeginArray();
        for (const std::string& manifest : warning.manifests) {
            writer->ValueString(manifest);
        }
        writer->EndArray();
    }
    writer->EndObject();
}

inline std::string EmitReportJson(const Report& report) {
    JsonWriter writer;
    writer.BeginObject();
    writer.Key("reportVersion");
    writer.ValueU64(kReportVersion);
    writer.Key("tool");
    writer.ValueString("packc");
    writer.Key("status");
    writer.ValueString(report.refused ? "refused" : "built");
    if (report.refused) {
        writer.Key("refusal");
        writer.BeginObject();
        writer.Key("message");
        writer.ValueString(report.refusalMessage);
        if (!report.refusalManifest.empty()) {
            writer.Key("manifest");
            writer.ValueString(report.refusalManifest);
        }
        writer.EndObject();
    }
    writer.Key("warnings");
    writer.BeginArray();
    for (const ReportWarning& warning : report.warnings) {
        EmitWarning(&writer, warning);
    }
    writer.EndArray();
    writer.Key("packs");
    writer.BeginArray();
    for (const ReportPack& pack : report.packs) {
        writer.BeginObject();
        writer.Key("manifest");
        writer.ValueString(pack.manifest);
        writer.Key("manifestVersion");
        writer.ValueU64(static_cast<std::uint64_t>(
            pack.manifestVersion));
        writer.Key("output");
        writer.ValueString(pack.output);
        writer.Key("container");
        writer.ValueString(pack.container);
        if (!pack.packKind.empty()) {
            writer.Key("packKind");
            writer.ValueString(pack.packKind);
            writer.Key("logicEntries");
            writer.BeginObject();
            for (const ReportLogicCount& count : pack.logicEntries) {
                writer.Key(count.section.c_str());
                writer.ValueU64(count.count);
            }
            writer.EndObject();
        }
        writer.Key("entries");
        writer.BeginArray();
        for (const ReportEntry& entry : pack.entries) {
            writer.BeginObject();
            writer.Key("id");
            writer.ValueString(harkenstudio::ObjectIdString(entry.id));
            writer.Key("source");
            writer.ValueString(entry.source);
            writer.Key("frames");
            writer.ValueU64(entry.frames);
            writer.Key("rate");
            writer.ValueU64(entry.rate);
            writer.Key("channels");
            writer.ValueU64(entry.channels);
            writer.Key("checksum");
            writer.ValueString(JsonWriter::HexU64(entry.checksum));
            writer.Key("peakAbs");
            writer.ValueRawNumber(FormatFloat(entry.peakAbs));
            writer.Key("silent");
            writer.ValueBool(entry.silent);
            writer.Key("clipped");
            writer.ValueBool(entry.clipped);
            writer.Key("skippedByCache");
            writer.ValueBool(entry.skippedByCache);
            writer.EndObject();
        }
        writer.EndArray();
        writer.Key("totals");
        writer.BeginObject();
        writer.Key("entryCount");
        writer.ValueU64(pack.entries.size());
        writer.Key("fileBytes");
        writer.ValueU64(pack.fileBytes);
        writer.Key("tocChecksum");
        writer.ValueString(JsonWriter::HexU64(pack.tocChecksum));
        writer.Key("cacheHits");
        writer.ValueU64(pack.cacheHits);
        writer.Key("cacheMisses");
        writer.ValueU64(pack.cacheMisses);
        writer.EndObject();
        writer.Key("warnings");
        writer.BeginArray();
        for (const ReportWarning& warning : pack.warnings) {
            EmitWarning(&writer, warning);
        }
        writer.EndArray();
        writer.EndObject();
    }
    writer.EndArray();
    writer.EndObject();
    return writer.Finish();
}

} // namespace packc
} // namespace harkenstudiojson

#endif // HARKEN_HOST_PACKC_REPORT_H
