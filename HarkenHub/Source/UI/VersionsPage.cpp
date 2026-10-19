#include "UI/VersionsPage.h"

#include "Localization/HubLocalization.h"
#include "UI/BoundedHeaderView.h"
#include "UI/HubIcons.h"
#include "UI/NoHoverItemDelegate.h"
#include "Packaging/PackageReader.h"
#include "Packaging/PackageVerifier.h"
#include "Platform/Elevation.h"
#include "Platform/PlatformPaths.h"
#include "Platform/ProcessScan.h"
#include "Projects/ProjectStore.h"
#include "Security/TrustAnchors.h"
#include "Transactions/HandoffArea.h"
#include "Transactions/InstallPackageCache.h"
#include "Transactions/TransactionJournal.h"
#include "Transactions/TransactionDiagnostics.h"
#include "UI/ConsentDialog.h"
#include "UI/ButtonPulseAnimator.h"
#include "UI/ServerVersionsDialog.h"
#include "Update/HubSelfUpdate.h"
#include "Update/ServerEndpoints.h"
#include "Update/UpdateStateStore.h"
#include "Versions/LaunchDiagnostics.h"
#include "Versions/LocalBuildRegistry.h"
#include "Versions/SelfTestRunner.h"
#include "Versions/StudioLaunchTarget.h"
#include "Versions/StudioCapabilities.h"

#include <QCoreApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSignalBlocker>
#include <QStyle>
#include <QTableWidget>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QVersionNumber>
#include <QWidgetAction>

#include <algorithm>

namespace VNH {

namespace {

constexpr int kColumnVersion = 0;
constexpr int kColumnInstallPath = 1;
constexpr int kColumnLinkedProjects = 2;
constexpr int kColumnMenu = 3;
constexpr int kVersionBuildIdRole = Qt::UserRole + 1;

bool isSafeFileToken(const QString& token)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"));
    return pattern.match(token).hasMatch();
}

QString humanSize(qint64 bytes)
{
    if (bytes < 0)
        return QStringLiteral("-");
    return QLocale::c().formattedDataSize(bytes, 1,
                                          QLocale::DataSizeSIFormat);
}

QString serverCheckPendingText()
{
    return QCoreApplication::translate("HarkenHub", "Checking the server for the latest version...");
}

QString normalizedExecutablePath(const QString& path)
{
    const QFileInfo executable(path);
    QString normalized = executable.canonicalFilePath();
    if (normalized.isEmpty())
        normalized = executable.absoluteFilePath();
    normalized = QDir::cleanPath(QDir::fromNativeSeparators(normalized));
#ifdef Q_OS_WIN
    normalized = normalized.toLower();
#endif
    return normalized;
}

}

