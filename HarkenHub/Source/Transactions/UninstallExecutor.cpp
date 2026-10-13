#include "Transactions/UninstallExecutor.h"

#include "Platform/PlatformPaths.h"
#include "Platform/ProcessScan.h"
#include "Transactions/HandoffArea.h"
#include "Transactions/InstallExecutor.h"
#include "Transactions/InstallIndex.h"
#include "Transactions/InstallPackageCache.h"
#include "Transactions/TransactionJournal.h"
#include "Versions/VersionScanner.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>

namespace VNH {

namespace {

const QString kProduct = QStringLiteral("HarkenStudio");

bool isValidBuildId(const QString& buildId)
{
    if (buildId.isEmpty() || buildId.size() > 128)
        return false;
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]*$"));
    return pattern.match(buildId).hasMatch();
}

void sweepCondemnedDirs()
{
    static const QRegularExpression transactionPattern(
        QStringLiteral("^\\.uninstall-[0-9a-f]{32}$"));
    QDir versions(studioVersionsRoot());
    const auto entries = versions.entryList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QString& entry : entries) {
        if (transactionPattern.match(entry).hasMatch())
            QDir(versions.filePath(entry)).removeRecursively();
    }
}

QString manifestBuildIdIn(const QString& installPath)
{
    QFile file(installPath + QStringLiteral("/package-manifest.json"));
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return QString();
    return doc.object()[QStringLiteral("buildId")].toString();
}

UninstallOutcome fail(const QString& transactionId, const QString& buildId,
                      const QString& failureCode, const QString& error,
                      const QDateTime& now)
{
    TransactionJournal::append(transactionId, kProduct, buildId,
                               QStringLiteral("Failed"), failureCode, now);
    UninstallOutcome outcome;
    outcome.error = error;
    outcome.failureCode = failureCode;
    outcome.buildId = buildId;
    return outcome;
}

}

UninstallOutcome UninstallExecutor::run(const QString& buildId,
                                        const QDateTime& now)
{
    return run(buildId, now, [](const QString& directory) {
        return runningExecutablesUnder(directory);
    });
}

UninstallOutcome UninstallExecutor::run(const QString& buildId,
                                        const QDateTime& now,
                                        const ProcessProbe& probe)
{
    UninstallOutcome outcome;
    outcome.buildId = buildId;

    if (!isValidBuildId(buildId)) {
        outcome.error = QStringLiteral("Invalid build id");
        outcome.failureCode = QStringLiteral("InvalidBuildId");
        return outcome;
    }

    if (!QDir().mkpath(machineDataDir())) {
        outcome.error = QStringLiteral("Cannot create the machine data directory");
        outcome.failureCode = QStringLiteral("LockFailed");
        return outcome;
    }
    QLockFile installLock(InstallExecutor::lockFilePath());
    installLock.setStaleLockTime(0);
    if (!installLock.tryLock(0)) {
        outcome.error =
            QStringLiteral("Another install or uninstall transaction is running");
        outcome.failureCode = QStringLiteral("InstallBusy");
        return outcome;
    }

    sweepCondemnedDirs();

    const QString transactionId = HandoffArea::newTransactionId();

    const InstallIndexSnapshot index = InstallIndex::snapshot();
    if (!index.ok)
        return fail(transactionId, buildId, QStringLiteral("IndexCorrupted"),
                    index.error, now);

    const InstalledVersionRecord* record = nullptr;
    for (const auto& candidate : index.records) {
        if (candidate.buildId == buildId) {
            record = &candidate;
            break;
        }
    }
    if (!record)
        return fail(transactionId, buildId, QStringLiteral("UnknownBuild"),
                    QStringLiteral("No installed version matches build id %1")
                        .arg(buildId),
                    now);

    const QString installPath = record->installPath;
    const QFileInfo installInfo(installPath);
    if (!installInfo.exists() || !installInfo.isDir()) {
        QString indexError;
        if (!InstallIndex::removeRecord(buildId, &indexError))
            return fail(transactionId, buildId,
                        QStringLiteral("IndexCommitFailed"), indexError, now);
        TransactionJournal::append(transactionId, kProduct, buildId,
                                   QStringLiteral("Uninstalled"), QString(), now);
        outcome.ok = true;
        return outcome;
    }

    const QString canonicalRoot =
        QFileInfo(studioVersionsRoot()).canonicalFilePath();
    const QString canonicalInstall = installInfo.canonicalFilePath();
    if (canonicalRoot.isEmpty() || canonicalInstall.isEmpty() ||
        !canonicalInstall.startsWith(canonicalRoot + QStringLiteral("/")))
        return fail(transactionId, buildId, QStringLiteral("IdentityMismatch"),
                    QStringLiteral(
                        "Install path is outside the versions directory: %1")
                        .arg(installPath),
                    now);

    QString inspectionError;
    const auto issue = VersionScanner::inspect(*record, nullptr, &inspectionError);
    const QString manifestBuildId = manifestBuildIdIn(installPath);
    if (issue == InstallationIssue::IdentityMismatch || issue == InstallationIssue::Unreadable
        || (!manifestBuildId.isEmpty() && manifestBuildId != buildId))
        return fail(transactionId, buildId, QStringLiteral("IdentityMismatch"),
                    inspectionError.isEmpty()
                        ? QStringLiteral("Version directory identity disagrees with the install record")
                        : inspectionError,
                    now);

    const QStringList running = probe(installPath);
    if (!running.isEmpty())
        return fail(transactionId, buildId, QStringLiteral("VersionInUse"),
                    QStringLiteral(
                        "HarkenStudio is still running from this version: %1")
                        .arg(running.first()),
                    now);

    TransactionJournal::append(transactionId, kProduct, buildId,
                               QStringLiteral("Uninstalling"), QString(), now);

    const QString condemnedDir = studioVersionsRoot() +
                                 QStringLiteral("/.uninstall-") + transactionId;
    if (!QDir().rename(installPath, condemnedDir))
        return fail(transactionId, buildId, QStringLiteral("CommitFailed"),
                    QStringLiteral("Cannot detach the version directory"), now);

    if (!QDir(condemnedDir).removeRecursively()) {
        if (QDir().rename(condemnedDir, installPath)) {
            QString healthError;
            InstallIndex::setHealthState(buildId, QStringLiteral("NeedsRepair"),
                                         &healthError);
            return fail(transactionId, buildId, QStringLiteral("DeleteFailed"),
                        QStringLiteral("Cannot delete the version directory; "
                                       "the version was restored and marked "
                                       "NeedsRepair"),
                        now);
        }
        QString healthError;
        InstallIndex::setHealthState(buildId, QStringLiteral("NeedsRepair"),
                                     &healthError);
        return fail(transactionId, buildId, QStringLiteral("RestoreFailed"),
                    QStringLiteral("Cannot delete the detached directory or "
                                   "restore it; leftover: %1")
                        .arg(condemnedDir),
                    now);
    }

    QString indexError;
    if (!InstallIndex::removeRecord(buildId, &indexError))
        return fail(transactionId, buildId, QStringLiteral("IndexCommitFailed"),
                    indexError, now);
    InstallPackageCache::remove(buildId);

    TransactionJournal::append(transactionId, kProduct, buildId,
                               QStringLiteral("Uninstalled"), QString(), now);
    outcome.ok = true;
    return outcome;
}

}
