#include "Update/UpdateCoordinator.h"

#include "Packaging/PackageVerifier.h"
#include "Platform/AtomicFile.h"
#include "Platform/PlatformPaths.h"
#include "Transactions/HandoffArea.h"
#include "Transactions/InstallIndex.h"
#include "Transactions/InstallRequest.h"
#include "Transactions/TransactionJournal.h"
#include "Update/BridgeDispatcher.h"
#include "Update/ServerEndpoints.h"
#include "Update/UpdateQueue.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStorageInfo>

namespace VNH {

namespace {

const QString kProduct = QStringLiteral("HarkenStudio");
const QString kServerProduct = QStringLiteral("harken");

bool isSafeFileToken(const QString& token)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"));
    return pattern.match(token).hasMatch();
}

}

UpdateCoordinator::UpdateCoordinator(QObject* parent)
    : QObject(parent), checker_(this)
{
    connect(&checker_, &UpdateChecker::checkFinished, this,
            [this](const UpdateCheckResult& result) {
                handleCheckResult(result, QDateTime::currentDateTimeUtc());
            });
    connect(&downloader_, &Downloader::finished, this,
            &UpdateCoordinator::handleDownloadFinished);
    connect(&downloader_, &Downloader::progress, this,
            [this](qint64 receivedBytes, qint64 totalBytes) {
                if (progressPersistTimer_.isValid() &&
                    progressPersistTimer_.elapsed() < 1000)
                    return;
                progressPersistTimer_.start();
                UpdateStateRecord record = UpdateStateStore::load(kProduct);
                if (!record.found ||
                    record.state != QStringLiteral("Downloading") ||
                    record.buildId != activeManifest_.buildId)
                    return;
                const auto expected = record;
                record.downloadedBytes = receivedBytes;
                if (totalBytes > 0 && record.packageSize <= 0)
                    record.packageSize = totalBytes;
                UpdateStateStore::replace(expected, record,
                                          QDateTime::currentDateTimeUtc());
            });
    connect(&bridgePollTimer_, &QTimer::timeout, this,
            &UpdateCoordinator::pollBridgeOutcome);
}

void UpdateCoordinator::configure(const Options& options)
{
    options_ = options;
    if (options_.downloadDir.isEmpty())
        options_.downloadDir = userDataDir() + QStringLiteral("/downloads");
    if (options_.quarantineDir.isEmpty())
        options_.quarantineDir = userDataDir() + QStringLiteral("/quarantine");
}

bool UpdateCoordinator::start(const Options& options)
{
    configure(options);

    UpdateChecker::Options checkerOptions;
    QList<QUrl> manifestUrls = ServerEndpoints::manifestUrls(
        options_.serverBaseUrl, kServerProduct, ServerEndpoints::hostChannel());
    checkerOptions.latestUrl = manifestUrls.takeFirst();
    checkerOptions.fallbackUrls = manifestUrls;
    checkerOptions.trustedHttpOrigin = options_.serverBaseUrl;
    checkerOptions.product = kServerProduct;
    checkerOptions.channel = ServerEndpoints::hostChannel();
    checkerOptions.manifestPublicKey = options_.manifestPublicKey;
    checkerOptions.intervalMs = options_.checkIntervalMs;
    started_ = checker_.start(checkerOptions);
    if (started_) {
        const UpdateStateRecord existing = UpdateStateStore::load(kProduct);
        if (existing.found &&
            existing.state == QStringLiteral("AwaitingBridge"))
            startBridgePoll();
        else if (existing.found &&
                 existing.state == QStringLiteral("Verified"))
            beginAutoInstall(QDateTime::currentDateTimeUtc());
    }
    return started_;
}

void UpdateCoordinator::stop()
{
    checker_.stop();
    downloader_.cancel();
    bridgePollTimer_.stop();
    started_ = false;
}

bool UpdateCoordinator::checkNow()
{
    checker_.invalidateCache();
    return checker_.checkNow();
}

void UpdateCoordinator::setAutomaticDownloadEnabled(bool enabled)
{
    if (options_.automaticDownloadEnabled == enabled)
        return;
    options_.automaticDownloadEnabled = enabled;
    if (!enabled && downloading_) {
        automaticDownloadPauseRequested_ = true;
        downloader_.cancel();
    }
}

