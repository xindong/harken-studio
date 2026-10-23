#include "Update/UpdateBridgeConsumer.h"

#include "Packaging/PackageReader.h"
#include "Platform/PlatformPaths.h"
#include "Support/ManifestTestUtils.h"
#include "Transactions/HandoffArea.h"
#include "Transactions/InstallIndex.h"
#include "Transactions/InstallExecutor.h"
#include "Transactions/TransactionJournal.h"
#include "Update/UpdateQueue.h"
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

using VNH::HandoffArea;
using VNH::InstallIndex;
using VNH::PackageReader;
using VNH::UpdateBridgeConsumer;
using VNH::UpdateQueue;
using HarkenHubTest::baseManifestPayload;
using HarkenHubTest::makeKeypair;
using HarkenHubTest::signEnvelope;
using HarkenHubTest::TestEntry;
using HarkenHubTest::TestKeypair;
using HarkenHubTest::writeArchive;

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

QByteArray sha256Hex(const QByteArray& data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

QString buildStudioPackage(const QString& path)
{
    const QByteArray binary = QByteArrayLiteral("stub-binary");

    QJsonArray files;
    QJsonObject binaryRecord;
    binaryRecord[QStringLiteral("path")] = VNH::studioBinaryRelativePath();
    binaryRecord[QStringLiteral("size")] = binary.size();
    binaryRecord[QStringLiteral("sha256")] =
        QString::fromLatin1(sha256Hex(binary));
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
    capabilities[QStringLiteral("capabilitiesVersion")] = 2;

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

class UpdateBridgeConsumerTest : public ::testing::Test
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
        hooks.runHubInstaller = [this](const QString& installerPath,
                                            const QString& logPath) {
            installerRuns.append(installerPath);
            installerLogPaths.append(logPath);
            QFile log(logPath);
            if (log.open(QIODevice::WriteOnly))
                log.write(QByteArrayLiteral("fake-install-log"));
            return installerExitCode;
        };
        hooks.restartHub = [this](const QStringList& arguments,
                                       const QString& userId, QString*) {
            restartRequests.append(arguments.join(QLatin1Char(' ')));
            restartUserIds.append(userId);
            return true;
        };
        hooks.runningHubProcesses = [this]() {
            return runningProcesses;
        };
        hooks.wait = [this](qint64) { runningProcesses.clear(); };
        hooks.hubQuietTimeoutMs = 2000;
    }

    QString queueStudioTransaction()
    {
        const QString archivePath =
            buildStudioPackage(dir.filePath(QStringLiteral("studio.zip")));
        EXPECT_FALSE(archivePath.isEmpty());
        QJsonObject payload = baseManifestPayload();
        payload[QStringLiteral("packageSize")] =
            QFileInfo(archivePath).size();
        payload[QStringLiteral("sha256")] =
            PackageReader::fileSha256Hex(archivePath);
        const auto envelope = signEnvelope(
            QJsonDocument(payload).toJson(QJsonDocument::Compact), keys);
        const auto placement = HandoffArea::place(archivePath, envelope);
        EXPECT_TRUE(placement.ok) << placement.error.toStdString();
        QString error;
        EXPECT_TRUE(
            UpdateQueue::markStudioInstall(placement.transactionId, &error))
            << error.toStdString();
        return placement.transactionId;
    }

    QString queueHubTransaction(const QString& restartMode,
                                     bool corruptHash = false)
    {
        const QString installerPath =
            dir.filePath(QStringLiteral("HarkenHubSetup.exe"));
        QFile installer(installerPath);
        EXPECT_TRUE(installer.open(QIODevice::WriteOnly));
        installer.write(QByteArrayLiteral("stub-msi-bytes"));
        installer.close();

        QJsonObject payload = baseManifestPayload();
        payload[QStringLiteral("product")] = QStringLiteral("HarkenHub");
        payload[QStringLiteral("version")] = QStringLiteral("0.2.0");
        payload[QStringLiteral("buildId")] = QStringLiteral("run4.dddd4444");
        payload[QStringLiteral("packageSize")] =
            QFileInfo(installerPath).size();
        payload[QStringLiteral("sha256")] =
            corruptHash ? QString(64, QLatin1Char('b'))
                        : PackageReader::fileSha256Hex(installerPath);
        const auto envelope = signEnvelope(
            QJsonDocument(payload).toJson(QJsonDocument::Compact), keys);
        const auto placement = HandoffArea::place(installerPath, envelope);
        EXPECT_TRUE(placement.ok) << placement.error.toStdString();
        QString error;
        EXPECT_TRUE(UpdateQueue::markHubInstall(
            placement.transactionId, restartMode,
            QStringLiteral("S-1-5-21-test-user"), &error))
            << error.toStdString();
        return placement.transactionId;
    }

    QTemporaryDir dir;
    std::unique_ptr<EnvGuard> machineGuard;
    std::unique_ptr<EnvGuard> versionsGuard;
    TestKeypair keys;
    UpdateBridgeConsumer::Hooks hooks;
    QStringList installerRuns;
    QStringList installerLogPaths;
    QStringList restartRequests;
    QStringList restartUserIds;
    QStringList runningProcesses;
    int installerExitCode = 0;
};

}

