#include "Transactions/InstallExecutor.h"

#include "Packaging/PackageReader.h"
#include "Platform/PlatformPaths.h"
#include "Support/ManifestTestUtils.h"
#include "Transactions/HandoffArea.h"
#include "Transactions/InstallIndex.h"
#include "Transactions/RepairExecutor.h"
#include "Transactions/TransactionJournal.h"
#include "Transactions/TransactionDiagnostics.h"
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
#include <QLockFile>
#include <QTemporaryDir>

#include <memory>

using VNH::InstallExecutor;
using VNH::InstallIndex;
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

QString buildPackage(const QString& path)
{
    const QByteArray binary = QByteArrayLiteral("stub-binary");
    const QByteArray asset = QByteArrayLiteral("payload-data");

    QJsonArray files;
    QJsonObject binaryRecord;
    binaryRecord[QStringLiteral("path")] = VNH::studioBinaryRelativePath();
    binaryRecord[QStringLiteral("size")] = binary.size();
    binaryRecord[QStringLiteral("sha256")] = QString::fromLatin1(sha256Hex(binary));
    files.append(binaryRecord);
    QJsonObject assetRecord;
    assetRecord[QStringLiteral("path")] = QStringLiteral("data/asset.bin");
    assetRecord[QStringLiteral("size")] = asset.size();
    assetRecord[QStringLiteral("sha256")] = QString::fromLatin1(sha256Hex(asset));
    files.append(assetRecord);

    QJsonObject manifest;
    manifest[QStringLiteral("product")] = QStringLiteral("HarkenStudio");
    manifest[QStringLiteral("version")] = QStringLiteral("1.4.0");
    manifest[QStringLiteral("buildId")] = QStringLiteral("run12.abcd1234");
    manifest[QStringLiteral("channel")] = QStringLiteral("stable");
    manifest[QStringLiteral("files")] = files;

    QJsonObject capabilities;
    capabilities[QStringLiteral("studioVersion")] = QStringLiteral("1.4.0");
    capabilities[QStringLiteral("buildId")] = QStringLiteral("run12.abcd1234");
    capabilities[QStringLiteral("capabilitiesVersion")] = 2;

    const std::vector<TestEntry> entries = {
        {VNH::studioBinaryRelativePath().toUtf8(), binary},
        {QByteArrayLiteral("data/asset.bin"), asset},
        {QByteArrayLiteral("package-manifest.json"),
         QJsonDocument(manifest).toJson(QJsonDocument::Compact)},
        {QByteArrayLiteral("capabilities.json"),
         QJsonDocument(capabilities).toJson(QJsonDocument::Compact)},
    };
    if (!writeArchive(path, entries))
        return QString();
    return path;
}

class InstallExecutorTest : public ::testing::Test
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
        archivePath = buildPackage(dir.filePath(QStringLiteral("studio.zip")));
        ASSERT_FALSE(archivePath.isEmpty());
    }

    QJsonObject payloadForArchive() const
    {
        QJsonObject payload = baseManifestPayload();
        payload[QStringLiteral("packageSize")] = QFileInfo(archivePath).size();
        payload[QStringLiteral("sha256")] =
            PackageReader::fileSha256Hex(archivePath);
        return payload;
    }

    QString placeHandoff(const QJsonObject& payload)
    {
        const auto envelope = signEnvelope(
            QJsonDocument(payload).toJson(QJsonDocument::Compact), keys);
        const auto placement = VNH::HandoffArea::place(archivePath, envelope);
        EXPECT_TRUE(placement.ok) << placement.error.toStdString();
        return placement.transactionId;
    }

    QString targetPath() const
    {
        return VNH::studioVersionsRoot() +
               QStringLiteral("/1.4.0-run12.abcd1234");
    }

    QTemporaryDir dir;
    std::unique_ptr<EnvGuard> machineGuard;
    std::unique_ptr<EnvGuard> versionsGuard;
    TestKeypair keys;
    QString archivePath;
};

QByteArray readAll(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

}

