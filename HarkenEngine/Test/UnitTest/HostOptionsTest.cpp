// The host demo's --seconds parsing contract, tested through the pure
// parser the tool itself uses: whole-string conversion, finiteness, and
// the (0, 3600] range — no partial parses, no infinities reaching the
// duration-to-frame conversions.
#include "DemoOptions.h"
#include "PackCompanion.h"
#include "SchedulingMeasureOptions.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <chrono>

#ifndef HARKEN_TEMP_DIR
#error "HARKEN_TEMP_DIR must point at the repository's .temp directory"
#endif

namespace {

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
    const std::string action = R"({"schemaVersion":3,"name":"Play_Head","guid":"11111111-2222-3333-4444-555555555555","display":{"gain":1}})";
    ASSERT_EQ(harkenhost::ParsePackCompanion(action, "Play_Head", "", {}, &input), VN_OK);
    EXPECT_EQ(harken::ObjectIdString(input.names.rootActionId), "11111111-2222-3333-4444-555555555555");
    EXPECT_EQ(harkenhost::ParsePackCompanion(action, "WrongName", "", {}, &input), VN_BAD_PACK);
    EXPECT_EQ(harkenhost::ParsePackCompanion(R"({"schemaVersion":3,"variables":[],"variables":[]})", "", "", {}, &input), VN_BAD_PACK);
    EXPECT_EQ(harkenhost::ParsePackCompanion(R"({"schemaVersion":3,"variables":[]} trailing)", "", "", {}, &input), VN_BAD_PACK);
    EXPECT_EQ(harkenhost::ParsePackCompanion(R"({"schemaVersion":3,"variables":[{"name":"Choice","guid":"11111111-2222-3333-4444-555555555555","labels":[{"guid":"21111111-2222-3333-4444-555555555555","name":"\u4e2d\u6587"}]}]})", "", "", {}, &input), VN_OK);
    ASSERT_EQ(input.labels.size(), 1u);
    EXPECT_STREQ(input.labels.front().utf8Name, "\xe4\xb8\xad\xe6\x96\x87");
    EXPECT_FALSE(harkenhost::PackCompanionName("../Play_Head"));
    EXPECT_FALSE(harkenhost::PackCompanionName("Play-Head"));
    EXPECT_FALSE(harkenhost::PackCompanionLanguage("../English"));
}

TEST(HostOptions, CompanionVersionUsesExactJsonIntegerSemantics) {
    harkenhost::PackCompanion input;
    for (bool init : {true, false}) {
        const std::string fields = init ? R"("variables":[])"
            : R"("name":"Play_Head","guid":"11111111-2222-3333-4444-555555555555")";
        const std::string name = init ? "" : "Play_Head";
        for (const char* version : {"3", "3.0", "3e0", "30e-1", "0.03E+2", "300e-2", "3e-0000"}) {
            SCOPED_TRACE(version);
            EXPECT_EQ(harkenhost::ParsePackCompanion(std::string("{\"schemaVersion\":") + version + "," + fields +
                R"(,"metadata":false,"waveforms":42})", name, "", {}, &input), VN_OK);
        }
        for (const char* version : {"", "999", "\"3\"", "true", "null", "[]", "{}", "3.5", "-3", "0",
                "3.0000000000000001", "2.9999999999999999", "3e999999999999999999999", "3e-999999999999999999999"}) {
            SCOPED_TRACE(version);
            const std::string field = *version ? std::string("\"schemaVersion\":") + version + "," : "";
            const char* stage = nullptr;
            EXPECT_EQ(harkenhost::ParsePackCompanion("{" + field + fields +
                R"(,"schema_version":3,"metadata":{"schemaVersion":3}})", name, "", {}, &input, &stage), VN_BAD_PACK);
            EXPECT_STREQ(stage, "schemaVersion");
        }
    }
    EXPECT_EQ(harkenhost::ParsePackCompanion(R"({"schemaVersion":3,"variables":[{"name":"Choice","guid":"11111111-2222-3333-4444-555555555555","labels":[]}]})",
        "", "", {}, &input), VN_OK);
}

TEST(HostOptions, CompanionCaptureUsesPairedFilesAndRefusesPartialLanguageOrPublication) {
    const auto source = std::filesystem::path(HARKEN_TEMP_DIR).parent_path() /
        "HarkenSDK/Test/UnityFixture/Data/CompanionPacks";
    harkenhost::PackCompanion init, fallback, localized;
    ASSERT_EQ(harkenhost::CapturePackCompanion(source, {}, {}, nullptr, &init), VN_OK);
    ASSERT_EQ(harkenhost::CapturePackCompanion(source, "Play_Head", "French", &init, &fallback), VN_OK);
    EXPECT_TRUE(fallback.language.empty());
    ASSERT_EQ(harkenhost::CapturePackCompanion(source, "Play_Head", "English", &init, &localized), VN_OK);
    EXPECT_EQ(localized.language, "English");
    EXPECT_EQ(localized.facts.rootActionId, fallback.facts.rootActionId);
    const auto root = std::filesystem::path(HARKEN_TEMP_DIR) /
        ("host-companion-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(root / "English"));
    struct Cleanup { std::filesystem::path root; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(root, ec); } } cleanup{root};
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
    { std::ofstream json(root / "English/Play_Head.json"); json << R"({"schemaVersion":999})"; }
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
