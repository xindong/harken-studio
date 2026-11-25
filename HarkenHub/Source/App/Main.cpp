#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <QFont>
#include <QFontDatabase>
#include <QIcon>
#include <QStyleFactory>
#include <QStyleHints>

#include "HubVersion.h"
#include "harken_buildinfo.h"

#include <QCoreApplication>
#include <QDateTime>

#include "App/FileLog.h"
#include "Cli/HubCli.h"
#include "App/SingleInstanceGuard.h"
#include "Launch/ProjectOpenFlow.h"
#include "Legal/ConsentStore.h"
#include "Localization/HubLocalization.h"
#include "Maintenance/BuildPinRegistry.h"
#include "Maintenance/CacheManager.h"
#include "Maintenance/LogExporter.h"
#include "Platform/AclHardening.h"
#include "Platform/LoginLaunch.h"
#include "Platform/PlatformPaths.h"
#include "Platform/ProcessScan.h"
#include "Platform/UpdateBridgeDeployment.h"
#include "Platform/UpdateBridgeTask.h"
#include "Projects/ProjectStore.h"
#include "Security/TrustAnchors.h"
#include "Settings/HubSettings.h"
#include "Transactions/HandoffArea.h"
#include "Transactions/InstallExecutor.h"
#include "Transactions/RepairExecutor.h"
#include "Transactions/TransactionDiagnostics.h"
#include "Transactions/TransactionReclaimer.h"
#include "Transactions/UninstallExecutor.h"
#include "UI/MessageDialog.h"
#include "UI/ConsentDialog.h"
#include "UI/HubWindow.h"
#include "UI/HubIcons.h"
#include "UI/HubTheme.h"
#include "Update/BridgeDispatcher.h"
#include "Update/HubSelfUpdate.h"
#include "Update/SelfUpdateHealthCheck.h"
#include "Update/ServerEndpoints.h"
#include "Update/UpdateBridgeConsumer.h"
#include "Update/UpdateChecker.h"
#include "Update/UpdateCoordinator.h"
#include "Update/UpdateNotifier.h"
#include "Update/UpdateQueue.h"
#include "Versions/VersionScanner.h"

#include <QFileSystemWatcher>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QSettings>
#include <QSet>
#include <QTimer>

#include <utility>

#ifdef Q_OS_WIN
#include "Platform/UserSession.h"
#include "Platform/Windows/ShellIcons.h"
#include "Platform/Windows/UserSessionReady.h"
#include <QThreadPool>
#include <windows.h>
#endif

namespace {

QString projectArgumentIn(const QStringList& arguments)
{
    for (int i = 1; i < arguments.size(); ++i) {
        const QString& argument = arguments.at(i);
        if (argument == QStringLiteral("--register-local-build")) {
            ++i;
            continue;
        }
        if (argument.startsWith(QStringLiteral("--")))
            continue;
        const QFileInfo info(argument);
        if (info.exists())
            return info.filePath();
    }
    return QString();
}

QString localBuildArgumentIn(const QStringList& arguments)
{
    const QString prefix = QStringLiteral("--register-local-build=");
    for (const QString& argument : arguments) {
        if (argument.startsWith(prefix))
            return argument.mid(prefix.size());
    }
    const int option =
        arguments.indexOf(QStringLiteral("--register-local-build"));
    if (option < 0 || option + 1 >= arguments.size())
        return QString();
    return arguments.at(option + 1);
}

QString harkenProjectArgumentIn(const QStringList& arguments)
{
    const QString prefix = QStringLiteral("--harken-project=");
    for (const QString& argument : arguments) {
        if (argument.startsWith(prefix))
            return argument.mid(prefix.size());
    }
    return {};
}

QString unityProjectArgumentIn(const QStringList& arguments)
{
    const QString prefix = QStringLiteral("--unity-project=");
    for (const QString& argument : arguments) {
        if (argument.startsWith(prefix))
            return argument.mid(prefix.size());
    }
    return {};
}

bool revealsLocalBuild(const QStringList& arguments)
{
    return arguments.contains(QStringLiteral("--reveal-local-build"));
}

QString hubTr(const char* sourceText)
{
    return QCoreApplication::translate("Main", sourceText);
}

void confirmHubReady(const QStringList& arguments)
{
#ifdef Q_OS_WIN
    const int eventIndex = arguments.indexOf(QStringLiteral("--update-ready-event"));
    if (eventIndex < 0 || eventIndex + 1 >= arguments.size())
        return;
    static QSet<QString> confirmedEvents;
    const QString eventName = arguments.at(eventIndex + 1);
    if (confirmedEvents.contains(eventName))
        return;
    QString error;
    if (!harken::hub::UserSessionReady::signal(eventName, &error)) {
        qWarning().noquote() << "Hub startup confirmation failed:" << error;
    } else {
        confirmedEvents.insert(eventName);
        qInfo() << "Hub startup confirmed:" << QCoreApplication::applicationPid();
    }
#else
    Q_UNUSED(arguments);
#endif
}

void confirmConsentReady(const QStringList& arguments)
{
    auto* dialog = qobject_cast<harken::hub::ConsentDialog*>(QApplication::activeModalWidget());
    if (dialog && dialog->isVisible()) {
        dialog->raise();
        dialog->activateWindow();
        confirmHubReady(arguments);
    }
}

}

