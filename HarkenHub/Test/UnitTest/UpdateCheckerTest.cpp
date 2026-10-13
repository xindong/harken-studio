#include "Update/UpdateChecker.h"

#include "Support/ManifestTestUtils.h"
#include "Support/RangeHttpServer.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>

#include <functional>

using VNH::UpdateChecker;
using VNH::UpdateCheckResult;
using HarkenHubTest::baseManifestPayload;
using HarkenHubTest::latestResponseJson;
using HarkenHubTest::makeKeypair;
using HarkenHubTest::RangeHttpServer;
using HarkenHubTest::signEnvelope;
using HarkenHubTest::TestKeypair;

namespace {

TEST(UpdateCheckerDefaults, PollsHourly)
{
    EXPECT_EQ(UpdateChecker::Options().intervalMs, 60ll * 60 * 1000);
}

bool spinUntil(const std::function<bool()>& predicate, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

void spinFor(int durationMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < durationMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

class ResultCapture
{
public:
    explicit ResultCapture(UpdateChecker& checker)
    {
        QObject::connect(&checker, &UpdateChecker::checkFinished, &checker,
                         [this](const UpdateCheckResult& value) {
                             results.append(value);
                         });
    }

    QList<UpdateCheckResult> results;
};

QByteArray releaseResponseJson(const QList<QJsonObject>& payloads,
                               const TestKeypair& keys)
{
    QJsonArray versions;
    for (const QJsonObject& payload : payloads) {
        QJsonObject entry;
        const QString version = payload[QStringLiteral("version")].toString();
        entry[QStringLiteral("version")] = version;
        entry[QStringLiteral("file")] =
            QStringLiteral("release/harken/%1/%2/run12-abcd1234-20261008T100000Z/Harken.zip")
                .arg(VNH::ServerEndpoints::hostChannel(), version);
        entry[QStringLiteral("integrity")] = QJsonDocument::fromJson(
            signEnvelope(
                QJsonDocument(payload).toJson(QJsonDocument::Compact), keys))
                                                  .object();
        versions.append(entry);
    }
    QJsonObject channels;
    channels[VNH::ServerEndpoints::hostChannel()] = versions;
    QJsonObject response;
    response[QStringLiteral("schema_version")] = 1;
    response[QStringLiteral("channels")] = channels;
    return QJsonDocument(response).toJson(QJsonDocument::Compact);
}

class UpdateCheckerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(server.listen());
        keys = makeKeypair();
        setManifest(7);
    }

    void setManifest(qint64 releaseSequence, const QByteArray& etag =
                                                 QByteArrayLiteral("\"m1\""))
    {
        QJsonObject payload = baseManifestPayload();
        payload[QStringLiteral("releaseSequence")] =
            static_cast<double>(releaseSequence);
        server.setPayload(
            latestResponseJson(signEnvelope(
                QJsonDocument(payload).toJson(QJsonDocument::Compact), keys)),
            etag);
    }

    UpdateChecker::Options makeOptions()
    {
        UpdateChecker::Options options;
        options.latestUrl = server.packageUrl();
        options.product = QStringLiteral("harken");
        options.channel = VNH::ServerEndpoints::hostChannel();
        options.manifestPublicKey = keys.publicKey;
        options.transferTimeoutMs = 2000;
        return options;
    }

    static VNH::BackoffPolicy fastBackoff()
    {
        VNH::BackoffPolicy policy;
        policy.baseDelayMs = 1;
        policy.maxDelayMs = 4;
        return policy;
    }

    RangeHttpServer server;
    TestKeypair keys;
};

}

TEST_F(UpdateCheckerTest, FirstCheckFetchesAndVerifiesManifest)
{
    UpdateChecker checker;
    ResultCapture capture(checker);

    ASSERT_TRUE(checker.start(makeOptions()));
    ASSERT_TRUE(spinUntil([&]() { return capture.results.size() == 1; }));

    const auto& result = capture.results[0];
    EXPECT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_FALSE(result.notModified);
    EXPECT_EQ(result.source, QStringLiteral("GitHub"));
    EXPECT_EQ(result.manifest.releaseSequence, 7);
    EXPECT_EQ(result.manifest.buildId, QStringLiteral("run12.abcd1234"));
    EXPECT_FALSE(result.envelope.isEmpty());
    EXPECT_TRUE(server.ifNoneMatchHistory().value(0).isEmpty());
    checker.stop();
}