VersionsPage::VersionsPage(ProjectStore* store, QWidget* parent)
    : QWidget(parent)
    , store_(store)
{
    auto* header = new QHBoxLayout();
    auto* title = new QLabel(QCoreApplication::translate("HarkenHub", "Versions"), this);
    title->setObjectName(QStringLiteral("pageTitle"));
    header->addWidget(title, 0, Qt::AlignBottom);
    countLabel_ = new QLabel(this);
    countLabel_->setObjectName(QStringLiteral("pageSubtitle"));
    header->addSpacing(10);
    header->addWidget(countLabel_, 0, Qt::AlignBottom);
    header->addStretch();
    installStatusLabel_ = new QLabel(this);
    installStatusLabel_->setObjectName(QStringLiteral("pageSubtitle"));
    header->addWidget(installStatusLabel_, 0, Qt::AlignBottom);
    header->addSpacing(6);
    busyBar_ = new QProgressBar(this);
    busyBar_->setRange(0, 0);
    busyBar_->setFixedSize(110, 8);
    busyBar_->setTextVisible(false);
    busyBar_->hide();
    header->addWidget(busyBar_, 0, Qt::AlignVCenter);
    header->addSpacing(10);
    getFromServerButton_ =
        new QPushButton(QCoreApplication::translate("HarkenHub", "Get from Server"), this);
    getFromServerButton_->setObjectName(
        QStringLiteral("getFromServerButton"));
    getFromServerButton_->setProperty("updateAvailable", false);
    getFromServerButton_->setToolTip(
        QCoreApplication::translate("HarkenHub", "Browse and install HarkenStudio versions published "
                       "on the release server."));
    connect(getFromServerButton_, &QPushButton::clicked, this,
            &VersionsPage::beginGetFromServer);
    serverUpdatePulse_ = new ButtonPulseAnimator(this);
    header->addWidget(getFromServerButton_);
    header->addSpacing(8);
    addButton_ = new QPushButton(QCoreApplication::translate("HarkenHub", "Add Version"), this);
    addButton_->setToolTip(
        QCoreApplication::translate("HarkenHub", "Add an official package or a standalone build."));
    connect(addButton_, &QPushButton::clicked, this,
            &VersionsPage::showAddVersionMenu);
    header->addWidget(addButton_);

    elevatedPollTimer_ = new QTimer(this);
    elevatedPollTimer_->setInterval(500);
    connect(elevatedPollTimer_, &QTimer::timeout, this,
            &VersionsPage::onElevatedPoll);

    statusPollTimer_ = new QTimer(this);
    statusPollTimer_->setInterval(1000);
    connect(statusPollTimer_, &QTimer::timeout, this, [this]() {
        updateAutoProgress();
        updateGetFromServerButton();
    });

    catalog_ = new ServerVersionCatalog(this);
    connect(catalog_, &ServerVersionCatalog::finished, this,
            &VersionsPage::onCatalogFinished);

    versionsWatcher_ = new QFileSystemWatcher(this);
    watchDebounceTimer_ = new QTimer(this);
    watchDebounceTimer_->setSingleShot(true);
    watchDebounceTimer_->setInterval(300);
    connect(versionsWatcher_, &QFileSystemWatcher::directoryChanged,
            watchDebounceTimer_, qOverload<>(&QTimer::start));
    connect(watchDebounceTimer_, &QTimer::timeout, this, [this]() {
        if (elevatedMode_ == ElevatedMode::None)
            refresh();
        else
            updateVersionsWatch();
    });
    updateVersionsWatch();

    repairBanner_ = new QFrame(this);
    repairBanner_->setObjectName(QStringLiteral("installationIssueBanner"));
    repairBanner_->setStyleSheet(QStringLiteral(
        "QFrame { background-color: #2A1420; border: 1px solid #FF0055; "
        "border-radius: 0; }"));
    auto* bannerLayout = new QHBoxLayout(repairBanner_);
    bannerLayout->setContentsMargins(14, 8, 14, 8);
    repairSelector_ = new QComboBox(repairBanner_);
    repairSelector_->setObjectName(QStringLiteral("installationIssueSelector"));
    repairSelector_->setMaximumWidth(240);
    repairSelector_->setMinimumContentsLength(12);
    repairSelector_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    bannerLayout->addWidget(repairSelector_);
    repairLabel_ = new QLabel(repairBanner_);
    repairLabel_->setObjectName(QStringLiteral("installationIssueText"));
    repairLabel_->setWordWrap(true);
    repairLabel_->setMinimumWidth(0);
    repairLabel_->setTextFormat(Qt::PlainText);
    repairLabel_->setStyleSheet(
        QStringLiteral("background: transparent; border: none; color: #E0E0E0;"));
    bannerLayout->addWidget(repairLabel_, 1);
    repairButton_ = new QPushButton(QCoreApplication::translate("HarkenHub", "Repair"), repairBanner_);
    repairButton_->setObjectName(QStringLiteral("installationRepairButton"));
    bannerLayout->addWidget(repairButton_);
    repairUninstallButton_ =
        new QPushButton(QCoreApplication::translate("HarkenHub", "Uninstall"), repairBanner_);
    repairUninstallButton_->setObjectName(QStringLiteral("installationUninstallButton"));
    bannerLayout->addWidget(repairUninstallButton_);
    connect(repairSelector_, &QComboBox::currentIndexChanged, this,
            &VersionsPage::updateSelectedIssue);
    connect(repairButton_, &QPushButton::clicked, this, [this]() {
        beginRepair(repairSelector_->currentData().toString());
    });
    connect(repairUninstallButton_, &QPushButton::clicked, this, [this]() {
        InstalledVersion version;
        version.buildId = repairSelector_->currentData().toString();
        beginUninstall(version);
    });
    repairBanner_->hide();

    hubUpdateBanner_ = new QFrame(this);
    hubUpdateBanner_->setStyleSheet(QStringLiteral(
        "QFrame { background-color: #0F2A30; border: 1px solid #00F0FF; "
        "border-radius: 0; }"));
    auto* updateBannerLayout = new QHBoxLayout(hubUpdateBanner_);
    updateBannerLayout->setContentsMargins(14, 8, 14, 8);
    hubUpdateLabel_ = new QLabel(hubUpdateBanner_);
    hubUpdateLabel_->setStyleSheet(
        QStringLiteral("background: transparent; border: none; color: #E0E0E0;"));
    updateBannerLayout->addWidget(hubUpdateLabel_, 1);
    hubUpdateButton_ = new QPushButton(
        QCoreApplication::translate("HarkenHub", "Install and Restart"), hubUpdateBanner_);
    connect(hubUpdateButton_, &QPushButton::clicked, this,
            &VersionsPage::beginHubUpdate);
    updateBannerLayout->addWidget(hubUpdateButton_);
    hubUpdateBanner_->hide();

    studioUpdateBanner_ = new QFrame(this);
    studioUpdateBanner_->setStyleSheet(QStringLiteral(
        "QFrame { background-color: #0F2A30; border: 1px solid #00F0FF; "
        "border-radius: 0; }"));
    auto* studioBannerLayout = new QHBoxLayout(studioUpdateBanner_);
    studioBannerLayout->setContentsMargins(14, 8, 14, 8);
    studioUpdateLabel_ = new QLabel(studioUpdateBanner_);
    studioUpdateLabel_->setStyleSheet(
        QStringLiteral("background: transparent; border: none; color: #E0E0E0;"));
    studioBannerLayout->addWidget(studioUpdateLabel_, 1);
    studioUpdateButton_ =
        new QPushButton(QCoreApplication::translate("HarkenHub", "Install"), studioUpdateBanner_);
    connect(studioUpdateButton_, &QPushButton::clicked, this,
            &VersionsPage::beginStudioUpdateInstall);
    studioBannerLayout->addWidget(studioUpdateButton_);
    studioUpdateBanner_->hide();

    autoProgressFrame_ = new QFrame(this);
    autoProgressFrame_->setStyleSheet(QStringLiteral(
        "QFrame { background-color: #0D111A; border: 1px solid #313848; "
        "border-radius: 0; }"));
    auto* progressLayout = new QHBoxLayout(autoProgressFrame_);
    progressLayout->setContentsMargins(14, 8, 14, 8);
    autoProgressLabel_ = new QLabel(autoProgressFrame_);
    autoProgressLabel_->setStyleSheet(
        QStringLiteral("background: transparent; border: none; color: #E0E0E0;"));
    progressLayout->addWidget(autoProgressLabel_, 1);
    autoProgressBar_ = new QProgressBar(autoProgressFrame_);
    autoProgressBar_->setFixedSize(180, 10);
    autoProgressBar_->setTextVisible(false);
    progressLayout->addWidget(autoProgressBar_);
    autoProgressFrame_->hide();

    table_ = new QTableWidget(this);
    auto* tableHeader = new BoundedHeaderView(Qt::Horizontal, table_);
    tableHeader->setFillSection(kColumnInstallPath);
    table_->setHorizontalHeader(tableHeader);
    table_->setColumnCount(4);
    table_->setHorizontalHeaderLabels({QCoreApplication::translate("HarkenHub", "VERSION"),
                                       QCoreApplication::translate("HarkenHub", "INSTALL PATH"),
                                       QCoreApplication::translate("HarkenHub", "LINKED PROJECTS"),
                                       QString()});
    table_->horizontalHeader()->setHighlightSections(false);
    table_->horizontalHeader()->setSectionsClickable(false);
    table_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    table_->horizontalHeader()->setSectionResizeMode(kColumnVersion,
                                                     QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(kColumnInstallPath,
                                                     QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(kColumnLinkedProjects,
                                                     QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(kColumnMenu, QHeaderView::Fixed);
    table_->horizontalHeader()->setMinimumSectionSize(70);
    table_->setColumnWidth(kColumnVersion, 230);
    table_->setColumnWidth(kColumnInstallPath, 600);
    table_->setColumnWidth(kColumnLinkedProjects, 110);
    table_->setColumnWidth(kColumnMenu, 70);
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(52);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setFocusPolicy(Qt::NoFocus);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setShowGrid(false);
    table_->setItemDelegateForColumn(kColumnInstallPath,
                                     new NoHoverItemDelegate(table_));
    table_->setItemDelegateForColumn(kColumnLinkedProjects,
                                     new NoHoverItemDelegate(table_));
    table_->setItemDelegateForColumn(kColumnMenu,
                                     new NoHoverItemDelegate(table_));
    connect(table_, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int column) {
                if (column != kColumnVersion)
                    return;
                const QTableWidgetItem* item =
                    table_->item(row, kColumnVersion);
                if (item)
                    emit projectLaunchGuidanceRequested(
                        item->data(kVersionBuildIdRole).toString());
            });
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QTableWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
                const int row = table_->rowAt(pos.y());
                const InstalledVersion* version = versionForRow(row);
                if (version)
                    showRowMenu(*version,
                                table_->viewport()->mapToGlobal(pos));
            });

    versionHighlightTimer_ = new QTimer(this);
    versionHighlightTimer_->setInterval(300);
    connect(versionHighlightTimer_, &QTimer::timeout, this,
            &VersionsPage::advanceVersionHighlight);

    auto* panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("tablePanel"));
    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(12, 4, 12, 8);
    panelLayout->addWidget(table_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->addLayout(header);
    layout->addSpacing(10);
    auto* launchHint = new QLabel(
        QCoreApplication::translate(
            "HarkenHub",
            "To create or open a project, use the Projects page. Double-click "
            "any version below to go to the Projects page."),
        this);
    launchHint->setWordWrap(true);
    launchHint->setStyleSheet(QStringLiteral(
        "background-color: #111722; border: 1px solid #3D4554; "
        "border-radius: 0; color: #AAB3C2; padding: 8px 11px;"));
    layout->addWidget(launchHint);
    layout->addWidget(hubUpdateBanner_);
    layout->addWidget(studioUpdateBanner_);
    layout->addWidget(repairBanner_);
    layout->addWidget(autoProgressFrame_);
    layout->addWidget(panel);
}

bool VersionsPage::operationBusy() const
{
    return preparing_ || elevatedMode_ != ElevatedMode::None;
}

QString VersionsPage::watchDirectoryFor(const QString& versionsRoot)
{
    QDir dir(QDir::cleanPath(versionsRoot));
    while (!dir.exists()) {
        const QString parent = QFileInfo(dir.absolutePath()).absolutePath();
        if (parent == dir.absolutePath())
            return QString();
        dir.setPath(parent);
    }
    return dir.absolutePath();
}

