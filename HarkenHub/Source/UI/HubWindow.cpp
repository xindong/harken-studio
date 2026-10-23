#include "UI/HubWindow.h"

#include "App/SingleInstanceGuard.h"
#include "Launch/ProjectOpenFlow.h"
#include "Localization/HubLocalization.h"
#include "Maintenance/BuildPinRegistry.h"
#include "Platform/PlatformPaths.h"
#include "Projects/ProjectIntakeConsumer.h"
#include "Security/TrustAnchors.h"
#include "Settings/HubSettings.h"
#include "Transactions/InstallIndex.h"
#include "Integrations/EngineTargetStore.h"
#include "Integrations/IntegrationStore.h"
#include "UI/ConsentDialog.h"
#include "UI/EngineIntegrationsPage.h"
#include "UI/HubIcons.h"
#include "UI/LogConsoleDialog.h"
#include "UI/ButtonPulseAnimator.h"
#include "UI/ProjectsPage.h"
#include "UI/ReleaseNotesDialog.h"
#include "UI/TitleBar.h"
#include "UI/VersionsPage.h"
#include "Update/BridgeDispatcher.h"
#include "Update/HubSelfUpdate.h"
#include "Update/ServerEndpoints.h"
#include "Update/UpdateChecker.h"
#include "Update/UpdateQueue.h"
#include "Update/UpdateStateStore.h"
#include "Versions/VersionScanner.h"
#include "Versions/LocalBuildRefreshCoordinator.h"

#include <QApplication>
#include <QActionGroup>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QCloseEvent>
#include <QMoveEvent>
#include <QProcess>
#include <QPixmap>
#include <QPushButton>
#include <QGuiApplication>
#include <QScreen>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#if defined(HARKEN_HAS_QWINDOWKIT) && defined(Q_OS_WIN)
#include <QWKWidgets/widgetwindowagent.h>
#endif

