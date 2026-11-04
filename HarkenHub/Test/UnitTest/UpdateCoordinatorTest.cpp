#include "Update/UpdateCoordinator.h"

#include "Packaging/PackageReader.h"
#include "Platform/PlatformPaths.h"
#include "Platform/UserSession.h"
#include "Support/ManifestTestUtils.h"
#include "Support/RangeHttpServer.h"
#include "Transactions/InstallExecutor.h"
#include "Transactions/InstallIndex.h"
#include "Transactions/HandoffArea.h"
#include "Transactions/TransactionJournal.h"
#include "Update/UpdateBridgeConsumer.h"
#include "Update/UpdateQueue.h"
#include "Update/UpdateStateStore.h"
#include "ZipTestUtils.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QTemporaryDir>

#include <functional>
#include <memory>

using VNH::ReleaseManifest;
using VNH::UpdateCheckResult;
using VNH::UpdateCoordinator;
using VNH::UpdateStateStore;
using HarkenHubTest::RangeHttpServer;
using HarkenHubTest::TestEntry;
using HarkenHubTest::writeArchive;

namespace {

const QDateTime kNow =
    QDateTime::fromString(QStringLiteral("2026-07-24T12:00:00Z"), Qt::ISODate);

bool spinUntil(const std::function<bool()>& predicate, int timeoutMs = 10000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

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

QByteArray buildPackageBytes(const QString& tempPath)
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
    manifest[QStringLiteral("version")] = QStringLiteral("1.5.0");
    manifest[QStringLiteral("buildId")] = QStringLiteral("run3.cccc3333");
    manifest[QStringLiteral("channel")] = QStringLiteral("stable");
    manifest[QStringLiteral("files")] = files;

    QJsonObject capabilities;
    capabilities[QStringLiteral("studioVersion")] = QStringLiteral("1.5.0");
    capabilities[QStringLiteral("buildId")] = QStringLiteral("run3.cccc3333");
    capabilities[QStringLiteral("capabilitiesVersion")] = 2;

    const std::vector<TestEntry> entries = {
        {VNH::studioBinaryRelativePath().toUtf8(), binary},
        {QByteArrayLiteral("package-manifest.json"),
         QJsonDocument(manifest).toJson(QJsonDocument::Compact)},
        {QByteArrayLiteral("capabilities.json"),
         QJsonDocument(capabilities).toJson(QJsonDocument::Compact)},
    };
    if (!writeArchive(tempPath, entries))
        return QByteArray();
    QFile file(tempPath);
    file.open(QIODevice::ReadOnly);
    return file.readAll();
}

class UpdateCoordinatorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        userGuard = std::make_unique<EnvGuard>("HARKEN_HUB_USER_ROOT",
                                               dir.path());
        machineGuard = std::make_unique<EnvGuard>(
            "HARKEN_HUB_MACHINE_ROOT",
            dir.filePath(QStringLiteral("machine")));
        packageBytes =
            buildPackageBytes(dir.filePath(QStringLiteral("source.zip")));
        ASSERT_FALSE(packageBytes.isEmpty());
    }

    UpdateCheckResult manifestResultFor()
    {
        UpdateCheckResult result;
        result.ok = true;
        result.manifest.product = QStringLiteral("HarkenStudio");
        result.manifest.version = QStringLiteral("1.5.0");
        result.manifest.buildId = QStringLiteral("run3.cccc3333");
        result.manifest.channel = QStringLiteral("stable");
        result.manifest.sha256Hex =
            QString::fromLatin1(sha256Hex(packageBytes));
        result.manifest.packageSize = packageBytes.size();
        result.manifest.releaseSequence = 9;
        result.envelope = QByteArrayLiteral("{\"stub\":\"envelope\"}");
        result.objectPath =
            QStringLiteral("release/harken/windows-x64/1.5.0/run3-cccc3333-20261008T100000Z/Harken.zip");
        return result;
    }

    UpdateCheckResult signedManifestResultFor(
        const HarkenHubTest::TestKeypair& keys)
    {
        UpdateCheckResult result = manifestResultFor();
        QJsonObject payload = HarkenHubTest::baseManifestPayload();
        payload[QStringLiteral("version")] = QStringLiteral("1.5.0");
        payload[QStringLiteral("buildId")] = QStringLiteral("run3.cccc3333");
        payload[QStringLiteral("packageSize")] = packageBytes.size();
        payload[QStringLiteral("sha256")] =
            QString::fromLatin1(sha256Hex(packageBytes));
        payload[QStringLiteral("releaseSequence")] = 9;
        result.envelope = HarkenHubTest::signEnvelope(
            QJsonDocument(payload).toJson(QJsonDocument::Compact), keys);
        return result;
    }

    QTemporaryDir dir;
    std::unique_ptr<EnvGuard> userGuard;
    std::unique_ptr<EnvGuard> machineGuard;
    QByteArray packageBytes;
};

