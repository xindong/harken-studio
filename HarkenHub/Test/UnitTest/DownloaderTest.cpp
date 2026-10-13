#include "Download/Downloader.h"

#include "Support/RangeHttpServer.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <functional>

using VNH::BackoffPolicy;
using VNH::Downloader;
using VNH::DownloadRequest;
using VNH::DownloadResult;
using HarkenHubTest::RangeHttpServer;

namespace {

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

QByteArray patternedPayload(int size)
{
    QByteArray data(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i)
        data[i] = char('A' + (i % 23));
    return data;
}

QByteArray readAll(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

bool writeFile(const QString& path, const QByteArray& content)
{
    QDir().mkpath(QFileInfo(path).dir().path());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(content) == content.size();
}

class ResultCapture
{
public:
    explicit ResultCapture(Downloader& downloader)
    {
        QObject::connect(&downloader, &Downloader::finished, &downloader,
                         [this](const DownloadResult& value) {
                             result = value;
                             ++finishedCount;
                         });
    }

    DownloadResult result;
    int finishedCount = 0;
};

class DownloaderTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        ASSERT_TRUE(server.listen());
    }

    DownloadRequest makeRequest(qint64 expectedSizeBytes = -1)
    {
        DownloadRequest request;
        request.url = server.packageUrl();
        request.targetPath = dir.filePath(QStringLiteral("pkg/studio.pkg"));
        request.expectedSizeBytes = expectedSizeBytes;
        request.maxAttempts = 4;
        request.transferTimeoutMs = 2000;
        return request;
    }

    static BackoffPolicy fastBackoff()
    {
        BackoffPolicy policy;
        policy.baseDelayMs = 1;
        policy.maxDelayMs = 4;
        return policy;
    }

    QTemporaryDir dir;
    RangeHttpServer server;
};

}

TEST_F(DownloaderTest, FreshDownloadDeliversTargetAndRemovesPartFiles)
{
    const QByteArray payload = patternedPayload(150000);
    server.setPayload(payload, QByteArrayLiteral("\"v1\""));

    Downloader downloader;
    downloader.setBackoffPolicy(fastBackoff());
    ResultCapture capture(downloader);
    const DownloadRequest request = makeRequest(payload.size());

    ASSERT_TRUE(downloader.start(request));
    ASSERT_TRUE(spinUntil([&]() { return capture.finishedCount == 1; }));

    EXPECT_TRUE(capture.result.ok) << capture.result.error.toStdString();
    EXPECT_EQ(capture.result.source, QStringLiteral("GitHub"));
    EXPECT_EQ(capture.result.attemptsUsed, 1);
    EXPECT_EQ(readAll(request.targetPath), payload);
    EXPECT_FALSE(QFile::exists(Downloader::partPath(request.targetPath)));
    EXPECT_FALSE(QFile::exists(Downloader::metaPath(request.targetPath)));
    EXPECT_EQ(server.requestCount(), 1);
    EXPECT_TRUE(server.rangeHistory().value(0).isEmpty());
    EXPECT_FALSE(downloader.isActive());
}

TEST_F(DownloaderTest, AbortedTransferResumesWithRangeAfterHeadCheck)
{
    const QByteArray payload = patternedPayload(200000);
    server.setPayload(payload, QByteArrayLiteral("\"v1\""));
    RangeHttpServer::Script abortScript;
    abortScript.mode = RangeHttpServer::Script::Mode::AbortAfterPrefix;
    abortScript.prefixBytes = 60000;
    server.enqueue(abortScript);

    Downloader downloader;
    downloader.setBackoffPolicy(fastBackoff());
    ResultCapture capture(downloader);
    const DownloadRequest request = makeRequest(payload.size());

    ASSERT_TRUE(downloader.start(request));
    ASSERT_TRUE(spinUntil([&]() { return capture.finishedCount == 1; }));

    EXPECT_TRUE(capture.result.ok) << capture.result.error.toStdString();
    EXPECT_EQ(capture.result.attemptsUsed, 2);
    EXPECT_EQ(readAll(request.targetPath), payload);
    ASSERT_EQ(server.requestCount(), 3);
    EXPECT_EQ(server.methodHistory().value(0), QByteArrayLiteral("GET"));
    EXPECT_EQ(server.methodHistory().value(1), QByteArrayLiteral("HEAD"));
    EXPECT_EQ(server.methodHistory().value(2), QByteArrayLiteral("GET"));
    EXPECT_TRUE(server.rangeHistory().value(0).isEmpty());
    EXPECT_TRUE(server.rangeHistory().value(2).startsWith("bytes="));
    EXPECT_TRUE(server.ifRangeHistory().value(2).isEmpty());
}

