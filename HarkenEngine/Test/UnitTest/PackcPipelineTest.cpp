// Pack build pipeline hardening, driven through the real compiler tool
// as a subprocess (the pack_roundtrip precedent): v2 manifests as the
// canonical authoring input, incremental rebuilds through the build
// cache with byte-identity to clean builds, the machine-readable
// validation report (schema, seeded-defect warnings, refusal reporting),
// and cache-corruption degradation. The v1 command line's continued
// operation is pinned here too; its deeper fidelity coverage stays in
// PackRoundtripTest.cpp.
//
// The report and cache files are strict JSON, so the assertions parse
// them with the same reader the tool uses (harkenhost::ParseJson — a pure
// parsing helper, the one host inclusion this test tree sanctions).
//
// The wav writer below is test-local on purpose: tests never include
// host tool code (baseline-regression precedent), and float32 wav keeps
// the synth floats exact through the decode chain.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"
#include "UnitTest/JsonTestUtil.h"

#include "JsonReader.h"

#include "harken/media/PackFormat.h"
#include "harken/media/PackFormatV2.h"
#include "harken/param/EffectParamLocator.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#ifndef HARKEN_PACKC_PATH
#error "HARKEN_PACKC_PATH must point at the packc executable"
#endif
#ifndef HARKEN_TEMP_DIR
#error "HARKEN_TEMP_DIR must point at the repository's .temp directory"
#endif

namespace {

using harkenhost::JsonValue;
using harkentest::Member;
using harkentest::MemberBool;
using harkentest::MemberNumber;
using harkentest::MemberString;
using harkentest::MemberU64;
using harkentest::ParseJsonFile;
using harkentest::ReadFileText;

constexpr std::uint32_t kRate = 48000;

std::string TempPath(const std::string& name) {
    return std::string(HARKEN_TEMP_DIR "/") + name;
}

void WriteTextFile(const std::string& path, const std::string& text) {
    std::filesystem::create_directories(HARKEN_TEMP_DIR);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good()) << "cannot open " << path;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    ASSERT_TRUE(out.good()) << "cannot write " << path;
}

bool ReadFileBytes(const std::string& path,
                   std::vector<std::uint8_t>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out->assign(std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());
    return true;
}

void WriteFileBytes(const std::string& path,
                    const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good()) << "cannot open " << path;
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(out.good()) << "cannot write " << path;
}

// Runs packc with the given argument list and returns the process exit
// code. Paths from the build system arrive with forward slashes;
// cmd.exe wants backslashes for the program itself, and the doubled
// outer quotes survive cmd's quote-stripping rule for the /C payload.
int RunPackc(const std::vector<std::string>& args) {
    std::string exe = HARKEN_PACKC_PATH;
    for (char& c : exe) {
        if (c == '/') {
            c = '\\';
        }
    }
    std::string command = "\"\"" + exe + "\"";
    for (const std::string& arg : args) {
        command += " \"" + arg + "\"";
    }
    command += "\"";
    return std::system(command.c_str());
}

// Minimal IEEE float32 (format tag 3) RIFF/WAVE writer: 18-byte fmt chunk
// (cbSize 0) plus the fact chunk the float format prescribes. Samples are
// written verbatim — no conversion, no dithering — which is the point.
void WriteWavF32(const std::string& path,
                 const std::vector<float>& interleaved,
                 std::uint32_t channels, std::uint32_t sampleRate) {
    ASSERT_NE(0u, channels);
    ASSERT_EQ(0u, interleaved.size() % channels);
    std::filesystem::create_directories(HARKEN_TEMP_DIR);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good()) << "cannot open " << path;

    const std::uint32_t bytesPerSample = 4;
    const std::uint16_t blockAlign =
        static_cast<std::uint16_t>(channels * bytesPerSample);
    const std::uint32_t dataBytes =
        static_cast<std::uint32_t>(interleaved.size() * bytesPerSample);
    const std::uint32_t byteRate = sampleRate * blockAlign;
    const std::uint32_t fmtSize = 18;
    const std::uint32_t factSize = 4;
    const std::uint32_t riffSize =
        4 + (8 + fmtSize) + (8 + factSize) + (8 + dataBytes);
    const std::uint16_t formatIeeeFloat = 3;
    const std::uint16_t bitsPerSample = 32;
    const std::uint16_t cbSize = 0;
    const std::uint16_t channels16 = static_cast<std::uint16_t>(channels);
    const std::uint32_t sampleFrames =
        static_cast<std::uint32_t>(interleaved.size() / channels);

    auto write = [&](const void* data, std::size_t bytes) {
        out.write(static_cast<const char*>(data),
                  static_cast<std::streamsize>(bytes));
    };
    write("RIFF", 4);
    write(&riffSize, 4);
    write("WAVE", 4);
    write("fmt ", 4);
    write(&fmtSize, 4);
    write(&formatIeeeFloat, 2);
    write(&channels16, 2);
    write(&sampleRate, 4);
    write(&byteRate, 4);
    write(&blockAlign, 2);
    write(&bitsPerSample, 2);
    write(&cbSize, 2);
    write("fact", 4);
    write(&factSize, 4);
    write(&sampleFrames, 4);
    write("data", 4);
    write(&dataBytes, 4);
    write(interleaved.data(), interleaved.size() * sizeof(float));
    ASSERT_TRUE(out.good()) << "cannot write " << path;
}

std::string HexU64(std::uint64_t value) {
    char buffer[17];
    std::snprintf(buffer, sizeof buffer, "%016llx",
                  static_cast<unsigned long long>(value));
    return std::string(buffer);
}

// Replaces exactly one occurrence, asserting it exists — cache-text
// surgery must never silently do nothing.
void ReplaceOnce(std::string* text, const std::string& from,
                 const std::string& to) {
    const std::size_t pos = text->find(from);
    ASSERT_NE(std::string::npos, pos)
        << "expected to find \"" << from << '"';
    text->replace(pos, from.size(), to);
}

// Re-stamps a hand-tampered cache file's whole-file integrity value the
// same way the tool computes it (FNV-1a 64 over the text with the value
// slot held at the all-zero placeholder), so a test can prove the
// per-entry defenses fire even when the file checksum itself is
// coherent.
void RestampCacheIntegrity(std::string* text) {
    const std::string marker = "\"integrity\": \"";
    const std::size_t pos = text->find(marker);
    ASSERT_NE(std::string::npos, pos);
    const std::size_t valuePos = pos + marker.size();
    text->replace(valuePos, 16, "0000000000000000");
    const std::uint64_t hash =
        harken::Fnv1a64(text->data(), text->size());
    text->replace(valuePos, 16, HexU64(hash));
}

// Validates the fixed report skeleton and returns pointers into the
// parsed tree. Every test goes through this, so the schema is pinned in
// one place.
void RequireReportSkeleton(const JsonValue& report,
                           const char* expectedStatus) {
    EXPECT_EQ(2u, MemberU64(report, "reportVersion"));
    EXPECT_EQ("packc", MemberString(report, "tool"));
    EXPECT_EQ(expectedStatus, MemberString(report, "status"));
    EXPECT_EQ(JsonValue::Kind::Array,
              Member(report, "warnings").kind);
    EXPECT_EQ(JsonValue::Kind::Array, Member(report, "packs").kind);
}

const JsonValue* FindWarning(const JsonValue& warnings,
                             const std::string& kind,
                             const std::string& idText = std::string()) {
    for (const JsonValue& warning : warnings.items) {
        const JsonValue* kindValue = warning.Find("kind");
        if (kindValue == nullptr || kindValue->text != kind) {
            continue;
        }
        if (idText.empty()) {
            return &warning;
        }
        const JsonValue* idValue = warning.Find("id");
        if (idValue != nullptr && idValue->text == idText) {
            return &warning;
        }
    }
    return nullptr;
}

bool HasWarning(const JsonValue& warnings, const std::string& kind,
                const std::string& idText = std::string()) {
    return FindWarning(warnings, kind, idText) != nullptr;
}

struct BuiltEntryFacts {
    VnObjectId id{};
    std::uint64_t frames = 0;
    std::uint32_t rate = 0;
    std::uint16_t channels = 0;
    std::uint64_t blobChecksum = 0;
    std::uint32_t flags = 0;
    std::uint64_t loopStartFrame = 0;
    std::uint64_t loopEndFrame = 0;
    std::vector<float> planar;
};

// Parses the emitted .pack and returns per-entry TOC facts in order,
// verifying the container-level checksums along the way.
void ReadPackFacts(const std::string& packPath,
                   std::vector<BuiltEntryFacts>* out,
                   std::uint64_t* outFileBytes,
                   std::uint64_t* outTocChecksum) {
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(ReadFileBytes(packPath, &bytes)) << packPath;
    ASSERT_GE(bytes.size(),
              static_cast<std::size_t>(harken::kPackHeaderBytes));
    harken::PackHeader header;
    ASSERT_TRUE(harken::ParsePackHeader(bytes.data(), &header));
    ASSERT_EQ(bytes.size(), header.fileBytes);
    *outFileBytes = header.fileBytes;
    *outTocChecksum = header.tocChecksum;
    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        const harken::PackTocEntry toc = harken::ParsePackTocEntry(
            bytes.data() + header.tocOffset +
            i * harken::kPackTocEntryBytes);
        ASSERT_LE(toc.blobOffset + toc.blobBytes, bytes.size());
        EXPECT_EQ(toc.blobChecksum,
                  harken::Fnv1a64(bytes.data() + toc.blobOffset,
                                 static_cast<std::size_t>(
                                     toc.blobBytes)));
        BuiltEntryFacts facts;
        facts.id = toc.soundId;
        facts.frames = toc.frameCount;
        facts.rate = toc.sampleRate;
        facts.channels = toc.channelCount;
        facts.blobChecksum = toc.blobChecksum;
        facts.flags = toc.flags;
        facts.loopStartFrame = toc.loopStartFrame;
        facts.loopEndFrame = toc.loopEndFrame;
        facts.planar.resize(
            static_cast<std::size_t>(toc.blobBytes / sizeof(float)));
        std::memcpy(facts.planar.data(),
                    bytes.data() + toc.blobOffset,
                    static_cast<std::size_t>(toc.blobBytes));
        out->push_back(facts);
    }
}

std::string V2EntryJson(const std::string& wavPath, std::uint64_t id,
                        bool loopable = false,
                        std::uint64_t loopStart = 0,
                        std::uint64_t loopEnd = 0,
                        const std::string& downmix = std::string()) {
    std::string entry =
        "    { \"path\": \"" + wavPath + "\", \"id\": " +
        harkentest::JsonObjectId(harkentest::ObjectId(id));
    if (loopable) {
        entry += ", \"loopStartFrame\": " + std::to_string(loopStart) +
                 ", \"loopEndFrame\": " + std::to_string(loopEnd);
    }
    if (!downmix.empty()) {
        entry += ", \"downmix\": \"" + downmix + "\"";
    }
    entry += " }";
    return entry;
}

std::string V2ManifestJson(const std::string& outputPath,
                           const std::vector<std::string>& entryJson,
                           const std::string& optionsJson = "") {
    std::string manifest = "{\n  \"manifestVersion\": 3,\n"
                           "  \"output\": \"" + outputPath + "\",\n";
    if (!optionsJson.empty()) {
        manifest += "  \"options\": " + optionsJson + ",\n";
    }
    manifest += "  \"entries\": [\n";
    for (std::size_t i = 0; i < entryJson.size(); ++i) {
        manifest += entryJson[i];
        manifest += (i + 1 < entryJson.size()) ? ",\n" : "\n";
    }
    manifest += "  ]\n}\n";
    return manifest;
}

TEST(PackcPipeline, ManifestV2BuildsAndReports) {
    const std::vector<float> tone =
        harkentest::MakeSine(700, 997.0, kRate, 0.6f);
    const std::vector<float> noise =
        harkentest::MakeSine(1000, 443.0, kRate, 0.5f);
    const std::string tonePath = TempPath("pipeline_v2_tone.wav");
    const std::string noisePath = TempPath("pipeline_v2_noise.wav");
    WriteWavF32(tonePath, tone, 1, kRate);
    WriteWavF32(noisePath, noise, 1, kRate);

    const std::string packPath = TempPath("pipeline_v2.pack");
    const std::string manifestPath =
        TempPath("pipeline_v2_manifest.json");
    WriteTextFile(manifestPath,
                  V2ManifestJson(packPath,
                                 {V2EntryJson(tonePath, 101),
                                  V2EntryJson(noisePath, 102, true, 100,
                                              500)}));
    std::filesystem::remove(packPath);
    std::filesystem::remove(packPath + ".cache.json");
    std::filesystem::remove(packPath + ".report.json");

    ASSERT_EQ(0, RunPackc({manifestPath}));

    std::vector<BuiltEntryFacts> facts;
    std::uint64_t fileBytes = 0;
    std::uint64_t tocChecksum = 0;
    ReadPackFacts(packPath, &facts, &fileBytes, &tocChecksum);
    ASSERT_EQ(2u, facts.size());
    EXPECT_EQ(harkentest::ObjectId(101u), facts[0].id);
    EXPECT_EQ(harkentest::ObjectId(102u), facts[1].id);

    // The report lands beside the output by default and mirrors the
    // emitted container exactly.
    JsonValue report;
    ParseJsonFile(packPath + ".report.json", &report);
    RequireReportSkeleton(report, "built");
    const JsonValue& packs = Member(report, "packs");
    ASSERT_EQ(1u, packs.items.size());
    const JsonValue& pack = packs.items[0];
    EXPECT_EQ(manifestPath, MemberString(pack, "manifest"));
    EXPECT_EQ(3u, MemberU64(pack, "manifestVersion"));
    EXPECT_EQ(packPath, MemberString(pack, "output"));
    EXPECT_EQ("pack", MemberString(pack, "container"));
    const JsonValue& entries = Member(pack, "entries");
    ASSERT_EQ(2u, entries.items.size());
    for (std::size_t i = 0; i < 2; ++i) {
        const JsonValue& entry = entries.items[i];
        EXPECT_EQ(harken::ObjectIdString(facts[i].id),
                  MemberString(entry, "id"));
        EXPECT_EQ(facts[i].frames, MemberU64(entry, "frames"));
        EXPECT_EQ(facts[i].rate, MemberU64(entry, "rate"));
        EXPECT_EQ(facts[i].channels, MemberU64(entry, "channels"));
        EXPECT_EQ(HexU64(facts[i].blobChecksum),
                  MemberString(entry, "checksum"));
        EXPECT_FALSE(MemberBool(entry, "silent"));
        EXPECT_FALSE(MemberBool(entry, "clipped"));
        EXPECT_FALSE(MemberBool(entry, "skippedByCache"));
        EXPECT_GT(MemberNumber(entry, "peakAbs"), 0.4);
    }
    EXPECT_EQ(tonePath, MemberString(entries.items[0], "source"));
    const JsonValue& totals = Member(pack, "totals");
    EXPECT_EQ(2u, MemberU64(totals, "entryCount"));
    EXPECT_EQ(fileBytes, MemberU64(totals, "fileBytes"));
    EXPECT_EQ(HexU64(tocChecksum), MemberString(totals, "tocChecksum"));
    EXPECT_EQ(0u, MemberU64(totals, "cacheHits"));
    EXPECT_EQ(2u, MemberU64(totals, "cacheMisses"));
    EXPECT_TRUE(Member(pack, "warnings").items.empty());
    EXPECT_TRUE(Member(report, "warnings").items.empty());

    // v2 caches by default: the cache file appears beside the output.
    EXPECT_TRUE(
        std::filesystem::exists(packPath + ".cache.json"));
}