void VersionsPage::sortForDisplay(std::vector<InstalledVersion>& versions)
{
    std::stable_sort(
        versions.begin(), versions.end(),
        [](const InstalledVersion& left, const InstalledVersion& right) {
            const QVersionNumber leftNumber =
                QVersionNumber::fromString(left.semanticVersion);
            const QVersionNumber rightNumber =
                QVersionNumber::fromString(right.semanticVersion);
            const int comparison =
                QVersionNumber::compare(leftNumber, rightNumber);
            if (comparison != 0)
                return comparison > 0;
            if (left.semanticVersion != right.semanticVersion)
                return left.semanticVersion > right.semanticVersion;
            return left.buildId > right.buildId;
        });
}

void VersionsPage::updateVersionsWatch()
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        const QString target = watchDirectoryFor(studioVersionsRoot());
        const QStringList watched = versionsWatcher_->directories();
        if (watched.size() == 1 && watched.first() == target)
            return;
        if (!watched.isEmpty())
            versionsWatcher_->removePaths(watched);
        if (target.isEmpty())
            return;
        if (versionsWatcher_->addPath(target))
            return;
    }
}

void VersionsPage::refresh()
{
    updateVersionsWatch();
    std::vector<InstalledVersion> scanned = VersionScanner::scanManaged();
    QSet<QString> validBuildIds;
    for (const auto& version : scanned)
        validBuildIds.insert(version.buildId);

    const InstallIndexSnapshot index = InstallIndex::snapshot();
    std::vector<InstalledVersionRecord> broken;
    if (index.ok) {
        for (const auto& record : index.records) {
            if (record.healthState == QStringLiteral("NeedsRepair") ||
                (record.healthState == QStringLiteral("Installed") &&
                 !validBuildIds.contains(record.buildId))) {
                broken.push_back(record);
            }
        }
    }
    updateRepairBanner(broken);
    if (!index.ok) {
        repairLabel_->setText(QCoreApplication::translate("HarkenHub", "Cannot read installation records: %1")
                                  .arg(index.error));
        repairSelector_->hide();
        repairButton_->hide();
        repairUninstallButton_->hide();
        repairBanner_->show();
    }
    updateHubUpdateBanner();
    updateStudioUpdateBanner();
    updateAutoProgress();
    updateGetFromServerButton();

    sortForDisplay(scanned);
    versions_ = std::move(scanned);
    const int totalCount = static_cast<int>(versions_.size());

    const auto projects = store_->listProjects();
    auto linkedCount = [&projects](const QString& buildId) {
        int count = 0;
        for (const auto& project : projects)
            if (project.linkedStudioBuildId() == buildId)
                ++count;
        return count;
    };

    countLabel_->setText(totalCount == 1
                             ? QCoreApplication::translate("HarkenHub", "1 version")
                             : QCoreApplication::translate("HarkenHub", "%1 versions").arg(totalCount));

    table_->clearSpans();
    table_->setRowCount(0);
    table_->setRowCount(totalCount);
    for (int index = 0; index < totalCount; ++index) {
        const auto& version = versions_[static_cast<size_t>(index)];
        addVersionRow(index, version, linkedCount(version.buildId));
    }
    tryStartVersionHighlight();
}

void VersionsPage::highlightVersion(const QString& executablePath)
{
    highlightedExecutablePath_ = executablePath;
    highlightedRuntimeBuildId_.clear();
    const QString target = normalizedExecutablePath(executablePath);
    for (const LocalBuildRecord& record : LocalBuildRegistry::load()) {
        if (normalizedExecutablePath(record.executablePath) == target) {
            highlightedRuntimeBuildId_ = record.reportedBuildId;
            break;
        }
    }
    versionHighlightDeadlineMs_ = 0;
    versionHighlightPhase_ = false;
    versionHighlightTimer_->stop();
    tryStartVersionHighlight();
}

void VersionsPage::highlightServerUpdate()
{
    serverUpdatePulse_->start(getFromServerButton_);
}

void VersionsPage::tryStartVersionHighlight()
{
    if (highlightedExecutablePath_.isEmpty())
        return;

    for (int row = 0; row < table_->rowCount(); ++row) {
        const InstalledVersion* version = versionForRow(row);
        if (!version || !versionMatchesHighlight(*version))
            continue;

        if (versionHighlightDeadlineMs_ == 0) {
            versionHighlightDeadlineMs_ =
                QDateTime::currentMSecsSinceEpoch() + 3000;
            versionHighlightPhase_ = true;
            versionHighlightTimer_->start();
            table_->scrollToItem(table_->item(row, kColumnVersion),
                                 QAbstractItemView::PositionAtCenter);
        }
        applyVersionHighlight();
        return;
    }
}

void VersionsPage::advanceVersionHighlight()
{
    if (QDateTime::currentMSecsSinceEpoch() >=
        versionHighlightDeadlineMs_) {
        versionHighlightTimer_->stop();
        versionHighlightPhase_ = false;
        applyVersionHighlight();
        highlightedExecutablePath_.clear();
        highlightedRuntimeBuildId_.clear();
        versionHighlightDeadlineMs_ = 0;
        return;
    }
    versionHighlightPhase_ = !versionHighlightPhase_;
    applyVersionHighlight();
}

void VersionsPage::applyVersionHighlight()
{
    table_->clearSelection();
    if (!versionHighlightPhase_)
        return;

    for (int row = 0; row < table_->rowCount(); ++row) {
        const InstalledVersion* version = versionForRow(row);
        if (version && versionMatchesHighlight(*version)) {
            table_->selectionModel()->select(
                table_->model()->index(row, kColumnVersion),
                QItemSelectionModel::ClearAndSelect);
            table_->viewport()->update();
            return;
        }
    }
}

bool VersionsPage::versionMatchesHighlight(
    const InstalledVersion& version) const
{
    if (normalizedExecutablePath(version.executablePath) ==
        normalizedExecutablePath(highlightedExecutablePath_))
        return true;
    return !highlightedRuntimeBuildId_.isEmpty() &&
           (version.buildId == highlightedRuntimeBuildId_ ||
            version.runtimeBuildId == highlightedRuntimeBuildId_);
}

const InstalledVersion* VersionsPage::versionForRow(int row) const
{
    const QTableWidgetItem* item = table_->item(row, kColumnVersion);
    if (!item)
        return nullptr;
    const QString buildId = item->data(kVersionBuildIdRole).toString();
    const auto match = std::find_if(
        versions_.begin(), versions_.end(), [&buildId](const auto& version) {
            return version.buildId == buildId;
        });
    return match == versions_.end() ? nullptr : &*match;
}

void VersionsPage::setAutoDownloadStudio(bool enabled)
{
    autoDownloadStudio_ = enabled;
    updateGetFromServerButton();
}

void VersionsPage::updateGetFromServerButton()
{
    const UpdateStateRecord record =
        UpdateStateStore::load(QStringLiteral("HarkenStudio"));
    const bool updateAvailable =
        !autoDownloadStudio_ && record.found &&
        record.state == QStringLiteral("MetadataAvailable");
    if (getFromServerButton_->property("updateAvailable").toBool() ==
        updateAvailable)
        return;
    getFromServerButton_->setProperty("updateAvailable", updateAvailable);
    getFromServerButton_->style()->unpolish(getFromServerButton_);
    getFromServerButton_->style()->polish(getFromServerButton_);
    getFromServerButton_->update();
}

