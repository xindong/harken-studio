#include "Transactions/InstallExecutor.h"

#include "Packaging/PackageVerifier.h"
#include "Packaging/PackageReader.h"
#include "Platform/AclHardening.h"
#include "Platform/PlatformPaths.h"
#include "Platform/ProcessScan.h"
#include "Security/BinaryTrustPolicy.h"
#include "Security/ManifestVerifier.h"
#include "Transactions/HandoffArea.h"
#include "Transactions/InstallIndex.h"
#include "Transactions/InstallPackageCache.h"
#include "Transactions/TransactionJournal.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLockFile>
#include <QRegularExpression>
#include <QThread>

#include <algorithm>

namespace VNH {

namespace {

constexpr qint64 kManifestMaxBytes = 1024 * 1024;
constexpr qint64 kPackageMaxBytes = 4ll * 1024 * 1024 * 1024;
constexpr int kDirectoryMoveAttempts = 5;
constexpr int kDirectoryMoveRetryDelayMs = 100;
const QString kProduct = QStringLiteral("HarkenStudio");

bool isPlainFile(const QString& path, qint64 maxBytes, QString* error)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        *error = QStringLiteral("Handoff file is missing: %1").arg(path);
        return false;
    }
    if (info.isSymbolicLink() || info.isJunction()) {
        *error = QStringLiteral("Handoff file is a reparse point: %1").arg(path);
        return false;
    }
    if (info.size() <= 0 || info.size() > maxBytes) {
        *error = QStringLiteral("Handoff file size is out of bounds: %1").arg(path);
        return false;
    }
    return true;
}

bool moveDirectoryIntoPlace(const QString& sourcePath, const QString& targetPath)
{
    for (int attempt = 1; attempt <= kDirectoryMoveAttempts; ++attempt) {
        if (QDir().rename(sourcePath, targetPath))
            return true;
        if (attempt < kDirectoryMoveAttempts)
            QThread::msleep(kDirectoryMoveRetryDelayMs);
    }
    return false;
}

bool copyPlainFile(const QString& sourcePath, const QString& destPath,
                   QString* error)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot read handoff file: %1").arg(sourcePath);
        return false;
    }
    QFile dest(destPath);
    if (!dest.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        *error = QStringLiteral("Cannot create staging file: %1").arg(destPath);
        return false;
    }
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    qint64 read = 0;
    while ((read = source.read(buffer.data(), buffer.size())) > 0) {
        if (dest.write(buffer.constData(), read) != read) {
            dest.close();
            QFile::remove(destPath);
            *error = QStringLiteral("Write failed: %1").arg(destPath);
            return false;
        }
    }
    if (read < 0) {
        dest.close();
        QFile::remove(destPath);
        *error = QStringLiteral("Read failed: %1").arg(sourcePath);
        return false;
    }
    return true;
}

QByteArray readAll(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

struct Cleanup
{
    QString stagingDir;
    QString extractDir;

    ~Cleanup()
    {
        if (!extractDir.isEmpty() && QDir(extractDir).exists())
            QDir(extractDir).removeRecursively();
        if (!stagingDir.isEmpty() && QDir(stagingDir).exists())
            QDir(stagingDir).removeRecursively();
    }
};

void sweepStaleStaging(const QString& currentTransactionId)
{
    static const QRegularExpression transactionPattern(
        QStringLiteral("^[0-9a-f]{32}$"));

    QDir staging(machineDataDir() + QStringLiteral("/staging"));
    const auto stagingEntries =
        staging.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& entry : stagingEntries) {
        if (entry != currentTransactionId &&
            transactionPattern.match(entry).hasMatch())
            QDir(staging.filePath(entry)).removeRecursively();
    }

    QDir versions(studioVersionsRoot());
    const auto versionEntries = versions.entryList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QString& entry : versionEntries) {
        if (!entry.startsWith(QStringLiteral(".staging-")))
            continue;
        const QString transactionId = entry.mid(9);
        if (transactionId != currentTransactionId &&
            transactionPattern.match(transactionId).hasMatch())
            QDir(versions.filePath(entry)).removeRecursively();
    }
}

