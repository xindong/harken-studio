#include "Update/HubSelfUpdate.h"

#include "Support/ManifestTestUtils.h"
#include "Support/RangeHttpServer.h"
#include "Transactions/HandoffArea.h"
#include "Update/UpdateQueue.h"
#include "Update/UpdateStateStore.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

using VNH::HandoffArea;
using VNH::HubSelfUpdate;
using VNH::UpdateQueue;
using VNH::UpdateStateRecord;
using VNH::UpdateStateStore;

namespace {

const QDateTime kNow =
    QDateTime::fromString(QStringLiteral("2026-07-24T12:00:00Z"), Qt::ISODate);

class HubSelfUpdateTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        qputenv("HARKEN_HUB_USER_ROOT", dir.path().toUtf8());
        qputenv("HARKEN_HUB_MACHINE_ROOT",
                dir.filePath(QStringLiteral("machine")).toUtf8());
    }
    void TearDown() override
    {
        qunsetenv("HARKEN_HUB_USER_ROOT");
        qunsetenv("HARKEN_HUB_MACHINE_ROOT");
        qunsetenv("HARKEN_HUB_SELFUPDATE_DISABLE");
        qunsetenv("HARKEN_HUB_BRIDGE_COMMAND");
        qunsetenv("HARKEN_HUB_BRIDGE_DISABLE");
    }

    QString writeInstaller(const QByteArray& content,
                           const QString& name = QStringLiteral("setup.exe"))
    {
        const QString path = dir.filePath(name);
        QFile file(path);
        file.open(QIODevice::WriteOnly);
        file.write(content);
        file.close();
        return path;
    }

    UpdateStateRecord readyBridgeRecord(const QByteArray& installerContent)
    {
        const QString installerPath = writeInstaller(
            installerContent, QStringLiteral("HarkenHubSetup.exe"));

        QJsonObject payload = HarkenHubTest::baseManifestPayload();
        payload[QStringLiteral("product")] = QStringLiteral("HarkenHub");
        payload[QStringLiteral("version")] = QStringLiteral("0.2.0");
        payload[QStringLiteral("buildId")] = QStringLiteral("run4.dddd4444");
        payload[QStringLiteral("packageSize")] = installerContent.size();
        payload[QStringLiteral("sha256")] = QString::fromLatin1(
            QCryptographicHash::hash(installerContent,
                                     QCryptographicHash::Sha256)
                .toHex());
        const auto envelope = HarkenHubTest::signEnvelope(
            QJsonDocument(payload).toJson(QJsonDocument::Compact),
            HarkenHubTest::makeKeypair());
        const QString envelopePath =
            dir.filePath(QStringLiteral("run4.dddd4444.manifest"));
        QFile envelopeFile(envelopePath);
        EXPECT_TRUE(envelopeFile.open(QIODevice::WriteOnly));
        envelopeFile.write(envelope);
        envelopeFile.close();

        UpdateStateRecord record;
        record.product = QStringLiteral("HarkenHub");
        record.state = QStringLiteral("AwaitingRestart");
        record.version = QStringLiteral("0.2.0");
        record.buildId = QStringLiteral("run4.dddd4444");
        record.downloadPath = installerPath;
        record.manifestEnvelopePath = envelopePath;
        record.expectedSha256 = QString::fromLatin1(
            QCryptographicHash::hash(installerContent,
                                     QCryptographicHash::Sha256)
                .toHex());
        record.packageSize = installerContent.size();
        return record;
    }

    QTemporaryDir dir;
};

TEST_F(HubSelfUpdateTest, VerifiesInstallerHashAndSize)
{
    const QByteArray content = QByteArrayLiteral("installer-bytes");
    const QString path = writeInstaller(content);
    const QString sha = QString::fromLatin1(
        QCryptographicHash::hash(content, QCryptographicHash::Sha256)
            .toHex());

    QString error;
    EXPECT_TRUE(HubSelfUpdate::verifyInstaller(path, sha, content.size(),
                                                    &error))
        << error.toStdString();
    EXPECT_FALSE(HubSelfUpdate::verifyInstaller(
        path, QString(64, QLatin1Char('b')), content.size(), &error));
    EXPECT_FALSE(HubSelfUpdate::verifyInstaller(path, sha,
                                                     content.size() + 1,
                                                     &error));
    EXPECT_FALSE(HubSelfUpdate::verifyInstaller(
        dir.filePath(QStringLiteral("missing.exe")), sha, content.size(),
        &error));
}

