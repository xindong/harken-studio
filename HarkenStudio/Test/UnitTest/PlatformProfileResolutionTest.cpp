#include "AssetPipeline/AssetTypes.h"
#include "AssetPipeline/PlatformProfile.h"
#include "ProjectPackExporter.h"
#include "ProjectTypes.h"

#include <gtest/gtest.h>

#include <QJsonObject>
#include <QStringList>
#include <QTemporaryDir>
#include "Support/ProjectPlatformTestUtils.h"

using VNX::Platform;
using VNX::PlatformProfileManager;

TEST(PlatformProfileResolution, FiveBuiltinProfilesRegistered)
{
    auto& mgr = PlatformProfileManager::instance();

    const Platform supported[] = {Platform::Windows, Platform::macOS, Platform::Linux,
                                  Platform::Android, Platform::iOS};
    for (Platform p : supported) {
        const VNX::PlatformProfile* profile = mgr.getProfile(p);
        ASSERT_NE(profile, nullptr) << VNX::platformToString(p).toStdString();
        EXPECT_EQ(profile->platform, p);
    }
    EXPECT_EQ(mgr.getProfile(Platform::Universal), nullptr);

    for (Platform desktop : {Platform::macOS, Platform::Linux}) {
        const VNX::PlatformProfile* profile = mgr.getProfile(desktop);
        ASSERT_NE(profile, nullptr);
        EXPECT_EQ(profile->sizePolicies.front().config.format,
                  VNX::ConversionSettings::Codec::PCM_Float);
        EXPECT_FALSE(profile->performResample);
    }
}

TEST(PlatformProfileResolution, InstanceProfileAndOverridePrecedence)
{
    auto platforms = HarkenTest::platforms({"Android"});
    VNX::AssetInfo asset;
    asset.sourceID = 123;
    auto resolved = PlatformProfileManager::effectiveSettingsForAsset(asset, platforms.front());
    EXPECT_FALSE(resolved.performResample);
    VNX::ProjectPlatformProfile profile;
    profile.performResample = true;
    profile.sizePolicies.front().config.targetSampleRate = 44100;
    platforms.front().profile = profile;
    resolved = PlatformProfileManager::effectiveSettingsForAsset(asset, platforms.front());
    EXPECT_EQ(resolved.targetSampleRate, 44100u);
    asset.settings.performResample = true;
    asset.settings.targetSampleRate = 32000;
    resolved = PlatformProfileManager::effectiveSettingsForAsset(asset, platforms.front());
    EXPECT_EQ(resolved.targetSampleRate, 32000u);
    VNX::ConversionSettings overrideSettings;
    resolved = PlatformProfileManager::effectiveSettingsForAsset(asset, platforms.front(), &overrideSettings);
    EXPECT_FALSE(resolved.performResample);
}

TEST(PlatformProfileResolution, AllBasePlatformsUseTheSameFullRangeSourcePreset)
{
    const auto expected = VNX::ProjectPlatformProfile::fromBasePlatform(VNX::BasePlatform::Windows).toJson();
    auto platforms = HarkenTest::platforms({"Windows", "macOS", "Linux", "Android", "iOS"});
    for (const auto& platform : platforms) {
        EXPECT_EQ(platform.profile.toJson(), expected);
        ASSERT_EQ(platform.profile.sizePolicies.size(), 1u);
        EXPECT_FALSE(platform.profile.performResample);
        EXPECT_EQ(platform.profile.sizePolicies.front().config.targetSampleRate, 48000u);
        EXPECT_FALSE(platform.profile.sizePolicies.front().maxBytes);
        for (const uint64_t size : {uint64_t(0), uint64_t(524288), UINT64_MAX}) {
            VNX::AssetInfo asset;
            asset.metadata.fileSize = size;
            EXPECT_FALSE(PlatformProfileManager::effectiveSettingsForAsset(asset, platform).performResample);
        }
    }
}