TEST_F(UpdateBridgeConsumerTest, ConsumesQueuedStudioInstall)
{
    const QString transactionId = queueStudioTransaction();

    const auto report =
        UpdateBridgeConsumer::run(keys.publicKey, kNow, hooks);
    EXPECT_EQ(report.installed, 1);
    EXPECT_EQ(report.failed, 0);
    EXPECT_TRUE(installerRuns.isEmpty());

    EXPECT_FALSE(QFile::exists(HandoffArea::queuePath(transactionId)));
    const auto index = InstallIndex::snapshot();
    ASSERT_TRUE(index.ok);
    ASSERT_EQ(index.records.size(), 1u);
    EXPECT_EQ(index.records[0].buildId, QStringLiteral("run12.abcd1234"));
}

TEST_F(UpdateBridgeConsumerTest, StudioInstallCannotReclaimAnActiveHubInstaller)
{
    queueHubTransaction(QStringLiteral("gui"));
    bool studioInstalled = false;
    hooks.runHubInstaller = [&](const QString& installerPath, const QString&) {
        EXPECT_TRUE(QFileInfo::exists(installerPath));
        const QByteArray originalHash = PackageReader::fileSha256Hex(installerPath).toUtf8();
        const auto installed = VNH::InstallExecutor::run(queueStudioTransaction(), keys.publicKey, kNow);
        studioInstalled = installed.ok;
        EXPECT_TRUE(installed.ok) << installed.error.toStdString();
        EXPECT_TRUE(QFileInfo::exists(installerPath));
        EXPECT_EQ(PackageReader::fileSha256Hex(installerPath).toUtf8(), originalHash);
        return 0;
    };
    const auto report = UpdateBridgeConsumer::run(keys.publicKey, kNow, hooks);
    EXPECT_TRUE(studioInstalled);
    EXPECT_EQ(report.installed, 1);
    EXPECT_EQ(report.failed, 0);
}

TEST_F(UpdateBridgeConsumerTest, DiscardsOrphanedQueueMarker)
{
    const QString transactionId = HandoffArea::newTransactionId();
    ASSERT_TRUE(QDir().mkpath(HandoffArea::rootDir()));
    QString error;
    ASSERT_TRUE(UpdateQueue::markStudioInstall(transactionId, &error));

    const auto report =
        UpdateBridgeConsumer::run(keys.publicKey, kNow, hooks);
    EXPECT_EQ(report.failed, 1);
    EXPECT_FALSE(QFile::exists(HandoffArea::queuePath(transactionId)));
}

TEST_F(UpdateBridgeConsumerTest, PersistsHubInstallLogWithRotation)
{
    const QString logsDir =
        VNH::machineDataDir() + QStringLiteral("/logs");
    ASSERT_TRUE(QDir().mkpath(logsDir));
    for (int index = 0; index < 6; ++index) {
        QFile seed(logsDir +
                   QStringLiteral("/install-seed%1.log").arg(index));
        ASSERT_TRUE(seed.open(QIODevice::WriteOnly));
        seed.write(QByteArrayLiteral("seed"));
    }

    const QString transactionId =
        queueHubTransaction(QStringLiteral("gui"));
    const auto report =
        UpdateBridgeConsumer::run(keys.publicKey, kNow, hooks);
    EXPECT_EQ(report.installed, 1);
    EXPECT_EQ(report.failed, 0);

    ASSERT_EQ(installerLogPaths.size(), 1);
    EXPECT_EQ(installerLogPaths.first(),
              logsDir + QStringLiteral("/install-") + transactionId +
                  QStringLiteral(".log"));
    const QStringList remaining = QDir(logsDir).entryList(
        {QStringLiteral("install-*.log")}, QDir::Files);
    EXPECT_EQ(remaining.size(), 5);
    EXPECT_TRUE(remaining.contains(QStringLiteral("install-") +
                                   transactionId + QStringLiteral(".log")));
}

