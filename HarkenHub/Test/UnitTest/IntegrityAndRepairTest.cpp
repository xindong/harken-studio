#include "Transactions/RepairExecutor.h"
#include "Versions/IntegrityChecker.h"

#include "Platform/PlatformPaths.h"
#include "Transactions/InstallIndex.h"
#include "Support/StudioProtocolTestUtils.h"

#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <memory>

using VNH::InstallIndex;
using VNH::InstalledVersionRecord;
using VNH::IntegrityChecker;
using VNH::RepairExecutor;

namespace {

const QDateTime kNow =
    QDateTime::fromString(QStringLiteral("2026-07-24T12:00:00Z"), Qt::ISODate);

class EnvGuard
{
public:
    EnvGuard(const char* name, const QString& value)
        : name_(name)
    {
        qputenv(name, value.toUtf8());
    }
    ~EnvGuard() { qunsetenv(name_); }

private:
    const char* name_;
};

void writeFile(const QString& path, const QByteArray& content)
{
    QDir().mkpath(QFileInfo(path).dir().path());
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(content);
}

QString makeInstallDir(const QString& root, const QString& buildId)
{
    const QByteArray binary = QByteArrayLiteral("stub-binary");
    const QByteArray asset = QByteArrayLiteral("payload-data");
    const QString installPath = root + QStringLiteral("/1.4.0-") + buildId;

    auto sha = [](const QByteArray& data) {
        return QString::fromLatin1(
            QCryptographicHash::hash(data, QCryptographicHash::Sha256)
                .toHex());
    };

    QJsonArray files;
    QJsonObject binaryRecord;
    binaryRecord[QStringLiteral("path")] = VNH::studioBinaryRelativePath();
    binaryRecord[QStringLiteral("size")] = binary.size();
    binaryRecord[QStringLiteral("sha256")] = sha(binary);
    files.append(binaryRecord);
    QJsonObject assetRecord;
    assetRecord[QStringLiteral("path")] = QStringLiteral("data/asset.bin");
    assetRecord[QStringLiteral("size")] = asset.size();
    assetRecord[QStringLiteral("sha256")] = sha(asset);
    files.append(assetRecord);

    QJsonObject manifest;
    manifest[QStringLiteral("product")] = QStringLiteral("HarkenStudio");
    manifest[QStringLiteral("buildId")] = buildId;
    manifest[QStringLiteral("version")] = QStringLiteral("1.4.0");
    manifest[QStringLiteral("files")] = files;

    writeFile(installPath + QStringLiteral("/") + VNH::studioBinaryRelativePath(), binary);
    writeFile(installPath + QStringLiteral("/data/asset.bin"), asset);
    writeFile(installPath + QStringLiteral("/package-manifest.json"),
              QJsonDocument(manifest).toJson());
    writeFile(installPath + QStringLiteral("/capabilities.json"),
              QJsonDocument(HarkenHubTest::studioCapabilities(buildId, "1.4.0")).toJson());
    return installPath;
}

class IntegrityAndRepairTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        machineGuard = std::make_unique<EnvGuard>(
            "HARKEN_HUB_MACHINE_ROOT",
            dir.filePath(QStringLiteral("machine")));
        versionsGuard = std::make_unique<EnvGuard>(
            "HARKEN_HUB_VERSIONS_ROOT",
            dir.filePath(QStringLiteral("versions")));
        installPath = makeInstallDir(VNH::studioVersionsRoot(),
                                     QStringLiteral("run1.aaaa1111"));
    }

    void registerRecord(const QString& healthState)
    {
        InstalledVersionRecord record;
        record.buildId = QStringLiteral("run1.aaaa1111");
        record.semanticVersion = QStringLiteral("1.4.0");
        record.installPath = installPath;
        record.healthState = healthState;
        QString error;
        ASSERT_TRUE(InstallIndex::commitInstallation(record, 1, &error))
            << error.toStdString();
    }

    QTemporaryDir dir;
    std::unique_ptr<EnvGuard> machineGuard;
    std::unique_ptr<EnvGuard> versionsGuard;
    QString installPath;
};

TEST_F(IntegrityAndRepairTest, AcceptsIntactInstall)
{
    const auto report = IntegrityChecker::check(installPath);
    EXPECT_TRUE(report.ok) << report.error.toStdString();
}