TEST_F(UpdateCheckerTest, UnchangedManifestYieldsNotModified)
{
    UpdateChecker checker;
    ResultCapture capture(checker);

    ASSERT_TRUE(checker.start(makeOptions()));
    ASSERT_TRUE(spinUntil([&]() { return capture.results.size() == 1; }));
    ASSERT_TRUE(checker.checkNow());
    ASSERT_TRUE(spinUntil([&]() { return capture.results.size() == 2; }));

    EXPECT_TRUE(capture.results[1].ok);
    EXPECT_TRUE(capture.results[1].notModified);
    EXPECT_EQ(server.requestCount(), 2);
    EXPECT_EQ(server.ifNoneMatchHistory().value(1), QByteArrayLiteral("\"m1\""));
    checker.stop();
}

TEST_F(UpdateCheckerTest, ChangedManifestIsRefetchedAndReverified)
{
    UpdateChecker checker;
    ResultCapture capture(checker);

    ASSERT_TRUE(checker.start(makeOptions()));
    ASSERT_TRUE(spinUntil([&]() { return capture.results.size() == 1; }));

    setManifest(9, QByteArrayLiteral("\"m2\""));
    ASSERT_TRUE(checker.checkNow());
    ASSERT_TRUE(spinUntil([&]() { return capture.results.size() == 2; }));

    EXPECT_TRUE(capture.results[1].ok);
    EXPECT_FALSE(capture.results[1].notModified);
    EXPECT_EQ(capture.results[1].manifest.releaseSequence, 9);
    checker.stop();
}

TEST_F(UpdateCheckerTest, InvalidateDuringActiveRoundForcesUnconditionalFollowUp)
{
    UpdateChecker checker;
    ResultCapture capture(checker);

    ASSERT_TRUE(checker.start(makeOptions()));
    ASSERT_TRUE(spinUntil([&]() { return capture.results.size() == 1; }));

    RangeHttpServer::Script delayed;
    delayed.delayMs = 200;
    server.enqueue(delayed);

    ASSERT_TRUE(checker.checkNow());
    ASSERT_TRUE(spinUntil([&]() { return server.requestCount() == 2; }));
    ASSERT_TRUE(checker.isChecking());
    checker.invalidateCache();

    ASSERT_TRUE(spinUntil([&]() { return capture.results.size() == 3; }));
    EXPECT_TRUE(capture.results[1].ok);
    EXPECT_TRUE(capture.results[1].notModified);
    EXPECT_TRUE(capture.results[2].ok);
    EXPECT_FALSE(capture.results[2].notModified);
    EXPECT_EQ(capture.results[2].manifest.releaseSequence, 7);
    EXPECT_EQ(server.requestCount(), 3);
    EXPECT_TRUE(server.ifNoneMatchHistory().value(2).isEmpty());
    checker.stop();
}

TEST_F(UpdateCheckerTest, InvalidateWhileIdleDoesNotScheduleExtraRound)
{
    UpdateChecker checker;
    ResultCapture capture(checker);

    ASSERT_TRUE(checker.start(makeOptions()));
    ASSERT_TRUE(spinUntil([&]() { return capture.results.size() == 1; }));

    checker.invalidateCache();
    spinFor(300);
    EXPECT_EQ(server.requestCount(), 1);

    ASSERT_TRUE(checker.checkNow());
    ASSERT_TRUE(spinUntil([&]() { return capture.results.size() == 2; }));
    EXPECT_FALSE(capture.results[1].notModified);
    EXPECT_TRUE(server.ifNoneMatchHistory().value(1).isEmpty());
    checker.stop();
}