int main(int argc, char* argv[])
{
#ifdef Q_OS_WIN
    if (argc == 2 && qstrcmp(argv[1], "--launch-after-setup") == 0) {
        QCoreApplication app(argc, argv);
        return harken::hub::launchInDesktopUserSession(QCoreApplication::applicationFilePath()) ? 0 : 1;
    }
#endif
    QString logRole = QStringLiteral("gui");
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--background") == 0) {
            logRole = QStringLiteral("background");
            break;
        }
        if (qstrncmp(argv[i], "--", 2) == 0 &&
            qstrcmp(argv[i], "--show-versions") != 0 &&
            qstrcmp(argv[i], "--after-update") != 0 &&
            qstrcmp(argv[i], "--update-ready-event") != 0)
            logRole = QStringLiteral("cli");
    }
    QCoreApplication::setOrganizationName(QStringLiteral("HarkenSound"));
    QCoreApplication::setApplicationName(QStringLiteral("HarkenHub"));
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--cli") != 0) continue;
        QCoreApplication app(argc, argv);
        app.setApplicationVersion(QString::fromUtf8(harken::hub::HubVersion.data(), int(harken::hub::HubVersion.size())));
#ifdef Q_OS_WIN
        const auto outputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
        const bool hasOutput = outputHandle && outputHandle != INVALID_HANDLE_VALUE;
        const auto errorHandle = GetStdHandle(STD_ERROR_HANDLE);
        const bool hasError = errorHandle && errorHandle != INVALID_HANDLE_VALUE;
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* stream = nullptr;
            if (!hasOutput) freopen_s(&stream, "CONOUT$", "w", stdout);
            if (!hasError) freopen_s(&stream, "CONOUT$", "w", stderr);
        }
