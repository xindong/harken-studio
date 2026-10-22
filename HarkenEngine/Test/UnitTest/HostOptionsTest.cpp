// The host demo's --seconds parsing contract, tested through the pure
// parser the tool itself uses: whole-string conversion, finiteness, and
// the (0, 3600] range — no partial parses, no infinities reaching the
// duration-to-frame conversions.
#include "DemoOptions.h"
#include "PackPlayPath.h"
#include "PackSidecar.h"
#include "PackCompanion.h"
#include "SchedulingMeasureOptions.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <chrono>

#ifndef HARKEN_TEMP_DIR
#error "HARKEN_TEMP_DIR must point at the repository's .temp directory"
#endif

namespace {

std::string CurrentPackSidecar(const std::string& members) {
    return "{\"schema_version\":" +
        std::to_string(HARKEN_PACK_SCHEMA_VERSION) + "," + members + "}";
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

TEST(HostOptions, PackPlayLocatorRejectsEmbeddedBackslashAndDotDot) {
    EXPECT_TRUE(harkenhost::PackPlayLocatorLegal("Windows"));
    EXPECT_TRUE(harkenhost::PackPlayLocatorLegal("Windows/Nested"));
    EXPECT_FALSE(harkenhost::PackPlayLocatorLegal("subdir\\..\\outside"));
    EXPECT_FALSE(harkenhost::PackPlayLocatorLegal("Windows\\pack"));
    EXPECT_FALSE(harkenhost::PackPlayLocatorLegal("../outside"));
    EXPECT_FALSE(harkenhost::PackPlayLocatorLegal("Windows/../outside"));
    EXPECT_FALSE(harkenhost::PackPlayLocatorLegal("/Windows"));
}

TEST(HostOptions, PackPlayJoinContainedStaysUnderRequestedRoot) {
    const std::filesystem::path root =
        std::filesystem::path(HARKEN_TEMP_DIR) / "packplay-join-root";
    std::filesystem::create_directories(root / "Windows");
    std::filesystem::path resolved;
    ASSERT_TRUE(harkenhost::PackPlayJoinContained(root, "Windows", &resolved));
    EXPECT_EQ(std::filesystem::weakly_canonical(root / "Windows"), resolved);
    EXPECT_FALSE(
        harkenhost::PackPlayJoinContained(root, "subdir\\..\\outside", &resolved));
    EXPECT_FALSE(harkenhost::PackPlayJoinContained(root, "../outside", &resolved));
}


TEST(HostOptions, PackSidecarAcceptsOnlyCompiledCurrentVersion) {
    const std::string indexMembers = R"("platforms":[])";
    const std::string manifestMembers = R"("platformId":"11111111-2222-3333-4444-555555555555","platformName":"Windows","basePlatform":"Windows","initPackStableId":"11","packs":[{"name":"Init","packStableId":"11","path":"Init.pack","platformId":"11111111-2222-3333-4444-555555555555","contentHash":"0","dependencies":[]}])";
    const auto current = std::to_string(HARKEN_PACK_SCHEMA_VERSION);
    const std::string rejectedVersions[] = {
        "0", "-1", std::to_string(HARKEN_PACK_SCHEMA_VERSION + 1),
        "\"" + current + "\"", "true", "null", current + ".5"
    };
    for (bool manifest : {false, true}) {
        SCOPED_TRACE(manifest ? "manifest" : "index");
        const auto& members = manifest ? manifestMembers : indexMembers;
        std::string error;
        const auto parse = [&](const std::string& json) {
            error.clear();
            if (manifest) {
                harkenhost::PackSidecarPlatform platform;
                std::vector<harkenhost::PackSidecarEntry> packs;
                return harkenhost::ParsePackSidecarManifest(json, &platform, &packs, &error);
            }
            std::vector<harkenhost::PackSidecarPlatform> platforms;
            return harkenhost::ParsePackSidecarIndex(json, &platforms, &error);
        };
        EXPECT_TRUE(parse(CurrentPackSidecar(members))) << error;
        EXPECT_TRUE(parse("{\"schema_version\":" + current + ".0," + members + "}")) << error;
        for (int version = 1; version < HARKEN_PACK_SCHEMA_VERSION; ++version)
            EXPECT_FALSE(parse("{\"schema_version\":" + std::to_string(version) + "," + members + "}"));
        for (const auto& version : rejectedVersions) {
            SCOPED_TRACE(version);
            EXPECT_FALSE(parse("{\"schema_version\":" + version + "," + members + "}"));
        }
        EXPECT_FALSE(parse("{" + members + "}"));
        EXPECT_FALSE(parse(CurrentPackSidecar("\"schema_version\":" + current + "," + members)));
    }
    {
        harkenhost::PackSidecarPlatform platform;
        std::vector<harkenhost::PackSidecarEntry> packs;
        std::string error;
        const std::string catalogMembers = manifestMembers +
            R"(,"gameplayCatalog":{"schema":1},"variableCatalog":{"variables":[]})";
        EXPECT_TRUE(harkenhost::ParsePackSidecarManifest(
            CurrentPackSidecar(catalogMembers), &platform, &packs, &error))
            << error;
        ASSERT_EQ(packs.size(), 1u);
        EXPECT_EQ(packs.front().packStableId, 11u);
    }
}

TEST(HostOptions, PackSidecarUsesInstanceSelectionAndRejectsAmbiguousBase) {
    const std::string index = CurrentPackSidecar(R"("platforms":[{"id":"11111111-2222-3333-4444-555555555555","name":"Android","basePlatform":"Android","path":"Android"},{"id":"22222222-2222-3333-4444-555555555555","name":"Mobile_High","basePlatform":"Android","path":"Mobile_High"}])");
    std::vector<harkenhost::PackSidecarPlatform> platforms;
    harkenhost::PackSidecarPlatform selected;
    std::string error;
    ASSERT_TRUE(harkenhost::ParsePackSidecarIndex(index, &platforms, &error)) << error;
    ASSERT_EQ(platforms.size(), 2u);
    EXPECT_FALSE(harkenhost::SelectPackSidecarPlatform(platforms, {}, {}, "Android", &selected, &error));
    EXPECT_NE(error.find("Mobile_High"), std::string::npos);
    EXPECT_TRUE(harkenhost::SelectPackSidecarPlatform(platforms, platforms[1].id, {}, "Windows", &selected, &error));
    EXPECT_EQ(selected.id, platforms[1].id);
    EXPECT_TRUE(harkenhost::SelectPackSidecarPlatform(platforms, {}, "Android", "Windows", &selected, &error));
    EXPECT_EQ(selected.id, platforms[0].id);
    EXPECT_FALSE(harkenhost::SelectPackSidecarPlatform(platforms, {}, {}, "Windows", &selected, &error));
    EXPECT_FALSE(harkenhost::SelectPackSidecarPlatform(platforms, "Android", {}, "Android", &selected, &error));
    EXPECT_FALSE(harkenhost::SelectPackSidecarPlatform(platforms, platforms[1].id, "Android", "Android", &selected, &error));
}

TEST(HostOptions, PackSidecarRejectsOldMixedDuplicateAndUnsafeIndexEntries) {
    std::vector<harkenhost::PackSidecarPlatform> platforms;
    std::string error;
    EXPECT_FALSE(harkenhost::ParsePackSidecarIndex(CurrentPackSidecar(R"("platforms":[{"name":"Windows","path":"Windows"}])"), &platforms, &error));
    EXPECT_FALSE(harkenhost::ParsePackSidecarIndex(CurrentPackSidecar(R"("platforms":[],"platform":"Windows")"), &platforms, &error));
    EXPECT_FALSE(harkenhost::ParsePackSidecarIndex(CurrentPackSidecar("\"schema_version\":" + std::to_string(HARKEN_PACK_SCHEMA_VERSION) + R"(,"platforms":[])"), &platforms, &error));
    EXPECT_TRUE(harkenhost::ParsePackSidecarIndex(CurrentPackSidecar(R"("platforms":[])"), &platforms, &error));
    const auto row = R"({"id":"11111111-2222-3333-4444-555555555555","name":"Mobile","basePlatform":"Android","path":"Mobile"})";
    EXPECT_FALSE(harkenhost::ParsePackSidecarIndex(CurrentPackSidecar(std::string("\"platforms\":[") + row + "," + row + "]"), &platforms, &error));
    EXPECT_FALSE(harkenhost::ParsePackSidecarIndex(CurrentPackSidecar(R"("platforms":[{"id":"11111111-2222-3333-4444-555555555555","name":"Mobile","basePlatform":"Android","path":"../Mobile"}])"), &platforms, &error));
    EXPECT_TRUE(harkenhost::ParsePackSidecarIndex(CurrentPackSidecar(R"("platforms":[{"id":"11111111-2222-3333-4444-555555555555","name":"Mobile_\u97f3_[HQ]","basePlatform":"Android","path":"Mobile_\u97f3_[HQ]"}])"), &platforms, &error)) << error;
    ASSERT_EQ(platforms.size(), 1u);
    EXPECT_EQ(platforms.front().name, "Mobile_\xE9\x9F\xB3_[HQ]");
}

TEST(HostOptions, PackSidecarIdentifiesInitByStableIdInsteadOfNamePathOrOrder) {
    const std::string manifest = CurrentPackSidecar(R"("platformId":"11111111-2222-3333-4444-555555555555","platformName":"Windows","basePlatform":"Windows","initPackStableId":"22","packs":[{"name":"Init","packStableId":"11","path":"Init.pack","platformId":"11111111-2222-3333-4444-555555555555","contentHash":"0","dependencies":["22"]},{"name":"Bootstrap","packStableId":"22","path":"startup/bootstrap.pack","platformId":"11111111-2222-3333-4444-555555555555","contentHash":"0","dependencies":[]}])");
    harkenhost::PackSidecarPlatform platform;
    std::vector<harkenhost::PackSidecarEntry> packs;
    std::string error;
    ASSERT_TRUE(harkenhost::ParsePackSidecarManifest(manifest, &platform, &packs, &error)) << error;
    ASSERT_EQ(packs.size(), 2u);
    EXPECT_FALSE(packs[0].isInit);
    EXPECT_TRUE(packs[1].isInit);
    EXPECT_EQ(packs[1].path, "startup/bootstrap.pack");
    auto missingInit = manifest;
    const auto offset = missingInit.find("\"initPackStableId\":\"22\"");
    missingInit.replace(offset, std::string("\"initPackStableId\":\"22\"").size(), "\"initPackStableId\":\"33\"");
    EXPECT_FALSE(harkenhost::ParsePackSidecarManifest(missingInit, &platform, &packs, &error));
}

TEST(HostOptions, PackSidecarFilesystemRejectsWrongManifestFilesAndRetiredDirectory) {
    const auto root = std::filesystem::path(HARKEN_TEMP_DIR) / "p6-sidecar-reader";
    std::filesystem::create_directories(root / "Android");
    const auto write = [](const std::filesystem::path& path, const std::string& text) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
    };
    const auto index = CurrentPackSidecar(R"("platforms":[{"id":"11111111-2222-3333-4444-555555555555","name":"Android","basePlatform":"Android","path":"Android"}])");
    const std::string manifest = CurrentPackSidecar(R"("platformId":"11111111-2222-3333-4444-555555555555","platformName":"Android","basePlatform":"Android","initPackStableId":"11","packs":[{"name":"Init","packStableId":"11","path":"Init.pack","platformId":"11111111-2222-3333-4444-555555555555","contentHash":"12638187200555641996","dependencies":[]}])");
    write(root / "HarkenPackIndex.json", index);
    write(root / "Android/HarkenPackManifest.json", manifest);
    write(root / "Android/Init.pack", "a");
    std::filesystem::path resolved;
    std::string error;
    ASSERT_TRUE(harkenhost::ResolvePackSidecarRoot(root, {}, {}, "Android", &resolved, &error)) << error;
    EXPECT_EQ(resolved, std::filesystem::weakly_canonical(root / "Android"));
    write(root / "HarkenPackIndex.json", CurrentPackSidecar(R"("platforms":[])"));
    EXPECT_FALSE(harkenhost::ResolvePackSidecarRoot(root, {}, {}, "Android", &resolved, &error));
    write(root / "HarkenPackIndex.json", "{");
    EXPECT_FALSE(harkenhost::ResolvePackSidecarRoot(root, {}, {}, "Android", &resolved, &error));
    std::filesystem::remove(root / "HarkenPackIndex.json");
    EXPECT_FALSE(harkenhost::ResolvePackSidecarRoot(root, {}, {}, "Android", &resolved, &error));
    write(root / "HarkenPackIndex.json", index);
    EXPECT_TRUE(harkenhost::ResolvePackSidecarRoot(root, {}, {}, "Android", &resolved, &error));
    write(root / "Android/Init.pack", "b");
    EXPECT_FALSE(harkenhost::ResolvePackSidecarRoot(root, {}, {}, "Android", &resolved, &error));
    write(root / "Android/Init.pack", "a");
    std::string wrong = manifest;
    wrong.replace(wrong.find("\"platformName\":\"Android\""), std::string("\"platformName\":\"Android\"").size(), "\"platformName\":\"Other\"");
    write(root / "Android/HarkenPackManifest.json", wrong);
    EXPECT_FALSE(harkenhost::ResolvePackSidecarRoot(root, {}, {}, "Android", &resolved, &error));
    write(root / "Android/HarkenPackManifest.json", manifest);
    write(root / "HarkenPackIndex.json", CurrentPackSidecar(R"("platforms":[])"));
    EXPECT_FALSE(harkenhost::ResolvePackSidecarRoot(root, {}, "Android", "Android", &resolved, &error));
}

TEST(HostOptions, PackSidecarSelectsLanguagePublicationBeforeLoadingFiles) {
    const auto root = std::filesystem::path(HARKEN_TEMP_DIR) / "pr494-language-sidecar";
    std::filesystem::create_directories(root / "English");
    std::filesystem::create_directories(root / "ja");
    const auto write = [](const std::filesystem::path& path, const std::string& text) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
    };
    const auto row = [](const std::string& id, const std::string& path, const std::string& language) {
        return "{\"name\":\"" + id + "\",\"packStableId\":\"" + id + "\",\"path\":\"" + path
            + "\",\"language\":\"" + language
            + R"(","platformId":"11111111-2222-3333-4444-555555555555","contentHash":"12638187200555641996","dependencies":[])" + "}";
    };
    const auto manifest = [](const std::string& language, const std::string& rows) {
        return CurrentPackSidecar(
            R"("platformId":"11111111-2222-3333-4444-555555555555","platformName":"Windows","basePlatform":"Windows","initPackStableId":"11","gameplayCatalog":{"schema":1},"language":")"
            + language + "\",\"packs\":[" + rows + "]");
    };
    const auto english = row("22", "English/Content.pack", "English");
    const auto japanese = row("33", "ja/Content.pack", "ja");
    write(root / "HarkenPackManifest.json", manifest("English", row("11", "Init.pack", "") + "," + english + "," + japanese));
    for (const std::string language : {"English", "ja"}) {
        write(root / language / "HarkenPackManifest.json", manifest(language,
            row("11", language + "/Init.pack", language) + "," + (language == "English" ? english : japanese)));
        write(root / language / "Init.pack", "a");
        write(root / language / "Content.pack", "a");
    }
    harkenhost::PackSidecarPlatform platform;
    std::vector<harkenhost::PackSidecarEntry> packs;
    std::string error;
    ASSERT_TRUE(harkenhost::LoadPackSidecarManifest(root, &platform, &packs, &error)) << error;
    ASSERT_EQ(packs.size(), 2u);
    EXPECT_EQ(packs.front().path, "English/Init.pack");
    ASSERT_TRUE(harkenhost::LoadPackSidecarManifest(root, &platform, &packs, &error, "ja")) << error;
    EXPECT_EQ(packs.front().path, "ja/Init.pack");
    EXPECT_EQ(packs.back().packStableId, 33u);
    EXPECT_FALSE(harkenhost::LoadPackSidecarManifest(root, &platform, &packs, &error, "../ja"));
    EXPECT_FALSE(harkenhost::LoadPackSidecarManifest(root, &platform, &packs, &error, "French"));
    std::filesystem::create_directories(root / "French");
    write(root / "French/Init.pack", "a");
    write(root / "French/HarkenPackManifest.json", manifest("French", row("11", "French/Init.pack", "French")));
    EXPECT_FALSE(harkenhost::LoadPackSidecarManifest(root, &platform, &packs, &error, "French"));
    write(root / "ja/HarkenPackManifest.json", manifest("English", row("11", "English/Init.pack", "English") + "," + english));
    EXPECT_FALSE(harkenhost::LoadPackSidecarManifest(root, &platform, &packs, &error, "ja"));
    EXPECT_TRUE(harkenhost::LoadPackSidecarManifest(root, &platform, &packs, &error));
}

