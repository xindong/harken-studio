#include "Update/HubSelfUpdate.h"

#include "App/SingleInstanceGuard.h"
#include "Packaging/PackageReader.h"
#include "Platform/AtomicFile.h"
#include "Platform/Elevation.h"
#include "Platform/PlatformPaths.h"
#include "Platform/UserSession.h"
#include "Security/BinaryTrustPolicy.h"
#include "Transactions/HandoffArea.h"
#include "Update/BridgeDispatcher.h"
#include "Update/ServerEndpoints.h"
#include "Update/UpdateQueue.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QTimer>

namespace VNH {

namespace {

const QString kProduct = QStringLiteral("HarkenHub");
const QString kServerProduct = QStringLiteral("hub");

bool isSafeFileToken(const QString& token)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"));
    return pattern.match(token).hasMatch();
}

bool isBridgeInstallerFile(const QString& path)
{
    return path.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive) ||
           path.endsWith(QStringLiteral(".pkg"), Qt::CaseInsensitive);
}

}

bool HubSelfUpdate::isNewerVersion(const QString& candidate,
                                   const QString& current)
{
    static const QRegularExpression pattern(
        QStringLiteral("^(\\d+)\\.(\\d+)\\.(\\d+)"));
    const QRegularExpressionMatch lhs = pattern.match(candidate);
    const QRegularExpressionMatch rhs = pattern.match(current);
    if (!lhs.hasMatch() || !rhs.hasMatch())
        return false;
    for (int index = 1; index <= 3; ++index) {
        const int candidatePart = lhs.captured(index).toInt();
        const int currentPart = rhs.captured(index).toInt();
        if (candidatePart != currentPart)
            return candidatePart > currentPart;
    }
    return false;
}

HubSelfUpdate::HubSelfUpdate(QObject* parent)
    : QObject(parent), checker_(this)
{
    connect(&checker_, &UpdateChecker::checkFinished, this,
            [this](const UpdateCheckResult& result) {
                handleCheckResult(result, QDateTime::currentDateTimeUtc());
            });
    connect(&downloader_, &Downloader::finished, this,
            &HubSelfUpdate::handleDownloadFinished);
    connect(&downloader_, &Downloader::progress, this,
            [this](qint64 receivedBytes, qint64 totalBytes) {
                if (progressPersistTimer_.isValid() &&
                    progressPersistTimer_.elapsed() < 1000)
                    return;
                progressPersistTimer_.start();
                UpdateStateRecord record = UpdateStateStore::load(kProduct);
                if (!record.found ||
                    record.state != QStringLiteral("Downloading"))
                    return;
                record.downloadedBytes = receivedBytes;
                if (totalBytes > 0 && record.packageSize <= 0)
                    record.packageSize = totalBytes;
                UpdateStateStore::save(record,
                                       QDateTime::currentDateTimeUtc());
            });
}

void HubSelfUpdate::configure(const Options& options)
{
    options_ = options;
    if (options_.downloadDir.isEmpty())
        options_.downloadDir =
            userDataDir() + QStringLiteral("/hub-updates");
}

bool HubSelfUpdate::start(const Options& options)
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
    return checker_.start(checkerOptions);
}

void HubSelfUpdate::stop()
{
    checker_.stop();
    downloader_.cancel();
}

UpdateStateRecord HubSelfUpdate::currentState() const
{
    return UpdateStateStore::load(kProduct);
}

bool HubSelfUpdate::selfUpdateEnabled()
{
    return qEnvironmentVariable("HARKEN_HUB_SELFUPDATE_DISABLE") !=
           QStringLiteral("1");
}

QString HubSelfUpdate::backupRootDir()
{
    return userDataDir() + QStringLiteral("/backup");
}

bool HubSelfUpdate::persist(UpdateStateRecord record, const QDateTime& now)
{
    if (record.state == QStringLiteral("DownloadFailed"))
        checker_.invalidateCache();
    record.product = kProduct;
    if (!UpdateStateStore::save(record, now)) {
        checker_.invalidateCache();
        qWarning().noquote() << kProduct << "cannot persist state" << record.state;
        return false;
    }
    record.found = true;
    if (record.failureCode.isEmpty())
        qInfo().noquote() << kProduct << record.version << "state ->"
                          << record.state;
    else
        qWarning().noquote() << kProduct << record.version << "state ->"
                             << record.state << "(" + record.failureCode + ")";
    emit stateChanged(record);
    return true;
}

