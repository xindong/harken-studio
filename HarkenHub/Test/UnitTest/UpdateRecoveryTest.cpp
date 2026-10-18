#include "Update/HubSelfUpdate.h"
#include "Update/UpdateCoordinator.h"
#include "Update/UpdateQueue.h"
#include "Platform/PlatformPaths.h"
#include "Support/ManifestTestUtils.h"
#include "Support/RangeHttpServer.h"
#include "ZipTestUtils.h"

#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <functional>
#include <iostream>
#include <type_traits>

namespace {

bool spinUntil(const std::function<bool()>& predicate)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 10000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

template<class Updater>
class UpdateRecoveryTest : public ::testing::Test
{
protected:
    static constexpr bool isHub = std::is_same_v<Updater, VNH::HubSelfUpdate>;

    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        qputenv("HARKEN_HUB_USER_ROOT", dir.path().toUtf8());
        qputenv("HARKEN_HUB_MACHINE_ROOT", dir.filePath("machine").toUtf8());
        qputenv("HARKEN_HUB_BRIDGE_DISABLE", "1");
        ASSERT_TRUE(feed.listen());
        ASSERT_TRUE(download.listen());
        QByteArray package("installer");
        if constexpr (!isHub) {
            const QByteArray binary("studio");
            const QJsonObject file{{"path", VNH::studioBinaryRelativePath()},
                {"size", binary.size()}, {"sha256", QString::fromLatin1(
                    QCryptographicHash::hash(binary, QCryptographicHash::Sha256).toHex())}};
            const QJsonObject manifest{{"product", "HarkenStudio"}, {"version", "1.4.0"},
                {"buildId", "run12.abcd1234"}, {"channel", "stable"},
                {"files", QJsonArray{file}}};
            const QJsonObject capabilities{{"studioVersion", "1.4.0"},
                {"buildId", "run12.abcd1234"}, {"capabilitiesVersion", 2}};
            const QString path = dir.filePath("studio.zip");
            ASSERT_TRUE(HarkenHubTest::writeArchive(path, {
                {VNH::studioBinaryRelativePath().toUtf8(), binary},
                {"package-manifest.json", QJsonDocument(manifest).toJson()},
                {"capabilities.json", QJsonDocument(capabilities).toJson()}}));
            QFile archive(path);
            ASSERT_TRUE(archive.open(QIODevice::ReadOnly));
            package = archive.readAll();
        }
        const auto keys = HarkenHubTest::makeKeypair();
        auto payload = HarkenHubTest::baseManifestPayload();
        payload["product"] = isHub ? "HarkenHub" : "HarkenStudio";
        payload["packageSize"] = package.size();
        payload["sha256"] = QString::fromLatin1(
            QCryptographicHash::hash(package, QCryptographicHash::Sha256).toHex());
        const auto envelope = HarkenHubTest::signEnvelope(QJsonDocument(payload).toJson(), keys);
        const QString channel = VNH::ServerEndpoints::hostChannel();
        const QJsonObject entry{{"version", "1.4.0"},
            {"file", QStringLiteral("release/%1/%2/1.4.0/%3/%4")
                .arg(isHub ? "hub" : "harken", channel,
                     isHub ? "run12.abcd1234" : "run12-abcd1234-20261008T100000Z",
                     isHub ? "HarkenHubSetup.exe" : "Harken.zip")},
            {"integrity", QJsonDocument::fromJson(envelope).object()}};
        feed.setPayload(QJsonDocument(QJsonObject{{"schema_version", 1},
            {"channels", QJsonObject{{channel, QJsonArray{entry}}}}}).toJson(), "\"feed-v1\"");
        download.setPayload(package, "\"package-v1\"");