TEST_F(UpdateBridgeConsumerTest, FallsBackToStagingLogWhenLogsDirUnavailable)
{
    ASSERT_TRUE(QDir().mkpath(VNH::machineDataDir()));
    QFile blocker(VNH::machineDataDir() + QStringLiteral("/logs"));
    ASSERT_TRUE(blocker.open(QIODevice::WriteOnly));
    blocker.write(QByteArrayLiteral("x"));
    blocker.close();

    queueHubTransaction(QStringLiteral("gui"));
    const auto report =
        UpdateBridgeConsumer::run(keys.publicKey, kNow, hooks);
    EXPECT_EQ(report.installed, 1);
    EXPECT_EQ(report.failed, 0);

    ASSERT_EQ(installerLogPaths.size(), 1);
    EXPECT_TRUE(installerLogPaths.first().endsWith(
        QStringLiteral("/install.log")));
    EXPECT_TRUE(
        installerLogPaths.first().contains(QStringLiteral("/hub-update-staging/")));
}

TEST_F(UpdateBridgeConsumerTest, InstallsQueuedHubUpdateAndRestartsGui)
{
    const QString transactionId =
        queueHubTransaction(QStringLiteral("gui"));

    const auto report =
        UpdateBridgeConsumer::run(keys.publicKey, kNow, hooks);
    EXPECT_EQ(report.installed, 1);
    EXPECT_EQ(report.failed, 0);
    ASSERT_EQ(installerRuns.size(), 1);
    ASSERT_EQ(restartRequests.size(), 1);
    EXPECT_EQ(restartRequests[0], QStringLiteral("--after-update"));
    ASSERT_EQ(restartUserIds.size(), 1);
    EXPECT_EQ(restartUserIds[0], QStringLiteral("S-1-5-21-test-user"));

    EXPECT_FALSE(QFile::exists(HandoffArea::packagePath(transactionId)));
    EXPECT_FALSE(QFile::exists(HandoffArea::queuePath(transactionId)));
    QFile journal(VNH::TransactionJournal::filePath());
    ASSERT_TRUE(journal.open(QIODevice::ReadOnly));
    EXPECT_TRUE(journal.readAll().contains(QByteArrayLiteral("\"Installed\"")));
}

TEST_F(UpdateBridgeConsumerTest, RestartsBackgroundModeWithBackgroundFlag)
{
    queueHubTransaction(QStringLiteral("background"));

    UpdateBridgeConsumer::run(keys.publicKey, kNow, hooks);
    ASSERT_EQ(restartRequests.size(), 1);
    EXPECT_EQ(restartRequests[0], QStringLiteral("--background"));
}

TEST_F(UpdateBridgeConsumerTest, DefersWhileHubProcessesAreRunning)
{
    const QString transactionId =
        queueHubTransaction(QStringLiteral("gui"));
    runningProcesses = {QStringLiteral("HarkenHub.exe")};
    hooks.wait = [](qint64) {};
    hooks.hubQuietTimeoutMs = 0;

    const auto report =
        UpdateBridgeConsumer::run(keys.publicKey, kNow, hooks);
    EXPECT_EQ(report.deferred, 1);
    EXPECT_EQ(report.installed, 0);
    EXPECT_TRUE(installerRuns.isEmpty());
    EXPECT_TRUE(QFile::exists(HandoffArea::packagePath(transactionId)));
    EXPECT_TRUE(QFile::exists(HandoffArea::queuePath(transactionId)));
}

TEST_F(UpdateBridgeConsumerTest, LandsAfterHubProcessesExit)
{
    queueHubTransaction(QStringLiteral("gui"));
    runningProcesses = {QStringLiteral("HarkenHub.exe")};

    const auto report =
        UpdateBridgeConsumer::run(keys.publicKey, kNow, hooks);
    EXPECT_EQ(report.installed, 1);
    EXPECT_EQ(report.deferred, 0);
}