namespace VNH {

namespace {

QString hubUpdateButtonStyle()
{
    return QStringLiteral(
        "QToolButton#hubUpdateButton { background-color: #00F0FF; "
        "border: 1px solid #00F0FF; color: #000000; font-weight: 600; "
        "padding: 4px 12px; }");
}

bool isStudioBuildInstalled(const QString& buildId)
{
    const InstallIndexSnapshot index = InstallIndex::snapshot();
    if (!index.ok)
        return false;
    for (const InstalledVersionRecord& record : index.records) {
        if (record.buildId == buildId)
            return true;
    }
    return false;
}

}

HubWindow::HubWindow(ProjectStore* store, QWidget* parent)
    : QMainWindow(parent)
    , store_(store)
{
    setWindowTitle(tr("Harken Studio Hub"));
    resize(1280, 720);

    sidebar_ = new QListWidget(this);
    sidebar_->setObjectName(QStringLiteral("sidebar"));
    sidebar_->setFocusPolicy(Qt::NoFocus);
    sidebar_->addItem(tr("Projects"));
    sidebar_->addItem(tr("Versions"));
    sidebar_->addItem(tr("Engine Integrations"));
    sidebar_->setCurrentRow(0);

    settingsButton_ = new QToolButton(this);
    settingsButton_->setObjectName(QStringLiteral("settingsButton"));
    settingsButton_->setIconSize({20, 20});
    settingsButton_->setIcon(HubIcons::settingsIcon(devicePixelRatioF()));
    settingsButton_->setFixedSize(32, 32);
    settingsButton_->setToolTip(tr("Settings"));
    settingsButton_->setPopupMode(QToolButton::InstantPopup);

    auto* settingsMenu = new QMenu(settingsButton_);
    settingsMenu->setToolTipsVisible(true);
    QMenu* languageMenu = settingsMenu->addMenu(tr("Language"));
    auto* languageGroup = new QActionGroup(languageMenu);
    languageGroup->setExclusive(true);
    const auto addLanguage = [languageMenu, languageGroup](
                                 const QString& name, const QString& value) {
        QAction* action = languageMenu->addAction(name);
        action->setCheckable(true);
        action->setData(value);
        languageGroup->addAction(action);
        return action;
    };
    QAction* systemLanguageAction = addLanguage(
        tr("System Default"), HubLocalization::systemPreference());
    languageMenu->addSeparator();
    addLanguage(QStringLiteral("English"),
                HubLocalization::englishPreference());
    addLanguage(QStringLiteral("\u7b80\u4f53\u4e2d\u6587"),
                HubLocalization::simplifiedChinesePreference());
    const QString languagePreference = HubSettings::languagePreference();
    for (QAction* action : languageGroup->actions()) {
        action->setChecked(action->data().toString() == languagePreference);
    }
    if (!languageGroup->checkedAction())
        systemLanguageAction->setChecked(true);
    connect(languageGroup, &QActionGroup::triggered, this,
            [this, languageGroup](QAction* action) {
                const QString previous = HubSettings::languagePreference();
                const QString selected = action->data().toString();
                if (selected == previous)
                    return;
                if (!HubSettings::setLanguagePreference(selected)) {
                    for (QAction* candidate : languageGroup->actions()) {
                        candidate->setChecked(
                            candidate->data().toString() == previous);
                    }
                    QMessageBox::warning(
                        this, tr("Harken Studio Hub"),
                        tr("The language setting could not be saved."));
                    return;
                }
                SingleInstanceGuard::sendTo(
                    SingleInstanceGuard::backgroundServerName(),
                    {QStringLiteral("--settings-changed")});
                QMessageBox::information(
                    this, tr("Harken Studio Hub"),
                    tr("The language change will take effect after restarting "
                       "Harken Studio Hub."));
            });
    settingsMenu->addSeparator();
    QMenu* autoHubUpdateMenu =
        settingsMenu->addMenu(tr("Automatically Check and Silently Update Hub"));
    auto* autoHubUpdateGroup = new QActionGroup(autoHubUpdateMenu);
    autoHubUpdateGroup->setExclusive(true);
    autoHubUpdateYesAction_ = autoHubUpdateMenu->addAction(tr("Yes"));
    autoHubUpdateNoAction_ = autoHubUpdateMenu->addAction(tr("No"));
    autoHubUpdateYesAction_->setCheckable(true);
    autoHubUpdateNoAction_->setCheckable(true);
    autoHubUpdateGroup->addAction(autoHubUpdateYesAction_);
    autoHubUpdateGroup->addAction(autoHubUpdateNoAction_);
    const bool autoSilentlyUpdateHub =
        HubSettings::autoSilentlyUpdateHub();
    autoHubUpdateYesAction_->setChecked(autoSilentlyUpdateHub);
    autoHubUpdateNoAction_->setChecked(!autoSilentlyUpdateHub);
    connect(autoHubUpdateYesAction_, &QAction::triggered, this,
            [this](bool checked) {
                if (checked)
                    applyAutoSilentlyUpdateHubPreference(true);
            });
    connect(autoHubUpdateNoAction_, &QAction::triggered, this,
            [this](bool checked) {
                if (checked)
                    applyAutoSilentlyUpdateHubPreference(false);
            });
    QMenu* autoDownloadMenu =
        settingsMenu->addMenu(tr("Automatically Check and Download the Latest Studio"));
    auto* autoDownloadGroup = new QActionGroup(autoDownloadMenu);
    autoDownloadGroup->setExclusive(true);
    autoDownloadYesAction_ =
        autoDownloadMenu->addAction(tr("Yes"));
    autoDownloadNoAction_ =
        autoDownloadMenu->addAction(tr("No"));
    autoDownloadYesAction_->setCheckable(true);
    autoDownloadNoAction_->setCheckable(true);
    autoDownloadGroup->addAction(autoDownloadYesAction_);
    autoDownloadGroup->addAction(autoDownloadNoAction_);
    const bool autoDownloadStudio = HubSettings::autoDownloadStudio();
    autoDownloadYesAction_->setChecked(autoDownloadStudio);
    autoDownloadNoAction_->setChecked(!autoDownloadStudio);
    connect(autoDownloadYesAction_, &QAction::triggered, this,
            [this](bool checked) {
                if (checked)
                    applyAutoDownloadStudioPreference(true);
            });
    connect(autoDownloadNoAction_, &QAction::triggered, this,
            [this](bool checked) {
                if (checked)
                    applyAutoDownloadStudioPreference(false);
            });
    settingsMenu->addSeparator();
    checkUpdatesAction_ = settingsMenu->addAction(
        tr("Manually Check for Hub/Studio Updates"));
    connect(checkUpdatesAction_, &QAction::triggered, this,
            &HubWindow::requestUpdateCheck);
    settingsMenu->addSeparator();
    QAction* logAction = settingsMenu->addAction(tr("Activity Log"));
    connect(logAction, &QAction::triggered, this,
            &HubWindow::showLogConsole);
    settingsMenu->addSeparator();
    QAction* legalDocumentsAction = settingsMenu->addAction(
        tr("License Agreement && Privacy Policy"));
    connect(legalDocumentsAction, &QAction::triggered, this,
            [this]() { ConsentDialog::showDocuments(this); });
    QAction* releaseNotesAction = settingsMenu->addAction(tr("About Hub"));
    connect(releaseNotesAction, &QAction::triggered, this,
            &HubWindow::showReleaseNotes);
    settingsButton_->setMenu(settingsMenu);

    hubUpdateButton_ = new QToolButton(this);
    hubUpdateButton_->setObjectName(QStringLiteral("hubUpdateButton"));
    hubUpdateButton_->setText(tr("Update"));
    hubUpdateButton_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    hubUpdateButton_->setFixedHeight(30);
    hubUpdateButton_->setToolTip(
        tr("A Harken Studio Hub update is ready. Click to close the hub, install it, "
           "and restart."));
    hubUpdateButton_->hide();
    connect(hubUpdateButton_, &QToolButton::clicked, this,
            &HubWindow::beginHubUpdateLanding);
    hubUpdatePulse_ = new ButtonPulseAnimator(this);

    auto* sidebarPanel = new QWidget(this);
    sidebarPanel->setObjectName(QStringLiteral("sidebarPanel"));
    sidebarPanel->setFixedWidth(196);
    auto* sidebarLayout = new QVBoxLayout(sidebarPanel);
    sidebarLayout->setContentsMargins(0, 0, 0, 14);
    auto* productLogo = new QLabel(sidebarPanel);
    productLogo->setObjectName(QStringLiteral("sidebarLogo"));
    productLogo->setAccessibleName(QStringLiteral("Harken Studio"));
    const qreal logoScale = devicePixelRatioF();
    QPixmap logo(QStringLiteral(":/Resources/branding/HarkenStudioBlueGradient.png"));
    logo = logo.scaledToWidth(qRound(156 * logoScale), Qt::SmoothTransformation);
    logo.setDevicePixelRatio(logoScale);
    productLogo->setPixmap(logo);
    auto* productLabel = new QLabel(tr("Hub"), sidebarPanel);
    productLabel->setObjectName(QStringLiteral("sidebarProduct"));
    auto* productColumn = new QVBoxLayout();
    productColumn->setSpacing(8);
    productColumn->addWidget(productLogo, 0, Qt::AlignLeft);
    productColumn->addWidget(productLabel, 0, Qt::AlignLeft);
    auto* productRow = new QHBoxLayout();
    productRow->setContentsMargins(26, 20, 14, 16);
    productRow->addLayout(productColumn);
    productRow->addStretch();
    sidebarLayout->addLayout(productRow);
    sidebarLayout->addWidget(sidebar_);
    auto* sidebarNote = new QLabel(
        tr("This early build includes only a small set of features for "
           "evaluation. More features will be added gradually with each "
           "release during testing. If you run into any problems, please "
           "open an issue in the repository, or feel free to contact us "
           "directly."),
        this);
    sidebarNote->setObjectName(QStringLiteral("sidebarNote"));
    sidebarNote->setWordWrap(true);
    auto* noteRow = new QHBoxLayout();
    noteRow->setContentsMargins(14, 0, 14, 10);
    noteRow->addWidget(sidebarNote);
    sidebarLayout->addLayout(noteRow);
    auto* sidebarLinks = new QLabel(
        tr(
            "<a href=\"https://harken.xd.com\" "
            "style=\"color:#00F0FF;\">Harken Website</a><br/>"
            "<a href=\"https://github.com/xindong/harken-studio\" "
            "style=\"color:#00F0FF;\">Harken Studio Repository</a>"),
        this);
    sidebarLinks->setObjectName(QStringLiteral("sidebarLinks"));
    sidebarLinks->setOpenExternalLinks(true);
    sidebarLinks->setTextInteractionFlags(Qt::TextBrowserInteraction);
    auto* linksRow = new QHBoxLayout();
    linksRow->setContentsMargins(14, 0, 14, 10);
    linksRow->addWidget(sidebarLinks);
    sidebarLayout->addLayout(linksRow);
    auto* bottomRow = new QHBoxLayout();
    bottomRow->setContentsMargins(14, 0, 14, 0);
    bottomRow->addWidget(settingsButton_);
    bottomRow->addSpacing(8);
    bottomRow->addWidget(hubUpdateButton_);
    bottomRow->addStretch();
    sidebarLayout->addLayout(bottomRow);

    updateStatePollTimer_ = new QTimer(this);
    updateStatePollTimer_->setInterval(3000);
    connect(updateStatePollTimer_, &QTimer::timeout, this,
            &HubWindow::refreshHubUpdateButton);
    updateStatePollTimer_->start();

    projectsPage_ = new ProjectsPage(store, this);
    versionsPage_ = new VersionsPage(store, this);
    versionsPage_->setAutoDownloadStudio(autoDownloadStudio);
    projectsPage_->setVersionsProvider(
        []() { return VersionScanner::scanAvailable(); });

    connect(projectsPage_, &ProjectsPage::projectsChanged, this, [this]() {
        QString pinError;
        if (store_ &&
            !BuildPinRegistry::refreshFromProjects(*store_, &pinError))
            qWarning().noquote() << "Build pin refresh failed:" << pinError;
    });
    connect(projectsPage_, &ProjectsPage::projectsChanged, versionsPage_,
            &VersionsPage::refresh);
    connect(versionsPage_, &VersionsPage::launchTargetChanged, projectsPage_,
            &ProjectsPage::refreshVersions);
    connect(versionsPage_, &VersionsPage::projectLaunchGuidanceRequested, this,
            [this](const QString& buildId) {
                showProjectsPage();
                projectsPage_->showProjectLaunchGuidance(buildId);
            },
            Qt::QueuedConnection);

    localBuildRefresh_ = new LocalBuildRefreshCoordinator(this);
    connect(localBuildRefresh_,
            &LocalBuildRefreshCoordinator::buildsChanged, versionsPage_,
            &VersionsPage::refresh);
    connect(localBuildRefresh_,
            &LocalBuildRefreshCoordinator::buildsChanged, projectsPage_,
            &ProjectsPage::refreshVersions);
    connect(localBuildRefresh_, &LocalBuildRefreshCoordinator::buildReady,
            versionsPage_, &VersionsPage::highlightVersion);
    connect(localBuildRefresh_,
            &LocalBuildRefreshCoordinator::buildRegistrationFailed, this,
            [](const QString& executablePath, const QString& error) {
                qWarning().noquote()
                    << "Local build registration failed for"
                    << executablePath << ':' << error;
            });
    localBuildRefresh_->start();

    intakeConsumer_ = new ProjectIntakeConsumer(store, this);
    connect(intakeConsumer_, &ProjectIntakeConsumer::projectsUpdated,
            projectsPage_, &ProjectsPage::refresh);
    intakeConsumer_->start();

    engineTargetStore_ = new EngineTargetStore();
    const QString engineTargetDatabase = userDataDir()
        + QStringLiteral("/engine-targets.db");
    if (!engineTargetStore_->open(engineTargetDatabase))
        qWarning().noquote() << "EngineTargetStore:"
                             << engineTargetStore_->lastError();
    integrationStore_ = new IntegrationStore();
    const QString integrationDatabase = userDataDir()
        + QStringLiteral("/integrations.db");
    if (!integrationStore_->open(integrationDatabase))
        qWarning().noquote() << "IntegrationStore:"
                             << integrationStore_->lastError();
    engineIntegrationsPage_ = new EngineIntegrationsPage(
        integrationStore_, engineTargetStore_, store_, this);
    connect(versionsPage_, &VersionsPage::versionsRefreshed,
            engineIntegrationsPage_,
            &EngineIntegrationsPage::refreshStudioVersion);
    connect(versionsPage_, &VersionsPage::launchTargetChanged,
            engineIntegrationsPage_,
            &EngineIntegrationsPage::refreshStudioVersion);
    connect(engineIntegrationsPage_,
            &EngineIntegrationsPage::projectsChanged,
            projectsPage_, &ProjectsPage::refresh);

    pages_ = new QStackedWidget(this);
    pages_->addWidget(projectsPage_);
    pages_->addWidget(versionsPage_);
    pages_->addWidget(engineIntegrationsPage_);

    connect(sidebar_, &QListWidget::currentRowChanged, this,
            [this](int index) {
                pages_->setCurrentIndex(index);
                if (index == pages_->indexOf(engineIntegrationsPage_))
                    engineIntegrationsPage_->showList();
            });

    auto* central = new QWidget(this);
    auto* layout = new QHBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(sidebarPanel);
    auto* sidebarSeparator = new QFrame(central);
    sidebarSeparator->setObjectName(QStringLiteral("sidebarSeparator"));
    sidebarSeparator->setFrameShape(QFrame::NoFrame);
    sidebarSeparator->setFixedWidth(1);
    layout->addWidget(sidebarSeparator);
    layout->addWidget(pages_, 1);
    setCentralWidget(central);

    restoreWindowGeometry();

    windowSizeSaveTimer_ = new QTimer(this);
    windowSizeSaveTimer_->setSingleShot(true);
    windowSizeSaveTimer_->setInterval(200);
    connect(windowSizeSaveTimer_, &QTimer::timeout, this,
            &HubWindow::persistWindowGeometry);

    installChrome();
    refreshHubUpdateButton();
    QTimer::singleShot(0, this, [this]() {
        clampToVisibleScreen();
        if (QGuiApplication::primaryScreen())
            persistWindowGeometry();
        beginStartupFlow();
    });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this](QScreen*) {
        clampToVisibleScreen();
        persistWindowGeometry();
    });
}