        typename Updater::Options options;
        options.serverBaseUrl = download.baseUrl();
        options.manifestPublicKey = keys.publicKey;
        if constexpr (isHub)
            options.currentVersion = "1.0.0";
        update.configure(options);
        checker = update.template findChild<VNH::UpdateChecker*>();
        ASSERT_NE(checker, nullptr);
        checkerOptions.latestUrl = feed.packageUrl();
        checkerOptions.trustedHttpOrigin = feed.baseUrl();
        checkerOptions.product = isHub ? "hub" : "harken";
        checkerOptions.channel = channel;
        checkerOptions.manifestPublicKey = keys.publicKey;
        QObject::connect(&update, &Updater::stateChanged, checker,
                         [this](const VNH::UpdateStateRecord& announced) {
            const auto stored = update.currentState();
            EXPECT_TRUE(stored.found);
            EXPECT_EQ(stored.state, announced.state);
            EXPECT_EQ(stored.buildId, announced.buildId);
        });
    }

    void TearDown() override
    {
        if (HasFailure()) {
            const auto state = update.currentState();
            std::cerr << "Update recovery failure: state=" << state.state.toStdString()
                      << " failure=" << state.failureCode.toStdString()
                      << " checking=" << (checker && checker->isChecking())
                      << " feedRequests=" << feed.requestCount()
                      << " packageRequests=" << download.requestCount()
                      << " packageExists=" << QFile::exists(state.downloadPath)
                      << " manifestExists=" << QFile::exists(state.manifestEnvelopePath)
                      << '\n';
            if constexpr (!isHub)
                std::cerr << "downloading=" << update.isDownloading() << '\n';
        }
        update.stop();
        qunsetenv("HARKEN_HUB_USER_ROOT");
        qunsetenv("HARKEN_HUB_MACHINE_ROOT");
        qunsetenv("HARKEN_HUB_BRIDGE_DISABLE");
        qunsetenv("HARKEN_HUB_BRIDGE_COMMAND");
    }

    void failBridgeLaunchWithBlockedRollback(QFile& reader)
    {
        qunsetenv("HARKEN_HUB_BRIDGE_DISABLE");
        qputenv("HARKEN_HUB_BRIDGE_COMMAND", " ");
        reader.setFileName(VNH::UpdateStateStore::filePath());
        QObject observer;
        QObject::connect(&update, &Updater::stateChanged, &observer,
                         [&](const VNH::UpdateStateRecord& record) {
            if (record.state == QStringLiteral("AwaitingBridge"))
                EXPECT_TRUE(reader.open(QIODevice::ReadOnly));
        });
        ASSERT_TRUE(checker->start(checkerOptions));
        if constexpr (isHub) {
            ASSERT_TRUE(spinUntil([&]() {
                return update.currentState().state == QStringLiteral("AwaitingRestart");
            }));
            QString error;
            EXPECT_FALSE(update.approveViaBridge("gui", &error, QDateTime::currentDateTimeUtc()));
            EXPECT_FALSE(error.isEmpty());
        } else {
            ASSERT_TRUE(spinUntil([&]() { return reader.isOpen(); }));
        }
        ASSERT_TRUE(reader.isOpen());
        EXPECT_EQ(update.currentState().state, QStringLiteral("AwaitingBridge"));
        EXPECT_TRUE(VNH::UpdateQueue::pendingEntries().empty());
    }

    bool ready() const
    {
        const auto state = update.currentState();
        return state.state == (isHub ? "AwaitingUserApproval" : "AwaitingUserInstall") &&
               QFile::exists(state.downloadPath) && QFile::exists(state.manifestEnvelopePath);
    }

    QTemporaryDir dir;
    HarkenHubTest::RangeHttpServer feed;
    HarkenHubTest::RangeHttpServer download;
    Updater update;
    VNH::UpdateChecker* checker = nullptr;
    VNH::UpdateChecker::Options checkerOptions;
};

using Updaters = ::testing::Types<VNH::UpdateCoordinator, VNH::HubSelfUpdate>;
TYPED_TEST_SUITE(UpdateRecoveryTest, Updaters);