TEST_F(InstallExecutorTest, HappyPathInstallsAndCommits)
{
    const QString transactionId = placeHandoff(payloadForArchive());

    const auto outcome =
        InstallExecutor::run(transactionId, keys.publicKey, kNow);
    ASSERT_TRUE(outcome.ok) << outcome.error.toStdString();
    EXPECT_EQ(outcome.exitCode(), 0);
    EXPECT_EQ(outcome.installPath, targetPath());
    EXPECT_EQ(outcome.buildId, QStringLiteral("run12.abcd1234"));

    EXPECT_EQ(readAll(targetPath() + QStringLiteral("/") +
                      VNH::studioBinaryRelativePath()),
              QByteArrayLiteral("stub-binary"));
    EXPECT_EQ(readAll(targetPath() + QStringLiteral("/data/asset.bin")),
              QByteArrayLiteral("payload-data"));

    EXPECT_FALSE(QFile::exists(VNH::HandoffArea::packagePath(transactionId)));
    EXPECT_FALSE(QFile::exists(VNH::HandoffArea::manifestPath(transactionId)));
    EXPECT_FALSE(QDir(VNH::machineDataDir() + QStringLiteral("/staging/") +
                      transactionId)
                     .exists());
    EXPECT_FALSE(QDir(VNH::studioVersionsRoot() + QStringLiteral("/.staging-") +
                      transactionId)
                     .exists());

    const auto index = InstallIndex::snapshot();
    ASSERT_TRUE(index.ok);
    EXPECT_EQ(index.lastReleaseSequence, 7);
    ASSERT_EQ(index.records.size(), 1u);
    EXPECT_EQ(index.records[0].healthState, QStringLiteral("Installed"));
    EXPECT_EQ(index.records[0].installPath, targetPath());

    const QByteArray journal =
        readAll(VNH::TransactionJournal::filePath());
    EXPECT_TRUE(journal.contains(QByteArrayLiteral("\"Installed\"")));
}

TEST_F(InstallExecutorTest, RepairRestoresTamperedInstallFromVerifiedCache)
{
    const QString transactionId = placeHandoff(payloadForArchive());
    ASSERT_TRUE(InstallExecutor::run(transactionId, keys.publicKey, kNow).ok);

    QFile damaged(targetPath() + QStringLiteral("/data/asset.bin"));
    ASSERT_TRUE(damaged.open(QIODevice::WriteOnly | QIODevice::Truncate));
    damaged.write("tampered");
    damaged.close();

    const EnvGuard trustAnchor("HARKEN_HUB_MANIFEST_PUBKEY",
                               QString::fromLatin1(keys.publicKey.toHex()));
    const auto repaired = VNH::RepairExecutor::run(
        QStringLiteral("run12.abcd1234"), kNow.addDays(90));
    ASSERT_TRUE(repaired.ok) << repaired.error.toStdString();
    EXPECT_EQ(readAll(targetPath() + QStringLiteral("/data/asset.bin")),
              QByteArrayLiteral("payload-data"));
    EXPECT_EQ(InstallIndex::snapshot().records.front().healthState,
              QStringLiteral("Installed"));
}

TEST_F(InstallExecutorTest, FailedRepairVerificationRestoresTheOriginalTreeAndKeepsTheMarker)
{
    const QString transactionId = placeHandoff(payloadForArchive());
    ASSERT_TRUE(InstallExecutor::run(transactionId, keys.publicKey, kNow).ok);
    auto record = InstallIndex::snapshot().records.front();
    record.capabilitiesHash = QString(64, QLatin1Char('f'));
    QString error;
    ASSERT_TRUE(InstallIndex::commitInstallation(record, 7, &error));
    QFile damaged(targetPath() + QStringLiteral("/data/asset.bin"));
    ASSERT_TRUE(damaged.open(QIODevice::WriteOnly | QIODevice::Truncate));
    damaged.write("original damaged payload");
    damaged.close();

    const EnvGuard trustAnchor("HARKEN_HUB_MANIFEST_PUBKEY",
                               QString::fromLatin1(keys.publicKey.toHex()));
    const auto repaired = VNH::RepairExecutor::run(record.buildId, kNow);
    EXPECT_FALSE(repaired.ok);
    EXPECT_EQ(repaired.failureCode, QStringLiteral("RepairVerificationFailed"));
    EXPECT_EQ(readAll(targetPath() + QStringLiteral("/data/asset.bin")),
              QByteArrayLiteral("original damaged payload"));
    EXPECT_EQ(InstallIndex::snapshot().records.front().healthState,
              QStringLiteral("NeedsRepair"));
    EXPECT_TRUE(QDir(VNH::studioVersionsRoot()).entryList(
        {QStringLiteral(".repair-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
}

TEST_F(InstallExecutorTest, TamperedManifestIsRejected)
{
    QJsonObject payload = payloadForArchive();
    const auto envelope =
        signEnvelope(QJsonDocument(payload).toJson(QJsonDocument::Compact),
                     makeKeypair(0x24));
    const auto placement = VNH::HandoffArea::place(archivePath, envelope);
    ASSERT_TRUE(placement.ok);

    const auto outcome =
        InstallExecutor::run(placement.transactionId, keys.publicKey, kNow);
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failureCode, QStringLiteral("ManifestRejected"));
    EXPECT_FALSE(QFileInfo::exists(targetPath()));
    EXPECT_FALSE(
        QFile::exists(VNH::HandoffArea::packagePath(placement.transactionId)));
}

TEST_F(InstallExecutorTest, ReplayedSequenceIsRejected)
{
    QString error;
    VNH::InstalledVersionRecord existing;
    existing.buildId = QStringLiteral("run99.99999999");
    existing.semanticVersion = QStringLiteral("2.0.0");
    existing.healthState = QStringLiteral("Installed");
    ASSERT_TRUE(InstallIndex::commitInstallation(existing, 100, &error));

    const QString transactionId = placeHandoff(payloadForArchive());
    const auto outcome =
        InstallExecutor::run(transactionId, keys.publicKey, kNow);
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failureCode, QStringLiteral("SequenceReplayed"));
    EXPECT_FALSE(QFileInfo::exists(targetPath()));
}

TEST_F(InstallExecutorTest, ExpiredManifestIsRejected)
{
    QJsonObject payload = payloadForArchive();
    payload[QStringLiteral("expiresAt")] = QStringLiteral("2026-07-23T11:00:00Z");
    const QString transactionId = placeHandoff(payload);

    const auto outcome =
        InstallExecutor::run(transactionId, keys.publicKey, kNow);
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failureCode, QStringLiteral("ManifestRejected"));
    EXPECT_TRUE(outcome.error.contains(QStringLiteral("expired")));
}

TEST_F(InstallExecutorTest, ExplicitInstallAcceptsOlderSequence)
{
    QString error;
    VNH::InstalledVersionRecord existing;
    existing.buildId = QStringLiteral("run99.99999999");
    existing.semanticVersion = QStringLiteral("2.0.0");
    existing.healthState = QStringLiteral("Installed");
    ASSERT_TRUE(InstallIndex::commitInstallation(existing, 100, &error));

    const QString transactionId = placeHandoff(payloadForArchive());
    const auto outcome =
        InstallExecutor::run(transactionId, keys.publicKey, kNow, true);
    ASSERT_TRUE(outcome.ok) << outcome.error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(targetPath()));

    const auto index = InstallIndex::snapshot();
    ASSERT_TRUE(index.ok);
    EXPECT_EQ(index.lastReleaseSequence, 100);
}

