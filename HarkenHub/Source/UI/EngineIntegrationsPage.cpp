#include "UI/EngineIntegrationsPage.h"

#include "UI/HubIcons.h"
#include "UI/IntegrationTaskDialog.h"

#include "Integrations/EngineTargetProbe.h"
#include "Integrations/EngineTargetStore.h"
#include "Integrations/HubUnityIntegrationService.h"
#include "Integrations/IntegrationStore.h"
#include "Integrations/ProjectIntegrationExecutor.h"
#include "Launch/LaunchPreflight.h"
#include "Platform/PlatformPaths.h"
#include "Projects/ProjectProbe.h"
#include "Projects/ProjectStore.h"
#include "Transactions/InstallIndex.h"
#include "UI/BoundedHeaderView.h"
#include "UI/NoHoverItemDelegate.h"
#include "UI/TableSorting.h"
#include "Versions/VersionScanner.h"
#include "Integrations/Unity/UnityInstallation.h"
#include "Integrations/Unity/UnityProjectAccess.h"
#include "Integrations/Unity/UnityIntegrationService.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCoreApplication>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QFrame>
#include <QHash>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QScopeGuard>
#include <QScrollArea>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextBrowser>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUuid>
#include <QUrl>
#include <QVBoxLayout>
#include <QVersionNumber>

#include <algorithm>
#include <functional>
#include <utility>

namespace VNH {
namespace {

constexpr int kHarkenProjectColumn = 0;
constexpr int kEngineProjectColumn = 1;
constexpr int kEngineColumn = 2;
constexpr int kSdkStatusColumn = 3;
constexpr int kSdkVersionColumn = 4;
constexpr int kActionColumn = 5;
constexpr int kPathButtonMinimumWidth = 126;
constexpr int kProjectFolderRole = Qt::UserRole + 1;

void addProjectFolderAction(QMenu& menu, int column, const QString& path)
{
    auto* action = menu.addAction(column == kHarkenProjectColumn
        ? QCoreApplication::translate("HarkenHub", "Open Harken Project Folder")
        : QCoreApplication::translate("HarkenHub", "Open Game Project Folder"));
    action->setToolTip(QDir::toNativeSeparators(path));
    action->setEnabled(!path.isEmpty() && QFileInfo(path).isDir());
    QObject::connect(action, &QAction::triggered, &menu, [path]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });
}

class SdkSourceComboBox final : public QComboBox
{
public:
    SdkSourceComboBox(QWidget* parent, std::function<void()> refreshSources)
        : QComboBox(parent), refreshSources_(std::move(refreshSources)) {}

    void showPopup() override
    {
        refreshSources_();
        QComboBox::showPopup();
    }

private:
    std::function<void()> refreshSources_;
};

bool hasPendingMigration(const EngineTargetRecord& target)
{
    return target.engineKind == EngineKind::Unity
        ? VNX::UnityInstallation::hasPendingMigration(target.canonicalRoot)
        : ProjectIntegrationExecutor::hasPendingUnrealMigration(target);
}

QString normalizedPath(const QString& path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty()
                               ? info.absoluteFilePath()
                               : canonical);
}

bool samePath(const QString& left, const QString& right)
{
#ifdef Q_OS_WIN
    return normalizedPath(left).compare(normalizedPath(right),
                                        Qt::CaseInsensitive) == 0;
#else
    return normalizedPath(left) == normalizedPath(right);
#endif
}

QString healthText(EngineTargetHealth health)
{
    switch (health) {
        case EngineTargetHealth::Missing:
            return QCoreApplication::translate("HarkenHub", "Target missing");
        case EngineTargetHealth::NotInstalled:
            return QCoreApplication::translate("HarkenHub", "Not installed");
        case EngineTargetHealth::Managed:
            return QCoreApplication::translate("HarkenHub", "Managed");
        case EngineTargetHealth::Unmanaged:
            return QCoreApplication::translate("HarkenHub", "Unmanaged");
        case EngineTargetHealth::Incomplete:
            return QCoreApplication::translate("HarkenHub", "Incomplete");
        case EngineTargetHealth::Unreadable:
            return QCoreApplication::translate("HarkenHub", "Unreadable");
    }
    return {};
}

QString engineName(EngineKind kind)
{
    return kind == EngineKind::Unity ? QStringLiteral("Unity")
                                     : QStringLiteral("Unreal");
}

QString sdkContentText(SdkContentState state)
{
    switch (state) {
        case SdkContentState::MatchesInstallation:
            return QCoreApplication::translate("HarkenHub", "Files match installation");
        case SdkContentState::Modified:
            return QCoreApplication::translate("HarkenHub", "Files modified");
        case SdkContentState::Missing:
            return QCoreApplication::translate("HarkenHub", "Files missing");
        case SdkContentState::ModifiedAndMissing:
            return QCoreApplication::translate("HarkenHub", "Files modified and missing");
        case SdkContentState::Unverified:
            return QCoreApplication::translate("HarkenHub", "File verification incomplete");
        case SdkContentState::NotChecked:
            return {};
    }
    return {};
}

QString versionLabel(const InstalledVersion& version)
{
    QString label = version.semanticVersion;
    const QString buildId = version.launchBuildId();
    if (label.isEmpty())
        label = QCoreApplication::translate("HarkenHub", "Current HarkenStudio");
    if (!buildId.isEmpty())
        label += QStringLiteral(" (%1)").arg(buildId);
    return label;
}

QString sdkVersion(const QString& path, EngineKind kind)
{
    if (path.trimmed().isEmpty()) return {};
    const QString root = kind == EngineKind::Unity
        ? VNX::UnityIntegrationService::locateSdkSource(path)
        : ProjectIntegrationExecutor::locateUnrealSource(path);
    if (root.isEmpty()) return {};
    QFile file(QDir(root).filePath(kind == EngineKind::Unity
        ? QStringLiteral("package.json") : QStringLiteral("Harken.uplugin")));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    return object.value(kind == EngineKind::Unity
        ? QStringLiteral("version") : QStringLiteral("VersionName")).toString();
}

QString studioSdkPath(const InstalledVersion& version, EngineKind kind)
{
    return QDir(QFileInfo(version.executablePath).absolutePath()).filePath(
        kind == EngineKind::Unity ? QStringLiteral("HarkenSDK/Unity/Harken")
                                  : QStringLiteral("HarkenSDK/Unreal/Harken"));
}

struct LocalSdkSource
{
    QString path;
    QString version;
    QString label;
};

QVector<LocalSdkSource> localSdkSources(
    EngineKind kind, const std::vector<InstalledVersion>& versions)
{
    QVector<LocalSdkSource> sources;
    for (const auto& studio : versions) {
        const QString path = studioSdkPath(studio, kind);
        const QString version = sdkVersion(path, kind);
        if (version.isEmpty()) continue;
        sources.push_back({path, version,
            QCoreApplication::translate("HarkenHub", "SDK %1 | Studio %2")
                .arg(version, versionLabel(studio))});
    }
    return sources;
}

std::vector<InstalledVersion> availableStudioVersions()
{
    auto versions = VersionScanner::scanAvailable();
    std::stable_sort(versions.begin(), versions.end(), [](const auto& left, const auto& right) {
        return left.releaseSequence > right.releaseSequence;
    });
    return versions;
}

int compareSdkVersions(const QString& left, const QString& right)
{
    qsizetype leftSuffix = 0;
    qsizetype rightSuffix = 0;
    const auto leftVersion = QVersionNumber::fromString(left, &leftSuffix);
    const auto rightVersion = QVersionNumber::fromString(right, &rightSuffix);
    if (leftVersion.isNull() || rightVersion.isNull()
        || leftSuffix != left.size() || rightSuffix != right.size()) return 0;
    return QVersionNumber::compare(leftVersion, rightVersion);
}

QString targetInputPath(const EngineTargetRecord& target)
{
    return target.engineKind == EngineKind::Unreal
               && !target.descriptorPath.isEmpty()
        ? target.descriptorPath
        : target.canonicalRoot;
}

QString hubTranslate(const QString& source)
{
    const QByteArray utf8 = source.toUtf8();
    return QCoreApplication::translate("HarkenHub", utf8.constData());
}

QString localizeCountSummary(const QString& text)
{
    static const QRegularExpression pattern(
        QStringLiteral("^(\\d+) missing SDK file\\(s\\) will be added; "
                       "(\\d+) existing SDK file\\(s\\) need updating; "
                       "(\\d+) obsolete Harken-managed file\\(s\\) will be removed; "
                       "(\\d+) project-specific file\\(s\\) will be kept unchanged; "
                       "(\\d+) Unity-generated metadata file\\(s\\) and "
                       "(\\d+) optional target runtime file\\(s\\) will be kept automatically$"));
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch())
        return hubTranslate(text);
    return hubTranslate(
               QStringLiteral("%1 missing SDK file(s) will be added; %2 existing SDK file(s) need updating; %3 obsolete Harken-managed file(s) will be removed; %4 project-specific file(s) will be kept unchanged; %5 Unity-generated metadata file(s) and %6 optional target runtime file(s) will be kept automatically"))
        .arg(match.captured(1), match.captured(2), match.captured(3),
             match.captured(4), match.captured(5), match.captured(6));
}

QString localizeIntegrationText(const QString& text)
{
    if (text.isEmpty())
        return {};

    const QString translated = HubUnityIntegrationService::localizeText(text);
    if (translated != text)
        return translated;

    if (text.contains(QLatin1Char('\n'))) {
        const QStringList lines = text.split(QLatin1Char('\n'));
        QStringList localizedLines;
        localizedLines.reserve(lines.size());
        bool changed = false;
        for (const QString& line : lines) {
            const QString localized = localizeIntegrationText(line);
            changed |= localized != line;
            localizedLines.push_back(localized);
        }
        if (changed)
            return localizedLines.join(QLatin1Char('\n'));
    }

    static const QRegularExpression validatedSdk(
        QStringLiteral("^Validated SDK (.+): (\\d+) file\\(s\\); the "
                       "(.+) runtime required for this editor is present\\.(.*)$"));
    const QRegularExpressionMatch validatedMatch = validatedSdk.match(text);
    if (validatedMatch.hasMatch()) {
        QString result = hubTranslate(
                             QStringLiteral("Validated SDK %1: %2 file(s); the %3 runtime required for this editor is present."))
                             .arg(validatedMatch.captured(1),
                                  validatedMatch.captured(2),
                                  validatedMatch.captured(3));
        const QString suffix = validatedMatch.captured(4);
        static const QRegularExpression expectedPlatforms(
            QStringLiteral("^ Target runtimes not included: (.+)\\. Available platforms can still be integrated\\.$"));
        static const QRegularExpression otherHost(
            QStringLiteral("^ Runtimes that require another editor operating system are not included: (.+)\\.$"));
        const QRegularExpressionMatch expectedMatch = expectedPlatforms.match(suffix);
        if (expectedMatch.hasMatch()) {
            result += hubTranslate(
                          QStringLiteral(" Target runtimes not included: %1. Available platforms can still be integrated."))
                          .arg(expectedMatch.captured(1));
        } else {
            const QRegularExpressionMatch otherMatch = otherHost.match(suffix);
            if (otherMatch.hasMatch())
                result += hubTranslate(
                              QStringLiteral(" Runtimes that require another editor operating system are not included: %1."))
                              .arg(otherMatch.captured(1));
        }
        return result;
    }

    static const QRegularExpression countCheck(
        QStringLiteral("^Check found SDK differences: (.+)\\.$"));
    const QRegularExpressionMatch countMatch = countCheck.match(text);
    if (countMatch.hasMatch())
        return hubTranslate(QStringLiteral("Check found SDK differences: %1.") )
            .arg(localizeCountSummary(countMatch.captured(1)));

    static const QRegularExpression existingDiff(
        QStringLiteral("^The existing SDK differs from selected SDK source (.+): (.+)\\.$"));
    const QRegularExpressionMatch existingMatch = existingDiff.match(text);
    if (existingMatch.hasMatch())
        return hubTranslate(QStringLiteral("The existing SDK differs from selected SDK source %1: %2."))
            .arg(existingMatch.captured(1), localizeCountSummary(existingMatch.captured(2)));

    static const QRegularExpression approvedUpdate(
        QStringLiteral("^Approved SDK update: (.+)\\.$"));
    const QRegularExpressionMatch approvedMatch = approvedUpdate.match(text);
    if (approvedMatch.hasMatch())
        return hubTranslate(QStringLiteral("Approved SDK update: %1."))
            .arg(localizeCountSummary(approvedMatch.captured(1)));

    static const QRegularExpression fileCount(
        QStringLiteral("^No embedded Harken SDK is installed; (\\d+) file\\(s\\) are ready to install\\.$"));
    const QRegularExpressionMatch fileCountMatch = fileCount.match(text);
    if (fileCountMatch.hasMatch())
        return hubTranslate(QStringLiteral("No embedded Harken SDK is installed; %1 file(s) are ready to install."))
            .arg(fileCountMatch.captured(1));

    static const QRegularExpression skippedStep(
        QStringLiteral("^Not run because (.+) failed\\.$"));
    const QRegularExpressionMatch skippedMatch = skippedStep.match(text);
    if (skippedMatch.hasMatch())
        return hubTranslate(QStringLiteral("Not run because %1 failed."))
            .arg(skippedMatch.captured(1));

    static const QRegularExpression groupedLocation(
        QStringLiteral("^Location: (.+) — (\\d+) files \\((.+)\\)$"));
    const QRegularExpressionMatch locationMatch = groupedLocation.match(text);
    if (locationMatch.hasMatch())
        return hubTranslate(QStringLiteral("Location: %1 — %2 files (%3)"))
            .arg(locationMatch.captured(1), locationMatch.captured(2),
                 locationMatch.captured(3));

    static const QRegularExpression changeGroup(
        QStringLiteral("^(Files to add|Files to update|Obsolete files to remove|Files kept unchanged) — (.+) \\((\\d+) files\\)$"));
    const QRegularExpressionMatch changeGroupMatch = changeGroup.match(text);
    if (changeGroupMatch.hasMatch())
        return hubTranslate(QStringLiteral("%1 — %2 (files: %3)"))
            .arg(hubTranslate(changeGroupMatch.captured(1)),
                 hubTranslate(changeGroupMatch.captured(2)),
                 changeGroupMatch.captured(3));

    static const QRegularExpression location(
        QStringLiteral("^Location: (.+)$"));
    const QRegularExpressionMatch singleLocation = location.match(text);
    if (singleLocation.hasMatch())
        return hubTranslate(QStringLiteral("Location: %1"))
            .arg(singleLocation.captured(1));

    static const QRegularExpression labeled(
        QStringLiteral("^(Purpose|Planned change|Impact): (.+)$"));
    const QRegularExpressionMatch labeledMatch = labeled.match(text);
    if (labeledMatch.hasMatch()) {
        return hubTranslate(QStringLiteral("%1: %2"))
            .arg(hubTranslate(labeledMatch.captured(1)),
                 hubTranslate(labeledMatch.captured(2)));
    }

    static const QRegularExpression projectProbe(
        QStringLiteral("^(No \\.hsp project file found at|Not a \\.hsp project file|Cannot read project file|Invalid project file): (.+)$"));
    const QRegularExpressionMatch projectProbeMatch = projectProbe.match(text);
    if (projectProbeMatch.hasMatch()) {
        static const QHash<QString, QString> templates = {
            {QStringLiteral("No .hsp project file found at"),
             QStringLiteral("No .hsp project file found at: %1")},
            {QStringLiteral("Not a .hsp project file"),
             QStringLiteral("Not a .hsp project file: %1")},
            {QStringLiteral("Cannot read project file"),
             QStringLiteral("Cannot read project file: %1")},
            {QStringLiteral("Invalid project file"),
             QStringLiteral("Invalid project file: %1")},
        };
        const auto found = templates.constFind(projectProbeMatch.captured(1));
        if (found != templates.cend())
            return hubTranslate(found.value()).arg(projectProbeMatch.captured(2));
    }

    static const QHash<QString, QString> auxiliaryErrors = {
        {QStringLiteral("Engine target output is unavailable"),
         QStringLiteral("Engine target output is unavailable")},
        {QStringLiteral("The selected directory is not a Unity project"),
         QStringLiteral("The selected directory is not a Unity project")},
        {QStringLiteral("The Unreal project directory must contain exactly one .uproject file"),
         QStringLiteral("The Unreal project directory must contain exactly one .uproject file")},
        {QStringLiteral("The engine project path cannot be resolved"),
         QStringLiteral("The engine project path cannot be resolved")},
        {QStringLiteral("The target is not a readable Unreal project"),
         QStringLiteral("The target is not a readable Unreal project")},
        {QStringLiteral("The Unreal SDK source is incomplete"),
         QStringLiteral("The Unreal SDK source is incomplete")},
        {QStringLiteral("The Unreal SDK version is unavailable"),
         QStringLiteral("The Unreal SDK version is unavailable")},
        {QStringLiteral("The Unreal plugin is already installed"),
         QStringLiteral("The Unreal plugin is already installed")},
        {QStringLiteral("The Unreal plugin is not installed"),
         QStringLiteral("The Unreal plugin is not installed")},
        {QStringLiteral("The existing Unreal plugin has no trusted ownership record"),
         QStringLiteral("The existing Unreal plugin has no trusted ownership record")},
        {QStringLiteral("Integration plan is invalid"),
         QStringLiteral("Integration plan is invalid")},
        {QStringLiteral("Integration operation requires confirmation"),
         QStringLiteral("Integration operation requires confirmation")},
        {QStringLiteral("The Unreal Plugins directory is not writable"),
         QStringLiteral("The Unreal Plugins directory is not writable")},
        {QStringLiteral("The existing Unreal plugin cannot be backed up"),
         QStringLiteral("The existing Unreal plugin cannot be backed up")},
        {QStringLiteral("The detached Unreal plugin cannot be removed"),
         QStringLiteral("The detached Unreal plugin cannot be removed")},
        {QStringLiteral("The Unreal plugin staging transaction failed"),
         QStringLiteral("The Unreal plugin staging transaction failed")},
        {QStringLiteral("The Unreal plugin cannot be activated"),
         QStringLiteral("The Unreal plugin cannot be activated")},
        {QStringLiteral("Integration is incomplete"),
         QStringLiteral("Integration is incomplete")},
        {QStringLiteral("The Harken project already has an integration"),
         QStringLiteral("The Harken project already has an integration")},
        {QStringLiteral("The engine project already has an integration"),
         QStringLiteral("The engine project already has an integration")},
        {QStringLiteral("Integration is unavailable"),
         QStringLiteral("Integration is unavailable")},
        {QStringLiteral("Store is not open"),
         QStringLiteral("Store is not open")},
        {QStringLiteral("Project is already in the list"),
         QStringLiteral("Project is already in the list")},
        {QStringLiteral("Engine target is incomplete"),
         QStringLiteral("Engine target is incomplete")},
        {QStringLiteral("Cannot create integration store directory"),
         QStringLiteral("Cannot create integration store directory")},
    };
    const auto auxiliary = auxiliaryErrors.constFind(text);
    if (auxiliary != auxiliaryErrors.cend())
        return hubTranslate(auxiliary.value());

    return text;
}

QString localizeUnityStepTitle(const QString& id, const QString& fallback)
{
    static const QHash<QString, QString> titles = {
        {QStringLiteral("unity_preflight"), QStringLiteral("Validate Unity project")},
        {QStringLiteral("sdk_preflight"), QStringLiteral("Validate Harken Unity SDK source")},
        {QStringLiteral("compare_sdk"), QStringLiteral("Compare installed Unity SDK")},
        {QStringLiteral("install_sdk"), QStringLiteral("Install or update Unity SDK")},
        {QStringLiteral("verify"), QStringLiteral("Verify installed SDK")},
        {QStringLiteral("report"), QStringLiteral("Write integration report")},
        {QStringLiteral("inspect_installation"), QStringLiteral("Review installed Harken SDK")},
        {QStringLiteral("remove_integration"), QStringLiteral("Back up and remove Harken SDK")},
        {QStringLiteral("removal_report"), QStringLiteral("Write removal report")},
    };
    const auto found = titles.constFind(id);
    return hubTranslate(found == titles.cend() ? fallback : found.value());
}

QString localizeFileChangeAction(const QString& action)
{
    static const QHash<QString, QString> actions = {
        {QStringLiteral("add"), QStringLiteral("Files to add")},
        {QStringLiteral("replace"), QStringLiteral("Files to update")},
        {QStringLiteral("remove"), QStringLiteral("Obsolete files to remove")},
        {QStringLiteral("preserve"), QStringLiteral("Files kept unchanged")},
    };
    const auto found = actions.constFind(action);
    return hubTranslate(found == actions.cend() ? action : found.value());
}

bool confirmIntegrationAction(
    QWidget* parent, const QString& title, const QString& message,
    const QString& details, const QString& approveText,
    QDialogButtonBox::ButtonRole approveRole)
{
    QDialog confirmation(parent);
    confirmation.setWindowTitle(title);
    confirmation.setWindowIcon(parent->windowIcon());
    confirmation.setWindowFlag(Qt::WindowMaximizeButtonHint, true);
    confirmation.setSizeGripEnabled(true);
    confirmation.setMinimumSize(640, 420);
    confirmation.resize(900, 620);

    auto* layout = new QVBoxLayout(&confirmation);
    auto* messageLayout = new QHBoxLayout();
    auto* icon = new QLabel(&confirmation);
    icon->setPixmap(QApplication::style()
                        ->standardIcon(QStyle::SP_MessageBoxWarning)
                        .pixmap(40, 40));
    icon->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    messageLayout->addWidget(icon, 0, Qt::AlignTop);
    auto* messageLabel = new QLabel(message, &confirmation);
    messageLabel->setWordWrap(true);
    messageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    messageLayout->addWidget(messageLabel, 1);
    layout->addLayout(messageLayout);

    auto* detailView = new QPlainTextEdit(&confirmation);
    detailView->setReadOnly(true);
    detailView->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    detailView->setPlainText(details);
    layout->addWidget(detailView, 1);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Cancel, &confirmation);
    auto* approve = buttons->addButton(approveText, approveRole);
    QObject::connect(approve, &QPushButton::clicked,
                     &confirmation, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected,
                     &confirmation, &QDialog::reject);
    layout->addWidget(buttons);
    return confirmation.exec() == QDialog::Accepted;
}

}