void VersionsPage::addVersionRow(int row, const InstalledVersion& version,
                                 int linkedProjects)
{
    QString displayName =
        QCoreApplication::translate("HarkenHub", "HarkenStudio %1").arg(version.semanticVersion);
    if (version.isLocalBuild())
        displayName += version.available ? QCoreApplication::translate("HarkenHub", " (Standalone)")
                                         : QCoreApplication::translate("HarkenHub", " (Standalone, Unavailable)");
    if (version.available && version.capabilitiesVersion < kStudioCapabilitiesVersion)
        displayName += QCoreApplication::translate("HarkenHub", " (Legacy)");
    auto* versionItem = new QTableWidgetItem(displayName);
    versionItem->setData(kVersionBuildIdRole, version.buildId);
    QFont nameFont = versionItem->font();
    nameFont.setWeight(QFont::DemiBold);
    versionItem->setFont(nameFont);
    versionItem->setToolTip(
        version.available
            ? QCoreApplication::translate("HarkenHub", "Build: %1\nChannel: %2")
                  .arg(version.launchBuildId(), version.channel)
            : version.availabilityError);
    if (version.available && version.capabilitiesVersion != kStudioCapabilitiesVersion)
        versionItem->setToolTip(versionItem->toolTip() + QLatin1Char('\n')
            + (version.capabilitiesVersion > kStudioCapabilitiesVersion
                ? QCoreApplication::translate("HarkenHub", "This Studio uses a newer protocol than Hub supports. Hub can request project opening but cannot report its result or preview upgrades.")
                : QCoreApplication::translate("HarkenHub", "Projects open in this Studio version. Hub upgrade previews are unavailable.")));
    table_->setItem(row, kColumnVersion, versionItem);

    const QString installPath = QDir::toNativeSeparators(
        version.isLocalBuild() ? version.executablePath : version.installPath);
    auto* installPathItem = new QTableWidgetItem(installPath);
    installPathItem->setToolTip(installPath);
    table_->setItem(row, kColumnInstallPath, installPathItem);

    table_->setItem(row, kColumnLinkedProjects,
                    new QTableWidgetItem(QString::number(linkedProjects)));
    table_->setItem(row, kColumnMenu, new QTableWidgetItem());

    auto* holder = new QWidget(table_);
    holder->setObjectName(QStringLiteral("rowActionCell"));
    holder->setStyleSheet(
        QStringLiteral("#rowActionCell { background: transparent; }"));
    auto* cellLayout = new QHBoxLayout(holder);
    cellLayout->setContentsMargins(0, 0, 12, 0);
    cellLayout->addStretch();
    auto* menuButton = new QToolButton(holder);
    menuButton->setObjectName(QStringLiteral("rowMenuButton"));
    menuButton->setIcon(HubIcons::moreIcon(holder->devicePixelRatioF()));
    menuButton->setIconSize(QSize(16, 16));
    menuButton->setFixedSize(34, 26);
    connect(menuButton, &QToolButton::clicked, this,
            [this, version, menuButton]() {
                showRowMenu(version, menuButton->mapToGlobal(
                                         menuButton->rect().bottomLeft()));
            });
    cellLayout->addWidget(menuButton);
    table_->setCellWidget(row, kColumnMenu, holder);
}

void VersionsPage::updateRepairBanner(
    const std::vector<InstalledVersionRecord>& broken)
{
    const QString selected = repairSelector_->currentData().toString();
    const QSignalBlocker blocker(repairSelector_);
    repairSelector_->clear();
    if (broken.empty()) {
        repairBanner_->hide();
        return;
    }
    for (const auto& record : broken) {
        repairSelector_->addItem(QStringLiteral("%1 [%2]")
                                     .arg(record.semanticVersion, record.buildId), record.buildId);
        repairSelector_->setItemData(repairSelector_->count() - 1,
                                    record.installPath, Qt::ToolTipRole);
    }
    const int previous = repairSelector_->findData(selected);
    repairSelector_->setCurrentIndex(previous < 0 ? 0 : previous);
    repairSelector_->setVisible(broken.size() > 1);
    updateSelectedIssue();
    repairBanner_->show();
}

void VersionsPage::updateSelectedIssue()
{
    const QString buildId = repairSelector_->currentData().toString();
    if (buildId.isEmpty())
        return;
    const auto index = InstallIndex::snapshot();
    const auto record = std::find_if(index.records.begin(), index.records.end(),
        [&buildId](const auto& candidate) { return candidate.buildId == buildId; });
    if (!index.ok || record == index.records.end()) {
        repairButton_->setEnabled(false);
        repairUninstallButton_->setEnabled(false);
        repairLabel_->setText(index.ok
            ? QCoreApplication::translate("HarkenHub", "The installation record is no longer available.")
            : index.error);
        return;
    }
    QString reason;
    const auto issue = VersionScanner::inspect(*record, nullptr, &reason);
    if (issue == InstallationIssue::None)
        reason = QCoreApplication::translate("HarkenHub", "The previous operation requires an integrity check.");
    const bool sourceAvailable = QFileInfo(InstallPackageCache::packagePath(buildId)).isFile()
        && QFileInfo(InstallPackageCache::manifestPath(buildId)).isFile();
    const bool repairable = issue != InstallationIssue::IdentityMismatch
        && issue != InstallationIssue::Unreadable;
    if (repairable && issue != InstallationIssue::None && !sourceAvailable)
        reason += QLatin1Char('\n') + QCoreApplication::translate("HarkenHub", "The repair package is unavailable.");
    repairLabel_->setText(QCoreApplication::translate("HarkenHub", "HarkenStudio %1: %2")
                              .arg(record->semanticVersion, reason));
    repairLabel_->setToolTip(QDir::toNativeSeparators(record->installPath));
    repairButton_->show();
    repairUninstallButton_->show();
    repairSelector_->setEnabled(!operationBusy());
    repairButton_->setEnabled(!operationBusy() && repairable
        && (issue == InstallationIssue::None || sourceAvailable));
    repairUninstallButton_->setEnabled(!operationBusy()
        && issue != InstallationIssue::IdentityMismatch && issue != InstallationIssue::Unreadable);
}

void VersionsPage::updateHubUpdateBanner()
{
    const UpdateStateRecord record =
        UpdateStateStore::load(QStringLiteral("HarkenHub"));
    if (record.found &&
        record.state == QStringLiteral("AwaitingUserApproval")) {
        hubUpdateLabel_->setText(
            QCoreApplication::translate("HarkenHub", "Harken Studio Hub %1 is ready to install.")
                .arg(record.version));
        hubUpdateButton_->setEnabled(true);
        hubUpdateBanner_->show();
        return;
    }
    if (record.found &&
        record.state == QStringLiteral("AwaitingElevation")) {
        hubUpdateLabel_->setText(
            QCoreApplication::translate("HarkenHub", "Harken Studio Hub %1 update is installing. The "
                           "hub closes and restarts automatically.")
                .arg(record.version));
        hubUpdateButton_->setEnabled(false);
        hubUpdateBanner_->show();
        return;
    }
    hubUpdateBanner_->hide();
}

void VersionsPage::updateStudioUpdateBanner()
{
    const UpdateStateRecord record =
        UpdateStateStore::load(QStringLiteral("HarkenStudio"));
    if (record.found &&
        record.state == QStringLiteral("AwaitingUserInstall") &&
        QFileInfo::exists(record.downloadPath) &&
        QFileInfo::exists(record.manifestEnvelopePath)) {
        studioUpdateLabel_->setText(
            QCoreApplication::translate("HarkenHub", "HarkenStudio %1 has been downloaded and verified.")
                .arg(record.version));
        studioUpdateButton_->setEnabled(!operationBusy());
        studioUpdateBanner_->show();
        return;
    }
    studioUpdateBanner_->hide();
}