TEST(HostOptions, CompanionNamesPreserveGuidAndStrictJsonContract) {
    harkenhost::PackCompanion input;
    const std::string action = R"({"name":"Play_Head","guid":"11111111-2222-3333-4444-555555555555","display":{"gain":1}})";
    ASSERT_EQ(harkenhost::ParsePackCompanion(action, "Play_Head", "", {}, &input), VN_OK);
    EXPECT_EQ(harken::ObjectIdString(input.names.rootActionId), "11111111-2222-3333-4444-555555555555");
    EXPECT_EQ(harkenhost::ParsePackCompanion(action, "WrongName", "", {}, &input), VN_BAD_PACK);
    EXPECT_EQ(harkenhost::ParsePackCompanion(R"({"variables":[],"variables":[]})", "", "", {}, &input), VN_BAD_PACK);
    EXPECT_EQ(harkenhost::ParsePackCompanion(R"({"variables":[]} trailing)", "", "", {}, &input), VN_BAD_PACK);
    EXPECT_EQ(harkenhost::ParsePackCompanion(R"({"variables":[{"name":"Choice","guid":"11111111-2222-3333-4444-555555555555","labels":[{"guid":"21111111-2222-3333-4444-555555555555","name":"\u4e2d\u6587"}]}]})", "", "", {}, &input), VN_OK);
    ASSERT_EQ(input.labels.size(), 1u);
    EXPECT_STREQ(input.labels.front().utf8Name, "\xe4\xb8\xad\xe6\x96\x87");
    EXPECT_FALSE(harkenhost::PackCompanionName("../Play_Head"));
    EXPECT_FALSE(harkenhost::PackCompanionName("Play-Head"));
    EXPECT_FALSE(harkenhost::PackCompanionLanguage("../English"));
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
    for (const auto* file : {"Play_Head.bytes", "Play_Head.json"})
        std::filesystem::copy_file(source / file, root / file);
    std::filesystem::copy_file(source / "English/Play_Head.bytes", root / "English/Play_Head.bytes");
    harkenhost::PackCompanion refused;
    EXPECT_EQ(harkenhost::CapturePackCompanion(root, "Play_Head", "English", &init, &refused), VN_BAD_PACK);
    { std::ofstream marker(root / ".harken-pack-publishing"); marker << "publishing"; }
    EXPECT_EQ(harkenhost::CapturePackCompanion(root, "Play_Head", "", &init, &refused), VN_PACK_STALE_REVISION);
    std::filesystem::remove(root / ".harken-pack-publishing");
    std::filesystem::rename(root / "Play_Head.bytes", root / "play_head.bytes");
    EXPECT_EQ(harkenhost::CapturePackCompanion(root, "Play_Head", "", &init, &refused), VN_BAD_PACK);
}

} // namespace