TEST_F(UpdateCheckerTest, OverlappingRoundsArePrevented)
{
    RangeHttpServer::Script delayed;
    delayed.delayMs = 200;
    server.enqueue(delayed);

    UpdateChecker checker;
    ResultCapture capture(checker);

    ASSERT_TRUE(checker.start(makeOptions()));
    ASSERT_TRUE(spinUntil([&]() { return server.requestCount() == 1; }));
    EXPECT_TRUE(checker.isChecking());
    EXPECT_FALSE(checker.checkNow());

    ASSERT_TRUE(spinUntil([&]() { return capture.results.size() == 1; }));
    EXPECT_EQ(server.requestCount(), 1);
    checker.stop();
}

TEST_F(UpdateCheckerTest, FailureRetriesWithBackoffWithoutWaitingFullInterval)
{
    RangeHttpServer::Script failure;
    failure.mode = RangeHttpServer::Script::Mode::Status;
    failure.status = 500;
    server.enqueue(failure);

    UpdateChecker checker;
    checker.setBackoffPolicy(fastBackoff());
    ResultCapture capture(checker);

    ASSERT_TRUE(checker.start(makeOptions()));
    ASSERT_TRUE(spinUntil([&]() { return capture.results.size() == 2; }));

    EXPECT_FALSE(capture.results[0].ok);
    EXPECT_TRUE(capture.results[0].error.contains(QStringLiteral("500")));
    EXPECT_TRUE(capture.results[1].ok);
    EXPECT_EQ(server.requestCount(), 2);
    checker.stop();
}

TEST_F(UpdateCheckerTest, AvailabilityFailureUsesFallbackInTheSameRound)
{
    RangeHttpServer fallback;
    ASSERT_TRUE(fallback.listen());
    QJsonObject payload = baseManifestPayload();
    payload[QStringLiteral("releaseSequence")] = 8;
    fallback.setPayload(
        latestResponseJson(signEnvelope(
            QJsonDocument(payload).toJson(QJsonDocument::Compact), keys)),
        QByteArrayLiteral("\"fallback\""));

    RangeHttpServer::Script unavailable;
    unavailable.mode = RangeHttpServer::Script::Mode::Status;
    unavailable.status = 503;
    server.enqueue(unavailable);

    auto options = makeOptions();
    options.fallbackUrls.append(fallback.packageUrl());
    UpdateChecker checker;
    ResultCapture capture(checker);
    ASSERT_TRUE(checker.start(options));
    ASSERT_TRUE(spinUntil([&]() { return capture.results.size() == 1; }));

    EXPECT_TRUE(capture.results[0].ok)
        << capture.results[0].error.toStdString();
    EXPECT_EQ(capture.results[0].manifest.releaseSequence, 8);
    EXPECT_EQ(capture.results[0].source, QStringLiteral("OSS"));
    EXPECT_EQ(server.requestCount(), 1);
    EXPECT_EQ(fallback.requestCount(), 1);
    checker.stop();
}

TEST_F(UpdateCheckerTest, MissingPrimaryFallsBackForBothProducts)
{
    for (const QString& product : {QStringLiteral("hub"), QStringLiteral("harken")}) {
        RangeHttpServer fallback;
        ASSERT_TRUE(fallback.listen());
        QJsonObject payload = baseManifestPayload();
        payload[QStringLiteral("product")] = product == QStringLiteral("hub")
            ? QStringLiteral("HarkenHub") : QStringLiteral("HarkenStudio");
        QByteArray response = latestResponseJson(signEnvelope(
            QJsonDocument(payload).toJson(QJsonDocument::Compact), keys));
        response.replace("release/harken/", ("release/" + product + "/").toUtf8());
        fallback.setPayload(response, QByteArrayLiteral("\"fallback\""));
        RangeHttpServer::Script unavailable;
        unavailable.mode = RangeHttpServer::Script::Mode::Status;
        unavailable.status = 404;
        server.enqueue(unavailable);
        auto options = makeOptions();
        options.product = product;
        options.fallbackUrls.append(fallback.packageUrl());
        UpdateChecker checker;
        ResultCapture capture(checker);
        ASSERT_TRUE(checker.start(options));
        ASSERT_TRUE(spinUntil([&]() { return capture.results.size() == 1; }));
        EXPECT_TRUE(capture.results[0].ok)
            << capture.results[0].error.toStdString();
        EXPECT_EQ(capture.results[0].manifest.product, payload["product"].toString());
        EXPECT_EQ(fallback.requestCount(), 1);
        checker.stop();
    }
}