TEST(PackcPipeline, EmptyContentV2IsDeterministicHeaderOnly) {
    const std::string firstPath = TempPath("pipeline_empty_first.pack");
    const std::string secondPath = TempPath("pipeline_empty_second.pack");
    const std::string firstManifest =
        TempPath("pipeline_empty_first_manifest.json");
    const std::string secondManifest =
        TempPath("pipeline_empty_second_manifest.json");
    WriteTextFile(
        firstManifest,
        "{\"manifestVersion\":3,\"output\":\"" + firstPath +
            "\",\"packKind\":\"content\",\"options\":{"
            "\"cache\":false}}");
    WriteTextFile(
        secondManifest,
        "{\"manifestVersion\":3,\"output\":\"" + secondPath +
            "\",\"packKind\":\"content\",\"options\":{"
            "\"cache\":false}}");
    ASSERT_EQ(0, RunPackc({firstManifest}));
    ASSERT_EQ(0, RunPackc({secondManifest}));

    std::vector<std::uint8_t> first;
    std::vector<std::uint8_t> second;
    ASSERT_TRUE(ReadFileBytes(firstPath, &first));
    ASSERT_TRUE(ReadFileBytes(secondPath, &second));
    ASSERT_EQ(harken::kPackV2HeaderBytes, first.size());
    ASSERT_EQ(first, second);

    harken::PackV2Header header;
    ASSERT_TRUE(harken::ParsePackV2Header(first.data(), &header));
    EXPECT_EQ(harken::kPackFormatVersion2, header.formatVersion);
    EXPECT_EQ(harken::kPackV2HeaderBytes, header.headerBytes);
    EXPECT_EQ(harken::kPackV2HeaderBytes, header.fileBytes);
    EXPECT_EQ(harken::kPackKindContent, header.packKind);
    EXPECT_EQ(0u, header.entryCount);
    EXPECT_EQ(harken::kPackV2HeaderBytes, header.directoryOffset);
    EXPECT_EQ(harken::kFnv1a64OffsetBasis, header.directoryChecksum);
}

TEST(PackcPipeline, EmptyResidentV2Refuses) {
    const std::string packPath = TempPath("pipeline_empty_resident.pack");
    const std::string manifestPath =
        TempPath("pipeline_empty_resident_manifest.json");
    WriteTextFile(
        manifestPath,
        "{\"manifestVersion\":3,\"output\":\"" + packPath +
            "\",\"packKind\":\"resident\",\"options\":{"
            "\"cache\":false}}");
    std::filesystem::remove(packPath);
    EXPECT_NE(0, RunPackc({manifestPath}));
    EXPECT_FALSE(std::filesystem::exists(packPath));
}

TEST(PackcPipeline, DownmixPolicyIsExactAndPreservesFormatFacts) {
    const std::vector<float> stereo = {
        0.75f, 0.75f,
        1.0f, -1.0f,
        0.25f, -0.5f,
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
    };
    const std::vector<float> mono = {0.5f, -0.25f, 0.125f};
    const std::vector<float> antiPhase = {
        0.8f, -0.8f,
        -0.5f, 0.5f,
        0.25f, -0.25f,
    };
    const std::string stereoPath = TempPath("pipeline_downmix_stereo.wav");
    const std::string monoPath = TempPath("pipeline_downmix_mono.wav");
    const std::string antiPath = TempPath("pipeline_downmix_anti.wav");
    WriteWavF32(stereoPath, stereo, 2, kRate);
    WriteWavF32(monoPath, mono, 1, kRate);
    WriteWavF32(antiPath, antiPhase, 2, kRate);

    const std::string packPath = TempPath("pipeline_downmix.pack");
    const std::string manifestPath =
        TempPath("pipeline_downmix_manifest.json");
    WriteTextFile(
        manifestPath,
        V2ManifestJson(
            packPath,
            {V2EntryJson(stereoPath, 1101),
             V2EntryJson(stereoPath, 1102, true, 1, 3, "mono"),
             V2EntryJson(monoPath, 1103, false, 0, 0, "mono"),
             V2EntryJson(antiPath, 1104, false, 0, 0, "mono")},
            "{ \"cache\": false }"));
    ASSERT_EQ(0, RunPackc({manifestPath}));

    std::vector<BuiltEntryFacts> facts;
    std::uint64_t fileBytes = 0;
    std::uint64_t tocChecksum = 0;
    ReadPackFacts(packPath, &facts, &fileBytes, &tocChecksum);
    ASSERT_EQ(4u, facts.size());

    EXPECT_EQ(harkentest::ObjectId(1101u), facts[0].id);
    EXPECT_EQ(kRate, facts[0].rate);
    EXPECT_EQ(4u, facts[0].frames);
    EXPECT_EQ(2u, facts[0].channels);
    EXPECT_EQ((std::vector<float>{
                  0.75f, 1.0f, 0.25f,
                  std::numeric_limits<float>::max(),
                  0.75f, -1.0f, -0.5f,
                  std::numeric_limits<float>::max()}),
              facts[0].planar)
        << "an absent policy must preserve stereo planes";

    EXPECT_EQ(harkentest::ObjectId(1102u), facts[1].id);
    EXPECT_EQ(kRate, facts[1].rate);
    EXPECT_EQ(4u, facts[1].frames);
    EXPECT_EQ(1u, facts[1].channels);
    EXPECT_EQ(harken::kPackFlagLoopable, facts[1].flags);
    EXPECT_EQ(1u, facts[1].loopStartFrame);
    EXPECT_EQ(3u, facts[1].loopEndFrame);
    EXPECT_EQ((std::vector<float>{
                  0.75f, 0.0f, -0.125f,
                  std::numeric_limits<float>::max()}),
              facts[1].planar)
        << "the golden pins fake stereo, anti-phase, and one final "
           "rounding without float-sum overflow";

    EXPECT_EQ(harkentest::ObjectId(1103u), facts[2].id);
    EXPECT_EQ(kRate, facts[2].rate);
    EXPECT_EQ(3u, facts[2].frames);
    EXPECT_EQ(1u, facts[2].channels);
    EXPECT_EQ(mono, facts[2].planar)
        << "a mono source under the mono policy must remain unchanged";

    EXPECT_EQ(harkentest::ObjectId(1104u), facts[3].id);
    EXPECT_EQ(kRate, facts[3].rate);
    EXPECT_EQ(3u, facts[3].frames);
    EXPECT_EQ(1u, facts[3].channels);
    EXPECT_EQ((std::vector<float>{0.0f, 0.0f, 0.0f}),
              facts[3].planar);

    JsonValue report;
    ParseJsonFile(packPath + ".report.json", &report);
    const JsonValue& pack = Member(report, "packs").items.at(0);
    const JsonValue& entries = Member(pack, "entries");
    ASSERT_EQ(4u, entries.items.size());
    EXPECT_EQ(2u, MemberU64(entries.items[0], "channels"));
    EXPECT_EQ(1u, MemberU64(entries.items[1], "channels"));
    EXPECT_EQ(1u, MemberU64(entries.items[2], "channels"));
    EXPECT_EQ(1u, MemberU64(entries.items[3], "channels"));

    const JsonValue& warnings = Member(pack, "warnings");
    EXPECT_TRUE(HasWarning(warnings, "downmix-cancellation", harken::ObjectIdString(harkentest::ObjectId(1104))));
    // The anti-phase entry cancels to true silence, so the independent
    // silent-entry facet fires alongside the cancellation warning: the two
    // conditions are separate report facts, not an either-or.
    EXPECT_TRUE(HasWarning(warnings, "silent-entry", harken::ObjectIdString(harkentest::ObjectId(1104))));
    EXPECT_FALSE(HasWarning(warnings, "downmix-cancellation", harken::ObjectIdString(harkentest::ObjectId(1102))));
    EXPECT_FALSE(HasWarning(warnings, "silent-entry", harken::ObjectIdString(harkentest::ObjectId(1102))));
    const JsonValue* cancellation =
        FindWarning(warnings, "downmix-cancellation", harken::ObjectIdString(harkentest::ObjectId(1104)));
    ASSERT_NE(nullptr, cancellation);
    EXPECT_EQ(0.0, MemberNumber(*cancellation, "peakAbs"));
    EXPECT_NEAR(0.8, MemberNumber(*cancellation, "sourcePeakAbs"),
                1.0e-7);
}

TEST(PackcPipeline, DownmixRefusesMultichannelWithoutFallbackOutput) {
    const std::vector<float> threeChannel = {
        0.5f, 0.25f, -0.25f,
        -0.5f, 0.125f, 0.75f,
    };
    const std::string wavPath = TempPath("pipeline_downmix_3ch.wav");
    WriteWavF32(wavPath, threeChannel, 3, kRate);
    const std::string packPath = TempPath("pipeline_downmix_3ch.pack");
    const std::string manifestPath =
        TempPath("pipeline_downmix_3ch_manifest.json");
    WriteTextFile(
        manifestPath,
        V2ManifestJson(
            packPath,
            {V2EntryJson(wavPath, 1110, false, 0, 0, "mono")},
            "{ \"cache\": false }"));
    std::filesystem::remove(packPath);

    EXPECT_NE(0, RunPackc({manifestPath}));
    EXPECT_FALSE(std::filesystem::exists(packPath))
        << "a refused transform must not fall back to multichannel";
    JsonValue report;
    ParseJsonFile(packPath + ".report.json", &report);
    RequireReportSkeleton(report, "refused");
    EXPECT_NE(std::string::npos,
              MemberString(Member(report, "refusal"), "message")
                  .find("source has 3 channels"));
}

TEST(PackcPipeline, PreserveEntryRefusesMultichannelSource) {
    const std::vector<float> threeChannel = {
        0.5f, 0.25f, -0.25f,
        -0.5f, 0.125f, 0.75f,
    };
    const std::string wavPath = TempPath("pipeline_preserve_3ch.wav");
    WriteWavF32(wavPath, threeChannel, 3, kRate);
    const std::string packPath = TempPath("pipeline_preserve_3ch.pack");
    const std::string manifestPath =
        TempPath("pipeline_preserve_3ch_manifest.json");
    WriteTextFile(
        manifestPath,
        V2ManifestJson(
            packPath,
            {V2EntryJson(wavPath, 1111)},
            "{ \"cache\": false }"));
    std::filesystem::remove(packPath);

    EXPECT_NE(0, RunPackc({manifestPath}));
    EXPECT_FALSE(std::filesystem::exists(packPath))
        << "a preserve entry must keep the mono/stereo decode bound";
    JsonValue report;
    ParseJsonFile(packPath + ".report.json", &report);
    RequireReportSkeleton(report, "refused");
    EXPECT_NE(std::string::npos,
              MemberString(Member(report, "refusal"), "message")
                  .find("cannot open or decode"));
}

TEST(PackcPipeline, ManifestV2RoundTrips) {
    const std::vector<float> tone =
        harkentest::MakeSine(600, 523.0, kRate, 0.5f);
    const std::string wavPath = TempPath("pipeline_rt_tone.wav");
    WriteWavF32(wavPath, tone, 1, kRate);

    const std::string packPath = TempPath("pipeline_rt.pack");
    const std::string manifestPath =
        TempPath("pipeline_rt_manifest.json");
    const std::string emittedPath =
        TempPath("pipeline_rt_emitted.json");
    const std::string emittedAgainPath =
        TempPath("pipeline_rt_emitted2.json");
    WriteTextFile(manifestPath,
                  V2ManifestJson(packPath, {V2EntryJson(wavPath, 501,
                                                        true, 50,
                                                        400, "mono")}));

    ASSERT_EQ(0,
              RunPackc({manifestPath, "--emit-manifest", emittedPath}));
    std::vector<std::uint8_t> firstBuild;
    ASSERT_TRUE(ReadFileBytes(packPath, &firstBuild));

    // The emitted manifest is a v2 document that reproduces the build.
    JsonValue emitted;
    ParseJsonFile(emittedPath, &emitted);
    EXPECT_EQ(3u, MemberU64(emitted, "manifestVersion"));
    EXPECT_EQ(packPath, MemberString(emitted, "output"));
    const JsonValue& entries = Member(emitted, "entries");
    ASSERT_EQ(1u, entries.items.size());
    EXPECT_EQ(harken::ObjectIdString(harkentest::ObjectId(501)),
              MemberString(entries.items[0], "id"));
    EXPECT_EQ(50u, MemberU64(entries.items[0], "loopStartFrame"));
    EXPECT_EQ(400u, MemberU64(entries.items[0], "loopEndFrame"));
    EXPECT_EQ("mono", MemberString(entries.items[0], "downmix"));

    ASSERT_EQ(0, RunPackc({emittedPath, "--no-cache",
                           "--emit-manifest", emittedAgainPath}));
    std::vector<std::uint8_t> rebuild;
    ASSERT_TRUE(ReadFileBytes(packPath, &rebuild));
    EXPECT_EQ(firstBuild, rebuild)
        << "a build from the emitted manifest must be byte-identical";

    // Emit(parse(emit(...))) is a fixpoint: normalization is stable.
    std::string emittedText;
    std::string emittedAgainText;
    ASSERT_TRUE(ReadFileText(emittedPath, &emittedText));
    ASSERT_TRUE(ReadFileText(emittedAgainPath, &emittedAgainText));
    EXPECT_EQ(emittedText, emittedAgainText);
}