TEST(PlatformProfileResolution, InclusiveLimitsSelectOnlyTheirOwnRange)
{
    auto platform = HarkenTest::platforms({"Windows"}).front();
    VNX::ConversionParameters lower;
    lower.targetSampleRate = 22050;
    auto upper = lower;
    upper.targetSampleRate = 44100;
    platform.profile.performResample = true;
    platform.profile.sizePolicies = {{64, lower}, {12 * 1024 * 1024, upper}, {std::nullopt, {}}};
    ASSERT_TRUE(VNX::ProjectPlatformSemantics::validatePlatforms({platform}));
    for (const uint64_t size : {uint64_t(0), uint64_t(64), uint64_t(65), uint64_t(12 * 1024 * 1024),
                                uint64_t(12 * 1024 * 1024 + 1), UINT64_MAX}) {
        VNX::AssetInfo asset;
        asset.metadata.fileSize = size;
        const auto resolved = PlatformProfileManager::effectiveSettingsForAsset(asset, platform);
        EXPECT_TRUE(resolved.performResample) << size;
        EXPECT_EQ(resolved.targetSampleRate, size <= 64 ? 22050u : size <= 12 * 1024 * 1024 ? 44100u : 48000u) << size;
    }
    const auto rows = platform.profile.toJson().value("sizePolicies");
    platform.profile.performResample = false;
    VNX::AssetInfo asset;
    asset.metadata.fileSize = 65;
    EXPECT_FALSE(PlatformProfileManager::effectiveSettingsForAsset(asset, platform).performResample);
    asset.settings.performResample = true;
    asset.settings.targetSampleRate = 32000;
    VNX::ConversionSettings file;
    file.performResample = true;
    file.targetSampleRate = 24000;
    EXPECT_FALSE(PlatformProfileManager::effectiveSettingsForAsset(asset, platform).performResample);
    EXPECT_FALSE(PlatformProfileManager::effectiveSettingsForAsset(asset, platform, &file).performResample);
    platform.profile.performResample = true;
    EXPECT_EQ(PlatformProfileManager::effectiveSettingsForAsset(asset, platform).targetSampleRate, 32000u);
    EXPECT_EQ(PlatformProfileManager::effectiveSettingsForAsset(asset, platform, &file).targetSampleRate, 24000u);
    EXPECT_EQ(platform.profile.toJson().value("sizePolicies"), rows);
}

TEST(PlatformProfileResolution, NormalizeDropsRetiredPlatforms)
{
    const QStringList input = {
        QStringLiteral("PS5"),
        QStringLiteral("Xbox"),
        QStringLiteral("Switch"),
        QStringLiteral("PlayStation 5"),
        QStringLiteral("Xbox Series X/S"),
        QStringLiteral("Nintendo Switch"),
        QStringLiteral("Web / HTML5"),
        QStringLiteral("Windows / Mac / Linux"),
        QStringLiteral("Android / iOS"),
    };
    const QStringList expected = {
        QStringLiteral("Windows"), QStringLiteral("macOS"), QStringLiteral("Linux"),
        QStringLiteral("Android"), QStringLiteral("iOS")};
    EXPECT_EQ(VNX::normalizeTargetPlatforms(input), expected);
}

TEST(PlatformProfileResolution, KnownTargetPlatformNamesRoundTrip)
{
    const QStringList names = VNX::knownTargetPlatformNames();
    EXPECT_EQ(names.size(), 5);
    EXPECT_FALSE(names.contains(QStringLiteral("Universal")));
    for (const QString& name : names) {
        const Platform p = VNX::platformFromString(name);
        EXPECT_NE(p, Platform::Universal) << name.toStdString();
        EXPECT_EQ(VNX::platformToString(p), name);
        EXPECT_NE(PlatformProfileManager::instance().getProfile(p), nullptr)
            << name.toStdString();
    }
}

TEST(PlatformProfileResolution, AssetInfoFromJsonDropsUnknownCachePlatforms)
{
    VNX::AssetInfo info;
    info.sourceID = 42;
    VNX::CacheEntry entry;
    entry.valid = true;
    entry.cachePath = HarkenTest::platformIdString("Windows") + QStringLiteral("/42_test.wav");
    entry.fingerprint = QStringLiteral("test");
    info.platformCaches.insert(HarkenTest::PlatformIds::Windows, entry);

    QJsonObject obj = info.toJson();
    QJsonObject caches = obj["platformCaches"].toObject();
    caches[QStringLiteral("PS5")] = entry.toJson();
    obj["platformCaches"] = caches;

    const VNX::AssetInfo restored = VNX::AssetInfo::fromJson(obj);
    EXPECT_EQ(restored.platformCaches.size(), 1);
    EXPECT_TRUE(restored.platformCaches.contains(HarkenTest::PlatformIds::Windows));
    EXPECT_FALSE(restored.platformCaches.contains(VNX::ProjectPlatformId{}));
}

