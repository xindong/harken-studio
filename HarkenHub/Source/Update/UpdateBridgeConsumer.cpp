#include "Update/UpdateBridgeConsumer.h"

#include "Packaging/PackageReader.h"
#include "Platform/PlatformPaths.h"
#include "Platform/ProcessScan.h"
#include "Platform/UserSession.h"
#include "Security/BinaryTrustPolicy.h"
#include "Security/ManifestVerifier.h"
#include "Transactions/HandoffArea.h"
#include "Transactions/InstallExecutor.h"
#include "Transactions/TransactionJournal.h"
#include "Update/UpdateQueue.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QThread>

namespace VNH {

namespace {

constexpr qint64 kInstallerMaxBytes = 512ll * 1024 * 1024;

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
        *error = QStringLiteral("Handoff file size is out of bounds: %1")
                     .arg(path);
        return false;
    }
    return true;
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

struct StagingCleanup
{
    QString dir;
    ~StagingCleanup()
    {
        if (!dir.isEmpty() && QDir(dir).exists())
            QDir(dir).removeRecursively();
    }
};

void journalHub(const QString& transactionId, const QString& buildId,
                     const QString& state, const QString& failureCode)
{
    TransactionJournal::append(transactionId, QStringLiteral("HarkenHub"),
                               buildId, state, failureCode,
                               QDateTime::currentDateTimeUtc());
}

void appendStageTiming(const QString& transactionId, const QString& stage,
                       qint64 elapsedMs, const QString& detail)
{
    QJsonObject entry;
    entry[QStringLiteral("at")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    entry[QStringLiteral("transactionId")] = transactionId;
    entry[QStringLiteral("stage")] = stage;
    entry[QStringLiteral("ms")] = static_cast<double>(elapsedMs);
    if (!detail.isEmpty())
        entry[QStringLiteral("detail")] = detail;
    const QString directory = machineDataDir();
    QDir().mkpath(directory);
    QFile file(directory + QStringLiteral("/hub-update-timing.log"));
    if (file.open(QIODevice::WriteOnly | QIODevice::Append))
        file.write(QJsonDocument(entry).toJson(QJsonDocument::Compact) +
                   QByteArrayLiteral("\n"));
    qInfo().noquote() << QStringLiteral("Hub update stage %1: %2 ms %3")
                             .arg(stage)
                             .arg(elapsedMs)
                             .arg(detail);
}

constexpr int kInstallLogCap = 5;

QString persistentInstallLogPath(const QString& transactionId,
                                 const QString& fallbackDir)
{
    const QString directory = machineDataDir() + QStringLiteral("/logs");
    if (!QDir().mkpath(directory))
        return fallbackDir + QStringLiteral("/install.log");
    QDir logs(directory);
    const QStringList existing =
        logs.entryList({QStringLiteral("install-*.log")}, QDir::Files,
                       QDir::Time | QDir::Reversed);
    int excess = existing.size() - kInstallLogCap + 1;
    for (const QString& name : existing) {
        if (excess <= 0)
            break;
        if (logs.remove(name))
            --excess;
    }
    return directory + QStringLiteral("/install-") + transactionId +
           QStringLiteral(".log");
}

bool waitForQuietHub(const UpdateBridgeConsumer::Hooks& hooks)
{
    QElapsedTimer timer;
    timer.start();
    while (!hooks.runningHubProcesses().isEmpty()) {
        if (timer.elapsed() >= hooks.hubQuietTimeoutMs)
            return false;
        hooks.wait(500);
    }
    return true;
}

void consumeHubEntry(const UpdateQueueEntry& entry,
                          const QByteArray& manifestPublicKey,
                          const QDateTime& now,
                          const UpdateBridgeConsumer::Hooks& hooks,
                          BridgeConsumeReport& report)
{
    const QString transactionId = entry.transactionId;
    const QString handoffPackage = HandoffArea::packagePath(transactionId);
    const QString handoffManifest = HandoffArea::manifestPath(transactionId);

    QElapsedTimer totalTimer;
    totalTimer.start();
    QElapsedTimer stageTimer;
    stageTimer.start();

    QString checkError;
    if (!isPlainFile(handoffManifest, 1024 * 1024, &checkError) ||
        !isPlainFile(handoffPackage, kInstallerMaxBytes, &checkError)) {
        HandoffArea::discard(transactionId);
        journalHub(transactionId, QString(), QStringLiteral("Failed"),
                        QStringLiteral("HandoffRejected"));
        ++report.failed;
        report.details.append(checkError);
        return;
    }

    if (!waitForQuietHub(hooks)) {
        appendStageTiming(transactionId, QStringLiteral("WaitQuiet"),
                          stageTimer.restart(),
                          QStringLiteral("HubStillRunning"));
        journalHub(transactionId, QString(), QStringLiteral("Deferred"),
                        QStringLiteral("HubStillRunning"));
        ++report.deferred;
        report.details.append(
            QStringLiteral("Hub processes are still running; deferred %1")
                .arg(transactionId));
        return;
    }
    appendStageTiming(transactionId, QStringLiteral("WaitQuiet"),
                      stageTimer.restart(), QString());

    StagingCleanup staging;
    staging.dir = machineDataDir() + QStringLiteral("/staging/") +
                  transactionId;
    if (QDir(staging.dir).exists())
        QDir(staging.dir).removeRecursively();
    if (!QDir().mkpath(staging.dir)) {
        journalHub(transactionId, QString(), QStringLiteral("Failed"),
                        QStringLiteral("StagingFailed"));
        ++report.failed;
        report.details.append(
            QStringLiteral("Cannot create the staging directory"));
        return;
    }

#ifdef Q_OS_MACOS
    const QString stagedInstaller =
        staging.dir + QStringLiteral("/installer.pkg");
#else
    const QString stagedInstaller =
        staging.dir + QStringLiteral("/installer.exe");
#endif
    const QString stagedManifest =
        staging.dir + QStringLiteral("/release.manifest");
    QString copyError;
    if (!copyPlainFile(handoffPackage, stagedInstaller, &copyError) ||
        !copyPlainFile(handoffManifest, stagedManifest, &copyError)) {
        journalHub(transactionId, QString(), QStringLiteral("Failed"),
                        QStringLiteral("StagingFailed"));
        ++report.failed;
        report.details.append(copyError);
        return;
    }

    HandoffArea::discard(transactionId);
    appendStageTiming(transactionId, QStringLiteral("StagePackage"),
                      stageTimer.restart(), QString());

    const ManifestVerifyResult manifestResult = ManifestVerifier::verify(
        readAll(stagedManifest), manifestPublicKey, now);
    if (!manifestResult.ok) {
        journalHub(transactionId, QString(), QStringLiteral("Failed"),
                        QStringLiteral("ManifestRejected"));
        ++report.failed;
        report.details.append(manifestResult.error);
        return;
    }
    const ReleaseManifest manifest = manifestResult.manifest;
    if (manifest.product != QStringLiteral("HarkenHub")) {
        journalHub(transactionId, manifest.buildId,
                        QStringLiteral("Failed"),
                        QStringLiteral("ManifestRejected"));
        ++report.failed;
        report.details.append(
            QStringLiteral("Manifest is for product: %1").arg(manifest.product));
        return;
    }
    appendStageTiming(transactionId, QStringLiteral("VerifyManifest"),
                      stageTimer.restart(), QString());

    const QFileInfo installerInfo(stagedInstaller);
    if (manifest.packageSize > 0 &&
        installerInfo.size() != manifest.packageSize) {
        journalHub(transactionId, manifest.buildId,
                        QStringLiteral("Failed"),
                        QStringLiteral("PackageRejected"));
        ++report.failed;
        report.details.append(QStringLiteral(
            "Installer size disagrees with the manifest"));
        return;
    }
    if (PackageReader::fileSha256Hex(stagedInstaller).toLower() !=
        manifest.sha256Hex.toLower()) {
        journalHub(transactionId, manifest.buildId,
                        QStringLiteral("Failed"),
                        QStringLiteral("PackageRejected"));
        ++report.failed;
        report.details.append(
            QStringLiteral("Installer hash disagrees with the manifest"));
        return;
    }
    if (BinaryTrustPolicy::installerRequired()) {
        QString trustError;
        if (!BinaryTrustPolicy::verifyInstallerBinary(stagedInstaller,
                                                      &trustError)) {
            journalHub(transactionId, manifest.buildId,
                            QStringLiteral("Failed"),
                            QStringLiteral("BinaryTrustRejected"));
            ++report.failed;
            report.details.append(trustError);
            return;
        }
    }
    appendStageTiming(transactionId, QStringLiteral("VerifyPackage"),
                      stageTimer.restart(), QString());

    journalHub(transactionId, manifest.buildId,
                    QStringLiteral("Installing"), QString());
    const QString logPath =
        persistentInstallLogPath(transactionId, staging.dir);
    const int exitCode = hooks.runHubInstaller(stagedInstaller, logPath);
    appendStageTiming(transactionId, QStringLiteral("Install"),
                      stageTimer.restart(),
                      QStringLiteral("exit %1").arg(exitCode));
    if (exitCode != 0 && exitCode != 3010) {
        journalHub(transactionId, manifest.buildId,
                        QStringLiteral("Failed"),
                        QStringLiteral("BridgeInstallFailed"));
        ++report.failed;
        report.details.append(
            QStringLiteral("Hub installer exited with %1").arg(exitCode));
        return;
    }

    journalHub(transactionId, manifest.buildId,
                    QStringLiteral("Installed"), QString());
    ++report.installed;

    const QString restartMode = entry.restartMode ==
                                        QStringLiteral("gui")
                                    ? QStringLiteral("gui")
                                    : QStringLiteral("background");
    const QStringList restartArguments =
        restartMode == QStringLiteral("gui")
            ? QStringList{QStringLiteral("--after-update")}
            : QStringList{QStringLiteral("--background")};
    QString restartError;
    bool restarted = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        restartError.clear();
        if (hooks.restartHub(restartArguments, entry.userId, &restartError)) {
            restarted = true;
            break;
        }
        qWarning().noquote() << "Hub restart attempt" << attempt + 1
                             << "failed:" << restartError;
        if (attempt + 1 < 3)
            hooks.wait(1000);
    }
    appendStageTiming(transactionId, QStringLiteral("Restart"),
                      stageTimer.restart(),
                      restarted ? restartMode
                                : QStringLiteral("SessionLaunchFailed"));
    if (!restarted) {
        ++report.failed;
        journalHub(transactionId, manifest.buildId,
                        QStringLiteral("RestartFailed"),
                        QStringLiteral("SessionLaunchFailed"));
        report.details.append(
            QStringLiteral("Hub restart failed: %1").arg(restartError));
    }
    appendStageTiming(transactionId, QStringLiteral("Total"),
                      totalTimer.elapsed(), manifest.version);
}

}