TEST_F(UpdateCoordinatorTest, DownloadsAndVerifiesNewBuild)
{
    const EnvGuard bridgeGuard("HARKEN_HUB_BRIDGE_DISABLE",
                               QStringLiteral("1"));
    RangeHttpServer server;
    ASSERT_TRUE(server.listen());
    server.setPayload(packageBytes, QByteArrayLiteral("\"etag-1\""));

    UpdateCoordinator coordinator;
    UpdateCoordinator::Options options;
    options.serverBaseUrl = server.baseUrl();
    options.downloadDir = dir.filePath(QStringLiteral("downloads"));
    options.quarantineDir = dir.filePath(QStringLiteral("quarantine"));
    coordinator.configure(options);

    coordinator.handleCheckResult(manifestResultFor(), kNow);

    ASSERT_TRUE(spinUntil([&]() {
        return coordinator.currentState().state ==
                   QStringLiteral("AwaitingUserInstall") ||
               coordinator.currentState().state.contains(
                   QStringLiteral("Failed"));
    }));
    const auto state = coordinator.currentState();
    EXPECT_EQ(state.state, QStringLiteral("AwaitingUserInstall"))
        << state.failureCode.toStdString();
    EXPECT_TRUE(QFile::exists(state.downloadPath));
    EXPECT_TRUE(QFile::exists(state.manifestEnvelopePath));
}

TEST_F(UpdateCoordinatorTest, RecordsAvailableBuildWithoutAutomaticDownload)
{
    UpdateCoordinator coordinator;
    UpdateCoordinator::Options options;
    options.serverBaseUrl = QUrl(QStringLiteral("https://unused.example"));
    options.downloadDir = dir.filePath(QStringLiteral("downloads"));
    options.automaticDownloadEnabled = false;
    coordinator.configure(options);

    coordinator.handleCheckResult(manifestResultFor(), kNow);

    const auto state = coordinator.currentState();
    EXPECT_EQ(state.state, QStringLiteral("MetadataAvailable"));
    EXPECT_EQ(state.version, QStringLiteral("1.5.0"));
    EXPECT_TRUE(state.downloadPath.isEmpty());
    EXPECT_TRUE(QFile::exists(state.manifestEnvelopePath));
    EXPECT_FALSE(coordinator.isDownloading());
}

TEST_F(UpdateCoordinatorTest, DisablingAutomaticDownloadPausesActiveTransfer)
{
    RangeHttpServer server;
    ASSERT_TRUE(server.listen());
    server.setPayload(packageBytes, QByteArrayLiteral("\"etag-pause\""));
    RangeHttpServer::Script delayed;
    delayed.delayMs = 1000;
    server.enqueue(delayed);

    UpdateCoordinator coordinator;
    UpdateCoordinator::Options options;
    options.serverBaseUrl = server.baseUrl();
    options.downloadDir = dir.filePath(QStringLiteral("downloads"));
    options.quarantineDir = dir.filePath(QStringLiteral("quarantine"));
    coordinator.configure(options);

    coordinator.handleCheckResult(manifestResultFor(), kNow);
    ASSERT_TRUE(coordinator.isDownloading());
    coordinator.setAutomaticDownloadEnabled(false);

    ASSERT_TRUE(spinUntil([&]() {
        return coordinator.currentState().state ==
               QStringLiteral("MetadataAvailable");
    }));
    EXPECT_FALSE(coordinator.isDownloading());
    EXPECT_TRUE(coordinator.currentState().downloadPath.isEmpty());
}