bool UpdateCoordinator::isDownloading() const
{
    return downloading_;
}

UpdateStateRecord UpdateCoordinator::currentState() const
{
    return UpdateStateStore::load(kProduct);
}

bool UpdateCoordinator::isBuildInstalled(const QString& buildId) const
{
    const InstallIndexSnapshot index = InstallIndex::snapshot();
    if (!index.ok)
        return false;
    for (const auto& record : index.records) {
        if (record.buildId == buildId)
            return true;
    }
    return false;
}

bool UpdateCoordinator::persist(UpdateStateRecord& record, const QDateTime& now,
                                const UpdateStateRecord& expected)
{
    if (record.state == QStringLiteral("DownloadFailed"))
        checker_.invalidateCache();
    record.product = kProduct;
    if (UpdateStateStore::replace(expected, record, now) !=
        UpdateStateStore::WriteResult::Saved) {
        checker_.invalidateCache();
        qWarning().noquote() << kProduct << "cannot persist state" << record.state;
        return false;
    }
    record.found = true;
    record.updatedAt = now.toUTC().toString(Qt::ISODateWithMs);
    if (record.failureCode.isEmpty())
        qInfo().noquote() << kProduct << record.version << "state ->"
                          << record.state;
    else
        qWarning().noquote() << kProduct << record.version << "state ->"
                             << record.state << "(" + record.failureCode + ")";
    emit stateChanged(record);
    return true;
}

void UpdateCoordinator::restoreState(const UpdateStateRecord& record,
                                     const UpdateStateRecord& pending)
{
    const auto now = QDateTime::currentDateTimeUtc();
    const auto result = UpdateStateStore::replace(pending, record, now);
    if (result == UpdateStateStore::WriteResult::Failed) {
        QTimer::singleShot(1000, this, [this, record, pending]() {
            restoreState(record, pending);
        });
    } else if (result == UpdateStateStore::WriteResult::Saved) {
        auto restored = record;
        restored.found = true;
        restored.updatedAt = now.toUTC().toString(Qt::ISODateWithMs);
        emit stateChanged(restored);
    }
}