#if defined(Q_OS_WIN)
TYPED_TEST(UpdateRecoveryTest, FailedLaunchRollbackRecoversWhenStorageIsReleased)
{
    QFile reader;
    ASSERT_NO_FATAL_FAILURE(this->failBridgeLaunchWithBlockedRollback(reader));
    const int downloads = this->download.requestCount();
    reader.close();
    ASSERT_TRUE(spinUntil([&]() {
        return this->update.currentState().state ==
            (this->isHub ? QStringLiteral("AwaitingRestart") : QStringLiteral("AwaitingUserInstall"));
    }));
    EXPECT_TRUE(this->update.currentState().transactionId.isEmpty());
    EXPECT_EQ(this->download.requestCount(), downloads);
}

TYPED_TEST(UpdateRecoveryTest, FailedLaunchRollbackCannotOverwriteANewerTransaction)
{
    QFile reader;
    ASSERT_NO_FATAL_FAILURE(this->failBridgeLaunchWithBlockedRollback(reader));
    reader.close();
    auto newer = this->update.currentState();
    newer.transactionId = QStringLiteral("newer-transaction");
    const auto now = QDateTime::currentDateTimeUtc();
    ASSERT_TRUE(VNH::UpdateStateStore::save(newer, now));
    QElapsedTimer elapsed;
    elapsed.start();
    ASSERT_TRUE(spinUntil([&]() { return elapsed.elapsed() > 1500; }));
    EXPECT_EQ(this->update.currentState().transactionId, newer.transactionId);
    EXPECT_EQ(this->update.currentState().state, newer.state);
    EXPECT_EQ(this->update.currentState().updatedAt, now.toUTC().toString(Qt::ISODateWithMs));
}
#endif

TYPED_TEST(UpdateRecoveryTest, RequiresDurableStateBeforeDownload)
{
    const QString path = VNH::UpdateStateStore::filePath();
    ASSERT_TRUE(QDir().mkpath(path));
    QStringList announced;
    QObject observer;
    const auto connection = QObject::connect(
        &this->update, &TypeParam::stateChanged, &observer,
        [&announced](const VNH::UpdateStateRecord& record) {
            announced.append(record.state);
        });
    ASSERT_TRUE(this->checker->start(this->checkerOptions));
    ASSERT_TRUE(spinUntil([this]() { return !this->checker->isChecking(); }));
    EXPECT_TRUE(announced.isEmpty());
    EXPECT_EQ(this->download.requestCount(), 0);
    EXPECT_FALSE(this->update.currentState().found);
    QObject::disconnect(connection);

    ASSERT_TRUE(QDir().rmdir(path));
    ASSERT_TRUE(this->checker->checkNow());
    ASSERT_TRUE(spinUntil([this]() { return this->ready(); }));
}

TYPED_TEST(UpdateRecoveryTest, RetriesFailedDownloadWhenReleaseHasNotChanged)
{
    HarkenHubTest::RangeHttpServer::Script failure;
    failure.mode = HarkenHubTest::RangeHttpServer::Script::Mode::Status;
    failure.status = 401;
    this->download.enqueue(failure);
    ASSERT_TRUE(this->checker->start(this->checkerOptions));
    ASSERT_TRUE(spinUntil([this]() { return this->update.currentState().state == "DownloadFailed"; }));
    EXPECT_EQ(this->feed.requestCount(), 1);
    ASSERT_TRUE(this->checker->checkNow());
    ASSERT_TRUE(spinUntil([this]() { return this->ready(); }));
    EXPECT_EQ(this->download.requestCount(), 2);
    ASSERT_EQ(this->feed.requestCount(), 2);
    EXPECT_TRUE(this->feed.ifNoneMatchHistory().last().isEmpty());
}

TYPED_TEST(UpdateRecoveryTest, RecoversMissingPackageAndManifestAfterConditionalCheck)
{
    ASSERT_TRUE(this->checker->start(this->checkerOptions));
    ASSERT_TRUE(spinUntil([this]() { return this->ready(); }));
    const auto state = this->update.currentState();
    for (const auto& path : {state.downloadPath, state.manifestEnvelopePath}) {
        const int requests = this->download.requestCount();
        ASSERT_TRUE(QFile::remove(path));
        if constexpr (std::is_same_v<TypeParam, VNH::HubSelfUpdate>) {
            if (path == state.downloadPath) {
                QString error;
                EXPECT_FALSE(this->update.approve(&error, QDateTime::currentDateTimeUtc()));
                EXPECT_EQ(this->update.currentState().state, "VerificationFailed");
            }
        }
        ASSERT_TRUE(this->checker->checkNow());
        ASSERT_TRUE(spinUntil([this]() { return this->ready(); }));
        EXPECT_EQ(this->download.requestCount(), requests + 1);
        const auto headers = this->feed.ifNoneMatchHistory();
        ASSERT_GE(headers.size(), 2);
        EXPECT_FALSE(headers.at(headers.size() - 2).isEmpty());
        EXPECT_TRUE(headers.last().isEmpty());
    }
}