EngineIntegrationsPage::EngineIntegrationsPage(
    IntegrationStore* integrations, EngineTargetStore* targets,
    ProjectStore* projects, QWidget* parent)
    : QWidget(parent)
    , integrations_(integrations)
    , targets_(targets)
    , projects_(projects)
{
    listPage_ = new QWidget(this);
    auto* listHeader = new QHBoxLayout();
    auto* title = new QLabel(QCoreApplication::translate("HarkenHub", "Engine Integrations"), listPage_);
    title->setObjectName(QStringLiteral("pageTitle"));
    listHeader->addWidget(title, 0, Qt::AlignBottom);
    listHeader->addStretch();
    auto* editButton = new QPushButton(QCoreApplication::translate("HarkenHub", "Manage Integration"), listPage_);
    editButton->setObjectName(QStringLiteral("addIntegrationButton"));
    editButton->setText(QStringLiteral("+"));
    editButton->setFixedSize(34, 34);
    editButton->setToolTip(QCoreApplication::translate("HarkenHub", "Manage Integration"));
    editButton->setAccessibleName(QCoreApplication::translate("HarkenHub", "Manage Integration"));
    connect(editButton, &QPushButton::clicked, this,
            [this]() { beginNewEditor(); });
    listHeader->addWidget(editButton);

    table_ = new QTableWidget(listPage_);
    table_->setObjectName(QStringLiteral("engineIntegrationsTable"));
    table_->setColumnCount(6);
    table_->setHorizontalHeaderLabels({
        QCoreApplication::translate("HarkenHub", "HARKEN PROJECT"),
        QCoreApplication::translate("HarkenHub", "GAME PROJECT"),
        QCoreApplication::translate("HarkenHub", "ENGINE"),
        QCoreApplication::translate("HarkenHub", "SDK STATUS"),
        QCoreApplication::translate("HarkenHub", "SDK VERSION"),
        QString(),
    });
    auto* boundedHeader = new BoundedHeaderView(Qt::Horizontal, table_);
    boundedHeader->setFillSection(kSdkVersionColumn);
    table_->setHorizontalHeader(boundedHeader);
    table_->horizontalHeader()->setHighlightSections(false);
    sorting_ = new TableSorting(table_, QStringLiteral("engineIntegrations"), kEngineColumn);
    table_->horizontalHeader()->setDefaultAlignment(
        Qt::AlignLeft | Qt::AlignVCenter);
    table_->horizontalHeader()->setSectionResizeMode(
        kHarkenProjectColumn, QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(
        kEngineProjectColumn, QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(
        kEngineColumn, QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(
        kSdkStatusColumn, QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(
        kSdkVersionColumn, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(
        kActionColumn, QHeaderView::Fixed);
    table_->horizontalHeader()->setMinimumSectionSize(70);
    table_->setColumnWidth(kHarkenProjectColumn, 180);
    table_->setColumnWidth(kEngineProjectColumn, 200);
    table_->setColumnWidth(kEngineColumn, 80);
    table_->setColumnWidth(kSdkStatusColumn, 130);
    table_->setColumnWidth(kSdkVersionColumn, 190);
    table_->setColumnWidth(kActionColumn, 70);
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(58);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setFocusPolicy(Qt::NoFocus);
    table_->setShowGrid(false);
    table_->setItemDelegateForColumn(
        kHarkenProjectColumn, new NoHoverItemDelegate(table_));
    table_->setItemDelegateForColumn(
        kEngineProjectColumn, new NoHoverItemDelegate(table_));
    table_->setItemDelegateForColumn(
        kEngineColumn, new NoHoverItemDelegate(table_));
    table_->setItemDelegateForColumn(
        kSdkStatusColumn, new NoHoverItemDelegate(table_));
    table_->setItemDelegateForColumn(
        kSdkVersionColumn, new NoHoverItemDelegate(table_));
    table_->setItemDelegateForColumn(
        kActionColumn, new NoHoverItemDelegate(table_));
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QTableWidget::customContextMenuRequested, this,
            [this](const QPoint& position) {
                if (operationRunning_) return;
                const auto* item = table_->itemAt(position);
                if (!item || (item->column() != kHarkenProjectColumn
                              && item->column() != kEngineProjectColumn)) return;
                QMenu popup(this);
                popup.setToolTipsVisible(true);
                addProjectFolderAction(popup, item->column(),
                                       item->data(kProjectFolderRole).toString());
                popup.exec(table_->viewport()->mapToGlobal(position));
            });

    auto* panel = new QFrame(listPage_);
    panel->setObjectName(QStringLiteral("tablePanel"));
    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(12, 4, 12, 8);
    panelLayout->addWidget(table_);
    auto* listLayout = new QVBoxLayout(listPage_);
    listLayout->setContentsMargins(24, 20, 24, 20);
    listLayout->addLayout(listHeader);
    listLayout->addSpacing(14);
    storageErrorLabel_ = new QLabel(listPage_);
    storageErrorLabel_->setObjectName(QStringLiteral("integrationStorageError"));
    storageErrorLabel_->setWordWrap(true);
    storageErrorLabel_->hide();
    listLayout->addWidget(storageErrorLabel_);
    listLayout->addWidget(panel);

    auto* editorScroll = new QScrollArea(this);
    editorPage_ = editorScroll;
    editorScroll->setWidgetResizable(true);
    editorScroll->setFrameShape(QFrame::NoFrame);
    auto* editorContent = new QWidget(editorScroll);
    editorScroll->setWidget(editorContent);
    auto* editorLayout = new QVBoxLayout(editorContent);
    editorLayout->setContentsMargins(24, 20, 24, 20);
    editorLayout->setSpacing(14);
    auto* editorHeader = new QHBoxLayout();
    auto* back = new QPushButton(QCoreApplication::translate("HarkenHub", "Back"), editorPage_);
    backButton_ = back;
    connect(back, &QPushButton::clicked, this,
            &EngineIntegrationsPage::showList);
    editorHeader->addWidget(back);
    editorHeader->addStretch();
    editorLayout->addLayout(editorHeader);

    auto* formPanel = new QWidget(editorPage_);
    integrationForm_ = formPanel;
    auto* form = new QVBoxLayout(formPanel);
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(10);

    auto* relationshipHeader = new QHBoxLayout();
    auto* relationshipTitle = new QLabel(QCoreApplication::translate("HarkenHub", "Project Association"), formPanel);
    const QString sectionStyle = QStringLiteral("font-size: 14px; font-weight: 600;");
    relationshipTitle->setStyleSheet(sectionStyle);
    relationshipHeader->addWidget(relationshipTitle);
    relationshipLabel_ = new QLabel(formPanel);
    relationshipLabel_->setObjectName(QStringLiteral("projectLinkStatus"));
    relationshipHeader->addStretch();
    linkProjectsButton_ = new QPushButton(QCoreApplication::translate("HarkenHub", "Link Projects..."), formPanel);
    linkProjectsButton_->setObjectName(QStringLiteral("linkProjects"));
    unlinkProjectsAction_ = new QAction(QCoreApplication::translate("HarkenHub", "Unlink Projects..."), this);
    unlinkProjectsAction_->setObjectName(QStringLiteral("unlinkProjects"));
    unlinkProjectsButton_ = new QPushButton(unlinkProjectsAction_->text(), formPanel);
    unlinkProjectsButton_->setObjectName(QStringLiteral("unlinkProjectsButton"));
    connect(unlinkProjectsButton_, &QPushButton::clicked, unlinkProjectsAction_, &QAction::trigger);
    form->addLayout(relationshipHeader);

    changeSdkButton_ = new QPushButton(QCoreApplication::translate("HarkenHub", "Change SDK..."), formPanel);
    changeSdkButton_->setObjectName(QStringLiteral("changeSdk"));
    migrateSdkButton_ = new QPushButton(QCoreApplication::translate("HarkenHub", "Move SDK..."), formPanel);
    migrateSdkButton_->setObjectName(QStringLiteral("changeSdkLocation"));
    removeInstalledSdkButton_ = new QPushButton(QCoreApplication::translate("HarkenHub", "Uninstall SDK..."), formPanel);
    removeInstalledSdkButton_->setObjectName(QStringLiteral("uninstallSdk"));

    auto makeRow = [formPanel](const QString& labelText) {
        auto* rowWidget = new QWidget(formPanel);
        auto* row = new QHBoxLayout(rowWidget);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        auto* label = new QLabel(labelText, rowWidget);
        label->setFixedWidth(150);
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        row->addWidget(label);
        return std::pair<QWidget*, QHBoxLayout*>(rowWidget, row);
    };

    auto [engineRowWidget, engineRow] = makeRow(
        QCoreApplication::translate("HarkenHub", "Game Project"));
    auto* engineFieldWidget = new QWidget(engineRowWidget);
    auto* engineField = new QHBoxLayout(engineFieldWidget);
    engineField->setContentsMargins(0, 0, 0, 0);
    engineField->setSpacing(8);
    engineProjectEdit_ = new QLineEdit(engineFieldWidget);
    engineProjectEdit_->setPlaceholderText(
        QCoreApplication::translate("HarkenHub", "Select a Unity project"));
    engineProjectEdit_->setObjectName(QStringLiteral("engineProjectPath"));
    engineBrowseButton_ = new QPushButton(
        QCoreApplication::translate("HarkenHub", "Select Game Project"), engineFieldWidget);
    engineField->addWidget(engineProjectEdit_, 1);
    engineField->addWidget(engineBrowseButton_);
    engineRow->addWidget(engineFieldWidget, 1);
    form->addWidget(engineRowWidget);

    auto [harkenRowWidget, harkenPathRow] = makeRow(
        QCoreApplication::translate("HarkenHub", "Harken Project"));
    harkenProjectEdit_ = new QLineEdit(harkenRowWidget);
    harkenProjectEdit_->setPlaceholderText(
        QCoreApplication::translate("HarkenHub", "Select a .hsp project file"));
    harkenBrowseButton_ = new QPushButton(
        QCoreApplication::translate("HarkenHub", "Browse..."), harkenRowWidget);
    harkenPathRow->addWidget(harkenProjectEdit_, 1);
    harkenPathRow->addWidget(harkenBrowseButton_);
    form->addWidget(harkenRowWidget);

    auto* relationshipActions = new QHBoxLayout();
    relationshipActions->addWidget(relationshipLabel_);
    relationshipActions->addStretch();
    relationshipActions->addWidget(linkProjectsButton_);
    relationshipActions->addWidget(unlinkProjectsButton_);
    form->addLayout(relationshipActions);
    const QString associationHint = QCoreApplication::translate("HarkenHub", "Linking adds this project pair to Engine Integrations and establishes a one-to-one relationship in this Hub. Unlink the pair before linking either project to another. SDK checks and management do not require linking.");
    auto* associationLabel = new QLabel(associationHint, formPanel);
    associationLabel->setObjectName(QStringLiteral("projectAssociationHint"));
    associationLabel->setWordWrap(true);
    associationLabel->setStyleSheet(QStringLiteral("color: #AAB2C0; font-size: 12px;"));
    form->addWidget(associationLabel);
    linkProjectsButton_->setToolTip(QCoreApplication::translate("HarkenHub", "Add this project pair to Engine Integrations. No project or SDK files are changed."));
    unlinkProjectsButton_->setToolTip(QCoreApplication::translate("HarkenHub", "Remove this project pair from Engine Integrations. Project files and the installed SDK are kept."));

    form->addSpacing(8);
    const QString separatorStyle = QStringLiteral("background-color: #313848; border: none;");
    auto* separator = new QFrame(formPanel);
    separator->setObjectName(QStringLiteral("integrationAssociationSeparator"));
    separator->setFrameShape(QFrame::NoFrame);
    separator->setFixedHeight(1);
    separator->setStyleSheet(separatorStyle);
    form->addWidget(separator);
    auto* sdkHeader = new QHBoxLayout();
    sdkHeading_ = new QLabel(QStringLiteral("SDK"), formPanel);
    sdkHeading_->setObjectName(QStringLiteral("sdkHeading"));
    sdkHeading_->setStyleSheet(sectionStyle);
    sdkHeader->addWidget(sdkHeading_);
    sdkHeader->addStretch();
    form->addLayout(sdkHeader);
    auto [installedPathRowWidget, installedPathRow] = makeRow(
        QCoreApplication::translate("HarkenHub", "Current SDK Location"));
    installedSdkPath_ = new QLineEdit(installedPathRowWidget);
    installedSdkPath_->setObjectName(QStringLiteral("installedSdkPath"));
    installedSdkPath_->setReadOnly(true);
    installedPathRow->addWidget(installedSdkPath_, 1);
    installedSdkLabel_ = new QLabel(installedPathRowWidget);
    installedSdkLabel_->setObjectName(QStringLiteral("installedSdkVersion"));
    installedPathRow->addWidget(installedSdkLabel_);
    form->addWidget(installedPathRowWidget);

    auto* sdkActions = new QHBoxLayout();
    checkButton_ = new QPushButton(QCoreApplication::translate("HarkenHub", "Check Current SDK Status"), formPanel);
    checkButton_->setObjectName(QStringLiteral("verifySdk"));
    sdkActions->addWidget(checkButton_);
    sdkActions->addStretch();
    sdkActions->addWidget(changeSdkButton_);
    sdkActions->addWidget(migrateSdkButton_);
    sdkActions->addWidget(removeInstalledSdkButton_);
    form->addLayout(sdkActions);
    sdkIssueRow_ = new QWidget(formPanel);
    auto* issueLayout = new QHBoxLayout(sdkIssueRow_);
    issueLayout->setContentsMargins(0, 0, 0, 0);
    sdkIssueLabel_ = new QLabel(sdkIssueRow_);
    sdkIssueLabel_->setObjectName(QStringLiteral("sdkIssueHint"));
    sdkIssueLabel_->setWordWrap(true);
    sdkIssueLabel_->setStyleSheet(QStringLiteral("color: #F0C36A;"));
    issueLayout->addWidget(sdkIssueLabel_, 1);
    sdkResolutionButton_ = new QPushButton(QCoreApplication::translate("HarkenHub", "View Resolution..."), sdkIssueRow_);
    sdkResolutionButton_->setObjectName(QStringLiteral("sdkResolution"));
    issueLayout->addWidget(sdkResolutionButton_, 0, Qt::AlignTop);
    connect(sdkResolutionButton_, &QPushButton::clicked, this, &EngineIntegrationsPage::showSdkResolution);
    form->addWidget(sdkIssueRow_);
    sdkIssueRow_->hide();

    sdkSelectionDialog_ = new QDialog(this);
    sdkSelectionDialog_->setObjectName(QStringLiteral("sdkSelectionDialog"));
    sdkSelectionDialog_->setWindowIcon(windowIcon());
    sdkSelectionDialog_->setWindowFlag(Qt::WindowMaximizeButtonHint, true);
    sdkSelectionDialog_->setSizeGripEnabled(true);
    sdkSelectionDialog_->setMinimumSize(640, 360);
    sdkSelectionDialog_->resize(820, 600);
    sdkSelectionDialog_->installEventFilter(this);
    auto* dialogLayout = new QVBoxLayout(sdkSelectionDialog_);
    dialogLayout->setContentsMargins(20, 20, 20, 20);
    dialogLayout->setSpacing(14);
    sdkSelectionRow_ = new QWidget(sdkSelectionDialog_);
    sdkSelectionRow_->setObjectName(QStringLiteral("sdkSelectionForm"));
    auto* selectionLayout = new QVBoxLayout(sdkSelectionRow_);
    selectionLayout->setContentsMargins(0, 0, 0, 0);
    selectionLayout->setSpacing(8);
    auto [sdkRowWidget, sdkPathRow] = makeRow(
        QCoreApplication::translate("HarkenHub", "SDK Source Directory"));
    sdkSourceCombo_ = new SdkSourceComboBox(sdkRowWidget, [this]() {
        if (!operationRunning_) refreshLocalSdkSources();
    });
    sdkSourceCombo_->setObjectName(QStringLiteral("sdkSourceSelector"));
    sdkSourceCombo_->installEventFilter(this);
    sdkSourceCombo_->setEditable(true);
    sdkSourceCombo_->setInsertPolicy(QComboBox::NoInsert);
    sdkSourceCombo_->setCompleter(nullptr);
    sdkSourceCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    sdkPathEdit_ = sdkSourceCombo_->lineEdit();
    sdkPathEdit_->setObjectName(QStringLiteral("sdkSourcePath"));
    sdkPathEdit_->setPlaceholderText(
        QCoreApplication::translate("HarkenHub", "Runtime/SDK source path"));
    sdkPathEdit_->setReadOnly(true);
    sdkPathEdit_->setCursor(Qt::ArrowCursor);
    sdkPathEdit_->installEventFilter(this);
    sdkSourceBrowseButton_ = new QPushButton(
        QCoreApplication::translate("HarkenHub", "Choose SDK Folder..."), sdkRowWidget);
    sdkSourceBrowseButton_->setObjectName(QStringLiteral("sdkSourceBrowse"));
    sdkPathRow->addWidget(sdkSourceCombo_, 1);
    sdkPathRow->addWidget(sdkSourceBrowseButton_);
    selectionLayout->addWidget(sdkRowWidget);

    auto [installRowWidget, installRow] = makeRow(
        QCoreApplication::translate("HarkenHub", "SDK Install Location"));
    sdkInstallRow_ = installRowWidget;
    sdkInstallPathEdit_ = new QLineEdit(installRowWidget);
    sdkInstallPathEdit_->setObjectName(QStringLiteral("sdkInstallPath"));
    sdkInstallPathEdit_->setPlaceholderText(QStringLiteral("Packages/com.harken.core"));
    sdkInstallBrowseButton_ = new QPushButton(
        QCoreApplication::translate("HarkenHub", "Browse..."), installRowWidget);
    sdkInstallBrowseButton_->setObjectName(QStringLiteral("sdkInstallBrowse"));
    installRow->addWidget(sdkInstallPathEdit_, 1);
    installRow->addWidget(sdkInstallBrowseButton_);
    selectionLayout->addWidget(installRowWidget);

    auto* integrationActionRow = new QHBoxLayout();
    integrationActionRow->setContentsMargins(0, 0, 0, 0);
    sdkCheckButton_ = new QPushButton(QCoreApplication::translate("HarkenHub", "Check SDK Changes"), sdkSelectionDialog_);
    sdkCheckButton_->setObjectName(QStringLiteral("sdkChangeCheck"));
    sdkCheckButton_->setAutoDefault(false);
    integrationActionRow->addWidget(sdkCheckButton_);
    integrationActionRow->addStretch(1);
    integrateButton_ = new QPushButton(
        QCoreApplication::translate("HarkenHub", "Install SDK..."), sdkSelectionDialog_);
    integrateButton_->setObjectName(QStringLiteral("sdkPrimaryAction"));
    integrateButton_->setAutoDefault(false);
    cancelSdkButton_ = new QPushButton(QCoreApplication::translate("HarkenHub", "Cancel"), sdkSelectionDialog_);
    cancelSdkButton_->setAutoDefault(false);
    cancelSdkButton_->setObjectName(QStringLiteral("cancelSdkSelection"));
    const auto editorButtons = {engineBrowseButton_,
                                harkenBrowseButton_, sdkSourceBrowseButton_,
                                sdkInstallBrowseButton_, integrateButton_,
                                linkProjectsButton_, unlinkProjectsButton_};
    int buttonWidth = kPathButtonMinimumWidth;
    for (auto* button : editorButtons) {
        button->ensurePolished();
        buttonWidth = std::max(buttonWidth, button->sizeHint().width());
    }
    for (auto* button : editorButtons)
        button->setMinimumWidth(buttonWidth);
    integrationActionRow->addWidget(cancelSdkButton_);
    integrationActionRow->addWidget(integrateButton_);
    sdkSelectionContext_ = new QLabel(sdkSelectionDialog_);
    sdkSelectionContext_->setObjectName(QStringLiteral("sdkSelectionContext"));
    sdkSelectionContext_->setTextFormat(Qt::PlainText);
    sdkSelectionContext_->setWordWrap(true);
    sdkSelectionContext_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sdkSelectionContext_->hide();
    dialogLayout->addWidget(sdkSelectionContext_);
    dialogLayout->addWidget(sdkSelectionRow_);
    sdkProgress_ = new IntegrationTaskProgress(sdkSelectionDialog_);
    sdkProgress_->setObjectName(QStringLiteral("sdkInlineProgress"));
    sdkProgress_->hide();
    dialogLayout->addWidget(sdkProgress_);
    sdkSelectionSummary_ = new QLabel(sdkSelectionDialog_);
    sdkSelectionSummary_->setObjectName(QStringLiteral("sdkSelectionSummary"));
    sdkSelectionSummary_->setWordWrap(true);
    sdkSelectionSummary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    dialogLayout->addWidget(sdkSelectionSummary_);
    sdkResultsTabs_ = new QTabWidget(sdkSelectionDialog_);
    sdkResultsTabs_->setObjectName(QStringLiteral("sdkCheckResults"));
    auto* changesPage = new QWidget(sdkResultsTabs_);
    auto* changesLayout = new QVBoxLayout(changesPage);
    changesLayout->setContentsMargins(12, 12, 12, 8);
    changesLayout->setSpacing(10);
    sdkDiffSummary_ = new QLabel(changesPage);
    sdkDiffSummary_->setObjectName(QStringLiteral("sdkDiffSummary"));
    sdkDiffSummary_->setWordWrap(true);
    sdkDiffSummary_->setTextFormat(Qt::RichText);
    sdkDiffSummary_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    connect(sdkDiffSummary_, &QLabel::linkActivated, this, [this]() {
        sdkResultsTabs_->setCurrentIndex(1);
    });
    changesLayout->addWidget(sdkDiffSummary_);
    sdkDiffTree_ = new QTreeWidget(changesPage);
    sdkDiffTree_->setObjectName(QStringLiteral("sdkFileChanges"));
    sdkDiffTree_->setHeaderHidden(true);
    sdkDiffTree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    sdkDiffTree_->setUniformRowHeights(true);
    sdkDiffTree_->setStyleSheet(QStringLiteral("QTreeWidget::item { padding: 4px; }"));
    changesLayout->addWidget(sdkDiffTree_, 1);
    sdkResultsTabs_->addTab(changesPage, QCoreApplication::translate("HarkenHub", "File Changes"));
    sdkSelectionDetails_ = new QTextBrowser(sdkResultsTabs_);
    sdkSelectionDetails_->setObjectName(QStringLiteral("sdkSelectionDetails"));
    sdkSelectionDetails_->setOpenLinks(false);
    sdkSelectionDetails_->setOpenExternalLinks(false);
    sdkResultsTabs_->addTab(sdkSelectionDetails_, QCoreApplication::translate("HarkenHub", "Check Log"));
    sdkSelectionPaths_ = new QPlainTextEdit(sdkResultsTabs_);
    sdkSelectionPaths_->setObjectName(QStringLiteral("sdkSelectionPaths"));
    sdkSelectionPaths_->setReadOnly(true);
    sdkResultsTabs_->addTab(sdkSelectionPaths_, QCoreApplication::translate("HarkenHub", "Project and SDK Paths"));
    sdkResultsTabs_->setMinimumHeight(160);
    dialogLayout->addWidget(sdkResultsTabs_, 1);
    sdkSafetyHint_ = new QLabel(sdkSelectionDialog_);
    sdkSafetyHint_->setObjectName(QStringLiteral("sdkEngineRunningHint"));
    sdkSafetyHint_->setWordWrap(true);
    sdkSafetyHint_->setStyleSheet(QStringLiteral("color: #FF6B6B;"));
    sdkSafetyHint_->hide();
    engineStatusTimer_ = new QTimer(this);
    engineStatusTimer_->setInterval(1000);
    connect(engineStatusTimer_, &QTimer::timeout, this, &EngineIntegrationsPage::refreshEngineRunningHint);
    dialogLayout->addWidget(sdkSafetyHint_);
    sdkReportButton_ = new QPushButton(QCoreApplication::translate("HarkenHub", "Open Report"), sdkSelectionDialog_);
    sdkReportButton_->setObjectName(QStringLiteral("sdkOpenReport"));
    sdkReportButton_->setAutoDefault(false);
    sdkReportButton_->hide();
    integrationActionRow->insertWidget(1, sdkReportButton_);
    connect(sdkReportButton_, &QPushButton::clicked, this, [this]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(sdkReportPath_));
    });
    dialogLayout->addLayout(integrationActionRow);
    editorLayout->addWidget(formPanel);

    auto* resultsSeparator = new QFrame(editorPage_);
    resultsSeparator->setObjectName(QStringLiteral("integrationResultsSeparator"));
    resultsSeparator->setFrameShape(QFrame::NoFrame);
    resultsSeparator->setFixedHeight(1);
    resultsSeparator->setStyleSheet(separatorStyle);
    editorLayout->addWidget(resultsSeparator);

    auto* resultsHeading = new QLabel(QCoreApplication::translate("HarkenHub", "Results and Logs"), editorPage_);
    resultsHeading->setObjectName(QStringLiteral("integrationResultsHeading"));
    resultsHeading->setStyleSheet(sectionStyle);
    editorLayout->addWidget(resultsHeading);

    summaryLabel_ = new QLabel(
        QCoreApplication::translate("HarkenHub", "No integration check has run."),
        editorPage_);
    summaryLabel_->setObjectName(QStringLiteral("integrationSummary"));
    summaryLabel_->setWordWrap(true);
    summaryLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    editorLayout->addWidget(summaryLabel_);

    stepsTable_ = new QTableWidget(0, 3, editorPage_);
    stepsTable_->setHorizontalHeaderLabels(
        {QCoreApplication::translate("HarkenHub", "Step"), QCoreApplication::translate("HarkenHub", "Status"), QCoreApplication::translate("HarkenHub", "Result")});
    stepsTable_->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::ResizeToContents);
    stepsTable_->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::ResizeToContents);
    stepsTable_->horizontalHeader()->setSectionResizeMode(
        2, QHeaderView::Stretch);
    stepsTable_->verticalHeader()->hide();
    stepsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    stepsTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    stepsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    stepsTable_->setWordWrap(true);
    stepsTable_->setTextElideMode(Qt::ElideNone);
    stepsTable_->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    stepsTable_->verticalHeader()->setMinimumSectionSize(30);
    detailEdit_ = new QPlainTextEdit(editorPage_);
    detailEdit_->setObjectName(QStringLiteral("integrationDetails"));
    detailEdit_->setReadOnly(true);
    detailEdit_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    detailEdit_->setMaximumBlockCount(2000);
    detailEdit_->setMinimumHeight(130);
    detailEdit_->hide();
    auto* resultsSplitter = new QSplitter(Qt::Vertical, editorPage_);
    resultsSplitter->setObjectName(QStringLiteral("resultsSplitter"));
    resultsSplitter->setChildrenCollapsible(false);
    resultsSplitter->setHandleWidth(8);
    resultsSplitter->addWidget(stepsTable_);
    resultsSplitter->addWidget(detailEdit_);
    resultsSplitter->setStretchFactor(0, 3);
    resultsSplitter->setStretchFactor(1, 2);
    resultsSplitter->setSizes({320, 200});
    auto* resultsPanel = new QFrame(editorPage_);
    resultsPanel->setObjectName(QStringLiteral("tablePanel"));
    auto* resultsPanelLayout = new QVBoxLayout(resultsPanel);
    resultsPanelLayout->setContentsMargins(12, 4, 12, 8);
    resultsPanelLayout->addWidget(resultsSplitter);
    editorLayout->addWidget(resultsPanel, 1);

    stack_ = new QStackedWidget(this);
    stack_->addWidget(listPage_);
    stack_->addWidget(editorPage_);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(stack_);

    connect(harkenBrowseButton_, &QPushButton::clicked, this,
            &EngineIntegrationsPage::browseHarkenProject);
    connect(engineBrowseButton_, &QPushButton::clicked, this,
            &EngineIntegrationsPage::browseEngineProject);
    connect(sdkInstallBrowseButton_, &QPushButton::clicked, this,
            &EngineIntegrationsPage::browseSdkInstallPath);
    connect(sdkInstallPathEdit_, &QLineEdit::textChanged, this, [this]() {
        clearResults();
        setSummaryText(QCoreApplication::translate(
            "HarkenHub", "The installation location changed. Check the integration again."));
    });
    connect(engineProjectEdit_, &QLineEdit::editingFinished, this, [this]() {
        if (!engineProjectEdit_->isReadOnly() && selectedEngineTargetId_.isEmpty()
            && !engineProjectEdit_->text().trimmed().isEmpty())
            acceptEngineTarget(engineProjectEdit_->text().trimmed(), true);
    });
    connect(checkButton_, &QPushButton::clicked, this,
            &EngineIntegrationsPage::runCheck);
    connect(sdkCheckButton_, &QPushButton::clicked, this,
            &EngineIntegrationsPage::runCheck);
    connect(integrateButton_, &QPushButton::clicked, this,
            &EngineIntegrationsPage::runIntegration);
    connect(changeSdkButton_, &QPushButton::clicked, this,
            &EngineIntegrationsPage::beginSdkSelection);
    connect(cancelSdkButton_, &QPushButton::clicked, sdkSelectionDialog_, &QDialog::reject);
    connect(sdkSelectionDialog_, &QDialog::rejected, this,
            &EngineIntegrationsPage::cancelSdkSelection);
    connect(removeInstalledSdkButton_, &QPushButton::clicked, this,
            &EngineIntegrationsPage::runSdkRemoval);
    connect(unlinkProjectsAction_, &QAction::triggered, this, [this]() {
        unlinkProjects(editingIntegrationId_);
    });
    connect(linkProjectsButton_, &QPushButton::clicked, this,
            &EngineIntegrationsPage::linkProjects);
    connect(migrateSdkButton_, &QPushButton::clicked, this,
            &EngineIntegrationsPage::browseSdkInstallPath);
    connect(sdkSourceBrowseButton_, &QPushButton::clicked, this,
            &EngineIntegrationsPage::browseSdkSource);
    connect(harkenProjectEdit_, &QLineEdit::textChanged, this, [this]() {
        selectedHarkenProjectPath_.clear();
        updateStudioVersionFields();
    });
    connect(harkenProjectEdit_, &QLineEdit::editingFinished, this, [this]() {
        if (!harkenProjectEdit_->isReadOnly()
            && !harkenProjectEdit_->text().trimmed().isEmpty()
            && harkenProjectEdit_->text().trimmed() != selectedHarkenProjectPath_) {
            acceptHarkenProject(harkenProjectEdit_->text().trimmed(), true);
        }
    });
    connect(engineProjectEdit_, &QLineEdit::textEdited, this, [this]() {
        selectedEngineTargetId_.clear();
        clearResults();
        updateRemovalButtonState();
    });
    connect(sdkPathEdit_, &QLineEdit::textChanged, this, [this]() {
        sdkPathUserSelected_ = true;
        clearResults();
        updateSdkSourceState();
        setSummaryText(QCoreApplication::translate("HarkenHub", "No integration check has run."));
    });
    connect(stepsTable_, &QTableWidget::currentCellChanged, this,
            &EngineIntegrationsPage::showSelectedStep);

    highlightTimer_ = new QTimer(this);
    highlightTimer_->setInterval(350);
    connect(highlightTimer_, &QTimer::timeout, this,
            &EngineIntegrationsPage::advanceHighlight);
    refresh();
}