void UpdateCoordinator::handleCheckResult(const UpdateCheckResult& result,
                                          const QDateTime& now)
{
    if (!result.ok) {
        if (!result.error.isEmpty())
            qWarning().noquote()
                << kProduct << "update check failed:" << result.error;
        return;
    }
    if (result.notModified) {
        if (currentState().needsDownloadRecovery())
            checkNow();
        return;
    }
    if (result.manifestAbsent) {
        qInfo().noquote()
            << kProduct
            << "check: no installable release is published on the server";
        const UpdateStateRecord existing = UpdateStateStore::load(kProduct);
        if (existing.found &&
            existing.state == QStringLiteral("MetadataAvailable"))
            UpdateStateStore::clear(existing);
        return;
    }
    const ReleaseManifest& manifest = result.manifest;
    if (manifest.product != kProduct)
        return;
    if (!isSafeFileToken(manifest.buildId) || !isSafeFileToken(manifest.version))
        return;
    const UpdateStateRecord observed = UpdateStateStore::load(kProduct);
    if (observed.found && manifest.releaseSequence >= 0 &&
        observed.releaseSequence >= 0 &&
        (manifest.releaseSequence < observed.releaseSequence ||
         (manifest.releaseSequence == observed.releaseSequence &&
          observed.buildId != manifest.buildId))) {
        qWarning().noquote() << kProduct
                             << "ignored a rollback or split-brain release";
        return;
    }
    if (isBuildInstalled(manifest.buildId)) {
        qInfo().noquote() << kProduct << manifest.version
                          << "poll result: latest build is already installed";
        const UpdateStateRecord existing = observed;
        if (existing.found && existing.buildId == manifest.buildId)
            UpdateStateStore::clear(existing);
        return;
    }
    qInfo().noquote() << kProduct << manifest.version << "seq"
                      << manifest.releaseSequence
                      << "is available on the server";

    const UpdateStateRecord existing = observed;
    if (existing.found && existing.buildId == manifest.buildId &&
        !existing.needsDownloadRecovery()) {
        if (existing.state == QStringLiteral("Verified")) {
            beginAutoInstall(now);
            return;
        }
        if (existing.state == QStringLiteral("AwaitingBridge") ||
            existing.state == QStringLiteral("AwaitingUserInstall") ||
            existing.state == QStringLiteral("InstallFailed"))
            return;
        if (downloading_)
            return;
        if (existing.state == QStringLiteral("Downloaded") ||
            existing.state == QStringLiteral("Verifying")) {
            if (QFileInfo::exists(existing.downloadPath)) {
                activeManifest_ = manifest;
                verifyDownloaded(existing.downloadPath, manifest, now);
                return;
            }
        }
    }
    if (downloading_)
        return;

    if (!QDir().mkpath(options_.downloadDir)) {
        UpdateStateRecord record;
        record.state = QStringLiteral("DownloadFailed");
        record.version = manifest.version;
        record.buildId = manifest.buildId;
        record.releaseSequence = manifest.releaseSequence;
        record.failureCode = QStringLiteral("DownloadDirUnavailable");
        persist(record, now, existing);
        return;
    }

    const QString envelopePath = options_.downloadDir + QStringLiteral("/") +
                                 manifest.buildId +
                                 QStringLiteral(".manifest");
    const auto lease = DownloadLease::acquire(envelopePath);
    if (!lease) {
        checker_.invalidateCache();
        return;
    }
    if (!writeAtomicFile(envelopePath, result.envelope)) {
        UpdateStateRecord record;
        record.state = QStringLiteral("DownloadFailed");
        record.version = manifest.version;
        record.buildId = manifest.buildId;
        record.releaseSequence = manifest.releaseSequence;
        record.failureCode = QStringLiteral("EnvelopePersistFailed");
        persist(record, now, existing);
        return;
    }

    UpdateStateRecord record;
    record.state = QStringLiteral("MetadataAvailable");
    record.version = manifest.version;
    record.buildId = manifest.buildId;
    record.manifestEnvelopePath = envelopePath;
    record.expectedSha256 = manifest.sha256Hex.toLower();
    record.packageSize = manifest.packageSize;
    record.releaseSequence = manifest.releaseSequence;
    if (!persist(record, now, existing))
        return;

    if (!options_.automaticDownloadEnabled)
        return;

    activeObjectPath_ = result.objectPath;
    activeReleaseTag_ = result.releaseTag;
    beginDownload(manifest, now, lease);
}

void UpdateCoordinator::beginDownload(const ReleaseManifest& manifest,
                                      const QDateTime& now,
                                      std::shared_ptr<DownloadLease> lease)
{
    const auto expected = UpdateStateStore::load(kProduct);
    if (!expected.found || expected.buildId != manifest.buildId ||
        expected.state != QStringLiteral("MetadataAvailable"))
        return;
    const QStorageInfo storage(options_.downloadDir);
    if (manifest.packageSize > 0 &&
        storage.bytesAvailable() < manifest.packageSize * 2) {
        UpdateStateRecord record = expected;
        record.state = QStringLiteral("DownloadFailed");
        record.failureCode = QStringLiteral("InsufficientDiskSpace");
        persist(record, now, expected);
        return;
    }

    const QString targetPath = options_.downloadDir + QStringLiteral("/") +
                               manifest.buildId + QStringLiteral(".zip");
    DownloadRequest request;
    QList<QUrl> downloadUrls = ServerEndpoints::downloadUrls(
        options_.serverBaseUrl, activeObjectPath_, activeReleaseTag_);
    if (downloadUrls.isEmpty()) {
        UpdateStateRecord record;
        record.state = QStringLiteral("DownloadFailed");
        record.version = manifest.version;
        record.buildId = manifest.buildId;
        record.releaseSequence = manifest.releaseSequence;
        record.failureCode = QStringLiteral("DownloadUrlUnavailable");
        persist(record, now, expected);
        return;
    }
    request.url = downloadUrls.takeFirst();
    request.fallbackUrls = downloadUrls;
    request.targetPath = targetPath;
    request.expectedSizeBytes = manifest.packageSize;
    request.lease = std::move(lease);
    activeManifest_ = manifest;
    UpdateStateRecord record = expected;
    record.state = QStringLiteral("Downloading");
    record.downloadPath = targetPath;
    if (!persist(record, QDateTime::currentDateTimeUtc(), expected))
        return;
    downloading_ = true;
    if (!downloader_.start(request)) {
        downloading_ = false;
        const auto started = record;
        record.state = QStringLiteral("DownloadFailed");
        record.failureCode = QStringLiteral("DownloadStartRejected");
        persist(record, now, started);
        return;
    }
}