HubWindow::~HubWindow()
{
    flushPendingWindowGeometry();
    delete integrationStore_;
    delete engineTargetStore_;
}

void HubWindow::beginStartupFlow()
{
    if (!HubSettings::autoDownloadStudio() ||
        !VersionScanner::scanAvailable().empty())
        return;
    kickBackgroundCheck(false);
    versionsPage_->showServerCheckPending();
    sidebar_->setCurrentRow(1);
}

void HubWindow::requestUpdateCheck()
{
    if (manualHubUpdateChecker_ || manualStudioUpdateChecker_)
        return;

    checkUpdatesAction_->setEnabled(false);
    manualUpdateChecksPending_ = 0;
    manualHubUpdateAvailable_ = false;
    manualStudioUpdateAvailable_ = false;
    manualUpdateCheckErrors_.clear();
    const QUrl serverBaseUrl = ServerEndpoints::resolvedBaseUrl();
    const auto startCheck = [this, &serverBaseUrl](bool hubProduct) {
        auto* checker = new UpdateChecker(this);
        if (hubProduct)
            manualHubUpdateChecker_ = checker;
        else
            manualStudioUpdateChecker_ = checker;
        ++manualUpdateChecksPending_;
        connect(checker, &UpdateChecker::checkFinished, this,
                [this, checker, hubProduct](const UpdateCheckResult& result) {
                    handleManualUpdateCheckResult(checker, hubProduct,
                                                  result);
                });

        UpdateChecker::Options options;
        QList<QUrl> manifestUrls = ServerEndpoints::manifestUrls(
            serverBaseUrl,
            hubProduct ? QStringLiteral("hub") : QStringLiteral("harken"),
            ServerEndpoints::hostChannel());
        options.latestUrl = manifestUrls.takeFirst();
        options.fallbackUrls = manifestUrls;
        options.trustedHttpOrigin = serverBaseUrl;
        options.product = hubProduct ? QStringLiteral("hub")
                                     : QStringLiteral("harken");
        options.channel = ServerEndpoints::hostChannel();
        options.manifestPublicKey = TrustAnchors::manifestPublicKey();
        if (checker->start(options))
            return;

        if (hubProduct)
            manualHubUpdateChecker_ = nullptr;
        else
            manualStudioUpdateChecker_ = nullptr;
        checker->deleteLater();
        --manualUpdateChecksPending_;
        manualUpdateCheckErrors_.append(
            QStringLiteral("%1: %2")
                .arg(hubProduct ? QStringLiteral("Harken Studio Hub")
                                : QStringLiteral("HarkenStudio"),
                     tr("The update service is unavailable.")));
    };

    startCheck(true);
    startCheck(false);

    kickBackgroundCheck();
    finishManualUpdateCheck();
}