EngineIntegrationsPage::~EngineIntegrationsPage()
{
    cancelTargetInspections();
}

void EngineIntegrationsPage::cancelTargetInspections()
{
    for (auto& inspection : targetInspections_) {
        if (inspection.cancelled) inspection.cancelled->store(true, std::memory_order_relaxed);
        inspection.cancelled.reset();
    }
}

EngineTargetObservation EngineIntegrationsPage::requestTargetObservation(
    const EngineTargetRecord& target, bool refresh)
{
    auto& inspection = targetInspections_[target.targetId];
    if (!operationRunning_ && (refresh || (!inspection.ready && !inspection.cancelled))) {
        if (inspection.cancelled) inspection.cancelled->store(true, std::memory_order_relaxed);
        if (!inspection.ready) {
            inspection.observation = {};
            inspection.observation.status = QCoreApplication::translate("HarkenHub", "Checking SDK status...");
        }
        const auto cancelled = std::make_shared<std::atomic<bool>>(false);
        inspection.cancelled = cancelled;
        const QPointer<EngineIntegrationsPage> self(this);
        QThreadPool::globalInstance()->start([self, target, cancelled]() {
            std::atomic<quint64> inspected{0};
            IntegrationProgress progress(inspected, *cancelled);
            EngineTargetObservation observation;
            try {
                IntegrationProgress::checkpoint();
                observation = EngineTargetProbe::observe(target, true);
                IntegrationProgress::checkpoint();
            } catch (const IntegrationScanStopped&) {
                return;
            }
            QMetaObject::invokeMethod(QCoreApplication::instance(), [self, target, cancelled, observation]() {
                if (!self || cancelled->load(std::memory_order_relaxed)) return;
                auto found = self->targetInspections_.find(target.targetId);
                if (found == self->targetInspections_.end() || found->cancelled != cancelled) return;
                found->observation = observation;
                found->ready = true;
                found->cancelled.reset();
                const bool recoverLocation = std::exchange(found->recoverLocation, false);
                self->populateList();
                if (self->selectedEngineTargetId_ == target.targetId)
                    self->updateRemovalButtonState(recoverLocation);
                emit self->sdkStatusChanged(target.targetId);
            }, Qt::QueuedConnection);
        });
    }
    return inspection.observation;
}