TEST_F(UpdateCoordinatorTest, QueuesVerifiedBuildForBridgeInstall)
{
    const EnvGuard versionsGuard(
        "HARKEN_HUB_VERSIONS_ROOT",
        dir.filePath(QStringLiteral("versions")));
    const EnvGuard bridgeGuard("HARKEN_HUB_BRIDGE_COMMAND",
                               QStringLiteral("noop"));
    const auto keys = HarkenHubTest::makeKeypair();

    RangeHttpServer server;
    ASSERT_TRUE(server.listen());
    server.setPayload(packageBytes, QByteArrayLiteral("\"etag-3\""));

    UpdateCoordinator coordinator;
    UpdateCoordinator::Options options;
    options.serverBaseUrl = server.baseUrl();
    options.manifestPublicKey = keys.publicKey;
    options.downloadDir = dir.filePath(QStringLiteral("downloads"));
    options.quarantineDir = dir.filePath(QStringLiteral("quarantine"));
    options.bridgePollIntervalMs = 50;
    coordinator.configure(options);

    coordinator.handleCheckResult(signedManifestResultFor(keys), kNow);

    ASSERT_TRUE(spinUntil([&]() {
        return coordinator.currentState().state ==
                   QStringLiteral("AwaitingBridge") ||
               coordinator.currentState().state.contains(
                   QStringLiteral("Failed"));
    }));
    auto state = coordinator.currentState();
    ASSERT_EQ(state.state, QStringLiteral("AwaitingBridge"))
        << state.failureCode.toStdString();
    ASSERT_FALSE(state.transactionId.isEmpty());
    const auto entries = VNH::UpdateQueue::pendingEntries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].transactionId, state.transactionId);
    EXPECT_EQ(entries[0].product, QStringLiteral("HarkenStudio"));

    QLockFile lock(UpdateStateStore::filePath() + QStringLiteral(".lock"));
    ASSERT_TRUE(lock.tryLock());
    const auto report = VNH::UpdateBridgeConsumer::run(
        keys.publicKey, kNow, VNH::UpdateBridgeConsumer::defaultHooks());
    ASSERT_EQ(report.installed, 1);
    EXPECT_FALSE(spinUntil([&]() { return coordinator.currentState().state == "Installed"; }, 100));
    QString error;
    const auto outcome = VNH::UpdateQueue::studioResult(state.transactionId, &error);
    ASSERT_TRUE(outcome.has_value()) << error.toStdString();
    EXPECT_TRUE(outcome->ok);
    lock.unlock();

    ASSERT_TRUE(spinUntil([&]() {
        return coordinator.currentState().state ==
               QStringLiteral("Installed");
    }));
    state = coordinator.currentState();
    EXPECT_EQ(state.state, QStringLiteral("Installed"));
    EXPECT_TRUE(state.downloadPath.isEmpty());
    EXPECT_FALSE(VNH::UpdateQueue::studioResult(state.transactionId, &error).has_value());
}

