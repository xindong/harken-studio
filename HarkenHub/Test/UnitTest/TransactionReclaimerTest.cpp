#include "Transactions/TransactionReclaimer.h"

#include "Platform/PlatformPaths.h"
#include "Transactions/HandoffArea.h"
#include "Transactions/InstallExecutor.h"
#include "Transactions/InstallIndex.h"
#include "Transactions/TransactionDiagnostics.h"
#include "Transactions/TransactionJournal.h"

#include <gtest/gtest.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QLockFile>
#include <QTemporaryDir>

using VNH::HandoffArea;
using VNH::TransactionJournal;
using VNH::TransactionReclaimer;

namespace {

const QDateTime kNow =
    QDateTime::fromString(QStringLiteral("2026-07-23T12:00:00Z"), Qt::ISODate);

class MachineRootGuard
{
public:
    explicit MachineRootGuard(const QString& root)
    {
        qputenv("HARKEN_HUB_MACHINE_ROOT", root.toUtf8());
    }
    ~MachineRootGuard() { qunsetenv("HARKEN_HUB_MACHINE_ROOT"); }
};

QString placeTransaction(const QTemporaryDir& dir)
{
    const QString source = dir.filePath(QStringLiteral("pkg.bin"));
    QFile file(source);
    if (!file.open(QIODevice::WriteOnly))
        return QString();
    file.write(QByteArrayLiteral("package-data"));
    file.close();
    const auto placement =
        HandoffArea::place(source, QByteArrayLiteral("{\"buildId\":\"x\"}"));
    return placement.ok ? placement.transactionId : QString();
}

bool handoffExists(const QString& transactionId)
{
    return QFile::exists(HandoffArea::packagePath(transactionId)) ||
           QFile::exists(HandoffArea::manifestPath(transactionId));
}

}

TEST(TransactionReclaimer, FailedTransactionIsReclaimedImmediately)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    MachineRootGuard guard(dir.path());

    const QString transactionId = placeTransaction(dir);
    ASSERT_FALSE(transactionId.isEmpty());
    ASSERT_TRUE(TransactionJournal::append(
        transactionId, QStringLiteral("HarkenStudio"), QString(),
        QStringLiteral("Failed"), QStringLiteral("PackageRejected"), kNow,
        QStringLiteral("Package SHA-256 mismatch")));

    const auto report = TransactionReclaimer::reclaimAtStartup(kNow);
    EXPECT_TRUE(report.locked);
    EXPECT_EQ(report.reclaimedTransactionIds, QStringList{transactionId});
    EXPECT_FALSE(handoffExists(transactionId));
    EXPECT_EQ(TransactionJournal::lastStateByTransaction().value(transactionId),
              QStringLiteral("Reclaimed"));

    const auto summaries = VNH::TransactionDiagnostics::summarize();
    ASSERT_EQ(summaries.size(), 1u);
    EXPECT_EQ(summaries.front().lastState, QStringLiteral("Reclaimed"));
    EXPECT_EQ(summaries.front().failureCode, QStringLiteral("PackageRejected"));
    EXPECT_EQ(summaries.front().failureDetail, QStringLiteral("Package SHA-256 mismatch"));
    const auto json = VNH::TransactionDiagnostics::toJson(summaries);
    EXPECT_EQ(json[0].toObject().value(QStringLiteral("failureDetail")).toString(),
              QStringLiteral("Package SHA-256 mismatch"));
    EXPECT_TRUE(VNH::TransactionDiagnostics::failureMessage(transactionId)
                    .contains(QStringLiteral("Package SHA-256 mismatch")));
}

TEST(TransactionReclaimer, InvalidInstallIndexDoesNotPreventFinishedHandoffCleanup)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    MachineRootGuard guard(dir.path());
    const QString transactionId = placeTransaction(dir);
    ASSERT_FALSE(transactionId.isEmpty());
    ASSERT_TRUE(TransactionJournal::append(transactionId, QStringLiteral("HarkenStudio"),
        QString(), QStringLiteral("Failed"), QStringLiteral("IndexCorrupted"), kNow));
    QFile index(VNH::InstallIndex::filePath());
    ASSERT_TRUE(index.open(QIODevice::WriteOnly));
    index.write("{broken");
    index.close();
    const auto report = TransactionReclaimer::reclaimAtStartup(kNow);
    EXPECT_FALSE(report.installRecoveryErrors.isEmpty());
    EXPECT_EQ(report.reclaimedTransactionIds, QStringList{transactionId});
    EXPECT_FALSE(handoffExists(transactionId));
    ASSERT_TRUE(index.open(QIODevice::ReadOnly));
    EXPECT_EQ(index.readAll(), "{broken");
}

TEST(TransactionReclaimer, YoungUnknownTransactionIsKept)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    MachineRootGuard guard(dir.path());

    const QString transactionId = placeTransaction(dir);
    ASSERT_FALSE(transactionId.isEmpty());

    const auto report =
        TransactionReclaimer::reclaimAtStartup(QDateTime::currentDateTimeUtc());
    EXPECT_TRUE(report.locked);
    EXPECT_TRUE(report.reclaimedTransactionIds.isEmpty());
    EXPECT_TRUE(handoffExists(transactionId));
}