void HubWindow::handleManualUpdateCheckResult(
    UpdateChecker* checker, bool hubProduct,
    const UpdateCheckResult& result)
{
    UpdateChecker*& activeChecker = hubProduct
                                        ? manualHubUpdateChecker_
                                        : manualStudioUpdateChecker_;
    if (activeChecker != checker)
        return;
    activeChecker = nullptr;
    checker->stop();
    checker->deleteLater();
    --manualUpdateChecksPending_;

    const QString productName = hubProduct ? QStringLiteral("Harken Studio Hub")
                                           : QStringLiteral("HarkenStudio");
    const QString expectedProduct = hubProduct
                                        ? QStringLiteral("HarkenHub")
                                        : QStringLiteral("HarkenStudio");
    if (!result.ok) {
        manualUpdateCheckErrors_.append(
            QStringLiteral("%1: %2").arg(productName, result.error));
    } else if (!result.manifestAbsent && !result.notModified &&
               result.manifest.product != expectedProduct) {
        manualUpdateCheckErrors_.append(
            tr("The server returned an invalid %1 update manifest.")
                .arg(productName));
    } else if (!result.manifestAbsent && !result.notModified) {
        if (hubProduct) {
            manualHubUpdateAvailable_ = HubSelfUpdate::isNewerVersion(
                result.manifest.version,
                QCoreApplication::applicationVersion());
        } else {
            manualStudioUpdateAvailable_ =
                !isStudioBuildInstalled(result.manifest.buildId);
            if (manualStudioUpdateAvailable_) {
                showVersionsPage();
                if (!HubSettings::autoDownloadStudio())
                    versionsPage_->highlightServerUpdate();
            }
        }
    }

    finishManualUpdateCheck();
}