TEST_F(UpdateCoordinatorTest, FailedInstallSurvivesMissingJournalRestartAndBlockedStateWrite)
{
    const EnvGuard versionsGuard(
        "HARKEN_HUB_VERSIONS_ROOT",
        dir.filePath(QStringLiteral("versions")));
    const EnvGuard bridgeGuard("HARKEN_HUB_BRIDGE_COMMAND",
                               QStringLiteral("noop"));
    const auto keys = HarkenHubTest::makeKeypair();

    RangeHttpServer server;
    ASSERT_TRUE(server.listen());
    server.setPayload(packageBytes, QByteArrayLiteral("\"etag-4\""));

    UpdateCoordinator coordinator;
    UpdateCoordinator::Options options;
    options.serverBaseUrl = server.baseUrl();
    options.manifestPublicKey = keys.publicKey;
    options.downloadDir = dir.filePath(QStringLiteral("downloads"));
    options.quarantineDir = dir.filePath(QStringLiteral("quarantine"));
    options.bridgePollIntervalMs = 50;
    coordinator.configure(options);

    coordinator.handleCheckResult(signedManifestResultFor(keys), kNow);

    ASSERT_TRUE(spinUntil([&]() {
        return coordinator.currentState().state ==
               QStringLiteral("AwaitingBridge");
    }));
    const auto pending = coordinator.currentState();
    coordinator.stop();
    ASSERT_TRUE(QDir().mkpath(VNH::TransactionJournal::filePath()));
    QFile package(VNH::HandoffArea::packagePath(pending.transactionId));
    ASSERT_TRUE(package.open(QIODevice::Append));
    package.write("damaged");
    package.close();
    const auto report = VNH::UpdateBridgeConsumer::run(
        keys.publicKey, kNow, VNH::UpdateBridgeConsumer::defaultHooks());
    ASSERT_EQ(report.failed, 1);
    QString error;
    const auto outcome = VNH::UpdateQueue::studioResult(pending.transactionId, &error);
    ASSERT_TRUE(outcome.has_value()) << error.toStdString();
    ASSERT_FALSE(outcome->ok);
    EXPECT_EQ(outcome->failureCode, QStringLiteral("PackageRejected"));
    ASSERT_FALSE(outcome->error.isEmpty());

    QLockFile lock(UpdateStateStore::filePath() + QStringLiteral(".lock"));
    ASSERT_TRUE(lock.tryLock());
    options.automaticDownloadEnabled = false;
    UpdateCoordinator resumed;
    ASSERT_TRUE(resumed.start(options));
    EXPECT_FALSE(spinUntil([&]() { return resumed.currentState().state == "InstallFailed"; }, 100));
    EXPECT_EQ(resumed.currentState(), pending);
    EXPECT_TRUE(VNH::UpdateQueue::studioResult(pending.transactionId, &error).has_value());
    lock.unlock();

    ASSERT_TRUE(spinUntil([&]() {
        return resumed.currentState().state ==
               QStringLiteral("InstallFailed");
    }));
    EXPECT_EQ(resumed.currentState().failureCode, outcome->failureCode);
    EXPECT_EQ(resumed.currentState().failureDetail, outcome->error);
    EXPECT_FALSE(VNH::UpdateQueue::studioResult(pending.transactionId, &error).has_value());
}

TEST_F(UpdateCoordinatorTest, InstallHandoffCannotBorrowAnotherPackagesOwnership)
{
    EnvGuard bridge("HARKEN_HUB_BRIDGE_COMMAND", QStringLiteral("noop"));
    VNH::UpdateStateRecord record;
    record.product = QStringLiteral("HarkenStudio");
    record.state = QStringLiteral("Verified");
    record.buildId = QStringLiteral("run3.cccc3333");
    record.downloadPath = dir.filePath(QStringLiteral("source.zip"));
    record.manifestEnvelopePath = dir.filePath(QStringLiteral("source.manifest"));
    {
        QFile envelope(record.manifestEnvelopePath);
        ASSERT_TRUE(envelope.open(QIODevice::WriteOnly));
        envelope.write(QByteArrayLiteral("manifest"));
    }
    ASSERT_TRUE(UpdateStateStore::save(record, kNow));
    const auto expected = UpdateStateStore::load(record.product);
    const auto owner = VNH::DownloadLease::acquire(record.downloadPath);
    ASSERT_TRUE(owner);
    const auto unrelated = VNH::DownloadLease::acquire(dir.filePath(QStringLiteral("other.zip")));
    ASSERT_TRUE(unrelated);
    UpdateCoordinator coordinator;
    coordinator.beginAutoInstall(kNow, unrelated);
    EXPECT_EQ(coordinator.currentState(), expected);
    EXPECT_TRUE(VNH::UpdateQueue::pendingEntries().empty());
}