TEST_F(IntegrityAndRepairTest, DetectsMissingFile)
{
    QFile::remove(installPath + QStringLiteral("/data/asset.bin"));
    const auto report = IntegrityChecker::check(installPath);
    EXPECT_FALSE(report.ok);
    EXPECT_EQ(report.missingFiles,
              QStringList{QStringLiteral("data/asset.bin")});
}

TEST_F(IntegrityAndRepairTest, DetectsTamperedFile)
{
    writeFile(installPath + QStringLiteral("/data/asset.bin"),
              QByteArrayLiteral("tampered-content"));
    const auto report = IntegrityChecker::check(installPath);
    EXPECT_FALSE(report.ok);
    EXPECT_EQ(report.mismatchedFiles,
              QStringList{QStringLiteral("data/asset.bin")});
}

TEST_F(IntegrityAndRepairTest, RejectsMissingManifest)
{
    QFile::remove(installPath + QStringLiteral("/package-manifest.json"));
    const auto report = IntegrityChecker::check(installPath);
    EXPECT_FALSE(report.ok);
}

TEST_F(IntegrityAndRepairTest, RejectsModifiedPackageMetadata)
{
    const auto report = IntegrityChecker::check(
        installPath, QString(64, QLatin1Char('f')), QString());
    EXPECT_FALSE(report.ok);
    EXPECT_EQ(report.mismatchedFiles,
              QStringList{QStringLiteral("package-manifest.json")});
}

TEST_F(IntegrityAndRepairTest, RepairMarksIntactVersionInstalled)
{
    registerRecord(QStringLiteral("NeedsRepair"));

    const auto outcome =
        RepairExecutor::run(QStringLiteral("run1.aaaa1111"), kNow);
    EXPECT_TRUE(outcome.ok) << outcome.error.toStdString();
    EXPECT_EQ(outcome.healthState, QStringLiteral("Installed"));

    const auto snapshot = InstallIndex::snapshot();
    ASSERT_TRUE(snapshot.ok);
    ASSERT_EQ(snapshot.records.size(), 1u);
    EXPECT_EQ(snapshot.records.front().healthState,
              QStringLiteral("Installed"));
}

TEST_F(IntegrityAndRepairTest, RepairKeepsBrokenVersionInNeedsRepair)
{
    registerRecord(QStringLiteral("NeedsRepair"));
    writeFile(installPath + QStringLiteral("/data/asset.bin"),
              QByteArrayLiteral("tampered-content"));

    const auto outcome =
        RepairExecutor::run(QStringLiteral("run1.aaaa1111"), kNow);
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failureCode,
              QStringLiteral("RepairSourceMissing"));

    const auto snapshot = InstallIndex::snapshot();
    ASSERT_TRUE(snapshot.ok);
    ASSERT_EQ(snapshot.records.size(), 1u);
    EXPECT_EQ(snapshot.records.front().healthState,
              QStringLiteral("NeedsRepair"));
}

TEST_F(IntegrityAndRepairTest, RepairDoesNotClearMarkerWhenIntactFilesBelongToAnotherVersion)
{
    registerRecord(QStringLiteral("NeedsRepair"));
    auto snapshot = InstallIndex::snapshot();
    ASSERT_TRUE(snapshot.ok);
    ASSERT_EQ(snapshot.records.size(), 1u);
    auto record = snapshot.records.front();
    record.semanticVersion = QStringLiteral("1.5.0");
    QString error;
    ASSERT_TRUE(InstallIndex::commitInstallation(record, 1, &error));
    ASSERT_TRUE(IntegrityChecker::check(installPath).ok);

    const auto outcome = RepairExecutor::run(record.buildId, kNow);
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failureCode, QStringLiteral("IdentityMismatch"));
    snapshot = InstallIndex::snapshot();
    ASSERT_TRUE(snapshot.ok);
    ASSERT_EQ(snapshot.records.size(), 1u);
    EXPECT_EQ(snapshot.records.front().healthState, QStringLiteral("NeedsRepair"));
    EXPECT_TRUE(IntegrityChecker::check(installPath).ok);
}

TEST_F(IntegrityAndRepairTest, RepairRejectsUnknownBuild)
{
    const auto outcome =
        RepairExecutor::run(QStringLiteral("run9.ffff9999"), kNow);
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failureCode, QStringLiteral("UnknownBuild"));
}

}