TEST_F(DownloaderTest, StoredMetaMismatchRestartsFromZero)
{
    const QByteArray payload = patternedPayload(120000);
    server.setPayload(payload, QByteArrayLiteral("\"new\""));

    Downloader downloader;
    downloader.setBackoffPolicy(fastBackoff());
    ResultCapture capture(downloader);
    const DownloadRequest request = makeRequest(payload.size());

    ASSERT_TRUE(writeFile(Downloader::partPath(request.targetPath),
                          QByteArray(10000, 'z')));
    ASSERT_TRUE(writeFile(
        Downloader::metaPath(request.targetPath),
        QJsonDocument(
            QJsonObject{
                {QStringLiteral("url"), request.url.toString()},
                {QStringLiteral("lastModified"),
                 QStringLiteral("Thu, 01 Jan 2026 00:00:00 GMT")},
                {QStringLiteral("totalSize"),
                 static_cast<qint64>(payload.size())},
            })
            .toJson(QJsonDocument::Compact)));

    ASSERT_TRUE(downloader.start(request));
    ASSERT_TRUE(spinUntil([&]() { return capture.finishedCount == 1; }));

    EXPECT_TRUE(capture.result.ok) << capture.result.error.toStdString();
    EXPECT_EQ(readAll(request.targetPath), payload);
    ASSERT_EQ(server.requestCount(), 2);
    EXPECT_EQ(server.methodHistory().value(0), QByteArrayLiteral("HEAD"));
    EXPECT_EQ(server.methodHistory().value(1), QByteArrayLiteral("GET"));
    EXPECT_TRUE(server.rangeHistory().value(1).isEmpty());
}

TEST_F(DownloaderTest, MatchingMetaResumesAfterHeadCheck)
{
    const QByteArray payload = patternedPayload(120000);
    server.setPayload(payload, QByteArrayLiteral("\"v1\""));

    Downloader downloader;
    downloader.setBackoffPolicy(fastBackoff());
    ResultCapture capture(downloader);
    const DownloadRequest request = makeRequest(payload.size());

    ASSERT_TRUE(writeFile(Downloader::partPath(request.targetPath),
                          payload.left(30000)));
    ASSERT_TRUE(writeFile(
        Downloader::metaPath(request.targetPath),
        QJsonDocument(
            QJsonObject{
                {QStringLiteral("url"), request.url.toString()},
                {QStringLiteral("lastModified"),
                 QStringLiteral("Fri, 24 Jul 2026 08:00:00 GMT")},
                {QStringLiteral("totalSize"),
                 static_cast<qint64>(payload.size())},
            })
            .toJson(QJsonDocument::Compact)));

    ASSERT_TRUE(downloader.start(request));
    ASSERT_TRUE(spinUntil([&]() { return capture.finishedCount == 1; }));

    EXPECT_TRUE(capture.result.ok) << capture.result.error.toStdString();
    EXPECT_EQ(readAll(request.targetPath), payload);
    ASSERT_EQ(server.requestCount(), 2);
    EXPECT_EQ(server.methodHistory().value(0), QByteArrayLiteral("HEAD"));
    EXPECT_EQ(server.rangeHistory().value(1),
              QByteArrayLiteral("bytes=30000-"));
}

