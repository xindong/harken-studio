#include "Launch/LaunchPreflight.h"

#include "Platform/PlatformPaths.h"
#include "Versions/LocalBuildRegistry.h"
#include "Support/StudioProtocolTestUtils.h"

#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QScopeGuard>

using VNH::InstalledVersion;
using VNH::InstalledVersionRecord;
using VNH::InstallIndexSnapshot;
using VNH::LaunchIntent;
using VNH::LaunchPreflight;
using VNH::LaunchVerdict;
using VNH::ProjectRecord;
using VNH::VersionSource;

namespace {

void writeFile(const QString& path, const QByteArray& content)
{
    QDir().mkpath(QFileInfo(path).dir().path());
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(file.write(content), content.size());
}

class LaunchPreflightTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        projectPath = dir.filePath(QStringLiteral("Demo.hsp"));
        const QString projectId =
            QStringLiteral("11111111-2222-3333-4444-555555555555");
        QJsonObject root{
            {QStringLiteral("harkenDocument"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("project.root")},
                {QStringLiteral("id"), projectId},
                {QStringLiteral("projectId"), projectId},
                {QStringLiteral("schemaVersion"), 5},
                {QStringLiteral("savedStudioVersion"), QStringLiteral("1.4.0")},
            }},
            {QStringLiteral("content"), QJsonObject{
                {QStringLiteral("metadata"), QJsonObject{
                    {QStringLiteral("name"), QStringLiteral("Demo")},
                }},
            }},
        };
        writeFile(projectPath, QJsonDocument(root).toJson());
        record.canonicalPath = QFileInfo(projectPath).canonicalFilePath();
        record.displayName = QStringLiteral("Demo");
        record.detectedSchema = 5;
        index.ok = true;
    }

    InstalledVersion makeVersion(const QString& buildId,
                                 const QString& softwareVersion = QStringLiteral("1.4.0"))
    {
        InstalledVersion version;
        version.buildId = buildId;
        version.semanticVersion = softwareVersion;
        version.installPath = dir.filePath(buildId);
        version.capabilitiesVersion = 2;
        const QByteArray binary = QByteArrayLiteral("stub");
        writeFile(version.installPath + QStringLiteral("/")
                      + VNH::studioBinaryRelativePath(), binary);
        QJsonObject fileRecord{
            {QStringLiteral("path"), VNH::studioBinaryRelativePath()},
            {QStringLiteral("size"), binary.size()},
            {QStringLiteral("sha256"),
             QString::fromLatin1(QCryptographicHash::hash(
                 binary, QCryptographicHash::Sha256).toHex())},
        };
        QJsonObject manifest{
            {QStringLiteral("product"), QStringLiteral("HarkenStudio")},
            {QStringLiteral("buildId"), buildId},
            {QStringLiteral("version"), version.semanticVersion},
            {QStringLiteral("files"), QJsonArray{fileRecord}},
        };
        writeFile(version.installPath + QStringLiteral("/package-manifest.json"),
                  QJsonDocument(manifest).toJson());
        writeFile(version.installPath + QStringLiteral("/capabilities.json"),
                  QJsonDocument(HarkenHubTest::studioCapabilities(buildId, softwareVersion)).toJson());
        return version;
    }

    QTemporaryDir dir;
    QString projectPath;
    ProjectRecord record;
    InstallIndexSnapshot index;
};

TEST_F(LaunchPreflightTest, DirectOpenUsesLockedCompatibleVersion)
{
    const auto version = makeVersion(QStringLiteral("run1.aaaa1111"));
    record.preferredStudioBuildId = version.buildId;
    const auto decision = LaunchPreflight::evaluate(record, {version}, index);
    EXPECT_EQ(decision.verdict, LaunchVerdict::Ready);
    EXPECT_EQ(decision.version.buildId, version.buildId);
    EXPECT_EQ(decision.intent, LaunchIntent::DirectOpenOnly);
}

TEST_F(LaunchPreflightTest, LocalBuildDoesNotRequirePackageMetadata)
{
    InstalledVersion version;
    version.buildId = QStringLiteral("local-user-aaaa1111");
    version.runtimeBuildId = QStringLiteral("local.aaaa1111");
    version.semanticVersion = QStringLiteral("0.1.21-dev");
    version.installPath = dir.filePath(QStringLiteral("local-build"));
    version.executablePath = QDir(version.installPath).filePath(
        QStringLiteral("custom/StudioUnderTest.exe"));
    version.capabilitiesVersion = 2;
    version.source = VersionSource::LocalBuild;
    writeFile(version.executablePath, QByteArrayLiteral("stub"));
    version.executableFingerprint = VNH::LocalBuildRegistry::fileFingerprint(version.executablePath);
    record.preferredStudioBuildId = version.buildId;
    const auto decision = LaunchPreflight::evaluate(record, {version}, index);
    EXPECT_EQ(decision.verdict, LaunchVerdict::Ready);
    EXPECT_EQ(decision.version.launchBuildId(), QStringLiteral("local.aaaa1111"));
}

