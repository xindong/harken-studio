// The host demo's --seconds parsing contract, tested through the pure
// parser the tool itself uses: whole-string conversion, finiteness, and
// the (0, 3600] range — no partial parses, no infinities reaching the
// duration-to-frame conversions.
#include "DemoOptions.h"
#include "PackCompanion.h"
#include "SchedulingMeasureOptions.h"
#include "Common/PackFoundationFixture.h"
#include "harken/core/PackCompanionNames.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <chrono>
#include <string>
#include <vector>

#ifndef HARKEN_TEMP_DIR
#error "HARKEN_TEMP_DIR must point at the repository's .temp directory"
#endif

namespace {

std::string CompanionJson(const std::string& fields) {
    return "{\"schemaVersion\":" + std::to_string(HARKEN_PACK_SCHEMA_VERSION) +
        "," + fields + "}";
}

TEST(HostOptions, AcceptsFiniteSecondsInRange) {
    double seconds = 0.0;
    EXPECT_TRUE(harkenhost::ParseSeconds("6", &seconds));
    EXPECT_DOUBLE_EQ(6.0, seconds);
    EXPECT_TRUE(harkenhost::ParseSeconds("0.5", &seconds));
    EXPECT_DOUBLE_EQ(0.5, seconds);
    EXPECT_TRUE(harkenhost::ParseSeconds("3600", &seconds));
    EXPECT_DOUBLE_EQ(3600.0, seconds);
}

TEST(HostOptions, RefusesMalformedInput) {
    double seconds = -1.0;
    EXPECT_FALSE(harkenhost::ParseSeconds(nullptr, &seconds));
    EXPECT_FALSE(harkenhost::ParseSeconds("", &seconds));
    EXPECT_FALSE(harkenhost::ParseSeconds("abc", &seconds));
    EXPECT_FALSE(harkenhost::ParseSeconds("6x", &seconds)); // trailing junk
    EXPECT_FALSE(harkenhost::ParseSeconds("6 7", &seconds));
    // A refused parse leaves the output untouched.
    EXPECT_DOUBLE_EQ(-1.0, seconds);
}

TEST(HostOptions, RefusesNonFiniteAndOutOfRange) {
    double seconds = -1.0;
    EXPECT_FALSE(harkenhost::ParseSeconds("inf", &seconds));
    EXPECT_FALSE(harkenhost::ParseSeconds("nan", &seconds));
    EXPECT_FALSE(harkenhost::ParseSeconds("1e999", &seconds)); // overflows
    EXPECT_FALSE(harkenhost::ParseSeconds("0", &seconds));
    EXPECT_FALSE(harkenhost::ParseSeconds("-3", &seconds));
    EXPECT_FALSE(harkenhost::ParseSeconds("3600.1", &seconds)); // over cap
    EXPECT_DOUBLE_EQ(-1.0, seconds);
}

TEST(HostOptions, ParsesSchedulingWorkerMatrix) {
    std::uint32_t workers = 99;
    EXPECT_TRUE(harkenhost::ParseSchedulingWorkerCount("0", &workers));
    EXPECT_EQ(0u, workers);
    EXPECT_TRUE(harkenhost::ParseSchedulingWorkerCount("1", &workers));
    EXPECT_EQ(1u, workers);
    EXPECT_TRUE(harkenhost::ParseSchedulingWorkerCount("2", &workers));
    EXPECT_EQ(2u, workers);
    EXPECT_TRUE(harkenhost::ParseSchedulingWorkerCount("4", &workers));
    EXPECT_EQ(4u, workers);
    EXPECT_TRUE(harkenhost::ParseSchedulingWorkerCount("8", &workers));
    EXPECT_EQ(8u, workers);
    EXPECT_FALSE(harkenhost::ParseSchedulingWorkerCount("3", &workers));
    EXPECT_FALSE(harkenhost::ParseSchedulingWorkerCount("9", &workers));
    EXPECT_FALSE(harkenhost::ParseSchedulingWorkerCount(" 4", &workers));
    EXPECT_EQ(8u, workers);
}

TEST(HostOptions, ParsesSchedulingLoadAndCompetition) {
    harkenhost::SchedulingLoad load = harkenhost::SchedulingLoad::Heavy;
    EXPECT_TRUE(harkenhost::ParseSchedulingLoad("light", &load));
    EXPECT_EQ(harkenhost::SchedulingLoad::Light, load);
    EXPECT_TRUE(harkenhost::ParseSchedulingLoad("medium", &load));
    EXPECT_EQ(harkenhost::SchedulingLoad::Medium, load);
    EXPECT_TRUE(harkenhost::ParseSchedulingLoad("heavy", &load));
    EXPECT_EQ(harkenhost::SchedulingLoad::Heavy, load);
    EXPECT_FALSE(harkenhost::ParseSchedulingLoad("LIGHT", &load));

    std::uint32_t duty = 99;
    EXPECT_TRUE(harkenhost::ParseSchedulingCpuDuty("0", &duty));
    EXPECT_EQ(0u, duty);
    EXPECT_TRUE(harkenhost::ParseSchedulingCpuDuty("50", &duty));
    EXPECT_EQ(50u, duty);
    EXPECT_TRUE(harkenhost::ParseSchedulingCpuDuty("90", &duty));
    EXPECT_EQ(90u, duty);
    EXPECT_FALSE(harkenhost::ParseSchedulingCpuDuty("10", &duty));
    EXPECT_EQ(90u, duty);

    std::uint32_t threads = 0;
    EXPECT_TRUE(harkenhost::ParseSchedulingCpuThreads("256", &threads));
    EXPECT_EQ(256u, threads);
    EXPECT_FALSE(harkenhost::ParseSchedulingCpuThreads("257", &threads));
    EXPECT_EQ(256u, threads);
}

TEST(HostOptions, CompanionNamesPreserveGuidAndStrictJsonContract) {
    harkenhost::PackCompanion input;
    const std::string action = CompanionJson(R"("name":"Play_Head","guid":"11111111-2222-3333-4444-555555555555","display":{"gain":1})");
    ASSERT_EQ(harkenhost::ParsePackCompanion(action, "Play_Head", "", {}, &input), VN_OK);
    EXPECT_EQ(harken::ObjectIdString(input.names.rootActionId), "11111111-2222-3333-4444-555555555555");
    EXPECT_EQ(harkenhost::ParsePackCompanion(action, "WrongName", "", {}, &input), VN_BAD_PACK);
    EXPECT_EQ(harkenhost::ParsePackCompanion(CompanionJson(R"("variables":[],"variables":[])"), "", "", {}, &input), VN_BAD_PACK);
    EXPECT_EQ(harkenhost::ParsePackCompanion(CompanionJson(R"("variables":[])") + " trailing", "", "", {}, &input), VN_BAD_PACK);
    EXPECT_EQ(harkenhost::ParsePackCompanion(CompanionJson(R"("variables":[{"name":"Choice","guid":"11111111-2222-3333-4444-555555555555","labels":[{"guid":"21111111-2222-3333-4444-555555555555","name":"\u4e2d\u6587"}]}])"), "", "", {}, &input), VN_OK);
    ASSERT_EQ(input.labels.size(), 1u);
    EXPECT_STREQ(input.labels.front().utf8Name, "\xe4\xb8\xad\xe6\x96\x87");
    EXPECT_FALSE(harkenhost::PackCompanionName("../Play_Head"));
    EXPECT_FALSE(harkenhost::PackCompanionName("Play-Head"));
    EXPECT_FALSE(harkenhost::PackCompanionLanguage("../English"));
}

TEST(HostOptions, CompanionVersionUsesExactJsonIntegerSemantics) {
    harkenhost::PackCompanion input;
    const std::string current = std::to_string(HARKEN_PACK_SCHEMA_VERSION);
    const std::string next = std::to_string(std::uint64_t{HARKEN_PACK_SCHEMA_VERSION} + 1);
    for (bool init : {true, false}) {
        const std::string fields = init ? R"("variables":[])"
            : R"("name":"Play_Head","guid":"11111111-2222-3333-4444-555555555555")";
        const std::string name = init ? "" : "Play_Head";
        for (const std::string& version : {current, current + ".0", current + "e0", current + "0e-1",
                "0.0" + current + "E+" + std::to_string(current.size() + 1), current + "00e-2", current + "e-0000"}) {
            SCOPED_TRACE(version);
            EXPECT_EQ(harkenhost::ParsePackCompanion(std::string("{\"schemaVersion\":") + version + "," + fields +
                R"(,"metadata":false,"waveforms":42})", name, "", {}, &input), VN_OK);
        }
        for (const std::string& version : std::vector<std::string>{"", next, "\"" + current + "\"", "true", "null", "[]", "{}",
                current + ".5", "-" + current, "0", current + ".0000000000000001",
                std::to_string(HARKEN_PACK_SCHEMA_VERSION - 1) + ".9999999999999999",
                current + "e999999999999999999999", current + "e-999999999999999999999"}) {
            SCOPED_TRACE(version);
            const std::string field = version.empty() ? "" : "\"schemaVersion\":" + version + ",";
            const char* stage = nullptr;
            EXPECT_EQ(harkenhost::ParsePackCompanion("{" + field + fields +
                ",\"schema_version\":" + current + ",\"metadata\":{\"schemaVersion\":" + current + "}}",
                name, "", {}, &input, &stage), VN_BAD_PACK);
            EXPECT_STREQ(stage, "schemaVersion");
        }
    }
    EXPECT_EQ(harkenhost::ParsePackCompanion(CompanionJson(R"("variables":[{"name":"Choice","guid":"11111111-2222-3333-4444-555555555555","labels":[]}])"),
        "", "", {}, &input), VN_OK);
}

TEST(HostOptions, CompanionCaptureUsesPairedFilesAndRefusesPartialLanguageOrPublication) {
    using namespace harkentest::packfoundation;
    const auto directory = std::filesystem::path(HARKEN_TEMP_DIR) /
        ("host-companion-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path root; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(root, ec); } } cleanup{directory};
    const auto source = directory / "source";
    const auto root = directory / "capture";
    ASSERT_TRUE(std::filesystem::create_directories(source / "English"));
    ASSERT_TRUE(std::filesystem::create_directories(root / "English"));
    const auto project = harkentest::ObjectId(1);
    const auto action = harkentest::ObjectId(kActionBase);
    const auto bus = DefaultRuntimeBusFixtureV1();
    const auto build = [&](bool init, const std::string& language) {
        const auto id = init ? kInitPackStableId :
            harken::CompanionActionPackId(kInitPackStableId, action, language);
        std::uint64_t bodyBytes = 0;
        auto partition = PartitionPayload(id, 1, init ? 0 : 1, 0x3f000000u,
            &bodyBytes, 64, project, bus.triple);
        ImageBuilder image(init ? harken::kPackKindInit : harken::kPackKindCanonicalContent);
        image.AddEntry(harken::kPackEntryKindPackRequirementBlock, id,
            RequirementPayload(id, 1, 1, init ? 0 : 2, bodyBytes, bus.triple));
        image.AddEntry(harken::kPackEntryKindCanonicalDefinitionPartition, id, std::move(partition));
        if (init) {
            image.AddEntry(harken::kPackEntryKindBusDefinitionSnapshot, 1, bus.snapshot);
        } else {
            image.AddEntry(harken::kPackEntryKindPackDependencyEdge, VnObjectId{},
                DependencyPayload(kInitPackStableId, 1, bus.triple), harken::kPackCanonicalRequirementVintage);
        }
        return image.Build();
    };
    const auto writeJson = [](const std::filesystem::path& path, const std::string& text) {
        std::ofstream output(path, std::ios::binary);
        output << text;
        return output.good();
    };
    const auto actionJson = CompanionJson("\"name\":\"Play_Head\",\"guid\":\"" + harken::ObjectIdString(action) + "\"");
    ASSERT_TRUE(WriteFile(source / "Init.bytes", build(true, "")));
    ASSERT_TRUE(writeJson(source / "Init.json", CompanionJson(R"("variables":[])")));
    ASSERT_TRUE(WriteFile(source / "Play_Head.bytes", build(false, "")));
    ASSERT_TRUE(writeJson(source / "Play_Head.json", actionJson));
    ASSERT_TRUE(WriteFile(source / "English/Play_Head.bytes", build(false, "English")));
    ASSERT_TRUE(writeJson(source / "English/Play_Head.json", actionJson));
    harkenhost::PackCompanion init, fallback, localized;
    ASSERT_EQ(harkenhost::CapturePackCompanion(source, {}, {}, nullptr, &init), VN_OK);
    ASSERT_EQ(harkenhost::CapturePackCompanion(source, "Play_Head", "French", &init, &fallback), VN_OK);
    EXPECT_TRUE(fallback.language.empty());
    ASSERT_EQ(harkenhost::CapturePackCompanion(source, "Play_Head", "English", &init, &localized), VN_OK);
    EXPECT_EQ(localized.language, "English");
    EXPECT_EQ(localized.facts.rootActionId, fallback.facts.rootActionId);
    { std::ofstream json(root / "HarkenPackManifest.json"); json << R"({"schema_version":3,"packs":[]})"; }
    { std::ofstream json(root / "HarkenPackIndex.json"); json << R"({"schema_version":3,"platforms":[]})"; }
    harkenhost::PackCompanion missingInit;
    EXPECT_EQ(harkenhost::CapturePackCompanion(root, {}, {}, nullptr, &missingInit), VN_CONTENT_NOT_LOADED);
    for (const auto* file : {"Play_Head.bytes", "Play_Head.json"})
        std::filesystem::copy_file(source / file, root / file);
    std::filesystem::copy_file(source / "English/Play_Head.bytes", root / "English/Play_Head.bytes");
    harkenhost::PackCompanion refused;
    harkenhost::PackCompanionDiagnostic diagnostic;
    EXPECT_EQ(harkenhost::CapturePackCompanion(root, "Play_Head", "English", &init, &refused, &diagnostic), VN_BAD_PACK);
    EXPECT_STREQ(diagnostic.stage, "file capture");
    ASSERT_TRUE(writeJson(root / "English/Play_Head.json", "{\"schemaVersion\":" +
        std::to_string(std::uint64_t{HARKEN_PACK_SCHEMA_VERSION} + 1) + "}"));
    EXPECT_EQ(harkenhost::CapturePackCompanion(root, "Play_Head", "English", &init, &refused, &diagnostic), VN_BAD_PACK);
    EXPECT_STREQ(diagnostic.kind, "Action");
    EXPECT_EQ(diagnostic.jsonPath, root / "English/Play_Head.json");
    EXPECT_STREQ(diagnostic.stage, "schemaVersion");
    std::filesystem::copy_file(source / "English/Play_Head.json", root / "English/Play_Head.json",
        std::filesystem::copy_options::overwrite_existing);
    EXPECT_EQ(harkenhost::CapturePackCompanion(root, "Play_Head", "English", &init, &refused), VN_OK);
    { std::ofstream marker(root / ".harken-pack-publishing"); marker << "publishing"; }
    EXPECT_EQ(harkenhost::CapturePackCompanion(root, "Play_Head", "", &init, &refused, &diagnostic), VN_PACK_STALE_REVISION);
    EXPECT_STREQ(diagnostic.stage, "file capture");
    std::filesystem::remove(root / ".harken-pack-publishing");
    std::filesystem::rename(root / "Play_Head.bytes", root / "play_head.bytes");
    EXPECT_EQ(harkenhost::CapturePackCompanion(root, "Play_Head", "", &init, &refused), VN_BAD_PACK);
}

} // namespace
