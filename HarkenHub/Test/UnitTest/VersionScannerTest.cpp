#include "Versions/VersionScanner.h"
#include "Support/StudioProtocolTestUtils.h"

#include "Platform/PlatformPaths.h"
#include "Transactions/InstallIndex.h"
#include "Versions/LocalBuildRegistry.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

using VNH::VersionScanner;

namespace {

class EnvGuard
{
public:
    EnvGuard(const char* name, const QString& value) : name_(name)
    {
        qputenv(name, value.toUtf8());
    }
    ~EnvGuard() { qunsetenv(name_); }

private:
    const char* name_;
};

bool writeJson(const QString& path, const QJsonObject& object)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(object).toJson());
    file.close();
    return true;
}

bool makeVersionDir(const QString& root, const QString& directoryName,
                    const QString& buildId, const QString& version,
                    bool withManifest = true, bool withCapabilities = true,
                    bool withBinary = true)
{
    const QString installPath = root + QStringLiteral("/") + directoryName;
    if (!QDir().mkpath(installPath))
        return false;

    if (withManifest) {
        QJsonObject manifest;
        manifest[QStringLiteral("product")] = QStringLiteral("HarkenStudio");
        manifest[QStringLiteral("buildId")] = buildId;
        manifest[QStringLiteral("version")] = version;
        manifest[QStringLiteral("channel")] = QStringLiteral("stable");
        manifest[QStringLiteral("releaseSequence")] = 12;
        if (!writeJson(installPath + QStringLiteral("/package-manifest.json"), manifest))
            return false;
    }

    if (withCapabilities) {
        const auto capabilities = HarkenHubTest::studioCapabilities(buildId, version);
        if (!writeJson(installPath + QStringLiteral("/capabilities.json"), capabilities))
            return false;
    }

    if (withBinary) {
        const QString binaryPath =
            installPath + QStringLiteral("/") + VNH::studioBinaryRelativePath();
        if (!QDir().mkpath(QFileInfo(binaryPath).absolutePath()))
            return false;
        QFile binary(binaryPath);
        if (!binary.open(QIODevice::WriteOnly))
            return false;
        binary.write("stub");
        binary.close();
    }

    return true;
}

}

TEST(VersionScanner, FindsCompleteVersionDirectory)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(makeVersionDir(dir.path(), QStringLiteral("1.4.0-run12.abcd1234"),
                               QStringLiteral("run12.abcd1234"),
                               QStringLiteral("1.4.0")));

    const auto versions = VersionScanner::scan(dir.path());
    ASSERT_EQ(versions.size(), 1u);
    EXPECT_EQ(versions[0].directoryName, QStringLiteral("1.4.0-run12.abcd1234"));
    EXPECT_EQ(versions[0].buildId, QStringLiteral("run12.abcd1234"));
    EXPECT_EQ(versions[0].semanticVersion, QStringLiteral("1.4.0"));
    EXPECT_EQ(versions[0].channel, QStringLiteral("stable"));
    EXPECT_EQ(versions[0].executablePath,
              QDir(versions[0].installPath)
                  .filePath(VNH::studioBinaryRelativePath()));
    EXPECT_EQ(versions[0].releaseSequence, 12);
}

TEST(VersionScanner, KeepsLegacyInstalledVersionsAvailable)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString directoryName = QStringLiteral("0.1.19-dev-legacy");
    ASSERT_TRUE(makeVersionDir(dir.path(), directoryName,
                               QStringLiteral("legacy.aaaa1111"),
                               QStringLiteral("0.1.19-dev")));

    const QJsonObject legacyCapabilities{
        {QStringLiteral("readableSchemaMin"), 1},
        {QStringLiteral("readableSchemaMax"), 2},
        {QStringLiteral("writableSchema"), 2},
    };
    ASSERT_TRUE(writeJson(dir.path() + QStringLiteral("/") + directoryName +
                              QStringLiteral("/capabilities.json"),
                          legacyCapabilities));

    const auto versions = VersionScanner::scan(dir.path());
    ASSERT_EQ(versions.size(), 1u);
    EXPECT_TRUE(versions.front().available);
    EXPECT_EQ(versions.front().capabilitiesVersion, 0);
}

TEST(VersionScanner, SkipsDirectoryWithoutManifest)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(makeVersionDir(dir.path(), QStringLiteral("1.4.0-run12.abcd1234"),
                               QStringLiteral("run12.abcd1234"),
                               QStringLiteral("1.4.0"), false));

    EXPECT_TRUE(VersionScanner::scan(dir.path()).empty());
}