TEST_F(LaunchPreflightTest, SameSizeReplacementWithPreservedTimestampRequiresRecheck)
{
    VNH::LocalBuildRecord local;
    local.executablePath = dir.filePath("local/HarkenStudio.exe");
    local.reportedBuildId = "local.aaaa1111";
    local.semanticVersion = "1.4.0";
    local.capabilitiesVersion = 2;
    writeFile(local.executablePath, "first build");
    const auto modified = QFileInfo(local.executablePath).lastModified();
    local.executableSize = QFileInfo(local.executablePath).size();
    local.executableModifiedMs = modified.toMSecsSinceEpoch();
    local.executableFingerprint = VNH::LocalBuildRegistry::fileFingerprint(local.executablePath);
    qputenv("HARKEN_HUB_USER_ROOT", dir.filePath("user").toUtf8());
    const auto cleanup = qScopeGuard([]() { qunsetenv("HARKEN_HUB_USER_ROOT"); });
    QString error;
    ASSERT_TRUE(VNH::LocalBuildRegistry::upsert(local, &error));
    const auto versions = VNH::VersionScanner::scanLocalBuilds();
    ASSERT_EQ(versions.size(), 1u);
    EXPECT_EQ(versions.front().executableFingerprint, local.executableFingerprint);
    EXPECT_EQ(LaunchPreflight::evaluate(record, versions, index).verdict, LaunchVerdict::Ready);

    writeFile(local.executablePath, "other build");
    QFile changed(local.executablePath);
    ASSERT_TRUE(changed.open(QIODevice::ReadWrite));
    ASSERT_TRUE(changed.setFileTime(modified, QFileDevice::FileModificationTime));
    changed.close();
    ASSERT_EQ(QFileInfo(local.executablePath).size(), local.executableSize);
    ASSERT_EQ(QFileInfo(local.executablePath).lastModified().toMSecsSinceEpoch(), local.executableModifiedMs);
    const auto decision = LaunchPreflight::evaluate(record, versions, index);
    EXPECT_EQ(decision.verdict, LaunchVerdict::Blocked);
    EXPECT_EQ(decision.blockCode, QStringLiteral("VersionUnavailable"));
}

TEST_F(LaunchPreflightTest, DefaultOpenNeverAuthorizesUpgrade)
{
    const auto version = makeVersion(QStringLiteral("run2.bbbb2222"));
    const auto decision = LaunchPreflight::evaluate(record, {version}, index);
    EXPECT_EQ(decision.verdict, LaunchVerdict::Ready);
    EXPECT_EQ(decision.intent, LaunchIntent::DirectOpenOnly);
}

TEST_F(LaunchPreflightTest, SelectedVersionCanBeAnalyzedWithoutAuthorizingUpgrade)
{
    const auto version = makeVersion(QStringLiteral("run2.bbbb2222"));
    const auto decision = LaunchPreflight::evaluate(
        record, {version}, index,
        LaunchIntent::DirectOpenOnly, version.buildId);
    EXPECT_EQ(decision.verdict, LaunchVerdict::Ready);
    EXPECT_EQ(decision.intent, LaunchIntent::DirectOpenOnly);
}

TEST_F(LaunchPreflightTest, HubApprovalKeepsTheAnalyzedVersion)
{
    const auto version = makeVersion(QStringLiteral("run2.bbbb2222"));
    const auto decision = LaunchPreflight::evaluate(
        record, {version}, index,
        LaunchIntent::UpgradeApprovedByHub, version.buildId);
    EXPECT_EQ(decision.verdict, LaunchVerdict::Ready);
    EXPECT_EQ(decision.intent, LaunchIntent::UpgradeApprovedByHub);
}

TEST_F(LaunchPreflightTest, FixedVersionDoesNotAuthorizeAnUpgrade)
{
    const auto version = makeVersion(QStringLiteral("run2.bbbb2222"));
    record.preferredStudioBuildId = version.buildId;
    const auto decision = LaunchPreflight::evaluate(record, {version}, index);
    EXPECT_EQ(decision.verdict, LaunchVerdict::Ready);
    EXPECT_EQ(decision.intent, LaunchIntent::DirectOpenOnly);
}

TEST_F(LaunchPreflightTest, MultipleMatchingInstallationsRequireSelection)
{
    auto oldVersion = makeVersion(QStringLiteral("run1.aaaa1111"));
    auto newVersion = makeVersion(QStringLiteral("run2.bbbb2222"));
    const auto decision = LaunchPreflight::evaluate(
        record, {newVersion, oldVersion}, index);
    EXPECT_EQ(decision.verdict, LaunchVerdict::Blocked);
    EXPECT_EQ(decision.blockCode, QStringLiteral("VersionSelectionRequired"));
}