void HubSelfUpdate::restoreState(const UpdateStateRecord& record,
                                const UpdateStateRecord& pending)
{
    const auto current = UpdateStateStore::load(kProduct);
    if (!current.found || current.state != pending.state ||
        current.buildId != pending.buildId ||
        current.transactionId != pending.transactionId ||
        current.updatedAt != pending.updatedAt)
        return;
    if (!UpdateStateStore::save(record, QDateTime::currentDateTimeUtc())) {
        QTimer::singleShot(1000, QCoreApplication::instance(), [record, pending]() {
            restoreState(record, pending);
        });
    }
}

void HubSelfUpdate::handleCheckResult(const UpdateCheckResult& result,
                                           const QDateTime& now)
{
    if (!result.ok || result.manifestAbsent)
        return;
    if (result.notModified) {
        if (currentState().needsDownloadRecovery())
            checkNow();
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
    if (!options_.currentVersion.isEmpty() &&
        !isNewerVersion(manifest.version, options_.currentVersion)) {
        const UpdateStateRecord existing = UpdateStateStore::load(kProduct);
        if (existing.found) {
            if (!existing.downloadPath.isEmpty())
                QFile::remove(existing.downloadPath);
            if (!existing.manifestEnvelopePath.isEmpty())
                QFile::remove(existing.manifestEnvelopePath);
            UpdateStateStore::clear(kProduct);
        }
        return;
    }

    const UpdateStateRecord existing = UpdateStateStore::load(kProduct);
    if (existing.found && existing.buildId == manifest.buildId &&
        !existing.needsDownloadRecovery() &&
        (existing.state == QStringLiteral("Verified") ||
         existing.state == QStringLiteral("AwaitingUserApproval") ||
         existing.state == QStringLiteral("AwaitingRestart") ||
         existing.state == QStringLiteral("AwaitingBridge") ||
         existing.state == QStringLiteral("AwaitingElevation")))
        return;
    if (downloading_)
        return;

    if (!QDir().mkpath(options_.downloadDir)) {
        UpdateStateRecord record;
        record.state = QStringLiteral("DownloadFailed");
        record.version = manifest.version;
        record.buildId = manifest.buildId;
        record.releaseSequence = manifest.releaseSequence;
        record.failureCode = QStringLiteral("DownloadDirUnavailable");
        persist(record, now);
        return;
    }

    const QStorageInfo storage(options_.downloadDir);
    if (manifest.packageSize > 0 &&
        storage.bytesAvailable() < manifest.packageSize * 2) {
        UpdateStateRecord record;
        record.state = QStringLiteral("DownloadFailed");
        record.version = manifest.version;
        record.buildId = manifest.buildId;
        record.releaseSequence = manifest.releaseSequence;
        record.failureCode = QStringLiteral("InsufficientDiskSpace");
        persist(record, now);
        return;
    }

    const QString envelopePath = options_.downloadDir + QStringLiteral("/") +
                                 manifest.buildId +
                                 QStringLiteral(".manifest");
    if (!writeAtomicFile(envelopePath, result.envelope)) {
        UpdateStateRecord record;
        record.state = QStringLiteral("DownloadFailed");
        record.version = manifest.version;
        record.buildId = manifest.buildId;
        record.releaseSequence = manifest.releaseSequence;
        record.failureCode = QStringLiteral("EnvelopePersistFailed");
        persist(record, now);
        return;
    }

#if defined(Q_OS_MACOS)
    const QString suffix = QStringLiteral(".pkg");
#else
    const QString suffix = QStringLiteral(".exe");
#endif
    const QString targetPath =
        options_.downloadDir + QStringLiteral("/HarkenHubSetup-") +
        manifest.buildId + suffix;
    DownloadRequest request;
    activeObjectPath_ = result.objectPath;
    QList<QUrl> downloadUrls = ServerEndpoints::downloadUrls(
        options_.serverBaseUrl, activeObjectPath_, result.releaseTag);
    if (downloadUrls.isEmpty()) {
        UpdateStateRecord record;
        record.state = QStringLiteral("DownloadFailed");
        record.version = manifest.version;
        record.buildId = manifest.buildId;
        record.releaseSequence = manifest.releaseSequence;
        record.failureCode = QStringLiteral("DownloadUrlUnavailable");
        persist(record, now);
        return;
    }
    request.url = downloadUrls.takeFirst();
    request.fallbackUrls = downloadUrls;
    request.targetPath = targetPath;
    request.expectedSizeBytes = manifest.packageSize;
    activeManifest_ = manifest;
    UpdateStateRecord record;
    record.state = QStringLiteral("Downloading");
    record.version = manifest.version;
    record.buildId = manifest.buildId;
    record.downloadPath = targetPath;
    record.manifestEnvelopePath = envelopePath;
    record.expectedSha256 = manifest.sha256Hex.toLower();
    record.packageSize = manifest.packageSize;
    record.releaseSequence = manifest.releaseSequence;
    if (!persist(record, now))
        return;
    downloading_ = true;
    if (!downloader_.start(request)) {
        downloading_ = false;
        record.state = QStringLiteral("DownloadFailed");
        record.failureCode = QStringLiteral("DownloadStartRejected");
        persist(record, now);
    }
}

void HubSelfUpdate::handleDownloadFinished(const DownloadResult& result)
{
    downloading_ = false;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    UpdateStateRecord record = UpdateStateStore::load(kProduct);
    if (!result.ok) {
        record.state = QStringLiteral("DownloadFailed");
        record.failureCode = QStringLiteral("DownloadFailed");
        persist(record, now);
        return;
    }

    QString verifyError;
    if (!verifyInstaller(result.filePath, record.expectedSha256,
                         record.packageSize, &verifyError)) {
        QFile::remove(result.filePath);
        record.state = QStringLiteral("VerificationFailed");
        record.failureCode = QStringLiteral("InstallerRejected");
        record.downloadPath.clear();
        persist(record, now);
        return;
    }

    const bool bridgeLanding = selfUpdateEnabled() &&
                               BridgeDispatcher::available() &&
                               isBridgeInstallerFile(result.filePath);
    record.state = bridgeLanding ? QStringLiteral("AwaitingRestart")
                                 : QStringLiteral("AwaitingUserApproval");
    record.downloadPath = result.filePath;
    record.failureCode.clear();
    persist(record, now);
}

bool HubSelfUpdate::verifyInstaller(const QString& installerPath,
                                         const QString& expectedSha256,
                                         qint64 expectedSize, QString* error)
{
    const QFileInfo info(installerPath);
    if (!info.exists() || !info.isFile()) {
        *error = QStringLiteral("Installer is missing: %1").arg(installerPath);
        return false;
    }
    if (expectedSize > 0 && info.size() != expectedSize) {
        *error = QStringLiteral("Installer size %1 disagrees with the "
                                "manifest size %2")
                     .arg(info.size())
                     .arg(expectedSize);
        return false;
    }
    if (expectedSha256.isEmpty() ||
        PackageReader::fileSha256Hex(installerPath).toLower() !=
            expectedSha256.toLower()) {
        *error = QStringLiteral("Installer hash disagrees with the manifest");
        return false;
    }
    if (BinaryTrustPolicy::installerRequired() &&
        !BinaryTrustPolicy::verifyInstallerBinary(installerPath, error))
        return false;
    return true;
}

bool HubSelfUpdate::backupUserData(const QDateTime& now, QString* error)
{
    const QString backupDir =
        backupRootDir() + QStringLiteral("/") +
        now.toUTC().toString(QStringLiteral("yyyyMMdd-HHmmsszzz"));
    if (!QDir().mkpath(backupDir)) {
        *error = QStringLiteral("Cannot create the backup directory");
        return false;
    }
    const QDir source(userDataDir());
    const QStringList candidates = {
        QStringLiteral("projects.db"),
        QStringLiteral("update-state.json"),
        QStringLiteral("consents.json"),
        QStringLiteral("launch-diagnostics.json"),
    };
    for (const QString& name : candidates) {
        const QString sourcePath = source.filePath(name);
        if (!QFileInfo::exists(sourcePath))
            continue;
        const QString targetPath =
            backupDir + QStringLiteral("/") + name;
        if (!QFile::copy(sourcePath, targetPath)) {
            *error = QStringLiteral("Cannot back up %1").arg(name);
            return false;
        }
    }
    return true;
}

bool HubSelfUpdate::approve(QString* error, const QDateTime& now)
{
    UpdateStateRecord record = UpdateStateStore::load(kProduct);
    if (!record.found ||
        record.state != QStringLiteral("AwaitingUserApproval")) {
        *error = QStringLiteral("No hub update is awaiting approval");
        return false;
    }

    QString verifyError;
    if (!verifyInstaller(record.downloadPath, record.expectedSha256,
                         record.packageSize, &verifyError)) {
        record.state = QStringLiteral("VerificationFailed");
        record.failureCode = QStringLiteral("InstallerRejected");
        persist(record, now);
        *error = verifyError;
        return false;
    }

    if (!backupUserData(now, error))
        return false;

    if (!selfUpdateEnabled()) {
        *error = QStringLiteral(
            "Hub self-update is disabled on this machine "
            "(HARKEN_HUB_SELFUPDATE_DISABLE)");
        return false;
    }

    QString program;
    QStringList arguments;
    installerLaunchCommand(record.downloadPath, &program, &arguments);
    record.state = QStringLiteral("AwaitingElevation");
    record.updatedAt = now.toUTC().toString(Qt::ISODateWithMs);
    if (!persist(record, now)) {
        *error = QStringLiteral("Cannot save hub update state");
        return false;
    }
    if (!startInstallerDetached(program, arguments, error)) {
        const auto pending = record;
        record.state = QStringLiteral("AwaitingUserApproval");
        restoreState(record, pending);
        return false;
    }

    SingleInstanceGuard::sendTo(SingleInstanceGuard::backgroundServerName(),
                                {QStringLiteral("--quit")});

    return true;
}

bool HubSelfUpdate::approveViaBridge(const QString& restartMode,
                                          QString* error, const QDateTime& now)
{
    UpdateStateRecord record = UpdateStateStore::load(kProduct);
    if (!record.found ||
        (record.state != QStringLiteral("AwaitingRestart") &&
         record.state != QStringLiteral("AwaitingUserApproval"))) {
        *error = QStringLiteral("No hub update is ready to land");
        return false;
    }

    if (!selfUpdateEnabled()) {
        *error = QStringLiteral(
            "Hub self-update is disabled on this machine "
            "(HARKEN_HUB_SELFUPDATE_DISABLE)");
        return false;
    }
    if (!BridgeDispatcher::available()) {
        *error = QStringLiteral("The update bridge is not available");
        return false;
    }

    QElapsedTimer stageTimer;
    stageTimer.start();

    QString verifyError;
    if (!verifyInstaller(record.downloadPath, record.expectedSha256,
                         record.packageSize, &verifyError)) {
        record.state = QStringLiteral("VerificationFailed");
        record.failureCode = QStringLiteral("InstallerRejected");
        persist(record, now);
        *error = verifyError;
        return false;
    }
    qInfo().noquote() << QStringLiteral("Hub landing stage VerifyInstaller:")
                      << stageTimer.restart() << QStringLiteral("ms");

    QFile envelopeFile(record.manifestEnvelopePath);
    if (!envelopeFile.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("The release manifest envelope is missing");
        return false;
    }
    const QByteArray envelope = envelopeFile.readAll();

    if (!backupUserData(now, error))
        return false;
    qInfo().noquote() << QStringLiteral("Hub landing stage Backup:")
                      << stageTimer.restart() << QStringLiteral("ms");

    const HandoffPlacement placement =
        HandoffArea::place(record.downloadPath, envelope);
    if (!placement.ok) {
        *error = placement.error;
        return false;
    }
    qInfo().noquote() << QStringLiteral("Hub landing stage HandoffPlace:")
                      << stageTimer.restart() << QStringLiteral("ms");

    QString queueError;
    if (!UpdateQueue::markHubInstall(placement.transactionId, restartMode,
                                          currentUserId(), &queueError)) {
        HandoffArea::discard(placement.transactionId);
        *error = queueError;
        return false;
    }

    const QString previousState = record.state;
    record.state = QStringLiteral("AwaitingBridge");
    record.transactionId = placement.transactionId;
    record.failureCode.clear();
    record.updatedAt = now.toUTC().toString(Qt::ISODateWithMs);
    if (!persist(record, now)) {
        HandoffArea::discard(placement.transactionId);
        *error = QStringLiteral("Cannot save hub update state");
        return false;
    }

    QString dispatchError;
    if (!BridgeDispatcher::dispatch(&dispatchError)) {
        HandoffArea::discard(placement.transactionId);
        const auto pending = record;
        record.state = previousState;
        record.transactionId.clear();
        restoreState(record, pending);
        *error = dispatchError;
        return false;
    }
    qInfo().noquote() << QStringLiteral("Hub landing stage Dispatch:")
                      << stageTimer.restart() << QStringLiteral("ms");
    return true;
}

bool HubSelfUpdate::checkNow()
{
    checker_.invalidateCache();
    return checker_.checkNow();
}

void HubSelfUpdate::installerLaunchCommand(const QString& installerPath,
                                                QString* program,
                                                QStringList* arguments)
{
    arguments->clear();
    if (installerPath.endsWith(QStringLiteral(".pkg"), Qt::CaseInsensitive)) {
        QString quotedPath = installerPath;
        quotedPath.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
        QString shellCommand = QStringLiteral(
                                   "/usr/sbin/installer -pkg '%1' -target /")
                                   .arg(quotedPath);
        shellCommand.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        shellCommand.replace(QLatin1Char('"'), QStringLiteral("\\\""));
        *program = QStringLiteral("/usr/bin/osascript");
        *arguments << QStringLiteral("-e")
                   << QStringLiteral(
                          "do shell script \"%1\" with administrator "
                          "privileges")
                          .arg(shellCommand);
        return;
    }
    const QString logPath =
        QFileInfo(installerPath).dir().filePath(QStringLiteral("install.log"));
    *program = installerPath;
    *arguments << QStringLiteral("/qb!") << QStringLiteral("/norestart")
               << QStringLiteral("HARKENSELFUPDATE=1") << QStringLiteral("/l*v")
               << QDir::toNativeSeparators(logPath);
}

bool HubSelfUpdate::recoverStaleElevation(const QString& currentVersion,
                                               const QDateTime& now)
{
    constexpr qint64 kElevationStaleMs = 15ll * 60 * 1000;

    UpdateStateRecord record = UpdateStateStore::load(kProduct);
    if (!record.found ||
        (record.state != QStringLiteral("AwaitingElevation") &&
         record.state != QStringLiteral("AwaitingBridge")))
        return false;
    if (record.version == currentVersion)
        return false;
    const QDateTime updatedAt =
        QDateTime::fromString(record.updatedAt, Qt::ISODateWithMs);
    if (updatedAt.isValid() && updatedAt.msecsTo(now) < kElevationStaleMs)
        return false;
    if (record.state == QStringLiteral("AwaitingBridge") &&
        UpdateQueue::hasPendingHubUpdate())
        return false;
    record.state = (selfUpdateEnabled() && BridgeDispatcher::available())
                       ? QStringLiteral("AwaitingRestart")
                       : QStringLiteral("AwaitingUserApproval");
    record.transactionId.clear();
    record.failureCode.clear();
    return UpdateStateStore::save(record, now);
}

bool HubSelfUpdate::reconcileLandingStateAtStartup(const QDateTime& now)
{
    UpdateStateRecord record = UpdateStateStore::load(kProduct);
    if (!record.found)
        return false;
    const bool bridgeLanding =
        selfUpdateEnabled() && BridgeDispatcher::available();
    if (record.state == QStringLiteral("AwaitingRestart") && !bridgeLanding) {
        record.state = QStringLiteral("AwaitingUserApproval");
        return UpdateStateStore::save(record, now);
    }
    if (record.state == QStringLiteral("AwaitingUserApproval") &&
        bridgeLanding && isBridgeInstallerFile(record.downloadPath) &&
        QFileInfo::exists(record.manifestEnvelopePath)) {
        record.state = QStringLiteral("AwaitingRestart");
        return UpdateStateStore::save(record, now);
    }
    return false;
}

bool HubSelfUpdate::finalizeAfterRestart(const QString& currentVersion)
{
    const UpdateStateRecord record = UpdateStateStore::load(kProduct);
    if (!record.found)
        return false;
    if (record.state != QStringLiteral("AwaitingElevation") &&
        record.state != QStringLiteral("AwaitingBridge"))
        return false;
    if (record.version != currentVersion)
        return false;
    if (!record.downloadPath.isEmpty())
        QFile::remove(record.downloadPath);
    if (!record.manifestEnvelopePath.isEmpty())
        QFile::remove(record.manifestEnvelopePath);
    return UpdateStateStore::clear(kProduct);
}

}