TEST(VersionScanner, SkipsDirectoryWithoutCapabilities)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(makeVersionDir(dir.path(), QStringLiteral("1.4.0-run12.abcd1234"),
                               QStringLiteral("run12.abcd1234"),
                               QStringLiteral("1.4.0"), true, false));

    EXPECT_TRUE(VersionScanner::scan(dir.path()).empty());
}

TEST(VersionScanner, SkipsDirectoryWithoutBinary)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(makeVersionDir(dir.path(), QStringLiteral("1.4.0-run12.abcd1234"),
                               QStringLiteral("run12.abcd1234"),
                               QStringLiteral("1.4.0"), true, true, false));

    EXPECT_TRUE(VersionScanner::scan(dir.path()).empty());
}

TEST(VersionScanner, SkipsTransientDotPrefixedDirectories)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(makeVersionDir(dir.path(),
                               QStringLiteral(".staging-0123456789abcdef0123456789abcdef"),
                               QStringLiteral("run12.abcd1234"),
                               QStringLiteral("1.4.0")));
    ASSERT_TRUE(makeVersionDir(dir.path(),
                               QStringLiteral(".uninstall-0123456789abcdef0123456789abcdef"),
                               QStringLiteral("run13.bcde2345"),
                               QStringLiteral("1.5.0")));
    ASSERT_TRUE(makeVersionDir(dir.path(), QStringLiteral("1.6.0-run14.cdef3456"),
                               QStringLiteral("run14.cdef3456"),
                               QStringLiteral("1.6.0")));

    const auto versions = VersionScanner::scan(dir.path());
    ASSERT_EQ(versions.size(), 1u);
    EXPECT_EQ(versions[0].buildId, QStringLiteral("run14.cdef3456"));
}

TEST(VersionScanner, MissingRootYieldsEmpty)
{
    EXPECT_TRUE(VersionScanner::scan(QStringLiteral("Z:/no/such/root")).empty());
}

TEST(VersionScanner, InstalledScanUsesIndexAsSourceOfTruth)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const EnvGuard machine("HARKEN_HUB_MACHINE_ROOT",
                           dir.filePath(QStringLiteral("machine")));
    const EnvGuard versions("HARKEN_HUB_VERSIONS_ROOT",
                            dir.filePath(QStringLiteral("versions")));
    ASSERT_TRUE(makeVersionDir(VNH::studioVersionsRoot(),
                               QStringLiteral("1.4.0-run12.abcd1234"),
                               QStringLiteral("run12.abcd1234"),
                               QStringLiteral("1.4.0")));
    ASSERT_TRUE(makeVersionDir(VNH::studioVersionsRoot(),
                               QStringLiteral("9.9.9-unknown"),
                               QStringLiteral("unknown"),
                               QStringLiteral("9.9.9")));

    VNH::InstalledVersionRecord record;
    record.buildId = QStringLiteral("run12.abcd1234");
    record.semanticVersion = QStringLiteral("1.4.0");
    record.channel = QStringLiteral("stable");
    record.installPath = VNH::studioVersionsRoot() +
                         QStringLiteral("/1.4.0-run12.abcd1234");
    record.healthState = QStringLiteral("Installed");
    QString error;
    ASSERT_TRUE(VNH::InstallIndex::commitInstallation(record, 7, &error))
        << error.toStdString();

    const auto installed = VersionScanner::scanInstalled();
    ASSERT_EQ(installed.size(), 1u);
    EXPECT_EQ(installed.front().buildId, QStringLiteral("run12.abcd1234"));
}

TEST(VersionScanner, InspectionDistinguishesMissingInvalidUnreadableAndMismatchedMetadata)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const EnvGuard root("HARKEN_HUB_VERSIONS_ROOT", dir.path());
    VNH::InstalledVersionRecord record;
    record.buildId = "registered-build";
    record.semanticVersion = "0.1.20-dev";
    record.installPath = dir.filePath("installed");
    QString error;
    EXPECT_EQ(VersionScanner::inspect(record, nullptr, &error), VNH::InstallationIssue::Missing);
    EXPECT_TRUE(error.contains("directory"));
    ASSERT_TRUE(makeVersionDir(dir.path(), "installed", "other-build", record.semanticVersion,
                               true, true, false));
    EXPECT_EQ(VersionScanner::inspect(record, nullptr, &error), VNH::InstallationIssue::IdentityMismatch);
    EXPECT_TRUE(error.contains("identity"));
    record.buildId = "other-build";
    EXPECT_EQ(VersionScanner::inspect(record, nullptr, &error), VNH::InstallationIssue::Missing);
    EXPECT_TRUE(error.contains("executable"));
    const auto manifest = record.installPath + "/package-manifest.json";
    ASSERT_TRUE(writeJson(manifest, {}));
    EXPECT_EQ(VersionScanner::inspect(record, nullptr, &error), VNH::InstallationIssue::InvalidMetadata);
    ASSERT_TRUE(QFile::remove(manifest));
    ASSERT_TRUE(QDir().mkpath(manifest));
    EXPECT_EQ(VersionScanner::inspect(record, nullptr, &error), VNH::InstallationIssue::Unreadable);
    EXPECT_TRUE(error.contains("Cannot read"));
}