TEST(PackcPipeline,
     VoiceRecipeAllBuiltinEffectsAreACanonicalManifestFixpoint) {
    const std::string wavPath =
        TempPath("pipeline_recipe_all_effects.wav");
    WriteWavF32(wavPath, harkentest::MakeSine(1024, 440.0, kRate, 0.5f),
                1, kRate);
    const std::string packPath =
        TempPath("pipeline_recipe_all_effects.pack");
    const std::string manifestPath =
        TempPath("pipeline_recipe_all_effects_manifest.json");
    const std::string emittedPath =
        TempPath("pipeline_recipe_all_effects_emitted.json");
    const std::string emittedAgainPath =
        TempPath("pipeline_recipe_all_effects_emitted_again.json");
    WriteTextFile(
        manifestPath,
        "{\n"
        "  \"manifestVersion\": 3,\n"
        "  \"output\": \"" + packPath + "\",\n"
        "  \"packKind\": \"content\",\n"
        "  \"options\": {\"cache\": false},\n"
        "  \"entries\": [{\"path\": \"" + wavPath +
            "\", \"id\": \"3da91a64-b947-4560-0000-0000000023be\"}],\n"
        "  \"voiceRecipes\": [{\n"
        "    \"playable\": \"3da91a64-b947-4560-0000-0000000023be\",\n"
        "    \"effects\": [\n"
        "      {\"slot\": 3, \"type\": \"limiter\", "
        "\"descriptorVintage\": 2, \"bypass\": false, "
        "\"parameters\": [-3.0, 150.0]},\n"
        "      {\"slot\": 1, \"type\": \"threeBandTone\", "
        "\"descriptorVintage\": 2, \"bypass\": true, "
        "\"parameters\": [6.0, 400.0, -3.0, 1200.0, 2.0, -4.0, "
        "6000.0]},\n"
        "      {\"slot\": 0, \"type\": \"feedbackDelay\", "
        "\"descriptorVintage\": 2, \"bypass\": false, "
        "\"parameters\": [10.0, 0.25, 0.75, 0.5]},\n"
        "      {\"slot\": 2, \"type\": \"gain\", "
        "\"descriptorVintage\": 2, \"bypass\": false, "
        "\"parameters\": [-6.0]}\n"
        "    ],\n"
        "    \"bindings\": [\"3da91a64-b947-4560-0000-00000000232b\", \"3da91a64-b947-4560-0000-000000002329\", \"3da91a64-b947-4560-0000-00000000232a\"]\n"
        "  }]\n"
        "}\n");

    ASSERT_EQ(0,
              RunPackc({manifestPath, "--emit-manifest", emittedPath}));
    std::vector<std::uint8_t> firstBuild;
    ASSERT_TRUE(ReadFileBytes(packPath, &firstBuild));
    ASSERT_FALSE(firstBuild.empty());
    ASSERT_GE(firstBuild.size(),
              static_cast<std::size_t>(
                  harken::kPackV2HeaderBytes +
                  2 * harken::kPackV2DirectoryEntryBytes));

    harken::PackV2Header header;
    ASSERT_TRUE(harken::ParsePackV2Header(firstBuild.data(), &header));
    EXPECT_EQ(harken::kPackFormatVersion2, header.formatVersion);
    EXPECT_EQ(harken::kPackV2HeaderBytes, header.headerBytes);
    EXPECT_EQ(firstBuild.size(), header.fileBytes);
    EXPECT_EQ(harken::kPackKindContent, header.packKind);
    ASSERT_EQ(2u, header.entryCount);
    ASSERT_LE(header.directoryOffset, firstBuild.size());
    ASSERT_LE(2u * harken::kPackV2DirectoryEntryBytes,
              firstBuild.size() -
                  static_cast<std::size_t>(header.directoryOffset));
    const harken::PackV2DirectoryEntry mediaEntry =
        harken::ParsePackV2DirectoryEntry(
            firstBuild.data() + header.directoryOffset);
    const harken::PackV2DirectoryEntry recipeEntry =
        harken::ParsePackV2DirectoryEntry(
            firstBuild.data() + header.directoryOffset +
            harken::kPackV2DirectoryEntryBytes);
    EXPECT_EQ(harken::kPackEntryKindMedia, mediaEntry.kind);
    EXPECT_EQ(harkentest::ObjectId(9150u), mediaEntry.id);
    EXPECT_EQ(harken::kPackEntryKindVoiceRecipe, recipeEntry.kind);
    EXPECT_EQ(harkentest::ObjectId(9150u), recipeEntry.id);
    ASSERT_LE(recipeEntry.offset, firstBuild.size());
    ASSERT_LE(recipeEntry.size,
              firstBuild.size() -
                  static_cast<std::size_t>(recipeEntry.offset));
    EXPECT_EQ(harken::kPackV2PlaybackRecipeSchemaVintage, recipeEntry.vintage);
    EXPECT_EQ(harken::PackV5VoiceRecipePayloadBytes(4, 3), recipeEntry.size);
    harken::PackVoiceRecipeEntryHeader packedRecipe;
    ASSERT_TRUE(harken::ParsePackV5VoiceRecipeHeader(
        firstBuild.data() + recipeEntry.offset, recipeEntry.size,
        &packedRecipe));
    EXPECT_EQ(harkentest::ObjectId(9150u), packedRecipe.playableId);
    EXPECT_EQ(4u, packedRecipe.installCount);
    EXPECT_EQ(3u, packedRecipe.bindingCount);

    JsonValue emitted;
    ParseJsonFile(emittedPath, &emitted);
    const JsonValue& recipes = Member(emitted, "voiceRecipes");
    ASSERT_EQ(1u, recipes.items.size());
    const JsonValue& effects = Member(recipes.items[0], "effects");
    ASSERT_EQ(4u, effects.items.size());
    const char* expectedTypes[] = {
        "feedbackDelay", "threeBandTone", "gain", "limiter"};
    const bool expectedBypass[] = {false, true, false, false};
    const std::vector<std::vector<double>> expectedParameters = {
        {10.0, 0.25, 0.75, 0.5},
        {6.0, 400.0, -3.0, 1200.0, 2.0, -4.0, 6000.0},
        {-6.0},
        {-3.0, 150.0}};
    for (std::size_t i = 0; i < effects.items.size(); ++i) {
        EXPECT_EQ(i, MemberU64(effects.items[i], "slot"));
        EXPECT_EQ(expectedTypes[i],
                  MemberString(effects.items[i], "type"));
        EXPECT_EQ(2u, MemberU64(effects.items[i], "descriptorVintage"));
        EXPECT_EQ(expectedBypass[i],
                  MemberBool(effects.items[i], "bypass"));
        const JsonValue& parameters =
            Member(effects.items[i], "parameters");
        ASSERT_EQ(JsonValue::Kind::Array, parameters.kind);
        ASSERT_EQ(expectedParameters[i].size(), parameters.items.size());
        for (std::size_t parameterIndex = 0;
             parameterIndex < parameters.items.size(); ++parameterIndex) {
            const JsonValue& value = parameters.items[parameterIndex];
            ASSERT_EQ(JsonValue::Kind::Number, value.kind);
            char* end = nullptr;
            const double actual = std::strtod(value.text.c_str(), &end);
            ASSERT_EQ(value.text.c_str() + value.text.size(), end);
            EXPECT_DOUBLE_EQ(expectedParameters[i][parameterIndex], actual);
        }
    }
    const JsonValue& bindings = Member(recipes.items[0], "bindings");
    ASSERT_EQ(3u, bindings.items.size());
    const std::uint64_t expectedBindings[] = {9001u, 9002u, 9003u};
    for (std::size_t i = 0; i < bindings.items.size(); ++i) {
        ASSERT_EQ(JsonValue::Kind::String, bindings.items[i].kind);
        EXPECT_EQ(harken::ObjectIdString(harkentest::ObjectId(expectedBindings[i])),
                  bindings.items[i].text);
    }

    ASSERT_EQ(0, RunPackc({emittedPath, "--no-cache",
                           "--emit-manifest", emittedAgainPath}));
    std::vector<std::uint8_t> rebuild;
    ASSERT_TRUE(ReadFileBytes(packPath, &rebuild));
    EXPECT_EQ(firstBuild, rebuild)
        << "the canonical voice Recipe rebuild must be byte-identical";

    std::string emittedText;
    std::string emittedAgainText;
    ASSERT_TRUE(ReadFileText(emittedPath, &emittedText));
    ASSERT_TRUE(ReadFileText(emittedAgainPath, &emittedAgainText));
    EXPECT_EQ(emittedText, emittedAgainText)
        << "emit(parse(emit)) must preserve canonical Recipe text";
}

TEST(PackcPipeline, BezierManifestHandlesLengthMatrix) {
    const std::string validCurve =
        R"({"points":[[0.0,0.0],[100.0,-6.0]],"handles":[[0.0,0.0,100.0,-6.0]]})";
    const std::string cases[] = {
        validCurve,
        R"({"points":[[0.0,0.0],[100.0,-6.0]]})",
        R"({"points":[[0.0,0.0],[50.0,-3.0],[100.0,-6.0]],"handles":[[0.0,0.0,50.0,-3.0]]})",
        R"({"points":[[0.0,0.0],[100.0,-6.0]],"handles":[[0.0,0.0,100.0,-6.0],[0.0,0.0,100.0,-6.0]]})",
        R"({"points":[[0.0,0.0]]})",
        R"({"points":[[0.0,0.0]],"handles":[]})",
    };
    const bool accepted[] = {true, false, false, false, true, false};
    for (std::size_t i = 0; i < std::size(cases); ++i) {
        const std::string packPath =
            TempPath("pipeline_handles_matrix_" + std::to_string(i) +
                     ".pack");
        const std::string manifestPath =
            TempPath("pipeline_handles_matrix_" + std::to_string(i) +
                     "_manifest.json");
        WriteTextFile(
            manifestPath,
            "{ \"manifestVersion\": 3, \"output\": \"" + packPath +
                "\", \"packKind\": \"content\", "
                "\"options\": { \"cache\": false }, "
                "\"bindings\": [ { \"id\": \"3da91a64-b947-4560-0000-000000001f41\", \"variable\": \"3da91a64-b947-4560-0000-000000001f42\", "
                "\"scope\": \"voice\", \"target\": 0, \"curve\": " +
                cases[i] + " } ] }");
        std::filesystem::remove(packPath);
        const int result = RunPackc({manifestPath});
        if (accepted[i]) {
            EXPECT_EQ(0, result) << "matrix case " << i;
            EXPECT_TRUE(std::filesystem::exists(packPath))
                << "matrix case " << i;
        } else {
            EXPECT_NE(0, result) << "matrix case " << i;
            EXPECT_FALSE(std::filesystem::exists(packPath))
                << "matrix case " << i;
        }
    }
}

TEST(PackcPipeline, EffectParamBindingScopesRoundTripCanonically) {
    const std::string packPath =
        TempPath("pipeline_effect_param_binding.pack");
    const std::string manifestPath =
        TempPath("pipeline_effect_param_binding_manifest.json");
    const std::string emittedPath =
        TempPath("pipeline_effect_param_binding_emitted.json");
    const std::string emittedAgainPath =
        TempPath("pipeline_effect_param_binding_emitted_again.json");
    const std::uint32_t target =
        harken::PackEffectParamLocator(VN_EFFECT_GAIN, 0, 0);
    WriteTextFile(
        manifestPath,
        "{ \"manifestVersion\": 3, \"output\": \"" + packPath +
            "\", \"packKind\": \"content\", "
            "\"options\": { \"cache\": false }, "
            "\"bindings\": ["
            "{ \"id\": \"3da91a64-b947-4560-0000-00000000206d\", \"variable\": \"3da91a64-b947-4560-0000-00000000206e\", "
            "\"scope\": \"voiceEffectParam\", \"target\": " +
            std::to_string(target) +
            ", \"transitionMs\": 0, "
            "\"curve\": { \"points\": [[0.0,-6.0],[1.0,0.0]], "
            "\"handles\": [[0.0,-6.0,1.0,0.0]] } },"
            "{ \"id\": \"3da91a64-b947-4560-0000-00000000206f\", \"variable\": \"3da91a64-b947-4560-0000-000000002070\", "
            "\"scope\": \"busEffectParam\", \"target\": " +
            std::to_string(target) +
            ", \"transitionMs\": 0, "
            "\"curve\": { \"points\": [[0.0,-6.0],[1.0,0.0]], "
            "\"handles\": [[0.0,-6.0,1.0,0.0]] } } ], "
            "\"attachments\": [ { \"binding\": \"3da91a64-b947-4560-0000-00000000206f\", "
            "\"scope\": \"busEffectParam\", \"object\": \"00000000-0000-0000-0000-000000000001\" } ] }");
    ASSERT_EQ(0, RunPackc({manifestPath, "--emit-manifest", emittedPath}));
    ASSERT_EQ(0, RunPackc({emittedPath, "--no-cache",
                           "--emit-manifest", emittedAgainPath}));

    JsonValue emitted;
    ParseJsonFile(emittedPath, &emitted);
    const JsonValue& bindings = Member(emitted, "bindings");
    ASSERT_EQ(2u, bindings.items.size());
    EXPECT_EQ("voiceEffectParam",
              MemberString(bindings.items[0], "scope"));
    EXPECT_EQ("busEffectParam",
              MemberString(bindings.items[1], "scope"));
    const JsonValue& attachments = Member(emitted, "attachments");
    ASSERT_EQ(1u, attachments.items.size());
    EXPECT_EQ("busEffectParam",
              MemberString(attachments.items[0], "scope"));
    EXPECT_EQ(harken::ObjectIdString(harkentest::ObjectId(8303)),
              MemberString(attachments.items[0], "binding"));
    EXPECT_EQ(harken::ObjectIdString(VN_BUS_MASTER),
              MemberString(attachments.items[0], "object"));
    std::string first;
    std::string second;
    ASSERT_TRUE(ReadFileText(emittedPath, &first));
    ASSERT_TRUE(ReadFileText(emittedAgainPath, &second));
    EXPECT_EQ(first, second);
}


TEST(PackcPipeline, BindingTransitionRangeAdmissionPreservesExistingOutput) {
    for (std::uint32_t ms : {60000u, 60001u, UINT32_MAX}) {
        const std::string packPath = TempPath("binding_range_" + std::to_string(ms) + ".pack");
        const std::string manifestPath = TempPath("binding_range_" + std::to_string(ms) + ".json");
        WriteTextFile(packPath, "existing-pack");
        WriteTextFile(manifestPath,
            "{ \"manifestVersion\": 3, \"output\": \"" + packPath +
            "\", \"packKind\": \"content\", \"options\": { \"cache\": false }, "
            "\"bindings\": [{ \"id\": \"3da91a64-b947-4560-0000-0000000020d1\", \"variable\": \"3da91a64-b947-4560-0000-0000000020d2\", \"scope\": \"voice\", "
            "\"target\": 0, \"transitionMs\": " + std::to_string(ms) +
            ", \"curve\": { \"points\": [[0.0,0.0]] } }] }");
        if (ms == 60000) {
            EXPECT_EQ(0, RunPackc({manifestPath}));
        } else {
            EXPECT_NE(0, RunPackc({manifestPath}));
            std::ifstream input(packPath, std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(input)), {});
            EXPECT_EQ("existing-pack", bytes);
        }
    }
}