InstallOutcome fail(const QString& transactionId, const QString& buildId,
                    const QString& failureCode, const QString& error)
{
    TransactionJournal::append(transactionId, kProduct, buildId,
                               QStringLiteral("Failed"), failureCode,
                               QDateTime::currentDateTimeUtc(), error);
    InstallOutcome outcome;
    outcome.error = error;
    outcome.failureCode = failureCode;
    outcome.buildId = buildId;
    return outcome;
}

}

int InstallOutcome::exitCode() const
{
    if (ok)
        return 0;
    if (failureCode == QStringLiteral("MachineDataUnavailable"))
        return kInstallMachineDataUnavailableExitCode;
    return 2;
}

QString InstallExecutor::lockFilePath()
{
    return machineDataDir() + QStringLiteral("/install.lock");
}

bool InstallExecutor::isTransactionActive()
{
    QLockFile lock(lockFilePath());
    qint64 pid = 0;
    QString hostName;
    QString applicationName;
    return lock.getLockInfo(&pid, &hostName, &applicationName) && isProcessRunning(pid);
}

InstallOutcome InstallExecutor::run(const QString& transactionId,
                                    const QByteArray& manifestPublicKey,
                                    const QDateTime& now,
                                    bool explicitInstall)
{
    InstallOutcome outcome;

    if (!HandoffArea::isValidTransactionId(transactionId)) {
        outcome.error = QStringLiteral("Invalid transaction id");
        outcome.failureCode = QStringLiteral("InvalidTransaction");
        return outcome;
    }

    if (!QDir().mkpath(machineDataDir())) {
        return fail(transactionId, {}, QStringLiteral("MachineDataUnavailable"),
                    QStringLiteral("Cannot create the machine data directory: %1").arg(machineDataDir()));
    }
    QLockFile installLock(lockFilePath());
    installLock.setStaleLockTime(0);
    if (!installLock.tryLock(0)) {
        return fail(transactionId, {}, QStringLiteral("InstallBusy"),
                    QStringLiteral("Another install transaction is running"));
    }

    sweepStaleStaging(transactionId);

    const QString handoffPackage = HandoffArea::packagePath(transactionId);
    const QString handoffManifest = HandoffArea::manifestPath(transactionId);

    QString checkError;
    if (!isPlainFile(handoffManifest, kManifestMaxBytes, &checkError) ||
        !isPlainFile(handoffPackage, kPackageMaxBytes, &checkError)) {
        return fail(transactionId, {}, QStringLiteral("HandoffRejected"), checkError);
    }

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    {
        QString aclError;
        if (!applyHandoffAcl(HandoffArea::rootDir(), &aclError)) {
            return fail(transactionId, {}, QStringLiteral("HandoffAclHardeningFailed"), aclError);
        }
    }
#endif

    Cleanup cleanup;
    cleanup.stagingDir =
        machineDataDir() + QStringLiteral("/staging/") + transactionId;
    if (QDir(cleanup.stagingDir).exists())
        QDir(cleanup.stagingDir).removeRecursively();
    if (!QDir().mkpath(cleanup.stagingDir)) {
        return fail(transactionId, {}, QStringLiteral("StagingFailed"),
                    QStringLiteral("Cannot create the staging directory: %1").arg(cleanup.stagingDir));
    }

    const QString stagedPackage =
        cleanup.stagingDir + QStringLiteral("/package.pkg");
    const QString stagedManifest =
        cleanup.stagingDir + QStringLiteral("/release.manifest");
    QString copyError;
    if (!copyPlainFile(handoffPackage, stagedPackage, &copyError) ||
        !copyPlainFile(handoffManifest, stagedManifest, &copyError)) {
        return fail(transactionId, {}, QStringLiteral("StagingFailed"), copyError);
    }

    HandoffArea::discard(transactionId);
    TransactionJournal::append(transactionId, kProduct, QString(),
                               QStringLiteral("Verifying"), QString(),
                               QDateTime::currentDateTimeUtc());

    const ManifestVerifyResult manifestResult =
        ManifestVerifier::verify(readAll(stagedManifest), manifestPublicKey,
                                 now, explicitInstall);
    if (!manifestResult.ok)
        return fail(transactionId, QString(), QStringLiteral("ManifestRejected"),
                    manifestResult.error);
    const ReleaseManifest manifest = manifestResult.manifest;

    if (manifest.product != kProduct)
        return fail(transactionId, manifest.buildId,
                    QStringLiteral("ManifestRejected"),
                    QStringLiteral("Manifest is for product: %1")
                        .arg(manifest.product));

    const InstallIndexSnapshot index = InstallIndex::snapshot();
    if (!index.ok)
        return fail(transactionId, manifest.buildId,
                    QStringLiteral("IndexCorrupted"), index.error);
    if (!explicitInstall &&
        manifest.releaseSequence < index.lastReleaseSequence)
        return fail(transactionId, manifest.buildId,
                    QStringLiteral("SequenceReplayed"),
                    QStringLiteral(
                        "Manifest releaseSequence %1 is older than the last "
                        "accepted sequence %2")
                        .arg(manifest.releaseSequence)
                        .arg(index.lastReleaseSequence));

    ExpectedPackage expected;
    expected.buildId = manifest.buildId;
    expected.semanticVersion = manifest.version;
    expected.sha256Hex = manifest.sha256Hex;
    expected.sizeBytes = manifest.packageSize;
    const PackageVerifyResult packageResult =
        PackageVerifier::verify(stagedPackage, expected);
    if (!packageResult.ok)
        return fail(transactionId, manifest.buildId,
                    QStringLiteral("PackageRejected"), packageResult.error);
    if (packageResult.channel != manifest.channel)
        return fail(transactionId, manifest.buildId,
                    QStringLiteral("PackageRejected"),
                    QStringLiteral("Package channel %1 disagrees with the "
                                   "manifest channel %2")
                        .arg(packageResult.channel, manifest.channel));

    for (const auto& record : index.records) {
        if (record.buildId == manifest.buildId)
            return fail(transactionId, manifest.buildId,
                        QStringLiteral("TargetConflict"),
                        QStringLiteral("Build is already registered at: %1")
                            .arg(record.installPath));
    }

    const QString versionsRoot = studioVersionsRoot();
    const QString targetBase = QDir(versionsRoot).filePath(
        QStringLiteral("%1-%2-%3-%4").arg(
            manifest.version, manifest.channel, manifest.platform.toLower(),
            manifest.architecture.toLower()));
    const auto occupied = [&index](const QString& path) {
        const QFileInfo target(path);
        if (target.exists() || target.isSymbolicLink() || target.isJunction())
            return true;
        return std::any_of(index.records.begin(), index.records.end(),
                           [&target](const InstalledVersionRecord& record) {
            return QFileInfo(record.installPath).absoluteFilePath().compare(
                       target.absoluteFilePath(), Qt::CaseInsensitive) == 0;
        });
    };
    QString targetDir = targetBase;
    for (int suffix = 2; occupied(targetDir); ++suffix)
        targetDir = targetBase + QStringLiteral("-%1").arg(suffix);

    cleanup.extractDir = versionsRoot + QStringLiteral("/.staging-") +
                         transactionId;
    if (QDir(cleanup.extractDir).exists())
        QDir(cleanup.extractDir).removeRecursively();
    if (!QDir().mkpath(versionsRoot))
        return fail(transactionId, manifest.buildId,
                    QStringLiteral("ExtractFailed"),
                    QStringLiteral("Cannot create the versions directory"));

    QHash<QString, QString> expectedHashes = packageResult.fileSha256ByPath;
    for (const QString& metadataName :
         {QStringLiteral("package-manifest.json"),
          QStringLiteral("capabilities.json")}) {
        QByteArray metadataBytes;
        QString metadataError;
        if (!PackageReader::readEntry(stagedPackage, metadataName, &metadataBytes,
                                      &metadataError))
            return fail(transactionId, manifest.buildId,
                        QStringLiteral("PackageRejected"), metadataError);
        expectedHashes.insert(
            metadataName,
            QString::fromLatin1(
                QCryptographicHash::hash(metadataBytes,
                                         QCryptographicHash::Sha256)
                    .toHex()));
    }

    QString extractError;
    if (!PackageReader::extractAllVerified(stagedPackage, cleanup.extractDir,
                                           expectedHashes, &extractError))
        return fail(transactionId, manifest.buildId,
                    QStringLiteral("ExtractFailed"), extractError);

    if (BinaryTrustPolicy::installedTreeRequired()) {
        QString trustError;
        if (!BinaryTrustPolicy::verifyInstalledTree(cleanup.extractDir,
                                                    &trustError))
            return fail(transactionId, manifest.buildId,
                        QStringLiteral("BinaryTrustRejected"), trustError);
    }

    QString cacheError;
    if (!InstallPackageCache::store(manifest.buildId, stagedPackage,
                                    stagedManifest, &cacheError))
        return fail(transactionId, manifest.buildId,
                    QStringLiteral("CacheCommitFailed"), cacheError);

    TransactionJournal::append(transactionId, kProduct, manifest.buildId,
                               QStringLiteral("Installing"), QString(),
                               QDateTime::currentDateTimeUtc());

    InstalledVersionRecord record;
    record.buildId = manifest.buildId;
    record.semanticVersion = manifest.version;
    record.channel = manifest.channel;
    record.installPath = targetDir;
    record.packageHash = manifest.sha256Hex.toLower();
    record.packageManifestHash = expectedHashes.value(
        QStringLiteral("package-manifest.json"));
    record.capabilitiesHash = expectedHashes.value(
        QStringLiteral("capabilities.json"));
    record.installedAt = now.toUTC().toString(Qt::ISODateWithMs);
    record.healthState = QStringLiteral("Installing");
    QString indexError;
    if (!InstallIndex::commitInstallation(record, manifest.releaseSequence,
                                          &indexError)) {
        InstallPackageCache::remove(manifest.buildId);
        return fail(transactionId, manifest.buildId,
                    QStringLiteral("IndexCommitFailed"), indexError);
    }
    if (!moveDirectoryIntoPlace(cleanup.extractDir, targetDir)) {
        if (InstallIndex::removeRecord(manifest.buildId, &indexError))
            InstallPackageCache::remove(manifest.buildId);
        return fail(transactionId, manifest.buildId,
                    QStringLiteral("CommitFailed"),
                    QStringLiteral("Cannot move the install into place: %1").arg(targetDir)
                        + (indexError.isEmpty() ? QString() : QLatin1Char('\n') + indexError));
    }
    if (!InstallIndex::setHealthState(manifest.buildId, QStringLiteral("Installed"),
                                      &indexError)) {
        return fail(transactionId, manifest.buildId,
                    QStringLiteral("IndexCommitFailed"),
                    indexError + QLatin1Char('\n') + QStringLiteral(
                        "Installation retained at %1. In Versions, choose Repair to recover it.").arg(targetDir));
    }
    cleanup.extractDir.clear();

    TransactionJournal::append(transactionId, kProduct, manifest.buildId,
                               QStringLiteral("Installed"), QString(),
                               QDateTime::currentDateTimeUtc());

    outcome.ok = true;
    outcome.buildId = manifest.buildId;
    outcome.installPath = targetDir;
    return outcome;
}

}
