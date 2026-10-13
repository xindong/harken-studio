#include "Transactions/TransactionReclaimer.h"

#include "Transactions/HandoffArea.h"
#include "Transactions/InstallExecutor.h"
#include "Transactions/TransactionJournal.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QLockFile>
#include <QRegularExpression>
#include <QSet>

namespace VNH {

namespace {

constexpr qint64 kUnknownHandoffMaxAgeMs = 24ll * 60 * 60 * 1000;

bool isTerminalState(const QString& state)
{
    return state == QStringLiteral("Failed") ||
           state == QStringLiteral("Installed") ||
           state == QStringLiteral("Reclaimed");
}

}

ReclaimReport TransactionReclaimer::reclaimAtStartup(const QDateTime& now)
{
    ReclaimReport report;

    QLockFile installLock(InstallExecutor::lockFilePath());
    installLock.setStaleLockTime(0);
    if (!installLock.tryLock(0))
        return report;
    report.locked = true;

    const QDir handoffDir(HandoffArea::rootDir());
    if (!handoffDir.exists())
        return report;

    static const QRegularExpression entryPattern(
        QStringLiteral("^([0-9a-f]{32})\\.(pkg|manifest|queue)$"));
    QHash<QString, QDateTime> newestByTransaction;
    const auto entries = handoffDir.entryInfoList(QDir::Files);
    for (const QFileInfo& info : entries) {
        const auto match = entryPattern.match(info.fileName());
        if (!match.hasMatch())
            continue;
        const QString transactionId = match.captured(1);
        const QDateTime modified = info.lastModified();
        const auto existing = newestByTransaction.constFind(transactionId);
        if (existing == newestByTransaction.constEnd() ||
            modified > existing.value())
            newestByTransaction.insert(transactionId, modified);
    }
    if (newestByTransaction.isEmpty())
        return report;

    const QHash<QString, QString> states =
        TransactionJournal::lastStateByTransaction();
    for (auto it = newestByTransaction.constBegin();
         it != newestByTransaction.constEnd(); ++it) {
        const QString& transactionId = it.key();
        const QString state = states.value(transactionId);
        const qint64 ageMs = it.value().msecsTo(now);
        if (!isTerminalState(state) && ageMs < kUnknownHandoffMaxAgeMs)
            continue;
        if (!HandoffArea::discard(transactionId))
            continue;
        if (!TransactionJournal::append(transactionId,
                                        QStringLiteral("HarkenStudio"), QString(),
                                        QStringLiteral("Reclaimed"), QString(),
                                        now))
            report.journalWriteFailures.append(transactionId);
        report.reclaimedTransactionIds.append(transactionId);
    }
    return report;
}

}