TEST_F(UpdateCheckerTest, InvalidPrimaryManifestDoesNotUseFallback)
{
    RangeHttpServer fallback;
    ASSERT_TRUE(fallback.listen());
    fallback.setPayload(latestResponseJson(QByteArray()),
                        QByteArrayLiteral("\"fallback\""));
    server.setPayload(QByteArrayLiteral("not json"),
                      QByteArrayLiteral("\"invalid\""));

    auto options = makeOptions();
    options.fallbackUrls.append(fallback.packageUrl());
    UpdateChecker checker;
    ResultCapture capture(checker);
    ASSERT_TRUE(checker.start(options));
    ASSERT_TRUE(spinUntil([&]() { return capture.results.size() == 1; }));

    EXPECT_FALSE(capture.results[0].ok);
    EXPECT_TRUE(capture.results[0].error.contains(QStringLiteral("JSON")));
    EXPECT_EQ(server.requestCount(), 1);
    EXPECT_EQ(fallback.requestCount(), 0);
    checker.stop();
}

TEST_F(UpdateCheckerTest, TamperedManifestIsRejected)
{
    QJsonObject payload = baseManifestPayload();
    server.setPayload(
        latestResponseJson(signEnvelope(
            QJsonDocument(payload).toJson(QJsonDocument::Compact),
            makeKeypair(0x24))),
        QByteArrayLiteral("\"evil\""));

    UpdateChecker checker;
    checker.setBackoffPolicy(fastBackoff());
    ResultCapture capture(checker);

    ASSERT_TRUE(checker.start(makeOptions()));
    ASSERT_TRUE(spinUntil([&]() { return !capture.results.isEmpty(); }));
    checker.stop();

    EXPECT_FALSE(capture.results[0].ok);
    EXPECT_TRUE(
        capture.results[0].error.contains(QStringLiteral("signature")));
}

TEST_F(UpdateCheckerTest, RejectsFeedThatIsNotOrderedNewestFirst)
{
    QJsonObject older = baseManifestPayload();
    older[QStringLiteral("releaseSequence")] = 7;
    QJsonObject newer = baseManifestPayload();
    newer[QStringLiteral("version")] = QStringLiteral("1.5.0");
    newer[QStringLiteral("releaseSequence")] = 9;
    server.setPayload(releaseResponseJson({older, newer}, keys),
                      QByteArrayLiteral("\"unordered\""));

    UpdateChecker checker;
    ResultCapture capture(checker);
    ASSERT_TRUE(checker.start(makeOptions()));
    ASSERT_TRUE(spinUntil([&]() { return !capture.results.isEmpty(); }));
    checker.stop();

    EXPECT_FALSE(capture.results[0].ok);
    EXPECT_TRUE(capture.results[0].error.contains(
        QStringLiteral("ordered newest first")));
}

TEST_F(UpdateCheckerTest, AcceptsExpiredHistoryBehindCurrentRelease)
{
    QJsonObject current = baseManifestPayload();
    current[QStringLiteral("version")] = QStringLiteral("1.5.0");
    current[QStringLiteral("releaseSequence")] = 9;
    QJsonObject historical = baseManifestPayload();
    historical[QStringLiteral("releaseSequence")] = 7;
    historical[QStringLiteral("expiresAt")] =
        QStringLiteral("2026-07-24T00:00:00Z");
    server.setPayload(releaseResponseJson({current, historical}, keys),
                      QByteArrayLiteral("\"history\""));

    UpdateChecker checker;
    ResultCapture capture(checker);
    ASSERT_TRUE(checker.start(makeOptions()));
    ASSERT_TRUE(spinUntil([&]() { return !capture.results.isEmpty(); }));
    checker.stop();

    EXPECT_TRUE(capture.results[0].ok)
        << capture.results[0].error.toStdString();
    EXPECT_EQ(capture.results[0].manifest.releaseSequence, 9);
}