#endif
        return harken::hub::Cli::run(app.arguments());
    }
    harken::hub::FileLog::install(logRole);
    QFile::remove(harken::hub::userDataDir() + QStringLiteral("/auth-token.bin"));
    QFile::remove(harken::hub::userDataDir() + QStringLiteral("/auth-state.json"));

    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--check-updates") == 0) {
            QCoreApplication app(argc, argv);
            QString product = QStringLiteral("harken");
            for (int j = 1; j < argc; ++j) {
                if (qstrcmp(argv[j], "--product") != 0)
                    continue;
                if (j + 1 >= argc) {
                    qCritical() << "--product requires hub or harken";
                    return 2;
                }
                product = QString::fromLatin1(argv[++j]);
            }
            if (product != QStringLiteral("hub") &&
                product != QStringLiteral("harken")) {
                qCritical() << "--product requires hub or harken";
                return 2;
            }

            const QUrl serverBaseUrl =
                harken::hub::ServerEndpoints::resolvedBaseUrl();

            harken::hub::UpdateChecker checker;
            harken::hub::UpdateChecker::Options options;
            QList<QUrl> manifestUrls = harken::hub::ServerEndpoints::manifestUrls(
                serverBaseUrl, product);
            options.latestUrl = manifestUrls.takeFirst();
            options.fallbackUrls = manifestUrls;
            options.trustedHttpOrigin = serverBaseUrl;
            options.product = product;
            options.channel = harken::hub::ServerEndpoints::hostChannel();
            options.manifestPublicKey = harken::hub::TrustAnchors::manifestPublicKey();
            int exitCode = 2;
            QObject::connect(
                &checker, &harken::hub::UpdateChecker::checkFinished, &app,
                [&exitCode, &app, &product](const harken::hub::UpdateCheckResult& result) {
                    qInfo().noquote() << "Update product:" << product;
                    if (!result.ok) {
                        qCritical().noquote()
                            << "Update check failed:" << result.error;
                    } else if (result.notModified) {
                        exitCode = 0;
                        qInfo().noquote() << "Update manifest is unchanged";
                    } else if (result.manifestAbsent) {
                        exitCode = 0;
                        qInfo().noquote()
                            << "No signed update manifest is published";
                    } else if (result.manifest.product !=
                               (product == QStringLiteral("hub")
                                    ? QStringLiteral("HarkenHub")
                                    : QStringLiteral("HarkenStudio"))) {
                        qCritical().noquote()
                            << "Update check failed: manifest product mismatch";
                    } else {
                        exitCode = 0;
                        qInfo().noquote()
                            << "Update manifest verified:"
                            << result.manifest.version
                            << result.manifest.buildId
                            << QStringLiteral("sequence %1")
                                   .arg(result.manifest.releaseSequence);
                    }
                    app.quit();
                });
            if (!checker.start(options)) {
                qCritical().noquote()
                    << "Update check failed: invalid server base URL";
                return 2;
            }
            app.exec();
            return exitCode;
        }
    }

    for (int i = 1; i + 1 < argc; ++i) {
        if (qstrcmp(argv[i], "--install-package") == 0) {
            QCoreApplication app(argc, argv);
            bool explicitInstall = false;
            QString reviewedDestination;
            QString reviewedIndexHash;
            QString reviewedManifestHash;
            for (int j = 1; j < argc; ++j) {
                if (j + 1 < argc && qstrcmp(argv[j], "--reviewed-destination") == 0) reviewedDestination = app.arguments().value(j + 1);
                if (j + 1 < argc && qstrcmp(argv[j], "--reviewed-index") == 0) reviewedIndexHash = QString::fromLatin1(argv[j + 1]);
                if (j + 1 < argc && qstrcmp(argv[j], "--reviewed-manifest") == 0) reviewedManifestHash = QString::fromLatin1(argv[j + 1]);
                if (qstrcmp(argv[j], "--explicit-install") == 0)
                    explicitInstall = true;
            }
            const auto outcome = harken::hub::InstallExecutor::run(
                QString::fromLatin1(argv[i + 1]),
                harken::hub::TrustAnchors::manifestPublicKey(),
                QDateTime::currentDateTimeUtc(), explicitInstall, reviewedDestination, reviewedIndexHash, reviewedManifestHash);
            if (!outcome.ok) {
                qCritical().noquote()
                    << "Install failed:" << outcome.failureCode << outcome.error;
                return outcome.exitCode();
            }
            qInfo().noquote() << "Installed:" << outcome.installPath;
            return 0;
        }
        if (qstrcmp(argv[i], "--uninstall-version") == 0) {
            QCoreApplication app(argc, argv);
            QString reviewedSnapshot;
            for (int j = 1; j + 1 < argc; ++j)
                if (qstrcmp(argv[j], "--reviewed-snapshot") == 0) reviewedSnapshot = QString::fromLatin1(argv[j + 1]);
            const auto outcome = harken::hub::UninstallExecutor::run(
                QString::fromLatin1(argv[i + 1]),
                QDateTime::currentDateTimeUtc(), reviewedSnapshot);
            if (!outcome.ok) {
                qCritical().noquote() << "Uninstall failed:"
                                      << outcome.failureCode << outcome.error;
                return 2;
            }
            qInfo().noquote() << "Uninstalled:" << outcome.buildId;
            return 0;
        }
        if (qstrcmp(argv[i], "--repair-version") == 0) {
            QCoreApplication app(argc, argv);
            QString transactionId;
            QString reviewedSnapshot;
            QString reviewedManifestHash;
            for (int j = 1; j + 1 < argc; ++j) {
                if (qstrcmp(argv[j], "--reviewed-snapshot") == 0) reviewedSnapshot = QString::fromLatin1(argv[j + 1]);
                if (qstrcmp(argv[j], "--reviewed-manifest") == 0) reviewedManifestHash = QString::fromLatin1(argv[j + 1]);
                if (qstrcmp(argv[j], "--transaction-id") == 0)
                    transactionId = QString::fromLatin1(argv[j + 1]);
            }
            const auto outcome = harken::hub::RepairExecutor::run(
                QString::fromLatin1(argv[i + 1]),
                QDateTime::currentDateTimeUtc(), transactionId, reviewedSnapshot, reviewedManifestHash);
            if (!outcome.ok) {
                qCritical().noquote() << "Repair failed:"
                                      << outcome.failureCode << outcome.error;
                return 2;
            }
            qInfo().noquote() << "Repaired:" << outcome.buildId;
            return 0;
        }
        if (qstrcmp(argv[i], "--export-logs") == 0) {
            QCoreApplication app(argc, argv);
            QString error;
            if (!harken::hub::LogExporter::exportDiagnostics(
                    QString::fromLocal8Bit(argv[i + 1]), &error)) {
                qCritical().noquote() << "Log export failed:" << error;
                return 2;
            }
            qInfo().noquote() << "Logs exported to" << argv[i + 1];
            return 0;
        }
        if (qstrcmp(argv[i], "--health-handshake") == 0) {
            QCoreApplication app(argc, argv);
            QLocalSocket socket;
            socket.connectToServer(QString::fromLatin1(argv[i + 1]));
            if (!socket.waitForConnected(5000)) {
                qCritical().noquote()
                    << "Health handshake failed:" << socket.errorString();
                return 2;
            }
            QJsonObject report;
            report[QStringLiteral("product")] = QStringLiteral("HarkenHub");
            report[QStringLiteral("healthy")] = true;
            report[QStringLiteral("version")] = QString::fromUtf8(
                harken::hub::HubVersion.data(),
                static_cast<int>(harken::hub::HubVersion.size()));
            report[QStringLiteral("buildId")] = QString::fromUtf8(
                Harken::BuildInfo::BuildId.data(),
                static_cast<int>(Harken::BuildInfo::BuildId.size()));
            const QByteArray line =
                QJsonDocument(report).toJson(QJsonDocument::Compact) +
                QByteArrayLiteral("\n");
            socket.write(line);
            socket.waitForBytesWritten(5000);
            socket.disconnectFromServer();
            return 0;
        }
    }

    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--consume-update-queue") == 0) {
            QCoreApplication app(argc, argv);
            const auto report = harken::hub::UpdateBridgeConsumer::run(
                harken::hub::TrustAnchors::manifestPublicKey(),
                QDateTime::currentDateTimeUtc(),
                harken::hub::UpdateBridgeConsumer::defaultHooks());
            for (const QString& detail : report.details)
                qInfo().noquote() << detail;
            qInfo().noquote()
                << QStringLiteral(
                       "Update queue consumed: %1 installed, %2 failed, "
                       "%3 deferred")
                       .arg(report.installed)
                       .arg(report.failed)
                       .arg(report.deferred);
            return report.failed > 0 ? 2 : 0;
        }