TEST_F(InstallExecutorTest, ExplicitInstallAcceptsExpiredManifest)
{
    QJsonObject payload = payloadForArchive();
    payload[QStringLiteral("expiresAt")] = QStringLiteral("2026-07-23T11:00:00Z");
    const QString transactionId = placeHandoff(payload);

    const auto outcome =
        InstallExecutor::run(transactionId, keys.publicKey, kNow, true);
    ASSERT_TRUE(outcome.ok) << outcome.error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(targetPath()));
}

TEST_F(InstallExecutorTest, ExplicitInstallStillRejectsBadSignature)
{
    QJsonObject payload = payloadForArchive();
    const auto envelope =
        signEnvelope(QJsonDocument(payload).toJson(QJsonDocument::Compact),
                     makeKeypair(0x24));
    const auto placement = VNH::HandoffArea::place(archivePath, envelope);
    ASSERT_TRUE(placement.ok);

    const auto outcome = InstallExecutor::run(placement.transactionId,
                                              keys.publicKey, kNow, true);
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failureCode, QStringLiteral("ManifestRejected"));
}

TEST_F(InstallExecutorTest, PackageHashMismatchIsRejected)
{
    QJsonObject payload = payloadForArchive();
    payload[QStringLiteral("sha256")] = QString(64, QLatin1Char('b'));
    const QString transactionId = placeHandoff(payload);

    const auto outcome =
        InstallExecutor::run(transactionId, keys.publicKey, kNow);
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failureCode, QStringLiteral("PackageRejected"));
    EXPECT_FALSE(QFileInfo::exists(targetPath()));
}

TEST_F(InstallExecutorTest, TargetConflictIsRejected)
{
    ASSERT_TRUE(QDir().mkpath(targetPath()));
    const QString transactionId = placeHandoff(payloadForArchive());

    const auto outcome =
        InstallExecutor::run(transactionId, keys.publicKey, kNow);
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failureCode, QStringLiteral("TargetConflict"));
    const auto summaries = VNH::TransactionDiagnostics::summarize();
    ASSERT_EQ(summaries.size(), 1u);
    EXPECT_EQ(summaries.front().transactionId, transactionId);
    EXPECT_EQ(summaries.front().failureDetail, outcome.error);
    const auto message = VNH::TransactionDiagnostics::failureMessage(transactionId);
    EXPECT_TRUE(message.contains(QStringLiteral("already exists")));
    EXPECT_TRUE(message.contains(targetPath()));
    EXPECT_TRUE(message.contains(QStringLiteral("TargetConflict")));
    EXPECT_TRUE(QFileInfo::exists(targetPath()));
}