void UpdateCoordinator::handleDownloadFinished(const DownloadResult& result)
{
    downloading_ = false;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const auto active = UpdateStateStore::load(kProduct);
    if (!active.found || active.buildId != activeManifest_.buildId ||
        active.state != QStringLiteral("Downloading")) {
        automaticDownloadPauseRequested_ = false;
        return;
    }
    if (automaticDownloadPauseRequested_) {
        automaticDownloadPauseRequested_ = false;
        UpdateStateRecord record = active;
        record.state = QStringLiteral("MetadataAvailable");
        record.downloadPath.clear();
        record.downloadedBytes = -1;
        record.failureCode.clear();
        if (!persist(record, now, active))
            return;
        if (options_.automaticDownloadEnabled && started_)
            checkNow();
        return;
    }
    if (!result.ok) {
        UpdateStateRecord record = active;
        record.state = QStringLiteral("DownloadFailed");
        record.failureCode = QStringLiteral("DownloadFailed");
        persist(record, now, active);
        return;
    }

    UpdateStateRecord record = active;
    record.state = QStringLiteral("Downloaded");
    record.downloadPath = result.filePath;
    if (!persist(record, now, active))
        return;

    verifyDownloaded(result.filePath, activeManifest_, now, result.lease);
}

void UpdateCoordinator::verifyDownloaded(const QString& packagePath,
                                         const ReleaseManifest& manifest,
                                         const QDateTime& now,
                                         std::shared_ptr<DownloadLease> lease)
{
    UpdateStateRecord record = UpdateStateStore::load(kProduct);
    auto expected = record;
    if (!record.found || record.buildId != manifest.buildId ||
        (record.state != QStringLiteral("Downloaded") &&
         record.state != QStringLiteral("Verifying")))
        return;
    if (!lease || !lease->covers(packagePath))
        lease = DownloadLease::acquire(packagePath);
    if (!lease)
        return;
    record.state = QStringLiteral("Verifying");
    if (!persist(record, now, expected))
        return;

    ExpectedPackage expectedPackage;
    expectedPackage.buildId = manifest.buildId;
    expectedPackage.semanticVersion = manifest.version;
    expectedPackage.sha256Hex = manifest.sha256Hex;
    expectedPackage.sizeBytes = manifest.packageSize;
    const PackageVerifyResult verifyResult =
        PackageVerifier::verify(packagePath, expectedPackage);

    if (!verifyResult.ok) {
        QDir().mkpath(options_.quarantineDir);
        const QString quarantinePath =
            options_.quarantineDir + QStringLiteral("/") +
            QFileInfo(packagePath).fileName();
        QFile::remove(quarantinePath);
        QFile::rename(packagePath, quarantinePath);
        expected = record;
        record.state = QStringLiteral("VerificationFailed");
        record.failureCode = QStringLiteral("PackageRejected");
        record.downloadPath.clear();
        persist(record, QDateTime::currentDateTimeUtc(), expected);
        return;
    }

    expected = record;
    record.state = QStringLiteral("Verified");
    record.downloadPath = packagePath;
    if (!persist(record, QDateTime::currentDateTimeUtc(), expected))
        return;

    beginAutoInstall(QDateTime::currentDateTimeUtc(), std::move(lease));
}