void VersionsPage::updateAutoProgress()
{
    QString text;
    int percent = -1;

    const UpdateStateRecord studio =
        UpdateStateStore::load(QStringLiteral("HarkenStudio"));
    const UpdateStateRecord hub =
        UpdateStateStore::load(QStringLiteral("HarkenHub"));

    if (studio.found && studio.state == QStringLiteral("Downloading")) {
        if (studio.packageSize > 0 && studio.downloadedBytes >= 0) {
            percent = static_cast<int>(studio.downloadedBytes * 100 /
                                       studio.packageSize);
            text = QCoreApplication::translate("HarkenHub", "Downloading HarkenStudio %1 - %2 of %3")
                       .arg(studio.version,
                            humanSize(studio.downloadedBytes),
                            humanSize(studio.packageSize));
        } else {
            text = QCoreApplication::translate("HarkenHub", "Downloading HarkenStudio %1...")
                       .arg(studio.version);
        }
    } else if (studio.found &&
               (studio.state == QStringLiteral("Downloaded") ||
                studio.state == QStringLiteral("Verifying"))) {
        text = QCoreApplication::translate("HarkenHub", "Verifying HarkenStudio %1...")
                   .arg(studio.version);
    } else if (studio.found &&
               studio.state == QStringLiteral("AwaitingBridge")) {
        text = QCoreApplication::translate("HarkenHub", "Installing HarkenStudio %1 in the background...")
                   .arg(studio.version);
    } else if (hub.found && hub.state == QStringLiteral("Downloading")) {
        if (hub.packageSize > 0 && hub.downloadedBytes >= 0) {
            percent = static_cast<int>(hub.downloadedBytes * 100 /
                                       hub.packageSize);
            text = QCoreApplication::translate("HarkenHub", "Downloading Harken Studio Hub %1 - %2 of %3")
                       .arg(hub.version, humanSize(hub.downloadedBytes),
                            humanSize(hub.packageSize));
        } else {
            text = QCoreApplication::translate("HarkenHub", "Downloading Harken Studio Hub %1...")
                       .arg(hub.version);
        }
    }

    if (text.isEmpty()) {
        autoProgressFrame_->hide();
        return;
    }
    if (installStatusLabel_->text() == serverCheckPendingText())
        installStatusLabel_->setText(QString());
    autoProgressLabel_->setText(text);
    if (percent >= 0) {
        autoProgressBar_->setRange(0, 100);
        autoProgressBar_->setValue(percent);
    } else {
        autoProgressBar_->setRange(0, 0);
    }
    autoProgressFrame_->show();
}

void VersionsPage::showServerCheckPending()
{
    installStatusLabel_->setText(serverCheckPendingText());
    QTimer::singleShot(20000, this, [this]() {
        if (installStatusLabel_->text() == serverCheckPendingText())
            installStatusLabel_->setText(QString());
    });
}

void VersionsPage::beginStudioUpdateInstall()
{
    if (operationBusy())
        return;
    const UpdateStateRecord record =
        UpdateStateStore::load(QStringLiteral("HarkenStudio"));
    if (!record.found ||
        record.state != QStringLiteral("AwaitingUserInstall"))
        return;
    beginInstallFromFiles(record.downloadPath, record.manifestEnvelopePath,
                          false);
}

void VersionsPage::beginHubUpdate()
{
    QMessageBox confirm(
        QMessageBox::Question, QCoreApplication::translate("HarkenHub", "Harken Studio Hub"),
        QCoreApplication::translate("HarkenHub", "Harken Studio Hub will close to install the update. "
                       "Open HarkenStudio sessions are not affected.\n\n"
                       "Continue?"),
        QMessageBox::Cancel, this);
    QPushButton* proceed = confirm.addButton(
        QCoreApplication::translate("HarkenHub", "Install and Restart"), QMessageBox::AcceptRole);
    confirm.exec();
    if (confirm.clickedButton() != proceed)
        return;

    HubSelfUpdate selfUpdate;
    QString error;
    if (!selfUpdate.approve(&error, QDateTime::currentDateTimeUtc()))
        QMessageBox::warning(
            this, QCoreApplication::translate("HarkenHub", "Harken Studio Hub"),
            QCoreApplication::translate(
                "HarkenHub", "The Hub update could not be started.\n\n%1")
                .arg(error));
    updateHubUpdateBanner();
}

VersionsPage::~VersionsPage()
{
    if (elevatedProcessHandle_) {
        int exitCode = 1;
        if (pollElevatedInstall(elevatedProcessHandle_, &exitCode) &&
            exitCode != 0 && elevatedMode_ == ElevatedMode::Install)
            HandoffArea::discard(activeTransactionId_);
        releaseElevatedInstall(elevatedProcessHandle_);
    }
}

void VersionsPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refresh();
    statusPollTimer_->start();
}

void VersionsPage::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    statusPollTimer_->stop();
}

void VersionsPage::beginAddVersion()
{
    if (operationBusy())
        return;
    if (!ConsentDialog::requestConsent(this,
                                       ConsentDialog::studioDocumentId()))
        return;

    const QString packagePath = QFileDialog::getOpenFileName(
        this, QCoreApplication::translate("HarkenHub", "Select a HarkenStudio package"), QString(),
        QCoreApplication::translate("HarkenHub", "HarkenStudio Package (*.zip)"));
    if (packagePath.isEmpty())
        return;

    QString platformError;
    if (!PackageVerifier::checkHostPlatform(packagePath, &platformError)) {
        QMessageBox::warning(this, QCoreApplication::translate("HarkenHub", "Harken Studio Hub"), platformError);
        return;
    }

    const QString manifestPath =
        InstallRequest::manifestPathForPackage(packagePath);
    if (!manifestPath.isEmpty()) {
        beginInstallFromFiles(packagePath, manifestPath, true);
        return;
    }
    fetchManifestForPackage(packagePath);
}

void VersionsPage::beginAddLocalBuild()
{
    if (operationBusy())
        return;

#ifdef Q_OS_WIN
    const QString filter = QCoreApplication::translate("HarkenHub", "HarkenStudio (HarkenStudio.exe)");
#else
    const QString filter = QCoreApplication::translate("HarkenHub", "HarkenStudio executable (*)");
#endif
    const QString executablePath = QFileDialog::getOpenFileName(
        this, QCoreApplication::translate("HarkenHub", "Select a standalone HarkenStudio build"), QString(),
        filter);
    if (executablePath.isEmpty())
        return;

    preparing_ = true;
    setManagementButtonsEnabled(false);
    busyBar_->show();
    installStatusLabel_->setText(QCoreApplication::translate("HarkenHub", "Checking standalone build..."));

    const QPointer<VersionsPage> self(this);
    QThreadPool::globalInstance()->start([self, executablePath]() {
        LocalBuildRecord record;
        QString error;
        const bool ok = LocalBuildRegistry::probe(executablePath, &record,
                                                  &error) &&
                        LocalBuildRegistry::upsert(record, &error);
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, ok, error]() {
                if (!self)
                    return;
                self->finishElevatedFlow(
                    ok ? QCoreApplication::translate("HarkenHub", "Standalone build added") : QString());
                if (!ok) {
                    QMessageBox::warning(
                        self, QCoreApplication::translate("HarkenHub", "Harken Studio Hub"),
                        QCoreApplication::translate("HarkenHub", "The standalone build could not be added.\n\n%1")
                            .arg(error));
                    return;
                }
                self->refresh();
                emit self->launchTargetChanged();
            },
            Qt::QueuedConnection);
    });
}