TEST(PackcPipeline, EffectParamBindingManifestRefusalMatrix) {
    const auto expectRefused = [&](const std::string& name,
                                   const std::string& section) {
        const std::string packPath = TempPath(name + ".pack");
        const std::string manifestPath =
            TempPath(name + "_manifest.json");
        WriteTextFile(
            manifestPath,
            "{ \"manifestVersion\": 3, \"output\": \"" + packPath +
                "\", \"packKind\": \"content\", "
                "\"options\": { \"cache\": false }, " + section + " }");
        std::filesystem::remove(packPath);
        EXPECT_NE(0, RunPackc({manifestPath})) << name;
        EXPECT_FALSE(std::filesystem::exists(packPath)) << name;
    };
    const auto binding = [](std::uint32_t target,
                            std::uint32_t transitionMs) {
        return "\"bindings\": [ { \"id\": \"3da91a64-b947-4560-0000-0000000020d1\", \"variable\": \"3da91a64-b947-4560-0000-0000000020d2\", "
               "\"scope\": \"voiceEffectParam\", \"target\": " +
               std::to_string(target) + ", \"transitionMs\": " +
               std::to_string(transitionMs) +
               ", \"curve\": { \"points\": [[0.0,0.0]] } } ]";
    };
    expectRefused(
        "pipeline_effect_param_unknown_type",
        binding(harken::PackEffectParamLocator(99, 0, 0), 0));
    expectRefused(
        "pipeline_effect_param_bad_index",
        binding(harken::PackEffectParamLocator(VN_EFFECT_GAIN, 0, 1), 0));
    expectRefused(
        "pipeline_effect_param_unbindable",
        binding(harken::PackEffectParamLocator(VN_EFFECT_FEEDBACK_DELAY,
                                              0, 0),
                0));
    expectRefused(
        "pipeline_effect_param_transition",
        binding(harken::PackEffectParamLocator(VN_EFFECT_GAIN, 0, 0), 1));
    expectRefused(
        "pipeline_effect_param_attachment",
        binding(harken::PackEffectParamLocator(VN_EFFECT_GAIN, 0, 0), 0) +
            ", \"attachments\": [ { \"binding\": \"3da91a64-b947-4560-0000-0000000020d1\", "
            "\"scope\": \"voiceEffectParam\", \"object\": \"00000000-0000-0000-0000-000000000001\" } ]");
}

TEST(PackcPipeline, BezierManifestChecksInjectedAuthoredDomains) {
    const auto expectRefused = [&](const std::string& name,
                                   const std::string& packKind,
                                   const std::string& section) {
        const std::string packPath = TempPath(name + ".pack");
        const std::string manifestPath = TempPath(name + "_manifest.json");
        WriteTextFile(
            manifestPath,
            "{ \"manifestVersion\": 3, \"output\": \"" + packPath +
                "\", \"packKind\": \"" + packKind +
                "\", \"options\": { \"cache\": false }, " + section + " }");
        std::filesystem::remove(packPath);
        EXPECT_NE(0, RunPackc({manifestPath})) << name;
        EXPECT_FALSE(std::filesystem::exists(packPath)) << name;
    };
    expectRefused(
        "pipeline_domain_volume",
        "resident",
        R"("attenuations": [ { "id": "3da91a64-b947-4560-0000-000000002009", "rolloff": "linear",
            "minDistance": 1.0, "maxDistance": 101.0,
            "airAbsorptionNearHz": 20000.0,
            "airAbsorptionFarHz": 20000.0,
            "volumeCurve": { "points": [[0.0,0.0],[100.0,-6.0]],
                "handles": [[0.0,1.0,100.0,-6.0]] } } ])");
    expectRefused(
        "pipeline_domain_lowpass",
        "resident",
        R"("attenuations": [ { "id": "3da91a64-b947-4560-0000-00000000200a", "rolloff": "linear",
            "minDistance": 1.0, "maxDistance": 101.0,
            "airAbsorptionNearHz": 20000.0,
            "airAbsorptionFarHz": 20000.0,
            "lowpassCurve": { "points": [[0.0,100.0],[100.0,200.0]],
                "handles": [[0.0,0.0,100.0,200.0]] } } ])");
    expectRefused(
        "pipeline_domain_width",
        "resident",
        R"("attenuations": [ { "id": "3da91a64-b947-4560-0000-00000000200b", "rolloff": "linear",
            "minDistance": 1.0, "maxDistance": 101.0,
            "airAbsorptionNearHz": 20000.0,
            "airAbsorptionFarHz": 20000.0,
            "widthCurve": { "points": [[0.0,0.0],[100.0,1.0]],
                "handles": [[0.0,1.1,100.0,1.0]] } } ])");
    expectRefused(
        "pipeline_domain_binding_geometry",
        "content",
        R"("bindings": [ { "id": "3da91a64-b947-4560-0000-00000000200c", "variable": "3da91a64-b947-4560-0000-00000000200d",
            "scope": "voice", "target": 0,
            "curve": { "points": [[0.0,0.0],[1.0,1.0]],
                "handles": [[-0.1,0.0,1.0,1.0]] } } ])");
}

TEST(PackcPipeline, BezierManifestEmitterIsSymmetric) {
    const std::string packPath = TempPath("pipeline_bezier_emit.pack");
    const std::string manifestPath =
        TempPath("pipeline_bezier_emit_manifest.json");
    const std::string emittedPath =
        TempPath("pipeline_bezier_emit_out.json");
    const std::string emittedAgainPath =
        TempPath("pipeline_bezier_emit_out_again.json");
    WriteTextFile(
        manifestPath,
        "{ \"manifestVersion\": 3, \"output\": \"" + packPath +
            "\", \"packKind\": \"resident\", "
            "\"options\": { \"cache\": false }, "
            "\"attenuations\": [ { \"id\": \"3da91a64-b947-4560-0000-000000001fa5\", "
            "\"rolloff\": \"linear\", \"minDistance\": 1.0, "
            "\"maxDistance\": 101.0, \"airAbsorptionNearHz\": 20000.0, "
            "\"airAbsorptionFarHz\": 20000.0, \"volumeCurve\": { "
            "\"points\": [[0.0,0.0],[100.0,-96.0]], "
            "\"handles\": [[25.0,0.0,75.0,-96.0]] } } ], "
            "\"bindings\": [ { \"id\": \"3da91a64-b947-4560-0000-000000001fa6\", \"variable\": \"3da91a64-b947-4560-0000-000000001fa7\", "
            "\"scope\": \"bus\", \"target\": 0, \"curve\": { "
            "\"points\": [[0.0,0.0],[1.0,1.0]], "
            "\"handles\": [[0.0,0.0,1.0,1.0]] } } ] }");
    ASSERT_EQ(0, RunPackc({manifestPath, "--emit-manifest", emittedPath}));
    ASSERT_EQ(0, RunPackc({emittedPath, "--no-cache",
                           "--emit-manifest", emittedAgainPath}));
    std::string first;
    std::string second;
    ASSERT_TRUE(ReadFileText(emittedPath, &first));
    ASSERT_TRUE(ReadFileText(emittedAgainPath, &second));
    EXPECT_EQ(first, second);
    JsonValue emitted;
    ParseJsonFile(emittedPath, &emitted);
    const JsonValue& attenuation =
        Member(emitted, "attenuations").items.at(0);
    const JsonValue& volumeCurve = Member(attenuation, "volumeCurve");
    EXPECT_EQ(2u, Member(volumeCurve, "points").items.size());
    EXPECT_EQ(1u, Member(volumeCurve, "handles").items.size());
    const JsonValue& binding = Member(emitted, "bindings").items.at(0);
    EXPECT_EQ(1u, Member(Member(binding, "curve"), "handles").items.size());
}

TEST(PackcPipeline, InvalidPreviousOutputInvalidatesCacheWhole) {
    const std::string wavPath = TempPath("pipeline_old_vintage.wav");
    WriteWavF32(wavPath, harkentest::MakeSine(500, 440.0, kRate, 0.5f), 1,
                kRate);
    const std::string packPath = TempPath("pipeline_old_vintage.pack");
    const std::string manifestPath =
        TempPath("pipeline_old_vintage_manifest.json");
    WriteTextFile(
        manifestPath,
        "{ \"manifestVersion\": 3, \"output\": \"" + packPath +
            "\", \"packKind\": \"content\", "
            "\"entries\": [ " + V2EntryJson(wavPath, 8801) + " ] }");
    std::filesystem::remove(packPath);
    std::filesystem::remove(packPath + ".cache.json");
    ASSERT_EQ(0, RunPackc({manifestPath}));
    std::vector<std::uint8_t> cleanBytes;
    ASSERT_TRUE(ReadFileBytes(packPath, &cleanBytes));

    harken::PackV2Header header;
    ASSERT_TRUE(harken::ParsePackV2Header(cleanBytes.data(), &header));
    ASSERT_EQ(harken::kPackFormatVersion2, header.formatVersion);
    ASSERT_EQ(1u, header.entryCount);
    const std::vector<std::uint8_t> expectedBytes = cleanBytes;
    const std::size_t directoryOffset =
        static_cast<std::size_t>(header.directoryOffset);

    ASSERT_EQ(0, RunPackc({manifestPath}));
    std::vector<std::uint8_t> rebuiltBytes;
    ASSERT_TRUE(ReadFileBytes(packPath, &rebuiltBytes));
    EXPECT_EQ(expectedBytes, rebuiltBytes);
    JsonValue report;
    ParseJsonFile(packPath + ".report.json", &report);
    RequireReportSkeleton(report, "built");
    EXPECT_FALSE(HasWarning(Member(report, "warnings"), "cache-ignored"));
    const JsonValue& hitTotals =
        Member(Member(report, "packs").items.at(0), "totals");
    EXPECT_EQ(1u, MemberU64(hitTotals, "cacheHits"));
    EXPECT_EQ(0u, MemberU64(hitTotals, "cacheMisses"));

    struct Corruption {
        std::size_t offset;
        std::uint32_t value;
        bool restampDirectory;
    };
    const Corruption corruptions[] = {
        {4, harken::kPackFormatVersion2 + 1, false},
        {8, harken::kPackV2HeaderBytes + 1, false},
        {directoryOffset + 4, 1, true},
    };
    for (const Corruption& corruption : corruptions) {
        SCOPED_TRACE(corruption.offset);
        std::vector<std::uint8_t> invalidBytes = expectedBytes;
        harken::StoreLE32(invalidBytes.data() + corruption.offset,
                          corruption.value);
        if (corruption.restampDirectory) {
            harken::StoreLE64(
                invalidBytes.data() + 36,
                harken::Fnv1a64(invalidBytes.data() + directoryOffset,
                               harken::kPackV2DirectoryEntryBytes));
        }
        WriteFileBytes(packPath, invalidBytes);
        ASSERT_EQ(0, RunPackc({manifestPath}));
        ASSERT_TRUE(ReadFileBytes(packPath, &rebuiltBytes));
        EXPECT_EQ(expectedBytes, rebuiltBytes);

        JsonValue rebuildReport;
        ParseJsonFile(packPath + ".report.json", &rebuildReport);
        RequireReportSkeleton(rebuildReport, "built");
        EXPECT_TRUE(HasWarning(Member(rebuildReport, "warnings"),
                               "cache-ignored"));
        const JsonValue& totals =
            Member(Member(rebuildReport, "packs").items.at(0), "totals");
        EXPECT_EQ(0u, MemberU64(totals, "cacheHits"));
        EXPECT_EQ(1u, MemberU64(totals, "cacheMisses"));
    }

    harken::PackV2Header rebuiltHeader;
    ASSERT_TRUE(harken::ParsePackV2Header(rebuiltBytes.data(), &rebuiltHeader));
    const harken::PackV2DirectoryEntry entry =
        harken::ParsePackV2DirectoryEntry(
            rebuiltBytes.data() +
            static_cast<std::size_t>(rebuiltHeader.directoryOffset));
    EXPECT_EQ(harken::kPackV2SchemaVintage, entry.vintage);
    EXPECT_EQ(harken::kPackFormatVersion2, rebuiltHeader.formatVersion);
}

TEST(PackcPipeline, IncrementalRebuildOnlyTouchedEntry) {
    const std::string wavA = TempPath("pipeline_incr_a.wav");
    const std::string wavB = TempPath("pipeline_incr_b.wav");
    const std::string wavC = TempPath("pipeline_incr_c.wav");
    WriteWavF32(wavA, harkentest::MakeSine(700, 200.0, kRate, 0.5f), 1,
                kRate);
    WriteWavF32(wavB, harkentest::MakeSine(800, 300.0, kRate, 0.5f), 1,
                kRate);
    WriteWavF32(wavC, harkentest::MakeSine(900, 400.0, kRate, 0.5f), 1,
                kRate);

    const std::string packPath = TempPath("pipeline_incr.pack");
    const std::string manifestPath =
        TempPath("pipeline_incr_manifest.json");
    WriteTextFile(manifestPath,
                  V2ManifestJson(packPath,
                                 {V2EntryJson(wavA, 201),
                                  V2EntryJson(wavB, 202),
                                  V2EntryJson(wavC, 203)}));
    std::filesystem::remove(packPath);
    std::filesystem::remove(packPath + ".cache.json");

    auto readHitFlags = [&](std::uint64_t* hits, std::uint64_t* misses,
                            std::vector<bool>* skipped) {
        JsonValue report;
        ParseJsonFile(packPath + ".report.json", &report);
        const JsonValue& pack = Member(report, "packs").items.at(0);
        const JsonValue& totals = Member(pack, "totals");
        *hits = MemberU64(totals, "cacheHits");
        *misses = MemberU64(totals, "cacheMisses");
        skipped->clear();
        for (const JsonValue& entry :
             Member(pack, "entries").items) {
            skipped->push_back(MemberBool(entry, "skippedByCache"));
        }
    };

    // Clean build: everything decodes.
    ASSERT_EQ(0, RunPackc({manifestPath}));
    std::vector<std::uint8_t> cleanBytes;
    ASSERT_TRUE(ReadFileBytes(packPath, &cleanBytes));
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::vector<bool> skipped;
    readHitFlags(&hits, &misses, &skipped);
    EXPECT_EQ(0u, hits);
    EXPECT_EQ(3u, misses);

    // Untouched rebuild: every entry hits and the bytes do not move.
    ASSERT_EQ(0, RunPackc({manifestPath}));
    std::vector<std::uint8_t> rebuildBytes;
    ASSERT_TRUE(ReadFileBytes(packPath, &rebuildBytes));
    EXPECT_EQ(cleanBytes, rebuildBytes);
    readHitFlags(&hits, &misses, &skipped);
    EXPECT_EQ(3u, hits);
    EXPECT_EQ(0u, misses);
    ASSERT_EQ(3u, skipped.size());
    EXPECT_TRUE(skipped[0]);
    EXPECT_TRUE(skipped[1]);
    EXPECT_TRUE(skipped[2]);

    // Touch exactly one source: only that entry rebuilds...
    WriteWavF32(wavB, harkentest::MakeSine(850, 310.0, kRate, 0.4f), 1,
                kRate);
    ASSERT_EQ(0, RunPackc({manifestPath}));
    std::vector<std::uint8_t> touchedBytes;
    ASSERT_TRUE(ReadFileBytes(packPath, &touchedBytes));
    readHitFlags(&hits, &misses, &skipped);
    EXPECT_EQ(2u, hits);
    EXPECT_EQ(1u, misses);
    ASSERT_EQ(3u, skipped.size());
    EXPECT_TRUE(skipped[0]);
    EXPECT_FALSE(skipped[1]);
    EXPECT_TRUE(skipped[2]);

    // ...and the incremental output is byte-identical to a clean build
    // of the same (new) inputs: same entries, fresh output, no cache.
    const std::string refPackPath =
        TempPath("pipeline_incr_ref.pack");
    const std::string refManifestPath =
        TempPath("pipeline_incr_ref_manifest.json");
    WriteTextFile(refManifestPath,
                  V2ManifestJson(refPackPath,
                                 {V2EntryJson(wavA, 201),
                                  V2EntryJson(wavB, 202),
                                  V2EntryJson(wavC, 203)},
                                 "{ \"cache\": false }"));
    ASSERT_EQ(0, RunPackc({refManifestPath}));
    std::vector<std::uint8_t> referenceBytes;
    ASSERT_TRUE(ReadFileBytes(refPackPath, &referenceBytes));
    EXPECT_EQ(referenceBytes, touchedBytes)
        << "a cache-assisted rebuild must equal a clean build byte for "
           "byte";
}