TEST_F(DownloaderTest, ServerIgnoringRangeRestartsCleanly)
{
    const QByteArray payload = patternedPayload(120000);
    server.setPayload(payload, QByteArrayLiteral("\"v1\""));
    server.enqueue(RangeHttpServer::Script{});
    RangeHttpServer::Script ignoreScript;
    ignoreScript.mode = RangeHttpServer::Script::Mode::IgnoreRange;
    server.enqueue(ignoreScript);

    Downloader downloader;
    downloader.setBackoffPolicy(fastBackoff());
    ResultCapture capture(downloader);
    const DownloadRequest request = makeRequest(payload.size());

    ASSERT_TRUE(writeFile(Downloader::partPath(request.targetPath),
                          payload.left(30000)));
    ASSERT_TRUE(writeFile(
        Downloader::metaPath(request.targetPath),
        QJsonDocument(
            QJsonObject{
                {QStringLiteral("url"), request.url.toString()},
                {QStringLiteral("lastModified"),
                 QStringLiteral("Fri, 24 Jul 2026 08:00:00 GMT")},
                {QStringLiteral("totalSize"),
                 static_cast<qint64>(payload.size())},
            })
            .toJson(QJsonDocument::Compact)));

    ASSERT_TRUE(downloader.start(request));
    ASSERT_TRUE(spinUntil([&]() { return capture.finishedCount == 1; }));

    EXPECT_TRUE(capture.result.ok) << capture.result.error.toStdString();
    EXPECT_EQ(readAll(request.targetPath), payload);
    ASSERT_EQ(server.requestCount(), 2);
    EXPECT_EQ(server.rangeHistory().value(1),
              QByteArrayLiteral("bytes=30000-"));
}

TEST_F(DownloaderTest, ChangedResourceBetweenHeadAndRangeRestartsFromZero)
{
    const QByteArray payload = patternedPayload(120000);
    server.setPayload(payload, QByteArrayLiteral("\"v1\""));
    server.enqueue(RangeHttpServer::Script{});
    RangeHttpServer::Script changed;
    changed.lastModifiedOverride =
        QByteArrayLiteral("Sat, 25 Jul 2026 09:00:00 GMT");
    server.enqueue(changed);

    Downloader downloader;
    downloader.setBackoffPolicy(fastBackoff());
    ResultCapture capture(downloader);
    const DownloadRequest request = makeRequest(payload.size());

    ASSERT_TRUE(writeFile(Downloader::partPath(request.targetPath),
                          payload.left(30000)));
    ASSERT_TRUE(writeFile(
        Downloader::metaPath(request.targetPath),
        QJsonDocument(
            QJsonObject{
                {QStringLiteral("url"), request.url.toString()},
                {QStringLiteral("lastModified"),
                 QStringLiteral("Fri, 24 Jul 2026 08:00:00 GMT")},
                {QStringLiteral("totalSize"),
                 static_cast<qint64>(payload.size())},
            })
            .toJson(QJsonDocument::Compact)));

    ASSERT_TRUE(downloader.start(request));
    ASSERT_TRUE(spinUntil([&]() { return capture.finishedCount == 1; }));

    EXPECT_TRUE(capture.result.ok) << capture.result.error.toStdString();
    EXPECT_EQ(capture.result.attemptsUsed, 2);
    EXPECT_EQ(readAll(request.targetPath), payload);
    ASSERT_EQ(server.requestCount(), 3);
    EXPECT_EQ(server.methodHistory().value(0), QByteArrayLiteral("HEAD"));
    EXPECT_EQ(server.rangeHistory().value(1),
              QByteArrayLiteral("bytes=30000-"));
    EXPECT_TRUE(server.rangeHistory().value(2).isEmpty());
}