void VersionsPage::removeLocalBuild(const InstalledVersion& version)
{
    QMessageBox confirm(
        QMessageBox::Warning, QCoreApplication::translate("HarkenHub", "Harken Studio Hub"),
        QCoreApplication::translate("HarkenHub",
            "Remove this standalone build from Harken Studio Hub?\n\nThe build files will "
            "not be deleted. Projects linked to it cannot open until the "
            "same build is added again."),
        QMessageBox::Cancel, this);
    QPushButton* remove = confirm.addButton(
        QCoreApplication::translate("HarkenHub", "Remove from Hub"), QMessageBox::DestructiveRole);
    confirm.exec();
    if (confirm.clickedButton() != remove)
        return;

    QString error;
    if (!LocalBuildRegistry::remove(version.executablePath, &error)) {
        QMessageBox::warning(
            this, QCoreApplication::translate("HarkenHub", "Harken Studio Hub"),
            QCoreApplication::translate(
                "HarkenHub", "The standalone build could not be removed.\n\n%1")
                .arg(error));
        return;
    }
    refresh();
    emit launchTargetChanged();
}

void VersionsPage::setManagementButtonsEnabled(bool enabled)
{
    addButton_->setEnabled(enabled);
    getFromServerButton_->setEnabled(enabled);
    if (enabled) {
        updateSelectedIssue();
    } else {
        repairSelector_->setEnabled(false);
        repairButton_->setEnabled(false);
        repairUninstallButton_->setEnabled(false);
    }
}

void VersionsPage::showAddVersionMenu()
{
    QMenu menu(this);
    menu.setAttribute(Qt::WA_TranslucentBackground);

    auto addChoice = [this, &menu](const QString& title,
                                   const QString& description,
                                   void (VersionsPage::*handler)()) {
        auto* action = new QWidgetAction(&menu);
        auto* button = new QPushButton(&menu);
        button->setCursor(Qt::PointingHandCursor);
        button->setMinimumSize(390, 62);
        button->setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; border: none; "
            "border-radius: 0; padding: 8px 14px; text-align: left; }"
            "QPushButton:hover { background: #333B4D; }"
            "QPushButton:pressed { background: #252C3A; }"));

        auto* layout = new QVBoxLayout(button);
        layout->setContentsMargins(14, 7, 14, 7);
        layout->setSpacing(3);
        auto* titleLabel = new QLabel(title, button);
        titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        titleLabel->setStyleSheet(QStringLiteral(
            "background: transparent; border: none; color: #F2F4F8; "
            "font-weight: 600;"));
        layout->addWidget(titleLabel);
        auto* descriptionLabel = new QLabel(description, button);
        descriptionLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        descriptionLabel->setStyleSheet(QStringLiteral(
            "background: transparent; border: none; color: #9AA6BD;"));
        layout->addWidget(descriptionLabel);

        action->setDefaultWidget(button);
        menu.addAction(action);
        connect(button, &QPushButton::clicked, this,
                [this, &menu, handler]() {
                    menu.close();
                    QTimer::singleShot(0, this, handler);
                });
    };

    addChoice(QCoreApplication::translate("HarkenHub", "Install from Package (.zip)"),
              QCoreApplication::translate("HarkenHub", "Select an official HarkenStudio .zip installation package."),
              &VersionsPage::beginAddVersion);
    addChoice(QCoreApplication::translate("HarkenHub", "Add Standalone Build (.exe)"),
              QCoreApplication::translate("HarkenHub", "Select a HarkenStudio executable outside Hub-managed installations."),
              &VersionsPage::beginAddLocalBuild);

    menu.exec(addButton_->mapToGlobal(QPoint(0, addButton_->height() + 4)));
}

void VersionsPage::fetchManifestForPackage(const QString& packagePath)
{
    QByteArray metadataBytes;
    QString readError;
    if (!PackageReader::readEntry(packagePath,
                                  QStringLiteral("package-manifest.json"),
                                  &metadataBytes, &readError)) {
        QMessageBox::warning(
            this, QCoreApplication::translate("HarkenHub", "Harken Studio Hub"),
            QCoreApplication::translate("HarkenHub", "No release manifest found next to this package, "
                           "and the package metadata cannot be read.\n%1")
                .arg(readError));
        return;
    }
    const QString buildId = QJsonDocument::fromJson(metadataBytes)
                                .object()[QStringLiteral("buildId")]
                                .toString();
    if (!isSafeFileToken(buildId)) {
        QMessageBox::warning(
            this, QCoreApplication::translate("HarkenHub", "Harken Studio Hub"),
            QCoreApplication::translate("HarkenHub", "No release manifest found next to this package, "
                           "and the package metadata has no usable build "
                           "id."));
        return;
    }
    if (catalog_->isFetching())
        return;

    pendingManifestPackagePath_ = packagePath;
    pendingManifestBuildId_ = buildId;
    preparing_ = true;
    setManagementButtonsEnabled(false);
    busyBar_->show();
    installStatusLabel_->setText(
        QCoreApplication::translate("HarkenHub", "Fetching the release manifest from the server..."));
    if (!catalog_->fetch(ServerEndpoints::manifestUrls(
            ServerEndpoints::resolvedBaseUrl(), QStringLiteral("harken"),
            ServerEndpoints::hostChannel()), QStringLiteral("harken"),
            ServerEndpoints::hostChannel())) {
        pendingManifestPackagePath_.clear();
        pendingManifestBuildId_.clear();
        finishElevatedFlow(QString());
        QMessageBox::warning(
            this, QCoreApplication::translate("HarkenHub", "Harken Studio Hub"),
            QCoreApplication::translate("HarkenHub", "No release manifest found next to this package, "
                           "and the update server is not configured."));
    }
}

void VersionsPage::onCatalogFinished(const ServerVersionList& result)
{
    if (pendingManifestPackagePath_.isEmpty())
        return;
    const QString packagePath = pendingManifestPackagePath_;
    const QString buildId = pendingManifestBuildId_;
    pendingManifestPackagePath_.clear();
    pendingManifestBuildId_.clear();
    preparing_ = false;

    if (!result.ok) {
        finishElevatedFlow(QString());
        QMessageBox::warning(
            this, QCoreApplication::translate("HarkenHub", "Harken Studio Hub"),
            QCoreApplication::translate("HarkenHub", "No release manifest found next to this package, "
                           "and the server lookup failed.\n%1")
                .arg(result.error));
        return;
    }

    const QByteArray envelope = ServerVersionCatalog::matchEnvelopeForBuild(
        result.entries, TrustAnchors::manifestPublicKey(), buildId,
        QDateTime::currentDateTimeUtc());
    if (envelope.isEmpty()) {
        finishElevatedFlow(QString());
        QMessageBox::warning(
            this, QCoreApplication::translate("HarkenHub", "Harken Studio Hub"),
            QCoreApplication::translate("HarkenHub", "The server has no release manifest for this "
                           "package (build %1).")
                .arg(buildId));
        return;
    }

    const QString downloadDir = userDataDir() + QStringLiteral("/downloads/manual");
    const QString envelopePath =
        downloadDir + QStringLiteral("/") + buildId +
        QStringLiteral(".manifest");
    QString leaseError;
    const auto manifestLease = DownloadLease::acquire(envelopePath, &leaseError);
    if (!manifestLease) {
        finishElevatedFlow(leaseError);
        return;
    }
    QSaveFile envelopeFile(envelopePath);
    if (!envelopeFile.open(QIODevice::WriteOnly) ||
        envelopeFile.write(envelope) != envelope.size() ||
        !envelopeFile.commit()) {
        finishElevatedFlow(QString());
        QMessageBox::warning(
            this, QCoreApplication::translate("HarkenHub", "Harken Studio Hub"),
            QCoreApplication::translate("HarkenHub", "The release manifest could not be stored."));
        return;
    }

    beginInstallFromFiles(packagePath, envelopePath, true, manifestLease);
}

