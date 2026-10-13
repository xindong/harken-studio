#include "Transactions/InstallRequest.h"

#include "Packaging/PackageReader.h"
#include "Platform/PlatformPaths.h"
#include "Support/ManifestTestUtils.h"
#include "Transactions/HandoffArea.h"
#include "Transactions/InstallExecutor.h"
#include "Transactions/InstallIndex.h"
#include "Versions/VersionScanner.h"
#include "Transactions/RepairExecutor.h"
#include "ZipTestUtils.h"

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

using VNH::InstallRequest;
using VNH::PackageReader;
using HarkenHubTest::baseManifestPayload;
using HarkenHubTest::makeKeypair;
using HarkenHubTest::signEnvelope;
using HarkenHubTest::TestEntry;
using HarkenHubTest::TestKeypair;
using HarkenHubTest::writeArchive;

namespace {

const QDateTime kNow =
    QDateTime::fromString(QStringLiteral("2026-07-23T12:00:00Z"), Qt::ISODate);

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

QByteArray sha256Hex(const QByteArray& data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

QString buildPackage(const QString& path, bool legacyCapabilities = false)
{
    const QByteArray binary = QByteArrayLiteral("stub-binary");

    QJsonArray files;
    QJsonObject binaryRecord;
    binaryRecord[QStringLiteral("path")] = VNH::studioBinaryRelativePath();
    binaryRecord[QStringLiteral("size")] = binary.size();
    binaryRecord[QStringLiteral("sha256")] = QString::fromLatin1(sha256Hex(binary));
    files.append(binaryRecord);

    QJsonObject manifest;
    manifest[QStringLiteral("product")] = QStringLiteral("HarkenStudio");
    manifest[QStringLiteral("version")] = QStringLiteral("1.4.0");
    manifest[QStringLiteral("buildId")] = QStringLiteral("run12.abcd1234");
    manifest[QStringLiteral("channel")] = QStringLiteral("stable");
    manifest[QStringLiteral("files")] = files;

    QJsonObject capabilities;
    capabilities[QStringLiteral("studioVersion")] = QStringLiteral("1.4.0");
    capabilities[QStringLiteral("buildId")] = QStringLiteral("run12.abcd1234");
    if (legacyCapabilities) {
        capabilities[QStringLiteral("readableSchemaMin")] = 1;
        capabilities[QStringLiteral("readableSchemaMax")] = 2;
        capabilities[QStringLiteral("writableSchema")] = 2;
    } else {
        capabilities[QStringLiteral("capabilitiesVersion")] = 2;
    }

    const std::vector<TestEntry> entries = {
        {VNH::studioBinaryRelativePath().toUtf8(), binary},
        {QByteArrayLiteral("package-manifest.json"),
         QJsonDocument(manifest).toJson(QJsonDocument::Compact)},
        {QByteArrayLiteral("capabilities.json"),
         QJsonDocument(capabilities).toJson(QJsonDocument::Compact)},
    };
    if (!writeArchive(path, entries))
        return QString();
    return path;
}

bool writeFile(const QString& path, const QByteArray& content)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(content) == content.size();
}

class InstallRequestTest : public ::testing::Test
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
        keys = makeKeypair();
        packagePath = buildPackage(dir.filePath(QStringLiteral("studio.zip")));
        ASSERT_FALSE(packagePath.isEmpty());
    }

    QByteArray envelopeForArchive() const
    {
        QJsonObject payload = baseManifestPayload();
        payload[QStringLiteral("packageSize")] = QFileInfo(packagePath).size();
        payload[QStringLiteral("sha256")] =
            PackageReader::fileSha256Hex(packagePath);
        return signEnvelope(QJsonDocument(payload).toJson(QJsonDocument::Compact),
                            keys);
    }

    QTemporaryDir dir;
    std::unique_ptr<EnvGuard> machineGuard;
    std::unique_ptr<EnvGuard> versionsGuard;
    TestKeypair keys;
    QString packagePath;
};

}

TEST_F(InstallRequestTest, ManifestPathResolutionPrefersFullSuffix)
{
    EXPECT_TRUE(InstallRequest::manifestPathForPackage(packagePath).isEmpty());

    const QString swapped =
        dir.filePath(QStringLiteral("studio.manifest"));
    ASSERT_TRUE(writeFile(swapped, QByteArrayLiteral("x")));
    EXPECT_EQ(InstallRequest::manifestPathForPackage(packagePath), swapped);

    const QString sibling = packagePath + QStringLiteral(".manifest");
    ASSERT_TRUE(writeFile(sibling, QByteArrayLiteral("x")));
    EXPECT_EQ(InstallRequest::manifestPathForPackage(packagePath), sibling);
}