void HubWindow::finishManualUpdateCheck()
{
    if (manualUpdateChecksPending_ > 0)
        return;
    checkUpdatesAction_->setEnabled(true);
    if (!manualUpdateCheckErrors_.isEmpty()) {
        QMessageBox::warning(
            this, tr("Harken Studio Hub"),
            tr("The update check could not be completed.\n\n%1")
                .arg(manualUpdateCheckErrors_.join(QLatin1Char('\n'))));
        return;
    }
    if (!manualHubUpdateAvailable_ && !manualStudioUpdateAvailable_) {
        QMessageBox::information(
            this, tr("Harken Studio Hub"),
            tr("The current Harken Studio Hub is up to date, and the latest "
               "HarkenStudio is already in the Versions list."));
    }
}

void HubWindow::kickBackgroundCheck(bool manual)
{
    const QString activation = manual ? QStringLiteral("--check-now")
                                      : QStringLiteral("--check-studio");
    if (!SingleInstanceGuard::sendTo(
            SingleInstanceGuard::backgroundServerName(),
            {activation}))
        QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                {QStringLiteral("--background"),
                                 activation});
}

void HubWindow::showLogConsole()
{
    if (!logConsole_)
        logConsole_ = new LogConsoleDialog(this);
    logConsole_->show();
    logConsole_->raise();
    logConsole_->activateWindow();
}