TEST_F(UpdateCheckerTest, IgnoresInvalidEntryBehindCurrentRelease)
{
    QJsonObject current = baseManifestPayload();
    current[QStringLiteral("version")] = QStringLiteral("1.5.0");
    current[QStringLiteral("releaseSequence")] = 9;
    QJsonObject historical = baseManifestPayload();
    historical[QStringLiteral("releaseSequence")] = 7;
    QJsonObject response = QJsonDocument::fromJson(
        releaseResponseJson({current, historical}, keys)).object();
    QJsonObject channels = response[QStringLiteral("channels")].toObject();
    QJsonArray versions =
        channels[VNH::ServerEndpoints::hostChannel()].toArray();
    QJsonObject invalidHistory = versions[1].toObject();
    invalidHistory[QStringLiteral("integrity")] = QJsonObject();
    versions[1] = invalidHistory;
    channels[VNH::ServerEndpoints::hostChannel()] = versions;
    response[QStringLiteral("channels")] = channels;
    server.setPayload(QJsonDocument(response).toJson(QJsonDocument::Compact),
                      QByteArrayLiteral("\"invalid-history\""));

    UpdateChecker checker;
    ResultCapture capture(checker);
    ASSERT_TRUE(checker.start(makeOptions()));
    ASSERT_TRUE(spinUntil([&]() { return !capture.results.isEmpty(); }));
    checker.stop();

    EXPECT_TRUE(capture.results[0].ok)
        << capture.results[0].error.toStdString();
    EXPECT_EQ(capture.results[0].manifest.releaseSequence, 9);
}

TEST_F(UpdateCheckerTest, UnsignedVersionMismatchIsRejected)
{
    QJsonObject payload = baseManifestPayload();
    const QByteArray envelope = signEnvelope(
        QJsonDocument(payload).toJson(QJsonDocument::Compact), keys);
    QJsonObject response = QJsonDocument::fromJson(
        latestResponseJson(envelope)).object();
    QJsonObject channels = response[QStringLiteral("channels")].toObject();
    QJsonArray versions = channels[VNH::ServerEndpoints::hostChannel()].toArray();
    QJsonObject entry = versions[0].toObject();
    entry[QStringLiteral("version")] = QStringLiteral("9.9.9");
    versions[0] = entry;
    channels[VNH::ServerEndpoints::hostChannel()] = versions;
    response[QStringLiteral("channels")] = channels;
    server.setPayload(QJsonDocument(response).toJson(QJsonDocument::Compact),
                      QByteArrayLiteral("\"mismatch\""));

    UpdateChecker checker;
    ResultCapture capture(checker);
    ASSERT_TRUE(checker.start(makeOptions()));
    ASSERT_TRUE(spinUntil([&]() { return !capture.results.isEmpty(); }));
    checker.stop();

    EXPECT_FALSE(capture.results[0].ok);
    EXPECT_TRUE(capture.results[0].error.contains(QStringLiteral("version")));
}

TEST_F(UpdateCheckerTest, ObjectPathOutsideSignedReleaseIsRejected)
{
    QJsonObject payload = baseManifestPayload();
    const QByteArray envelope = signEnvelope(
        QJsonDocument(payload).toJson(QJsonDocument::Compact), keys);
    QJsonObject response = QJsonDocument::fromJson(
        latestResponseJson(envelope)).object();
    QJsonObject channels = response[QStringLiteral("channels")].toObject();
    QJsonArray versions = channels[VNH::ServerEndpoints::hostChannel()].toArray();
    QJsonObject entry = versions[0].toObject();
    entry[QStringLiteral("file")] =
        QStringLiteral("release/hub/windows-x64/1.4.0/run12.abcd1234/Harken.zip");
    versions[0] = entry;
    channels[VNH::ServerEndpoints::hostChannel()] = versions;
    response[QStringLiteral("channels")] = channels;
    server.setPayload(QJsonDocument(response).toJson(QJsonDocument::Compact),
                      QByteArrayLiteral("\"outside\""));

    UpdateChecker checker;
    ResultCapture capture(checker);
    ASSERT_TRUE(checker.start(makeOptions()));
    ASSERT_TRUE(spinUntil([&]() { return !capture.results.isEmpty(); }));
    checker.stop();

    EXPECT_FALSE(capture.results[0].ok);
    EXPECT_TRUE(capture.results[0].error.contains(QStringLiteral("object path")));
}