TEST_F(InstallRequestTest, PrepareVerifiesAndPlacesHandoff)
{
    const QString manifestPath = packagePath + QStringLiteral(".manifest");
    ASSERT_TRUE(writeFile(manifestPath, envelopeForArchive()));

    const auto preparation =
        InstallRequest::prepare(packagePath, manifestPath, keys.publicKey, kNow);
    ASSERT_TRUE(preparation.ok) << preparation.error.toStdString();
    EXPECT_EQ(preparation.version, QStringLiteral("1.4.0"));
    EXPECT_EQ(preparation.buildId, QStringLiteral("run12.abcd1234"));
    EXPECT_TRUE(
        VNH::HandoffArea::isValidTransactionId(preparation.transactionId));
    EXPECT_TRUE(QFile::exists(
        VNH::HandoffArea::packagePath(preparation.transactionId)));
    EXPECT_TRUE(QFile::exists(
        VNH::HandoffArea::manifestPath(preparation.transactionId)));
}

TEST_F(InstallRequestTest, PreparedTransactionInstallsEndToEnd)
{
    const QString manifestPath = packagePath + QStringLiteral(".manifest");
    ASSERT_TRUE(writeFile(manifestPath, envelopeForArchive()));

    const auto preparation =
        InstallRequest::prepare(packagePath, manifestPath, keys.publicKey, kNow);
    ASSERT_TRUE(preparation.ok) << preparation.error.toStdString();

    const auto outcome = VNH::InstallExecutor::run(preparation.transactionId,
                                                   keys.publicKey, kNow);
    ASSERT_TRUE(outcome.ok) << outcome.error.toStdString();
    EXPECT_TRUE(QFile::exists(outcome.installPath + QStringLiteral("/") +
                              VNH::studioBinaryRelativePath()));
}

TEST_F(InstallRequestTest, LegacyPackageInstallsRemainsAvailableAndCanBeRepaired)
{
    ASSERT_FALSE(buildPackage(packagePath, true).isEmpty());
    const QString manifestPath = packagePath + QStringLiteral(".manifest");
    ASSERT_TRUE(writeFile(manifestPath, envelopeForArchive()));
    const auto preparation =
        InstallRequest::prepare(packagePath, manifestPath, keys.publicKey, kNow);
    ASSERT_TRUE(preparation.ok) << preparation.error.toStdString();
    const auto installed = VNH::InstallExecutor::run(preparation.transactionId, keys.publicKey, kNow, true);
    ASSERT_TRUE(installed.ok) << installed.error.toStdString();
    const auto versions = VNH::VersionScanner::scanAvailable();
    ASSERT_EQ(versions.size(), 1u);
    EXPECT_EQ(versions.front().capabilitiesVersion, 0);
    EXPECT_TRUE(versions.front().available);
    QString error;
    ASSERT_TRUE(VNH::InstallIndex::setHealthState(installed.buildId, "NeedsRepair", &error));
    const auto repaired = VNH::RepairExecutor::run(installed.buildId, kNow);
    ASSERT_TRUE(repaired.ok) << repaired.error.toStdString();
    ASSERT_EQ(VNH::VersionScanner::scanAvailable().size(), 1u);
}

TEST_F(InstallRequestTest, BadSignatureLeavesNoHandoff)
{
    QJsonObject payload = baseManifestPayload();
    payload[QStringLiteral("packageSize")] = QFileInfo(packagePath).size();
    payload[QStringLiteral("sha256")] = PackageReader::fileSha256Hex(packagePath);
    const auto envelope = signEnvelope(
        QJsonDocument(payload).toJson(QJsonDocument::Compact), makeKeypair(0x24));
    const QString manifestPath = packagePath + QStringLiteral(".manifest");
    ASSERT_TRUE(writeFile(manifestPath, envelope));

    const auto preparation =
        InstallRequest::prepare(packagePath, manifestPath, keys.publicKey, kNow);
    EXPECT_FALSE(preparation.ok);
    const QDir handoffDir(VNH::HandoffArea::rootDir());
    EXPECT_TRUE(!handoffDir.exists() ||
                handoffDir.entryList(QDir::Files).isEmpty());
}

TEST_F(InstallRequestTest, PackageMismatchLeavesNoHandoff)
{
    QJsonObject payload = baseManifestPayload();
    payload[QStringLiteral("packageSize")] = QFileInfo(packagePath).size();
    payload[QStringLiteral("sha256")] = QString(64, QLatin1Char('b'));
    const QString manifestPath = packagePath + QStringLiteral(".manifest");
    ASSERT_TRUE(writeFile(
        manifestPath,
        signEnvelope(QJsonDocument(payload).toJson(QJsonDocument::Compact),
                     keys)));

    const auto preparation =
        InstallRequest::prepare(packagePath, manifestPath, keys.publicKey, kNow);
    EXPECT_FALSE(preparation.ok);
    EXPECT_TRUE(preparation.error.contains(QStringLiteral("SHA-256")));
}

TEST_F(InstallRequestTest, MissingManifestFails)
{
    const auto preparation = InstallRequest::prepare(
        packagePath, packagePath + QStringLiteral(".manifest"), keys.publicKey,
        kNow);
    EXPECT_FALSE(preparation.ok);
    EXPECT_TRUE(preparation.error.contains(QStringLiteral("manifest")));
}