TEST_F(InstallExecutorTest, MissingHandoffIsRejected)
{
    const auto transactionId = VNH::HandoffArea::newTransactionId();
    const auto outcome = InstallExecutor::run(transactionId, keys.publicKey, kNow);
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failureCode, QStringLiteral("HandoffRejected"));
    EXPECT_EQ(outcome.exitCode(), 2);
    EXPECT_TRUE(VNH::TransactionDiagnostics::failureMessage(transactionId).contains(outcome.error));
}

TEST_F(InstallExecutorTest, UnavailableMachineDirectoryReportsWithoutJournal)
{
    QFile blocker(VNH::machineDataDir());
    ASSERT_TRUE(blocker.open(QIODevice::WriteOnly));
    blocker.close();

    const auto transactionId = VNH::HandoffArea::newTransactionId();
    const auto outcome = InstallExecutor::run(transactionId, keys.publicKey, kNow);
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failureCode, QStringLiteral("MachineDataUnavailable"));
    EXPECT_EQ(outcome.exitCode(), VNH::kInstallMachineDataUnavailableExitCode);
    EXPECT_FALSE(QFileInfo::exists(VNH::TransactionJournal::filePath()));

    const auto message = VNH::TransactionDiagnostics::failureMessage(
        transactionId, outcome.exitCode());
    EXPECT_TRUE(message.contains(QStringLiteral("could not create its shared data folder")));
    EXPECT_TRUE(message.contains(VNH::machineDataDir()));
    EXPECT_TRUE(message.contains(QStringLiteral("permissions and available disk space")));
}

TEST_F(InstallExecutorTest, InvalidTransactionIdIsRejected)
{
    const auto outcome = InstallExecutor::run(QStringLiteral("../escape"),
                                              keys.publicKey, kNow);
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failureCode, QStringLiteral("InvalidTransaction"));
}

TEST_F(InstallExecutorTest, StaleStagingIsSweptUnderLock)
{
    const QString staleTx(32, QLatin1Char('e'));
    const QString machineStale =
        VNH::machineDataDir() + QStringLiteral("/staging/") + staleTx;
    const QString extractStale = VNH::studioVersionsRoot() +
                                 QStringLiteral("/.staging-") + staleTx;
    ASSERT_TRUE(QDir().mkpath(machineStale));
    ASSERT_TRUE(QDir().mkpath(extractStale));

    const QString transactionId = placeHandoff(payloadForArchive());
    const auto outcome =
        InstallExecutor::run(transactionId, keys.publicKey, kNow);
    ASSERT_TRUE(outcome.ok) << outcome.error.toStdString();

    EXPECT_FALSE(QDir(machineStale).exists());
    EXPECT_FALSE(QDir(extractStale).exists());
}

TEST_F(InstallExecutorTest, AuthenticodeGateBlocksWhenRequired)
{
    const EnvGuard gate("HARKEN_HUB_REQUIRE_AUTHENTICODE",
                        QStringLiteral("1"));
    const QString transactionId = placeHandoff(payloadForArchive());

    const auto outcome =
        InstallExecutor::run(transactionId, keys.publicKey, kNow);
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failureCode, QStringLiteral("BinaryTrustRejected"));
    EXPECT_FALSE(QFileInfo::exists(targetPath()));
    EXPECT_EQ(InstallIndex::snapshot().lastReleaseSequence, 0);
}

TEST_F(InstallExecutorTest, CorruptIndexBlocksInstall)
{
    ASSERT_TRUE(QDir().mkpath(VNH::machineDataDir()));
    QFile file(InstallIndex::filePath());
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(QByteArrayLiteral("not json"));
    file.close();

    const QString transactionId = placeHandoff(payloadForArchive());
    const auto outcome =
        InstallExecutor::run(transactionId, keys.publicKey, kNow);
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failureCode, QStringLiteral("IndexCorrupted"));
    EXPECT_FALSE(QFileInfo::exists(targetPath()));
}

TEST_F(InstallExecutorTest, ConcurrentInstallIsRejected)
{
    ASSERT_TRUE(QDir().mkpath(VNH::machineDataDir()));
    QLockFile heldLock(VNH::machineDataDir() + QStringLiteral("/install.lock"));
    heldLock.setStaleLockTime(0);
    ASSERT_TRUE(heldLock.tryLock(0));

    const QString transactionId = placeHandoff(payloadForArchive());
    const auto outcome =
        InstallExecutor::run(transactionId, keys.publicKey, kNow);
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failureCode, QStringLiteral("InstallBusy"));
    EXPECT_TRUE(
        QFile::exists(VNH::HandoffArea::packagePath(transactionId)));
}