TEST_F(UpdateBridgeConsumerTest, RejectsTamperedHubInstaller)
{
    const QString transactionId =
        queueHubTransaction(QStringLiteral("gui"), true);

    const auto report =
        UpdateBridgeConsumer::run(keys.publicKey, kNow, hooks);
    EXPECT_EQ(report.failed, 1);
    EXPECT_TRUE(installerRuns.isEmpty());
    EXPECT_TRUE(restartRequests.isEmpty());
    EXPECT_FALSE(QFile::exists(HandoffArea::packagePath(transactionId)));
    QFile journal(VNH::TransactionJournal::filePath());
    ASSERT_TRUE(journal.open(QIODevice::ReadOnly));
    EXPECT_TRUE(
        journal.readAll().contains(QByteArrayLiteral("PackageRejected")));
}

TEST_F(UpdateBridgeConsumerTest, RetriesRestartAndJournalsPersistentFailure)
{
    queueHubTransaction(QStringLiteral("gui"));
    int restartAttempts = 0;
    hooks.restartHub = [&restartAttempts](const QStringList&,
                                               const QString&,
                                               QString* error) {
        ++restartAttempts;
        *error = QStringLiteral("No active console session");
        return false;
    };

    const auto report =
        UpdateBridgeConsumer::run(keys.publicKey, kNow, hooks);
    EXPECT_EQ(report.installed, 1);
    EXPECT_EQ(restartAttempts, 3);
    EXPECT_EQ(report.failed, 1);
    QFile journal(VNH::TransactionJournal::filePath());
    ASSERT_TRUE(journal.open(QIODevice::ReadOnly));
    const QByteArray entries = journal.readAll();
    EXPECT_TRUE(entries.contains(QByteArrayLiteral("RestartFailed")));
    EXPECT_TRUE(entries.contains(QByteArrayLiteral("SessionLaunchFailed")));
}

TEST_F(UpdateBridgeConsumerTest, RetriesUntilStartupIsConfirmed)
{
    queueHubTransaction(QStringLiteral("gui"));
    int attempts = 0;
    hooks.restartHub = [&attempts](const QStringList&, const QString&, QString* error) {
        if (++attempts == 2)
            return true;
        *error = QStringLiteral("Hub exited before startup confirmation");
        return false;
    };
    const auto report = UpdateBridgeConsumer::run(keys.publicKey, kNow, hooks);
    EXPECT_EQ(attempts, 2);
    EXPECT_EQ(report.installed, 1);
    EXPECT_EQ(report.failed, 0);
    QFile journal(VNH::TransactionJournal::filePath());
    ASSERT_TRUE(journal.open(QIODevice::ReadOnly));
    const QByteArray entries = journal.readAll();
    EXPECT_TRUE(entries.contains(QByteArrayLiteral("\"Installed\"")));
    EXPECT_FALSE(entries.contains(QByteArrayLiteral("RestartFailed")));
}

TEST_F(UpdateBridgeConsumerTest, ReportsInstallerFailureWithoutRestart)
{
    queueHubTransaction(QStringLiteral("gui"));
    installerExitCode = 1603;

    const auto report =
        UpdateBridgeConsumer::run(keys.publicKey, kNow, hooks);
    EXPECT_EQ(report.failed, 1);
    EXPECT_TRUE(restartRequests.isEmpty());
    QFile journal(VNH::TransactionJournal::filePath());
    ASSERT_TRUE(journal.open(QIODevice::ReadOnly));
    EXPECT_TRUE(
        journal.readAll().contains(QByteArrayLiteral("BridgeInstallFailed")));
}

TEST_F(UpdateBridgeConsumerTest, TreatsRebootRequiredInstallAsSuccess)
{
    queueHubTransaction(QStringLiteral("gui"));
    installerExitCode = 3010;

    const auto report =
        UpdateBridgeConsumer::run(keys.publicKey, kNow, hooks);
    EXPECT_EQ(report.installed, 1);
    EXPECT_EQ(report.failed, 0);
    ASSERT_EQ(restartRequests.size(), 1);
    QFile journal(VNH::TransactionJournal::filePath());
    ASSERT_TRUE(journal.open(QIODevice::ReadOnly));
    const QByteArray entries = journal.readAll();
    EXPECT_TRUE(entries.contains(QByteArrayLiteral("\"Installed\"")));
    EXPECT_FALSE(entries.contains(QByteArrayLiteral("BridgeInstallFailed")));
}