TEST(PackcPipeline, CacheIdentityIncludesDownmixPolicyAndAlgorithmVersion) {
    const std::vector<float> stereo = {
        0.75f, 0.25f,
        -0.5f, 0.25f,
        1.0f, -1.0f,
    };
    const std::string wavPath = TempPath("pipeline_downmix_cache.wav");
    WriteWavF32(wavPath, stereo, 2, kRate);

    const std::string packPath = TempPath("pipeline_downmix_cache.pack");
    const std::string cachePath = packPath + ".cache.json";
    const std::string manifestPath =
        TempPath("pipeline_downmix_cache_manifest.json");
    std::filesystem::remove(packPath);
    std::filesystem::remove(cachePath);

    auto writeManifest = [&](bool mono) {
        WriteTextFile(
            manifestPath,
            V2ManifestJson(
                packPath,
                {V2EntryJson(wavPath, 1201, false, 0, 0,
                             mono ? "mono" : "")}));
    };
    auto expectCacheCounts = [&](std::uint64_t hits,
                                 std::uint64_t misses) {
        JsonValue report;
        ParseJsonFile(packPath + ".report.json", &report);
        const JsonValue& totals = Member(
            Member(report, "packs").items.at(0), "totals");
        EXPECT_EQ(hits, MemberU64(totals, "cacheHits"));
        EXPECT_EQ(misses, MemberU64(totals, "cacheMisses"));
    };
    auto readOnlyFacts = [&]() {
        std::vector<BuiltEntryFacts> facts;
        std::uint64_t fileBytes = 0;
        std::uint64_t tocChecksum = 0;
        ReadPackFacts(packPath, &facts, &fileBytes, &tocChecksum);
        EXPECT_EQ(1u, facts.size());
        return facts.empty() ? BuiltEntryFacts{} : facts.front();
    };

    writeManifest(false);
    ASSERT_EQ(0, RunPackc({manifestPath}));
    expectCacheCounts(0, 1);
    EXPECT_EQ(2u, readOnlyFacts().channels);

    // Same path, source bytes, and stable id, but a different policy:
    // the stereo blob must not poison the mono variant.
    writeManifest(true);
    ASSERT_EQ(0, RunPackc({manifestPath}));
    expectCacheCounts(0, 1);
    const BuiltEntryFacts monoFacts = readOnlyFacts();
    EXPECT_EQ(1u, monoFacts.channels);
    EXPECT_EQ((std::vector<float>{0.5f, -0.125f, 0.0f}),
              monoFacts.planar);

    // The matching mono identity now hits.
    ASSERT_EQ(0, RunPackc({manifestPath}));
    expectCacheCounts(1, 0);

    // A coherent cache record with another algorithm version must miss
    // rather than serving the previous algorithm's blob.
    std::string cacheText;
    ASSERT_TRUE(ReadFileText(cachePath, &cacheText));
    EXPECT_NE(std::string::npos,
              cacheText.find("\"downmix\": \"mono\""));
    ReplaceOnce(&cacheText,
                "\"downmixAlgorithmVersion\": 1",
                "\"downmixAlgorithmVersion\": 2");
    RestampCacheIntegrity(&cacheText);
    WriteTextFile(cachePath, cacheText);

    ASSERT_EQ(0, RunPackc({manifestPath}));
    expectCacheCounts(0, 1);
    const BuiltEntryFacts rebuiltMono = readOnlyFacts();
    EXPECT_EQ(1u, rebuiltMono.channels);
    EXPECT_EQ(monoFacts.planar, rebuiltMono.planar);
}

// The content-hash clause of the cache-hit predicate, isolated: a source
// rewritten with the SAME byte count but different samples (a same-length
// re-record — realistic authoring) must invalidate its cache entry. Byte
// count, codec, and blob size all still match the record, so only the
// FNV-1a content hash can tell the difference; without it the stale blob
// would splice into the output as a silently wrong pack.
TEST(PackcPipeline, SameSizeContentChangeInvalidatesCacheEntry) {
    const std::string wavPath = TempPath("pipeline_samesize.wav");
    WriteWavF32(wavPath, harkentest::MakeSine(700, 200.0, kRate, 0.5f), 1,
                kRate);

    const std::string packPath = TempPath("pipeline_samesize.pack");
    const std::string manifestPath =
        TempPath("pipeline_samesize_manifest.json");
    WriteTextFile(manifestPath,
                  V2ManifestJson(packPath, {V2EntryJson(wavPath, 601)}));
    std::filesystem::remove(packPath);
    std::filesystem::remove(packPath + ".cache.json");

    ASSERT_EQ(0, RunPackc({manifestPath}));
    std::vector<std::uint8_t> firstBytes;
    ASSERT_TRUE(ReadFileBytes(packPath, &firstBytes));

    // Same frame count and format -> identical wav byte size; different
    // frequency -> different samples.
    WriteWavF32(wavPath, harkentest::MakeSine(700, 320.0, kRate, 0.5f), 1,
                kRate);
    ASSERT_EQ(0, RunPackc({manifestPath}));
    std::vector<std::uint8_t> rebuiltBytes;
    ASSERT_TRUE(ReadFileBytes(packPath, &rebuiltBytes));

    // The entry must have been rebuilt, not served from the cache.
    JsonValue report;
    ParseJsonFile(packPath + ".report.json", &report);
    const JsonValue& pack = Member(report, "packs").items.at(0);
    EXPECT_EQ(0u, MemberU64(Member(pack, "totals"), "cacheHits"));
    EXPECT_FALSE(MemberBool(Member(pack, "entries").items.at(0),
                            "skippedByCache"));

    // And the output equals a clean cache-free build of the new content.
    const std::string refPackPath = TempPath("pipeline_samesize_ref.pack");
    const std::string refManifestPath =
        TempPath("pipeline_samesize_ref_manifest.json");
    WriteTextFile(refManifestPath,
                  V2ManifestJson(refPackPath, {V2EntryJson(wavPath, 601)},
                                 "{ \"cache\": false }"));
    ASSERT_EQ(0, RunPackc({refManifestPath}));
    std::vector<std::uint8_t> referenceBytes;
    ASSERT_TRUE(ReadFileBytes(refPackPath, &referenceBytes));
    EXPECT_EQ(referenceBytes, rebuiltBytes)
        << "a same-size content change must not serve the stale blob";
    EXPECT_NE(firstBytes, rebuiltBytes)
        << "the two contents must produce different packs for this case "
           "to prove anything";
}

TEST(PackcPipeline, CorruptedCacheDegradesToCleanBuild) {
    const std::string wavA = TempPath("pipeline_corrupt_a.wav");
    const std::string wavB = TempPath("pipeline_corrupt_b.wav");
    WriteWavF32(wavA, harkentest::MakeSine(500, 210.0, kRate, 0.5f), 1,
                kRate);
    WriteWavF32(wavB, harkentest::MakeSine(600, 320.0, kRate, 0.5f), 1,
                kRate);

    const std::string packPath = TempPath("pipeline_corrupt.pack");
    const std::string cachePath = packPath + ".cache.json";
    const std::string manifestPath =
        TempPath("pipeline_corrupt_manifest.json");
    WriteTextFile(manifestPath,
                  V2ManifestJson(packPath, {V2EntryJson(wavA, 211),
                                            V2EntryJson(wavB, 212)}));
    std::filesystem::remove(packPath);
    std::filesystem::remove(cachePath);
    ASSERT_EQ(0, RunPackc({manifestPath}));
    std::vector<std::uint8_t> cleanBytes;
    ASSERT_TRUE(ReadFileBytes(packPath, &cleanBytes));
    ASSERT_TRUE(std::filesystem::exists(cachePath));

    const std::string corruptions[] = {
        "this is not json at all {{{",
        "{ \"cacheVersion\": 999, \"sources\": [] }\n",
        "{ \"cacheVersion\": 1, \"sources\": [], \"surprise\": 1 }\n",
    };
    for (const std::string& corruption : corruptions) {
        WriteTextFile(cachePath, corruption);
        ASSERT_EQ(0, RunPackc({manifestPath})) << corruption;
        std::vector<std::uint8_t> rebuiltBytes;
        ASSERT_TRUE(ReadFileBytes(packPath, &rebuiltBytes));
        EXPECT_EQ(cleanBytes, rebuiltBytes) << corruption;

        JsonValue report;
        ParseJsonFile(packPath + ".report.json", &report);
        RequireReportSkeleton(report, "built");
        EXPECT_TRUE(HasWarning(Member(report, "warnings"),
                               "cache-ignored"))
            << corruption;
        const JsonValue& totals =
            Member(Member(report, "packs").items.at(0), "totals");
        EXPECT_EQ(0u, MemberU64(totals, "cacheHits")) << corruption;
        EXPECT_EQ(2u, MemberU64(totals, "cacheMisses")) << corruption;
    }

    // A valid cache whose previous output vanished also degrades clean:
    // there is nothing to lift blobs from, so everything decodes.
    ASSERT_EQ(0, RunPackc({manifestPath})); // repopulate a good cache
    std::filesystem::remove(packPath);
    ASSERT_EQ(0, RunPackc({manifestPath}));
    std::vector<std::uint8_t> recreatedBytes;
    ASSERT_TRUE(ReadFileBytes(packPath, &recreatedBytes));
    EXPECT_EQ(cleanBytes, recreatedBytes);
    JsonValue report;
    ParseJsonFile(packPath + ".report.json", &report);
    const JsonValue& totals =
        Member(Member(report, "packs").items.at(0), "totals");
    EXPECT_EQ(0u, MemberU64(totals, "cacheHits"));
    EXPECT_EQ(2u, MemberU64(totals, "cacheMisses"));
}

TEST(PackcPipeline, CacheCrossWiredRecordDegradesPerEntry) {
    // The nastiest cache defect: a syntactically perfect record that
    // splices one entry's SOURCE facts to another entry's BLOB facts.
    // The source-side checks pass (the source file really is
    // unchanged), so only the id-bound comparison against the previous
    // output's own TOC can catch that the blob the record points at is
    // not this entry's blob. The tampered entry must decode fresh and
    // the output must stay byte-identical to a clean build — never a
    // wrong pack.
    const std::string wavX = TempPath("pipeline_splice_x.wav");
    const std::string wavY = TempPath("pipeline_splice_y.wav");
    WriteWavF32(wavX, harkentest::MakeSine(700, 250.0, kRate, 0.5f), 1,
                kRate);
    WriteWavF32(wavY, harkentest::MakeSine(900, 350.0, kRate, 0.5f), 1,
                kRate);
    const std::string packPath = TempPath("pipeline_splice.pack");
    const std::string cachePath = packPath + ".cache.json";
    const std::string manifestPath =
        TempPath("pipeline_splice_manifest.json");
    WriteTextFile(manifestPath,
                  V2ManifestJson(packPath, {V2EntryJson(wavX, 221),
                                            V2EntryJson(wavY, 222)}));
    std::filesystem::remove(packPath);
    std::filesystem::remove(cachePath);
    ASSERT_EQ(0, RunPackc({manifestPath}));
    std::vector<std::uint8_t> cleanBytes;
    ASSERT_TRUE(ReadFileBytes(packPath, &cleanBytes));
    std::vector<BuiltEntryFacts> facts;
    std::uint64_t fileBytes = 0;
    std::uint64_t tocChecksum = 0;
    ReadPackFacts(packPath, &facts, &fileBytes, &tocChecksum);
    ASSERT_EQ(2u, facts.size());
    ASSERT_EQ(700u, facts[0].frames);
    ASSERT_EQ(900u, facts[1].frames);

    // Splice: X's record keeps X's source facts but claims Y's blob
    // facts (frames, bytes, checksum) — a coherent-looking lie. The
    // file-level integrity is restamped so the WHOLE-file guard passes
    // and the id-bound per-entry guard is what must fire.
    std::string cacheText;
    ASSERT_TRUE(ReadFileText(cachePath, &cacheText));
    const std::size_t recStart =
        cacheText.find("\"path\": \"" + wavX + "\"");
    ASSERT_NE(std::string::npos, recStart);
    const std::size_t recEnd = cacheText.find('}', recStart);
    ASSERT_NE(std::string::npos, recEnd);
    std::string record = cacheText.substr(recStart, recEnd - recStart);
    ReplaceOnce(&record, "\"frameCount\": 700", "\"frameCount\": 900");
    ReplaceOnce(&record, "\"blobBytes\": 2800", "\"blobBytes\": 3600");
    ReplaceOnce(&record,
                "\"blobChecksum\": \"" + HexU64(facts[0].blobChecksum),
                "\"blobChecksum\": \"" + HexU64(facts[1].blobChecksum));
    cacheText = cacheText.substr(0, recStart) + record +
                cacheText.substr(recEnd);
    RestampCacheIntegrity(&cacheText);
    WriteTextFile(cachePath, cacheText);

    ASSERT_EQ(0, RunPackc({manifestPath}));
    std::vector<std::uint8_t> rebuiltBytes;
    ASSERT_TRUE(ReadFileBytes(packPath, &rebuiltBytes));
    EXPECT_EQ(cleanBytes, rebuiltBytes)
        << "a spliced cache record must never assemble another entry's "
           "blob";

    JsonValue report;
    ParseJsonFile(packPath + ".report.json", &report);
    RequireReportSkeleton(report, "built");
    const JsonValue& pack = Member(report, "packs").items.at(0);
    const JsonValue& entries = Member(pack, "entries");
    ASSERT_EQ(2u, entries.items.size());
    // The spliced entry decodes fresh; the untouched record still hits
    // (which also proves the integrity restamp above is faithful to the
    // tool's own scheme).
    EXPECT_FALSE(MemberBool(entries.items[0], "skippedByCache"));
    EXPECT_TRUE(MemberBool(entries.items[1], "skippedByCache"));
    const JsonValue& totals = Member(pack, "totals");
    EXPECT_EQ(1u, MemberU64(totals, "cacheHits"));
    EXPECT_EQ(1u, MemberU64(totals, "cacheMisses"));
}