TEST_F(UpdateCheckerTest, UnexpectedNotModifiedOnFirstRoundIsError)
{
    RangeHttpServer::Script forced;
    forced.mode = RangeHttpServer::Script::Mode::ForceNotModified;
    server.enqueue(forced);

    UpdateChecker checker;
    checker.setBackoffPolicy(fastBackoff());
    ResultCapture capture(checker);

    ASSERT_TRUE(checker.start(makeOptions()));
    ASSERT_TRUE(spinUntil([&]() { return !capture.results.isEmpty(); }));
    checker.stop();

    EXPECT_FALSE(capture.results[0].ok);
    EXPECT_FALSE(capture.results[0].notModified);
    EXPECT_TRUE(capture.results[0].error.contains(QStringLiteral("304")));
}

TEST_F(UpdateCheckerTest, OversizedResponseIsAbortedAsFailure)
{
    server.setPayload(QByteArray(2 * 1024 * 1024, 'x'),
                      QByteArrayLiteral("\"big\""));

    UpdateChecker checker;
    checker.setBackoffPolicy(fastBackoff());
    ResultCapture capture(checker);

    ASSERT_TRUE(checker.start(makeOptions()));
    ASSERT_TRUE(spinUntil([&]() { return !capture.results.isEmpty(); }));

    EXPECT_FALSE(capture.results[0].ok);
    EXPECT_TRUE(capture.results[0].error.contains(QStringLiteral("exceeds")));
    checker.stop();
}

TEST_F(UpdateCheckerTest, RemotePlaintextUrlIsRejected)
{
    UpdateChecker checker;
    auto options = makeOptions();

    options.latestUrl = QUrl(QStringLiteral("http://updates.example/m.json"));
    EXPECT_FALSE(checker.start(options));

    options.latestUrl = QUrl(QStringLiteral("file:///C:/manifest.json"));
    EXPECT_FALSE(checker.start(options));

    options.latestUrl = QUrl(QStringLiteral("https://updates.example/m.json"));
    EXPECT_TRUE(checker.start(options));
    checker.stop();
}

TEST_F(UpdateCheckerTest, TrustedHttpOriginUnlocksPlaintextBase)
{
    auto options = makeOptions();
    options.latestUrl = QUrl(QStringLiteral(
        "http://172.20.1.9:8000/api/harken/client/harken/latest/windows-x64"));

    UpdateChecker rejected;
    EXPECT_FALSE(rejected.start(options));

    options.trustedHttpOrigin =
        QUrl(QStringLiteral("http://172.20.1.9:9000"));
    UpdateChecker wrongPort;
    EXPECT_FALSE(wrongPort.start(options));

    options.trustedHttpOrigin =
        QUrl(QStringLiteral("http://172.20.1.9:8000"));
    UpdateChecker accepted;
    EXPECT_TRUE(accepted.start(options));
    accepted.stop();
}

TEST_F(UpdateCheckerTest, EmptyUpdateManifestIsSkippedSilently)
{
    server.setPayload(latestResponseJson(QByteArray()),
                      QByteArrayLiteral("\"m8\""));

    UpdateChecker checker;
    ResultCapture capture(checker);

    ASSERT_TRUE(checker.start(makeOptions()));
    ASSERT_TRUE(spinUntil([&]() { return capture.results.size() == 1; }));

    const auto& result = capture.results[0];
    EXPECT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_TRUE(result.manifestAbsent);
    EXPECT_FALSE(result.notModified);
    EXPECT_TRUE(result.envelope.isEmpty());
    checker.stop();
}