#ifdef Q_OS_WIN
        if (qstrcmp(argv[i], "--migrate-shell-shortcuts") == 0) {
            QCoreApplication app(argc, argv);
            harken::hub::migrateShellShortcuts();
            return 0;
        }
#endif
        if (qstrcmp(argv[i], "--register-update-bridge") == 0) {
            QCoreApplication app(argc, argv);
            QString bridgeExePath = QCoreApplication::applicationFilePath();
#ifdef Q_OS_WIN
            QString deployError;
            const QString deployed = harken::hub::deployUpdateBridgePayload(
                QCoreApplication::applicationDirPath() +
                    QStringLiteral("/Updater"),
                harken::hub::machineDataDir() + QStringLiteral("/Updater"),
                QString::fromUtf8(
                    Harken::BuildInfo::BuildId.data(),
                    static_cast<int>(Harken::BuildInfo::BuildId.size())),
                harken::hub::updaterAclSddl(), &deployError);
            if (deployed.isEmpty()) {
                qCritical().noquote()
                    << "Update bridge deployment failed:" << deployError;
                return 2;
            }
            bridgeExePath = deployed;
#endif
            QString error;
            if (!harken::hub::registerUpdateBridgeTask(bridgeExePath, &error)) {
                qCritical().noquote()
                    << "Update bridge registration failed:" << error;
                return 2;
            }
#ifdef Q_OS_WIN
            harken::hub::pruneUpdateBridgePayloads(
                harken::hub::machineDataDir() + QStringLiteral("/Updater"),
                bridgeExePath);
#endif
            qInfo().noquote() << "Update bridge registered:"
                              << harken::hub::updateBridgeTaskName() << "->"
                              << QDir::toNativeSeparators(bridgeExePath);
            return 0;
        }
        if (qstrcmp(argv[i], "--unregister-update-bridge") == 0) {
            QCoreApplication app(argc, argv);
            QString error;
            if (!harken::hub::unregisterUpdateBridgeTask(&error)) {
                qCritical().noquote()
                    << "Update bridge removal failed:" << error;
                return 2;
            }
#ifdef Q_OS_WIN
            QDir(harken::hub::machineDataDir() + QStringLiteral("/Updater"))
                .removeRecursively();
#endif
            qInfo().noquote() << "Update bridge unregistered";
            return 0;
        }
        if (qstrcmp(argv[i], "--await-health-handshake") == 0) {
            QCoreApplication app(argc, argv);
            const auto report = harken::hub::SelfUpdateHealthCheck::runInstalledCheck(
                QCoreApplication::applicationFilePath(), 30000);
            if (!report.ok) {
                qCritical().noquote()
                    << "Health handshake failed:" << report.error;
                return 2;
            }
            const QString ownVersion = QString::fromUtf8(
                harken::hub::HubVersion.data(),
                static_cast<int>(harken::hub::HubVersion.size()));
            if (report.version != ownVersion) {
                qCritical().noquote()
                    << "Health handshake reported version" << report.version
                    << "but expected" << ownVersion;
                return 2;
            }
            qInfo().noquote() << "Health handshake verified:" << report.version
                              << report.buildId;
            return 0;
        }
        if (qstrcmp(argv[i], "--diagnose-transactions") == 0) {
            QCoreApplication app(argc, argv);
            const auto summaries = harken::hub::TransactionDiagnostics::summarize();
            const QJsonDocument doc(
                harken::hub::TransactionDiagnostics::toJson(summaries));
            fputs(doc.toJson(QJsonDocument::Indented).constData(), stdout);
            return 0;
        }
        if (qstrcmp(argv[i], "--background") == 0) {
            QApplication app(argc, argv);
            harken::hub::HubIcons::installApplicationIcon(app);
            harken::hub::HubLocalization::install(&app);
            app.setQuitOnLastWindowClosed(false);

            const int resumeIndex = app.arguments().indexOf(QStringLiteral("--resume-after-pid"));
            if (resumeIndex >= 0) {
                const qint64 previousPid = app.arguments().value(resumeIndex + 1).toLongLong();
                if (previousPid <= 0 || previousPid == QCoreApplication::applicationPid()) return 2;
                if (harken::hub::isProcessRunning(previousPid)) {
                    QEventLoop waiting;
                    QTimer poll;
                    QObject::connect(&poll, &QTimer::timeout, &waiting, [&] {
                        if (!harken::hub::isProcessRunning(previousPid)) waiting.quit();
                    });
                    poll.start(250);
                    waiting.exec();
                }
            }
            harken::hub::SingleInstanceGuard guard(
                harken::hub::SingleInstanceGuard::backgroundServerName());
            if (!guard.isPrimary()) {
                guard.notifyPrimary(app.arguments());
                return 0;
            }
#ifdef Q_OS_WIN
            QTimer::singleShot(0, &app, [] {
                QSettings settings;
                if (settings.value(QStringLiteral("registerBackgroundOnLogin"), true).toBool() &&
                    !harken::hub::isBackgroundOnLoginRegistered())
                    harken::hub::registerBackgroundOnLogin();
                QThreadPool::globalInstance()->start(&harken::hub::refreshShellIconsIfChanged);
            });
#endif

            const QString currentVersion = QString::fromUtf8(
                harken::hub::HubVersion.data(),
                static_cast<int>(harken::hub::HubVersion.size()));
            const bool selfUpdateFinalized =
                harken::hub::HubSelfUpdate::finalizeAfterRestart(currentVersion);
            if (selfUpdateFinalized) {
                harken::hub::HubSettings::setPendingUpdateNotesVersion(currentVersion);
                qInfo().noquote()
                    << "Hub self-update to" << currentVersion
                    << "completed";
            }
            if (harken::hub::HubSelfUpdate::recoverStaleElevation(
                    currentVersion, QDateTime::currentDateTimeUtc()))
                qInfo().noquote()
                    << "Recovered a stale hub update elevation request";
            harken::hub::HubSelfUpdate::reconcileLandingStateAtStartup(
                QDateTime::currentDateTimeUtc());

            const auto reclaim = harken::hub::TransactionReclaimer::reclaimAtStartup(
                QDateTime::currentDateTimeUtc());
            for (const QString& error : reclaim.installRecoveryErrors)
                qWarning().noquote() << "Install recovery failed:" << error;
            for (const QString& transactionId : reclaim.reclaimedTransactionIds)
                qInfo().noquote()
                    << "Reclaimed unfinished install transaction"
                    << transactionId;

            harken::hub::CacheCleanOptions cleanOptions;
            for (const QString& product : {QStringLiteral("HarkenStudio"),
                                           QStringLiteral("HarkenHub")}) {
                const auto update = harken::hub::UpdateStateStore::load(product);
                if (!update.downloadPath.isEmpty()) {
                    cleanOptions.protectedFiles << update.downloadPath
                        << update.downloadPath + QStringLiteral(".part")
                        << update.downloadPath + QStringLiteral(".part.meta");
                }
                if (!update.manifestEnvelopePath.isEmpty())
                    cleanOptions.protectedFiles << update.manifestEnvelopePath;
            }
            harken::hub::CacheManager::clean(harken::hub::userDataDir(), cleanOptions,
                                     QDateTime::currentDateTimeUtc());

            {
                harken::hub::ProjectStore pinStore;
                if (pinStore.open(harken::hub::userDataDir() +
                                  QStringLiteral("/projects.db"))) {
                    QString pinError;
                    if (!harken::hub::BuildPinRegistry::refreshFromProjects(pinStore,
                                                                    &pinError))
                        qWarning().noquote()
                            << "Build pin refresh failed:" << pinError;
                }
            }

            if (harken::hub::UpdateQueue::hasPendingHubUpdate() &&
                !harken::hub::SingleInstanceGuard::isServerActive(
                    harken::hub::SingleInstanceGuard::guiServerName())) {
                QString dispatchError;
                if (!harken::hub::BridgeDispatcher::dispatch(&dispatchError)) {
                    qWarning().noquote()
                        << "Update bridge dispatch failed:" << dispatchError;
                } else {
                    qInfo().noquote()
                        << "A hub update is queued; the background service "
                           "exits so the update bridge can land it";
                    return 0;
                }
            }

            harken::hub::UpdateCoordinator coordinator;
            harken::hub::HubSelfUpdate selfUpdate;
            harken::hub::UpdateNotifier notifier;
            notifier.setTrayIcon(app.windowIcon());

            notifier.attach(&coordinator);
            notifier.attach(&selfUpdate);

            QTimer hubUpdateLandingTimer;
            hubUpdateLandingTimer.setInterval(1000);
            const auto landHubUpdate =
                [&app, &selfUpdate,
                 &hubUpdateLandingTimer](const harken::hub::UpdateStateRecord& record) {
                    if (record.product != QStringLiteral("HarkenHub") ||
                        record.state != QStringLiteral("AwaitingRestart") ||
                        !harken::hub::HubSettings::autoSilentlyUpdateHub()) {
                        hubUpdateLandingTimer.stop();
                        return;
                    }
                    if (harken::hub::SingleInstanceGuard::isServerActive(
                            harken::hub::SingleInstanceGuard::guiServerName())) {
                        if (!hubUpdateLandingTimer.isActive())
                            hubUpdateLandingTimer.start();
                        return;
                    }
                    hubUpdateLandingTimer.stop();
                    QString error;
                    if (selfUpdate.approveViaBridge(
                            QStringLiteral("background"), &error,
                            QDateTime::currentDateTimeUtc())) {
                        qInfo().noquote()
                            << "Hub update handed to the update bridge; "
                               "the background service exits";
                        app.quit();
                    } else {
                        qWarning().noquote()
                            << "Hub update landing failed:" << error;
                    }
                };
            QObject::connect(&selfUpdate,
                             &harken::hub::HubSelfUpdate::stateChanged, &app,
                             landHubUpdate);
            QObject::connect(
                &hubUpdateLandingTimer, &QTimer::timeout, &app,
                [&selfUpdate, &landHubUpdate]() {
                    landHubUpdate(selfUpdate.currentState());
                });
            QObject::connect(
                &guard, &harken::hub::SingleInstanceGuard::activationRequested, &app,
                [&app, &coordinator, &selfUpdate, &notifier,
                 &landHubUpdate](const QStringList& arguments) {
                    if (arguments.contains(QStringLiteral("--quit"))) {
                        app.quit();
                        return;
                    }
                    if (arguments.contains(
                            QStringLiteral("--settings-changed"))) {
                        harken::hub::HubLocalization::install(&app);
                        notifier.retranslateTray();
                        coordinator.setAutomaticDownloadEnabled(
                            harken::hub::HubSettings::autoDownloadStudio());
                        selfUpdate.setAutomaticUpdateEnabled(
                            harken::hub::HubSettings::autoSilentlyUpdateHub());
                        landHubUpdate(selfUpdate.currentState());
                    }
                    if (arguments.contains(QStringLiteral("--check-now"))) {
                        coordinator.checkNow();
                        selfUpdate.checkNow();
                    } else if (arguments.contains(
                                   QStringLiteral("--check-studio"))) {
                        coordinator.checkNow(false);
                    }
                    confirmHubReady(arguments);
                });

            bool anyStarted = false;

            const QUrl serverBaseUrl =
                harken::hub::ServerEndpoints::resolvedBaseUrl();

            harken::hub::UpdateCoordinator::Options options;
            options.serverBaseUrl = serverBaseUrl;
            options.manifestPublicKey =
                harken::hub::TrustAnchors::manifestPublicKey();
            options.automaticDownloadEnabled =
                harken::hub::HubSettings::autoDownloadStudio();
            anyStarted = coordinator.start(options) || anyStarted;

            harken::hub::HubSelfUpdate::Options hubOptions;
            hubOptions.serverBaseUrl = serverBaseUrl;
            hubOptions.automaticUpdateEnabled =
                harken::hub::HubSettings::autoSilentlyUpdateHub();
            hubOptions.manifestPublicKey =
                harken::hub::TrustAnchors::manifestPublicKey();
            hubOptions.currentVersion = QString::fromUtf8(
                harken::hub::HubVersion.data(),
                static_cast<int>(harken::hub::HubVersion.size()));
            anyStarted = selfUpdate.start(hubOptions) || anyStarted;

            if (app.arguments().contains(QStringLiteral("--check-now"))) {
                coordinator.checkNow();
                selfUpdate.checkNow();
            }

            if (!anyStarted) {
                qInfo().noquote()
                    << "Background mode has no usable update service "
                       "configuration; exiting";
                return 0;
            }

            QFileSystemWatcher handoffWatcher;
            if (QDir(harken::hub::HandoffArea::rootDir()).exists())
                handoffWatcher.addPath(harken::hub::HandoffArea::rootDir());
            QObject::connect(
                &handoffWatcher, &QFileSystemWatcher::directoryChanged, &app,
                [&app]() {
                    if (harken::hub::UpdateQueue::hasPendingHubUpdate()) {
                        qInfo().noquote()
                            << "A hub update is queued; the background "
                               "service exits so the update bridge can land it";
                        app.quit();
                    }
                });

            notifier.showTrayIcon();
            if (selfUpdateFinalized)
                notifier.notifyHubUpdated(currentVersion);
            notifier.handleState(coordinator.currentState());
            notifier.handleState(selfUpdate.currentState());
            landHubUpdate(selfUpdate.currentState());
            QTimer::singleShot(0, &app, [&app]() {
                confirmHubReady(app.arguments());
            });
            return app.exec();
        }
    }

    QApplication app(argc, argv);
    harken::hub::HubIcons::installApplicationIcon(app);
    harken::hub::HubLocalization::install(&app);
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion")))
        QApplication::setStyle(fusion);
    app.setOrganizationDomain(QStringLiteral("harkensound.com"));
    app.setApplicationVersion(
        QString::fromUtf8(harken::hub::HubVersion.data(),
                          static_cast<int>(harken::hub::HubVersion.size())));

    qInfo().noquote() << "Harken Hub"
                      << QString::fromUtf8(Harken::BuildInfo::Label.data(),
                                           static_cast<int>(Harken::BuildInfo::Label.size()));

    harken::hub::SingleInstanceGuard guard(harken::hub::SingleInstanceGuard::guiServerName());
    if (!guard.isPrimary()) {
#ifdef Q_OS_WIN
        const qint64 primaryProcessId = guard.primaryProcessId();
        if (primaryProcessId > 0 && primaryProcessId <= MAXDWORD)
            AllowSetForegroundWindow(static_cast<DWORD>(primaryProcessId));
#endif
        if (guard.notifyPrimary(app.arguments()))
            return 0;
        if (!guard.retryPrimary()) {
            harken::hub::MessageDialog::critical(
                nullptr, hubTr("Harken Hub"),
                hubTr("Another Harken Hub instance is running but not "
                      "responding. Please close it and try again."));
            return 1;
        }
    }