TEST(VersionScanner, KeepsDistinctMissingLocalBuildsVisibleAndRemovable)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const EnvGuard user("HARKEN_HUB_USER_ROOT", dir.filePath("user"));
    const EnvGuard machine("HARKEN_HUB_MACHINE_ROOT", dir.filePath("machine"));
    const EnvGuard root("HARKEN_HUB_VERSIONS_ROOT", dir.filePath("versions"));
    VNH::LocalBuildRecord first;
    first.executablePath = dir.filePath("first/HarkenStudio.exe");
    first.reportedBuildId = "first-build";
    first.semanticVersion = "1.4.0";
    first.capabilitiesVersion = 2;
    auto second = first;
    second.executablePath = dir.filePath("second/HarkenStudio.exe");
    second.reportedBuildId = "second-build";
    QString error;
    ASSERT_TRUE(VNH::LocalBuildRegistry::upsert(first, &error));
    ASSERT_TRUE(VNH::LocalBuildRegistry::upsert(second, &error));

    const auto versions = VersionScanner::scanManaged();
    ASSERT_EQ(versions.size(), 2u);
    EXPECT_FALSE(versions[0].available);
    EXPECT_FALSE(versions[1].available);
    EXPECT_NE(versions[0].buildId, versions[1].buildId);
    EXPECT_TRUE(VersionScanner::scanAvailable().empty());
    ASSERT_TRUE(VNH::LocalBuildRegistry::remove(first.executablePath, &error));
    const auto remaining = VersionScanner::scanManaged();
    ASSERT_EQ(remaining.size(), 1u);
    EXPECT_EQ(remaining.front().executablePath, second.executablePath);
}

TEST(VersionScanner, DeduplicatesTheSameExecutableButKeepsSeparateCopies)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const EnvGuard user("HARKEN_HUB_USER_ROOT", dir.filePath("user"));
    const EnvGuard machine("HARKEN_HUB_MACHINE_ROOT", dir.filePath("machine"));
    const EnvGuard root("HARKEN_HUB_VERSIONS_ROOT", dir.filePath("versions"));
    ASSERT_TRUE(makeVersionDir(VNH::studioVersionsRoot(), "installed", "shared-build", "1.4.0"));
    ASSERT_TRUE(makeVersionDir(dir.path(), "local", "shared-build", "1.4.0"));
    VNH::InstalledVersionRecord installed;
    installed.buildId = "shared-build";
    installed.semanticVersion = "1.4.0";
    installed.installPath = QDir(VNH::studioVersionsRoot()).filePath("installed");
    installed.healthState = "Installed";
    QString error;
    ASSERT_TRUE(VNH::InstallIndex::commitInstallation(installed, 7, &error));
    VNH::LocalBuildRecord local;
    local.executablePath = QDir(installed.installPath).filePath(VNH::studioBinaryRelativePath());
    local.reportedBuildId = installed.buildId;
    local.semanticVersion = installed.semanticVersion;
    ASSERT_TRUE(VNH::LocalBuildRegistry::upsert(local, &error));
    const auto sameFile = VersionScanner::scanManaged();
    ASSERT_EQ(sameFile.size(), 1u);
    EXPECT_FALSE(sameFile.front().isLocalBuild());

    local.executablePath = QDir(dir.filePath("local")).filePath(VNH::studioBinaryRelativePath());
    ASSERT_TRUE(VNH::LocalBuildRegistry::upsert(local, &error));
    const auto copies = VersionScanner::scanManaged();
    ASSERT_EQ(copies.size(), 2u);
    EXPECT_EQ(copies[0].expectedBuildId(), copies[1].expectedBuildId());
    EXPECT_NE(copies[0].executablePath, copies[1].executablePath);
}
