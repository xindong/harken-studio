#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <QFont>
#include <QFontDatabase>
#include <QIcon>
#include <QMessageBox>
#include <QStyleFactory>
#include <QStyleHints>

#include "harken_buildinfo.h"

#include <QCoreApplication>
#include <QDateTime>

#include "App/FileLog.h"
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
#include "UI/ConsentDialog.h"
#include "UI/HubWindow.h"
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
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QSettings>
#include <QSet>
#include <QTimer>

#include <utility>

#ifdef Q_OS_WIN
#include "Platform/Windows/UserSessionReady.h"
#include <windows.h>
#endif

namespace {

const char kStyleSheet[] = R"(
QWidget {
    background-color: #0D111A;
    color: #E0E0E0;
    font-family: "Inter", "Segoe UI", sans-serif;
    font-size: 12px;
    selection-background-color: #00F0FF;
    selection-color: #000000;
}
QMainWindow { background-color: #06080D; }
#sidebarPanel { background-color: #06080D; }
#sidebarSeparator { background-color: #313848; }
#sidebar { background-color: #06080D; border: none; outline: none; }
#sidebar::item {
    margin: 0;
    padding: 12px 22px;
    border: none;
    background-color: #171C28;
    color: #AAAAAA;
}
#sidebar::item:hover { background-color: #222836; color: #CCCCCC; }
#sidebar::item:selected {
    background-color: #143C48;
    border: none;
    border-left: 2px solid #00F0FF;
    padding-left: 20px;
    color: #00F0FF;
}
  #pageTitle { font-size: 16px; font-weight: 600; color: #00F0FF; background: transparent; }
  #pageSubtitle { color: #888888; background: transparent; }
  #sidebarLogo { background: transparent; }
  #sidebarProduct { color: #00F0FF; background: transparent; font-size: 24px; font-weight: 700; }
#sidebarNote { color: #6B7488; background: transparent; font-size: 11px; }
#sidebarLinks { background: transparent; font-size: 11px; }
  #tablePanel { background-color: #0D111A; border: 1px solid #272E3C; border-radius: 0; }
  #newProjectPanel { background-color: #0D111A; border: 1px solid #3B4658; border-radius: 0; }
  QLineEdit#newProjectField, QComboBox#newProjectVersion {
      background-color: #080C14;
      border: 1px solid #465268;
      border-radius: 0;
      color: #E0E0E0;
      padding: 3px 7px;
  }
  QLineEdit#newProjectField:hover, QComboBox#newProjectVersion:hover { border-color: #627089; }
  QLineEdit#newProjectField:focus, QComboBox#newProjectVersion:focus { border-color: #00F0FF; }
  QComboBox#newProjectVersion::drop-down { border-left: 1px solid #465268; width: 24px; }
  QPushButton#newProjectBrowse, QPushButton#newProjectAddTemplate { border-color: #536078; padding: 4px 16px; }
  QPushButton#newProjectBrowse:hover, QPushButton#newProjectAddTemplate:hover { border-color: #71809A; }
  QTreeWidget#templatePreviewTree {
      background-color: #080C14;
      border: 1px solid #3F4A5E;
      border-radius: 0;
      outline: none;
  }
  QTreeWidget#templatePreviewTree::item { border: none; padding: 1px 4px; }
  QTreeWidget#templatePreviewTree QHeaderView::section {
      background-color: #1A2130;
      border: none;
      border-bottom: 1px solid #4A566A;
      color: #AAB3C2;
      padding: 6px 9px;
  }