void VersionsPage::beginGetFromServer()
{
    if (operationBusy())
        return;
    if (!ConsentDialog::requestConsent(this,
                                       ConsentDialog::studioDocumentId()))
        return;

    QSet<QString> installedBuildIds;
    QSet<QString> installedVersionNames;
    for (const auto& version : VersionScanner::scanInstalled()) {
        installedBuildIds.insert(version.buildId);
        installedVersionNames.insert(version.semanticVersion);
    }
    const InstallIndexSnapshot index = InstallIndex::snapshot();
    if (index.ok) {
        for (const auto& record : index.records) {
            installedBuildIds.insert(record.buildId);
            installedVersionNames.insert(record.semanticVersion);
        }
    }

    ServerVersionsDialog dialog(TrustAnchors::manifestPublicKey(),
                                installedBuildIds, installedVersionNames,
                                this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (dialog.packagePath().isEmpty() || dialog.manifestPath().isEmpty())
        return;
    beginInstallFromFiles(dialog.packagePath(), dialog.manifestPath(), true,
                          dialog.takeDownloadLease());
}

void VersionsPage::beginInstallFromFiles(const QString& packagePath,
                                         const QString& manifestPath,
                                         bool explicitInstall,
                                         std::shared_ptr<DownloadLease> lease)
{
    if (operationBusy())
        return;
    auto packageLease = lease && lease->covers(packagePath)
                            ? lease : std::shared_ptr<DownloadLease>();
    if (!packageLease && DownloadLease::isCachePath(packagePath)) {
        QString error;
        packageLease = DownloadLease::acquire(packagePath, &error);
        if (!packageLease) {
            finishElevatedFlow(error);
            return;
        }
    }
    auto manifestLease = lease && lease->covers(manifestPath) ? lease
                         : packageLease && packageLease->covers(manifestPath)
                             ? packageLease : std::shared_ptr<DownloadLease>();
    if (!manifestLease && DownloadLease::isCachePath(manifestPath)) {
        QString error;
        manifestLease = DownloadLease::acquire(manifestPath, &error);
        if (!manifestLease) {
            finishElevatedFlow(error);
            return;
        }
    }
    preparing_ = true;
    setManagementButtonsEnabled(false);
    busyBar_->show();
    activeExplicitInstall_ = explicitInstall;
    installStatusLabel_->setText(QCoreApplication::translate("HarkenHub", "Verifying package..."));

    const QByteArray publicKey = TrustAnchors::manifestPublicKey();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QPointer<VersionsPage> self(this);
    QThreadPool::globalInstance()->start(
        [self, packagePath, manifestPath, publicKey, now, explicitInstall,
         packageLease = std::move(packageLease),
         manifestLease = std::move(manifestLease)]() {
            const InstallPreparation preparation =
                InstallRequest::prepare(packagePath, manifestPath, publicKey,
                                        now, explicitInstall);
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [self, preparation]() {
                    if (self)
                        self->onPrepared(preparation);
                    else if (preparation.ok)
                        HandoffArea::discard(preparation.transactionId);
                },
                Qt::QueuedConnection);
        });
}

void VersionsPage::onPrepared(const InstallPreparation& preparation)
{
    if (!preparation.ok) {
        QMessageBox::warning(
            this, QCoreApplication::translate("HarkenHub", "Harken Studio Hub"),
            QCoreApplication::translate(
                "HarkenHub", "The operation could not be prepared.\n\n%1")
                .arg(preparation.error));
        finishElevatedFlow(QString());
        return;
    }

    installStatusLabel_->setText(
        QCoreApplication::translate("HarkenHub", "Installing HarkenStudio %1...").arg(preparation.version));

    const ElevationLaunchResult launch = launchElevatedInstall(
        preparation.transactionId, activeExplicitInstall_);
    if (!launch.ok) {
        HandoffArea::discard(preparation.transactionId);
        TransactionJournal::append(preparation.transactionId,
                                   QStringLiteral("HarkenStudio"),
                                   preparation.buildId,
                                   QStringLiteral("Failed"),
                                   launch.canceled
                                       ? QStringLiteral("ElevationDeclined")
                                       : QStringLiteral("ElevationFailed"),
                                   QDateTime::currentDateTimeUtc());
        if (!launch.canceled)
            QMessageBox::warning(
                this, QCoreApplication::translate("HarkenHub", "Harken Studio Hub"),
                QCoreApplication::translate(
                    "HarkenHub", "The operation could not be started.\n\n%1")
                    .arg(launch.error));
        finishElevatedFlow(QString());
        return;
    }

    activeTransactionId_ = preparation.transactionId;
    activeBuildId_ = preparation.buildId;
    elevatedMode_ = ElevatedMode::Install;
    preparing_ = false;
    elevatedProcessHandle_ = launch.processHandle;
    elevatedPollTimer_->start();
}

void VersionsPage::beginUninstall(const InstalledVersion& version)
{
    if (operationBusy())
        return;

    if (!version.installPath.isEmpty() &&
        !runningExecutablesUnder(version.installPath).isEmpty()) {
        QMessageBox::information(
            this, QCoreApplication::translate("HarkenHub", "Harken Studio Hub"),
            QCoreApplication::translate("HarkenHub", "HarkenStudio is still running from this version. "
                           "Close it and try again."));
        return;
    }

    const auto projects = store_->listProjects();
    QStringList linkedNames;
    for (const auto& project : projects)
        if (project.linkedStudioBuildId() == version.buildId)
            linkedNames.append(project.displayName);

    QString text = QCoreApplication::translate("HarkenHub",
        "Uninstall HarkenStudio %1?\n\nThis removes the version for every "
        "user of this machine.").arg(version.semanticVersion.isEmpty()
                                         ? version.buildId
                                         : version.semanticVersion);
    if (!linkedNames.isEmpty())
        text += QCoreApplication::translate("HarkenHub",
                    "\n\n%1 of your projects use this version and will show "
                    "\"Version unavailable\" afterwards:\n- %2")
                    .arg(linkedNames.size())
                    .arg(linkedNames.mid(0, 8)
                             .join(QStringLiteral("\n- ")));

    QMessageBox confirm(QMessageBox::Warning,
                        QCoreApplication::translate("HarkenHub", "Harken Studio Hub"), text,
                        QMessageBox::Cancel, this);
    QPushButton* proceed = confirm.addButton(
        QCoreApplication::translate("HarkenHub", "Uninstall"), QMessageBox::DestructiveRole);
    confirm.exec();
    if (confirm.clickedButton() != proceed)
        return;

    if (!linkedNames.isEmpty()) {
        QMessageBox second(QMessageBox::Warning,
                           QCoreApplication::translate("HarkenHub", "Harken Studio Hub"),
                           QCoreApplication::translate("HarkenHub",
                               "Linked projects will lose their selected "
                               "version. You can also cancel and pick "
                               "another version for them first.\n\n"
                               "Really uninstall?"),
                           QMessageBox::Cancel, this);
        QPushButton* really = second.addButton(
            QCoreApplication::translate("HarkenHub", "Uninstall anyway"), QMessageBox::DestructiveRole);
        second.exec();
        if (second.clickedButton() != really)
            return;
    }

    setManagementButtonsEnabled(false);
    busyBar_->show();
    installStatusLabel_->setText(QCoreApplication::translate("HarkenHub", "Uninstalling..."));

    const ElevationLaunchResult launch =
        launchElevatedUninstall(version.buildId);
    if (!launch.ok) {
        if (!launch.canceled)
            QMessageBox::warning(
                this, QCoreApplication::translate("HarkenHub", "Harken Studio Hub"),
                QCoreApplication::translate(
                    "HarkenHub", "The operation could not be started.\n\n%1")
                    .arg(launch.error));
        finishElevatedFlow(QString());
        return;
    }
    activeBuildId_ = version.buildId;
    elevatedMode_ = ElevatedMode::Uninstall;
    elevatedProcessHandle_ = launch.processHandle;
    elevatedPollTimer_->start();
}