UpdateBridgeConsumer::Hooks UpdateBridgeConsumer::defaultHooks()
{
    Hooks hooks;
    hooks.runHubInstaller = [](const QString& installerPath,
                                    const QString& logPath) {
#if !defined(Q_OS_WIN) || defined(HARKEN_HUB_TEST_OVERRIDES)
        const QString overrideCommand =
            qEnvironmentVariable("HARKEN_HUB_BRIDGE_INSTALLER_COMMAND");
        if (!overrideCommand.isEmpty()) {
            QStringList parts = QProcess::splitCommand(overrideCommand);
            if (parts.isEmpty())
                return 2;
            const QString program = parts.takeFirst();
            parts.append(installerPath);
            return QProcess::execute(program, parts);
        }
#endif
#ifdef Q_OS_MACOS
        Q_UNUSED(logPath);
        return QProcess::execute(
            QStringLiteral("/usr/sbin/installer"),
            {QStringLiteral("-pkg"), installerPath, QStringLiteral("-target"),
             QStringLiteral("/")});
#else
        return QProcess::execute(
            installerPath,
            {QStringLiteral("/qn"), QStringLiteral("/norestart"),
             QStringLiteral("HARKENSELFUPDATE=1"),
             QStringLiteral("HARKENBRIDGE=1"), QStringLiteral("/l*v"),
             QDir::toNativeSeparators(logPath)});
#endif
    };
    hooks.restartHub = [](const QStringList& arguments,
                          const QString& userId, QString* error) {
#if !defined(Q_OS_WIN) || defined(HARKEN_HUB_TEST_OVERRIDES)
        const QString overrideCommand =
            qEnvironmentVariable("HARKEN_HUB_BRIDGE_RESTART_COMMAND");
        if (!overrideCommand.isEmpty()) {
            QStringList parts = QProcess::splitCommand(overrideCommand);
            if (parts.isEmpty()) {
                *error = QStringLiteral("Invalid restart command override");
                return false;
            }
            const QString program = parts.takeFirst();
            parts.append(arguments);
            if (!QProcess::startDetached(program, parts)) {
                *error = QStringLiteral("Cannot start the restart override");
                return false;
            }
            return true;
        }
#endif
#ifdef Q_OS_MACOS
        const QString hub =
            hubInstallDir() + QStringLiteral("/HarkenHub");
#else
        const QString hub = hubInstallDir() +
                            QStringLiteral("/HarkenHub.exe");
#endif
        return launchInActiveUserSession(hub, arguments, userId, error);
    };
    hooks.runningHubProcesses = []() {
        return runningExecutablesUnder(hubInstallDir(),
                                       QCoreApplication::applicationPid());
    };
    hooks.wait = [](qint64 milliseconds) {
        QThread::msleep(static_cast<unsigned long>(milliseconds));
    };
#if !defined(Q_OS_WIN) || defined(HARKEN_HUB_TEST_OVERRIDES)
    bool timeoutValid = false;
    const qint64 timeoutOverride =
        qEnvironmentVariable("HARKEN_HUB_BRIDGE_WAIT_MS")
            .toLongLong(&timeoutValid);
    if (timeoutValid && timeoutOverride >= 0)
        hooks.hubQuietTimeoutMs = timeoutOverride;
#endif
    return hooks;
}

