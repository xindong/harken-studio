#include "Update/UpdateQueue.h"

#include "Transactions/HandoffArea.h"
#include "Transactions/TransactionReclaimer.h"
#include "Platform/UserSession.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <memory>

using VNH::HandoffArea;
using VNH::UpdateQueue;

namespace {

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

class UpdateQueueTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        machineGuard = std::make_unique<EnvGuard>(
            "HARKEN_HUB_MACHINE_ROOT", dir.path());
        ASSERT_TRUE(QDir().mkpath(HandoffArea::rootDir()));
    }

    QTemporaryDir dir;
    std::unique_ptr<EnvGuard> machineGuard;
};

}

TEST_F(UpdateQueueTest, MarksAndListsStudioInstall)
{
    const QString transactionId = HandoffArea::newTransactionId();
    QString error;
    ASSERT_TRUE(UpdateQueue::markStudioInstall(transactionId, &error))
        << error.toStdString();

    const auto entries = UpdateQueue::pendingEntries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].transactionId, transactionId);
    EXPECT_EQ(entries[0].product, QStringLiteral("HarkenStudio"));
    EXPECT_FALSE(UpdateQueue::hasPendingHubUpdate());
}

TEST_F(UpdateQueueTest, MarksHubInstallWithRestartMode)
{
    const QString transactionId = HandoffArea::newTransactionId();
    QString error;
    ASSERT_TRUE(UpdateQueue::markHubInstall(
        transactionId, QStringLiteral("gui"),
        QStringLiteral("S-1-5-21-test-user"), &error))
        << error.toStdString();

    const auto entries = UpdateQueue::pendingEntries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].product, QStringLiteral("HarkenHub"));
    EXPECT_EQ(entries[0].restartMode, QStringLiteral("gui"));
    EXPECT_EQ(entries[0].userId, QStringLiteral("S-1-5-21-test-user"));
    EXPECT_TRUE(UpdateQueue::hasPendingHubUpdate());
}

TEST_F(UpdateQueueTest, RejectsInvalidRestartMode)
{
    QString error;
    EXPECT_FALSE(UpdateQueue::markHubInstall(
        HandoffArea::newTransactionId(), QStringLiteral("elevated"),
        QString(), &error));
}

TEST_F(UpdateQueueTest, RejectsDuplicateMarker)
{
    const QString transactionId = HandoffArea::newTransactionId();
    QString error;
    ASSERT_TRUE(UpdateQueue::markStudioInstall(transactionId, &error));
    EXPECT_FALSE(UpdateQueue::markStudioInstall(transactionId, &error));
}

TEST_F(UpdateQueueTest, IgnoresMalformedMarkers)
{
    const QString transactionId = HandoffArea::newTransactionId();
    QFile marker(HandoffArea::queuePath(transactionId));
    ASSERT_TRUE(marker.open(QIODevice::WriteOnly));
    marker.write(QByteArrayLiteral("not json"));
    marker.close();

    QFile foreign(HandoffArea::rootDir() + QStringLiteral("/not-a-txid.queue"));
    ASSERT_TRUE(foreign.open(QIODevice::WriteOnly));
    foreign.write(QByteArrayLiteral("{\"product\":\"HarkenHub\"}"));
    foreign.close();

    EXPECT_TRUE(UpdateQueue::pendingEntries().empty());
    EXPECT_FALSE(UpdateQueue::hasPendingHubUpdate());
}

TEST_F(UpdateQueueTest, DiscardRemovesQueueMarker)
{
    const QString transactionId = HandoffArea::newTransactionId();
    QString error;
    ASSERT_TRUE(UpdateQueue::markStudioInstall(transactionId, &error));
    ASSERT_TRUE(QFile::exists(HandoffArea::queuePath(transactionId)));

    EXPECT_TRUE(HandoffArea::discard(transactionId));
    EXPECT_FALSE(QFile::exists(HandoffArea::queuePath(transactionId)));
    EXPECT_TRUE(UpdateQueue::pendingEntries().empty());
}

TEST_F(UpdateQueueTest, CompletedResultSurvivesHandoffCleanupUntilAcknowledged)
{
    const auto transaction = HandoffArea::newTransactionId();
    VNH::InstallOutcome outcome;
    outcome.failureCode = "PackageRejected";
    outcome.error = "Package verification failed";
    QString error;
    ASSERT_TRUE(UpdateQueue::markStudioInstall(transaction, &error));
    ASSERT_TRUE(UpdateQueue::writeStudioResult(transaction, outcome, VNH::currentUserId(), &error));
    ASSERT_TRUE(HandoffArea::discard(transaction));
    VNH::TransactionReclaimer::reclaimAtStartup(QDateTime::currentDateTimeUtc().addDays(2));
    const auto retained = UpdateQueue::studioResult(transaction, &error);
    ASSERT_TRUE(retained.has_value()) << error.toStdString();
    EXPECT_FALSE(retained->ok);
    EXPECT_EQ(retained->failureCode, outcome.failureCode);
    EXPECT_EQ(retained->error, outcome.error);
    EXPECT_FALSE(UpdateQueue::studioResult(HandoffArea::newTransactionId(), &error).has_value());
    EXPECT_TRUE(error.isEmpty());
    ASSERT_TRUE(UpdateQueue::discardStudioResult(transaction));
    EXPECT_FALSE(UpdateQueue::studioResult(transaction, &error).has_value());
}

TEST_F(UpdateQueueTest, InvalidOrForeignResultsCannotReplaceATransactionsOutcome)
{
    const auto transaction = HandoffArea::newTransactionId();
    VNH::InstallOutcome outcome;
    outcome.failureCode = "PackageRejected";
    QString error;
    ASSERT_TRUE(UpdateQueue::writeStudioResult(transaction, outcome, VNH::currentUserId(), &error));
    auto invalid = outcome;
    invalid.failureCode = "InstallBusy";
    EXPECT_FALSE(UpdateQueue::writeStudioResult(transaction, invalid, VNH::currentUserId(), &error));
    EXPECT_FALSE(UpdateQueue::writeStudioResult("../outside", outcome, VNH::currentUserId(), &error));
    ASSERT_EQ(UpdateQueue::studioResult(transaction, &error)->failureCode, outcome.failureCode);
    const auto path = HandoffArea::rootDir() + QLatin1Char('/') + transaction + ".result";
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const auto original = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    for (const char* field : {"transactionId", "product", "state", "failureCode", "buildId"}) {
        auto malformed = original;
        malformed[QLatin1String(field)] = field == QByteArray("buildId") ? QJsonValue(42) : QJsonValue("foreign");
        if (field == QByteArray("failureCode")) malformed["failureCode"] = "";
        const auto bytes = QJsonDocument(malformed).toJson();
        ASSERT_TRUE(QFile::remove(path));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(bytes);
        file.close();
        EXPECT_FALSE(UpdateQueue::studioResult(transaction, &error).has_value());
        EXPECT_FALSE(error.isEmpty());
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        EXPECT_EQ(file.readAll(), bytes);
        file.close();
    }
}