TEST_F(HubSelfUpdateTest, InstallerRejectedWhenAuthenticodeRequired)
{
    const QByteArray content = QByteArrayLiteral("installer-bytes");
    const QString path = writeInstaller(content);
    const QString sha = QString::fromLatin1(
        QCryptographicHash::hash(content, QCryptographicHash::Sha256)
            .toHex());

    qputenv("HARKEN_HUB_REQUIRE_AUTHENTICODE", "1");
    QString error;
    EXPECT_FALSE(HubSelfUpdate::verifyInstaller(path, sha,
                                                     content.size(), &error));
    qunsetenv("HARKEN_HUB_REQUIRE_AUTHENTICODE");
}

TEST_F(HubSelfUpdateTest, BackupCopiesUserData)
{
    {
        QFile db(dir.filePath(QStringLiteral("projects.db")));
        db.open(QIODevice::WriteOnly);
        db.write(QByteArrayLiteral("db-bytes"));
    }
    QString error;
    ASSERT_TRUE(HubSelfUpdate::backupUserData(kNow, &error))
        << error.toStdString();

    const QDir backupRoot(HubSelfUpdate::backupRootDir());
    const auto entries =
        backupRoot.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_TRUE(QFile::exists(backupRoot.filePath(
        entries.first() + QStringLiteral("/projects.db"))));
}

TEST_F(HubSelfUpdateTest, ApproveRequiresAwaitingState)
{
    HubSelfUpdate update;
    QString error;
    EXPECT_FALSE(update.approve(&error, kNow));
}

TEST_F(HubSelfUpdateTest, ApproveHonorsExplicitDisableSwitch)
{
    qputenv("HARKEN_HUB_SELFUPDATE_DISABLE", "1");
    const QByteArray content = QByteArrayLiteral("installer-bytes");
    const QString path = writeInstaller(content);

    UpdateStateRecord record;
    record.product = QStringLiteral("HarkenHub");
    record.state = QStringLiteral("AwaitingUserApproval");
    record.version = QStringLiteral("0.2.0");
    record.buildId = QStringLiteral("run4.dddd4444");
    record.downloadPath = path;
    record.expectedSha256 = QString::fromLatin1(
        QCryptographicHash::hash(content, QCryptographicHash::Sha256)
            .toHex());
    record.packageSize = content.size();
    ASSERT_TRUE(UpdateStateStore::save(record, kNow));

    HubSelfUpdate update;
    QString error;
    EXPECT_FALSE(update.approve(&error, kNow));
    EXPECT_TRUE(error.contains(QStringLiteral("disabled")))
        << error.toStdString();
    EXPECT_EQ(UpdateStateStore::load(QStringLiteral("HarkenHub")).state,
              QStringLiteral("AwaitingUserApproval"));
}

TEST_F(HubSelfUpdateTest, FinalizeAfterRestartClearsCompletedUpdate)
{
    const QString path = writeInstaller(QByteArrayLiteral("installer-bytes"));

    UpdateStateRecord record;
    record.product = QStringLiteral("HarkenHub");
    record.state = QStringLiteral("AwaitingElevation");
    record.version = QStringLiteral("0.2.0");
    record.buildId = QStringLiteral("run4.dddd4444");
    record.downloadPath = path;
    ASSERT_TRUE(UpdateStateStore::save(record, kNow));

    EXPECT_FALSE(
        HubSelfUpdate::finalizeAfterRestart(QStringLiteral("0.1.0")));
    EXPECT_TRUE(QFile::exists(path));
    EXPECT_EQ(UpdateStateStore::load(QStringLiteral("HarkenHub")).state,
              QStringLiteral("AwaitingElevation"));

    EXPECT_TRUE(
        HubSelfUpdate::finalizeAfterRestart(QStringLiteral("0.2.0")));
    EXPECT_FALSE(QFile::exists(path));
    EXPECT_FALSE(
        UpdateStateStore::load(QStringLiteral("HarkenHub")).found);
}

TEST_F(HubSelfUpdateTest, FinalizeIgnoresOtherStates)
{
    UpdateStateRecord record;
    record.product = QStringLiteral("HarkenHub");
    record.state = QStringLiteral("AwaitingUserApproval");
    record.version = QStringLiteral("0.2.0");
    record.buildId = QStringLiteral("run4.dddd4444");
    ASSERT_TRUE(UpdateStateStore::save(record, kNow));

    EXPECT_FALSE(
        HubSelfUpdate::finalizeAfterRestart(QStringLiteral("0.2.0")));
    EXPECT_EQ(UpdateStateStore::load(QStringLiteral("HarkenHub")).state,
              QStringLiteral("AwaitingUserApproval"));
}