#ifdef Q_OS_WIN
    QTimer::singleShot(0, &app, [] {
        QThreadPool::globalInstance()->start(&harken::hub::refreshShellIconsIfChanged);
    });
#endif

    QList<QStringList> pendingActivations;
    const auto startupConnection = QObject::connect(
        &guard, &harken::hub::SingleInstanceGuard::activationRequested,
        &app, [&pendingActivations](const QStringList& arguments) {
            if (arguments.contains(QStringLiteral("--quit"))) {
                qInfo() << "Hub received a quit request during startup";
                if (auto* dialog = qobject_cast<harken::hub::ConsentDialog*>(QApplication::activeModalWidget()))
                    dialog->reject();
                QCoreApplication::quit();
            } else {
                confirmConsentReady(arguments);
                pendingActivations.append(arguments);
            }
        });
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, []() {
        qInfo() << "Hub GUI exiting:" << QCoreApplication::applicationPid();
    });

    {
        const QStringList families = QFontDatabase::families();
        QString sansFamily;
        for (const QString& candidate :
             {QStringLiteral("Inter"), QStringLiteral("Segoe UI")}) {
            if (families.contains(candidate)) {
                sansFamily = candidate;
                break;
            }
        }
        if (!sansFamily.isEmpty()) {
            QFont appFont(sansFamily);
            appFont.setPointSizeF(9.0);
            app.setFont(appFont);
        }
    }

    app.setApplicationDisplayName(QStringLiteral("Harken Hub"));
    harken::hub::HubTheme::initialize();

    QTimer::singleShot(0, &app, [&app]() {
        confirmConsentReady(app.arguments());
    });
    if (!harken::hub::ConsentDialog::requestConsent(
            nullptr, harken::hub::ConsentDialog::hubDocumentId()))
        return 0;

    const bool selfUpdateFinalized =
        harken::hub::HubSelfUpdate::finalizeAfterRestart(app.applicationVersion());
    if (selfUpdateFinalized) {
        harken::hub::HubSettings::setPendingUpdateNotesVersion(
            app.applicationVersion());
        qInfo().noquote() << "Hub self-update to"
                          << app.applicationVersion() << "completed";
    }
    const bool updateNotesPending =
        harken::hub::HubSettings::pendingUpdateNotesVersion() ==
        app.applicationVersion();
    if (harken::hub::HubSelfUpdate::recoverStaleElevation(
            app.applicationVersion(), QDateTime::currentDateTimeUtc()))
        qInfo().noquote()
            << "Recovered a stale hub update elevation request";
    harken::hub::HubSelfUpdate::reconcileLandingStateAtStartup(
        QDateTime::currentDateTimeUtc());

    {
        QSettings settings;
        if (settings.value(QStringLiteral("registerBackgroundOnLogin"), true)
                .toBool()) {
            if (!harken::hub::isBackgroundOnLoginRegistered())
                harken::hub::registerBackgroundOnLogin();
            QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                    {QStringLiteral("--background")});
        }
    }