void VersionsPage::beginRepair(const QString& buildId)
{
    if (operationBusy())
        return;

    setManagementButtonsEnabled(false);
    busyBar_->show();
    installStatusLabel_->setText(QCoreApplication::translate("HarkenHub", "Repairing..."));

    const ElevationLaunchResult launch = launchElevatedRepair(buildId);
    if (!launch.ok) {
        if (!launch.canceled)
            QMessageBox::warning(
                this, QCoreApplication::translate("HarkenHub", "Harken Studio Hub"),
                QCoreApplication::translate(
                    "HarkenHub", "The operation could not be started.\n\n%1")
                    .arg(launch.error));
        finishElevatedFlow(QString());
        return;
    }
    activeBuildId_ = buildId;
    elevatedMode_ = ElevatedMode::Repair;
    elevatedProcessHandle_ = launch.processHandle;
    elevatedPollTimer_->start();
}

void VersionsPage::onElevatedPoll()
{
    int exitCode = 1;
    if (!pollElevatedInstall(elevatedProcessHandle_, &exitCode))
        return;

    elevatedPollTimer_->stop();
    releaseElevatedInstall(elevatedProcessHandle_);
    elevatedProcessHandle_ = nullptr;
    const ElevatedMode mode = elevatedMode_;
    elevatedMode_ = ElevatedMode::None;

    if (exitCode == 0) {
        switch (mode) {
        case ElevatedMode::Install: {
            const UpdateStateRecord pendingUpdate =
                UpdateStateStore::load(QStringLiteral("HarkenStudio"));
            if (pendingUpdate.found &&
                pendingUpdate.buildId == activeBuildId_)
                UpdateStateStore::clear(pendingUpdate);
            finishElevatedFlow(QCoreApplication::translate("HarkenHub", "Install finished"));
            runSelfTest(activeBuildId_);
            break;
        }
        case ElevatedMode::Uninstall:
            finishElevatedFlow(QCoreApplication::translate("HarkenHub", "Uninstall finished"));
            break;
        case ElevatedMode::Repair:
            finishElevatedFlow(QString());
            break;
        default:
            finishElevatedFlow(QString());
            break;
        }
    } else {
        if (mode == ElevatedMode::Install)
            HandoffArea::discard(activeTransactionId_);
        finishElevatedFlow(mode == ElevatedMode::Install
                               ? QCoreApplication::translate("HarkenHub", "Install failed")
                               : mode == ElevatedMode::Uninstall
                                     ? QCoreApplication::translate("HarkenHub", "Uninstall failed")
                                     : QCoreApplication::translate("HarkenHub", "Repair failed"));
        QString failure = mode == ElevatedMode::Install
            ? TransactionDiagnostics::failureMessage(activeTransactionId_, exitCode) : QString();
        if (failure.isEmpty())
            failure = QCoreApplication::translate("HarkenHub",
                "The operation stopped without reporting a reason (exit code %1).\nDiagnostic log: %2")
                    .arg(exitCode).arg(TransactionJournal::filePath());
        QMessageBox::warning(this, QCoreApplication::translate("HarkenHub", "Harken Studio Hub"), failure);
    }
    activeTransactionId_.clear();
    refresh();
    if (mode == ElevatedMode::Repair && exitCode == 0) {
        const bool restored = std::any_of(versions_.begin(), versions_.end(),
            [this](const auto& version) { return version.buildId == activeBuildId_; });
        installStatusLabel_->setText(restored
            ? QCoreApplication::translate("HarkenHub", "Installation files repaired")
            : QCoreApplication::translate("HarkenHub", "The installation still needs attention"));
    }
    activeBuildId_.clear();
    emit launchTargetChanged();
}

void VersionsPage::runSelfTest(const QString& buildId)
{
    if (buildId.isEmpty())
        return;
    QString installPath;
    for (const auto& version : VersionScanner::scanInstalled()) {
        if (version.buildId == buildId) {
            if (version.capabilitiesVersion != kStudioCapabilitiesVersion)
                return;
            installPath = version.installPath;
            break;
        }
    }
    if (installPath.isEmpty())
        return;

    LaunchDiagnostics::record(buildId, QStringLiteral("SelfTesting"),
                              QString(), QDateTime::currentDateTimeUtc());
    const QPointer<VersionsPage> self(this);
    QThreadPool::globalInstance()->start([self, buildId, installPath]() {
        const SelfTestResult result =
            SelfTestRunner::runForInstall(installPath, buildId);
        LaunchDiagnostics::record(
            buildId,
            result.ok ? QStringLiteral("Ready") : QStringLiteral("LaunchIssue"),
            result.ok ? QString() : result.error,
            QDateTime::currentDateTimeUtc());
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, result]() {
                if (self)
                    self->installStatusLabel_->setText(
                        result.ok
                            ? QCoreApplication::translate("HarkenHub", "Install finished · self-test OK")
                            : QCoreApplication::translate("HarkenHub", "Install finished · self-test issue"));
            },
            Qt::QueuedConnection);
    });
}

void VersionsPage::finishElevatedFlow(const QString& status)
{
    preparing_ = false;
    installStatusLabel_->setText(status);
    busyBar_->hide();
    setManagementButtonsEnabled(true);
}

void VersionsPage::showRowMenu(const InstalledVersion& version,
                               const QPoint& globalPos)
{
    QMenu menu(this);
    menu.setAttribute(Qt::WA_TranslucentBackground);

    QAction* openFolder = menu.addAction(QCoreApplication::translate("HarkenHub", "Open Folder"));
    openFolder->setEnabled(!version.installPath.isEmpty());
    connect(openFolder, &QAction::triggered, this, [version]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(version.installPath));
    });

    if (version.isLocalBuild()) {
        menu.addSeparator();
        QAction* remove = menu.addAction(QCoreApplication::translate("HarkenHub", "Remove from Hub"));
        remove->setEnabled(!operationBusy());
        connect(remove, &QAction::triggered, this,
                [this, version]() { removeLocalBuild(version); });
        menu.exec(globalPos);
        return;
    }

    const QString pinnedBuildId = StudioLaunchTarget::pinnedBuildId();
    const InstalledVersion* target =
        StudioLaunchTarget::pick(versions_, pinnedBuildId, {});
    const bool pinned = pinnedBuildId == version.buildId;
    QAction* setDefault =
        menu.addAction(QCoreApplication::translate("HarkenHub", "Set as Launch Harken Default"));
    setDefault->setVisible(false);
    setDefault->setCheckable(true);
    setDefault->setChecked(target && target->buildId == version.buildId);
    connect(setDefault, &QAction::triggered, this, [this, version, pinned]() {
        StudioLaunchTarget::setPinnedBuildId(pinned ? QString()
                                                    : version.buildId);
        emit launchTargetChanged();
    });

    menu.addSeparator();

    QAction* uninstall = menu.addAction(QCoreApplication::translate("HarkenHub", "Uninstall"));
    uninstall->setEnabled(!operationBusy());
    connect(uninstall, &QAction::triggered, this,
            [this, version]() { beginUninstall(version); });

    menu.exec(globalPos);
}

}