TEST_F(HubSelfUpdateTest, RecoverStaleElevationRearmsApproval)
{
    qputenv("HARKEN_HUB_BRIDGE_DISABLE", "1");
    UpdateStateRecord record;
    record.product = QStringLiteral("HarkenHub");
    record.state = QStringLiteral("AwaitingElevation");
    record.version = QStringLiteral("0.2.0");
    record.buildId = QStringLiteral("run4.dddd4444");
    ASSERT_TRUE(UpdateStateStore::save(record, kNow));

    EXPECT_FALSE(HubSelfUpdate::recoverStaleElevation(
        QStringLiteral("0.1.0"), kNow.addSecs(60)));
    EXPECT_EQ(UpdateStateStore::load(QStringLiteral("HarkenHub")).state,
              QStringLiteral("AwaitingElevation"));

    EXPECT_TRUE(HubSelfUpdate::recoverStaleElevation(
        QStringLiteral("0.1.0"), kNow.addSecs(16 * 60)));
    EXPECT_EQ(UpdateStateStore::load(QStringLiteral("HarkenHub")).state,
              QStringLiteral("AwaitingUserApproval"));
}

TEST_F(HubSelfUpdateTest, RecoverIgnoresCompletedOrOtherStates)
{
    UpdateStateRecord record;
    record.product = QStringLiteral("HarkenHub");
    record.state = QStringLiteral("AwaitingElevation");
    record.version = QStringLiteral("0.2.0");
    record.buildId = QStringLiteral("run4.dddd4444");
    ASSERT_TRUE(UpdateStateStore::save(record, kNow));

    EXPECT_FALSE(HubSelfUpdate::recoverStaleElevation(
        QStringLiteral("0.2.0"), kNow.addSecs(16 * 60)));
    EXPECT_EQ(UpdateStateStore::load(QStringLiteral("HarkenHub")).state,
              QStringLiteral("AwaitingElevation"));

    record.state = QStringLiteral("AwaitingUserApproval");
    ASSERT_TRUE(UpdateStateStore::save(record, kNow));
    EXPECT_FALSE(HubSelfUpdate::recoverStaleElevation(
        QStringLiteral("0.1.0"), kNow.addSecs(16 * 60)));
    EXPECT_EQ(UpdateStateStore::load(QStringLiteral("HarkenHub")).state,
              QStringLiteral("AwaitingUserApproval"));
}

TEST_F(HubSelfUpdateTest, ApproveViaBridgeQueuesHandoffAndDispatches)
{
    qputenv("HARKEN_HUB_BRIDGE_COMMAND", "noop");
    const QByteArray content = QByteArrayLiteral("msi-bytes");
    ASSERT_TRUE(UpdateStateStore::save(readyBridgeRecord(content), kNow));

    HubSelfUpdate update;
    QString error;
    ASSERT_TRUE(update.approveViaBridge(QStringLiteral("gui"), &error, kNow))
        << error.toStdString();

    const UpdateStateRecord state =
        UpdateStateStore::load(QStringLiteral("HarkenHub"));
    EXPECT_EQ(state.state, QStringLiteral("AwaitingBridge"));
    ASSERT_FALSE(state.transactionId.isEmpty());
    EXPECT_TRUE(QFile::exists(HandoffArea::packagePath(state.transactionId)));
    EXPECT_TRUE(QFile::exists(HandoffArea::manifestPath(state.transactionId)));
    const auto entries = UpdateQueue::pendingEntries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].product, QStringLiteral("HarkenHub"));
    EXPECT_EQ(entries[0].restartMode, QStringLiteral("gui"));

    const QDir backupRoot(HubSelfUpdate::backupRootDir());
    EXPECT_FALSE(
        backupRoot.entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
}