bool EngineIntegrationsPage::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == sdkSelectionDialog_) {
        if (event->type() == QEvent::Show) {
            engineRunning_ = false;
            sdkSafetyHint_->hide();
            engineStatusTimer_->start();
            QTimer::singleShot(0, this, &EngineIntegrationsPage::refreshEngineRunningHint);
        } else if (event->type() == QEvent::Hide) {
            engineStatusTimer_->stop();
        }
    }
    if ((watched == sdkSourceCombo_ || watched == sdkPathEdit_)
        && sdkSourceCombo_->isEnabled()
        && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease)
        && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        if (event->type() == QEvent::MouseButtonPress)
            sdkSourceCombo_->setFocus(Qt::MouseFocusReason);
        else
            sdkSourceCombo_->showPopup();
        return true;
    }
    if (watched == sdkSelectionDialog_ && operationRunning_
        && (event->type() == QEvent::Close
            || (event->type() == QEvent::KeyPress
                && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape))) {
        event->ignore();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void EngineIntegrationsPage::refresh()
{
    if (operationRunning_ || listMenuOpen_ || refreshingList_) {
        versionRefreshPending_ = true;
        return;
    }
    const auto targets = targets_->list();
    const auto integrations = integrations_->list();
    inspectionStopped_ = false;
    for (const auto& target : targets) {
        if (target.targetId == selectedEngineTargetId_
            || std::any_of(integrations.begin(), integrations.end(), [&](const auto& record) {
                return record.engineTargetId == target.targetId;
            }))
            requestTargetObservation(target, true);
    }
    populateList();
}

void EngineIntegrationsPage::populateList()
{
    if (operationRunning_ || listMenuOpen_ || refreshingList_) {
        observationRefreshPending_ = true;
        return;
    }
    observationRefreshPending_ = false;
    refreshingList_ = true;
    const auto finishRefresh = qScopeGuard([this]() {
        refreshingList_ = false;
        if (versionRefreshPending_)
            QTimer::singleShot(0, this, &EngineIntegrationsPage::refreshStudioVersion);
    });
    versionRefreshPending_ = false;
    sorting_->beginRefresh();
    const auto selectedRows = table_->selectionModel()->selectedRows(kHarkenProjectColumn);
    const QString selectedId = selectedRows.isEmpty()
        ? QString() : selectedRows.front().data(Qt::UserRole).toString();
    const auto* currentItem = table_->item(table_->currentRow(), kHarkenProjectColumn);
    const QString currentId = currentItem ? currentItem->data(Qt::UserRole).toString() : QString();
    const auto integrations = integrations_->list();
    const auto targets = targets_->list();
    const QString storageError = integrations_->lastError().isEmpty()
        ? targets_->lastError() : integrations_->lastError();
    storageErrorLabel_->setVisible(!storageError.isEmpty());
    storageErrorLabel_->setText(storageError.isEmpty() ? QString()
        : QCoreApplication::translate("HarkenHub", "Integration records could not be read.\n%1").arg(localizeIntegrationText(storageError)));
    const auto projects = projects_->listProjects();
    const auto versions = availableStudioVersions();
    const auto unitySources = localSdkSources(EngineKind::Unity, versions);
    const auto unrealSources = localSdkSources(EngineKind::Unreal, versions);
    table_->clearContents();
    table_->setCurrentCell(-1, -1);
    table_->setRowCount(static_cast<int>(integrations.size()));
    for (int row = 0; row < static_cast<int>(integrations.size()); ++row) {
        const IntegrationRecord& integration =
            integrations[static_cast<size_t>(row)];
        const auto target = std::find_if(
            targets.begin(), targets.end(), [&](const EngineTargetRecord& value) {
                return value.targetId == integration.engineTargetId;
            });
        const auto project = std::find_if(
            projects.begin(), projects.end(), [&](const ProjectRecord& value) {
                return samePath(value.canonicalPath,
                                integration.harkenProjectPath);
            });
        const QString harkenName = project == projects.end()
            ? QFileInfo(integration.harkenProjectPath).completeBaseName()
            : project->displayName;
        auto* harkenItem = new SortableTableItem(harkenName);
        harkenItem->setToolTip(integration.harkenProjectPath);
        harkenItem->setData(Qt::UserRole, integration.integrationId);
        const QString harkenRoot = QFileInfo(integration.harkenProjectPath).absolutePath();
        harkenItem->setData(kProjectFolderRole, harkenRoot);
        table_->setItem(row, kHarkenProjectColumn, harkenItem);
        if (target == targets.end()) {
            table_->setItem(row, kEngineProjectColumn,
                            new SortableTableItem(QString()));
            table_->setItem(row, kEngineColumn, new QTableWidgetItem());
            table_->setItem(
                row, kSdkStatusColumn,
                new QTableWidgetItem(healthText(EngineTargetHealth::Missing)));
            table_->setItem(row, kSdkVersionColumn, new QTableWidgetItem(
                QCoreApplication::translate("HarkenHub", "Unknown")));
        } else {
            const auto observation = requestTargetObservation(*target);
            auto* targetItem = new SortableTableItem(target->displayName);
            targetItem->setToolTip(targetInputPath(*target));
            targetItem->setData(Qt::UserRole, target->canonicalRoot);
            targetItem->setData(kProjectFolderRole, target->canonicalRoot);
            table_->setItem(row, kEngineProjectColumn, targetItem);
            table_->setItem(row, kEngineColumn,
                            new QTableWidgetItem(engineName(target->engineKind)));
            const QString contentText = sdkContentText(observation.sdkContentState);
            QString statusText = observation.status;
            if (statusText.isEmpty()
                && (observation.health != EngineTargetHealth::Managed || contentText.isEmpty()))
                statusText = healthText(observation.health);
            if (!contentText.isEmpty()) {
                if (!statusText.isEmpty()) statusText += QLatin1Char('\n');
                statusText += contentText;
            }
            auto* statusItem = new QTableWidgetItem(statusText);
            QString statusDetails = localizeIntegrationText(observation.error);
            if (observation.error.isEmpty() && !observation.sdkInstallPath.isEmpty())
                statusDetails = QCoreApplication::translate(
                    "HarkenHub", "Current SDK installation path:\n%1").arg(observation.sdkInstallPath);
            if (!observation.sdkContentDetails.isEmpty())
                statusDetails += QLatin1Char('\n') + observation.sdkContentDetails;
            statusItem->setToolTip(statusDetails);
            if (observation.sdkContentState != SdkContentState::NotChecked
                && observation.sdkContentState != SdkContentState::MatchesInstallation)
                statusItem->setForeground(QColor(QStringLiteral("#E3B341")));
            table_->setItem(row, kSdkStatusColumn, statusItem);
            const auto& sources = target->engineKind == EngineKind::Unity ? unitySources : unrealSources;
            QString versionText = observation.integrationVersion;
            if (!versionText.isEmpty())
                versionText = QCoreApplication::translate("HarkenHub", "Installed: %1").arg(versionText);
            if (versionText.isEmpty())
                versionText = observation.health == EngineTargetHealth::NotInstalled
                    ? QCoreApplication::translate("HarkenHub", "Not installed")
                    : QCoreApplication::translate("HarkenHub", "Unknown");
            QString newestVersion = observation.integrationVersion;
            QStringList sourceLabels;
            for (const auto& source : sources) {
                sourceLabels << source.label;
                if (compareSdkVersions(source.version, newestVersion) > 0)
                    newestVersion = source.version;
            }
            const bool newer = !observation.integrationVersion.isEmpty()
                && newestVersion != observation.integrationVersion;
            if (newer)
                versionText += QLatin1Char('\n') + QCoreApplication::translate(
                    "HarkenHub", "Update available: %1").arg(newestVersion);
            auto* versionItem = new QTableWidgetItem(versionText);
            if (newer) versionItem->setForeground(QColor(QStringLiteral("#00D9EF")));
            versionItem->setToolTip(sourceLabels.isEmpty()
                ? QCoreApplication::translate("HarkenHub", "No local SDK sources are available.")
                : QCoreApplication::translate("HarkenHub", "Local SDK packages available: %1").arg(sources.size())
                    + QLatin1Char('\n') + sourceLabels.join(QLatin1Char('\n')) + QLatin1Char('\n')
                    + QCoreApplication::translate("HarkenHub", "SDKs with the same version number can contain different files. Select a source to check its contents."));
            table_->setItem(row, kSdkVersionColumn, versionItem);
        }

        table_->setItem(row, kActionColumn, new QTableWidgetItem());
        auto* holder = new QWidget(table_);
        holder->setObjectName(QStringLiteral("projectActionCell"));
        holder->setStyleSheet(QStringLiteral(
            "#projectActionCell { background: transparent; }"));
        auto* actions = new QHBoxLayout(holder);
        actions->setContentsMargins(0, 0, 12, 0);
        actions->addStretch();
        auto* menu = new QToolButton(holder);
        menu->setObjectName(QStringLiteral("rowMenuButton"));
        menu->setIcon(HubIcons::moreIcon(holder->devicePixelRatioF()));
        menu->setIconSize(QSize(16, 16));
        menu->setFixedSize(34, 26);
        const QString integrationId = integration.integrationId;
        const QString engineRoot = target == targets.end() ? QString() : target->canonicalRoot;
        connect(menu, &QToolButton::clicked, this, [this, integrationId, harkenRoot, engineRoot, menu]() {
            if (operationRunning_) return;
            listMenuOpen_ = true;
            QMenu popup;
            popup.setToolTipsVisible(true);
            QAction* edit = popup.addAction(QCoreApplication::translate("HarkenHub", "Manage SDK and Project Links"));
            edit->setToolTip(QCoreApplication::translate("HarkenHub", "Manage project links, verify SDK files, change versions, move or uninstall the SDK, and view detailed reports."));
            auto* change = popup.addMenu(QCoreApplication::translate("HarkenHub", "Quick Change SDK"));
            change->setObjectName(QStringLiteral("quickSdkSources"));
            change->setToolTipsVisible(true);
            const auto records = integrations_->list();
            const auto record = std::find_if(records.begin(), records.end(), [&](const auto& value) {
                return value.integrationId == integrationId;
            });
            const QString targetId = record == records.end() ? QString() : record->engineTargetId;
            const auto targets = targets_->list();
            for (const auto& target : targets)
                if (target.targetId == targetId) requestTargetObservation(target, true);
            populateQuickSdkMenu(change, integrationId);
            connect(this, &EngineIntegrationsPage::sdkStatusChanged, change,
                [this, change, integrationId, targetId](const QString& updatedId) {
                    if (updatedId == targetId) populateQuickSdkMenu(change, integrationId);
                });
            popup.addSeparator();
            addProjectFolderAction(popup, kHarkenProjectColumn, harkenRoot);
            addProjectFolderAction(popup, kEngineProjectColumn, engineRoot);
            popup.addSeparator();
            QAction* remove = popup.addAction(QCoreApplication::translate("HarkenHub", "Unlink Projects..."));
            QAction* selected = popup.exec(
                menu->mapToGlobal(QPoint(0, menu->height())));
            listMenuOpen_ = false;
            if (observationRefreshPending_)
                QTimer::singleShot(0, this, &EngineIntegrationsPage::populateList);
            const QString sourcePath = selected ? selected->data().toString() : QString();
            if (selected == edit)
                beginExistingEditor(integrationId);
            else if (selected == remove)
                unlinkProjects(integrationId);
            else if (!sourcePath.isEmpty())
                changeSdkFromList(integrationId, sourcePath);
            if (versionRefreshPending_)
                QTimer::singleShot(0, this, &EngineIntegrationsPage::refreshStudioVersion);
        });
        actions->addWidget(menu);
        table_->setCellWidget(row, kActionColumn, holder);
        if (integrationId == selectedId)
            table_->selectRow(row);
        if (integrationId == currentId)
            table_->setCurrentCell(row, kHarkenProjectColumn, QItemSelectionModel::NoUpdate);
    }
    sorting_->endRefresh();
    if (!highlightedIntegrationId_.isEmpty())
        applyHighlight();
}


void EngineIntegrationsPage::populateQuickSdkMenu(QMenu* change, const QString& integrationId)
{
    change->clear();
    const auto records = integrations_->list();
    const auto record = std::find_if(records.begin(), records.end(), [&](const auto& value) {
        return value.integrationId == integrationId;
    });
    const auto targets = targets_->list();
    const auto target = std::find_if(targets.begin(), targets.end(), [&](const auto& value) {
        return record != records.end() && value.targetId == record->engineTargetId;
    });
    if (target != targets.end()) {
        const auto observation = requestTargetObservation(*target);
        const auto sources = localSdkSources(target->engineKind, availableStudioVersions());
        if (!targetInspections_[target->targetId].ready) {
            change->addAction(observation.status)->setEnabled(false);
            change->addSeparator();
        } else if (!observation.integrationVersion.isEmpty()) {
            QString installed = QCoreApplication::translate("HarkenHub", "Installed SDK: %1")
                .arg(observation.integrationVersion);
            const QString contentText = sdkContentText(observation.sdkContentState);
            if (!contentText.isEmpty()) installed += QStringLiteral(" — ") + contentText;
            change->addAction(installed)->setEnabled(false);
            change->addSeparator();
        }
        for (const auto& source : sources) {
            QString label = source.label;
            const bool sameVersion = source.version == observation.integrationVersion;
            const bool installedSource = sameVersion && !record->sdkSourcePath.isEmpty()
                && samePath(source.path, record->sdkSourcePath);
            const int comparison = compareSdkVersions(source.version, observation.integrationVersion);
            if (installedSource) {
                label += QCoreApplication::translate("HarkenHub", " (Installed source)");
                if (observation.sdkContentState != SdkContentState::NotChecked
                    && observation.sdkContentState != SdkContentState::MatchesInstallation)
                    label += QStringLiteral(" — ") + sdkContentText(observation.sdkContentState);
            } else if (sameVersion)
                label += QCoreApplication::translate("HarkenHub", " (Same version number as installed)");
            else if (comparison > 0)
                label += QCoreApplication::translate("HarkenHub", " (Upgrade)");
            else if (comparison < 0)
                label += QCoreApplication::translate("HarkenHub", " (Older version)");
            auto* action = change->addAction(label);
            action->setData(source.path);
            action->setCheckable(installedSource);
            action->setChecked(installedSource);
            action->setEnabled(observation.canChangeSdk
                && observation.health != EngineTargetHealth::NotInstalled);
            QString tooltip = action->isEnabled()
                ? QDir::toNativeSeparators(source.path) + QLatin1Char('\n')
                    + QCoreApplication::translate("HarkenHub", "SDK files will be checked before you confirm the change.")
                : observation.remediation;
            if (installedSource)
                tooltip += QLatin1Char('\n') + QCoreApplication::translate("HarkenHub", "This is the source recorded for the installed SDK. File verification compares the installed files with their installation record; the source folder may have changed since installation.");
            if (sameVersion)
                tooltip += QLatin1Char('\n') + QCoreApplication::translate("HarkenHub", "SDKs with the same version number can contain different files. Select a source to check its contents.");
            if (!observation.sdkContentDetails.isEmpty())
                tooltip += QLatin1Char('\n') + observation.sdkContentDetails;
            action->setToolTip(tooltip);
        }
        if (sources.isEmpty())
            change->addAction(QCoreApplication::translate(
                "HarkenHub", "No local SDK sources are available."))->setEnabled(false);
    }
    if (change->isEmpty())
        change->addAction(QCoreApplication::translate(
            "HarkenHub", "No local SDK sources are available."))->setEnabled(false);
}

void EngineIntegrationsPage::beginEditForProject(
    const QString& harkenProjectPath, const QString& studioExecutablePath,
    const QString& engineProjectPath)
{
    if (operationRunning_) return;
    editingIntegrationId_.clear();
    launchingStudioExecutablePath_ = studioExecutablePath.trimmed().isEmpty()
        ? QString()
        : normalizedPath(studioExecutablePath);
    const ProjectProbeResult probe = ProjectProbe::probe(harkenProjectPath);
    if (probe.ok) {
        const QString conflict = conflictingIntegration(probe.canonicalPath, {});
        if (!conflict.isEmpty()) {
            beginExistingEditor(conflict);
            return;
        }
    }
    beginNewEditor(harkenProjectPath);
    if (!engineProjectPath.trimmed().isEmpty())
        acceptEngineTarget(engineProjectPath, true);
}

void EngineIntegrationsPage::beginNewEditor(const QString& harkenProjectPath)
{
    sdkSelectionDialog_->hide();
    sdkSelection_.reset();
    ++versionRequest_;
    studioVersion_.reset();
    versionResolutionPending_ = false;
    checkButton_->setEnabled(true);
    integrateButton_->setEnabled(true);
    editingIntegrationId_.clear();
    selectedHarkenProjectPath_.clear();
    selectedEngineTargetId_.clear();
    installPathTargetId_.clear();
    sdkPathUserSelected_ = false;
    {
        const QSignalBlocker harkenBlocker(harkenProjectEdit_);
        const QSignalBlocker engineBlocker(engineProjectEdit_);
        harkenProjectEdit_->clear();
        engineProjectEdit_->clear();
    }
    harkenProjectEdit_->setToolTip({});
    setSdkPath({}, false);
    setEditorLocked(false);
    updateRemovalButtonState();
    clearResults();
    setSummaryText(
        QCoreApplication::translate("HarkenHub", "No integration check has run."));
    stack_->setCurrentWidget(editorPage_);
    if (!harkenProjectPath.trimmed().isEmpty())
        acceptHarkenProject(harkenProjectPath, true);
}

void EngineIntegrationsPage::beginExistingEditor(
    const QString& integrationId)
{
    if (!loadExistingIntegration(integrationId)) return;
    stack_->setCurrentWidget(editorPage_);
    updateStudioVersionFields();
}

bool EngineIntegrationsPage::loadExistingIntegration(const QString& integrationId, bool refreshObservation)
{
    sdkSelectionDialog_->hide();
    quickSdkChange_ = false;
    ++versionRequest_;
    versionResolutionPending_ = false;
    studioVersion_.reset();
    const auto integrations = integrations_->list();
    const auto integration = std::find_if(
        integrations.begin(), integrations.end(),
        [&integrationId](const IntegrationRecord& value) {
            return value.integrationId == integrationId;
        });
    if (integration == integrations.end()) {
        QMessageBox::warning(this, QCoreApplication::translate("HarkenHub", "Manage Integration"),
                             QCoreApplication::translate("HarkenHub", "The integration is no longer available."));
        showList();
        return false;
    }
    const auto targets = targets_->list();
    const auto target = std::find_if(
        targets.begin(), targets.end(), [&](const EngineTargetRecord& value) {
            return value.targetId == integration->engineTargetId;
        });
    if (target == targets.end()) {
        QMessageBox::warning(this, QCoreApplication::translate("HarkenHub", "Manage Integration"),
                             QCoreApplication::translate("HarkenHub", "The engine project is no longer available."));
        showConflict(integrationId, QCoreApplication::translate("HarkenHub", "The stored engine project is missing."));
        return false;
    }

    sdkSelection_.reset();
    editingIntegrationId_ = integrationId;
    selectedHarkenProjectPath_ = integration->harkenProjectPath;
    selectedEngineTargetId_ = integration->engineTargetId;
    sdkPathUserSelected_ = false;
    {
        const QSignalBlocker harkenBlocker(harkenProjectEdit_);
        const QSignalBlocker engineBlocker(engineProjectEdit_);
        harkenProjectEdit_->setText(integration->harkenProjectPath);
        engineProjectEdit_->setText(targetInputPath(*target));
    }
    setEditorLocked(true);
    clearResults();
    setSummaryText(
        QCoreApplication::translate("HarkenHub", "No integration check has run."));
    if (!integration->sdkSourcePath.trimmed().isEmpty())
        setSdkPath(integration->sdkSourcePath, true);
    else
        setSdkPath({}, false);
    if (refreshObservation) {
        targetInspections_[target->targetId].ready = false;
        requestTargetObservation(*target, true);
    }
    updateRemovalButtonState(true);
    return true;
}

void EngineIntegrationsPage::changeSdkFromList(
    const QString& integrationId, const QString& sourcePath)
{
    if (operationRunning_ || !loadExistingIntegration(integrationId, false)) return;
    if (targetBlocked_ || !sdkInstalled_) {
        QMessageBox::warning(this, QCoreApplication::translate("HarkenHub", "Change SDK"),
            changeSdkButton_->toolTip().isEmpty()
                ? QCoreApplication::translate("HarkenHub", "The installed SDK is no longer available. Open Manage Integration to review its status.")
                : changeSdkButton_->toolTip());
        refresh();
        return;
    }
    quickSdkChange_ = true;
    beginSdkSelection();
    setSdkPath(sourcePath, true);
    runCheck();
    refresh();
}

void EngineIntegrationsPage::showList()
{
    if (operationRunning_) return;
    cancelSdkSelection();
    ++versionRequest_;
    studioVersion_.reset();
    versionResolutionPending_ = false;
    launchingStudioExecutablePath_.clear();
    stack_->setCurrentWidget(listPage_);
    refresh();
}

void EngineIntegrationsPage::refreshStudioVersion()
{
    if (!isVisible() || operationRunning_ || listMenuOpen_ || refreshingList_) {
        versionRefreshPending_ = true;
        return;
    }
    if (sdkSelectionDialog_->isVisible()) {
        versionRefreshPending_ = true;
        if (!quickSdkChange_ && !sdkSourceCombo_->view()->isVisible())
            refreshLocalSdkSources();
        return;
    }
    versionRefreshPending_ = false;
    if (stack_->currentWidget() == editorPage_)
        updateStudioVersionFields();
    else
        refresh();
}

void EngineIntegrationsPage::unlinkProjects(const QString& integrationId)
{
    if (operationRunning_ || sdkSelection_ || integrationId.isEmpty()) return;
    const QString id = integrationId;
    setOperationRunning(true);
    const auto finish = qScopeGuard([this]() { setOperationRunning(false); });
    const auto answer = QMessageBox::question(
        this, QCoreApplication::translate("HarkenHub", "Unlink Projects"),
        QCoreApplication::translate("HarkenHub", "Unlink the game and Harken projects? This entry will disappear from Engine Integrations. Project files and the installed SDK will be kept."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;
    if (!integrations_->remove(id)) {
        QMessageBox::warning(this, QCoreApplication::translate("HarkenHub", "Unlink Projects"),
            QCoreApplication::translate("HarkenHub", "Could not unlink the projects.\n%1")
                .arg(localizeIntegrationText(integrations_->lastError())));
        return;
    }
    if (id == editingIntegrationId_) {
        editingIntegrationId_.clear();
        setEditorLocked(false);
        setSummaryText(QCoreApplication::translate("HarkenHub", "Projects unlinked. Project files and the installed SDK were kept."));
    }
    refresh();
}

void EngineIntegrationsPage::linkProjects()
{
    if (operationRunning_ || sdkSelection_ || !editingIntegrationId_.isEmpty()) return;
    ProjectRecord project;
    EngineTargetRecord target;
    if (!resolveSelection(&project, &target, true, true, false)) return;
    setOperationRunning(true);
    const auto finish = qScopeGuard([this]() { setOperationRunning(false); });
    if (!confirmIntegrationAction(this,
            QCoreApplication::translate("HarkenHub", "Link Projects"),
            QCoreApplication::translate("HarkenHub", "Harken project:\n%1\n\nGame project:\n%2")
                .arg(QDir::toNativeSeparators(project.canonicalPath),
                     QDir::toNativeSeparators(targetInputPath(target)))
                + QStringLiteral("\n\n") + QCoreApplication::translate("HarkenHub", "Add this project pair to Engine Integrations. No project or SDK files are changed."),
            {}, QCoreApplication::translate("HarkenHub", "Link Projects"),
            QDialogButtonBox::AcceptRole)) return;
    if (persistIntegration(project.canonicalPath, target.targetId, sdkPathEdit_->text().trimmed())) {
        clearResults();
        setSummaryText(QCoreApplication::translate("HarkenHub", "Projects linked. SDK files were not changed."));
        refresh();
    }
}

void EngineIntegrationsPage::browseHarkenProject()
{
    auto* dialog = new QFileDialog(
        this, QCoreApplication::translate("HarkenHub", "Select Harken Project"), harkenProjectEdit_->text(),
        QCoreApplication::translate("HarkenHub", "Harken Project (*.hsp)"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setOptions(QFileDialog::DontUseCustomDirectoryIcons);
    dialog->setFileMode(QFileDialog::ExistingFile);
    connect(dialog, &QFileDialog::fileSelected, this, [this](const QString& path) {
        acceptHarkenProject(path, true);
    });
    dialog->open();
}

void EngineIntegrationsPage::browseEngineProject()
{
    auto* dialog = new QFileDialog(this,
        QCoreApplication::translate("HarkenHub", "Select Game Project"),
        engineProjectEdit_->text());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setOptions(QFileDialog::ShowDirsOnly
                       | QFileDialog::DontUseCustomDirectoryIcons);
    dialog->setFileMode(QFileDialog::Directory);
    connect(dialog, &QFileDialog::fileSelected, this, [this](const QString& path) {
        acceptEngineTarget(path, true);
    });
    dialog->open();
}

void EngineIntegrationsPage::browseSdkSource()
{
    if (!sdkSelection_ || operationRunning_) return;
    const QString start = sdkPathEdit_->text().trimmed().isEmpty()
        ? QCoreApplication::applicationDirPath()
        : sdkPathEdit_->text().trimmed();
    auto* dialog = new QFileDialog(
        sdkSelectionDialog_, QCoreApplication::translate("HarkenHub", "Choose SDK Folder"), start);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setOptions(QFileDialog::ShowDirsOnly
                       | QFileDialog::DontUseCustomDirectoryIcons);
    dialog->setFileMode(QFileDialog::Directory);
    connect(dialog, &QFileDialog::fileSelected, this, [this](const QString& path) {
        setSdkPath(path, true);
    });
    dialog->open();
}

void EngineIntegrationsPage::beginSdkSelection()
{
    if (operationRunning_ || sdkSelection_ || selectedEngineTargetId_.isEmpty()
        || selectedHarkenProjectPath_.isEmpty()
        || targetBlocked_) return;
    sdkSelection_ = SdkSelection{sdkPathEdit_->text(), sdkInstallPathEdit_->text(),
                                 sdkPathUserSelected_};
    sdkPhase_ = SdkPhase::Choosing;
    sdkPreviousVersion_ = sdkInstalled_ ? installedSdkVersion_
        : QCoreApplication::translate("HarkenHub", "Not installed");
    refreshLocalSdkSources();
    clearResults();
    updateRemovalButtonState();
    setSummaryText(QCoreApplication::translate("HarkenHub", "No integration check has run."));
    sdkSelectionDialog_->setWindowTitle(sdkInstalled_
        ? QCoreApplication::translate("HarkenHub", "Change SDK")
        : QCoreApplication::translate("HarkenHub", "Install SDK"));
    sdkSelectionDialog_->open();
    if (sdkSourceCombo_->count() > 0)
        sdkSourceCombo_->setFocus();
    else
        sdkSourceBrowseButton_->setFocus();
}

void EngineIntegrationsPage::cancelSdkSelection()
{
    if (operationRunning_) return;
    if (quickSdkChange_ || sdkPhase_ == SdkPhase::Finished) {
        quickSdkChange_ = false;
        sdkPhase_ = SdkPhase::Choosing;
        sdkSelection_.reset();
        sdkSelectionDialog_->hide();
        updatePrimaryAction();
        refresh();
        return;
    }
    if (!sdkSelection_) return;
    const SdkSelection original = *sdkSelection_;
    sdkSelection_.reset();
    sdkSelectionDialog_->hide();
    setSdkPath(original.sourcePath, original.userSelected);
    updateDefaultSdkPath();
    {
        const QSignalBlocker blocker(sdkInstallPathEdit_);
        sdkInstallPathEdit_->setText(original.installPath);
    }
    setEditorLocked(!editingIntegrationId_.isEmpty());
    updateRemovalButtonState();
    setSummaryText(QCoreApplication::translate("HarkenHub", "Cancelled. SDK files and project relationships were not changed."));
    if (versionRefreshPending_) refreshStudioVersion();
}

void EngineIntegrationsPage::browseSdkInstallPath()
{
    if (operationRunning_) return;
    const auto targets = targets_->list();
    const auto target = std::find_if(targets.begin(), targets.end(), [&](const auto& value) {
        return value.targetId == selectedEngineTargetId_;
    });
    if (target == targets.end()) return;
    const bool recover = hasPendingMigration(*target);
    if (recover || sdkInstallPathEdit_->isReadOnly()) {
        runSdkMigration(recover);
        return;
    }
    if (!sdkSelection_) return;
    const QString path = QFileDialog::getExistingDirectory(
        sdkSelectionDialog_, QCoreApplication::translate("HarkenHub", "Select SDK Installation Directory"),
        QDir(target->canonicalRoot).filePath(sdkInstallPathEdit_->text()));
    if (path.isEmpty()) return;
    QString resolved;
    QString error;
    const bool valid = target->engineKind == EngineKind::Unity
        ? VNX::UnityInstallation::resolveTarget(target->canonicalRoot, path, &resolved, &error)
        : ProjectIntegrationExecutor::resolveUnrealLocation(*target, path, &resolved, &error);
    if (!valid) {
        QMessageBox::warning(this, QCoreApplication::translate("HarkenHub", "SDK Install Location"), localizeIntegrationText(error));
        return;
    }
    sdkInstallPathEdit_->setText(QDir(target->canonicalRoot).relativeFilePath(resolved));
    sdkInstallPathEdit_->setToolTip(QDir::toNativeSeparators(resolved));
}

bool EngineIntegrationsPage::acceptHarkenProject(
    const QString& path, bool redirectOnConflict)
{
    selectedHarkenProjectPath_.clear();
    updateStudioVersionFields();
    const ProjectProbeResult probe = ProjectProbe::probe(path);
    if (!probe.ok) {
        QMessageBox::warning(
            this, QCoreApplication::translate("HarkenHub", "Manage Integration"),
            QCoreApplication::translate("HarkenHub", "The Harken project could not be used.\n\n%1")
                .arg(localizeIntegrationText(probe.error)));
        return false;
    }
    const QString conflict = conflictingIntegration(probe.canonicalPath, {});
    if (redirectOnConflict && !conflict.isEmpty()) {
        showConflict(
            conflict,
            QCoreApplication::translate("HarkenHub", "This Harken project already belongs to an engine integration."));
        return false;
    }
    if (!projects_->contains(probe.canonicalPath)) {
        ProjectRecord record;
        record.projectId = probe.projectId;
        record.canonicalPath = probe.canonicalPath;
        record.displayName = probe.displayName;
        record.detectedSchema = probe.schemaVersion;
        if (!projects_->addProject(record)) {
            QMessageBox::warning(
                this, QCoreApplication::translate("HarkenHub", "Manage Integration"),
            QCoreApplication::translate("HarkenHub", "The Harken project could not be added to Projects.\n\n%1")
                    .arg(localizeIntegrationText(projects_->lastError())));
            return false;
        }
        emit projectsChanged();
    }
    selectedHarkenProjectPath_ = probe.canonicalPath;
    {
        const QSignalBlocker blocker(harkenProjectEdit_);
        harkenProjectEdit_->setText(probe.canonicalPath);
    }
    updateStudioVersionFields();
    return true;
}

bool EngineIntegrationsPage::acceptEngineTarget(
    const QString& path, bool redirectOnConflict)
{
    EngineTargetRecord candidate;
    QString error;
    if (!EngineTargetProbe::resolve(path, &candidate, &error)) {
        QMessageBox::warning(
                this, QCoreApplication::translate("HarkenHub", "Manage Integration"),
            QCoreApplication::translate("HarkenHub", "The engine project could not be used.\n\n%1")
                .arg(localizeIntegrationText(error)));
        return false;
    }

    const auto targets = targets_->list();
    const auto existing = std::find_if(
        targets.begin(), targets.end(), [&](const EngineTargetRecord& value) {
            return samePath(value.canonicalRoot, candidate.canonicalRoot);
        });
    EngineTargetRecord selected = candidate;
    if (existing != targets.end()) {
        selected = *existing;
    } else {
        selected.addedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        if (!targets_->add(selected)) {
            QMessageBox::warning(
                this, QCoreApplication::translate("HarkenHub", "Manage Integration"),
            QCoreApplication::translate("HarkenHub", "The engine project could not be registered.\n\n%1")
                    .arg(localizeIntegrationText(targets_->lastError())));
            return false;
        }
    }

    const QString conflict = conflictingIntegration({}, selected.targetId);
    if (redirectOnConflict && !conflict.isEmpty()) {
        showConflict(
            conflict,
            QCoreApplication::translate("HarkenHub", "This Unity project already belongs to an engine integration."));
        return false;
    }
    selectedEngineTargetId_ = selected.targetId;
    targetInspections_[selected.targetId].ready = false;
    requestTargetObservation(selected, true);
    clearResults();
    {
        const QSignalBlocker blocker(engineProjectEdit_);
        engineProjectEdit_->setText(targetInputPath(selected));
    }
    updateDefaultSdkPath();
    updateSdkSourceState();
    updateRemovalButtonState(true);
    return true;
}

bool EngineIntegrationsPage::resolveSelection(
    ProjectRecord* project, EngineTargetRecord* target,
    bool registerTarget,
    bool redirectOnConflict, bool requireSdkSource)
{
    if (versionResolutionPending_)
        return false;
    integrations_->list();
    targets_->list();
    const QString storageError = integrations_->lastError().isEmpty()
        ? targets_->lastError() : integrations_->lastError();
    if (!storageError.isEmpty()) {
        QMessageBox::warning(this, QCoreApplication::translate("HarkenHub", "Manage Integration"),
            QCoreApplication::translate("HarkenHub", "Integration records could not be read.\n%1").arg(localizeIntegrationText(storageError)));
        return false;
    }
    const QString harkenInput = harkenProjectEdit_->text().trimmed();
    if (harkenInput.isEmpty()) {
        QMessageBox::warning(
            this, QCoreApplication::translate("HarkenHub", "Manage Integration"),
            QCoreApplication::translate("HarkenHub", "Select a Harken project before checking integration."));
        return false;
    }
    if (selectedHarkenProjectPath_.isEmpty()
        || !samePath(selectedHarkenProjectPath_, harkenInput)) {
        if (!acceptHarkenProject(harkenInput, redirectOnConflict))
            return false;
    }
    if (versionResolutionPending_)
        return false;
    const auto projects = projects_->listProjects();
    const auto projectMatch = std::find_if(
        projects.begin(), projects.end(), [&](const ProjectRecord& value) {
            return samePath(value.canonicalPath, selectedHarkenProjectPath_);
        });
    if (projectMatch == projects.end())
        return false;

    const QString engineInput = engineProjectEdit_->text().trimmed();
    if (engineInput.isEmpty()) {
        QMessageBox::warning(
            this, QCoreApplication::translate("HarkenHub", "Manage Integration"),
            QCoreApplication::translate("HarkenHub", "Select a Unity project before checking integration."));
        return false;
    }
    auto targets = targets_->list();
    auto targetMatch = std::find_if(
        targets.begin(), targets.end(), [&](const EngineTargetRecord& value) {
            return value.targetId == selectedEngineTargetId_
                && samePath(targetInputPath(value), engineInput);
        });
    if (targetMatch == targets.end()) {
        if (!registerTarget
            || !acceptEngineTarget(engineInput, redirectOnConflict)) {
            if (!registerTarget) {
                QMessageBox::warning(
                    this, QCoreApplication::translate("HarkenHub", "Manage Integration"),
                    QCoreApplication::translate("HarkenHub", "Select a valid Unity project folder."));
            }
            return false;
        }
        targets = targets_->list();
        targetMatch = std::find_if(
            targets.begin(), targets.end(), [&](const EngineTargetRecord& value) {
                return value.targetId == selectedEngineTargetId_;
            });
    }
    if (targetMatch == targets.end())
        return false;

    const QString conflict = conflictingIntegration(
        projectMatch->canonicalPath, targetMatch->targetId);
    if (redirectOnConflict && !conflict.isEmpty()) {
        showConflict(conflict,
                     QCoreApplication::translate("HarkenHub", "The selected project relationship already exists."));
        return false;
    }
    if (requireSdkSource && sdkPathEdit_->text().trimmed().isEmpty()) {
        QMessageBox::warning(
            sdkSelectionDialog_, QCoreApplication::translate("HarkenHub", "Manage Integration"),
            QCoreApplication::translate("HarkenHub", "Select a Runtime / SDK source before checking integration."));
        return false;
    }
    *project = *projectMatch;
    *target = *targetMatch;
    return true;
}

bool EngineIntegrationsPage::resolveStudioVersion(
    const ProjectRecord& project, const QString& launchingExecutable,
    InstalledVersion* version)
{
    auto versions = VersionScanner::scanManaged();
    if (!launchingExecutable.isEmpty()) {
        const auto launched = std::find_if(
            versions.begin(), versions.end(), [&](const InstalledVersion& value) {
                return samePath(value.executablePath,
                                launchingExecutable);
            });
        if (launched != versions.end()) {
            *version = *launched;
            return true;
        }
        if (QFileInfo(launchingExecutable).isFile()) {
            version->installPath =
                QFileInfo(launchingExecutable).absolutePath();
            version->executablePath = launchingExecutable;
            version->source = VersionSource::LocalBuild;
            return true;
        }
    }
    versions.erase(std::remove_if(versions.begin(), versions.end(),
                                  [](const InstalledVersion& value) {
                                      return !value.available;
                                  }), versions.end());
    const LaunchDecision decision = LaunchPreflight::evaluate(
        project, versions, InstallIndex::snapshot());
    if (decision.verdict != LaunchVerdict::Ready)
        return false;
    *version = decision.version;
    return true;
}

void EngineIntegrationsPage::updateStudioVersionFields()
{
    const quint64 request = ++versionRequest_;
    studioVersion_.reset();
    versionResolutionPending_ = false;
    checkButton_->setEnabled(true);
    clearResults();
    updateSdkSourceState();
    setSummaryText(QCoreApplication::translate("HarkenHub", "No integration check has run."));
    if (!sdkPathUserSelected_)
        setSdkPath({}, false);
    if (selectedHarkenProjectPath_.isEmpty()) {
        harkenProjectEdit_->setToolTip({});
        return;
    }
    const auto projects = projects_->listProjects();
    const auto project = std::find_if(
        projects.begin(), projects.end(), [&](const ProjectRecord& value) {
            return samePath(value.canonicalPath, selectedHarkenProjectPath_);
        });
    if (project == projects.end()) {
        harkenProjectEdit_->setToolTip(QCoreApplication::translate("HarkenHub", "No compatible version found"));
        return;
    }
    versionResolutionPending_ = true;
    updatePrimaryAction();
    harkenProjectEdit_->setToolTip(
        QCoreApplication::translate("HarkenHub", "Checking Studio version..."));
    const QPointer<EngineIntegrationsPage> self(this);
    QThreadPool::globalInstance()->start(
        [self, request, project = *project,
         executable = launchingStudioExecutablePath_]() {
            InstalledVersion version;
            const bool resolved = resolveStudioVersion(project, executable, &version);
            QMetaObject::invokeMethod(QCoreApplication::instance(),
                [self, request, resolved, version]() {
                    if (!self || self->versionRequest_ != request)
                        return;
                    self->versionResolutionPending_ = false;
                    self->checkButton_->setEnabled(!self->operationRunning_);
                    if (!resolved) {
                        self->harkenProjectEdit_->setToolTip(
                            QCoreApplication::translate("HarkenHub", "No compatible version found"));
                        self->updateSdkSourceState();
                        return;
                    }
                    self->studioVersion_ = version;
                    const QString displayName = versionLabel(version);
                    self->harkenProjectEdit_->setToolTip(
                        QCoreApplication::translate("HarkenHub", "Studio for opening this project: %1").arg(displayName));
                    self->updateDefaultSdkPath();
                    self->updateSdkSourceState();
                }, Qt::QueuedConnection);
        });
}

void EngineIntegrationsPage::updateDefaultSdkPath()
{
    if (sdkPathUserSelected_ || !studioVersion_)
        return;
    const auto targets = targets_->list();
    const auto target = std::find_if(
        targets.begin(), targets.end(), [&](const EngineTargetRecord& value) {
            return value.targetId == selectedEngineTargetId_;
        });
    setSdkPath(target == targets.end()
                   ? QFileInfo(studioVersion_->executablePath).absolutePath()
                   : studioSdkPath(*studioVersion_, target->engineKind), false);
}

void EngineIntegrationsPage::refreshLocalSdkSources()
{
    const auto targets = targets_->list();
    const auto target = std::find_if(targets.begin(), targets.end(), [this](const auto& value) {
        return value.targetId == selectedEngineTargetId_;
    });
    if (target == targets.end()) return;
    const QString selectedPath = sdkPathEdit_->text();
    const QSignalBlocker comboBlocker(sdkSourceCombo_);
    const QSignalBlocker editBlocker(sdkPathEdit_);
    sdkSourceCombo_->clear();
    const auto sources = localSdkSources(target->engineKind, availableStudioVersions());
    for (const auto& source : sources) {
        sdkSourceCombo_->addItem(source.path, source.path);
        sdkSourceCombo_->setItemData(sdkSourceCombo_->count() - 1,
            source.label + QLatin1Char('\n') + QDir::toNativeSeparators(source.path), Qt::ToolTipRole);
    }
    sdkSourceCombo_->setEditText(selectedPath);
    updateSdkSourceState();
}

void EngineIntegrationsPage::setSdkPath(
    const QString& path, bool userSelected)
{
    const QSignalBlocker blocker(sdkPathEdit_);
    sdkPathEdit_->setText(path.trimmed().isEmpty()
                              ? QString()
                              : QDir::cleanPath(
                                    QFileInfo(path).absoluteFilePath()));
    sdkPathUserSelected_ = userSelected;
    clearResults();
    updateSdkSourceState();
    setSummaryText(QCoreApplication::translate("HarkenHub", "No integration check has run."));
}

void EngineIntegrationsPage::updateSdkSourceState()
{
    const auto targets = targets_->list();
    const auto target = std::find_if(targets.begin(), targets.end(), [this](const auto& value) {
        return value.targetId == selectedEngineTargetId_;
    });
    const EngineKind kind = target == targets.end() ? EngineKind::Unity : target->engineKind;
    const QString selectedPath = sdkPathEdit_->text().trimmed();
    int selectedIndex = -1;
    for (int index = 0; !selectedPath.isEmpty() && index < sdkSourceCombo_->count(); ++index) {
        if (samePath(selectedPath, sdkSourceCombo_->itemData(index).toString())) {
            selectedIndex = index;
            break;
        }
    }
    const QSignalBlocker comboBlocker(sdkSourceCombo_);
    const QSignalBlocker editBlocker(sdkPathEdit_);
    sdkSourceCombo_->setCurrentIndex(selectedIndex);
    sdkSourceCombo_->setEditText(selectedPath);
    sdkSourceCombo_->setToolTip(selectedIndex < 0 ? selectedPath
        : sdkSourceCombo_->currentData(Qt::ToolTipRole).toString());
    sdkSourceAvailable_ = !sdkVersion(selectedPath, kind).isEmpty();
    sdkPathEdit_->setToolTip(selectedPath);
    sdkPathEdit_->setCursorPosition(0);
    updatePrimaryAction();
}

void EngineIntegrationsPage::updatePrimaryAction()
{
    const bool current = sdkComparison_ == SdkComparison::Current;
    const bool linked = !editingIntegrationId_.isEmpty();
    const bool selecting = sdkSelection_.has_value();
    const auto inspection = targetInspections_.constFind(selectedEngineTargetId_);
    const bool inspecting = inspection != targetInspections_.cend()
        && !inspection->ready && inspection->cancelled;
    checkButton_->setEnabled(!operationRunning_ && !versionResolutionPending_ && !selecting && !inspecting);
    sdkCheckButton_->setVisible(selecting && (sdkPhase_ != SdkPhase::Choosing
        || sdkComparison_ != SdkComparison::Unknown));
    sdkCheckButton_->setText(QCoreApplication::translate("HarkenHub", "Check Again"));
    sdkCheckButton_->setEnabled(!operationRunning_ && !versionResolutionPending_ && selecting && !inspecting);
    changeSdkButton_->setText(sdkInstalled_
        ? QCoreApplication::translate("HarkenHub", "Change SDK...")
        : QCoreApplication::translate("HarkenHub", "Install SDK..."));
    changeSdkButton_->setEnabled(!operationRunning_ && !selecting
        && !selectedEngineTargetId_.isEmpty() && !selectedHarkenProjectPath_.isEmpty()
        && !targetBlocked_);
    sdkSelectionRow_->setVisible(selecting && sdkPhase_ != SdkPhase::Finished);
    sdkSelectionContext_->setVisible(selecting);
    sdkSafetyHint_->setVisible(selecting && sdkPhase_ != SdkPhase::Finished && engineRunning_);
    sdkReportButton_->setVisible(selecting && !sdkReportPath_.isEmpty());
    sdkReportButton_->setEnabled(!operationRunning_);
    updateSdkSelectionContext();
    sdkSourceBrowseButton_->setEnabled(selecting && !operationRunning_);
    cancelSdkButton_->setVisible(selecting || quickSdkChange_);
    cancelSdkButton_->setText(quickSdkChange_ || sdkPhase_ == SdkPhase::Finished
        ? QCoreApplication::translate("HarkenHub", "Close")
        : QCoreApplication::translate("HarkenHub", "Cancel"));
    cancelSdkButton_->setEnabled(!operationRunning_);
    integrateButton_->setVisible(selecting && sdkPhase_ != SdkPhase::Finished);
    integrateButton_->setText(sdkPhase_ == SdkPhase::Applying
        ? QCoreApplication::translate("HarkenHub", "Applying SDK...")
        : sdkPhase_ == SdkPhase::Checking
            ? QCoreApplication::translate("HarkenHub", "Checking SDK...")
        : current ? QCoreApplication::translate("HarkenHub", "Up to Date")
        : sdkPhase_ == SdkPhase::Reviewing
            ? (sdkInstalled_ ? QCoreApplication::translate("HarkenHub", "Confirm SDK Change")
                             : QCoreApplication::translate("HarkenHub", "Confirm SDK Installation"))
            : QCoreApplication::translate("HarkenHub", "Check SDK Changes"));
    integrateButton_->setEnabled(!operationRunning_ && !versionResolutionPending_
        && !selectedHarkenProjectPath_.isEmpty() && !selectedEngineTargetId_.isEmpty()
        && sdkSourceAvailable_ && !targetBlocked_
        && sdkComparison_ != SdkComparison::Blocked && !current && selecting
        && sdkPhase_ != SdkPhase::Finished);
    integrateButton_->setToolTip(!sdkSourceAvailable_
        ? QCoreApplication::translate("HarkenHub", "Select a valid SDK source or run a check for details.")
        : sdkComparison_ == SdkComparison::Blocked
            ? QCoreApplication::translate("HarkenHub", "Resolve the failed check before continuing.")
            : QString());
    relationshipLabel_->setText(linked
        ? QCoreApplication::translate("HarkenHub", "Projects linked")
        : QCoreApplication::translate("HarkenHub", "Projects not linked"));
    relationshipLabel_->setStyleSheet(QStringLiteral("font-size: 12px; font-weight: 400; color: %1;")
        .arg(linked ? QStringLiteral("#69D391") : QStringLiteral("#FF6B6B")));
    linkProjectsButton_->setVisible(!linked);
    linkProjectsButton_->setEnabled(!operationRunning_ && !selecting && !versionResolutionPending_
        && !selectedHarkenProjectPath_.isEmpty() && !selectedEngineTargetId_.isEmpty());
    unlinkProjectsAction_->setEnabled(!operationRunning_ && !selecting && linked);
    unlinkProjectsButton_->setVisible(linked);
    unlinkProjectsButton_->setEnabled(unlinkProjectsAction_->isEnabled());
    const auto actionButtons = {engineBrowseButton_, harkenBrowseButton_, linkProjectsButton_,
        unlinkProjectsButton_, changeSdkButton_, migrateSdkButton_, removeInstalledSdkButton_, sdkResolutionButton_};
    int actionWidth = kPathButtonMinimumWidth;
    for (auto* button : actionButtons)
        actionWidth = std::max(actionWidth, button->sizeHint().width());
    for (auto* button : actionButtons)
        button->setFixedWidth(actionWidth);
}

void EngineIntegrationsPage::updateSdkSelectionContext()
{
    if (!sdkSelection_) return;
    const auto targets = targets_->list();
    const auto target = std::find_if(targets.begin(), targets.end(), [this](const auto& value) {
        return value.targetId == selectedEngineTargetId_;
    });
    if (target == targets.end()) return;
    const QString version = sdkVersion(sdkPathEdit_->text(), target->engineKind);
    QString context = QCoreApplication::translate("HarkenHub", "Game project: %1 | Harken project: %2")
        .arg(target->displayName, QFileInfo(selectedHarkenProjectPath_).completeBaseName());
    if (!version.isEmpty()) {
        context += QLatin1Char('\n') + QCoreApplication::translate("HarkenHub", "SDK version: %1 → %2")
            .arg(sdkPreviousVersion_, version);
        if (compareSdkVersions(version, sdkPreviousVersion_) < 0)
            context += QCoreApplication::translate("HarkenHub", " (Downgrade)");
    }
    sdkSelectionContext_->setText(context);
    sdkSelectionPaths_->setPlainText(QCoreApplication::translate("HarkenHub",
        "Harken project:\n%1\n\nGame project:\n%2\n\nSDK source:\n%3\n\nSDK installation location:\n%4")
        .arg(QDir::toNativeSeparators(selectedHarkenProjectPath_),
             QDir::toNativeSeparators(targetInputPath(*target)),
             QDir::toNativeSeparators(sdkPathEdit_->text()),
             QDir::toNativeSeparators(QDir(target->canonicalRoot).filePath(sdkInstallPathEdit_->text()))));
}

void EngineIntegrationsPage::setEditorLocked(bool locked)
{
    harkenProjectEdit_->setReadOnly(locked);
    engineProjectEdit_->setReadOnly(locked);
    harkenBrowseButton_->setEnabled(!locked);
    engineBrowseButton_->setEnabled(!locked);
    harkenBrowseButton_->setVisible(!locked);
    engineBrowseButton_->setVisible(!locked);
    updatePrimaryAction();
    updateIdleSummary();
}

void EngineIntegrationsPage::scanStopped()
{
    inspectionStopped_ = true;
    clearResults();
    setSummaryText(QCoreApplication::translate("HarkenHub", "SDK scan stopped. The check is incomplete; no SDK files were changed."));
}

EngineTargetObservation EngineIntegrationsPage::observeTarget(const EngineTargetRecord& target, bool verifyContent)
{
    const auto result = runIntegrationTask(this, [target, verifyContent]() {
        return EngineTargetProbe::observe(target, verifyContent);
    }, sdkPhase_ != SdkPhase::Applying, sdkSelection_ ? sdkProgress_ : nullptr);
    if (result) {
        inspectionStopped_ = false;
        auto& inspection = targetInspections_[target.targetId];
        if (inspection.cancelled) inspection.cancelled->store(true, std::memory_order_relaxed);
        inspection.cancelled.reset();
        inspection.observation = *result;
        inspection.ready = true;
        return *result;
    }
    EngineTargetObservation stopped;
    inspectionStopped_ = true;
    reviewedUnrealPlan_.reset();
    sdkComparison_ = SdkComparison::Unknown;
    stopped.status = QCoreApplication::translate("HarkenHub", "SDK scan stopped");
    stopped.remediation = QCoreApplication::translate("HarkenHub", "Run the SDK check again to finish inspecting this project.");
    stopped.error = stopped.status + QStringLiteral(". ") + stopped.remediation;
    setSummaryText(resultSteps_.isEmpty() ? stopped.error : summaryLabel_->text() + QLatin1Char('\n') + stopped.error);
    return stopped;
}

UnityIntegrationResult EngineIntegrationsPage::runUnityTask(const UnityIntegrationRequest& request)
{
    const bool readOnly = request.verifyInstalledOnly || request.checkOnly
        || (request.removeIntegration && !request.confirmRemoval)
        || (request.migrateIntegration && !request.confirmMigration);
    const auto result = runIntegrationTask(this, [request]() { return HubUnityIntegrationService::run(request); },
        readOnly, sdkSelection_ ? sdkProgress_ : nullptr);
    if (result) return *result;
    scanStopped();
    UnityIntegrationResult stopped;
    stopped.cancelled = true;
    return stopped;
}

IntegrationPlan EngineIntegrationsPage::inspectUnrealTask(const EngineTargetRecord& target,
    const QString& source, std::optional<IntegrationOperation> operation)
{
    const auto result = runIntegrationTask(this, [target, source, operation]() {
        return operation ? ProjectIntegrationExecutor::planUnreal(target, source, *operation)
                         : ProjectIntegrationExecutor::inspectUnreal(target);
    }, true, sdkSelection_ ? sdkProgress_ : nullptr);
    if (result) return *result;
    scanStopped();
    IntegrationPlan stopped;
    stopped.cancelled = true;
    stopped.error = summaryLabel_->text();
    return stopped;
}

void EngineIntegrationsPage::updateRemovalButtonState(bool recoverLocation)
{
    const auto targets = targets_->list();
    const auto target = std::find_if(
        targets.begin(), targets.end(), [&](const EngineTargetRecord& value) {
            return value.targetId == selectedEngineTargetId_;
        });
    const bool unity = target != targets.end()
        && target->engineKind == EngineKind::Unity;
    sdkHeading_->setText(target == targets.end() ? QStringLiteral("SDK")
        : QCoreApplication::translate("HarkenHub", "%1 SDK").arg(engineName(target->engineKind)));
    removeInstalledSdkButton_->setVisible(true);
    migrateSdkButton_->setVisible(true);
    const EngineTargetObservation observation = target == targets.end()
        ? EngineTargetObservation{} : operationRunning_ ? observeTarget(*target)
                                                       : requestTargetObservation(*target);
    if (target != targets.end() && recoverLocation && !targetInspections_[target->targetId].ready)
        targetInspections_[target->targetId].recoverLocation = true;
    sdkInstalled_ = target != targets.end() && observation.validTarget
        && observation.health != EngineTargetHealth::NotInstalled;
    targetBlocked_ = target != targets.end() && !observation.canChangeSdk;
    installedSdkVersion_ = target == targets.end()
        ? QCoreApplication::translate("HarkenHub", "No game project selected")
        : observation.integrationVersion.isEmpty() ? (observation.status.isEmpty()
            ? healthText(observation.health) : observation.status)
                                                  : observation.integrationVersion;
    installedSdkLabel_->setText(observation.integrationVersion.isEmpty() ? installedSdkVersion_
        : QCoreApplication::translate("HarkenHub", "(SDK Version: %1)").arg(installedSdkVersion_));
    installedSdkLabel_->setToolTip(localizeIntegrationText(observation.error));
    const QString installedPath = target == targets.end() || observation.sdkInstallPath.isEmpty()
        ? QString() : QDir::toNativeSeparators(QDir::cleanPath(
            QDir(target->canonicalRoot).absoluteFilePath(observation.sdkInstallPath)));
    installedSdkPath_->setText(installedPath);
    installedSdkPath_->setToolTip(installedPath);
    installedSdkPath_->setCursorPosition(0);
    QString issue = observation.status.isEmpty()
        ? localizeIntegrationText(observation.error).section(QLatin1Char('\n'), 0, 0)
        : observation.status;
    if (issue.isEmpty()) {
        for (const auto& step : resultSteps_) {
            if (step.status != ResultStatus::Failed) continue;
            issue = step.summary;
            break;
        }
    }
    sdkIssueLabel_->setText(issue);
    sdkIssueLabel_->setStyleSheet(observation.dependencyRestricted
        ? QStringLiteral("color: #9AA4B5;") : QStringLiteral("color: #F0C36A;"));
    sdkResolutionButton_->setText(observation.dependencyRestricted
        ? QCoreApplication::translate("HarkenHub", "View Dependencies...")
        : QCoreApplication::translate("HarkenHub", "View Resolution..."));
    sdkIssueRow_->setVisible(!issue.isEmpty());
    sdkResolutionButton_->setEnabled(!operationRunning_ && !sdkSelection_);
    changeSdkButton_->setToolTip(observation.canChangeSdk ? QString() : observation.remediation);
    removeInstalledSdkButton_->setToolTip(observation.canRemoveSdk ? QString() : observation.remediation);
    migrateSdkButton_->setToolTip(observation.canMoveSdk ? QString() : observation.remediation);
    sdkInstallRow_->setVisible(target != targets.end() && !sdkInstalled_);
    updatePrimaryAction();
    if (target == targets.end()) {
        sdkInstallBrowseButton_->setText(QCoreApplication::translate("HarkenHub", "Browse..."));
        removeInstalledSdkButton_->setEnabled(!operationRunning_ && !sdkSelection_
            && observation.validTarget && observation.canRemoveSdk);
        migrateSdkButton_->setText(QCoreApplication::translate("HarkenHub", "Move SDK..."));
        migrateSdkButton_->setEnabled(false);
        sdkInstallPathEdit_->setEnabled(false);
        sdkInstallBrowseButton_->setEnabled(false);
        sdkInstallBrowseButton_->hide();
        const QSignalBlocker blocker(sdkInstallPathEdit_);
        sdkInstallPathEdit_->clear();
        installPathTargetId_.clear();
        updatePrimaryAction();
        updateIdleSummary();
        return;
    }
    const bool installed = sdkInstalled_;
    const bool pendingMigration = hasPendingMigration(*target);
    sdkInstallBrowseButton_->setText(QCoreApplication::translate("HarkenHub", "Browse..."));
    sdkInstallBrowseButton_->setVisible(sdkSelection_.has_value() && !installed && !pendingMigration);
    sdkInstallPathEdit_->setEnabled(true);
    sdkInstallPathEdit_->setReadOnly(installed || !sdkSelection_);
    sdkInstallBrowseButton_->setEnabled(!operationRunning_
        && (!installed || pendingMigration || observation.health == EngineTargetHealth::Managed));
    migrateSdkButton_->setText(pendingMigration
        ? QCoreApplication::translate("HarkenHub", "Recover SDK Migration...")
        : QCoreApplication::translate("HarkenHub", "Move SDK..."));
    migrateSdkButton_->setEnabled(!operationRunning_ && !sdkSelection_
        && observation.canMoveSdk);
    if (!operationRunning_ && (!observation.sdkInstallPath.isEmpty()
                              || installPathTargetId_ != target->targetId)) {
        const QSignalBlocker blocker(sdkInstallPathEdit_);
        sdkInstallPathEdit_->setText(!observation.sdkInstallPath.isEmpty()
            ? observation.sdkInstallPath
            : target->sdkInstallPath.isEmpty() ? (unity ? VNX::UnityInstallation::defaultRelativePath()
                                                       : QStringLiteral("Plugins/Harken"))
                                              : target->sdkInstallPath);
    }
    installPathTargetId_ = target->targetId;
    sdkInstallPathEdit_->setToolTip(observation.error.isEmpty()
        ? QDir::toNativeSeparators(QDir(target->canonicalRoot).filePath(sdkInstallPathEdit_->text()))
        : localizeIntegrationText(observation.error));
    if (recoverLocation && observation.health == EngineTargetHealth::Managed
        && observation.sdkInstallPath != target->sdkInstallPath
        && !targets_->updateSdkInstallPath(target->targetId, observation.sdkInstallPath)) {
        setSummaryText(QCoreApplication::translate(
            "HarkenHub", "The SDK was found, but its installation location could not be saved.\n%1")
                .arg(targets_->lastError()));
    }
    removeInstalledSdkButton_->setEnabled(
        !operationRunning_ && !sdkSelection_ && observation.validTarget
        && observation.canRemoveSdk);
    updatePrimaryAction();
    updateIdleSummary();
}

void EngineIntegrationsPage::showSdkResolution()
{
    if (operationRunning_ || sdkSelection_) return;
    const auto targets = targets_->list();
    const auto target = std::find_if(targets.begin(), targets.end(), [this](const auto& value) {
        return value.targetId == selectedEngineTargetId_;
    });
    if (target == targets.end()) return;
    const auto observation = observeTarget(*target);
    if (inspectionStopped_) return;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("sdkResolutionDialog"));
    dialog.setWindowTitle(observation.dependencyRestricted
        ? QCoreApplication::translate("HarkenHub", "SDK Dependencies")
        : QCoreApplication::translate("HarkenHub", "SDK Resolution"));
    dialog.resize(680, 430);
    dialog.setMinimumSize(520, 320);
    auto* layout = new QVBoxLayout(&dialog);
    QStringList sections;
    if (!observation.status.isEmpty()) sections << observation.status;
    if (!observation.error.isEmpty()) sections << localizeIntegrationText(observation.error);
    if (!observation.remediation.isEmpty()) sections << observation.remediation;
    if (observation.remediation.isEmpty()) {
        for (const auto& step : resultSteps_) {
            if (step.status == ResultStatus::Failed) {
                sections << formatStepDetails(step);
                break;
            }
        }
    }
    const auto availability = [&](bool supported) {
        if (!supported && observation.health == EngineTargetHealth::NotInstalled)
            return QCoreApplication::translate("HarkenHub", "No SDK installed");
        return supported ? QCoreApplication::translate("HarkenHub", "Available after safety checks")
                         : QCoreApplication::translate("HarkenHub", "Resolve the issue above first");
    };
    sections << QCoreApplication::translate("HarkenHub", "Change SDK: %1\nMove SDK: %2\nUninstall SDK: %3")
        .arg(availability(observation.canChangeSdk),
             hasPendingMigration(*target)
                    ? QCoreApplication::translate("HarkenHub", "Use Recover SDK Migration first")
                    : availability(observation.canMoveSdk),
             availability(observation.canRemoveSdk));
    auto* details = new QPlainTextEdit(&dialog);
    details->setObjectName(QStringLiteral("sdkResolutionDetails"));
    details->setReadOnly(true);
    details->setPlainText(sections.join(QStringLiteral("\n\n")));
    layout->addWidget(details, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    const QString directory = observation.sdkInstallPath.isEmpty() ? target->canonicalRoot
        : QDir(target->canonicalRoot).filePath(observation.sdkInstallPath);
    auto* open = buttons->addButton(observation.sdkInstallPath.isEmpty()
        ? QCoreApplication::translate("HarkenHub", "Open Project Folder")
        : QCoreApplication::translate("HarkenHub", "Open SDK Folder"), QDialogButtonBox::ActionRole);
    open->setEnabled(QDir(directory).exists());
    connect(open, &QPushButton::clicked, &dialog, [directory]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
    });
    auto* recheck = buttons->addButton(QCoreApplication::translate("HarkenHub", "Check Again"), QDialogButtonBox::AcceptRole);
    connect(recheck, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() == QDialog::Accepted) runCheck();
}

void EngineIntegrationsPage::refreshEngineRunningHint()
{
    if (engineStatusPending_ || operationRunning_ || !sdkSelectionDialog_->isVisible()
        || !sdkSelection_ || sdkPhase_ == SdkPhase::Finished) return;
    const auto targets = targets_->list();
    const auto target = std::find_if(targets.begin(), targets.end(), [this](const auto& value) {
        return value.targetId == selectedEngineTargetId_;
    });
    if (target == targets.end()) return;
    engineStatusPending_ = true;
    const QPointer<EngineIntegrationsPage> self(this);
    QThreadPool::globalInstance()->start([self, target = *target]() {
        QString error;
        bool isOpen = false;
        if (target.engineKind == EngineKind::Unity)
            VNX::checkUnityProjectClosed(target.canonicalRoot, &error, &isOpen);
        else
            ProjectIntegrationExecutor::checkUnrealClosed(target, &error, &isOpen);
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, target, isOpen, error]() {
            if (!self) return;
            self->engineStatusPending_ = false;
            if (self->selectedEngineTargetId_ != target.targetId || !self->sdkSelectionDialog_->isVisible()) return;
            self->engineRunning_ = isOpen;
            self->sdkSafetyHint_->setText(isOpen ? localizeIntegrationText(error) : QString());
            self->updatePrimaryAction();
        }, Qt::QueuedConnection);
    });
}

void EngineIntegrationsPage::setOperationRunning(bool running)
{
    operationRunning_ = running;
    if (running) {
        inspectionStopped_ = false;
        cancelTargetInspections();
    }
    integrationForm_->setEnabled(!running);
    sdkSelectionRow_->setEnabled(!running);
    backButton_->setEnabled(!running);
    checkButton_->setEnabled(!running && !versionResolutionPending_);
    updatePrimaryAction();
    if (!running && !inspectionStopped_) {
        refresh();
        updateRemovalButtonState();
        refreshEngineRunningHint();
    }
    if (!running && versionRefreshPending_)
        QTimer::singleShot(0, this, &EngineIntegrationsPage::refreshStudioVersion);
}

void EngineIntegrationsPage::setSummaryText(const QString& text)
{
    summaryLabel_->setStyleSheet(QString());
    showingIdleSummary_ = text == QCoreApplication::translate(
        "HarkenHub", "No integration check has run.");
    if (showingIdleSummary_)
        updateIdleSummary();
    else
        summaryLabel_->setText(text);
    sdkSelectionSummary_->setText(summaryLabel_->text());
}

void EngineIntegrationsPage::updateIdleSummary()
{
    if (!showingIdleSummary_ || operationRunning_) return;
    QStringList hints;
    if (sdkSelection_ && !sdkPathEdit_->text().trimmed().isEmpty() && !sdkSourceAvailable_)
        hints << QCoreApplication::translate("HarkenHub", "The selected SDK source is unavailable.");
    if (sdkInstalled_)
        hints << (sdkSelection_
            ? QCoreApplication::translate("HarkenHub", "Installed SDK detected. Target SDK has not been compared yet.")
            : QCoreApplication::translate("HarkenHub", "SDK installed."));
    if (targetBlocked_)
        hints << installedSdkLabel_->toolTip();
    EngineTargetRecord target;
    target.canonicalRoot = engineProjectEdit_->text();
    if (VNX::UnityInstallation::hasPendingMigration(target.canonicalRoot)
        || ProjectIntegrationExecutor::hasPendingUnrealMigration(target))
        hints << QCoreApplication::translate("HarkenHub", "The SDK migration is unfinished. Choose \"Recover...\" before continuing.");
    summaryLabel_->setText(hints.isEmpty()
        ? QCoreApplication::translate("HarkenHub", "No integration check has run.")
        : hints.join(QLatin1Char('\n')));
    sdkSelectionSummary_->setText(summaryLabel_->text());
}

void EngineIntegrationsPage::clearResults()
{
    summaryLabel_->setStyleSheet(QString());
    reviewedUnrealPlan_.reset();
    reviewedSdkHash_.clear();
    sdkReportPath_.clear();
    if (!operationRunning_) sdkPhase_ = SdkPhase::Choosing;
    sdkComparison_ = SdkComparison::Unknown;
    const QSignalBlocker blocker(stepsTable_);
    resultSteps_.clear();
    stepsTable_->clearContents();
    stepsTable_->setRowCount(0);
    detailEdit_->clear();
    sdkSelectionDetails_->clear();
    sdkDiffSummary_->clear();
    sdkDiffTree_->clear();
    sdkResultsTabs_->setTabText(1, QCoreApplication::translate("HarkenHub", "Check Log"));
    sdkResultsTabs_->setCurrentIndex(0);
    detailEdit_->hide();
    summaryLabel_->setToolTip(QString());
    updatePrimaryAction();
}

void EngineIntegrationsPage::runCheck()
{
    if (operationRunning_) return;
    if (!sdkSelection_) {
        const auto targets = targets_->list();
        const auto selected = std::find_if(targets.begin(), targets.end(), [this](const auto& value) {
            return value.targetId == selectedEngineTargetId_;
        });
        if (selected != targets.end()) {
            setOperationRunning(true);
            const auto finish = qScopeGuard([this]() { setOperationRunning(false); });
            clearResults();
            QVector<ResultStep> steps;
            bool success = false;
            if (selected->engineKind == EngineKind::Unity) {
                UnityIntegrationRequest request;
                request.unityProjectPath = selected->canonicalRoot;
                request.verifyInstalledOnly = true;
                const auto result = runUnityTask(request);
                if (result.cancelled) return;
                success = result.success;
                steps = unityResultSteps(result, true);
            } else {
                const auto result = inspectUnrealTask(*selected);
                if (result.cancelled) return;
                success = result.valid && result.modifiedManagedFiles.isEmpty();
                ResultStep step;
                step.id = QStringLiteral("verify");
                step.title = QCoreApplication::translate("HarkenHub", "Verify installed SDK");
                step.status = success ? ResultStatus::CheckPassed : ResultStatus::Failed;
                step.cause = result.error;
                step.artifacts = {result.targetRoot};
                step.summary = success
                    ? QCoreApplication::translate("HarkenHub", "Installed SDK files match the installation record.")
                    : QCoreApplication::translate("HarkenHub", "Installed SDK files do not match the installation record or could not be read.");
                step.details = result.modifiedManagedFiles;
                if (!result.modifiedManagedFiles.isEmpty())
                    step.remediation = QCoreApplication::translate("HarkenHub", "Review the listed files and local changes. Restore missing files from backup or use Change SDK with a complete SDK package.");
                steps.push_back(step);
            }
            if (!success) {
                const auto observation = observeTarget(*selected);
                if (inspectionStopped_) return;
                for (auto& step : steps) {
                    if (step.status != ResultStatus::Failed) continue;
                    if (!observation.status.isEmpty()) {
                        if (!step.cause.isEmpty()) step.cause += QLatin1Char('\n');
                        step.cause += observation.status;
                    }
                    if (!observation.remediation.isEmpty()) {
                        if (!step.remediation.isEmpty()) step.remediation += QStringLiteral("\n\n");
                        step.remediation += observation.remediation;
                    }
                    break;
                }
            }
            showResults(steps, success
                ? QCoreApplication::translate("HarkenHub", "Installed SDK verification passed. No files were changed.")
                : QCoreApplication::translate("HarkenHub", "Installed SDK verification did not pass. Review the results; no files were changed."),
                QStringLiteral("verify"));
            if (success)
                summaryLabel_->setStyleSheet(QStringLiteral("color: #69D391;"));
            return;
        }
        setSummaryText(QCoreApplication::translate("HarkenHub", "Select a Unity project before checking integration."));
        return;
    }
    ProjectRecord project;
    EngineTargetRecord target;
    if (!resolveSelection(&project, &target, true, true))
        return;
    setOperationRunning(true);
    const auto finish = qScopeGuard([this]() { setOperationRunning(false); });
    const bool success = runCheckForSelection(
        target, sdkPathEdit_->text().trimmed());
    if (!success)
        stepsTable_->selectRow(std::max(0, stepsTable_->currentRow()));
}

void EngineIntegrationsPage::runIntegration()
{
    if (operationRunning_ || !sdkSelection_) return;
    if (sdkPhase_ != SdkPhase::Reviewing || reviewedSdkHash_.isEmpty()) {
        runCheck();
        return;
    }
    ProjectRecord project;
    EngineTargetRecord target;
    if (!resolveSelection(&project, &target, true, true)) return;
    const QString sdkPath = sdkPathEdit_->text().trimmed();
    const QByteArray approved = reviewedSdkHash_;
    setOperationRunning(true);
    const auto finish = qScopeGuard([this]() { setOperationRunning(false); });
    if (!runCheckForSelection(target, sdkPath)) return;
    if (sdkComparison_ == SdkComparison::Current) return;
    if (approved != reviewedSdkHash_) {
        setSummaryText(QCoreApplication::translate("HarkenHub",
            "SDK files changed. Review the refreshed results and confirm again. No SDK files were changed."));
        return;
    }
    const QString conflict = conflictingIntegration(project.canonicalPath, target.targetId);
    if (!conflict.isEmpty()) {
        sdkPhase_ = SdkPhase::Choosing;
        reviewedSdkHash_.clear();
        setSummaryText(QCoreApplication::translate("HarkenHub",
            "The selected project relationship was created while the check was running."));
        return;
    }
    sdkPhase_ = SdkPhase::Applying;
    sdkReviewInvalidated_ = false;
    updatePrimaryAction();
    const bool success = target.engineKind == EngineKind::Unity
        ? runUnity(target, sdkPath, false) : runUnreal(target, sdkPath, false);
    if (sdkReviewInvalidated_) {
        if (runCheckForSelection(target, sdkPath))
            setSummaryText(QCoreApplication::translate("HarkenHub",
                "SDK files changed. Review the refreshed results and confirm again. No SDK files were changed."));
        return;
    }
    sdkPhase_ = SdkPhase::Finished;
    if (success && !editingIntegrationId_.isEmpty())
        persistIntegration(project.canonicalPath, target.targetId, sdkPath);
    setEditorLocked(!editingIntegrationId_.isEmpty());
}

bool EngineIntegrationsPage::runCheckForSelection(
    const EngineTargetRecord& target, const QString& sdkPath)
{
    sdkPhase_ = SdkPhase::Checking;
    updatePrimaryAction();
    const bool success = target.engineKind == EngineKind::Unity
        ? runUnity(target, sdkPath, true) : runUnreal(target, sdkPath, true);
    sdkPhase_ = success && sdkComparison_ == SdkComparison::Different && !reviewedSdkHash_.isEmpty()
        ? SdkPhase::Reviewing : SdkPhase::Choosing;
    if (success) {
        sdkPreviousVersion_ = sdkInstalled_ ? installedSdkVersion_
            : QCoreApplication::translate("HarkenHub", "Not installed");
        if (sdkPhase_ == SdkPhase::Reviewing)
            setSummaryText(QCoreApplication::translate("HarkenHub", "Check complete. Review the changes, then confirm to continue."));
    }
    updatePrimaryAction();
    return success;
}

QVector<EngineIntegrationsPage::ResultStep>
EngineIntegrationsPage::unityResultSteps(
    const UnityIntegrationResult& result, bool checkOnly) const
{
    QVector<ResultStep> steps;
    steps.reserve(result.steps.size());
    for (const UnityIntegrationStep& step : result.steps) {
        ResultStatus status = ResultStatus::Pending;
        switch (step.status) {
            case UnityIntegrationStepStatus::Pending:
                status = ResultStatus::Pending;
                break;
            case UnityIntegrationStepStatus::Running:
                status = ResultStatus::Running;
                break;
            case UnityIntegrationStepStatus::Succeeded:
                status = ResultStatus::Succeeded;
                break;
            case UnityIntegrationStepStatus::Warning:
                status = ResultStatus::Warning;
                break;
            case UnityIntegrationStepStatus::Failed:
                status = ResultStatus::Failed;
                break;
            case UnityIntegrationStepStatus::Skipped:
                status = ResultStatus::Skipped;
                break;
        }
        const bool preflight = checkOnly || result.requiresConfirmation;
        if (preflight && status == ResultStatus::Succeeded)
            status = ResultStatus::CheckPassed;
        if (preflight && status == ResultStatus::Skipped)
            status = ResultStatus::NotRun;
        if (result.requiresConfirmation
            && (step.id == QStringLiteral("remove_integration")
                || step.id == QStringLiteral("migration_review")))
            status = ResultStatus::AwaitingConfirmation;
        if (checkOnly && result.success && !result.targetAlreadyCurrent
            && step.id == QStringLiteral("install_sdk"))
            status = ResultStatus::AwaitingConfirmation;
        QStringList details;
        details.reserve(step.details.size());
        for (const QString& detail : step.details)
            details.push_back(localizeIntegrationText(detail));
        const bool checkReport = preflight && (step.id == QStringLiteral("report")
            || step.id == QStringLiteral("removal_report"));
        steps.push_back({step.id, checkReport
                            ? QCoreApplication::translate("HarkenHub", "Write check report")
                            : localizeUnityStepTitle(step.id, step.title), status,
                         checkReport && step.status == UnityIntegrationStepStatus::Succeeded
                            ? QCoreApplication::translate("HarkenHub", "Check report written. No SDK changes were executed.")
                            : localizeIntegrationText(step.summary),
                         localizeIntegrationText(step.cause),
                         localizeIntegrationText(step.remediation), details,
                         step.artifacts, step.elapsedMs});
        if (step.id == QStringLiteral("compare_sdk")
            && (step.status == UnityIntegrationStepStatus::Succeeded
                || step.status == UnityIntegrationStepStatus::Warning)) {
            auto& comparison = steps.back();
            comparison.hasFileComparison = true;
            comparison.modifiedFiles = result.modifiedFiles;
            comparison.missingFiles = result.missingFiles;
            comparison.attentionRequired = !result.modifiedFiles.isEmpty() || !result.missingFiles.isEmpty()
                || (result.targetAlreadyCurrent && step.status == UnityIntegrationStepStatus::Warning);
            for (const auto& change : result.fileChanges)
                comparison.fileChanges.push_back({change.path, change.action});
        }
    }
    return steps;
}

void EngineIntegrationsPage::cancelPendingOperation()
{
    auto steps = resultSteps_;
    QString cancelledStep;
    for (auto& step : steps) {
        if (step.status != ResultStatus::AwaitingConfirmation
            && step.status != ResultStatus::NotRun) continue;
        if (cancelledStep.isEmpty()) cancelledStep = step.id;
        step.status = ResultStatus::NotRun;
        step.summary = QCoreApplication::translate("HarkenHub", "Cancelled. This step was not executed.");
        step.cause.clear();
        step.remediation.clear();
    }
    showResults(steps, QCoreApplication::translate("HarkenHub", "Cancelled. SDK files and project relationships were not changed."), cancelledStep);
}

void EngineIntegrationsPage::runSdkMigration(bool recover)
{
    const auto targets = targets_->list();
    integrations_->list();
    const QString storageError = targets_->lastError().isEmpty()
        ? integrations_->lastError() : targets_->lastError();
    if (!storageError.isEmpty()) {
        setSummaryText(QCoreApplication::translate("HarkenHub", "Integration records could not be read.\n%1")
            .arg(localizeIntegrationText(storageError)));
        return;
    }
    const auto selected = std::find_if(targets.begin(), targets.end(), [&](const auto& target) {
        return target.targetId == selectedEngineTargetId_
            && samePath(targetInputPath(target), engineProjectEdit_->text().trimmed());
    });
    if (selected == targets.end() || operationRunning_ || sdkSelection_) return;
    const EngineTargetRecord target = *selected;
    setOperationRunning(true);
    const auto finish = qScopeGuard([this]() { setOperationRunning(false); });
    UnityIntegrationRequest request;
    request.unityProjectPath = target.canonicalRoot;
    request.sdkInstallPath = sdkInstallPathEdit_->text().trimmed();
    request.migrateIntegration = !recover;
    request.recoverMigration = recover;
    struct MigrationResult {
        bool cancelled = false;
        bool requiresConfirmation = false;
        bool completed = false;
        QString sourcePath;
        QString targetPath;
        QString backupPath;
        QString planHash;
    };
    SdkMigrationPlan unrealPlan;
    const auto execute = [&]() -> MigrationResult {
        clearResults();
        setSummaryText(request.confirmMigration
            ? QCoreApplication::translate("HarkenHub", "The approved SDK migration is running...")
            : QCoreApplication::translate("HarkenHub", "Checking SDK migration safety..."));
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        if (target.engineKind == EngineKind::Unity) {
            const auto result = runUnityTask(request);
            if (!result.cancelled)
                showResults(unityResultSteps(result, !request.confirmMigration), summaryLabel_->text());
            return {result.cancelled, result.requiresConfirmation,
                    result.integrationMigrated || result.migrationRecovered,
                    result.sdkInstallPath, result.migrationTargetPath,
                    result.migrationBackupPath, result.migrationPlanHash};
        }
        ResultStep step;
        step.id = QStringLiteral("migrate_sdk");
        step.title = QCoreApplication::translate("HarkenHub", "Review SDK migration");
        MigrationResult result;
        if (!request.confirmMigration) {
            const auto plan = runIntegrationTask(this, [&]() {
                return ProjectIntegrationExecutor::planUnrealMigration(target, request.migrationTargetPath, recover);
            }, true);
            if (!plan) { scanStopped(); result.cancelled = true; return result; }
            unrealPlan = *plan;
            result.requiresConfirmation = plan->valid;
            result.planHash = QString::fromLatin1(plan->fingerprint.toHex());
            result.sourcePath = QDir(target.canonicalRoot).relativeFilePath(plan->sourceRoot);
            result.targetPath = QDir(target.canonicalRoot).relativeFilePath(plan->targetRoot);
            result.backupPath = plan->backupRoot;
            step.status = plan->valid ? ResultStatus::AwaitingConfirmation : ResultStatus::Failed;
            step.summary = plan->valid
                ? QCoreApplication::translate("HarkenHub", "Safety check passed. No SDK files were moved.")
                : localizeIntegrationText(plan->error);
        } else {
            const auto committed = *runIntegrationTask(this, [&]() {
                return ProjectIntegrationExecutor::executeUnrealMigration(target, unrealPlan, true);
            });
            result.completed = committed.success;
            result.sourcePath = committed.installedPath;
            result.backupPath = committed.backupPath;
            step.status = committed.success ? ResultStatus::Succeeded : ResultStatus::Failed;
            step.summary = committed.success
                ? QCoreApplication::translate("HarkenHub", "SDK migration completed. All installed files and project relationships were kept.")
                : localizeIntegrationText(committed.error);
        }
        step.artifacts = {unrealPlan.sourceRoot, unrealPlan.targetRoot, unrealPlan.backupRoot};
        showResults({step}, step.summary);
        return result;
    };
    std::optional<MigrationResult> reviewedResult;
    if (!recover) {
        request.migrationBackupPath = QDir(userDataDir()).filePath(
            QStringLiteral("UnityIntegration/Backups/migration-%1-%2")
                .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss")),
                     QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QDialog dialog(this);
        dialog.setWindowTitle(QCoreApplication::translate("HarkenHub", "Migrate SDK"));
        dialog.setMinimumWidth(560);
        dialog.resize(640, 360);
        auto* layout = new QVBoxLayout(&dialog);
        layout->addWidget(new QLabel(QCoreApplication::translate("HarkenHub", "New SDK installation location"), &dialog));
        auto* row = new QHBoxLayout();
        auto* path = new QLineEdit(request.sdkInstallPath, &dialog);
        path->setObjectName(QStringLiteral("sdkMigrationDestination"));
        auto* browse = new QPushButton(QCoreApplication::translate("HarkenHub", "Browse..."), &dialog);
        row->addWidget(path, 1);
        row->addWidget(browse);
        layout->addLayout(row);
        auto* safetyStatus = new QLabel(QCoreApplication::translate("HarkenHub", "Safety check required."), &dialog);
        safetyStatus->setObjectName(QStringLiteral("sdkMigrationSafetyStatus"));
        safetyStatus->setWordWrap(true);
        layout->addWidget(safetyStatus);
        auto* safetyDetails = new QPlainTextEdit(&dialog);
        safetyDetails->setObjectName(QStringLiteral("sdkMigrationSafetyDetails"));
        safetyDetails->setReadOnly(true);
        safetyDetails->setMinimumHeight(160);
        layout->addWidget(safetyDetails, 1);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
        auto* safetyCheck = buttons->addButton(QCoreApplication::translate("HarkenHub", "Safety Check"), QDialogButtonBox::ActionRole);
        safetyCheck->setObjectName(QStringLiteral("sdkMigrationSafetyCheck"));
        auto* next = buttons->addButton(QCoreApplication::translate("HarkenHub", "Continue..."), QDialogButtonBox::AcceptRole);
        next->setObjectName(QStringLiteral("sdkMigrationContinue"));
        next->setEnabled(false);
        safetyCheck->setDefault(true);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
            if (reviewedResult && reviewedResult->requiresConfirmation)
                dialog.accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        bool safetyCheckRun = false;
        connect(path, &QLineEdit::textChanged, &dialog, [&]() {
            reviewedResult.reset();
            next->setEnabled(false);
            safetyCheck->setDefault(true);
            safetyStatus->setText(QCoreApplication::translate("HarkenHub", "Safety check required."));
            safetyDetails->clear();
            if (safetyCheckRun) {
                clearResults();
                setSummaryText(QCoreApplication::translate("HarkenHub", "Migration destination changed. Safety check required."));
            }
        });
        connect(safetyCheck, &QPushButton::clicked, &dialog, [&]() {
            path->setEnabled(false);
            browse->setEnabled(false);
            buttons->setEnabled(false);
            safetyStatus->setText(QCoreApplication::translate("HarkenHub", "Checking SDK migration safety..."));
            safetyDetails->clear();
            request.migrationTargetPath = path->text().trimmed();
            reviewedResult = execute();
            safetyCheckRun = true;
            const bool passed = reviewedResult->requiresConfirmation
                && !reviewedResult->planHash.isEmpty();
            safetyStatus->setText(reviewedResult->cancelled ? summaryLabel_->text() : passed
                ? QCoreApplication::translate("HarkenHub", "Safety check passed. No SDK files were moved.")
                : QCoreApplication::translate("HarkenHub", "Safety check failed. No SDK files were moved."));
            safetyDetails->setPlainText(confirmationDetails());
            setSummaryText(safetyStatus->text());
            path->setEnabled(true);
            browse->setEnabled(true);
            buttons->setEnabled(true);
            next->setEnabled(passed);
            if (passed) next->setDefault(true);
        });
        connect(browse, &QPushButton::clicked, &dialog, [&]() {
            const QString chosen = QFileDialog::getExistingDirectory(&dialog,
                QCoreApplication::translate("HarkenHub", "Select SDK Installation Directory"),
                QDir(target.canonicalRoot).filePath(path->text()));
            if (!chosen.isEmpty()) path->setText(QDir(target.canonicalRoot).relativeFilePath(chosen));
        });
        if (dialog.exec() != QDialog::Accepted || !reviewedResult
            || !reviewedResult->requiresConfirmation || reviewedResult->planHash.isEmpty()) {
            if (safetyCheckRun) cancelPendingOperation();
            return;
        }
    }
    auto result = reviewedResult ? *reviewedResult : execute();
    if (result.cancelled) return;
    if (result.requiresConfirmation) {
        request.migrationBackupPath = result.backupPath;
        request.migrationPlanHash = result.planHash;
        const QString description = recover
            ? QCoreApplication::translate("HarkenHub", "Restore the SDK to its previous location using the migration record. Project relationships will be kept.\n\nPrevious location:\n%1\n\nMigration destination:\n%2\n\nBackup:\n%3")
            : target.engineKind == EngineKind::Unity
                ? QCoreApplication::translate("HarkenHub", "Move the installed SDK with all existing files, modifications and Unity metadata. Project relationships will be kept. A verified backup will be created first.\n\nCurrent location:\n%1\n\nNew location:\n%2\n\nBackup:\n%3\n\nClose Unity before continuing. Any paths hard-coded in your project must be updated separately.")
                : QCoreApplication::translate("HarkenHub", "Move the installed SDK with all existing files and modifications. Project relationships will be kept. A verified backup will be created first.\n\nCurrent location:\n%1\n\nNew location:\n%2\n\nBackup:\n%3\n\nClose Unreal Editor before continuing. Any paths hard-coded in your project must be updated separately.");
        if (!confirmIntegrationAction(this,
                recover ? QCoreApplication::translate("HarkenHub", "Recover SDK Migration")
                        : QCoreApplication::translate("HarkenHub", "Migrate SDK"),
                description.arg(QDir::toNativeSeparators(QDir(target.canonicalRoot).filePath(result.sourcePath)),
                                QDir::toNativeSeparators(QDir(target.canonicalRoot).filePath(result.targetPath)),
                                QDir::toNativeSeparators(result.backupPath)),
                confirmationDetails(), recover ? QCoreApplication::translate("HarkenHub", "Recover")
                                               : QCoreApplication::translate("HarkenHub", "Migrate"),
                QDialogButtonBox::AcceptRole)) {
            cancelPendingOperation();
            return;
        }
        request.confirmMigration = true;
        result = execute();
    }
    if (result.completed) {
        if (!targets_->updateSdkInstallPath(target.targetId, result.sourcePath)) {
            setSummaryText(QCoreApplication::translate("HarkenHub", "The SDK location changed, but Hub could not save it. Reopen this integration to rediscover the location.\n%1")
                .arg(localizeIntegrationText(targets_->lastError())));
        } else {
            setSummaryText(recover
                ? QCoreApplication::translate("HarkenHub", "The SDK was restored to its previous location. Project relationships were kept.")
                : target.engineKind == EngineKind::Unity
                    ? QCoreApplication::translate("HarkenHub", "SDK migration completed. All installed files, Unity GUIDs and project relationships were kept.")
                    : QCoreApplication::translate("HarkenHub", "SDK migration completed. All installed files and project relationships were kept."));
        }
    } else {
        setSummaryText(hasPendingMigration(target)
            ? QCoreApplication::translate("HarkenHub", "The SDK migration is unfinished. Choose \"Recover...\" before continuing.")
            : QCoreApplication::translate("HarkenHub", "SDK migration failed. Select the failed step for details."));
    }
    if (!result.backupPath.isEmpty())
        summaryLabel_->setToolTip(QCoreApplication::translate("HarkenHub", "Safety backup: %1").arg(result.backupPath));
}

void EngineIntegrationsPage::runSdkRemoval()
{
    if (operationRunning_ || sdkSelection_) return;
    const auto targets = targets_->list();
    const auto target = std::find_if(targets.begin(), targets.end(), [this](const auto& value) {
        return value.targetId == selectedEngineTargetId_;
    });
    if (target == targets.end()) return;
    if (target->engineKind == EngineKind::Unity) runUnityRemoval();
    else runUnrealRemoval(*target);
}

void EngineIntegrationsPage::runUnrealRemoval(const EngineTargetRecord& target)
{
    setOperationRunning(true);
    const auto finish = qScopeGuard([this]() {
        setOperationRunning(false);
        if (!inspectionStopped_) refresh();
    });
    clearResults();
    const auto plan = inspectUnrealTask(target, {}, IntegrationOperation::Remove);
    if (plan.cancelled) return;
    ResultStep review;
    review.id = QStringLiteral("review_removal");
    review.title = QCoreApplication::translate("HarkenHub", "Safety Check");
    review.status = plan.valid ? ResultStatus::CheckPassed : ResultStatus::Failed;
    review.summary = plan.valid
        ? QCoreApplication::translate("HarkenHub", "Safety check passed. No SDK files were changed.") : plan.error;
    review.details = plan.preservedTargetFiles;
    review.artifacts = {plan.targetRoot};
    showResults({review}, review.summary);
    if (!plan.valid) return;
    if (!confirmIntegrationAction(this,
            QCoreApplication::translate("HarkenHub", "Uninstall SDK"),
            QCoreApplication::translate("HarkenHub", "Uninstall the SDK from this game project? A backup will be kept in Saved/HarkenHub/Backups. Project-specific files and the project relationship will be kept.\n\n%1")
                .arg(QDir::toNativeSeparators(plan.targetRoot)),
            confirmationDetails(), QCoreApplication::translate("HarkenHub", "Uninstall SDK"),
            QDialogButtonBox::DestructiveRole)) {
        cancelPendingOperation();
        return;
    }
    const auto result = *runIntegrationTask(this, [target, plan]() {
        return ProjectIntegrationExecutor::executeUnreal(target, plan, IntegrationOperation::Remove, true);
    });
    ResultStep removal;
    removal.id = QStringLiteral("remove_sdk");
    removal.title = QCoreApplication::translate("HarkenHub", "Uninstall SDK");
    removal.status = result.success ? ResultStatus::Succeeded : ResultStatus::Failed;
    removal.summary = result.success
        ? QCoreApplication::translate("HarkenHub", "SDK removed. Project-specific files and the project relationship were kept.")
        : result.error;
    if (!result.backupPath.isEmpty()) removal.artifacts = {result.backupPath};
    showResults({review, removal}, removal.summary, removal.id);
}

void EngineIntegrationsPage::runUnityRemoval()
{
    if (operationRunning_ || sdkSelection_) return;
    const auto targets = targets_->list();
    const auto selected = std::find_if(
        targets.begin(), targets.end(), [&](const EngineTargetRecord& value) {
            return value.targetId == selectedEngineTargetId_
                && samePath(targetInputPath(value),
                            engineProjectEdit_->text().trimmed());
        });
    if (selected == targets.end()
        || selected->engineKind != EngineKind::Unity) {
        QMessageBox::warning(
            this, QCoreApplication::translate("HarkenHub", "Uninstall Unity SDK"),
            QCoreApplication::translate("HarkenHub", "The selected Unity project is no longer available."));
        return;
    }
    const EngineTargetRecord target = *selected;
    setOperationRunning(true);
    const auto finish = qScopeGuard([this]() { setOperationRunning(false); });

    removeInstalledSdkButton_->setEnabled(false);

    UnityIntegrationRequest request;
    request.unityProjectPath = target.canonicalRoot;
    request.sdkInstallPath = sdkInstallPathEdit_->text().trimmed();
    request.removeIntegration = true;

    auto execute = [this, &request]() {
        clearResults();
        setSummaryText(request.confirmRemoval
            ? QCoreApplication::translate("HarkenHub", "Removing the approved Unity SDK integration...")
            : QCoreApplication::translate("HarkenHub", "Reviewing the installed Unity SDK and safety backup plan..."));
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        const UnityIntegrationResult result =
            runUnityTask(request);
        if (result.cancelled) return result;
        showResults(unityResultSteps(result, !request.confirmRemoval), summaryLabel_->text(),
                    request.confirmRemoval ? QStringLiteral("remove_integration")
                                           : QStringLiteral("inspect_installation"));
        return result;
    };

    UnityIntegrationResult result = execute();
    if (result.cancelled) return;
    if (result.requiresConfirmation) {
        request.removalBackupPath = result.removalBackupPath;
        if (confirmIntegrationAction(
                this,
                QCoreApplication::translate(
                    "HarkenHub", "Uninstall Unity SDK"),
                QCoreApplication::translate(
                    "HarkenHub",
                    "A verified backup will be created before Harken-managed Unity SDK and runtime files are removed. Project-specific files and the project relationship will be kept.\n\nBackup folder:\n%1\n\nClose Unity before continuing.")
                    .arg(QDir::toNativeSeparators(result.removalBackupPath)),
                confirmationDetails(),
                QCoreApplication::translate(
                    "HarkenHub", "Uninstall SDK"),
                QDialogButtonBox::DestructiveRole)) {
            request.confirmRemoval = true;
            result = execute();
        } else {
            cancelPendingOperation();
            return;
        }
    }

    if (result.integrationRemoved) {
        if (!result.success) {
            setSummaryText(QCoreApplication::translate(
                "HarkenHub",
                "The Unity SDK was backed up and removed, but the removal report could not be completed. Backup: %1")
                .arg(QDir::toNativeSeparators(result.removalBackupPath)));
        } else if (result.hasWarnings) {
            setSummaryText(QCoreApplication::translate(
                "HarkenHub",
                "The Unity SDK and runtimes were backed up and removed with a cleanup warning. Backup: %1")
                .arg(QDir::toNativeSeparators(result.removalBackupPath)));
        } else {
            setSummaryText(QCoreApplication::translate(
                "HarkenHub",
                "The Unity SDK and runtimes were backed up and removed. %1 project-specific file(s) were preserved. Backup: %2")
                .arg(result.targetOnlyFilesPreserved)
                .arg(QDir::toNativeSeparators(result.removalBackupPath)));
        }
    } else if (result.success && !result.integrationFound) {
        setSummaryText(QCoreApplication::translate(
            "HarkenHub", "No Harken Unity SDK is installed in this project."));
    } else if (result.removalBackupCreated) {
        setSummaryText(QCoreApplication::translate(
            "HarkenHub",
            "The safety backup was created, but the Unity SDK could not be removed. The project was restored or left unchanged. Backup: %1")
            .arg(QDir::toNativeSeparators(result.removalBackupPath)));
    } else {
        setSummaryText(QCoreApplication::translate(
            "HarkenHub",
            "Unity SDK removal failed. Select the first failed step for details."));
    }
    if (!result.textReportPath.isEmpty())
        summaryLabel_->setToolTip(QCoreApplication::translate(
            "HarkenHub", "Removal report: %1").arg(result.textReportPath));
    updateRemovalButtonState();
}

bool EngineIntegrationsPage::runUnity(
    const EngineTargetRecord& target, const QString& sdkPath, bool checkOnly)
{
    const bool updating = sdkInstalled_;
    const QByteArray approved = reviewedSdkHash_;
    clearResults();
    setSummaryText(checkOnly
        ? QCoreApplication::translate("HarkenHub", "Checking only. No SDK files will be changed...")
        : QCoreApplication::translate("HarkenHub", "The approved SDK integration is running..."));
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    UnityIntegrationRequest request;
    request.unityProjectPath = target.canonicalRoot;
    request.sdkSourcePath = sdkPath;
    request.sdkInstallPath = sdkInstallPathEdit_->text().trimmed();
    request.checkOnly = checkOnly;
    request.confirmTargetChanges = !checkOnly;
    if (!checkOnly) request.approvedPlanHash = approved;
    const UnityIntegrationResult result =
        runUnityTask(request);
    if (result.cancelled) return false;
    if (checkOnly && result.success) reviewedSdkHash_ = result.planHash;
    if (!checkOnly) sdkReviewInvalidated_ = result.requiresConfirmation;
    sdkReportPath_ = QFileInfo::exists(result.textReportPath) ? result.textReportPath : QString();
    const QVector<ResultStep> steps = unityResultSteps(result, checkOnly);
    sdkComparison_ = !result.success ? SdkComparison::Blocked
        : (checkOnly ? result.targetAlreadyCurrent : result.targetCurrent)
            ? SdkComparison::Current : SdkComparison::Different;

    QString summary;
    if (checkOnly) {
        if (!result.success)
            summary = QCoreApplication::translate("HarkenHub", "SDK check failed. Select the first failed step for details.");
        else if (!result.targetInstalled)
            summary = QCoreApplication::translate("HarkenHub", "Check completed. The selected SDK is valid and is not installed in this Unity project.");
        else if (result.changesDetected)
            summary = QCoreApplication::translate("HarkenHub", "Check completed. Differences were found; no SDK files were changed.");
        else if (result.hasWarnings)
            summary = QCoreApplication::translate("HarkenHub", "Check completed with guidance for additional target platforms.");
        else
            summary = QCoreApplication::translate("HarkenHub", "Check completed. The installed Unity SDK matches the selected source.");
    } else if (!result.success) {
        summary = QCoreApplication::translate("HarkenHub", "SDK integration failed. Select the first failed step for details.");
    } else if (result.targetAlreadyCurrent) {
        summary = QCoreApplication::translate("HarkenHub", "The Unity SDK is already current and passed verification.");
    } else if (result.hasWarnings) {
        summary = QCoreApplication::translate("HarkenHub", "SDK integration completed with warnings. Review the highlighted steps.");
    } else {
        summary = updating
            ? QCoreApplication::translate("HarkenHub", "SDK update complete. Installed files passed verification.")
            : QCoreApplication::translate("HarkenHub", "SDK installation complete. Installed files passed verification.");
    }
    showResults(steps, summary, checkOnly ? QStringLiteral("compare_sdk")
                                        : QStringLiteral("install_sdk"));
    if (!result.textReportPath.isEmpty())
        summaryLabel_->setToolTip(
            QCoreApplication::translate("HarkenHub", "Report: %1").arg(result.textReportPath));
    updateRemovalButtonState();
    if (!checkOnly && result.targetCurrent
        && !targets_->updateSdkInstallPath(target.targetId, result.sdkInstallPath)) {
        setSummaryText(summary + QLatin1Char('\n') + QCoreApplication::translate(
            "HarkenHub", "The SDK was installed, but its installation location could not be saved.\n%1")
                .arg(targets_->lastError()));
        return false;
    }
    return result.success;
}

bool EngineIntegrationsPage::runUnreal(
    const EngineTargetRecord& target, const QString& sdkPath, bool checkOnly)
{
    const auto reviewed = reviewedUnrealPlan_;
    clearResults();
    setSummaryText(checkOnly
        ? QCoreApplication::translate("HarkenHub", "Checking only. No SDK files will be changed...")
        : QCoreApplication::translate("HarkenHub", "The approved SDK integration is running..."));
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    QVector<ResultStep> steps;
    ResultStep targetStep;
    targetStep.id = QStringLiteral("validate_target");
    targetStep.title = QCoreApplication::translate("HarkenHub", "Validate Unreal project");
    targetStep.status = ResultStatus::Succeeded;
    targetStep.summary = QCoreApplication::translate("HarkenHub", "The Unreal project descriptor is valid.");
    targetStep.artifacts = {target.descriptorPath};
    steps.push_back(targetStep);

    const EngineTargetObservation observation = observeTarget(target);
    if (inspectionStopped_) return false;
    const IntegrationOperation operation =
        observation.health == EngineTargetHealth::NotInstalled
        ? IntegrationOperation::Install
        : IntegrationOperation::Update;
    auto executionTarget = target;
    if (operation == IntegrationOperation::Install)
        executionTarget.sdkInstallPath = sdkInstallPathEdit_->text().trimmed();
    const IntegrationPlan plan = checkOnly
        ? inspectUnrealTask(executionTarget, sdkPath, operation)
        : reviewed.value_or(IntegrationPlan{});
    if (plan.cancelled) return false;
    if (checkOnly && plan.valid) {
        reviewedUnrealPlan_ = plan;
        reviewedSdkHash_ = plan.fingerprint;
    }
    sdkComparison_ = !plan.valid ? SdkComparison::Blocked
        : plan.changes.empty() ? SdkComparison::Current : SdkComparison::Different;

    ResultStep sourceStep;
    sourceStep.id = QStringLiteral("validate_sdk");
    sourceStep.title = QCoreApplication::translate("HarkenHub", "Validate SDK source");
    sourceStep.status = plan.valid ? ResultStatus::Succeeded
                                   : ResultStatus::Failed;
    sourceStep.summary = plan.valid
        ? QCoreApplication::translate("HarkenHub", "Validated Unreal SDK %1.").arg(plan.version)
        : QCoreApplication::translate("HarkenHub", "The Unreal SDK source or installed plugin is not safe to use.");
    sourceStep.cause = localizeIntegrationText(plan.error);
    sourceStep.remediation = plan.valid
        ? QString()
        : QCoreApplication::translate("HarkenHub", "Select a complete Harken Unreal SDK source or repair the existing managed plugin, then check again.");
    sourceStep.artifacts = {sdkPath};
    steps.push_back(sourceStep);

    ResultStep compareStep;
    compareStep.id = QStringLiteral("compare_sdk");
    compareStep.title = QCoreApplication::translate("HarkenHub", "Compare installed SDK");
    compareStep.hasFileComparison = plan.valid;
    compareStep.attentionRequired = !plan.modifiedManagedFiles.isEmpty();
    compareStep.modifiedFiles = plan.modifiedManagedFiles;
    compareStep.missingFiles = plan.missingManagedFiles;
    for (const auto& path : plan.missingManagedFiles) compareStep.modifiedFiles.removeAll(path);
    for (const auto& path : plan.unreadableManagedFiles) compareStep.modifiedFiles.removeAll(path);
    if (plan.valid) {
        compareStep.fileChanges = plan.changes;
        for (const QString& path : plan.preservedTargetFiles)
            compareStep.fileChanges.push_back({path, QStringLiteral("preserve")});
    }
    if (!plan.valid) {
        compareStep.status = ResultStatus::Skipped;
        compareStep.summary = QCoreApplication::translate("HarkenHub", "Comparison was skipped because SDK validation failed.");
    } else if (plan.changes.empty()) {
        compareStep.status = ResultStatus::Succeeded;
        compareStep.summary = QCoreApplication::translate("HarkenHub", "The installed Unreal SDK matches the selected source.");
    } else {
        compareStep.status = checkOnly || !plan.modifiedManagedFiles.isEmpty()
            ? ResultStatus::Warning : ResultStatus::Succeeded;
        compareStep.summary = plan.modifiedManagedFiles.isEmpty()
            ? QCoreApplication::translate("HarkenHub", "%1 SDK file change(s) were found.")
                  .arg(plan.changes.size())
            : QCoreApplication::translate("HarkenHub", "%1 SDK file change(s) were found, including %2 locally modified managed file(s) that require confirmation before overwrite.")
                  .arg(plan.changes.size())
                  .arg(plan.modifiedManagedFiles.size());
        if (!plan.modifiedManagedFiles.isEmpty()) {
            compareStep.details.push_back(
                QCoreApplication::translate("HarkenHub", "Locally modified managed files:\n%1")
                    .arg(plan.modifiedManagedFiles.join(QLatin1Char('\n'))));
        }
        if (!plan.preservedTargetFiles.isEmpty()) {
            compareStep.details.push_back(
                QCoreApplication::translate("HarkenHub", "Target-only plugin files that will be preserved:\n%1")
                    .arg(plan.preservedTargetFiles.join(QLatin1Char('\n'))));
        }
        for (const IntegrationFileChange& change : plan.changes) {
            compareStep.details.push_back(
                QStringLiteral("%1: %2")
                    .arg(localizeFileChangeAction(change.action), change.path));
        }
    }
    steps.push_back(compareStep);

    ResultStep actionStep;
    actionStep.id = QStringLiteral("apply_sdk");
    actionStep.title = QCoreApplication::translate("HarkenHub", "Apply SDK integration");
    if (!plan.valid) {
        actionStep.status = ResultStatus::Skipped;
        actionStep.summary = QCoreApplication::translate("HarkenHub", "Integration was not run because validation failed.");
        steps.push_back(actionStep);
        showResults(steps, QCoreApplication::translate("HarkenHub", "SDK check failed. Select the failed step for details."));
        return false;
    }
    if (checkOnly) {
        actionStep.status = plan.changes.empty() ? ResultStatus::NotRun : ResultStatus::AwaitingConfirmation;
        actionStep.summary = QCoreApplication::translate("HarkenHub", "Check-only mode requested; no SDK files were changed.");
        steps.push_back(actionStep);
        for (auto& step : steps)
            if (step.status == ResultStatus::Succeeded) step.status = ResultStatus::CheckPassed;
        showResults(steps, plan.changes.empty()
            ? QCoreApplication::translate("HarkenHub", "Check completed. The installed Unreal SDK matches the selected source.")
            : QCoreApplication::translate("HarkenHub", "Check completed. Differences were found; no SDK files were changed."),
                    QStringLiteral("compare_sdk"));
        return true;
    }

    const IntegrationOperationResult result =
        *runIntegrationTask(this, [executionTarget, plan, operation]() {
            return ProjectIntegrationExecutor::executeUnreal(executionTarget, plan, operation, true);
        }, false, sdkSelection_ ? sdkProgress_ : nullptr);
    sdkReviewInvalidated_ = result.reviewRequired;
    sdkComparison_ = result.success ? SdkComparison::Current : SdkComparison::Blocked;
    actionStep.status = result.success ? ResultStatus::Succeeded
                                       : ResultStatus::Failed;
    actionStep.summary = result.success
        ? (plan.changes.empty()
               ? QCoreApplication::translate("HarkenHub", "The Unreal SDK is already current.")
               : QCoreApplication::translate("HarkenHub", "The Unreal SDK integration completed successfully."))
        : QCoreApplication::translate("HarkenHub", "The Unreal SDK integration failed.");
    actionStep.cause = localizeIntegrationText(result.error);
    actionStep.remediation = result.success
        ? QString()
        : QCoreApplication::translate("HarkenHub", "Close Unreal Editor, check project permissions and file locks, then try again.");
    if (!result.backupPath.isEmpty())
        actionStep.artifacts = {result.backupPath};
    steps.push_back(actionStep);
    showResults(steps, result.success
        ? QCoreApplication::translate("HarkenHub", "SDK integration completed successfully.")
        : QCoreApplication::translate("HarkenHub", "SDK integration failed. Select the failed step for details."),
                QStringLiteral("apply_sdk"));
    if (result.success && !targets_->updateSdkInstallPath(target.targetId, result.installedPath))
        setSummaryText(QCoreApplication::translate("HarkenHub", "The SDK location changed, but Hub could not save it. Reopen this integration to rediscover the location.\n%1")
            .arg(localizeIntegrationText(targets_->lastError())));
    return result.success;
}

void EngineIntegrationsPage::showResults(
    const QVector<ResultStep>& steps, const QString& summary,
    const QString& preferredStep)
{
    const QSignalBlocker blocker(stepsTable_);
    resultSteps_ = steps;
    stepsTable_->clearContents();
    stepsTable_->setRowCount(steps.size());
    int selectedRow = -1;
    for (int row = 0; row < steps.size(); ++row) {
        const ResultStep& step = steps[row];
        const QStringList values = {
            step.title, statusText(step.status), step.summary,
        };
        for (int column = 0; column < values.size(); ++column) {
            auto* item = new QTableWidgetItem(values[column]);
            if (step.status == ResultStatus::Failed)
                item->setForeground(QColor(QStringLiteral("#FF6B6B")));
            else if (step.status == ResultStatus::Warning)
                item->setForeground(QColor(QStringLiteral("#F0C36A")));
            else if (step.status == ResultStatus::Succeeded)
                item->setForeground(QColor(QStringLiteral("#69D391")));
            stepsTable_->setItem(row, column, item);
        }
        if (step.status == ResultStatus::Failed
            && (selectedRow < 0 || steps[selectedRow].status != ResultStatus::Failed)) {
            selectedRow = row;
        } else if (step.id == preferredStep
                   && (selectedRow < 0 || steps[selectedRow].status != ResultStatus::Failed)) {
            selectedRow = row;
        }
    }
    setSummaryText(summary);
    updateSdkCheckResults();
    if (selectedRow < 0 && !steps.isEmpty())
        selectedRow = 0;
    stepsTable_->resizeRowsToContents();
    detailEdit_->setVisible(selectedRow >= 0);
    if (selectedRow >= 0) {
        stepsTable_->setCurrentCell(selectedRow, 0,
                                    QItemSelectionModel::ClearAndSelect
                                        | QItemSelectionModel::Rows);
        showSelectedStep();
    } else {
        detailEdit_->clear();
    }
}

void EngineIntegrationsPage::showSelectedStep()
{
    const int row = stepsTable_->currentRow();
    if (row >= 0 && row < resultSteps_.size())
        detailEdit_->setPlainText(formatStepDetails(resultSteps_[row]));
}

void EngineIntegrationsPage::updateSdkCheckResults()
{
    sdkDiffTree_->clear();
    const ResultStep* comparison = nullptr;
    const ResultStep* failure = nullptr;
    int warnings = 0;
    QVector<const ResultStep*> logSteps;
    for (const auto& step : resultSteps_) {
        logSteps.push_back(&step);
        if (step.hasFileComparison) comparison = &step;
        if (step.status == ResultStatus::Failed && !failure) failure = &step;
        if (step.attentionRequired || (step.status == ResultStatus::Warning
            && step.id != QStringLiteral("compare_sdk"))) ++warnings;
    }
    const auto priority = [](const ResultStep* step) {
        if (step->status == ResultStatus::Failed) return 0;
        if (step->attentionRequired || (step->status == ResultStatus::Warning
            && step->id != QStringLiteral("compare_sdk"))) return 1;
        return step->hasFileComparison ? 2 : 3;
    };
    std::stable_sort(logSteps.begin(), logSteps.end(), [&](const auto* left, const auto* right) {
        return priority(left) < priority(right);
    });
    QString log;
    for (const auto* step : logSteps) log += formatStepHtml(*step);
    sdkSelectionDetails_->setHtml(log);
    sdkResultsTabs_->setTabText(1, sdkPhase_ == SdkPhase::Applying
        ? QCoreApplication::translate("HarkenHub", "Results and Logs")
        : warnings > 0 ? QCoreApplication::translate("HarkenHub", "Check Log (warnings: %1)").arg(warnings)
                       : QCoreApplication::translate("HarkenHub", "Check Log"));
    sdkResultsTabs_->setCurrentIndex(failure || !comparison ? 1 : 0);
    if (failure)
        sdkSelectionSummary_->setText(sdkPhase_ == SdkPhase::Applying
            ? QCoreApplication::translate("HarkenHub", "SDK integration failed. Review the log for details.")
            : QCoreApplication::translate("HarkenHub", "SDK check failed. Review the check log for details."));
    if (!comparison) {
        sdkDiffSummary_->clear();
        return;
    }

    QStringList summary;
    const bool applied = sdkPhase_ == SdkPhase::Applying && sdkComparison_ == SdkComparison::Current;
    const auto addAttention = [&](const QStringList& files, const char* label, bool changedOnly) {
        QStringList affected;
        for (const auto& path : files) {
            if (!changedOnly || std::any_of(comparison->fileChanges.begin(), comparison->fileChanges.end(),
                [&](const auto& change) { return change.path == path && change.action != QStringLiteral("preserve"); }))
                affected << path;
        }
        if (affected.isEmpty()) return;
        affected.sort();
        const QString title = QCoreApplication::translate("HarkenHub", label).arg(affected.size());
        summary << QStringLiteral("<span style='color:#F0C36A'><b>%1</b></span>").arg(title.toHtmlEscaped());
        auto* section = new QTreeWidgetItem(sdkDiffTree_, {title});
        section->setForeground(0, QColor(QStringLiteral("#F0C36A")));
        for (const auto& path : affected) new QTreeWidgetItem(section, {path});
        section->setExpanded(true);
    };
    addAttention(comparison->modifiedFiles, applied
        ? QT_TRANSLATE_NOOP("HarkenHub", "Locally modified files overwritten or removed: %1")
        : QT_TRANSLATE_NOOP("HarkenHub", "Locally modified files to overwrite or remove: %1"), true);
    addAttention(comparison->missingFiles, applied
        ? QT_TRANSLATE_NOOP("HarkenHub", "SDK files missing before the change: %1")
        : QT_TRANSLATE_NOOP("HarkenHub", "Missing installed SDK files: %1"), false);
    struct ChangeGroup
    {
        const char* action;
        const char* color;
        const char* countText;
        const char* completedText;
    };
    const ChangeGroup groups[] = {
        {"add", "#69D391", QT_TRANSLATE_NOOP("HarkenHub", "Add %1"), QT_TRANSLATE_NOOP("HarkenHub", "Files added")},
        {"replace", "#83B9FF", QT_TRANSLATE_NOOP("HarkenHub", "Update %1"), QT_TRANSLATE_NOOP("HarkenHub", "Files updated")},
        {"remove", "#FF8B8B", QT_TRANSLATE_NOOP("HarkenHub", "Remove %1"), QT_TRANSLATE_NOOP("HarkenHub", "Files removed")},
        {"preserve", "#9AA6B8", QT_TRANSLATE_NOOP("HarkenHub", "Keep %1"), QT_TRANSLATE_NOOP("HarkenHub", "Files kept unchanged")},
    };
    QStringList counts;
    int modified = 0;
    for (const auto& group : groups) {
        const QString action = QString::fromLatin1(group.action);
        QStringList paths;
        for (const auto& change : comparison->fileChanges)
            if (change.action == action) paths << change.path;
        paths.sort();
        const bool preserved = action == QStringLiteral("preserve");
        if (!preserved) modified += paths.size();
        counts << QStringLiteral("<span style='color:%1'><b>%2</b></span>")
            .arg(QLatin1String(group.color), QCoreApplication::translate("HarkenHub", group.countText)
                .arg(paths.size()).toHtmlEscaped());
        if (paths.isEmpty()) continue;
        auto* section = new QTreeWidgetItem(sdkDiffTree_,
            {QStringLiteral("%1 (%2)").arg(applied
                ? QCoreApplication::translate("HarkenHub", group.completedText)
                : localizeFileChangeAction(action)).arg(paths.size())});
        QFont font = section->font(0);
        font.setBold(true);
        section->setFont(0, font);
        section->setForeground(0, QColor(QLatin1String(group.color)));
        for (const auto& path : paths) {
            auto* item = new QTreeWidgetItem(section, {path});
            item->setToolTip(0, path);
        }
        section->setExpanded(!preserved);
    }
    summary << counts.join(QStringLiteral("&nbsp;&nbsp;&nbsp;&nbsp;"));
    if (modified == 0)
        summary << QStringLiteral("<span style='color:#69D391'>%1</span>").arg(
            QCoreApplication::translate("HarkenHub", "No SDK files need to be changed.").toHtmlEscaped());
    if (failure)
        summary.prepend(QStringLiteral("<span style='color:#FF8B8B'><b>%1</b></span>")
            .arg(failure->summary.toHtmlEscaped()));
    if (warnings > 0)
        summary << QStringLiteral("<a href='log' style='color:#F0C36A'>%1</a>").arg(
            QCoreApplication::translate("HarkenHub", "Items needing attention: %1. View check log.")
                .arg(warnings).toHtmlEscaped());
    sdkDiffSummary_->setText(summary.join(QStringLiteral("<br>")));
}

QString EngineIntegrationsPage::formatStepHtml(const ResultStep& step) const
{
    const auto text = [](const QString& value) {
        return value.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    };
    const QString color = step.status == ResultStatus::Failed ? QStringLiteral("#FF8B8B")
        : step.status == ResultStatus::Warning || step.status == ResultStatus::AwaitingConfirmation
            ? QStringLiteral("#F0C36A")
        : step.status == ResultStatus::Succeeded || step.status == ResultStatus::CheckPassed
            ? QStringLiteral("#69D391") : QStringLiteral("#9AA6B8");
    const QString summary = step.status == ResultStatus::Failed
        ? QStringLiteral("<span style='color:%1'>%2</span>").arg(color, text(step.summary))
        : text(step.summary);
    QString html = QStringLiteral("<h3 style='margin-top:18px;margin-bottom:6px'>%1"
        " &nbsp; <span style='color:%2;font-size:small'>%3</span></h3><p><b>%4</b></p>")
        .arg(text(step.title), color, text(statusText(step.status)), summary);
    if (!step.cause.isEmpty()) {
        const QStringList accessHeadings{
            QCoreApplication::translate("HarkenHub", "Read-only files:\n%1").section(QLatin1Char('\n'), 0, 0),
            QCoreApplication::translate("HarkenHub", "Files that could not be opened for writing:\n%1").section(QLatin1Char('\n'), 0, 0)};
        QStringList lines;
        for (const auto& line : step.cause.split(QLatin1Char('\n'))) {
            lines << (step.status == ResultStatus::Failed && accessHeadings.contains(line)
                ? QStringLiteral("<span style='color:%1'><b>%2</b></span>").arg(color, text(line))
                : text(line));
        }
        html += QStringLiteral("<p>%1</p>").arg(lines.join(QStringLiteral("<br>")));
    }
    if (!step.remediation.isEmpty())
        html += QStringLiteral("<p style='color:%1'><b>%2</b></p>")
            .arg(color, text(QCoreApplication::translate("HarkenHub", "Recommended action:\n%1")
                .arg(step.remediation)));
    for (const auto& detail : step.details) {
        const int newline = detail.indexOf(QLatin1Char('\n'));
        html += newline < 0 ? QStringLiteral("<p>%1</p>").arg(text(detail))
            : QStringLiteral("<p><b>%1</b><br>%2</p>")
                .arg(text(detail.left(newline)), text(detail.mid(newline + 1)));
    }
    if (!step.artifacts.isEmpty())
        html += QStringLiteral("<p style='color:#9AA6B8'>%1</p>").arg(text(
            QCoreApplication::translate("HarkenHub", "Files:\n%1").arg(step.artifacts.join(QLatin1Char('\n')))));
    if (step.elapsedMs > 0)
        html += QStringLiteral("<p style='color:#9AA6B8;font-size:small'>%1</p>").arg(text(
            QCoreApplication::translate("HarkenHub", "Elapsed: %1 ms").arg(step.elapsedMs)));
    return html + QStringLiteral("<hr>");
}

QString EngineIntegrationsPage::formatStepDetails(
    const ResultStep& step) const
{
    QStringList sections;
    sections << step.title;
    sections << QCoreApplication::translate("HarkenHub", "Status: %1").arg(statusText(step.status));
    if (!step.summary.isEmpty())
        sections << step.summary;
    if (!step.cause.isEmpty())
        sections << QCoreApplication::translate("HarkenHub", "Context:\n%1").arg(step.cause);
    if (!step.remediation.isEmpty())
        sections << QCoreApplication::translate("HarkenHub", "Recommended action:\n%1").arg(step.remediation);
    if (!step.details.isEmpty())
        sections << QCoreApplication::translate("HarkenHub", "Details:\n%1")
                        .arg(step.details.join(QStringLiteral("\n\n")));
    if (!step.artifacts.isEmpty())
        sections << QCoreApplication::translate("HarkenHub", "Files:\n%1")
                        .arg(step.artifacts.join(QLatin1Char('\n')));
    if (step.elapsedMs > 0)
        sections << QCoreApplication::translate("HarkenHub", "Elapsed: %1 ms").arg(step.elapsedMs);
    return sections.join(QStringLiteral("\n\n"));
}

QString EngineIntegrationsPage::confirmationDetails() const
{
    QStringList details;
    for (const ResultStep& step : resultSteps_)
        details << formatStepDetails(step);
    return details.join(QStringLiteral("\n\n----------------\n\n"));
}

QString EngineIntegrationsPage::statusText(ResultStatus status) const
{
    switch (status) {
        case ResultStatus::Pending:
            return QCoreApplication::translate("HarkenHub", "Pending");
        case ResultStatus::Running:
            return QCoreApplication::translate("HarkenHub", "Running");
        case ResultStatus::Succeeded:
            return QCoreApplication::translate("HarkenHub", "Succeeded");
        case ResultStatus::Warning:
            return QCoreApplication::translate("HarkenHub", "Warning");
        case ResultStatus::Failed:
            return QCoreApplication::translate("HarkenHub", "Failed");
        case ResultStatus::Skipped:
            return QCoreApplication::translate("HarkenHub", "Skipped");
        case ResultStatus::CheckPassed:
            return QCoreApplication::translate("HarkenHub", "Check Passed");
        case ResultStatus::AwaitingConfirmation:
            return QCoreApplication::translate("HarkenHub", "Awaiting Confirmation");
        case ResultStatus::NotRun:
            return QCoreApplication::translate("HarkenHub", "Not Run");
    }
    return {};
}

void EngineIntegrationsPage::showConflict(
    const QString& integrationId, const QString& reason)
{
    const auto integrations = integrations_->list();
    const auto integration = std::find_if(
        integrations.begin(), integrations.end(),
        [&integrationId](const IntegrationRecord& value) {
            return value.integrationId == integrationId;
        });
    QString details = reason;
    if (integration != integrations.end()) {
        const auto targets = targets_->list();
        const auto target = std::find_if(
            targets.begin(), targets.end(), [&](const EngineTargetRecord& value) {
                return value.targetId == integration->engineTargetId;
            });
        details += QCoreApplication::translate("HarkenHub", "\n\nHarken project:\n%1")
                       .arg(QDir::toNativeSeparators(
                           integration->harkenProjectPath));
        if (target != targets.end()) {
            details += QCoreApplication::translate("HarkenHub", "\n\nEngine project:\n%1")
                           .arg(QDir::toNativeSeparators(
                               targetInputPath(*target)));
        }
    }
    QMessageBox::information(this, QCoreApplication::translate("HarkenHub", "Integration Already Exists"),
                             details);
    showList();
    highlightIntegration(integrationId);
}

QString EngineIntegrationsPage::conflictingIntegration(
    const QString& harkenProjectPath, const QString& engineTargetId) const
{
    for (const IntegrationRecord& integration : integrations_->list()) {
        if (integration.integrationId == editingIntegrationId_)
            continue;
        if (!harkenProjectPath.isEmpty()
            && samePath(integration.harkenProjectPath, harkenProjectPath)) {
            return integration.integrationId;
        }
        if (!engineTargetId.isEmpty()
            && integration.engineTargetId == engineTargetId) {
            return integration.integrationId;
        }
    }
    return {};
}

void EngineIntegrationsPage::highlightIntegration(
    const QString& integrationId)
{
    highlightedIntegrationId_ = integrationId;
    highlightDeadlineMs_ = QDateTime::currentMSecsSinceEpoch() + 3000;
    highlightPhase_ = true;
    for (int row = 0; row < table_->rowCount(); ++row) {
        const QTableWidgetItem* item = table_->item(row, kHarkenProjectColumn);
        if (item && item->data(Qt::UserRole).toString() == integrationId) {
            table_->scrollToItem(item, QAbstractItemView::PositionAtCenter);
            break;
        }
    }
    applyHighlight();
    highlightTimer_->start();
}

void EngineIntegrationsPage::advanceHighlight()
{
    if (QDateTime::currentMSecsSinceEpoch() >= highlightDeadlineMs_) {
        highlightTimer_->stop();
        highlightPhase_ = false;
        applyHighlight();
        highlightedIntegrationId_.clear();
        highlightDeadlineMs_ = 0;
        return;
    }
    highlightPhase_ = !highlightPhase_;
    applyHighlight();
}

void EngineIntegrationsPage::applyHighlight()
{
    table_->clearSelection();
    if (!highlightPhase_ || highlightedIntegrationId_.isEmpty())
        return;
    for (int row = 0; row < table_->rowCount(); ++row) {
        const QTableWidgetItem* item = table_->item(row, kHarkenProjectColumn);
        if (!item
            || item->data(Qt::UserRole).toString()
                   != highlightedIntegrationId_) {
            continue;
        }
        table_->selectionModel()->select(
            table_->model()->index(row, kHarkenProjectColumn),
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        table_->viewport()->update();
        return;
    }
}

bool EngineIntegrationsPage::persistIntegration(
    const QString& harkenProjectPath, const QString& engineTargetId,
    const QString& sdkSourcePath)
{
    if (!editingIntegrationId_.isEmpty()) {
        if (integrations_->updateSdkSourcePath(editingIntegrationId_,
                                               sdkSourcePath)) {
            return true;
        }
        QMessageBox::warning(
            this, QCoreApplication::translate("HarkenHub", "Manage Integration"),
            QCoreApplication::translate("HarkenHub", "The SDK was integrated, but the Runtime / SDK path could not be saved.\n\n%1")
                .arg(localizeIntegrationText(integrations_->lastError())));
        return false;
    }
    IntegrationRecord integration;
    integration.integrationId =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    integration.harkenProjectPath = harkenProjectPath;
    integration.engineTargetId = engineTargetId;
    integration.sdkSourcePath = sdkSourcePath;
    integration.createdAt =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    integration.updatedAt = integration.createdAt;
    if (!integrations_->add(integration, false)) {
        QMessageBox::warning(
            this, QCoreApplication::translate("HarkenHub", "Manage Integration"),
            QCoreApplication::translate("HarkenHub", "The project relationship could not be saved.\n\n%1")
                .arg(localizeIntegrationText(integrations_->lastError())));
        return false;
    }
    editingIntegrationId_ = integration.integrationId;
    setEditorLocked(true);
    setSummaryText(
        summaryLabel_->text() + QStringLiteral("\n\n")
        + QCoreApplication::translate("HarkenHub", "The integration relationship was added to Engine Integrations."));
    return true;
}

}