TEST_F(UpdateCheckerTest, MissingChannelOr404IsSilentNoUpdate)
{
    RangeHttpServer::Script notFound;
    notFound.mode = RangeHttpServer::Script::Mode::Status;
    notFound.status = 404;
    server.enqueue(notFound);

    UpdateChecker checker;
    ResultCapture capture(checker);

    ASSERT_TRUE(checker.start(makeOptions()));
    ASSERT_TRUE(spinUntil([&]() { return capture.results.size() == 1; }));

    EXPECT_TRUE(capture.results[0].ok);
    EXPECT_TRUE(capture.results[0].manifestAbsent);
    EXPECT_TRUE(capture.results[0].error.isEmpty());
    checker.stop();
}

TEST_F(UpdateCheckerTest, NonJsonResponseIsError)
{
    server.setPayload(QByteArrayLiteral("<html>gateway error</html>"),
                      QByteArrayLiteral("\"m7\""));

    UpdateChecker checker;
    checker.setBackoffPolicy(fastBackoff());
    ResultCapture capture(checker);

    ASSERT_TRUE(checker.start(makeOptions()));
    ASSERT_TRUE(spinUntil([&]() { return !capture.results.isEmpty(); }));

    EXPECT_FALSE(capture.results[0].ok);
    EXPECT_TRUE(capture.results[0].error.contains(QStringLiteral("JSON")));
    checker.stop();
}

TEST_F(UpdateCheckerTest, UnsupportedFeedSchemaIsRejected)
{
    server.setPayload(
        QByteArrayLiteral("{\"schema_version\":2,\"channels\":{}}"),
        QByteArrayLiteral("\"schema2\""));

    UpdateChecker checker;
    ResultCapture capture(checker);
    ASSERT_TRUE(checker.start(makeOptions()));
    ASSERT_TRUE(spinUntil([&]() { return !capture.results.isEmpty(); }));
    checker.stop();

    EXPECT_FALSE(capture.results[0].ok);
    EXPECT_TRUE(capture.results[0].error.contains(QStringLiteral("schema")));
}

TEST_F(UpdateCheckerTest, RedirectsAreNotFollowed)
{
    RangeHttpServer::Script redirect;
    redirect.mode = RangeHttpServer::Script::Mode::Redirect;
    redirect.redirectLocation =
        QByteArrayLiteral("http://update-attacker.example/manifest");
    server.enqueue(redirect);

    // Default (slow) backoff: a fast retry could land a second request
    // inside the event pump before the requestCount assertion runs.
    UpdateChecker checker;
    ResultCapture capture(checker);

    ASSERT_TRUE(checker.start(makeOptions()));
    ASSERT_TRUE(spinUntil([&]() { return !capture.results.isEmpty(); }));

    EXPECT_FALSE(capture.results[0].ok);
    EXPECT_TRUE(capture.results[0].error.contains(QStringLiteral("302")));
    EXPECT_EQ(server.requestCount(), 1);
    checker.stop();
}

TEST_F(UpdateCheckerTest, IntervalIsClampedToTheFloor)
{
    UpdateChecker checker;
    auto options = makeOptions();
    options.intervalMs = 1000;

    ASSERT_TRUE(checker.start(options));
    EXPECT_EQ(checker.effectiveIntervalMs(), UpdateChecker::kMinIntervalMs);
    checker.stop();
}

TEST_F(UpdateCheckerTest, StopAbortsInFlightRoundSilently)
{
    RangeHttpServer::Script delayed;
    delayed.delayMs = 300;
    server.enqueue(delayed);

    UpdateChecker checker;
    ResultCapture capture(checker);

    ASSERT_TRUE(checker.start(makeOptions()));
    ASSERT_TRUE(spinUntil([&]() { return server.requestCount() == 1; }));
    checker.stop();
    EXPECT_FALSE(checker.isChecking());

    spinFor(400);
    EXPECT_TRUE(capture.results.isEmpty());
    EXPECT_FALSE(checker.checkNow());
}

TEST_F(UpdateCheckerTest, StartRejectsInvalidConfiguration)
{
    UpdateChecker checker;
    UpdateChecker::Options options;
    EXPECT_FALSE(checker.start(options));

    ASSERT_TRUE(checker.start(makeOptions()));
    EXPECT_FALSE(checker.start(makeOptions()));
    checker.stop();
}