#ifdef Q_OS_MACOS
    if (QCoreApplication::applicationDirPath().startsWith(
            QStringLiteral("/Applications/"))) {
        QString bridgeError;
        if (!harken::hub::updateBridgeTaskAvailable() &&
            !harken::hub::registerUpdateBridgeTask(
                QCoreApplication::applicationFilePath(), &bridgeError))
            qWarning().noquote()
                << "Update bridge registration failed:" << bridgeError;
    }
#endif

    const auto reclaim =
        harken::hub::TransactionReclaimer::reclaimAtStartup(QDateTime::currentDateTimeUtc());
    for (const QString& error : reclaim.installRecoveryErrors)
        qWarning().noquote() << "Install recovery failed:" << error;
    for (const QString& transactionId : reclaim.reclaimedTransactionIds)
        qInfo().noquote() << "Reclaimed unfinished install transaction"
                          << transactionId;
    for (const QString& transactionId : reclaim.journalWriteFailures)
        qWarning().noquote()
            << "Reclaimed transaction could not be journaled" << transactionId;

    harken::hub::ProjectStore store;
    const QString databasePath =
        harken::hub::userDataDir() + QStringLiteral("/projects.db");
    if (!store.open(databasePath))
        qWarning().noquote() << "ProjectStore:" << store.lastError();
    else {
        QString pinError;
        if (!harken::hub::BuildPinRegistry::refreshFromProjects(store, &pinError))
            qWarning().noquote() << "Build pin refresh failed:" << pinError;
    }

    const auto activateHubWindow = [](harken::hub::HubWindow& window,
                                      const QStringList& arguments) {
        window.setWindowState(window.windowState() & ~Qt::WindowMinimized);
        window.show();
        window.raise();
        window.activateWindow();
        if (arguments.contains(QStringLiteral("--show-new-project")))
            window.showNewProjectPage();
        else if (arguments.contains(QStringLiteral("--show-versions")))
            window.showVersionsPage();
        else if (arguments.contains(QStringLiteral("--edit-integration"))
                 || arguments.contains(QStringLiteral("--create-integration")))
            window.showEngineIntegrationsPage(
                harkenProjectArgumentIn(arguments),
                localBuildArgumentIn(arguments),
                unityProjectArgumentIn(arguments));
        else if (arguments.contains(QStringLiteral("--show-projects")))
            window.showProjectsPage();
        const QString localBuild = localBuildArgumentIn(arguments);
        if (revealsLocalBuild(arguments))
            window.revealLocalBuild(localBuild);
        else
            window.registerLocalBuild(localBuild);
        const QString path = projectArgumentIn(arguments);
        if (!path.isEmpty())
            window.openProjectFromPath(path);
        if (arguments.contains(QStringLiteral("--after-update")))
            window.restoreAfterUpdate();
        QTimer::singleShot(300, &window, [&window, arguments]() {
            if (window.isVisible() && !window.isMinimized())
                confirmHubReady(arguments);
        });
    };

    const auto runHubWindow = [&app, &guard, &store, activateHubWindow,
                               updateNotesPending, startupConnection,
                               &pendingActivations](bool showVersions) -> int {
        harken::hub::HubWindow window(&store);

        QObject::disconnect(startupConnection);
        QObject::connect(&guard, &harken::hub::SingleInstanceGuard::activationRequested, &window,
                         [&window, activateHubWindow](const QStringList& arguments) {
                             if (arguments.contains(QStringLiteral("--quit"))) {
                                 qInfo() << "Hub GUI received a quit request";
                                 QCoreApplication::quit();
                                 return;
                             }
                             activateHubWindow(window, arguments);
                         });

        if (showVersions)
            window.showVersionsPage();
        else if (app.arguments().contains(
                     QStringLiteral("--show-new-project")))
            window.showNewProjectPage();
        else if (app.arguments().contains(QStringLiteral("--edit-integration"))
                 || app.arguments().contains(QStringLiteral("--create-integration")))
            window.showEngineIntegrationsPage(
                harkenProjectArgumentIn(app.arguments()),
                localBuildArgumentIn(app.arguments()),
                unityProjectArgumentIn(app.arguments()));
        else if (app.arguments().contains(QStringLiteral("--show-projects")))
            window.showProjectsPage();
        const QString localBuild = localBuildArgumentIn(app.arguments());
        if (revealsLocalBuild(app.arguments()))
            window.revealLocalBuild(localBuild);
        else
            window.registerLocalBuild(localBuild);
        const QString project = projectArgumentIn(app.arguments());
        if (!project.isEmpty())
            window.openProjectFromPath(project);
        window.show();
        if (updateNotesPending ||
            app.arguments().contains(QStringLiteral("--after-update"))) {
            window.restoreAfterUpdate();
            QTimer::singleShot(300, &window,
                               &harken::hub::HubWindow::restoreAfterUpdate);
        }
        if (updateNotesPending) {
            QTimer::singleShot(450, &window,
                               &harken::hub::HubWindow::showReleaseNotesAfterUpdate);
        }
        for (const QStringList& arguments : std::as_const(pendingActivations))
            activateHubWindow(window, arguments);
        pendingActivations.clear();
        QTimer::singleShot(350, &window, [&window, &app]() {
            qInfo() << "Hub GUI ready:" << QCoreApplication::applicationPid()
                    << "visible" << window.isVisible()
                    << "minimized" << window.isMinimized();
            if (window.isVisible() && !window.isMinimized())
                confirmHubReady(app.arguments());
        });
        return app.exec();
    };

    const QString initialProject = projectArgumentIn(app.arguments());
    if (!initialProject.isEmpty())
        return runHubWindow(false);

    return runHubWindow(
        app.arguments().contains(QStringLiteral("--show-versions")));
}