BridgeConsumeReport UpdateBridgeConsumer::run(
    const QByteArray& manifestPublicKey, const QDateTime& now,
    const Hooks& hooks)
{
    BridgeConsumeReport report;

    const auto entries = UpdateQueue::pendingEntries();
    for (const UpdateQueueEntry& entry : entries) {
        if (entry.product == QStringLiteral("HarkenStudio")) {
            const QString handoffPackage =
                HandoffArea::packagePath(entry.transactionId);
            const QString handoffManifest =
                HandoffArea::manifestPath(entry.transactionId);
            if (!QFileInfo::exists(handoffPackage) ||
                !QFileInfo::exists(handoffManifest)) {
                HandoffArea::discard(entry.transactionId);
                TransactionJournal::append(
                    entry.transactionId, QStringLiteral("HarkenStudio"),
                    QString(), QStringLiteral("Failed"),
                    QStringLiteral("HandoffRejected"), now);
                ++report.failed;
                report.details.append(
                    QStringLiteral("Queue entry without handoff files: %1")
                        .arg(entry.transactionId));
                continue;
            }
            const InstallOutcome outcome = InstallExecutor::run(
                entry.transactionId, manifestPublicKey, now);
            if (outcome.ok) {
                ++report.installed;
            } else if (outcome.failureCode ==
                       QStringLiteral("InstallBusy")) {
                ++report.deferred;
                report.details.append(outcome.error);
            } else {
                HandoffArea::discard(entry.transactionId);
                ++report.failed;
                report.details.append(outcome.error);
            }
            continue;
        }
        consumeHubEntry(entry, manifestPublicKey, now, hooks, report);
    }

    return report;
}

}