bool HubWindow::showReleaseNotes()
{
    if (!releaseNotes_)
        releaseNotes_ = new ReleaseNotesDialog(this);
    releaseNotes_->show();
    releaseNotes_->raise();
    releaseNotes_->activateWindow();
    return releaseNotes_->hasLoadedCatalog();
}

void HubWindow::showReleaseNotesAfterUpdate()
{
    if (showReleaseNotes())
        HubSettings::clearPendingUpdateNotesVersion(
            QCoreApplication::applicationVersion());
}

void HubWindow::restoreAfterUpdate()
{
    setWindowState(windowState() & ~Qt::WindowMinimized);
    clampToVisibleScreen();
    show();
    raise();
    activateWindow();
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        const HWND handle = reinterpret_cast<HWND>(winId());
        const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
        SetWindowPos(handle, HWND_TOPMOST, 0, 0, 0, 0, flags);
        SetWindowPos(handle, HWND_NOTOPMOST, 0, 0, 0, 0, flags | SWP_SHOWWINDOW);
        SetForegroundWindow(handle);
    }
#endif
}

void HubWindow::applyAutoDownloadStudioPreference(bool enabled)
{
    if (!HubSettings::setAutoDownloadStudio(enabled)) {
        const bool stored = HubSettings::autoDownloadStudio();
        autoDownloadYesAction_->setChecked(stored);
        autoDownloadNoAction_->setChecked(!stored);
        QMessageBox::warning(
            this, tr("Harken Studio Hub"),
            tr("The automatic download setting could not be saved."));
        return;
    }

    autoDownloadYesAction_->setChecked(enabled);
    autoDownloadNoAction_->setChecked(!enabled);
    versionsPage_->setAutoDownloadStudio(enabled);

    const bool notified = SingleInstanceGuard::sendTo(
        SingleInstanceGuard::backgroundServerName(),
        {QStringLiteral("--settings-changed")});
    if (enabled && !notified)
        QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                {QStringLiteral("--background")});
}

void HubWindow::applyAutoSilentlyUpdateHubPreference(bool enabled)
{
    if (!HubSettings::setAutoSilentlyUpdateHub(enabled)) {
        const bool stored = HubSettings::autoSilentlyUpdateHub();
        autoHubUpdateYesAction_->setChecked(stored);
        autoHubUpdateNoAction_->setChecked(!stored);
        QMessageBox::warning(
            this, tr("Harken Studio Hub"),
            tr("The automatic Hub update setting could not be saved."));
        return;
    }

    autoHubUpdateYesAction_->setChecked(enabled);
    autoHubUpdateNoAction_->setChecked(!enabled);

    const bool notified = SingleInstanceGuard::sendTo(
        SingleInstanceGuard::backgroundServerName(),
        {QStringLiteral("--settings-changed")});
    if (enabled && !notified)
        QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                {QStringLiteral("--background")});
}

void HubWindow::refreshHubUpdateButton()
{
    const UpdateStateRecord record =
        UpdateStateStore::load(QStringLiteral("HarkenHub"));
    const bool ownUpdateReady =
        record.found && record.state == QStringLiteral("AwaitingRestart");
    const bool machineUpdateQueued = UpdateQueue::hasPendingHubUpdate();
    const bool visible = ownUpdateReady || machineUpdateQueued;
    if (visible == hubUpdateButton_->isVisible())
        return;
    hubUpdateButton_->setVisible(visible);
    if (!visible) {
        hubUpdatePulse_->stop();
        hubUpdateButton_->setStyleSheet(QString());
        return;
    }
    hubUpdateButton_->setStyleSheet(hubUpdateButtonStyle());
    hubUpdatePulse_->start(hubUpdateButton_);
}

