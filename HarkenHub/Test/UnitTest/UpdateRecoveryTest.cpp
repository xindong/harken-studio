#include "Update/HubSelfUpdate.h"
#include "Update/UpdateCoordinator.h"
#include "Platform/PlatformPaths.h"
#include "Support/ManifestTestUtils.h"
#include "Support/RangeHttpServer.h"
#include "ZipTestUtils.h"

#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QDebug>
#include <QFileInfo>
#include <QTemporaryDir>
#include <functional>
#include <type_traits>

namespace {

std::function<void()> timeoutDiagnostics;

bool spinUntil(const std::function<bool()>& predicate)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 10000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    const bool complete = predicate();
    if (!complete && timeoutDiagnostics)
        timeoutDiagnostics();
    return complete;
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
        QObject::connect(checker, &VNH::UpdateChecker::checkFinished, checker,
                         [this](const VNH::UpdateCheckResult& result) {
                             lastCheck = result;
                         });
        timeoutDiagnostics = [this]() {
            const auto state = update.currentState();
            if constexpr (!isHub)
                qWarning() << "DIAGNOSTIC downloading=" << update.isDownloading();
            qWarning().noquote() << "DIAGNOSTIC state=" << state.state
                << "failure=" << state.failureCode
                << "checking=" << checker->isChecking()
                << "feedRequests=" << feed.requestCount()
                << "packageRequests=" << download.requestCount()
                << "lastCheckOk=" << lastCheck.ok
                << "lastCheckNotModified=" << lastCheck.notModified
                << "lastCheckError=" << lastCheck.error
                << "packageExists=" << QFileInfo::exists(state.downloadPath)
                << "manifestExists=" << QFileInfo::exists(state.manifestEnvelopePath);
            QFile persisted(VNH::UpdateStateStore::filePath());
            const bool opened = persisted.open(QIODevice::ReadOnly);
            qWarning().noquote() << "DIAGNOSTIC persistedOpen=" << opened
                << "persistedError=" << persisted.errorString()
                << "persistedBytes=" << (opened ? persisted.readAll() : QByteArray());
        };
    }

    void TearDown() override
    {
        timeoutDiagnostics = {};
        update.stop();
        qunsetenv("HARKEN_HUB_USER_ROOT");
        qunsetenv("HARKEN_HUB_MACHINE_ROOT");
        qunsetenv("HARKEN_HUB_BRIDGE_DISABLE");
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
    VNH::UpdateCheckResult lastCheck;
};

using Updaters = ::testing::Types<VNH::UpdateCoordinator, VNH::HubSelfUpdate>;
TYPED_TEST_SUITE(UpdateRecoveryTest, Updaters);

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