TYPED_TEST(UpdateRecoveryTest, RecoversMissingDownloadFromPersistedState)
{
    VNH::UpdateStateRecord record;
    record.product = this->isHub ? "HarkenHub" : "HarkenStudio";
    record.state = this->isHub ? "AwaitingRestart" : "InstallFailed";
    record.version = "1.4.0";
    record.buildId = "run12.abcd1234";
    record.releaseSequence = 7;
    ASSERT_TRUE(VNH::UpdateStateStore::save(record, QDateTime::currentDateTimeUtc()));
    ASSERT_TRUE(this->checker->start(this->checkerOptions));
    ASSERT_TRUE(spinUntil([this]() { return this->ready(); }));
    EXPECT_EQ(this->download.requestCount(), 1);
}

TYPED_TEST(UpdateRecoveryTest, DoesNotRedownloadRejectedPackageOnUnchangedFeed)
{
    ASSERT_TRUE(this->checker->start(this->checkerOptions));
    ASSERT_TRUE(spinUntil([this]() { return this->ready(); }));
    const auto state = this->update.currentState();
    QFile file(state.downloadPath);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const auto package = file.readAll();
    file.close();
    ASSERT_FALSE(package.isEmpty());
    auto tampered = package;
    tampered[0] = static_cast<char>(~tampered[0]);
    this->download.setPayload(tampered, "\"bad-package\"");
    ASSERT_TRUE(QFile::remove(state.downloadPath));
    ASSERT_TRUE(this->checker->checkNow());
    ASSERT_TRUE(spinUntil([this]() { return this->update.currentState().state == "VerificationFailed"; }));
    EXPECT_TRUE(this->update.currentState().downloadPath.isEmpty());

    const int downloads = this->download.requestCount();
    const int checks = this->feed.requestCount();
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt == 1)
            ASSERT_TRUE(QFile::remove(state.manifestEnvelopePath));
        ASSERT_TRUE(this->checker->checkNow());
        ASSERT_TRUE(spinUntil([this]() { return !this->checker->isChecking(); }));
        EXPECT_EQ(this->download.requestCount(), downloads);
        EXPECT_EQ(this->feed.requestCount(), checks + attempt + 1);
        EXPECT_EQ(this->update.currentState().state, "VerificationFailed");
    }

    this->download.setPayload(package, "\"repaired-package\"");
    ASSERT_TRUE(this->update.checkNow());
    ASSERT_TRUE(spinUntil([this]() { return this->ready(); }));
    EXPECT_EQ(this->download.requestCount(), downloads + 1);
}

TYPED_TEST(UpdateRecoveryTest, KeepsQueuedInstallWhenOriginalDownloadIsMissing)
{
    VNH::UpdateStateRecord record;
    record.product = this->isHub ? "HarkenHub" : "HarkenStudio";
    record.state = "AwaitingBridge";
    record.version = "1.4.0";
    record.buildId = "run12.abcd1234";
    record.releaseSequence = 7;
    ASSERT_TRUE(VNH::UpdateStateStore::save(record, QDateTime::currentDateTimeUtc()));
    ASSERT_TRUE(this->checker->start(this->checkerOptions));
    ASSERT_TRUE(spinUntil([this]() { return !this->checker->isChecking(); }));
    EXPECT_EQ(this->update.currentState().state, "AwaitingBridge");
    EXPECT_EQ(this->download.requestCount(), 0);
}

}