TEST_F(HubSelfUpdateTest, DownloadOwnershipSurvivesImmediateBridgeHandoff)
{
    qputenv("HARKEN_HUB_BRIDGE_COMMAND", "noop");
    const QByteArray content = QByteArrayLiteral("installer-bytes");
    const auto ready = readyBridgeRecord(content);
    QFile envelope(ready.manifestEnvelopePath);
    ASSERT_TRUE(envelope.open(QIODevice::ReadOnly));

    HarkenHubTest::RangeHttpServer server;
    ASSERT_TRUE(server.listen());
    server.setPayload(content, QByteArrayLiteral("hub-build"));
    HubSelfUpdate::Options options;
    options.serverBaseUrl = server.baseUrl();
    options.currentVersion = QStringLiteral("0.1.0");
    options.downloadDir = dir.filePath(QStringLiteral("hub-updates"));
    HubSelfUpdate update;
    HubSelfUpdate contender;
    update.configure(options);
    contender.configure(options);

    VNH::UpdateCheckResult result;
    result.ok = true;
    result.manifest.product = ready.product;
    result.manifest.version = ready.version;
    result.manifest.buildId = ready.buildId;
    result.manifest.packageSize = ready.packageSize;
    result.manifest.sha256Hex = ready.expectedSha256;
    result.envelope = envelope.readAll();
    result.objectPath = QStringLiteral("release/hub/windows-x64/0.2.0/run4.dddd4444/HarkenHubSetup.exe");
    int approvals = 0;
    QObject::connect(&update, &HubSelfUpdate::stateChanged, &update,
        [&](const UpdateStateRecord& record) {
            if (record.state != QStringLiteral("AwaitingRestart"))
                return;
            ++approvals;
            EXPECT_FALSE(VNH::DownloadLease::acquire(record.downloadPath));
            EXPECT_FALSE(VNH::DownloadLease::acquire(record.manifestEnvelopePath));
            QString error;
            EXPECT_TRUE(update.approveViaBridge(QStringLiteral("background"), &error, kNow))
                << error.toStdString();
        });
    update.handleCheckResult(result, kNow);
    const auto downloading = update.currentState();
    ASSERT_EQ(downloading.state, QStringLiteral("Downloading"));
    auto conflicting = result;
    conflicting.envelope = QByteArrayLiteral("must-not-overwrite");
    contender.handleCheckResult(conflicting, kNow);
    EXPECT_EQ(update.currentState(), downloading);
    QFile storedEnvelope(downloading.manifestEnvelopePath);
    ASSERT_TRUE(storedEnvelope.open(QIODevice::ReadOnly));
    EXPECT_EQ(storedEnvelope.readAll(), result.envelope);
    storedEnvelope.close();

    QElapsedTimer timer;
    timer.start();
    while (approvals == 0 && timer.elapsed() < 8000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    EXPECT_EQ(approvals, 1);
    const auto landed = update.currentState();
    ASSERT_EQ(landed.state, QStringLiteral("AwaitingBridge"));
    QFile handedPackage(HandoffArea::packagePath(landed.transactionId));
    ASSERT_TRUE(handedPackage.open(QIODevice::ReadOnly));
    EXPECT_EQ(handedPackage.readAll(), content);
    EXPECT_TRUE(VNH::DownloadLease::acquire(landed.downloadPath));
}

TEST_F(HubSelfUpdateTest, ApproveViaBridgeRollsBackWhenDispatchFails)
{
    qputenv("HARKEN_HUB_BRIDGE_COMMAND", " ");
    const QByteArray content = QByteArrayLiteral("msi-bytes");
    ASSERT_TRUE(UpdateStateStore::save(readyBridgeRecord(content), kNow));

    HubSelfUpdate update;
    QString error;
    EXPECT_FALSE(
        update.approveViaBridge(QStringLiteral("gui"), &error, kNow));

    const UpdateStateRecord state =
        UpdateStateStore::load(QStringLiteral("HarkenHub"));
    EXPECT_EQ(state.state, QStringLiteral("AwaitingRestart"));
    EXPECT_TRUE(state.transactionId.isEmpty());
    EXPECT_TRUE(UpdateQueue::pendingEntries().empty());
}

TEST_F(HubSelfUpdateTest, FailedBridgeLaunchDoesNotReenterAutomaticApproval)
{
    qputenv("HARKEN_HUB_BRIDGE_COMMAND", " ");
    const auto ready = readyBridgeRecord(QByteArrayLiteral("installer"));
    ASSERT_TRUE(UpdateStateStore::save(ready, kNow));
    HubSelfUpdate update;
    int attempts = 0;
    const auto approveWhenReady = [&](const UpdateStateRecord& record) {
        if (record.state != QStringLiteral("AwaitingRestart"))
            return;
        ++attempts;
        if (attempts > 1)
            return;
        QString error;
        EXPECT_FALSE(update.approveViaBridge("background", &error, kNow));
    };
    QObject::connect(&update, &HubSelfUpdate::stateChanged, &update, approveWhenReady);
    approveWhenReady(ready);
    EXPECT_EQ(attempts, 1);
    EXPECT_EQ(update.currentState().state, ready.state);
    EXPECT_TRUE(UpdateQueue::pendingEntries().empty());
    qputenv("HARKEN_HUB_BRIDGE_COMMAND", "noop");
    QString error;
    EXPECT_TRUE(update.approveViaBridge("background", &error, kNow.addSecs(1)));
    EXPECT_EQ(update.currentState().state, QStringLiteral("AwaitingBridge"));
    EXPECT_EQ(attempts, 1);
}

#if defined(Q_OS_WIN)
TEST_F(HubSelfUpdateTest, FailedLaunchRollbackOutlivesTheApprovalRequest)
{
    qputenv("HARKEN_HUB_BRIDGE_COMMAND", " ");
    const auto ready = readyBridgeRecord(QByteArrayLiteral("installer"));
    ASSERT_TRUE(UpdateStateStore::save(ready, kNow));
    QFile reader(UpdateStateStore::filePath());
    {
        HubSelfUpdate update;
        QObject::connect(&update, &HubSelfUpdate::stateChanged, &update,
                         [&](const UpdateStateRecord& record) {
            if (record.state == QStringLiteral("AwaitingBridge"))
                EXPECT_TRUE(reader.open(QIODevice::ReadOnly));
        });
        QString error;
        EXPECT_FALSE(update.approveViaBridge("gui", &error, kNow));
        ASSERT_TRUE(reader.isOpen());
        EXPECT_EQ(update.currentState().state, QStringLiteral("AwaitingBridge"));
    }
    reader.close();
    QElapsedTimer elapsed;
    elapsed.start();
    while (UpdateStateStore::load(ready.product).state != ready.state && elapsed.elapsed() < 10000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    EXPECT_EQ(UpdateStateStore::load(ready.product).state, ready.state);
    EXPECT_TRUE(UpdateStateStore::load(ready.product).transactionId.isEmpty());
}
#endif

TEST_F(HubSelfUpdateTest, ApproveViaBridgeRequiresBridgeAvailability)
{
    qputenv("HARKEN_HUB_BRIDGE_DISABLE", "1");
    const QByteArray content = QByteArrayLiteral("msi-bytes");
    ASSERT_TRUE(UpdateStateStore::save(readyBridgeRecord(content), kNow));

    HubSelfUpdate update;
    QString error;
    EXPECT_FALSE(
        update.approveViaBridge(QStringLiteral("gui"), &error, kNow));
    EXPECT_TRUE(error.contains(QStringLiteral("bridge")))
        << error.toStdString();
    EXPECT_EQ(UpdateStateStore::load(QStringLiteral("HarkenHub")).state,
              QStringLiteral("AwaitingRestart"));
}

TEST_F(HubSelfUpdateTest, ApproveViaBridgeHonorsExplicitDisableSwitch)
{
    qputenv("HARKEN_HUB_SELFUPDATE_DISABLE", "1");
    qputenv("HARKEN_HUB_BRIDGE_COMMAND", "noop");
    const QByteArray content = QByteArrayLiteral("msi-bytes");
    ASSERT_TRUE(UpdateStateStore::save(readyBridgeRecord(content), kNow));

    HubSelfUpdate update;
    QString error;
    EXPECT_FALSE(
        update.approveViaBridge(QStringLiteral("gui"), &error, kNow));
    EXPECT_TRUE(error.contains(QStringLiteral("disabled")))
        << error.toStdString();
    EXPECT_FALSE(HubSelfUpdate::selfUpdateEnabled());

    qunsetenv("HARKEN_HUB_SELFUPDATE_DISABLE");
    EXPECT_TRUE(HubSelfUpdate::selfUpdateEnabled());
}

TEST_F(HubSelfUpdateTest, FinalizeClearsBridgeLanding)
{
    const QString path = writeInstaller(QByteArrayLiteral("installer-bytes"));

    UpdateStateRecord record;
    record.product = QStringLiteral("HarkenHub");
    record.state = QStringLiteral("AwaitingBridge");
    record.version = QStringLiteral("0.2.0");
    record.buildId = QStringLiteral("run4.dddd4444");
    record.downloadPath = path;
    ASSERT_TRUE(UpdateStateStore::save(record, kNow));

    EXPECT_TRUE(
        HubSelfUpdate::finalizeAfterRestart(QStringLiteral("0.2.0")));
    EXPECT_FALSE(QFile::exists(path));
    EXPECT_FALSE(
        UpdateStateStore::load(QStringLiteral("HarkenHub")).found);
}

TEST_F(HubSelfUpdateTest, ReconcileFallsBackWithoutBridge)
{
    qputenv("HARKEN_HUB_BRIDGE_DISABLE", "1");
    UpdateStateRecord record;
    record.product = QStringLiteral("HarkenHub");
    record.state = QStringLiteral("AwaitingRestart");
    record.version = QStringLiteral("0.2.0");
    record.buildId = QStringLiteral("run4.dddd4444");
    ASSERT_TRUE(UpdateStateStore::save(record, kNow));

    EXPECT_TRUE(HubSelfUpdate::reconcileLandingStateAtStartup(kNow));
    EXPECT_EQ(UpdateStateStore::load(QStringLiteral("HarkenHub")).state,
              QStringLiteral("AwaitingUserApproval"));
}

TEST_F(HubSelfUpdateTest, ReconcilePromotesApprovalToBridgeLanding)
{
    qputenv("HARKEN_HUB_BRIDGE_COMMAND", "noop");
    UpdateStateRecord record =
        readyBridgeRecord(QByteArrayLiteral("msi-bytes"));
    record.state = QStringLiteral("AwaitingUserApproval");
    ASSERT_TRUE(UpdateStateStore::save(record, kNow));

    EXPECT_TRUE(HubSelfUpdate::reconcileLandingStateAtStartup(kNow));
    EXPECT_EQ(UpdateStateStore::load(QStringLiteral("HarkenHub")).state,
              QStringLiteral("AwaitingRestart"));
}

TEST_F(HubSelfUpdateTest, InstallerLaunchCommandRunsSetupExeQuietly)
{
    const QString path =
        dir.filePath(QStringLiteral("HarkenHubSetup-run4.exe"));
    QString program;
    QStringList arguments;
    HubSelfUpdate::installerLaunchCommand(path, &program, &arguments);
    EXPECT_EQ(program, path);
    EXPECT_FALSE(arguments.contains(QStringLiteral("/i")));
    EXPECT_EQ(arguments.value(0), QStringLiteral("/qb!"));
    EXPECT_TRUE(arguments.contains(QStringLiteral("/norestart")));
    EXPECT_TRUE(arguments.contains(QStringLiteral("HARKENSELFUPDATE=1")));
    EXPECT_TRUE(arguments.contains(QStringLiteral("/l*v")));
}

TEST_F(HubSelfUpdateTest, HandleManifestIgnoresCurrentVersion)
{
    HubSelfUpdate update;
    HubSelfUpdate::Options options;
    options.serverBaseUrl = QUrl(QStringLiteral("https://unused.example"));
    options.currentVersion = QStringLiteral("0.1.0");
    options.downloadDir = dir.filePath(QStringLiteral("hub-updates"));
    update.configure(options);

    VNH::UpdateCheckResult result;
    result.ok = true;
    result.manifest.product = QStringLiteral("HarkenHub");
    result.manifest.version = QStringLiteral("0.1.0");
    result.manifest.buildId = QStringLiteral("run4.dddd4444");
    result.manifest.packageSize = 10;
    result.manifest.sha256Hex = QString(64, QLatin1Char('a'));
    result.objectPath = QStringLiteral(
        "release/hub/windows-x64/0.2.0/run4.dddd4444/HarkenHubSetup.exe");
    update.handleCheckResult(result, kNow);

    EXPECT_FALSE(update.currentState().found);
}

TEST_F(HubSelfUpdateTest, ComparesSemanticVersionNumbers)
{
    EXPECT_TRUE(HubSelfUpdate::isNewerVersion(QStringLiteral("0.1.38-dev"),
                                              QStringLiteral("0.1.37-dev")));
    EXPECT_FALSE(HubSelfUpdate::isNewerVersion(QStringLiteral("0.1.37-dev"),
                                               QStringLiteral("0.1.37-dev")));
    EXPECT_FALSE(HubSelfUpdate::isNewerVersion(QStringLiteral("0.1.36-dev"),
                                               QStringLiteral("0.1.37-dev")));
    EXPECT_FALSE(HubSelfUpdate::isNewerVersion(QStringLiteral("invalid"),
                                               QStringLiteral("0.1.37-dev")));
}

TEST_F(HubSelfUpdateTest, HandleManifestIgnoresOlderVersion)
{
    HubSelfUpdate update;
    HubSelfUpdate::Options options;
    options.serverBaseUrl = QUrl(QStringLiteral("https://unused.example"));
    options.currentVersion = QStringLiteral("0.1.23-dev");
    options.downloadDir = dir.filePath(QStringLiteral("hub-updates"));
    update.configure(options);

    VNH::UpdateCheckResult result;
    result.ok = true;
    result.manifest.product = QStringLiteral("HarkenHub");
    result.manifest.version = QStringLiteral("0.1.21-dev");
    result.manifest.buildId = QStringLiteral("run9.aaaa9999");
    result.manifest.packageSize = 10;
    result.manifest.sha256Hex = QString(64, QLatin1Char('a'));
    result.objectPath = QStringLiteral(
        "release/hub/windows-x64/0.2.0/run4.dddd4444/HarkenHubSetup.exe");
    update.handleCheckResult(result, kNow);

    EXPECT_FALSE(update.currentState().found);
}

TEST_F(HubSelfUpdateTest, OlderManifestClearsArmedState)
{
    UpdateStateRecord record;
    record.product = QStringLiteral("HarkenHub");
    record.state = QStringLiteral("AwaitingRestart");
    record.version = QStringLiteral("0.1.21-dev");
    record.buildId = QStringLiteral("run9.aaaa9999");
    ASSERT_TRUE(UpdateStateStore::save(record, kNow));

    HubSelfUpdate update;
    HubSelfUpdate::Options options;
    options.serverBaseUrl = QUrl(QStringLiteral("https://unused.example"));
    options.currentVersion = QStringLiteral("0.1.23-dev");
    options.downloadDir = dir.filePath(QStringLiteral("hub-updates"));
    update.configure(options);

    VNH::UpdateCheckResult result;
    result.ok = true;
    result.manifest.product = QStringLiteral("HarkenHub");
    result.manifest.version = QStringLiteral("0.1.21-dev");
    result.manifest.buildId = QStringLiteral("run9.aaaa9999");
    result.manifest.packageSize = 10;
    result.manifest.sha256Hex = QString(64, QLatin1Char('a'));
    update.handleCheckResult(result, kNow);

    EXPECT_FALSE(
        UpdateStateStore::load(QStringLiteral("HarkenHub")).found);
}

TEST_F(HubSelfUpdateTest, RollbackClearsStateArmedForRetractedBuild)
{
    const QString installerPath =
        writeInstaller(QByteArrayLiteral("retracted-bytes"));
    const QString envelopePath =
        dir.filePath(QStringLiteral("run13.bbbb1313.manifest"));
    {
        QFile envelope(envelopePath);
        ASSERT_TRUE(envelope.open(QIODevice::WriteOnly));
        envelope.write(QByteArrayLiteral("envelope"));
    }

    UpdateStateRecord record;
    record.product = QStringLiteral("HarkenHub");
    record.state = QStringLiteral("AwaitingRestart");
    record.version = QStringLiteral("0.1.24-dev");
    record.buildId = QStringLiteral("run13.bbbb1313");
    record.downloadPath = installerPath;
    record.manifestEnvelopePath = envelopePath;
    ASSERT_TRUE(UpdateStateStore::save(record, kNow));

    HubSelfUpdate update;
    HubSelfUpdate::Options options;
    options.serverBaseUrl = QUrl(QStringLiteral("https://unused.example"));
    options.currentVersion = QStringLiteral("0.1.23-dev");
    options.downloadDir = dir.filePath(QStringLiteral("hub-updates"));
    update.configure(options);

    VNH::UpdateCheckResult result;
    result.ok = true;
    result.manifest.product = QStringLiteral("HarkenHub");
    result.manifest.version = QStringLiteral("0.1.21-dev");
    result.manifest.buildId = QStringLiteral("run9.aaaa9999");
    result.manifest.packageSize = 10;
    result.manifest.sha256Hex = QString(64, QLatin1Char('a'));
    update.handleCheckResult(result, kNow);

    EXPECT_FALSE(
        UpdateStateStore::load(QStringLiteral("HarkenHub")).found);
    EXPECT_FALSE(QFile::exists(installerPath));
    EXPECT_FALSE(QFile::exists(envelopePath));
}

TEST_F(HubSelfUpdateTest, LowerReleaseSequenceCannotReplaceObservedState)
{
    UpdateStateRecord record;
    record.product = QStringLiteral("HarkenHub");
    record.state = QStringLiteral("AwaitingRestart");
    record.version = QStringLiteral("0.1.24-dev");
    record.buildId = QStringLiteral("run13.bbbb1313");
    record.releaseSequence = 13;
    ASSERT_TRUE(UpdateStateStore::save(record, kNow));

    HubSelfUpdate update;
    HubSelfUpdate::Options options;
    options.serverBaseUrl = QUrl(QStringLiteral("https://unused.example"));
    options.currentVersion = QStringLiteral("0.1.23-dev");
    options.downloadDir = dir.filePath(QStringLiteral("hub-updates"));
    update.configure(options);

    VNH::UpdateCheckResult result;
    result.ok = true;
    result.manifest.product = QStringLiteral("HarkenHub");
    result.manifest.version = QStringLiteral("0.1.25-dev");
    result.manifest.buildId = QStringLiteral("run12.aaaa1212");
    result.manifest.releaseSequence = 12;
    update.handleCheckResult(result, kNow);

    const UpdateStateRecord observed =
        UpdateStateStore::load(QStringLiteral("HarkenHub"));
    ASSERT_TRUE(observed.found);
    EXPECT_EQ(observed.buildId, record.buildId);
    EXPECT_EQ(observed.releaseSequence, record.releaseSequence);
}

TEST_F(HubSelfUpdateTest, UnauthorizedDownloadIsAnonymousFailure)
{
    HarkenHubTest::RangeHttpServer server;
    ASSERT_TRUE(server.listen());
    server.setPayload(QByteArrayLiteral("installer"),
                      QByteArrayLiteral("\"h1\""));
    HarkenHubTest::RangeHttpServer::Script unauthorized;
    unauthorized.mode =
        HarkenHubTest::RangeHttpServer::Script::Mode::Status;
    unauthorized.status = 401;
    server.enqueue(unauthorized);

    HubSelfUpdate update;
    HubSelfUpdate::Options options;
    options.serverBaseUrl = server.baseUrl();
    options.currentVersion = QStringLiteral("0.1.0");
    options.downloadDir = dir.filePath(QStringLiteral("hub-updates"));
    update.configure(options);

    VNH::UpdateCheckResult result;
    result.ok = true;
    result.manifest.product = QStringLiteral("HarkenHub");
    result.manifest.version = QStringLiteral("0.2.0");
    result.manifest.buildId = QStringLiteral("run4.dddd4444");
    result.manifest.packageSize = 9;
    result.manifest.sha256Hex = QString(64, QLatin1Char('a'));
    result.objectPath = QStringLiteral(
        "release/hub/windows-x64/0.2.0/run4.dddd4444/HarkenHubSetup.exe");
    update.handleCheckResult(result, kNow);

    QElapsedTimer timer;
    timer.start();
    while (update.currentState().state != QStringLiteral("DownloadFailed") &&
           timer.elapsed() < 8000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

    EXPECT_EQ(update.currentState().state,
              QStringLiteral("DownloadFailed"));
    EXPECT_EQ(update.currentState().failureCode,
              QStringLiteral("DownloadFailed"));
    EXPECT_TRUE(server.authorizationHistory().value(0).isEmpty());
}

TEST_F(HubSelfUpdateTest, DownloadTargetUsesPlatformInstallerSuffix)
{
    HubSelfUpdate update;
    HubSelfUpdate::Options options;
    options.serverBaseUrl = QUrl(QStringLiteral("https://unused.example"));
    options.currentVersion = QStringLiteral("0.1.0");
    options.downloadDir = dir.filePath(QStringLiteral("hub-updates"));
    update.configure(options);

    VNH::UpdateCheckResult result;
    result.ok = true;
    result.manifest.product = QStringLiteral("HarkenHub");
    result.manifest.version = QStringLiteral("0.2.0");
    result.manifest.buildId = QStringLiteral("run4.dddd4444");
    result.manifest.packageSize = 10;
    result.manifest.sha256Hex = QString(64, QLatin1Char('a'));
    result.objectPath = QStringLiteral(
        "release/hub/windows-x64/0.2.0/run4.dddd4444/HarkenHubSetup.exe");
    update.handleCheckResult(result, kNow);

    const UpdateStateRecord state = update.currentState();
    EXPECT_EQ(state.state, QStringLiteral("Downloading"));
#if defined(Q_OS_MACOS)
    EXPECT_TRUE(state.downloadPath.endsWith(QStringLiteral(".pkg")))
        << state.downloadPath.toStdString();
#else
    EXPECT_TRUE(state.downloadPath.endsWith(QStringLiteral(".exe")))
        << state.downloadPath.toStdString();
#endif
    update.stop();
}

TEST_F(HubSelfUpdateTest, InstallerLaunchCommandWrapsPkgInstaller)
{
    QString program;
    QStringList arguments;
    HubSelfUpdate::installerLaunchCommand(
        dir.filePath(QStringLiteral("HarkenHubSetup-run4.pkg")), &program,
        &arguments);
    EXPECT_EQ(program, QStringLiteral("/usr/bin/osascript"));
    ASSERT_EQ(arguments.size(), 2);
    EXPECT_EQ(arguments.value(0), QStringLiteral("-e"));
    EXPECT_TRUE(arguments.value(1).startsWith(
        QStringLiteral("do shell script \"")));
    EXPECT_TRUE(arguments.value(1).contains(
        QStringLiteral("/usr/sbin/installer -pkg")));
    EXPECT_TRUE(arguments.value(1).endsWith(
        QStringLiteral("\" with administrator privileges")));
}

}