TEST(PlatformProfileResolution, ResolveExportPlatformsContract)
{
    VNX::ProjectData project;
    project.platforms = HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("Android"),
                               QStringLiteral("iOS")});
    project.packExportSelectedPlatformIds = {HarkenTest::platformIdString("Android"), HarkenTest::platformIdString("iOS")};

    QString error;
    EXPECT_EQ(VNX::ProjectPackExporter::resolveExportPlatforms(project, QString(), &error),
              project.packExportSelectedPlatformIds);
    EXPECT_TRUE(error.isEmpty());

    EXPECT_EQ(VNX::ProjectPackExporter::resolveExportPlatforms(
                  project, HarkenTest::platformIdString("Windows"), &error),
              QStringList{HarkenTest::platformIdString("Windows")});
    EXPECT_TRUE(error.isEmpty());

    EXPECT_TRUE(VNX::ProjectPackExporter::resolveExportPlatforms(
                    project, QStringLiteral("PS5"), &error)
                    .isEmpty());
    EXPECT_TRUE(error.contains(QStringLiteral("PS5")));

    error.clear();
    project.packExportSelectedPlatformIds.clear();
    EXPECT_TRUE(
        VNX::ProjectPackExporter::resolveExportPlatforms(project, QString(), &error).isEmpty());
    EXPECT_FALSE(error.isEmpty());
}

TEST(PlatformProfileResolution, ResolveIntersectsSelectedWithTargetPlatforms)
{
    VNX::ProjectData project;
    project.platforms = HarkenTest::platforms({QStringLiteral("macOS"), QStringLiteral("Windows")});
    project.packExportSelectedPlatformIds = {
        HarkenTest::platformIdString("Windows"), HarkenTest::platformIdString("Android"), HarkenTest::platformIdString("macOS")};

    QString error;
    EXPECT_EQ(VNX::ProjectPackExporter::resolveExportPlatforms(project, QString(), &error),
              (QStringList{HarkenTest::platformIdString("Windows"), HarkenTest::platformIdString("macOS")}));
    EXPECT_TRUE(error.isEmpty());
}

TEST(PlatformProfileResolution, ResolveRejectsExplicitNonTargetPlatform)
{
    VNX::ProjectData project;
    project.platforms = HarkenTest::platforms({QStringLiteral("Android"), QStringLiteral("iOS")});
    project.packExportSelectedPlatformIds = project.platformIds();

    QString error;
    EXPECT_TRUE(VNX::ProjectPackExporter::resolveExportPlatforms(
                    project, HarkenTest::platformIdString("Windows"), &error)
                    .isEmpty());
    EXPECT_TRUE(error.contains(HarkenTest::platformIdString("Windows")));
    EXPECT_TRUE(error.contains(QStringLiteral("Unknown project platform ID")));

    error.clear();
    EXPECT_EQ(VNX::ProjectPackExporter::resolveExportPlatforms(
                  project, HarkenTest::platformIdString("iOS"), &error),
              QStringList{HarkenTest::platformIdString("iOS")});
    EXPECT_TRUE(error.isEmpty());
}

TEST(PlatformProfileResolution, ResolveFailsWhenTargetPlatformsEmpty)
{
    VNX::ProjectData project;
    project.packExportSelectedPlatformIds = {HarkenTest::platformIdString("Windows")};

    QString error;
    EXPECT_TRUE(
        VNX::ProjectPackExporter::resolveExportPlatforms(project, QString(), &error).isEmpty());
    EXPECT_FALSE(error.isEmpty());
}

TEST(PlatformProfileResolution, ExportRejectsUnsupportedPlatform)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    auto project = HarkenTest::projectWithPlatforms();
    VNX::ProjectPackExporter exporter;

    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = QStringLiteral("PS5");
    auto result = exporter.ExportAllPacks(project, dir.path(), opts);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.contains(QStringLiteral("PS5")));

    opts.platform.clear();
    result = exporter.ExportAllPacks(project, dir.path(), opts);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.contains(QStringLiteral("Unknown project platform ID")));
}