TEST_F(DownloaderTest, HeadFailureKeepsPartAndRetries)
{
    const QByteArray payload = patternedPayload(120000);
    server.setPayload(payload, QByteArrayLiteral("\"v1\""));
    RangeHttpServer::Script headFailure;
    headFailure.mode = RangeHttpServer::Script::Mode::Status;
    headFailure.status = 500;
    server.enqueue(headFailure);

    Downloader downloader;
    downloader.setBackoffPolicy(fastBackoff());
    ResultCapture capture(downloader);
    const DownloadRequest request = makeRequest(payload.size());

    ASSERT_TRUE(writeFile(Downloader::partPath(request.targetPath),
                          payload.left(30000)));
    ASSERT_TRUE(writeFile(
        Downloader::metaPath(request.targetPath),
        QJsonDocument(
            QJsonObject{
                {QStringLiteral("url"), request.url.toString()},
                {QStringLiteral("lastModified"),
                 QStringLiteral("Fri, 24 Jul 2026 08:00:00 GMT")},
                {QStringLiteral("totalSize"),
                 static_cast<qint64>(payload.size())},
            })
            .toJson(QJsonDocument::Compact)));

    ASSERT_TRUE(downloader.start(request));
    ASSERT_TRUE(spinUntil([&]() { return capture.finishedCount == 1; }));

    EXPECT_TRUE(capture.result.ok) << capture.result.error.toStdString();
    EXPECT_EQ(capture.result.attemptsUsed, 2);
    EXPECT_EQ(readAll(request.targetPath), payload);
    ASSERT_EQ(server.requestCount(), 3);
    EXPECT_EQ(server.methodHistory().value(0), QByteArrayLiteral("HEAD"));
    EXPECT_EQ(server.methodHistory().value(1), QByteArrayLiteral("HEAD"));
    EXPECT_EQ(server.rangeHistory().value(2),
              QByteArrayLiteral("bytes=30000-"));
}

TEST_F(DownloaderTest, RangeNotSatisfiableDiscardsBreakpointAndRefetches)
{
    const QByteArray payload = patternedPayload(120000);
    server.setPayload(payload, QByteArrayLiteral("\"v1\""));
    server.enqueue(RangeHttpServer::Script{});
    RangeHttpServer::Script rangeError;
    rangeError.mode = RangeHttpServer::Script::Mode::Status;
    rangeError.status = 416;
    server.enqueue(rangeError);

    Downloader downloader;
    downloader.setBackoffPolicy(fastBackoff());
    ResultCapture capture(downloader);
    const DownloadRequest request = makeRequest(payload.size());

    ASSERT_TRUE(writeFile(Downloader::partPath(request.targetPath),
                          payload.left(30000)));
    ASSERT_TRUE(writeFile(
        Downloader::metaPath(request.targetPath),
        QJsonDocument(
            QJsonObject{
                {QStringLiteral("url"), request.url.toString()},
                {QStringLiteral("lastModified"),
                 QStringLiteral("Fri, 24 Jul 2026 08:00:00 GMT")},
                {QStringLiteral("totalSize"),
                 static_cast<qint64>(payload.size())},
            })
            .toJson(QJsonDocument::Compact)));

    ASSERT_TRUE(downloader.start(request));
    ASSERT_TRUE(spinUntil([&]() { return capture.finishedCount == 1; }));

    EXPECT_TRUE(capture.result.ok) << capture.result.error.toStdString();
    EXPECT_EQ(capture.result.attemptsUsed, 2);
    EXPECT_EQ(readAll(request.targetPath), payload);
    ASSERT_EQ(server.requestCount(), 3);
    EXPECT_EQ(server.methodHistory().value(2), QByteArrayLiteral("GET"));
    EXPECT_TRUE(server.rangeHistory().value(2).isEmpty());
}