TEST_F(LaunchPreflightTest, MissingSavedVersionUsesLastSuccessfulInstallation)
{
    auto oldVersion = makeVersion(QStringLiteral("run1.aaaa1111"));
    auto newVersion = makeVersion(QStringLiteral("run2.bbbb2222"));
    QFile input(projectPath);
    ASSERT_TRUE(input.open(QIODevice::ReadOnly));
    auto root = QJsonDocument::fromJson(input.readAll()).object();
    input.close();
    auto header = root.value(QStringLiteral("harkenDocument")).toObject();
    header.remove(QStringLiteral("savedStudioVersion"));
    root.insert(QStringLiteral("harkenDocument"), header);
    writeFile(projectPath, QJsonDocument(root).toJson());
    record.lastKnownGoodStudioBuildId = oldVersion.buildId;
    const auto decision = LaunchPreflight::evaluate(
        record, {oldVersion, newVersion}, index);
    EXPECT_EQ(decision.verdict, LaunchVerdict::Ready);
    EXPECT_EQ(decision.version.buildId, oldVersion.buildId);
}

TEST_F(LaunchPreflightTest, BlocksTamperedVersion)
{
    auto version = makeVersion(QStringLiteral("run2.bbbb2222"));
    record.preferredStudioBuildId = version.buildId;
    writeFile(version.installPath + QStringLiteral("/")
                  + VNH::studioBinaryRelativePath(), QByteArrayLiteral("evil"));
    const auto decision = LaunchPreflight::evaluate(record, {version}, index);
    EXPECT_EQ(decision.blockCode, QStringLiteral("VersionNeedsRepair"));
}

TEST_F(LaunchPreflightTest, BlocksMissingProject)
{
    record.canonicalPath = dir.filePath(QStringLiteral("Gone.hsp"));
    const auto decision = LaunchPreflight::evaluate(record, {}, index);
    EXPECT_EQ(decision.blockCode, QStringLiteral("ProjectMissing"));
}

TEST_F(LaunchPreflightTest, BlocksMissingLockedVersion)
{
    record.preferredStudioBuildId = QStringLiteral("run9.ffff9999");
    const auto version = makeVersion(QStringLiteral("run1.aaaa1111"));
    const auto decision = LaunchPreflight::evaluate(record, {version}, index);
    EXPECT_EQ(decision.blockCode, QStringLiteral("VersionUnavailable"));
}

TEST_F(LaunchPreflightTest, BlocksNeedsRepairVersion)
{
    const auto version = makeVersion(QStringLiteral("run1.aaaa1111"));
    record.preferredStudioBuildId = version.buildId;
    InstalledVersionRecord broken;
    broken.buildId = version.buildId;
    broken.healthState = QStringLiteral("NeedsRepair");
    index.records.push_back(broken);
    const auto decision = LaunchPreflight::evaluate(record, {version}, index);
    EXPECT_EQ(decision.blockCode, QStringLiteral("VersionNeedsRepair"));
}

TEST_F(LaunchPreflightTest, BlocksIdentityChange)
{
    const QString projectId =
        QStringLiteral("22222222-2222-2222-2222-222222222222");
    QJsonObject root{
        {QStringLiteral("harkenDocument"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("project.root")},
            {QStringLiteral("id"), projectId},
            {QStringLiteral("projectId"), projectId},
            {QStringLiteral("schemaVersion"), 5},
            {QStringLiteral("savedStudioVersion"), QStringLiteral("1.4.0")},
        }},
        {QStringLiteral("content"), QJsonObject{}},
    };
    writeFile(projectPath, QJsonDocument(root).toJson());
    record.projectId = QStringLiteral("11111111-1111-1111-1111-111111111111");
    const auto version = makeVersion(QStringLiteral("run1.aaaa1111"));
    const auto decision = LaunchPreflight::evaluate(record, {version}, index);
    EXPECT_EQ(decision.blockCode, QStringLiteral("IdentityMismatch"));
}

TEST_F(LaunchPreflightTest, ExplicitChoiceOverridesTheSavedSoftwareVersion)
{
    const auto saved = makeVersion(QStringLiteral("saved"));
    const auto selected = makeVersion(QStringLiteral("selected"), QStringLiteral("0.9.0"));
    record.preferredStudioBuildId = saved.buildId;
    const auto decision = LaunchPreflight::evaluate(record, {saved, selected}, index,
        LaunchIntent::DirectOpenOnly, selected.buildId);
    EXPECT_EQ(decision.verdict, LaunchVerdict::Ready);
    EXPECT_EQ(decision.version.buildId, selected.buildId);
}

TEST_F(LaunchPreflightTest, UnknownProjectDetailsDoNotBlockAnExplicitInstallation)
{
    writeFile(projectPath, QByteArrayLiteral("unrecognized project contents"));
    const auto version = makeVersion(QStringLiteral("selected"));
    const auto decision = LaunchPreflight::evaluate(record, {version}, index,
        LaunchIntent::DirectOpenOnly, version.buildId);
    EXPECT_EQ(decision.verdict, LaunchVerdict::Ready);
}

TEST_F(LaunchPreflightTest, MissingSavedVersionAndHistoryRequireSelection)
{
    writeFile(projectPath, QByteArrayLiteral("{}"));
    const auto version = makeVersion(QStringLiteral("selected"));
    const auto decision = LaunchPreflight::evaluate(record, {version}, index);
    EXPECT_EQ(decision.blockCode, QStringLiteral("VersionSelectionRequired"));
}

}