void HubWindow::beginHubUpdateLanding()
{
    QMessageBox confirm(
        QMessageBox::Question, tr("Harken Studio Hub"),
        tr(
            "Harken Studio Hub will close now, update in the background, and "
            "restart automatically. This usually takes a few seconds.\n\n"
            "If Harken Studio Hub does not come back within 2 minutes, close any "
            "remaining Harken Studio Hub processes and start it manually.\n\n"
            "Development builds can be unstable - if the wait drags on, "
            "download the latest installer from the Harken website and "
            "install it over the current version.\n\n"
            "Continue?"),
        QMessageBox::Cancel, this);
    QPushButton* proceed = confirm.addButton(
        tr("Update and Restart"), QMessageBox::AcceptRole);
    confirm.exec();
    if (confirm.clickedButton() != proceed)
        return;

    const UpdateStateRecord record =
        UpdateStateStore::load(QStringLiteral("HarkenHub"));
    if (record.found && record.state == QStringLiteral("AwaitingRestart")) {
        HubSelfUpdate selfUpdate;
        QString error;
        if (!selfUpdate.approveViaBridge(QStringLiteral("gui"), &error,
                                         QDateTime::currentDateTimeUtc())) {
            QMessageBox::warning(
                this, tr("Harken Studio Hub"),
                tr("The Hub update could not be started.\n\n%1").arg(error));
            return;
        }
    } else if (UpdateQueue::hasPendingHubUpdate()) {
        QString dispatchError;
        if (!BridgeDispatcher::dispatch(&dispatchError)) {
            QMessageBox::warning(
                this, tr("Harken Studio Hub"),
                tr("The Hub update could not be started.\n\n%1").arg(dispatchError));
            return;
        }
    } else {
        refreshHubUpdateButton();
        return;
    }
    SingleInstanceGuard::sendTo(SingleInstanceGuard::backgroundServerName(),
                                {QStringLiteral("--quit")});
    QApplication::quit();
}

void HubWindow::installChrome()
{
#if defined(HARKEN_HAS_QWINDOWKIT) && defined(Q_OS_WIN)
    titleBar_ = new TitleBar(this);

    auto* agent = new QWK::WidgetWindowAgent(this);
    agent->setup(this);
    agent->setTitleBar(titleBar_);
    agent->setSystemButton(QWK::WindowAgentBase::Minimize, titleBar_->minimizeButton());
    agent->setSystemButton(QWK::WindowAgentBase::Maximize, titleBar_->maximizeButton());
    agent->setSystemButton(QWK::WindowAgentBase::Close, titleBar_->closeButton());

    connect(titleBar_->minimizeButton(), &QPushButton::clicked, this,
            &QWidget::showMinimized);
    connect(titleBar_->maximizeButton(), &QPushButton::clicked, this, [this]() {
        if (isMaximized())
            showNormal();
        else
            showMaximized();
    });
    connect(titleBar_->closeButton(), &QPushButton::clicked, this, &QWidget::close);

    if (centralWidget() && centralWidget()->layout()) {
        QLayout* central = centralWidget()->layout();
        const QMargins margins = central->contentsMargins();
        central->setContentsMargins(margins.left(),
                                    margins.top() + titleBar_->height(),
                                    margins.right(), margins.bottom());
    }
    titleBar_->setGeometry(0, 0, width(), titleBar_->height());
    titleBar_->raise();
#endif
}

bool HubWindow::event(QEvent* event)
{
    if (windowSizeSaveTimer_ && windowSizeSaveTimer_->isActive() &&
        (event->type() == QEvent::MouseButtonRelease ||
         event->type() == QEvent::NonClientAreaMouseButtonRelease)) {
        windowSizeSaveTimer_->stop();
        persistWindowGeometry();
    }
    return QMainWindow::event(event);
}

void HubWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    if (titleBar_) {
        titleBar_->setGeometry(0, 0, width(), titleBar_->height());
        titleBar_->raise();
    }
    if (windowSizeSaveTimer_ && !isMaximized() && !isMinimized() &&
        !isFullScreen())
        windowSizeSaveTimer_->start();
}

void HubWindow::moveEvent(QMoveEvent* event)
{
    QMainWindow::moveEvent(event);
    if (windowSizeSaveTimer_ && !isMaximized() && !isMinimized() &&
        !isFullScreen())
        windowSizeSaveTimer_->start();
}

void HubWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange && titleBar_)
        titleBar_->setMaximized(isMaximized());
    if (event->type() != QEvent::WindowStateChange)
        return;
    if (isMaximized() || isMinimized() || isFullScreen()) {
        if (windowSizeSaveTimer_ && windowSizeSaveTimer_->isActive()) {
            windowSizeSaveTimer_->stop();
            persistNormalGeometry(normalGeometry());
        } else if (windowSizeSaveTimer_) {
            windowSizeSaveTimer_->stop();
        }
    }
}

void HubWindow::closeEvent(QCloseEvent* event)
{
    flushPendingWindowGeometry();
    QMainWindow::closeEvent(event);
}