TEST_F(DownloaderTest, TransientServerErrorsAreRetried)
{
    const QByteArray payload = patternedPayload(50000);
    server.setPayload(payload, QByteArrayLiteral("\"v1\""));
    RangeHttpServer::Script first;
    first.mode = RangeHttpServer::Script::Mode::Status;
    first.status = 500;
    RangeHttpServer::Script second;
    second.mode = RangeHttpServer::Script::Mode::Status;
    second.status = 503;
    server.enqueue(first);
    server.enqueue(second);

    Downloader downloader;
    downloader.setBackoffPolicy(fastBackoff());
    ResultCapture capture(downloader);
    const DownloadRequest request = makeRequest(payload.size());

    ASSERT_TRUE(downloader.start(request));
    ASSERT_TRUE(spinUntil([&]() { return capture.finishedCount == 1; }));

    EXPECT_TRUE(capture.result.ok) << capture.result.error.toStdString();
    EXPECT_EQ(capture.result.attemptsUsed, 3);
    EXPECT_EQ(server.requestCount(), 3);
    EXPECT_EQ(readAll(request.targetPath), payload);
}

TEST_F(DownloaderTest, AvailabilityFailureSwitchesToFallbackSource)
{
    const QByteArray payload = patternedPayload(50000);
    RangeHttpServer fallback;
    ASSERT_TRUE(fallback.listen());
    fallback.setPayload(payload, QByteArrayLiteral("\"mirror\""));
    RangeHttpServer::Script unavailable;
    unavailable.mode = RangeHttpServer::Script::Mode::Status;
    unavailable.status = 503;
    server.enqueue(unavailable);

    Downloader downloader;
    downloader.setBackoffPolicy(fastBackoff());
    ResultCapture capture(downloader);
    DownloadRequest request = makeRequest(payload.size());
    request.fallbackUrls.append(fallback.packageUrl());

    ASSERT_TRUE(downloader.start(request));
    ASSERT_TRUE(spinUntil([&]() { return capture.finishedCount == 1; }));

    EXPECT_TRUE(capture.result.ok) << capture.result.error.toStdString();
    EXPECT_EQ(capture.result.source, QStringLiteral("OSS"));
    EXPECT_EQ(capture.result.attemptsUsed, 2);
    EXPECT_EQ(server.requestCount(), 1);
    EXPECT_EQ(fallback.requestCount(), 1);
    EXPECT_EQ(readAll(request.targetPath), payload);
}

TEST_F(DownloaderTest, UnapprovedRedirectDoesNotUseFallback)
{
    RangeHttpServer fallback;
    ASSERT_TRUE(fallback.listen());
    fallback.setPayload(QByteArrayLiteral("fallback"),
                        QByteArrayLiteral("\"mirror\""));
    RangeHttpServer::Script redirect;
    redirect.mode = RangeHttpServer::Script::Mode::Redirect;
    redirect.redirectLocation = fallback.packageUrl().toString().toUtf8();
    server.enqueue(redirect);

    Downloader downloader;
    ResultCapture capture(downloader);
    DownloadRequest request = makeRequest();
    request.fallbackUrls.append(fallback.packageUrl());

    ASSERT_TRUE(downloader.start(request));
    ASSERT_TRUE(spinUntil([&]() { return capture.finishedCount == 1; }));

    EXPECT_FALSE(capture.result.ok);
    EXPECT_TRUE(capture.result.error.contains(QStringLiteral("redirect")));
    EXPECT_EQ(server.requestCount(), 1);
    EXPECT_EQ(fallback.requestCount(), 0);
}

TEST_F(DownloaderTest, HeadAndGetRequestsAreAnonymous)
{
    const QByteArray payload = patternedPayload(60000);
    server.setPayload(payload, QByteArrayLiteral("\"v1\""));

    Downloader downloader;
    downloader.setBackoffPolicy(fastBackoff());
    ResultCapture capture(downloader);
    DownloadRequest request = makeRequest(payload.size());

    ASSERT_TRUE(writeFile(Downloader::partPath(request.targetPath),
                          payload.left(20000)));
    ASSERT_TRUE(writeFile(
        Downloader::metaPath(request.targetPath),
        QJsonDocument(
            QJsonObject{
                {QStringLiteral("url"), request.url.toString()},
                {QStringLiteral("lastModified"),
                 QStringLiteral("Fri, 24 Jul 2026 08:00:00 GMT")},
                {QStringLiteral("totalSize"),
                 static_cast<qint64>(payload.size())},
            })
            .toJson(QJsonDocument::Compact)));

    ASSERT_TRUE(downloader.start(request));
    ASSERT_TRUE(spinUntil([&]() { return capture.finishedCount == 1; }));

    EXPECT_TRUE(capture.result.ok) << capture.result.error.toStdString();
    ASSERT_EQ(server.requestCount(), 2);
    EXPECT_TRUE(server.authorizationHistory().value(0).isEmpty());
    EXPECT_TRUE(server.authorizationHistory().value(1).isEmpty());
}