TEST(PackcPipeline, CacheIntegrityGuardsCorruptedText) {
    // A single flipped digit inside one record — valid JSON, valid
    // schema, no restamp — must discard the cache WHOLE via the
    // file-level integrity check: every entry misses (not just the
    // corrupted one), the degradation is named in a warning, and the
    // output is the clean build's bytes.
    const std::string wavA = TempPath("pipeline_integrity_a.wav");
    const std::string wavB = TempPath("pipeline_integrity_b.wav");
    WriteWavF32(wavA, harkentest::MakeSine(500, 270.0, kRate, 0.5f), 1,
                kRate);
    WriteWavF32(wavB, harkentest::MakeSine(600, 380.0, kRate, 0.5f), 1,
                kRate);
    const std::string packPath = TempPath("pipeline_integrity.pack");
    const std::string cachePath = packPath + ".cache.json";
    const std::string manifestPath =
        TempPath("pipeline_integrity_manifest.json");
    WriteTextFile(manifestPath,
                  V2ManifestJson(packPath, {V2EntryJson(wavA, 231),
                                            V2EntryJson(wavB, 232)}));
    std::filesystem::remove(packPath);
    std::filesystem::remove(cachePath);
    ASSERT_EQ(0, RunPackc({manifestPath}));
    std::vector<std::uint8_t> cleanBytes;
    ASSERT_TRUE(ReadFileBytes(packPath, &cleanBytes));

    std::string cacheText;
    ASSERT_TRUE(ReadFileText(cachePath, &cacheText));
    const std::size_t hashPos = cacheText.find("\"hash\": \"");
    ASSERT_NE(std::string::npos, hashPos);
    const std::size_t digitPos = hashPos + std::strlen("\"hash\": \"");
    cacheText[digitPos] = cacheText[digitPos] == '0' ? '1' : '0';
    WriteTextFile(cachePath, cacheText);

    ASSERT_EQ(0, RunPackc({manifestPath}));
    std::vector<std::uint8_t> rebuiltBytes;
    ASSERT_TRUE(ReadFileBytes(packPath, &rebuiltBytes));
    EXPECT_EQ(cleanBytes, rebuiltBytes);

    JsonValue report;
    ParseJsonFile(packPath + ".report.json", &report);
    RequireReportSkeleton(report, "built");
    EXPECT_TRUE(
        HasWarning(Member(report, "warnings"), "cache-ignored"));
    const JsonValue& totals =
        Member(Member(report, "packs").items.at(0), "totals");
    // Whole-file degrade: even the record the flip did not touch
    // misses.
    EXPECT_EQ(0u, MemberU64(totals, "cacheHits"));
    EXPECT_EQ(2u, MemberU64(totals, "cacheMisses"));
}

TEST(PackcPipeline, MalformedManifestAloneStillWritesReport) {
    // A v2-shaped invocation (single positional) whose manifest file
    // exists but is not JSON is a REFUSED BUILD, not a usage error: the
    // report lands beside the manifest and names the reason.
    const std::string manifestPath =
        TempPath("pipeline_malformed_manifest.json");
    WriteTextFile(manifestPath, "{ \"sounds\": [");
    std::filesystem::remove(manifestPath + ".report.json");
    EXPECT_NE(0, RunPackc({manifestPath}));
    JsonValue report;
    ParseJsonFile(manifestPath + ".report.json", &report);
    RequireReportSkeleton(report, "refused");
    const JsonValue& refusal = Member(report, "refusal");
    EXPECT_FALSE(MemberString(refusal, "message").empty());
    EXPECT_EQ(manifestPath, MemberString(refusal, "manifest"));

    // A manifest path that does not exist at all also refuses with a
    // report beside the named path (the reason is the unreadable file).
    const std::string missingPath =
        TempPath("pipeline_no_such_manifest.json");
    std::filesystem::remove(missingPath);
    std::filesystem::remove(missingPath + ".report.json");
    EXPECT_NE(0, RunPackc({missingPath}));
    JsonValue missingReport;
    ParseJsonFile(missingPath + ".report.json", &missingReport);
    RequireReportSkeleton(missingReport, "refused");
}

TEST(PackcPipeline, ReportFlagsSeededDefects) {
    // Seeded defects: one silent source (peak below the 1e-4 silence
    // floor), one clipped source (peak above unity — float wav carries
    // it exactly), one healthy source, plus an id declared by BOTH
    // manifests of the invocation.
    std::vector<float> silent(400, 0.0f);
    for (std::size_t n = 0; n < silent.size(); ++n) {
        silent[n] = (n % 2 == 0) ? 5e-5f : -5e-5f;
    }
    std::vector<float> clipped =
        harkentest::MakeSine(500, 330.0, kRate, 1.0f);
    for (float& sample : clipped) {
        sample *= 1.5f;
    }
    const std::vector<float> healthy =
        harkentest::MakeSine(600, 440.0, kRate, 0.5f);
    const std::vector<float> other =
        harkentest::MakeSine(700, 550.0, kRate, 0.5f);

    const std::string silentPath = TempPath("pipeline_defect_silent.wav");
    const std::string clippedPath =
        TempPath("pipeline_defect_clipped.wav");
    const std::string healthyPath =
        TempPath("pipeline_defect_healthy.wav");
    const std::string otherPath = TempPath("pipeline_defect_other.wav");
    WriteWavF32(silentPath, silent, 1, kRate);
    WriteWavF32(clippedPath, clipped, 1, kRate);
    WriteWavF32(healthyPath, healthy, 1, kRate);
    WriteWavF32(otherPath, other, 1, kRate);

    const std::string packA = TempPath("pipeline_defect_a.pack");
    const std::string packB = TempPath("pipeline_defect_b.pack");
    const std::string manifestA =
        TempPath("pipeline_defect_a_manifest.json");
    const std::string manifestB =
        TempPath("pipeline_defect_b_manifest.json");
    WriteTextFile(manifestA,
                  V2ManifestJson(packA, {V2EntryJson(silentPath, 301),
                                         V2EntryJson(clippedPath, 302),
                                         V2EntryJson(healthyPath,
                                                     303)}));
    WriteTextFile(manifestB,
                  V2ManifestJson(packB, {V2EntryJson(otherPath, 301)}));

    // Warnings never fail a build.
    ASSERT_EQ(0, RunPackc({manifestA, manifestB, "--report",
                           TempPath("pipeline_defect.report.json")}));
    ASSERT_TRUE(std::filesystem::exists(packA));
    ASSERT_TRUE(std::filesystem::exists(packB));

    JsonValue report;
    ParseJsonFile(TempPath("pipeline_defect.report.json"), &report);
    RequireReportSkeleton(report, "built");
    const JsonValue& packs = Member(report, "packs");
    ASSERT_EQ(2u, packs.items.size());

    const JsonValue& entriesA = Member(packs.items[0], "entries");
    ASSERT_EQ(3u, entriesA.items.size());
    EXPECT_TRUE(MemberBool(entriesA.items[0], "silent"));
    EXPECT_FALSE(MemberBool(entriesA.items[0], "clipped"));
    EXPECT_FALSE(MemberBool(entriesA.items[1], "silent"));
    EXPECT_TRUE(MemberBool(entriesA.items[1], "clipped"));
    EXPECT_GT(MemberNumber(entriesA.items[1], "peakAbs"), 1.0);
    EXPECT_FALSE(MemberBool(entriesA.items[2], "silent"));
    EXPECT_FALSE(MemberBool(entriesA.items[2], "clipped"));

    const JsonValue& warningsA = Member(packs.items[0], "warnings");
    EXPECT_TRUE(HasWarning(warningsA, "silent-entry", harken::ObjectIdString(harkentest::ObjectId(301))));
    EXPECT_TRUE(HasWarning(warningsA, "clipped-source", harken::ObjectIdString(harkentest::ObjectId(302))));
    EXPECT_FALSE(HasWarning(warningsA, "silent-entry", harken::ObjectIdString(harkentest::ObjectId(303))));
    EXPECT_FALSE(HasWarning(warningsA, "clipped-source", harken::ObjectIdString(harkentest::ObjectId(303))));

    // The id shared across the two manifests is a cross-pack warning
    // naming both manifests; per-pack it is perfectly legal.
    const JsonValue& topWarnings = Member(report, "warnings");
    EXPECT_TRUE(HasWarning(topWarnings,
                           "duplicate-id-across-manifests", harken::ObjectIdString(harkentest::ObjectId(301))));
    bool namesBothManifests = false;
    for (const JsonValue& warning : topWarnings.items) {
        const JsonValue* kind = warning.Find("kind");
        if (kind == nullptr ||
            kind->text != "duplicate-id-across-manifests") {
            continue;
        }
        const JsonValue* manifests = warning.Find("manifests");
        ASSERT_NE(nullptr, manifests);
        ASSERT_EQ(2u, manifests->items.size());
        namesBothManifests =
            manifests->items[0].text == manifestA &&
            manifests->items[1].text == manifestB;
    }
    EXPECT_TRUE(namesBothManifests);
}

TEST(PackcPipeline, RefusedBuildStillWritesReport) {
    const std::vector<float> tone =
        harkentest::MakeSine(300, 660.0, kRate, 0.5f);
    const std::string wavPath = TempPath("pipeline_refuse_tone.wav");
    WriteWavF32(wavPath, tone, 1, kRate);
    const std::string packPath = TempPath("pipeline_refuse.pack");

    // Refusal after the manifest parsed: the report defaults beside the
    // output the manifest names.
    {
        const std::string manifestPath =
            TempPath("pipeline_refuse_missing_manifest.json");
        WriteTextFile(
            manifestPath,
            V2ManifestJson(packPath,
                           {V2EntryJson(
                               TempPath("pipeline_refuse_missing.wav"),
                               401)}));
        std::filesystem::remove(packPath + ".report.json");
        EXPECT_NE(0, RunPackc({manifestPath}));
        JsonValue report;
        ParseJsonFile(packPath + ".report.json", &report);
        RequireReportSkeleton(report, "refused");
        const JsonValue& refusal = Member(report, "refusal");
        EXPECT_FALSE(MemberString(refusal, "message").empty());
        EXPECT_EQ(manifestPath, MemberString(refusal, "manifest"));
    }

    // Refusals before an output is known land beside the manifest.
    struct ParseRefusal {
        const char* name;
        std::string manifest;
    };
    const ParseRefusal refusals[] = {
        {"unknown top-level key",
         "{ \"manifestVersion\": 3, \"output\": \"" + packPath +
             "\", \"bogus\": 1, \"entries\": [ { \"path\": \"" +
             wavPath + "\", \"id\": \"3da91a64-b947-4560-0000-000000000001\" } ] }"},
        {"missing id",
         "{ \"manifestVersion\": 3, \"output\": \"" + packPath +
             "\", \"entries\": [ { \"path\": \"" + wavPath +
             "\" } ] }"},
        {"unknown codec",
         "{ \"manifestVersion\": 3, \"output\": \"" + packPath +
             "\", \"entries\": [ { \"path\": \"" + wavPath +
             "\", \"id\": \"3da91a64-b947-4560-0000-000000000001\", \"codec\": \"opus\" } ] }"},
        {"unknown downmix policy",
         "{ \"manifestVersion\": 3, \"output\": \"" + packPath +
             "\", \"entries\": [ { \"path\": \"" + wavPath +
             "\", \"id\": \"3da91a64-b947-4560-0000-000000000001\", \"downmix\": \"surround\" } ] }"},
        {"unsupported version",
         "{ \"manifestVersion\": 99, \"output\": \"" + packPath +
             "\", \"entries\": [ { \"path\": \"" + wavPath +
             "\", \"id\": \"3da91a64-b947-4560-0000-000000000001\" } ] }"},
        {"duplicate declared ids",
         "{ \"manifestVersion\": 3, \"output\": \"" + packPath +
             "\", \"entries\": [ { \"path\": \"" + wavPath +
             "\", \"id\": \"3da91a64-b947-4560-0000-000000000007\" }, { \"path\": \"" + wavPath +
             "\", \"id\": \"3da91a64-b947-4560-0000-000000000007\" } ] }"},
    };
    for (const ParseRefusal& refusal : refusals) {
        const std::string manifestPath =
            TempPath("pipeline_refuse_parse_manifest.json");
        WriteTextFile(manifestPath, refusal.manifest);
        std::filesystem::remove(manifestPath + ".report.json");
        EXPECT_NE(0, RunPackc({manifestPath})) << refusal.name;
        JsonValue report;
        ParseJsonFile(manifestPath + ".report.json", &report);
        RequireReportSkeleton(report, "refused");
        EXPECT_FALSE(
            MemberString(Member(report, "refusal"), "message").empty())
            << refusal.name;
    }
}

TEST(PackcPipeline, RetiredManifestAndOutputPositionalAreRejected) {
    const std::vector<float> tone =
        harkentest::MakeSine(500, 770.0, kRate, 0.5f);
    const std::string wavPath = TempPath("pipeline_v1_tone.wav");
    WriteWavF32(wavPath, tone, 1, kRate);
    const std::string manifestPath =
        TempPath("pipeline_v1_manifest.json");
    const std::string packPath = TempPath("pipeline_v1.pack");
    WriteTextFile(manifestPath,
                  "{ \"sounds\": [ { \"path\": \"" + wavPath +
                      "\", \"id\": \"3da91a64-b947-4560-0000-000000000259\" } ] }\n");
    std::filesystem::remove(packPath);
    EXPECT_NE(0, RunPackc({manifestPath, packPath}));
    EXPECT_FALSE(std::filesystem::exists(packPath));
    JsonValue report;
    ParseJsonFile(manifestPath + ".report.json", &report);
    RequireReportSkeleton(report, "refused");
    EXPECT_NE(0, RunPackc({manifestPath}));
    const std::string v2ManifestPath =
        TempPath("pipeline_v1_shape_v2_manifest.json");
    WriteTextFile(v2ManifestPath,
                  V2ManifestJson(packPath,
                                 {V2EntryJson(wavPath, 602)}));
    EXPECT_NE(0, RunPackc({v2ManifestPath,
                           TempPath("pipeline_v1_other.pack")}));
}

TEST(PackcPipeline, MissingUuidCannotBeRecoveredFromMediaCache) {
    const std::string wavA = TempPath("pipeline_v1c_a.wav");
    const std::string wavB = TempPath("pipeline_v1c_b.wav");
    const std::string wavC = TempPath("pipeline_v1c_c.wav");
    WriteWavF32(wavA, harkentest::MakeSine(400, 260.0, kRate, 0.5f), 1,
                kRate);
    WriteWavF32(wavB, harkentest::MakeSine(500, 370.0, kRate, 0.5f), 1,
                kRate);
    WriteWavF32(wavC, harkentest::MakeSine(600, 480.0, kRate, 0.5f), 1,
                kRate);
    const std::string manifestPath =
        TempPath("pipeline_v1c_manifest.json");
    const std::string packPath = TempPath("pipeline_v1c.pack");
    const std::string cachePath = TempPath("pipeline_v1c.cache.json");
    const auto valid = V2ManifestJson(packPath, {V2EntryJson(wavA, 701),
        V2EntryJson(wavB, 702), V2EntryJson(wavC, 703)});
    WriteTextFile(manifestPath, valid);
    std::filesystem::remove(packPath);
    std::filesystem::remove(cachePath);

    ASSERT_EQ(0, RunPackc({manifestPath, "--cache-path",
                           cachePath}));
    std::vector<std::uint8_t> firstBytes;
    ASSERT_TRUE(ReadFileBytes(packPath, &firstBytes));
    EXPECT_TRUE(std::filesystem::exists(cachePath));

    ASSERT_EQ(0, RunPackc({manifestPath, "--cache-path",
                           cachePath}));
    std::vector<std::uint8_t> secondBytes;
    ASSERT_TRUE(ReadFileBytes(packPath, &secondBytes));
    EXPECT_EQ(firstBytes, secondBytes);

    JsonValue report;
    ParseJsonFile(packPath + ".report.json", &report);
    const JsonValue& pack = Member(report, "packs").items.at(0);
    const JsonValue& entries = Member(pack, "entries");
    ASSERT_EQ(3u, entries.items.size());
    EXPECT_TRUE(MemberBool(entries.items[0], "skippedByCache"));
    EXPECT_TRUE(MemberBool(entries.items[1], "skippedByCache"));
    EXPECT_TRUE(MemberBool(entries.items[2], "skippedByCache"));
    const JsonValue& totals = Member(pack, "totals");
    EXPECT_EQ(3u, MemberU64(totals, "cacheHits"));
    EXPECT_EQ(0u, MemberU64(totals, "cacheMisses"));
    std::string missing = valid;
    ReplaceOnce(&missing, ", \"id\": " + harkentest::JsonObjectId(harkentest::ObjectId(703)), "");
    WriteTextFile(manifestPath, missing);
    EXPECT_NE(0, RunPackc({manifestPath, "--cache-path", cachePath}));
    std::vector<std::uint8_t> afterRefusal;
    ASSERT_TRUE(ReadFileBytes(packPath, &afterRefusal));
    EXPECT_EQ(firstBytes, afterRefusal);
}