void UpdateCoordinator::beginAutoInstall(const QDateTime& now,
                                          std::shared_ptr<DownloadLease> lease)
{
    UpdateStateRecord record = UpdateStateStore::load(kProduct);
    auto expected = record;
    if (!record.found || record.state != QStringLiteral("Verified"))
        return;
    if (!QFileInfo::exists(record.downloadPath) ||
        !QFileInfo::exists(record.manifestEnvelopePath)) {
        record.state = QStringLiteral("InstallFailed");
        record.failureCode = QStringLiteral("DownloadMissing");
        persist(record, now, expected);
        return;
    }

    if (!BridgeDispatcher::available()) {
        record.state = QStringLiteral("AwaitingUserInstall");
        record.failureCode.clear();
        persist(record, now, expected);
        return;
    }

    if (!lease || !lease->covers(record.downloadPath))
        lease = DownloadLease::acquire(record.downloadPath);
    if (!lease)
        return;
    if (isBuildInstalled(record.buildId)) {
        UpdateStateStore::clear(record);
        return;
    }
    const InstallPreparation preparation = InstallRequest::prepare(
        record.downloadPath, record.manifestEnvelopePath,
        options_.manifestPublicKey, now);
    if (!preparation.ok) {
        qWarning().noquote()
            << kProduct << "install preparation failed:" << preparation.error;
        record.state = QStringLiteral("InstallFailed");
        record.failureCode = QStringLiteral("QueuePlacementFailed");
        persist(record, now, expected);
        return;
    }

    QString queueError;
    if (!UpdateQueue::markStudioInstall(preparation.transactionId,
                                        &queueError)) {
        qWarning().noquote()
            << kProduct << "queue placement failed:" << queueError;
        HandoffArea::discard(preparation.transactionId);
        record.state = QStringLiteral("InstallFailed");
        record.failureCode = QStringLiteral("QueuePlacementFailed");
        persist(record, now, expected);
        return;
    }

    record.state = QStringLiteral("AwaitingBridge");
    record.transactionId = preparation.transactionId;
    record.failureCode.clear();
    record.updatedAt = now.toUTC().toString(Qt::ISODateWithMs);
    if (!persist(record, now, expected)) {
        HandoffArea::discard(preparation.transactionId);
        return;
    }

    QString dispatchError;
    if (!BridgeDispatcher::dispatch(&dispatchError)) {
        qWarning().noquote()
            << kProduct << "bridge dispatch failed:" << dispatchError;
        HandoffArea::discard(preparation.transactionId);
        const auto pending = record;
        record.state = QStringLiteral("AwaitingUserInstall");
        record.transactionId.clear();
        record.failureCode = QStringLiteral("BridgeDispatchFailed");
        restoreState(record, pending);
        return;
    }

    startBridgePoll();
}

void UpdateCoordinator::startBridgePoll()
{
    bridgePollElapsedMs_ = 0;
    bridgePollTimer_.start(static_cast<int>(options_.bridgePollIntervalMs));
}

void UpdateCoordinator::pollBridgeOutcome()
{
    bridgePollElapsedMs_ += options_.bridgePollIntervalMs;
    const QDateTime now = QDateTime::currentDateTimeUtc();

    UpdateStateRecord record = UpdateStateStore::load(kProduct);
    auto expected = record;
    if (!record.found || record.state != QStringLiteral("AwaitingBridge")) {
        bridgePollTimer_.stop();
        return;
    }

    if (isBuildInstalled(record.buildId)) {
        bridgePollTimer_.stop();
        const auto lease = DownloadLease::acquire(record.downloadPath);
        if (lease) {
            QFile::remove(record.downloadPath);
            QFile::remove(record.manifestEnvelopePath);
        }
        record.state = QStringLiteral("Installed");
        record.downloadPath.clear();
        record.failureCode.clear();
        persist(record, now, expected);
        return;
    }

    if (!record.transactionId.isEmpty()) {
        const QString journalState =
            TransactionJournal::lastStateByTransaction().value(
                record.transactionId);
        if (journalState == QStringLiteral("Failed")) {
            bridgePollTimer_.stop();
            record.state = QStringLiteral("InstallFailed");
            record.failureCode = QStringLiteral("BridgeInstallFailed");
            persist(record, now, expected);
            return;
        }
    }

    if (bridgePollElapsedMs_ >= options_.bridgePollTimeoutMs) {
        bridgePollTimer_.stop();
        record.state = QStringLiteral("AwaitingUserInstall");
        record.failureCode = QStringLiteral("BridgeTimeout");
        persist(record, now, expected);
    }
}

}