TEST(TransactionReclaimer, OldUnknownTransactionIsReclaimed)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    MachineRootGuard guard(dir.path());

    const QString transactionId = placeTransaction(dir);
    ASSERT_FALSE(transactionId.isEmpty());

    const auto report = TransactionReclaimer::reclaimAtStartup(
        QDateTime::currentDateTimeUtc().addDays(2));
    EXPECT_TRUE(report.locked);
    EXPECT_EQ(report.reclaimedTransactionIds, QStringList{transactionId});
    EXPECT_FALSE(handoffExists(transactionId));
    EXPECT_TRUE(VNH::TransactionDiagnostics::failureMessage(transactionId).isEmpty());
}

TEST(TransactionReclaimer, NonTerminalRecentJournalStateIsKept)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    MachineRootGuard guard(dir.path());

    const QString transactionId = placeTransaction(dir);
    ASSERT_FALSE(transactionId.isEmpty());
    ASSERT_TRUE(TransactionJournal::append(
        transactionId, QStringLiteral("HarkenStudio"), QString(),
        QStringLiteral("Verifying"), QString(),
        QDateTime::currentDateTimeUtc()));

    const auto report =
        TransactionReclaimer::reclaimAtStartup(QDateTime::currentDateTimeUtc());
    EXPECT_TRUE(report.locked);
    EXPECT_TRUE(report.reclaimedTransactionIds.isEmpty());
    EXPECT_TRUE(handoffExists(transactionId));
}

TEST(TransactionReclaimer, HeldInstallLockSkipsReclamation)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    MachineRootGuard guard(dir.path());

    const QString transactionId = placeTransaction(dir);
    ASSERT_FALSE(transactionId.isEmpty());
    ASSERT_TRUE(TransactionJournal::append(
        transactionId, QStringLiteral("HarkenStudio"), QString(),
        QStringLiteral("Failed"), QString(), kNow));

    QLockFile heldLock(VNH::InstallExecutor::lockFilePath());
    heldLock.setStaleLockTime(0);
    ASSERT_TRUE(heldLock.tryLock(0));

    const auto report = TransactionReclaimer::reclaimAtStartup(kNow);
    EXPECT_FALSE(report.locked);
    EXPECT_TRUE(report.reclaimedTransactionIds.isEmpty());
    EXPECT_TRUE(handoffExists(transactionId));
}

TEST(TransactionReclaimer, FutureTimestampIsKeptUntilRealAgingCatchesUp)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    MachineRootGuard guard(dir.path());

    const QString transactionId = placeTransaction(dir);
    ASSERT_FALSE(transactionId.isEmpty());
    const QDateTime future = QDateTime::currentDateTimeUtc().addDays(3);
    for (const QString& path : {HandoffArea::packagePath(transactionId),
                                HandoffArea::manifestPath(transactionId)}) {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::ReadWrite));
        ASSERT_TRUE(file.setFileTime(future, QFileDevice::FileModificationTime));
    }

    const auto kept =
        TransactionReclaimer::reclaimAtStartup(QDateTime::currentDateTimeUtc());
    EXPECT_TRUE(kept.locked);
    EXPECT_TRUE(kept.reclaimedTransactionIds.isEmpty());
    EXPECT_TRUE(handoffExists(transactionId));

    const auto aged = TransactionReclaimer::reclaimAtStartup(
        QDateTime::currentDateTimeUtc().addDays(5));
    EXPECT_TRUE(aged.locked);
    EXPECT_EQ(aged.reclaimedTransactionIds, QStringList{transactionId});
    EXPECT_FALSE(handoffExists(transactionId));
}

TEST(TransactionReclaimer, JournalWriteFailureIsReportedHonestly)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    MachineRootGuard guard(dir.path());

    const QString transactionId = placeTransaction(dir);
    ASSERT_FALSE(transactionId.isEmpty());
    ASSERT_TRUE(TransactionJournal::append(
        transactionId, QStringLiteral("HarkenStudio"), QString(),
        QStringLiteral("Failed"), QString(), kNow));

    const QString journalPath = TransactionJournal::filePath();
    ASSERT_TRUE(QFile::remove(journalPath));
    ASSERT_TRUE(QDir().mkpath(journalPath));

    const auto report = TransactionReclaimer::reclaimAtStartup(
        QDateTime::currentDateTimeUtc().addDays(2));
    EXPECT_TRUE(report.locked);
    EXPECT_EQ(report.reclaimedTransactionIds, QStringList{transactionId});
    EXPECT_EQ(report.journalWriteFailures, QStringList{transactionId});
    EXPECT_FALSE(handoffExists(transactionId));
}

TEST(TransactionReclaimer, JournalTracksLastStatePerTransaction)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    MachineRootGuard guard(dir.path());

    ASSERT_TRUE(TransactionJournal::append(
        QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        QStringLiteral("HarkenStudio"), QString(), QStringLiteral("Verifying"),
        QString(), kNow));
    ASSERT_TRUE(TransactionJournal::append(
        QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        QStringLiteral("HarkenStudio"), QString(), QStringLiteral("Installed"),
        QString(), kNow));
    ASSERT_TRUE(TransactionJournal::append(
        QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"),
        QStringLiteral("HarkenStudio"), QString(), QStringLiteral("Failed"),
        QStringLiteral("InstallBusy"), kNow));

    const auto states = TransactionJournal::lastStateByTransaction();
    EXPECT_EQ(states.value(QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")),
              QStringLiteral("Installed"));
    EXPECT_EQ(states.value(QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")),
              QStringLiteral("Failed"));
}