TEST(PackcPipeline, ManifestRelativePathsResolveAgainstManifestDir) {
    // The test runner's working directory is a build directory, not
    // .temp — relative entries only resolve if the tool uses the
    // manifest's own directory as the base.
    const std::string wavPath = TempPath("pipeline_rel_src.wav");
    WriteWavF32(wavPath, harkentest::MakeSine(400, 620.0, kRate, 0.5f),
                1, kRate);
    const std::string manifestPath =
        TempPath("pipeline_rel_manifest.json");
    WriteTextFile(manifestPath,
                  V2ManifestJson("pipeline_rel_out.pack",
                                 {V2EntryJson("pipeline_rel_src.wav",
                                              801)}));
    const std::string packPath = TempPath("pipeline_rel_out.pack");
    std::filesystem::remove(packPath);

    ASSERT_EQ(0, RunPackc({manifestPath}));
    std::vector<BuiltEntryFacts> facts;
    std::uint64_t fileBytes = 0;
    std::uint64_t tocChecksum = 0;
    ReadPackFacts(packPath, &facts, &fileBytes, &tocChecksum);
    ASSERT_EQ(1u, facts.size());
    EXPECT_EQ(harkentest::ObjectId(801u), facts[0].id);
    EXPECT_TRUE(
        std::filesystem::exists(packPath + ".report.json"));
}

TEST(PackcPipeline, VoiceRecipeUsesRealKindElevenPackPath) {
    const std::string wavPath = TempPath("pipeline_recipe_src.wav");
    WriteWavF32(wavPath, harkentest::MakeSine(256, 440.0, kRate, 0.5f),
                1, kRate);
    const std::string packPath = TempPath("pipeline_recipe.pack");
    const std::string manifestPath =
        TempPath("pipeline_recipe_manifest.json");
    WriteTextFile(
        manifestPath,
        "{\n"
        "  \"manifestVersion\": 3,\n"
        "  \"output\": \"" + packPath + "\",\n"
        "  \"packKind\": \"content\",\n"
        "  \"entries\": [{\"path\": \"" + wavPath +
            "\", \"id\": \"3da91a64-b947-4560-0000-00000000238d\"}],\n"
        "  \"voiceRecipes\": [{\n"
        "    \"playable\": \"3da91a64-b947-4560-0000-00000000238d\",\n"
        "    \"effects\": [{\"slot\": 0, \"type\": \"gain\", "
        "\"descriptorVintage\": 2, \"bypass\": false, "
        "\"parameters\": [-6.0]}],\n"
        "    \"bindings\": []\n"
        "  }]\n"
        "}\n");
    ASSERT_EQ(0, RunPackc({manifestPath}));
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(ReadFileBytes(packPath, &bytes));
    harken::PackV2Header header;
    ASSERT_TRUE(harken::ParsePackV2Header(bytes.data(), &header));
    ASSERT_EQ(2u, header.entryCount);
    const harken::PackV2DirectoryEntry recipeEntry =
        harken::ParsePackV2DirectoryEntry(
            bytes.data() + harken::kPackV2HeaderBytes +
            harken::kPackV2DirectoryEntryBytes);
    ASSERT_EQ(harken::kPackEntryKindVoiceRecipe, recipeEntry.kind);
    EXPECT_EQ(harken::kPackV2PlaybackRecipeSchemaVintage, recipeEntry.vintage);
    EXPECT_EQ(harken::PackV5VoiceRecipePayloadBytes(1, 0), recipeEntry.size);
    harken::PackVoiceRecipeEntryHeader recipe;
    ASSERT_TRUE(harken::ParsePackV5VoiceRecipeHeader(
        bytes.data() + recipeEntry.offset, recipeEntry.size, &recipe));
    EXPECT_EQ(harkentest::ObjectId(9101u), recipe.playableId);
    harken::PackVoiceRecipeInstall install;
    ASSERT_TRUE(harken::ParsePackV2VoiceRecipeInstall(
        bytes.data() + recipeEntry.offset +
            harken::kPackV2VoiceRecipeHeaderBytes +
            harken::kPackV3VoiceRecipeToneBytes +
            harken::kPackV4VoiceRecipeSpatialBytes +
            harken::kPackV5VoiceRecipePlaybackBytes,
        &install));
    EXPECT_EQ(VN_EFFECT_GAIN, install.effectType);
    EXPECT_FLOAT_EQ(-6.0f, install.params[0]);
}

TEST(PackcPipeline, VoiceRecipeAlwaysWritesPlaybackVintage) {
    auto expectPlaybackVintage = [](const std::string& stem,
                                    const std::string& recipeBody,
                                    std::uint64_t playableId,
                                    bool expectTone,
                                    bool expectPlayback,
                                    std::uint32_t expectedLoopCount,
                                    std::uint64_t expectedAttenuationId = 0,
                                    float expectedDistanceScale = 0.0f) {
        const std::string wavPath = TempPath(stem + "_src.wav");
        WriteWavF32(wavPath, harkentest::MakeSine(256, 440.0, kRate, 0.5f),
                    1, kRate);
        const std::string packPath = TempPath(stem + ".pack");
        const std::string manifestPath =
            TempPath(stem + "_manifest.json");
        WriteTextFile(
            manifestPath,
            "{\n"
            "  \"manifestVersion\": 3,\n"
            "  \"output\": \"" + packPath + "\",\n"
            "  \"packKind\": \"content\",\n"
            "  \"entries\": [{\"path\": \"" + wavPath +
                "\", \"id\": " + harkentest::JsonObjectId(harkentest::ObjectId(playableId)) + "}],\n"
            "  \"voiceRecipes\": [{\n"
            "    \"playable\": " + harkentest::JsonObjectId(harkentest::ObjectId(playableId)) + ",\n" +
            recipeBody +
            "    \"effects\": [{\"slot\": 0, \"type\": \"gain\", "
            "\"descriptorVintage\": 2, \"bypass\": false, "
            "\"parameters\": [-6.0]}],\n"
            "    \"bindings\": []\n"
            "  }]\n"
            "}\n");
        ASSERT_EQ(0, RunPackc({manifestPath}));
        std::vector<std::uint8_t> bytes;
        ASSERT_TRUE(ReadFileBytes(packPath, &bytes));
        harken::PackV2Header header;
        ASSERT_TRUE(harken::ParsePackV2Header(bytes.data(), &header));
        const harken::PackV2DirectoryEntry recipeEntry =
            harken::ParsePackV2DirectoryEntry(
                bytes.data() + harken::kPackV2HeaderBytes +
                harken::kPackV2DirectoryEntryBytes);
        ASSERT_EQ(harken::kPackEntryKindVoiceRecipe, recipeEntry.kind);
        EXPECT_EQ(harken::kPackV2PlaybackRecipeSchemaVintage,
                  recipeEntry.vintage);
        EXPECT_EQ(harken::PackV5VoiceRecipePayloadBytes(1, 0),
                  recipeEntry.size);
        harken::PackVoiceRecipeEntryHeader v5;
        ASSERT_TRUE(harken::ParsePackV5VoiceRecipeHeader(
            bytes.data() + recipeEntry.offset, recipeEntry.size, &v5));
        harken::PackVoiceRecipeEntryHeader rejected{};
        EXPECT_FALSE(harken::ParsePackV2VoiceRecipeHeader(
            bytes.data() + recipeEntry.offset, recipeEntry.size, &rejected));
        EXPECT_FALSE(harken::ParsePackV3VoiceRecipeHeader(
            bytes.data() + recipeEntry.offset, recipeEntry.size, &rejected));
        harken::PackVoiceRecipeTone tone;
        ASSERT_TRUE(harken::ParsePackV3VoiceRecipeTone(
            bytes.data() + recipeEntry.offset +
                harken::kPackV2VoiceRecipeHeaderBytes,
            &tone));
        EXPECT_EQ(expectTone ? 1u : 0u, tone.present);
        harken::PackVoiceRecipeSpatial spatial;
        ASSERT_TRUE(harken::ParsePackV5VoiceRecipeSpatial(
            bytes.data() + recipeEntry.offset +
                harken::kPackV2VoiceRecipeHeaderBytes +
                harken::kPackV3VoiceRecipeToneBytes,
            &spatial));
        EXPECT_EQ(harkentest::ObjectId(expectedAttenuationId), spatial.attenuationId);
        if (expectedAttenuationId == 0u) {
            EXPECT_FLOAT_EQ(0.0f, spatial.distanceScale);
        } else {
            EXPECT_FLOAT_EQ(expectedDistanceScale, spatial.distanceScale);
        }
        harken::PackVoiceRecipePlayback playback;
        ASSERT_TRUE(harken::ParsePackV5VoiceRecipePlayback(
            bytes.data() + recipeEntry.offset +
                harken::kPackV2VoiceRecipeHeaderBytes +
                harken::kPackV3VoiceRecipeToneBytes +
                harken::kPackV4VoiceRecipeSpatialBytes,
            &playback));
        EXPECT_EQ(expectPlayback ? 1u : 0u, playback.present);
        EXPECT_EQ(expectedLoopCount, playback.loopCount);
    };

    expectPlaybackVintage("pipeline_recipe_current_plain", "", 9201, false,
                          false, 0);
    expectPlaybackVintage(
        "pipeline_recipe_current_tone",
        "    \"tone\": {\n"
        "      \"lowpassEnabled\": false, \"lowpassFrequencyHz\": 8000.0, "
        "\"lowpassQ\": 0.707,\n"
        "      \"highpassEnabled\": false, \"highpassFrequencyHz\": 80.0, "
        "\"highpassQ\": 0.707,\n"
        "      \"lowShelfEnabled\": true, \"lowShelfFrequencyHz\": 200.0, "
        "\"lowShelfQ\": 0.707, \"lowShelfGainDb\": 6.0,\n"
        "      \"highShelfEnabled\": false, "
        "\"highShelfFrequencyHz\": 6000.0, "
        "\"highShelfQ\": 0.707, \"highShelfGainDb\": 0.0\n"
        "    },\n",
        9202, true, false, 0);
    expectPlaybackVintage(
        "pipeline_recipe_current_spatial",
        "    \"spatial\": {\"attenuation\": \"3da91a64-b947-4560-0000-000000002455\", \"distanceScale\": 1.25},\n",
        9204, false, false, 0, 9301, 1.25f);
}

TEST(PackcPipeline, VoiceRecipePlaybackWritesCurrentVintage) {
    const std::string wavPath = TempPath("pipeline_recipe_v5_src.wav");
    WriteWavF32(wavPath, harkentest::MakeSine(256, 440.0, kRate, 0.5f),
                1, kRate);
    const std::string packPath = TempPath("pipeline_recipe_v5.pack");
    const std::string manifestPath =
        TempPath("pipeline_recipe_v5_manifest.json");
    WriteTextFile(
        manifestPath,
        "{\n"
        "  \"manifestVersion\": 3,\n"
        "  \"output\": \"" + packPath + "\",\n"
        "  \"packKind\": \"content\",\n"
        "  \"entries\": [{\"path\": \"" + wavPath +
            "\", \"id\": \"3da91a64-b947-4560-0000-0000000023f3\"}],\n"
        "  \"voiceRecipes\": [{\n"
        "    \"playable\": \"3da91a64-b947-4560-0000-0000000023f3\",\n"
        "    \"playback\": {\"loopCount\": 4},\n"
        "    \"effects\": [{\"slot\": 0, \"type\": \"gain\", "
        "\"descriptorVintage\": 2, \"bypass\": false, "
        "\"parameters\": [-6.0]}],\n"
        "    \"bindings\": []\n"
        "  }]\n"
        "}\n");
    ASSERT_EQ(0, RunPackc({manifestPath}));
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(ReadFileBytes(packPath, &bytes));
    harken::PackV2Header header;
    ASSERT_TRUE(harken::ParsePackV2Header(bytes.data(), &header));
    const harken::PackV2DirectoryEntry recipeEntry =
        harken::ParsePackV2DirectoryEntry(
            bytes.data() + harken::kPackV2HeaderBytes +
            harken::kPackV2DirectoryEntryBytes);
    ASSERT_EQ(harken::kPackEntryKindVoiceRecipe, recipeEntry.kind);
    EXPECT_EQ(harken::kPackV2PlaybackRecipeSchemaVintage,
              recipeEntry.vintage);
    EXPECT_EQ(harken::PackV5VoiceRecipePayloadBytes(1, 0), recipeEntry.size);
    harken::PackVoiceRecipeEntryHeader v5;
    EXPECT_TRUE(harken::ParsePackV5VoiceRecipeHeader(
        bytes.data() + recipeEntry.offset, recipeEntry.size, &v5));
    harken::PackVoiceRecipeSpatial spatial;
    EXPECT_TRUE(harken::ParsePackV5VoiceRecipeSpatial(
        bytes.data() + recipeEntry.offset +
            harken::kPackV2VoiceRecipeHeaderBytes +
            harken::kPackV3VoiceRecipeToneBytes,
        &spatial));
    EXPECT_EQ(harkentest::ObjectId(0u), spatial.attenuationId);
    harken::PackVoiceRecipePlayback playback;
    ASSERT_TRUE(harken::ParsePackV5VoiceRecipePlayback(
        bytes.data() + recipeEntry.offset +
            harken::kPackV2VoiceRecipeHeaderBytes +
            harken::kPackV3VoiceRecipeToneBytes +
            harken::kPackV4VoiceRecipeSpatialBytes,
        &playback));
    EXPECT_EQ(1u, playback.present);
    EXPECT_EQ(4u, playback.loopCount);
}

