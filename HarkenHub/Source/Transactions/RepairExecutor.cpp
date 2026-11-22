#include "Transactions/RepairExecutor.h"

#include "Packaging/PackageReader.h"
#include "Packaging/PackageVerifier.h"
#include "Platform/PlatformPaths.h"
#include "Platform/ProcessScan.h"
#include "Security/BinaryTrustPolicy.h"
#include "Security/ManifestVerifier.h"
#include "Security/TrustAnchors.h"
#include "Transactions/HandoffArea.h"
#include "Transactions/InstallExecutor.h"
#include "Transactions/InstallIndex.h"
#include "Transactions/InstallPackageCache.h"
#include "Transactions/TransactionJournal.h"
#include "Versions/IntegrityChecker.h"
#include "Versions/VersionScanner.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLockFile>

namespace harken::hub {

namespace {

const QString kProduct = QStringLiteral("HarkenStudio");

RepairOutcome fail(const QString& transactionId, const QString& buildId,
                   const QString& failureCode, const QString& error,
                   const QDateTime& now)
{
    TransactionJournal::append(transactionId, kProduct, buildId,
                               QStringLiteral("Failed"), failureCode, now, error);
    RepairOutcome outcome;
    outcome.error = error;
    outcome.failureCode = failureCode;
    outcome.buildId = buildId;
    outcome.healthState = QStringLiteral("NeedsRepair");
    return outcome;
}

QByteArray readAll(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

bool isInstallPathSafe(const QString& installPath)
{
    const QFileInfo rootInfo(studioVersionsRoot());
    const QFileInfo installInfo(installPath);
    return QDir::cleanPath(installInfo.absolutePath()) ==
               QDir::cleanPath(rootInfo.absoluteFilePath()) &&
           !installInfo.fileName().startsWith(QLatin1Char('.'));
}

bool expectedHashesFor(const QString& packagePath,
                       const PackageVerifyResult& packageResult,
                       QHash<QString, QString>* hashes, QString* error)
{
    *hashes = packageResult.fileSha256ByPath;
    for (const QString& metadataName : packageResult.metadataEntries) {
        QByteArray bytes;
        if (!PackageReader::readEntry(packagePath, metadataName, &bytes, error))
            return false;
        hashes->insert(
            metadataName,
            QString::fromLatin1(
                QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
                    .toHex()));
    }
    return true;
}

}

RepairSource RepairExecutor::verifySource(const QString& buildId, const QDateTime& now)
{
    RepairSource source;
    if (!ManifestVerifier::isSafeFileToken(buildId)) {
        source.error = QStringLiteral("Invalid build id");
        return source;
    }
    source.packagePath = InstallPackageCache::packagePath(buildId);
    if (!QFileInfo(source.packagePath).isFile() || !QFileInfo(InstallPackageCache::manifestPath(buildId)).isFile()) {
        source.error = QStringLiteral("Verified repair package is unavailable");
        source.failureCode = QStringLiteral("RepairSourceMissing");
        return source;
    }
    const auto envelope = readAll(InstallPackageCache::manifestPath(buildId));
    const auto manifest = ManifestVerifier::verify(envelope, TrustAnchors::manifestPublicKey(), now, true);
    if (!manifest.ok || manifest.manifest.product != kProduct || manifest.manifest.buildId != buildId) {
        source.error = manifest.ok ? QStringLiteral("Repair manifest identity mismatch") : manifest.error;
        return source;
    }
    source.manifestHash = QString::fromLatin1(QCryptographicHash::hash(envelope, QCryptographicHash::Sha256).toHex());
    const auto& release = manifest.manifest;
    const auto package = PackageVerifier::verify(source.packagePath,
        {buildId, release.version, release.sha256Hex, release.packageSize});
    if (!package.ok || package.channel != release.channel) {
        source.error = package.ok ? QStringLiteral("Repair package channel mismatch") : package.error;
        return source;
    }
    source.ok = expectedHashesFor(source.packagePath, package, &source.files, &source.error);
    if (source.ok && PackageReader::fileSha256Hex(source.packagePath).compare(release.sha256Hex, Qt::CaseInsensitive) != 0) {
        source.ok = false;
        source.error = QStringLiteral("The repair package changed during verification");
    }
    return source;
}

RepairOutcome RepairExecutor::run(const QString& buildId, const QDateTime& now,
                                  const QString& transactionId, const QString& reviewedSnapshot, const QString& reviewedManifestHash)
{
    return run(buildId, now, [](const QString& path) { return runningExecutablesUnder(path); }, transactionId, reviewedSnapshot, reviewedManifestHash);
}

RepairOutcome RepairExecutor::run(const QString& buildId, const QDateTime& now,
                                  const ProcessProbe& probe, const QString& requestedTransactionId, const QString& reviewedSnapshot, const QString& reviewedManifestHash)
{
    RepairOutcome outcome;
    outcome.buildId = buildId;

    const QString transactionId = requestedTransactionId.isEmpty()
        ? HandoffArea::newTransactionId() : requestedTransactionId;
    if (!HandoffArea::isValidTransactionId(transactionId)) {
        outcome.error = QStringLiteral("Invalid repair transaction id");
        outcome.failureCode = QStringLiteral("InvalidTransactionId");
        return outcome;
    }

    if (!ManifestVerifier::isSafeFileToken(buildId)) {
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
        return fail(transactionId, buildId, QStringLiteral("InstallBusy"),
                    QStringLiteral("Another install transaction is running"), now);
    }

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
    if (!isInstallPathSafe(record->installPath))
        return fail(transactionId, buildId, QStringLiteral("IdentityMismatch"),
                    QStringLiteral("Install path is outside the versions root"),
                    now);

    if (!reviewedSnapshot.isEmpty() && IntegrityChecker::snapshot(record->installPath).hash != reviewedSnapshot)
        return fail(transactionId, buildId, QStringLiteral("ReviewChanged"),
                    QStringLiteral("Installed files changed; review the operation again"), now);

    TransactionJournal::append(transactionId, kProduct, buildId,
                               QStringLiteral("Repairing"), QString(), now);

    const IntegrityReport current = IntegrityChecker::check(
        record->installPath, record->packageManifestHash);
    if (current.ok) {
        QString identityError;
        if (VersionScanner::inspect(*record, nullptr, &identityError) != InstallationIssue::None)
            return fail(transactionId, buildId, QStringLiteral("IdentityMismatch"), identityError, now);
        if (BinaryTrustPolicy::installedTreeRequired()) {
            QString trustError;
            if (!BinaryTrustPolicy::verifyInstalledTree(record->installPath,
                                                        &trustError))
                return fail(transactionId, buildId,
                            QStringLiteral("BinaryTrustRejected"), trustError,
                            now);
        }
        QString indexError;
        if (!InstallIndex::setHealthState(buildId, QStringLiteral("Installed"),
                                          &indexError))
            return fail(transactionId, buildId,
                        QStringLiteral("IndexCommitFailed"), indexError, now);
        TransactionJournal::append(transactionId, kProduct, buildId,
                                   QStringLiteral("Installed"), QString(), now);
        outcome.ok = true;
        outcome.healthState = QStringLiteral("Installed");
        return outcome;
    }

    const auto inUse = [&]() {
        return probe ? probe(record->installPath) : runningExecutablesUnder(record->installPath);
    };
    const auto runningFailure = [&](const QStringList& processes) {
        return fail(transactionId, buildId, QStringLiteral("VersionInUse"),
                    QStringLiteral("HarkenStudio is still running from this version: %1")
                        .arg(processes.join(QStringLiteral(", "))), now);
    };
    const QStringList running = inUse();
    if (!running.isEmpty())
        return runningFailure(running);

    QString healthError;
    InstallIndex::setHealthState(buildId, QStringLiteral("NeedsRepair"),
                                 &healthError);

    const auto source = verifySource(buildId, now);
    if (!source.ok)
        return fail(transactionId, buildId, source.failureCode, source.error, now);
    if (!reviewedManifestHash.isEmpty() && source.manifestHash != reviewedManifestHash)
        return fail(transactionId, buildId, QStringLiteral("ReviewChanged"),
                    QStringLiteral("The repair source changed; review the operation again"), now);

    const QString replacement = studioVersionsRoot() +
                                QStringLiteral("/.repair-") + transactionId;
    const QString previous = studioVersionsRoot() +
                             QStringLiteral("/.repair-old-") + transactionId;
    QDir(replacement).removeRecursively();
    QDir(previous).removeRecursively();
    QString extractError;
    if (!PackageReader::extractAllVerified(source.packagePath, replacement, source.files, &extractError)) {
        QDir(replacement).removeRecursively();
        return fail(transactionId, buildId, QStringLiteral("RepairExtractFailed"),
                    extractError, now);
    }
    if (BinaryTrustPolicy::installedTreeRequired()) {
        QString trustError;
        if (!BinaryTrustPolicy::verifyInstalledTree(replacement, &trustError)) {
            QDir(replacement).removeRecursively();
            return fail(transactionId, buildId,
                        QStringLiteral("BinaryTrustRejected"), trustError, now);
        }
    }

    const QStringList startedDuringRepair = inUse();
    if (!startedDuringRepair.isEmpty()) {
        QDir(replacement).removeRecursively();
        return runningFailure(startedDuringRepair);
    }

    if (!reviewedSnapshot.isEmpty() && IntegrityChecker::snapshot(record->installPath).hash != reviewedSnapshot) {
        QDir(replacement).removeRecursively();
        return fail(transactionId, buildId, QStringLiteral("ReviewChanged"),
                    QStringLiteral("Installed files changed; review the operation again"), now);
    }
    const bool hadPrevious = QFileInfo::exists(record->installPath);
    if (hadPrevious && !QDir().rename(record->installPath, previous)) {
        QDir(replacement).removeRecursively();
        return fail(transactionId, buildId, QStringLiteral("RepairCommitFailed"),
                    QStringLiteral("Cannot detach the damaged installation"),
                    now);
    }
    const auto rollback = [&](const QString& failureCode, const QString& error,
                              bool replacementInstalled) {
        if (replacementInstalled && !QDir().rename(record->installPath, replacement))
            return fail(transactionId, buildId, QStringLiteral("RepairRollbackFailed"),
                QStringLiteral("Cannot move repaired installation to %1. Previous installation: %2. Original failure: %3")
                    .arg(replacement, hadPrevious ? previous : QStringLiteral("(none)"), error), now);
        if (hadPrevious && !QDir().rename(previous, record->installPath))
            return fail(transactionId, buildId, QStringLiteral("RepairRollbackFailed"),
                QStringLiteral("Cannot restore original installation from %1 to %2. Repaired files retained at %3. Original failure: %4")
                    .arg(previous, record->installPath, replacement, error), now);
        if (!QDir(replacement).removeRecursively())
            return fail(transactionId, buildId, QStringLiteral("RepairCleanupFailed"),
                QStringLiteral("Cannot clean up repair files at %1 after rollback. Original failure: %2")
                    .arg(replacement, error), now);
        return fail(transactionId, buildId, failureCode, error, now);
    };
    if (!QDir().rename(replacement, record->installPath))
        return rollback(QStringLiteral("RepairCommitFailed"),
                        QStringLiteral("Cannot move the repaired installation into place"), false);

    QString validationError;
    const auto repaired = IntegrityChecker::check(
        record->installPath, record->packageManifestHash);
    const bool valid = repaired.ok
        && VersionScanner::inspect(*record, nullptr, &validationError) == InstallationIssue::None;
    if (!valid)
        return rollback(QStringLiteral("RepairVerificationFailed"),
                        repaired.ok ? validationError : repaired.error, true);

    QString indexError;
    if (!InstallIndex::setHealthState(buildId, QStringLiteral("Installed"),
                                      &indexError))
        return rollback(QStringLiteral("IndexCommitFailed"), indexError, true);
    if (hadPrevious)
        QDir(previous).removeRecursively();

    TransactionJournal::append(transactionId, kProduct, buildId,
                               QStringLiteral("Installed"), QString(), now);
    outcome.ok = true;
    outcome.healthState = QStringLiteral("Installed");
    return outcome;
}

}