TEST_F(DownloaderTest, UnauthorizedFailureSurfacesHttpStatus)
{
    server.setPayload(patternedPayload(1000), QByteArrayLiteral("\"v1\""));
    RangeHttpServer::Script unauthorized;
    unauthorized.mode = RangeHttpServer::Script::Mode::Status;
    unauthorized.status = 401;
    server.enqueue(unauthorized);

    Downloader downloader;
    downloader.setBackoffPolicy(fastBackoff());
    ResultCapture capture(downloader);

    ASSERT_TRUE(downloader.start(makeRequest()));
    ASSERT_TRUE(spinUntil([&]() { return capture.finishedCount == 1; }));

    EXPECT_FALSE(capture.result.ok);
    EXPECT_EQ(capture.result.httpStatus, 401);
    EXPECT_EQ(capture.result.attemptsUsed, 1);
}

TEST_F(DownloaderTest, ClientHttpErrorFailsWithoutRetry)
{
    server.setPayload(patternedPayload(1000), QByteArrayLiteral("\"v1\""));
    RangeHttpServer::Script notFound;
    notFound.mode = RangeHttpServer::Script::Mode::Status;
    notFound.status = 404;
    server.enqueue(notFound);

    Downloader downloader;
    downloader.setBackoffPolicy(fastBackoff());
    ResultCapture capture(downloader);

    ASSERT_TRUE(downloader.start(makeRequest()));
    ASSERT_TRUE(spinUntil([&]() { return capture.finishedCount == 1; }));

    EXPECT_FALSE(capture.result.ok);
    EXPECT_EQ(capture.result.attemptsUsed, 1);
    EXPECT_EQ(server.requestCount(), 1);
    EXPECT_TRUE(capture.result.error.contains(QStringLiteral("404")));
}

TEST_F(DownloaderTest, SizeMismatchFailsAndDiscardsPartFiles)
{
    const QByteArray payload = patternedPayload(40000);
    server.setPayload(payload, QByteArrayLiteral("\"v1\""));

    Downloader downloader;
    downloader.setBackoffPolicy(fastBackoff());
    ResultCapture capture(downloader);
    const DownloadRequest request = makeRequest(payload.size() + 7);

    ASSERT_TRUE(downloader.start(request));
    ASSERT_TRUE(spinUntil([&]() { return capture.finishedCount == 1; }));

    EXPECT_FALSE(capture.result.ok);
    EXPECT_FALSE(QFile::exists(request.targetPath));
    EXPECT_FALSE(QFile::exists(Downloader::partPath(request.targetPath)));
    EXPECT_FALSE(QFile::exists(Downloader::metaPath(request.targetPath)));
    EXPECT_EQ(server.requestCount(), 1);
}

TEST_F(DownloaderTest, InsufficientDiskSpaceFailsBeforeAnyRequest)
{
    server.setPayload(patternedPayload(1000), QByteArrayLiteral("\"v1\""));

    Downloader downloader;
    downloader.setBackoffPolicy(fastBackoff());
    ResultCapture capture(downloader);

    ASSERT_TRUE(downloader.start(makeRequest(1ll << 60)));
    ASSERT_TRUE(spinUntil([&]() { return capture.finishedCount == 1; }));

    EXPECT_FALSE(capture.result.ok);
    EXPECT_TRUE(capture.result.error.contains(QStringLiteral("disk"),
                                              Qt::CaseInsensitive));
    EXPECT_EQ(server.requestCount(), 0);
}