TEST(PackcPipeline, VoiceRecipePlaybackUnknownKeyRefuses) {
    const std::string packPath =
        TempPath("pipeline_recipe_playback_bad.pack");
    const std::string manifestPath =
        TempPath("pipeline_recipe_playback_bad_manifest.json");
    std::filesystem::remove(packPath);
    WriteTextFile(
        manifestPath,
        "{\n"
        "  \"manifestVersion\": 3,\n"
        "  \"output\": \"" + packPath + "\",\n"
        "  \"packKind\": \"content\",\n"
        "  \"voiceRecipes\": [{\"playable\": \"3da91a64-b947-4560-0000-000000000001\", "
        "\"playback\": {\"loopCount\": 2, \"loops\": 2}, "
        "\"effects\": [], \"bindings\": []}]\n"
        "}\n");
    EXPECT_NE(0, RunPackc({manifestPath}));
    EXPECT_FALSE(std::filesystem::exists(packPath));
}

TEST(PackcPipeline, AttenuationAlwaysWritesCurrentVintage) {
    auto expectAttenuationVintage = [](const std::string& stem,
                                       const std::string& extraFields,
                                       float expectedFrequency) {
        const std::string packPath = TempPath(stem + ".pack");
        const std::string manifestPath =
            TempPath(stem + "_manifest.json");
        WriteTextFile(
            manifestPath,
            "{\n"
            "  \"manifestVersion\": 3,\n"
            "  \"output\": \"" + packPath + "\",\n"
            "  \"packKind\": \"resident\",\n"
            "  \"attenuations\": [{\n"
            "    \"id\": \"3da91a64-b947-4560-0000-000000002455\", \"rolloff\": \"linear\",\n"
            "    \"minDistance\": 1.0, \"maxDistance\": 100.0,\n"
            "    \"airAbsorptionNearHz\": 20000.0, "
            "\"airAbsorptionFarHz\": 20000.0" + extraFields + "\n"
            "  }]\n"
            "}\n");
        ASSERT_EQ(0, RunPackc({manifestPath}));
        std::vector<std::uint8_t> bytes;
        ASSERT_TRUE(ReadFileBytes(packPath, &bytes));
        harken::PackV2Header header;
        ASSERT_TRUE(harken::ParsePackV2Header(bytes.data(), &header));
        const harken::PackV2DirectoryEntry attenuationEntry =
            harken::ParsePackV2DirectoryEntry(
                bytes.data() + harken::kPackV2HeaderBytes);
        ASSERT_EQ(harken::kPackEntryKindAttenuation, attenuationEntry.kind);
        EXPECT_EQ(harken::kPackV2AuthoredToneSchemaVintage,
                  attenuationEntry.vintage);
        EXPECT_EQ(harken::kPackV3AttenuationPayloadBytes,
                  attenuationEntry.size);
        VnAttenuationDesc v3{};
        ASSERT_TRUE(harken::ParsePackV3Attenuation(
            bytes.data() + attenuationEntry.offset, attenuationEntry.size,
            &v3));
        EXPECT_FLOAT_EQ(expectedFrequency, v3.proximityFrequency);
        VnAttenuationDesc v2{};
        EXPECT_FALSE(harken::ParsePackV2Attenuation(
            bytes.data() + attenuationEntry.offset, attenuationEntry.size,
            &v2));
    };

    expectAttenuationVintage("pipeline_atten_current_default", "", 250.0f);
    expectAttenuationVintage(
        "pipeline_atten_current_authored",
        ",\n    \"proximityFrequency\": 400.0",
        400.0f);
}

TEST(PackcPipeline, VoiceRecipeMalformedPayloadRefusesAtAuthoring) {
    const std::string packPath = TempPath("pipeline_recipe_bad.pack");
    const std::string manifestPath =
        TempPath("pipeline_recipe_bad_manifest.json");
    std::filesystem::remove(packPath);
    WriteTextFile(
        manifestPath,
        "{\n"
        "  \"manifestVersion\": 3,\n"
        "  \"output\": \"" + packPath + "\",\n"
        "  \"packKind\": \"content\",\n"
        "  \"voiceRecipes\": [{\"playable\": \"3da91a64-b947-4560-0000-000000000001\", \"effects\": ["
        "{\"slot\": 0, \"type\": \"gain\", "
        "\"descriptorVintage\": 2, \"parameters\": [-120.0]}], "
        "\"bindings\": []}]\n"
        "}\n");
    EXPECT_NE(0, RunPackc({manifestPath}));
    EXPECT_FALSE(std::filesystem::exists(packPath));
}

TEST(PackcPipeline, VoiceRecipeUnknownTypeDiagnosticRefuses) {
    const std::string packPath =
        TempPath("pipeline_recipe_unknown_type.pack");
    const std::string manifestPath =
        TempPath("pipeline_recipe_unknown_type_manifest.json");
    std::filesystem::remove(packPath);
    std::filesystem::remove(manifestPath + ".report.json");
    WriteTextFile(
        manifestPath,
        "{\n"
        "  \"manifestVersion\": 3,\n"
        "  \"output\": \"" + packPath + "\",\n"
        "  \"packKind\": \"content\",\n"
        "  \"voiceRecipes\": [{\"playable\": \"3da91a64-b947-4560-0000-000000000001\", \"effects\": ["
        "{\"slot\": 0, \"type\": \"unknownEffect\", "
        "\"descriptorVintage\": 2, \"parameters\": []}], "
        "\"bindings\": []}]\n"
        "}\n");
    EXPECT_NE(0, RunPackc({manifestPath}));
    EXPECT_FALSE(std::filesystem::exists(packPath));

    JsonValue report;
    ParseJsonFile(manifestPath + ".report.json", &report);
    const std::string message =
        MemberString(Member(report, "refusal"), "message");
    EXPECT_NE(std::string::npos, message.find("compressor"));
}

TEST(PackcPipeline, BusEffectAndDynamicRouteUseKindsTwelveAndThirteen) {
    const std::string packPath =
        TempPath("pipeline_bus_effect_route.pack");
    const std::string manifestPath =
        TempPath("pipeline_bus_effect_route_manifest.json");
    WriteTextFile(
        manifestPath,
        "{\n"
        "  \"manifestVersion\": 3,\n"
        "  \"output\": \"" + packPath + "\",\n"
        "  \"packKind\": \"resident\",\n"
        "  \"buses\": [{\"id\": \"3da91a64-b947-4560-0000-000000001005\", \"parent\": \"00000000-0000-0000-0000-000000000001\"}, "
        "{\"id\": \"3da91a64-b947-4560-0000-000000001006\", \"parent\": \"00000000-0000-0000-0000-000000000001\"}],\n"
        "  \"busEffects\": [{\"bus\": \"3da91a64-b947-4560-0000-000000001005\", \"slot\": 2, "
        "\"type\": \"compressor\", \"descriptorVintage\": 2, "
        "\"bypass\": false, \"faderPointGuard\": 8, "
        "\"parameters\": [-18.0, 4.0, 10.0, 120.0, 6.0, 1.5]}],\n"
        "  \"dynamicRoutes\": [{\"targetBus\": \"3da91a64-b947-4560-0000-000000001005\", "
        "\"effectSlot\": 2, \"routeSlot\": 0, \"sourceBus\": \"3da91a64-b947-4560-0000-000000001006\", "
        "\"tap\": \"postEffects\", \"levelDb\": -3.0}]\n"
        "}\n");
    ASSERT_EQ(0, RunPackc({manifestPath}));

    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(ReadFileBytes(packPath, &bytes));
    harken::PackV2Header header;
    ASSERT_TRUE(harken::ParsePackV2Header(bytes.data(), &header));
    ASSERT_EQ(4u, header.entryCount);
    const harken::PackV2DirectoryEntry effectDir =
        harken::ParsePackV2DirectoryEntry(
            bytes.data() + harken::kPackV2HeaderBytes +
            2 * harken::kPackV2DirectoryEntryBytes);
    const harken::PackV2DirectoryEntry routeDir =
        harken::ParsePackV2DirectoryEntry(
            bytes.data() + harken::kPackV2HeaderBytes +
            3 * harken::kPackV2DirectoryEntryBytes);
    EXPECT_EQ(harken::kPackEntryKindBusEffect, effectDir.kind);
    EXPECT_EQ(harken::kPackV2DynamicSchemaVintage, effectDir.vintage);
    EXPECT_EQ(harken::kPackV2BusEffectPayloadBytes, effectDir.size);
    EXPECT_EQ(harken::kPackEntryKindEffectDynamicRoute, routeDir.kind);
    EXPECT_EQ(harken::kPackV2DynamicSchemaVintage, routeDir.vintage);
    EXPECT_EQ(harken::kPackV2EffectDynamicRoutePayloadBytes,
              routeDir.size);

    harken::PackBusEffectEntry effect;
    ASSERT_TRUE(harken::ParsePackV2BusEffect(
        bytes.data() + effectDir.offset, effectDir.size, &effect));
    EXPECT_EQ(harkentest::ObjectId(4101u), effect.busId);
    EXPECT_EQ(2u, effect.slot);
    EXPECT_EQ(VN_EFFECT_COMPRESSOR, effect.effectType);
    EXPECT_EQ(6u, effect.paramCount);
    EXPECT_EQ(24u, effect.paramBytes);
    EXPECT_FLOAT_EQ(1.5f, effect.params[5]);

    harken::PackEffectDynamicRouteEntry route;
    ASSERT_TRUE(harken::ParsePackV2EffectDynamicRoute(
        bytes.data() + routeDir.offset, routeDir.size, &route));
    EXPECT_EQ(harkentest::ObjectId(4101u), route.targetBusId);
    EXPECT_EQ(2u, route.effectSlot);
    EXPECT_EQ(0u, route.routeSlot);
    EXPECT_EQ(harkentest::ObjectId(4102u), route.sourceId);
    EXPECT_FLOAT_EQ(-3.0f, route.levelDb);
}

TEST(PackcPipeline, BusMixFieldsUseFullUuidPayload) {
    const std::string packPath = TempPath("pipeline_bus_mix.pack");
    const std::string manifestPath =
        TempPath("pipeline_bus_mix_manifest.json");
    const std::string emittedPath =
        TempPath("pipeline_bus_mix_emitted.json");
    WriteTextFile(
        manifestPath,
        "{\n"
        "  \"manifestVersion\": 3,\n"
        "  \"output\": \"" + packPath + "\",\n"
        "  \"packKind\": \"resident\",\n"
        "  \"buses\": [{\"id\": \"3da91a64-b947-4560-0000-0000000013ed\", \"parent\": \"00000000-0000-0000-0000-000000000001\", \"volume\": -6.0, "
        "\"outputBusVolume\": -3.5, \"makeUpGain\": 2.0}, "
        "{\"id\": \"3da91a64-b947-4560-0000-0000000013ee\", \"parent\": \"00000000-0000-0000-0000-000000000001\"}]\n"
        "}\n");
    ASSERT_EQ(0, RunPackc({manifestPath, "--emit-manifest", emittedPath}));

    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(ReadFileBytes(packPath, &bytes));
    harken::PackV2Header header;
    ASSERT_TRUE(harken::ParsePackV2Header(bytes.data(), &header));
    ASSERT_EQ(2u, header.entryCount);

    const harken::PackV2DirectoryEntry mixed =
        harken::ParsePackV2DirectoryEntry(bytes.data() +
                                         harken::kPackV2HeaderBytes);
    const harken::PackV2DirectoryEntry defaults =
        harken::ParsePackV2DirectoryEntry(
            bytes.data() + harken::kPackV2HeaderBytes +
            harken::kPackV2DirectoryEntryBytes);
    EXPECT_EQ(harken::kPackEntryKindBus, mixed.kind);
    EXPECT_EQ(harken::kPackV2BusMixSchemaVintage, mixed.vintage);
    EXPECT_EQ(harken::kPackV6BusPayloadBytes, mixed.size);
    EXPECT_EQ(harken::kPackEntryKindBus, defaults.kind);
    EXPECT_EQ(harken::kPackV2BusMixSchemaVintage, defaults.vintage);
    EXPECT_EQ(harken::kPackV6BusPayloadBytes, defaults.size);

    harken::PackBusEntry mixedBus;
    ASSERT_TRUE(harken::ParsePackV6Bus(bytes.data() + mixed.offset, mixed.size,
                                      &mixedBus));
    EXPECT_EQ(harkentest::ObjectId(5101u), mixedBus.busId);
    EXPECT_EQ(VN_BUS_MASTER, mixedBus.parentBusId);
    EXPECT_FLOAT_EQ(-6.0f, mixedBus.volume);
    EXPECT_FLOAT_EQ(-3.5f, mixedBus.outputBusVolume);
    EXPECT_FLOAT_EQ(2.0f, mixedBus.makeUpGain);

    harken::PackBusEntry defaultBus;
    ASSERT_TRUE(harken::ParsePackV6Bus(
        bytes.data() + defaults.offset, defaults.size, &defaultBus));
    EXPECT_EQ(harkentest::ObjectId(5102u), defaultBus.busId);
    EXPECT_FLOAT_EQ(0.0f, defaultBus.volume);
    EXPECT_FLOAT_EQ(0.0f, defaultBus.outputBusVolume);
    EXPECT_FLOAT_EQ(0.0f, defaultBus.makeUpGain);

    JsonValue emitted;
    ParseJsonFile(emittedPath, &emitted);
    const JsonValue& buses = Member(emitted, "buses");
    ASSERT_EQ(2u, buses.items.size());
    EXPECT_DOUBLE_EQ(-6.0, MemberNumber(buses.items[0], "volume"));
    EXPECT_DOUBLE_EQ(-3.5, MemberNumber(buses.items[0], "outputBusVolume"));
    EXPECT_DOUBLE_EQ(2.0, MemberNumber(buses.items[0], "makeUpGain"));
    EXPECT_DOUBLE_EQ(0.0, MemberNumber(buses.items[1], "volume"));
    EXPECT_DOUBLE_EQ(0.0, MemberNumber(buses.items[1], "outputBusVolume"));
    EXPECT_DOUBLE_EQ(0.0, MemberNumber(buses.items[1], "makeUpGain"));
}

TEST(PackcPipeline, NonFiniteBusMixSumRefusesWithoutWritingPack) {
    const std::string packPath =
        TempPath("pipeline_bus_mix_overflow.pack");
    const std::string manifestPath =
        TempPath("pipeline_bus_mix_overflow_manifest.json");
    std::filesystem::remove(packPath);
    WriteTextFile(
        manifestPath,
        "{\n"
        "  \"manifestVersion\": 3,\n"
        "  \"output\": \"" + packPath + "\",\n"
        "  \"packKind\": \"resident\",\n"
        "  \"buses\": [{\"id\": \"3da91a64-b947-4560-0000-000000001451\", \"parent\": \"00000000-0000-0000-0000-000000000001\", \"volume\": 3e38, "
        "\"outputBusVolume\": 3e38, \"makeUpGain\": 3e38}]\n"
        "}\n");
    EXPECT_NE(0, RunPackc({manifestPath}));
    EXPECT_FALSE(std::filesystem::exists(packPath));
}

} // namespace