void HubWindow::flushPendingWindowGeometry()
{
    if (windowSizeSaveTimer_ && windowSizeSaveTimer_->isActive())
        windowSizeSaveTimer_->stop();
    if (isMaximized() || isFullScreen())
        persistNormalGeometry(normalGeometry());
    else if (!isMinimized())
        persistWindowGeometry();
}

void HubWindow::persistWindowGeometry()
{
    if (isMaximized() || isMinimized() || isFullScreen())
        return;
    persistNormalGeometry(QRect(pos(), size()));
}

void HubWindow::persistNormalGeometry(const QRect& geometry)
{
    if (!geometry.isValid())
        return;
    HubSettings::setWindowSize(geometry.size());
    HubSettings::setWindowPosition(geometry.topLeft());
}

void HubWindow::restoreWindowGeometry()
{
    const QSize savedWindowSize = HubSettings::windowSize();
    if (savedWindowSize.isValid())
        resize(savedWindowSize.expandedTo(minimumSizeHint()));

    if (const std::optional<QPoint> savedPosition = HubSettings::windowPosition())
        move(*savedPosition);
    clampToVisibleScreen();
}

QScreen* HubWindow::visibleScreenFor(const QRect& frame) const
{
    if (QScreen* screen = QGuiApplication::screenAt(frame.center()))
        return screen;
    if (QScreen* screen = QGuiApplication::screenAt(frame.topLeft()))
        return screen;
    if (QScreen* screen = QGuiApplication::screenAt(frame.topRight()))
        return screen;
    if (QScreen* screen = QGuiApplication::screenAt(frame.bottomLeft()))
        return screen;
    if (QScreen* screen = QGuiApplication::screenAt(frame.bottomRight()))
        return screen;
    QScreen* best = nullptr;
    int bestArea = 0;
    for (QScreen* screen : QGuiApplication::screens()) {
        const int area = (screen->availableGeometry() & frame).width() *
                         (screen->availableGeometry() & frame).height();
        if (area > bestArea) {
            bestArea = area;
            best = screen;
        }
    }
    return best ? best : QApplication::primaryScreen();
}

void HubWindow::clampToVisibleScreen()
{
    const QRect frame = frameGeometry();
    QScreen* screen = visibleScreenFor(frame);
    if (!screen)
        return;

    const QRect available = screen->availableGeometry();
    const QSize frameMargins = frame.size() - size();
    QRect placed = frame;
    if (placed.width() > available.width())
        placed.setWidth(available.width());
    if (placed.height() > available.height())
        placed.setHeight(available.height());
    if (placed.left() < available.left())
        placed.moveLeft(available.left());
    if (placed.top() < available.top())
        placed.moveTop(available.top());
    if (placed.right() > available.right())
        placed.moveRight(available.right());
    if (placed.bottom() > available.bottom())
        placed.moveBottom(available.bottom());
    if (placed.topLeft() != frame.topLeft())
        move(placed.topLeft());
    const QSize clientSize(qMax(1, placed.width() - frameMargins.width()),
                           qMax(1, placed.height() - frameMargins.height()));
    if (clientSize != size())
        resize(clientSize);
}

void HubWindow::openProjectFromPath(const QString& path)
{
    QString canonicalPath;
    QString error;
    if (!ProjectOpenFlow::ensureRegistered(store_, path, &canonicalPath,
                                           &error)) {
        QMessageBox::warning(
            this, tr("Open Project"),
            tr("The project could not be opened.\n\n%1").arg(error));
        return;
    }
    sidebar_->setCurrentRow(0);
    projectsPage_->refresh();
    projectsPage_->revealProject(canonicalPath);
}

void HubWindow::registerLocalBuild(const QString& executablePath)
{
    if (!executablePath.isEmpty())
        localBuildRefresh_->registerBuild(executablePath);
}

void HubWindow::revealLocalBuild(const QString& executablePath)
{
    if (executablePath.isEmpty())
        return;
    showVersionsPage();
    localBuildRefresh_->registerBuild(executablePath);
}

void HubWindow::showVersionsPage()
{
    sidebar_->setCurrentRow(1);
}

void HubWindow::showEngineIntegrationsPage(
    const QString& harkenProjectPath, const QString& studioExecutablePath,
    const QString& engineProjectPath)
{
    sidebar_->setCurrentRow(2);
    engineIntegrationsPage_->beginEditForProject(
        harkenProjectPath, studioExecutablePath, engineProjectPath);
}

void HubWindow::showProjectsPage()
{
    sidebar_->setCurrentRow(0);
    projectsPage_->showProjectsList();
}

void HubWindow::showNewProjectPage()
{
    sidebar_->setCurrentRow(0);
    projectsPage_->showNewProjectPage();
}

}