TEST_F(UpdateCoordinatorTest, IncompleteInstallationCannotOverrideAFailedCompletion)
{
    UpdateCoordinator::Options options;
    options.serverBaseUrl = QUrl("https://unused.example");
    options.automaticDownloadEnabled = false;
    options.bridgePollIntervalMs = 10;
    for (const QString& health : {QString("Installing"), QString("NeedsRepair")}) {
        VNH::InstalledVersionRecord installed;
        installed.buildId = "incomplete";
        installed.semanticVersion = "1.5.0";
        installed.installPath = dir.filePath("versions/incomplete");
        installed.healthState = health;
        QString error;
        ASSERT_TRUE(VNH::InstallIndex::commitInstallation(installed, 9, &error));
        VNH::UpdateStateRecord state;
        state.product = "HarkenStudio";
        state.state = "AwaitingBridge";
        state.buildId = installed.buildId;
        state.transactionId = VNH::HandoffArea::newTransactionId();
        ASSERT_TRUE(UpdateStateStore::save(state, kNow));
        VNH::InstallOutcome outcome;
        outcome.buildId = installed.buildId;
        outcome.failureCode = "IndexCommitFailed";
        outcome.error = "Installation needs repair";
        ASSERT_TRUE(VNH::UpdateQueue::writeStudioResult(state.transactionId, outcome,
            VNH::currentUserId(), &error));
        UpdateCoordinator coordinator;
        ASSERT_TRUE(coordinator.start(options));
        ASSERT_TRUE(spinUntil([&]() { return coordinator.currentState().state == "InstallFailed"; }));
        EXPECT_EQ(coordinator.currentState().failureCode, outcome.failureCode);
        EXPECT_EQ(VNH::InstallIndex::snapshot().records.front().healthState, health);
    }
}

TEST_F(UpdateCoordinatorTest, QuarantinesTamperedPackage)
{
    QByteArray tampered = packageBytes;
    tampered[tampered.size() / 2] =
        static_cast<char>(~tampered[tampered.size() / 2]);

    RangeHttpServer server;
    ASSERT_TRUE(server.listen());
    server.setPayload(tampered, QByteArrayLiteral("\"etag-2\""));

    UpdateCoordinator coordinator;
    UpdateCoordinator::Options options;
    options.serverBaseUrl = server.baseUrl();
    options.downloadDir = dir.filePath(QStringLiteral("downloads"));
    options.quarantineDir = dir.filePath(QStringLiteral("quarantine"));
    coordinator.configure(options);

    coordinator.handleCheckResult(manifestResultFor(), kNow);

    ASSERT_TRUE(spinUntil([&]() {
        const QString state = coordinator.currentState().state;
        return state == QStringLiteral("VerificationFailed") ||
               state == QStringLiteral("Verified") ||
               state == QStringLiteral("DownloadFailed");
    }));
    const auto state = coordinator.currentState();
    EXPECT_EQ(state.state, QStringLiteral("VerificationFailed"));
    EXPECT_TRUE(QFile::exists(
        dir.filePath(QStringLiteral("quarantine/run3.cccc3333.zip"))));
}