QPushButton {
    background-color: #2A3140;
    border: 1px solid #3D4554;
    padding: 8px 16px;
    border-radius: 0;
    color: #E0E0E0;
    font-weight: 500;
}
QPushButton:hover { background-color: #333B4D; border-color: #4D5668; }
QPushButton:pressed { background-color: #00F0FF; color: #000000; }
QPushButton:disabled { background-color: #171C28; color: #555555; border-color: #272D3A; }
QPushButton#rowLaunchButton, QPushButton#newProjectButton, QPushButton#createProjectButton, QPushButton#getFromServerButton[updateAvailable="true"] {
    background-color: #00F0FF;
    border: 1px solid #00F0FF;
    color: #000000;
    font-weight: 600;
}
QPushButton#rowLaunchButton:hover, QPushButton#newProjectButton:hover, QPushButton#createProjectButton:hover, QPushButton#getFromServerButton[updateAvailable="true"]:hover { background-color: #5CF6FF; border-color: #5CF6FF; }
QPushButton#rowLaunchButton:pressed, QPushButton#newProjectButton:pressed, QPushButton#createProjectButton:pressed, QPushButton#getFromServerButton[updateAvailable="true"]:pressed { background-color: #00B0BC; border-color: #00B0BC; color: #000000; }
QPushButton#rowLaunchButton:disabled, QPushButton#newProjectButton:disabled, QPushButton#createProjectButton:disabled, QPushButton#getFromServerButton[updateAvailable="true"]:disabled { background-color: #171C28; color: #555555; border-color: #272D3A; }
QPushButton#rowLaunchButton { padding: 4px 12px; }
QPushButton#addIntegrationButton { padding: 0; font-size: 18px; }
QToolButton {
    background-color: transparent;
    border: 1px solid transparent;
    border-radius: 0;
    padding: 4px;
}
QToolButton:hover { background-color: #2A3140; border-color: #3D4554; }
QToolButton:pressed { background-color: #00F0FF; color: #000000; }
QToolButton:disabled { color: #555555; }
QToolButton#rowMenuButton {
    background-color: #2A3140;
    border: 1px solid #3D4554;
    color: #AAAAAA;
    font-size: 10px;
}
QToolButton#rowMenuButton:hover { background-color: #333B4D; border-color: #4D5668; color: #E0E0E0; }
QToolButton#settingsButton {
    background-color: transparent;
    border: none;
    padding: 5px;
}
QToolButton#settingsButton:hover { background-color: #1B2230; border: none; }
QToolButton#settingsButton:pressed,
QToolButton#settingsButton:checked { background-color: #242C3A; border: none; }
QToolButton#settingsButton::menu-indicator { image: none; }
QTableWidget {
    background-color: transparent;
    border: none;
    outline: none;
    selection-background-color: #2A3140;
    selection-color: #E0E0E0;
}
QTableWidget::item { padding: 4px 10px; border-bottom: 1px solid #1A2030; background: transparent; }
QTableWidget::item:hover { background-color: #222836; }
QTableWidget::item:selected { background-color: #2A3140; color: #E0E0E0; }
QHeaderView::section {
    background-color: #171C28;
    color: #888888;
    padding: 8px 10px;
    border: none;
    border-bottom: 1px solid #313848;
    font-weight: 600;
    font-size: 10px;
}
QTableCornerButton::section { background-color: #171C28; border: none; }
QSplitter#resultsSplitter::handle:vertical {
    background-color: #171C28;
    border-top: 1px solid #4A566A;
    border-bottom: 1px solid #1A2030;
}
QMenu {
    background-color: #1D2330;
    border: 1px solid #3D4554;
    border-radius: 0;
    padding: 6px 0;
}
QMenu::item { padding: 6px 32px 6px 24px; min-width: 150px; background: transparent; }
QMenu#versionSelectMenu::item { padding: 6px 32px 6px 36px; }
QMenu#versionSelectMenu::icon { left: 10px; }
QMenu::item:selected { background-color: #00F0FF; color: #000000; }
QMenu::item:disabled { color: #555B66; }
QMenu::item:disabled:selected { background-color: transparent; color: #555B66; }
QMenu::separator { height: 1px; background-color: #3D4554; margin: 4px 12px; }
#destructiveMenu::item { color: #FF0055; }
#destructiveMenu::item:selected { background-color: #FF0055; color: #000000; }
#destructiveMenu::item:disabled { color: #802B48; }
#destructiveMenu::item:disabled:selected { background-color: transparent; color: #802B48; }
QToolTip { background-color: #1D2330; color: #E0E0E0; border: 1px solid #3D4554; }
QScrollBar:vertical { background-color: transparent; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background-color: #3A4250; min-height: 20px; border-radius: 0; margin: 2px; }
QScrollBar::handle:vertical:hover { background-color: #4D5668; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }
QScrollBar:horizontal { background-color: transparent; height: 10px; margin: 0; }
QScrollBar::handle:horizontal { background-color: #3A4250; min-width: 20px; border-radius: 0; margin: 2px; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }
#TitleBarWidget {
    background-color: #171C28;
    border-bottom: 1px solid #313848;
}
#titleBarLogo { background-color: transparent; }
#titleBarLabel {
    background-color: transparent;
    color: #E0E0E0;
    font-size: 13px;
    font-weight: 500;
}
#titleBarMin,
#titleBarMax,
#titleBarClose {
    background: transparent;
    border: none;
    border-radius: 0;
    color: #AAB0BA;
    font-weight: 400;
    margin: 0;
    padding: 0;
    min-width: 46px;
    max-width: 46px;
    min-height: 36px;
    max-height: 36px;
}
#titleBarMin:hover,
#titleBarMax:hover {
    background-color: #2A3140;
    color: #FFFFFF;
}
#titleBarMin:pressed,
#titleBarMax:pressed {
    background-color: #222836;
}
#titleBarClose:hover {
    background-color: #E81123;
    color: #FFFFFF;
}
#titleBarClose:pressed {
    background-color: #F1707A;
    color: #FFFFFF;
}
#releaseNotesScroll { background-color: transparent; }
#releaseNotesScroll > QWidget > QWidget { background-color: transparent; }
#currentReleaseVersion {
    color: #FFFFFF;
    font-size: 17px;
    font-weight: 600;
    padding: 4px 0;
}
#releaseNotesSeparator {
    color: #3D4554;
    background-color: #3D4554;
    max-height: 1px;
    margin: 8px 0;
}
#releaseNotesVersion {
    background-color: #171C28;
    border: 1px solid #313848;
    color: #E0E0E0;
    font-size: 13px;
    font-weight: 600;
    padding: 9px 12px;
    text-align: left;
}
#releaseNotesVersion:hover { background-color: #222836; border-color: #465268; }
#releaseNotesVersion:checked { border-color: #536078; }
#releaseNotesChange { color: #C8CDD6; padding-left: 8px; }
#releaseNotesContributors { color: #8F98A8; padding: 6px 0 2px 8px; }
#releaseNotesCategory_added,
#releaseNotesCategory_improved,
#releaseNotesCategory_removed {
    font-size: 12px;
    font-weight: 600;
    margin-top: 5px;
}
#releaseNotesCategory_added { color: #00F0FF; }
#releaseNotesCategory_improved { color: #8BB8FF; }
#releaseNotesCategory_removed { color: #AAB0BA; }
)";

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
    if (!VNH::UserSessionReady::signal(eventName, &error)) {
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
    auto* dialog = qobject_cast<VNH::ConsentDialog*>(QApplication::activeModalWidget());
    if (dialog && dialog->isVisible()) {
        dialog->raise();
        dialog->activateWindow();
        confirmHubReady(arguments);
    }
}

}

int main(int argc, char* argv[])
{
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
    VNH::FileLog::install(logRole);
    QFile::remove(VNH::userDataDir() + QStringLiteral("/auth-token.bin"));
    QFile::remove(VNH::userDataDir() + QStringLiteral("/auth-state.json"));

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
                VNH::ServerEndpoints::resolvedBaseUrl();

            VNH::UpdateChecker checker;
            VNH::UpdateChecker::Options options;
            QList<QUrl> manifestUrls = VNH::ServerEndpoints::manifestUrls(
                serverBaseUrl, product,
                VNH::ServerEndpoints::hostChannel());
            options.latestUrl = manifestUrls.takeFirst();
            options.fallbackUrls = manifestUrls;
            options.trustedHttpOrigin = serverBaseUrl;
            options.product = product;
            options.channel = VNH::ServerEndpoints::hostChannel();
            options.manifestPublicKey = VNH::TrustAnchors::manifestPublicKey();
            int exitCode = 2;
            QObject::connect(
                &checker, &VNH::UpdateChecker::checkFinished, &app,
                [&exitCode, &app, &product](const VNH::UpdateCheckResult& result) {
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
            for (int j = 1; j < argc; ++j) {
                if (qstrcmp(argv[j], "--explicit-install") == 0)
                    explicitInstall = true;
            }
            const auto outcome = VNH::InstallExecutor::run(
                QString::fromLatin1(argv[i + 1]),
                VNH::TrustAnchors::manifestPublicKey(),
                QDateTime::currentDateTimeUtc(), explicitInstall);
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
            const auto outcome = VNH::UninstallExecutor::run(
                QString::fromLatin1(argv[i + 1]),
                QDateTime::currentDateTimeUtc());
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
            const auto outcome = VNH::RepairExecutor::run(
                QString::fromLatin1(argv[i + 1]),
                QDateTime::currentDateTimeUtc());
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
            if (!VNH::LogExporter::exportDiagnostics(
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
                Harken::BuildInfo::HubVersion.data(),
                static_cast<int>(Harken::BuildInfo::HubVersion.size()));
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
            const auto report = VNH::UpdateBridgeConsumer::run(
                VNH::TrustAnchors::manifestPublicKey(),
                QDateTime::currentDateTimeUtc(),
                VNH::UpdateBridgeConsumer::defaultHooks());
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
        if (qstrcmp(argv[i], "--register-update-bridge") == 0) {
            QCoreApplication app(argc, argv);
            QString bridgeExePath = QCoreApplication::applicationFilePath();
#ifdef Q_OS_WIN
            QString deployError;
            const QString deployed = VNH::deployUpdateBridgePayload(
                QCoreApplication::applicationDirPath() +
                    QStringLiteral("/Updater"),
                VNH::machineDataDir() + QStringLiteral("/Updater"),
                QString::fromUtf8(
                    Harken::BuildInfo::BuildId.data(),
                    static_cast<int>(Harken::BuildInfo::BuildId.size())),
                VNH::updaterAclSddl(), &deployError);
            if (deployed.isEmpty()) {
                qCritical().noquote()
                    << "Update bridge deployment failed:" << deployError;
                return 2;
            }
            bridgeExePath = deployed;
#endif
            QString error;
            if (!VNH::registerUpdateBridgeTask(bridgeExePath, &error)) {
                qCritical().noquote()
                    << "Update bridge registration failed:" << error;
                return 2;
            }
#ifdef Q_OS_WIN
            VNH::pruneUpdateBridgePayloads(
                VNH::machineDataDir() + QStringLiteral("/Updater"),
                bridgeExePath);
#endif
            qInfo().noquote() << "Update bridge registered:"
                              << VNH::updateBridgeTaskName() << "->"
                              << QDir::toNativeSeparators(bridgeExePath);
            return 0;
        }
        if (qstrcmp(argv[i], "--unregister-update-bridge") == 0) {
            QCoreApplication app(argc, argv);
            QString error;
            if (!VNH::unregisterUpdateBridgeTask(&error)) {
                qCritical().noquote()
                    << "Update bridge removal failed:" << error;
                return 2;
            }
#ifdef Q_OS_WIN
            QDir(VNH::machineDataDir() + QStringLiteral("/Updater"))
                .removeRecursively();
#endif
            qInfo().noquote() << "Update bridge unregistered";
            return 0;
        }
        if (qstrcmp(argv[i], "--await-health-handshake") == 0) {
            QCoreApplication app(argc, argv);
            const auto report = VNH::SelfUpdateHealthCheck::runInstalledCheck(
                QCoreApplication::applicationFilePath(), 30000);
            if (!report.ok) {
                qCritical().noquote()
                    << "Health handshake failed:" << report.error;
                return 2;
            }
            const QString ownVersion = QString::fromUtf8(
                Harken::BuildInfo::HubVersion.data(),
                static_cast<int>(Harken::BuildInfo::HubVersion.size()));
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
            const auto summaries = VNH::TransactionDiagnostics::summarize();
            const QJsonDocument doc(
                VNH::TransactionDiagnostics::toJson(summaries));
            fputs(doc.toJson(QJsonDocument::Indented).constData(), stdout);
            return 0;
        }
        if (qstrcmp(argv[i], "--background") == 0) {
            QApplication app(argc, argv);
            VNH::HubLocalization::install(&app);
            app.setQuitOnLastWindowClosed(false);

            VNH::SingleInstanceGuard guard(
                VNH::SingleInstanceGuard::backgroundServerName());
            if (!guard.isPrimary()) {
                guard.notifyPrimary(app.arguments());
                return 0;
            }

            const QString currentVersion = QString::fromUtf8(
                Harken::BuildInfo::HubVersion.data(),
                static_cast<int>(Harken::BuildInfo::HubVersion.size()));
            const bool selfUpdateFinalized =
                VNH::HubSelfUpdate::finalizeAfterRestart(currentVersion);
            if (selfUpdateFinalized) {
                VNH::HubSettings::setPendingUpdateNotesVersion(currentVersion);
                qInfo().noquote()
                    << "Hub self-update to" << currentVersion
                    << "completed";
            }
            if (VNH::HubSelfUpdate::recoverStaleElevation(
                    currentVersion, QDateTime::currentDateTimeUtc()))
                qInfo().noquote()
                    << "Recovered a stale hub update elevation request";
            VNH::HubSelfUpdate::reconcileLandingStateAtStartup(
                QDateTime::currentDateTimeUtc());

            const auto reclaim = VNH::TransactionReclaimer::reclaimAtStartup(
                QDateTime::currentDateTimeUtc());
            for (const QString& transactionId : reclaim.reclaimedTransactionIds)
                qInfo().noquote()
                    << "Reclaimed unfinished install transaction"
                    << transactionId;

            VNH::CacheCleanOptions cleanOptions;
            for (const QString& product : {QStringLiteral("HarkenStudio"),
                                           QStringLiteral("HarkenHub")}) {
                const auto update = VNH::UpdateStateStore::load(product);
                if (!update.downloadPath.isEmpty()) {
                    cleanOptions.protectedFiles << update.downloadPath
                        << update.downloadPath + QStringLiteral(".part")
                        << update.downloadPath + QStringLiteral(".part.meta");
                }
                if (!update.manifestEnvelopePath.isEmpty())
                    cleanOptions.protectedFiles << update.manifestEnvelopePath;
            }
            VNH::CacheManager::clean(VNH::userDataDir(), cleanOptions,
                                     QDateTime::currentDateTimeUtc());

            {
                VNH::ProjectStore pinStore;
                if (pinStore.open(VNH::userDataDir() +
                                  QStringLiteral("/projects.db"))) {
                    QString pinError;
                    if (!VNH::BuildPinRegistry::refreshFromProjects(pinStore,
                                                                    &pinError))
                        qWarning().noquote()
                            << "Build pin refresh failed:" << pinError;
                }
            }

            if (VNH::UpdateQueue::hasPendingHubUpdate() &&
                !VNH::SingleInstanceGuard::isServerActive(
                    VNH::SingleInstanceGuard::guiServerName())) {
                QString dispatchError;
                if (!VNH::BridgeDispatcher::dispatch(&dispatchError)) {
                    qWarning().noquote()
                        << "Update bridge dispatch failed:" << dispatchError;
                } else {
                    qInfo().noquote()
                        << "A hub update is queued; the background service "
                           "exits so the update bridge can land it";
                    return 0;
                }
            }

            VNH::UpdateCoordinator coordinator;
            VNH::HubSelfUpdate selfUpdate;
            VNH::UpdateNotifier notifier;
            notifier.attach(&coordinator);
            notifier.attach(&selfUpdate);

            QTimer hubUpdateLandingTimer;
            hubUpdateLandingTimer.setInterval(1000);
            const auto landHubUpdate =
                [&app, &selfUpdate,
                 &hubUpdateLandingTimer](const VNH::UpdateStateRecord& record) {
                    if (record.product != QStringLiteral("HarkenHub") ||
                        record.state != QStringLiteral("AwaitingRestart") ||
                        !VNH::HubSettings::autoSilentlyUpdateHub()) {
                        hubUpdateLandingTimer.stop();
                        return;
                    }
                    if (VNH::SingleInstanceGuard::isServerActive(
                            VNH::SingleInstanceGuard::guiServerName())) {
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
                             &VNH::HubSelfUpdate::stateChanged, &app,
                             landHubUpdate);
            QObject::connect(
                &hubUpdateLandingTimer, &QTimer::timeout, &app,
                [&selfUpdate, &landHubUpdate]() {
                    landHubUpdate(selfUpdate.currentState());
                });
            QObject::connect(
                &guard, &VNH::SingleInstanceGuard::activationRequested, &app,
                [&app, &coordinator, &selfUpdate, &notifier,
                 &landHubUpdate](const QStringList& arguments) {
                    if (arguments.contains(QStringLiteral("--quit"))) {
                        app.quit();
                        return;
                    }
                    if (arguments.contains(
                            QStringLiteral("--settings-changed"))) {
                        VNH::HubLocalization::install(&app);
                        notifier.retranslateTray();
                        coordinator.setAutomaticDownloadEnabled(
                            VNH::HubSettings::autoDownloadStudio());
                        landHubUpdate(selfUpdate.currentState());
                    }
                    if (arguments.contains(QStringLiteral("--check-now"))) {
                        coordinator.checkNow();
                        selfUpdate.checkNow();
                    }
                    confirmHubReady(arguments);
                });

            bool anyStarted = false;

            const QUrl serverBaseUrl =
                VNH::ServerEndpoints::resolvedBaseUrl();

            VNH::UpdateCoordinator::Options options;
            options.serverBaseUrl = serverBaseUrl;
            options.manifestPublicKey =
                VNH::TrustAnchors::manifestPublicKey();
            options.automaticDownloadEnabled =
                VNH::HubSettings::autoDownloadStudio();
            anyStarted = coordinator.start(options) || anyStarted;

            VNH::HubSelfUpdate::Options hubOptions;
            hubOptions.serverBaseUrl = serverBaseUrl;
            hubOptions.manifestPublicKey =
                VNH::TrustAnchors::manifestPublicKey();
            hubOptions.currentVersion = QString::fromUtf8(
                Harken::BuildInfo::HubVersion.data(),
                static_cast<int>(Harken::BuildInfo::HubVersion.size()));
            anyStarted = selfUpdate.start(hubOptions) || anyStarted;

            if (!anyStarted) {
                qInfo().noquote()
                    << "Background mode has no usable update service "
                       "configuration; exiting";
                return 0;
            }

            QFileSystemWatcher handoffWatcher;
            if (QDir(VNH::HandoffArea::rootDir()).exists())
                handoffWatcher.addPath(VNH::HandoffArea::rootDir());
            QObject::connect(
                &handoffWatcher, &QFileSystemWatcher::directoryChanged, &app,
                [&app]() {
                    if (VNH::UpdateQueue::hasPendingHubUpdate()) {
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
    app.setWindowIcon(QIcon(QStringLiteral(":/Resources/icons/HubIcon.png")));
    VNH::HubLocalization::install(&app);
    app.styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion")))
        QApplication::setStyle(fusion);
    app.setOrganizationDomain(QStringLiteral("harkensound.com"));
    app.setApplicationVersion(
        QString::fromUtf8(Harken::BuildInfo::HubVersion.data(),
                          static_cast<int>(Harken::BuildInfo::HubVersion.size())));

    qInfo().noquote() << "Harken Studio Hub"
                      << QString::fromUtf8(Harken::BuildInfo::Label.data(),
                                           static_cast<int>(Harken::BuildInfo::Label.size()));

    VNH::SingleInstanceGuard guard(VNH::SingleInstanceGuard::guiServerName());
    if (!guard.isPrimary()) {
#ifdef Q_OS_WIN
        const qint64 primaryProcessId = guard.primaryProcessId();
        if (primaryProcessId > 0 && primaryProcessId <= MAXDWORD)
            AllowSetForegroundWindow(static_cast<DWORD>(primaryProcessId));
#endif
        if (guard.notifyPrimary(app.arguments()))
            return 0;
        if (!guard.retryPrimary()) {
            QMessageBox::critical(
                nullptr, hubTr("Harken Studio Hub"),
                hubTr("Another Harken Studio Hub instance is running but not "
                      "responding. Please close it and try again."));
            return 1;
        }
    }

    QList<QStringList> pendingActivations;
    const auto startupConnection = QObject::connect(
        &guard, &VNH::SingleInstanceGuard::activationRequested,
        &app, [&pendingActivations](const QStringList& arguments) {
            if (arguments.contains(QStringLiteral("--quit"))) {
                qInfo() << "Hub received a quit request during startup";
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

    app.setApplicationDisplayName(QStringLiteral("Harken Studio Hub"));
    app.setStyleSheet(QString::fromLatin1(kStyleSheet));

    QTimer::singleShot(0, &app, [&app]() {
        confirmConsentReady(app.arguments());
    });
    if (!VNH::ConsentDialog::requestConsent(
            nullptr, VNH::ConsentDialog::hubDocumentId()))
        return 0;

    const bool selfUpdateFinalized =
        VNH::HubSelfUpdate::finalizeAfterRestart(app.applicationVersion());
    if (selfUpdateFinalized) {
        VNH::HubSettings::setPendingUpdateNotesVersion(
            app.applicationVersion());
        qInfo().noquote() << "Hub self-update to"
                          << app.applicationVersion() << "completed";
    }
    const bool updateNotesPending =
        VNH::HubSettings::pendingUpdateNotesVersion() ==
        app.applicationVersion();
    if (VNH::HubSelfUpdate::recoverStaleElevation(
            app.applicationVersion(), QDateTime::currentDateTimeUtc()))
        qInfo().noquote()
            << "Recovered a stale hub update elevation request";
    VNH::HubSelfUpdate::reconcileLandingStateAtStartup(
        QDateTime::currentDateTimeUtc());

    {
        QSettings settings;
        if (settings.value(QStringLiteral("registerBackgroundOnLogin"), true)
                .toBool()) {
            if (!VNH::isBackgroundOnLoginRegistered())
                VNH::registerBackgroundOnLogin();
            QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                    {QStringLiteral("--background")});
        }
    }

#ifdef Q_OS_MACOS
    if (QCoreApplication::applicationDirPath().startsWith(
            QStringLiteral("/Applications/"))) {
        QString bridgeError;
        if (!VNH::updateBridgeTaskAvailable() &&
            !VNH::registerUpdateBridgeTask(
                QCoreApplication::applicationFilePath(), &bridgeError))
            qWarning().noquote()
                << "Update bridge registration failed:" << bridgeError;
    }
#endif

    const auto reclaim =
        VNH::TransactionReclaimer::reclaimAtStartup(QDateTime::currentDateTimeUtc());
    for (const QString& transactionId : reclaim.reclaimedTransactionIds)
        qInfo().noquote() << "Reclaimed unfinished install transaction"
                          << transactionId;
    for (const QString& transactionId : reclaim.journalWriteFailures)
        qWarning().noquote()
            << "Reclaimed transaction could not be journaled" << transactionId;

    VNH::ProjectStore store;
    const QString databasePath =
        VNH::userDataDir() + QStringLiteral("/projects.db");
    if (!store.open(databasePath))
        qWarning().noquote() << "ProjectStore:" << store.lastError();
    else {
        QString pinError;
        if (!VNH::BuildPinRegistry::refreshFromProjects(store, &pinError))
            qWarning().noquote() << "Build pin refresh failed:" << pinError;
    }

    const auto activateHubWindow = [](VNH::HubWindow& window,
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
        VNH::HubWindow window(&store);

        QObject::disconnect(startupConnection);
        QObject::connect(&guard, &VNH::SingleInstanceGuard::activationRequested, &window,
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
                               &VNH::HubWindow::restoreAfterUpdate);
        }
        if (updateNotesPending) {
            QTimer::singleShot(450, &window,
                               &VNH::HubWindow::showReleaseNotesAfterUpdate);
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