TEST_F(DownloaderTest, StartWhileActiveIsRejected)
{
    const QByteArray payload = patternedPayload(20000);
    server.setPayload(payload, QByteArrayLiteral("\"v1\""));
    RangeHttpServer::Script delayed;
    delayed.delayMs = 200;
    server.enqueue(delayed);

    Downloader downloader;
    downloader.setBackoffPolicy(fastBackoff());
    ResultCapture capture(downloader);
    const DownloadRequest request = makeRequest(payload.size());

    ASSERT_TRUE(downloader.start(request));
    EXPECT_TRUE(downloader.isActive());
    EXPECT_FALSE(downloader.start(request));

    ASSERT_TRUE(spinUntil([&]() { return capture.finishedCount == 1; }));
    EXPECT_TRUE(capture.result.ok) << capture.result.error.toStdString();
    EXPECT_EQ(server.requestCount(), 1);
}

TEST_F(DownloaderTest, CancelDuringBackoffFinishesImmediately)
{
    server.setPayload(patternedPayload(1000), QByteArrayLiteral("\"v1\""));
    RangeHttpServer::Script failure;
    failure.mode = RangeHttpServer::Script::Mode::Status;
    failure.status = 500;
    server.enqueue(failure);

    Downloader downloader;
    BackoffPolicy slowBackoff;
    slowBackoff.baseDelayMs = 5000;
    slowBackoff.maxDelayMs = 5000;
    slowBackoff.jitterFactor = 0.0;
    downloader.setBackoffPolicy(slowBackoff);
    ResultCapture capture(downloader);

    ASSERT_TRUE(downloader.start(makeRequest()));
    ASSERT_TRUE(spinUntil([&]() { return server.requestCount() == 1; }));
    spinFor(150);
    downloader.cancel();

    ASSERT_TRUE(spinUntil([&]() { return capture.finishedCount == 1; }, 1000));
    EXPECT_FALSE(capture.result.ok);
    EXPECT_EQ(server.requestCount(), 1);
    EXPECT_FALSE(downloader.isActive());
}

TEST_F(DownloaderTest, ExistingTargetIsReplacedWithoutResidue)
{
    const QByteArray payload = patternedPayload(80000);
    server.setPayload(payload, QByteArrayLiteral("\"v2\""));

    Downloader downloader;
    downloader.setBackoffPolicy(fastBackoff());
    ResultCapture capture(downloader);
    const DownloadRequest request = makeRequest(payload.size());

    ASSERT_TRUE(writeFile(request.targetPath, QByteArray(5000, 'o')));

    ASSERT_TRUE(downloader.start(request));
    ASSERT_TRUE(spinUntil([&]() { return capture.finishedCount == 1; }));

    EXPECT_TRUE(capture.result.ok) << capture.result.error.toStdString();
    EXPECT_EQ(readAll(request.targetPath), payload);
    EXPECT_FALSE(QFile::exists(request.targetPath + QStringLiteral(".stale")));
    EXPECT_FALSE(QFile::exists(Downloader::partPath(request.targetPath)));
}

TEST_F(DownloaderTest, CancelStopsWithoutRetry)
{
    server.setPayload(patternedPayload(20000), QByteArrayLiteral("\"v1\""));
    RangeHttpServer::Script delayed;
    delayed.delayMs = 300;
    server.enqueue(delayed);

    Downloader downloader;
    downloader.setBackoffPolicy(fastBackoff());
    ResultCapture capture(downloader);

    ASSERT_TRUE(downloader.start(makeRequest()));
    ASSERT_TRUE(spinUntil([&]() { return server.requestCount() == 1; }));
    downloader.cancel();

    ASSERT_TRUE(spinUntil([&]() { return capture.finishedCount == 1; }));
    EXPECT_FALSE(capture.result.ok);
    EXPECT_EQ(capture.result.attemptsUsed, 1);
    EXPECT_EQ(server.requestCount(), 1);
    EXPECT_FALSE(downloader.isActive());
}