TEST_F(UpdateCoordinatorTest, ClearsStateWhenBuildAlreadyInstalled)
{
    VNH::InstalledVersionRecord record;
    record.buildId = QStringLiteral("run3.cccc3333");
    record.installPath = dir.filePath(QStringLiteral("versions/x"));
    record.semanticVersion = QStringLiteral("1.5.0");
    record.healthState = QStringLiteral("Installed");
    QString error;
    ASSERT_TRUE(VNH::InstallIndex::commitInstallation(record, 1, &error));

    VNH::UpdateStateRecord stale;
    stale.product = QStringLiteral("HarkenStudio");
    stale.state = QStringLiteral("Verified");
    stale.buildId = QStringLiteral("run3.cccc3333");
    ASSERT_TRUE(UpdateStateStore::save(stale, kNow));

    UpdateCoordinator coordinator;
    UpdateCoordinator::Options options;
    options.serverBaseUrl = QUrl(QStringLiteral("https://unused.example"));
    options.downloadDir = dir.filePath(QStringLiteral("downloads"));
    coordinator.configure(options);

    coordinator.handleCheckResult(manifestResultFor(), kNow);
    EXPECT_FALSE(coordinator.currentState().found);
}

TEST_F(UpdateCoordinatorTest, IgnoresForeignProductManifest)
{
    UpdateCoordinator coordinator;
    UpdateCoordinator::Options options;
    options.serverBaseUrl = QUrl(QStringLiteral("https://unused.example"));
    options.downloadDir = dir.filePath(QStringLiteral("downloads"));
    coordinator.configure(options);

    UpdateCheckResult result = manifestResultFor();
    result.manifest.product = QStringLiteral("HarkenHub");
    coordinator.handleCheckResult(result, kNow);
    EXPECT_FALSE(coordinator.currentState().found);
}

TEST_F(UpdateCoordinatorTest, UnauthorizedDownloadIsAnonymousFailure)
{
    RangeHttpServer server;
    ASSERT_TRUE(server.listen());
    server.setPayload(packageBytes, QByteArrayLiteral("\"etag-5\""));
    RangeHttpServer::Script unauthorized;
    unauthorized.mode = RangeHttpServer::Script::Mode::Status;
    unauthorized.status = 401;
    server.enqueue(unauthorized);

    UpdateCoordinator coordinator;
    UpdateCoordinator::Options options;
    options.serverBaseUrl = server.baseUrl();
    options.downloadDir = dir.filePath(QStringLiteral("downloads"));
    options.quarantineDir = dir.filePath(QStringLiteral("quarantine"));
    coordinator.configure(options);

    coordinator.handleCheckResult(manifestResultFor(), kNow);

    ASSERT_TRUE(spinUntil([&]() {
        return coordinator.currentState().state ==
               QStringLiteral("DownloadFailed");
    }));
    EXPECT_EQ(coordinator.currentState().failureCode,
              QStringLiteral("DownloadFailed"));
    EXPECT_TRUE(server.authorizationHistory().value(0).isEmpty());
}

TEST_F(UpdateCoordinatorTest, InstalledLatestBuildDoesNotProbeDownload)
{
    VNH::InstalledVersionRecord installed;
    installed.buildId = QStringLiteral("run3.cccc3333");
    installed.installPath = dir.filePath(QStringLiteral("versions/x"));
    installed.semanticVersion = QStringLiteral("1.5.0");
    installed.healthState = QStringLiteral("Installed");
    QString error;
    ASSERT_TRUE(VNH::InstallIndex::commitInstallation(installed, 1, &error));
    RangeHttpServer server;
    ASSERT_TRUE(server.listen());
    server.setPayload(packageBytes, QByteArrayLiteral("\"etag-6\""));

    UpdateCoordinator coordinator;
    UpdateCoordinator::Options options;
    options.serverBaseUrl = server.baseUrl();
    options.downloadDir = dir.filePath(QStringLiteral("downloads"));
    coordinator.configure(options);

    coordinator.handleCheckResult(manifestResultFor(), kNow);

    EXPECT_EQ(server.requestCount(), 0);
    EXPECT_FALSE(coordinator.currentState().found);
}

}
