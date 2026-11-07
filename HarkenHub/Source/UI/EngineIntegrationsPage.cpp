#include "UI/EngineIntegrationsPage.h"
#include "UI/DialogLayout.h"
#include "UI/MessageDialog.h"

#include "UI/HubIcons.h"
#include "UI/IntegrationTaskDialog.h"

#include "Integrations/EngineTargetProbe.h"
#include "Integrations/EngineTargetStore.h"
#include "Integrations/HubUnityIntegrationService.h"
#include "Integrations/OfficialUnitySource.h"
#include "Security/TrustAnchors.h"
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
#include <QStyledItemDelegate>
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
constexpr int kIntegrationStatusColumn = 3;
constexpr int kIntegrationVersionColumn = 4;
constexpr int kActionColumn = 5;
constexpr int kPathButtonMinimumWidth = 126;
constexpr int kProjectFolderRole = Qt::UserRole + 1;
constexpr int kIntegrationVersionRole = Qt::UserRole + 1;
constexpr int kIntegrationPackageNameRole = Qt::UserRole + 2;

QString integrationPackageName(EngineKind kind)
{
    return kind == EngineKind::Unity
        ? QCoreApplication::translate("HarkenHub", "Unity Integration Package")
        : QCoreApplication::translate("HarkenHub", "UE Integration Package");
}

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

class IntegrationSourceDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        option->text = QCoreApplication::translate("HarkenHub", "%1 %2 | %3")
            .arg(index.data(kIntegrationPackageNameRole).toString(),
                 index.data(kIntegrationVersionRole).toString(), option->text);
        option->textElideMode = Qt::ElideRight;
    }
};

class IntegrationSourceComboBox final : public QComboBox
{
public:
    IntegrationSourceComboBox(QWidget* parent, std::function<void()> refreshSources)
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

QString integrationContentText(IntegrationContentState state)
{
    switch (state) {
        case IntegrationContentState::MatchesInstallation:
        case IntegrationContentState::OptionalFilesMissing:
            return QCoreApplication::translate("HarkenHub", "Normal");
        case IntegrationContentState::Modified:
            return QCoreApplication::translate("HarkenHub", "Files modified");
        case IntegrationContentState::Missing:
            return QCoreApplication::translate("HarkenHub", "Files missing");
        case IntegrationContentState::ModifiedAndMissing:
            return QCoreApplication::translate("HarkenHub", "Files modified and missing");
        case IntegrationContentState::Unverified:
            return QCoreApplication::translate("HarkenHub", "File verification incomplete");
        case IntegrationContentState::NotChecked:
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

QString integrationVersion(const QString& path, EngineKind kind)
{
    if (path.trimmed().isEmpty()) return {};
    const QString root = kind == EngineKind::Unity
        ? VNX::UnityIntegrationService::locateIntegrationSource(path)
        : ProjectIntegrationExecutor::locateUnrealSource(path);
    if (root.isEmpty()) return {};
    QFile file(QDir(root).filePath(kind == EngineKind::Unity
        ? QStringLiteral("package.json") : QStringLiteral("Harken.uplugin")));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    return object.value(kind == EngineKind::Unity
        ? QStringLiteral("version") : QStringLiteral("VersionName")).toString();
}

QString studioIntegrationPath(const InstalledVersion& version, EngineKind kind)
{
    const QDir directory(QFileInfo(version.executablePath).absolutePath());
    const QString package = kind == EngineKind::Unity
        ? QStringLiteral("/Unity/Harken") : QStringLiteral("/Unreal/Harken");
    const QString current = directory.filePath(QStringLiteral("HarkenIntegration") + package);
    const QString legacy = directory.filePath(QStringLiteral("HarkenSDK") + package);
    return QFileInfo::exists(current) || !QFileInfo::exists(legacy) ? current : legacy;
}

struct LocalIntegrationSource
{
    QString path;
    QString version;
    QString label;
    QString studioVersion;
};

QVector<LocalIntegrationSource> localIntegrationSources(
    EngineKind kind, const std::vector<InstalledVersion>& versions)
{
    QVector<LocalIntegrationSource> sources;
    for (const auto& studio : versions) {
        const QString path = studioIntegrationPath(studio, kind);
        const QString version = integrationVersion(path, kind);
        if (version.isEmpty()) continue;
        sources.push_back({path, version,
            QCoreApplication::translate("HarkenHub", "%1 %2 | Studio %3")
                .arg(integrationPackageName(kind), version, studio.semanticVersion), studio.semanticVersion});
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

int compareIntegrationVersions(const QString& left, const QString& right)
{
    qsizetype leftSuffix = 0;
    qsizetype rightSuffix = 0;
    const auto leftVersion = QVersionNumber::fromString(left, &leftSuffix);
    const auto rightVersion = QVersionNumber::fromString(right, &rightSuffix);
    if (leftVersion.isNull() || rightVersion.isNull()
        || leftSuffix != left.size() || rightSuffix != right.size()) return 0;
    return QVersionNumber::compare(leftVersion, rightVersion);
}

int compareSourceVersions(const QString& left, const QString& right)
{
    const int comparison = QVersionNumber::compare(
        QVersionNumber::fromString(left), QVersionNumber::fromString(right));
    if (comparison != 0) return comparison;
    const QString leftPrerelease = left.section(QLatin1Char('+'), 0, 0).section(QLatin1Char('-'), 1);
    const QString rightPrerelease = right.section(QLatin1Char('+'), 0, 0).section(QLatin1Char('-'), 1);
    if (leftPrerelease.isEmpty() || rightPrerelease.isEmpty())
        return int(leftPrerelease.isEmpty()) - int(rightPrerelease.isEmpty());
    const auto leftParts = leftPrerelease.split(QLatin1Char('.'));
    const auto rightParts = rightPrerelease.split(QLatin1Char('.'));
    static const QRegularExpression numericIdentifier(QStringLiteral("^[0-9]+$"));
    for (qsizetype index = 0; index < std::min(leftParts.size(), rightParts.size()); ++index) {
        const QString& leftPart = leftParts[index];
        const QString& rightPart = rightParts[index];
        if (leftPart == rightPart) continue;
        const bool leftNumeric = numericIdentifier.match(leftPart).hasMatch();
        const bool rightNumeric = numericIdentifier.match(rightPart).hasMatch();
        if (leftNumeric != rightNumeric) return leftNumeric ? -1 : 1;
        if (leftNumeric && leftPart.size() != rightPart.size())
            return leftPart.size() > rightPart.size() ? 1 : -1;
        return QString::compare(leftPart, rightPart, Qt::CaseSensitive);
    }
    return int(leftParts.size() > rightParts.size()) - int(leftParts.size() < rightParts.size());
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
        QStringLiteral("^(\\d+) missing Integration Package file\\(s\\) will be added; "
                       "(\\d+) existing Integration Package file\\(s\\) need updating; "
                       "(\\d+) obsolete Harken-managed file\\(s\\) will be removed; "
                       "(\\d+) project-specific file\\(s\\) will be kept unchanged; "
                       "(\\d+) Unity-generated metadata file\\(s\\) and "
                       "(\\d+) optional target runtime file\\(s\\) will be kept automatically$"));
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch())
        return hubTranslate(text);
    return hubTranslate(
               QStringLiteral("%1 missing Integration Package file(s) will be added; %2 existing Integration Package file(s) need updating; %3 obsolete Harken-managed file(s) will be removed; %4 project-specific file(s) will be kept unchanged; %5 Unity-generated metadata file(s) and %6 optional target runtime file(s) will be kept automatically"))
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

    static const QRegularExpression validatedIntegration(
        QStringLiteral("^Validated Integration Package (.+): (\\d+) file\\(s\\); the "
                       "(.+) runtime required for this editor is present\\.(.*)$"));
    const QRegularExpressionMatch validatedMatch = validatedIntegration.match(text);
    if (validatedMatch.hasMatch()) {
        QString result = hubTranslate(
                             QStringLiteral("Validated Integration Package %1: %2 file(s); the %3 runtime required for this editor is present."))
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
        QStringLiteral("^Check found Integration Package differences: (.+)\\.$"));
    const QRegularExpressionMatch countMatch = countCheck.match(text);
    if (countMatch.hasMatch())
        return hubTranslate(QStringLiteral("Check found Integration Package differences: %1.") )
            .arg(localizeCountSummary(countMatch.captured(1)));

    static const QRegularExpression existingDiff(
        QStringLiteral("^The existing Integration Package differs from selected Integration Package source (.+): (.+)\\.$"));
    const QRegularExpressionMatch existingMatch = existingDiff.match(text);
    if (existingMatch.hasMatch())
        return hubTranslate(QStringLiteral("The existing Integration Package differs from selected Integration Package source %1: %2."))
            .arg(existingMatch.captured(1), localizeCountSummary(existingMatch.captured(2)));

    static const QRegularExpression approvedUpdate(
        QStringLiteral("^Approved Integration Package update: (.+)\\.$"));
    const QRegularExpressionMatch approvedMatch = approvedUpdate.match(text);
    if (approvedMatch.hasMatch())
        return hubTranslate(QStringLiteral("Approved Integration Package update: %1."))
            .arg(localizeCountSummary(approvedMatch.captured(1)));

    static const QRegularExpression fileCount(
        QStringLiteral("^No embedded Harken Integration Package is installed; (\\d+) file\\(s\\) are ready to install\\.$"));
    const QRegularExpressionMatch fileCountMatch = fileCount.match(text);
    if (fileCountMatch.hasMatch())
        return hubTranslate(QStringLiteral("No embedded Harken Integration Package is installed; %1 file(s) are ready to install."))
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
        {QStringLiteral("The Unreal Integration Package source is incomplete"),
         QStringLiteral("The Unreal Integration Package source is incomplete")},
        {QStringLiteral("The Unreal Integration Package version is unavailable"),
         QStringLiteral("The Unreal Integration Package version is unavailable")},
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
        {QStringLiteral("sdk_preflight"), QStringLiteral("Validate Harken Unity Integration Package source")},
        {QStringLiteral("compare_sdk"), QStringLiteral("Compare installed Unity Integration Package")},
        {QStringLiteral("install_sdk"), QStringLiteral("Install or update Unity Integration Package")},
        {QStringLiteral("verify"), QStringLiteral("Verify installed Integration Package")},
        {QStringLiteral("report"), QStringLiteral("Write integration report")},
        {QStringLiteral("inspect_installation"), QStringLiteral("Review installed Harken Integration Package")},
        {QStringLiteral("remove_integration"), QStringLiteral("Back up and remove Harken Integration Package")},
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
    confirmation.setSizeGripEnabled(true);
    configureDialog(&confirmation, QSize(760, 520), QSize(480, 320));

    auto* layout = new QVBoxLayout(&confirmation);
    auto* messageLayout = new QHBoxLayout();
    auto* icon = new QLabel(&confirmation);
    icon->setPixmap(QApplication::style()
                        ->standardIcon(QStyle::SP_MessageBoxWarning)
                        .pixmap(40, 40));
    icon->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    messageLayout->addWidget(icon, 0, Qt::AlignTop);
    auto* messageLabel = new DialogTextView(&confirmation);
    messageLabel->setText(message);
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
        QCoreApplication::translate("HarkenHub", "INTEGRATION\nPACKAGE STATUS"),
        QCoreApplication::translate("HarkenHub", "INTEGRATION\nPACKAGE VERSION"),
        QString(),
    });
    auto* boundedHeader = new BoundedHeaderView(Qt::Horizontal, table_);
    boundedHeader->setFillSection(kIntegrationVersionColumn);
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
        kIntegrationStatusColumn, QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(
        kIntegrationVersionColumn, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(
        kActionColumn, QHeaderView::Fixed);
    table_->horizontalHeader()->setMinimumSectionSize(70);
    table_->setColumnWidth(kHarkenProjectColumn, 180);
    table_->setColumnWidth(kEngineProjectColumn, 200);
    table_->setColumnWidth(kEngineColumn, 80);
    table_->setColumnWidth(kIntegrationStatusColumn, 130);
    table_->setColumnWidth(kIntegrationVersionColumn, 190);
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
        kIntegrationStatusColumn, new NoHoverItemDelegate(table_));
    table_->setItemDelegateForColumn(
        kIntegrationVersionColumn, new NoHoverItemDelegate(table_));
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

    changeIntegrationButton_ = new QPushButton(QCoreApplication::translate("HarkenHub", "Change Integration Package..."), formPanel);
    changeIntegrationButton_->setObjectName(QStringLiteral("changeIntegration"));
    migrateIntegrationButton_ = new QPushButton(QCoreApplication::translate("HarkenHub", "Move Integration Package..."), formPanel);
    migrateIntegrationButton_->setObjectName(QStringLiteral("changeIntegrationLocation"));
    removeInstalledIntegrationButton_ = new QPushButton(QCoreApplication::translate("HarkenHub", "Uninstall Integration Package..."), formPanel);
    removeInstalledIntegrationButton_->setObjectName(QStringLiteral("uninstallIntegration"));

    auto makeRow = [formPanel](const QString& labelText, QLabel** rowLabel = nullptr) {
        auto* rowWidget = new QWidget(formPanel);
        auto* row = new QHBoxLayout(rowWidget);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        auto* label = new QLabel(labelText, rowWidget);
        if (rowLabel) *rowLabel = label;
        label->setFixedWidth(150);
        label->setWordWrap(true);
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
    const QString associationHint = QCoreApplication::translate("HarkenHub", "Linking adds this project pair to Engine Integrations and establishes a one-to-one relationship in this Hub. Unlink the pair before linking either project to another. Integration Package checks and management do not require linking.");
    auto* associationLabel = new QLabel(associationHint, formPanel);
    associationLabel->setObjectName(QStringLiteral("projectAssociationHint"));
    associationLabel->setWordWrap(true);
    associationLabel->setStyleSheet(QStringLiteral("color: #AAB2C0; font-size: 12px;"));
    form->addWidget(associationLabel);
    linkProjectsButton_->setToolTip(QCoreApplication::translate("HarkenHub", "Add this project pair to Engine Integrations. No project or Integration Package files are changed."));
    unlinkProjectsButton_->setToolTip(QCoreApplication::translate("HarkenHub", "Remove this project pair from Engine Integrations. Project files and the installed Integration Package are kept."));

    form->addSpacing(8);
    const QString separatorStyle = QStringLiteral("background-color: #313848; border: none;");
    auto* separator = new QFrame(formPanel);
    separator->setObjectName(QStringLiteral("integrationAssociationSeparator"));
    separator->setFrameShape(QFrame::NoFrame);
    separator->setFixedHeight(1);
    separator->setStyleSheet(separatorStyle);
    form->addWidget(separator);
    auto* integrationHeader = new QHBoxLayout();
    integrationHeading_ = new QLabel(QCoreApplication::translate("HarkenHub", "Integration"), formPanel);
    integrationHeading_->setObjectName(QStringLiteral("integrationHeading"));
    integrationHeading_->setStyleSheet(sectionStyle);
    integrationHeader->addWidget(integrationHeading_);
    integrationHeader->addStretch();
    form->addLayout(integrationHeader);
    auto [installedPathRowWidget, installedPathRow] = makeRow(
        QCoreApplication::translate("HarkenHub", "Current Integration Package Location"));
    installedIntegrationPath_ = new QLineEdit(installedPathRowWidget);
    installedIntegrationPath_->setObjectName(QStringLiteral("installedIntegrationPath"));
    installedIntegrationPath_->setReadOnly(true);
    installedPathRow->addWidget(installedIntegrationPath_, 1);
    installedIntegrationLabel_ = new QLabel(installedPathRowWidget);
    installedIntegrationLabel_->setObjectName(QStringLiteral("installedIntegrationVersion"));
    installedPathRow->addWidget(installedIntegrationLabel_);
    form->addWidget(installedPathRowWidget);

    auto* verificationActions = new QHBoxLayout();
    checkButton_ = new QPushButton(QCoreApplication::translate("HarkenHub", "Check Current Integration Package Status"), formPanel);
    checkButton_->setObjectName(QStringLiteral("verifyIntegration"));
    verificationActions->addWidget(checkButton_);
    restoreIntegrationButton_ = new QPushButton(QCoreApplication::translate("HarkenHub", "Restore Current Version..."), formPanel);
    restoreIntegrationButton_->setObjectName(QStringLiteral("restoreIntegration"));
    verificationActions->addWidget(restoreIntegrationButton_);
    connect(restoreIntegrationButton_, &QPushButton::clicked, this, &EngineIntegrationsPage::runUnityRestoration);
    verificationActions->addStretch();
    form->addLayout(verificationActions);
    auto* integrationActions = new QHBoxLayout();
    integrationActions->addStretch();
    integrationActions->addWidget(changeIntegrationButton_);
    integrationActions->addWidget(migrateIntegrationButton_);
    integrationActions->addWidget(removeInstalledIntegrationButton_);
    form->addLayout(integrationActions);
    integrationIssueRow_ = new QWidget(formPanel);
    auto* issueLayout = new QHBoxLayout(integrationIssueRow_);
    issueLayout->setContentsMargins(0, 0, 0, 0);
    integrationIssueLabel_ = new QLabel(integrationIssueRow_);
    integrationIssueLabel_->setObjectName(QStringLiteral("integrationIssueHint"));
    integrationIssueLabel_->setWordWrap(true);
    integrationIssueLabel_->setStyleSheet(QStringLiteral("color: #F0C36A;"));
    issueLayout->addWidget(integrationIssueLabel_, 1);
    integrationResolutionButton_ = new QPushButton(QCoreApplication::translate("HarkenHub", "View Resolution..."), integrationIssueRow_);
    integrationResolutionButton_->setObjectName(QStringLiteral("integrationResolution"));
    issueLayout->addWidget(integrationResolutionButton_, 0, Qt::AlignTop);
    connect(integrationResolutionButton_, &QPushButton::clicked, this, &EngineIntegrationsPage::showIntegrationResolution);
    form->addWidget(integrationIssueRow_);
    integrationIssueRow_->hide();

    integrationSelectionDialog_ = new QDialog(this);
    integrationSelectionDialog_->setObjectName(QStringLiteral("integrationSelectionDialog"));
    integrationSelectionDialog_->setSizeGripEnabled(true);
    configureDialog(integrationSelectionDialog_, QSize(820, 600), QSize(480, 320));
    integrationSelectionDialog_->installEventFilter(this);
    auto* dialogRoot = new QVBoxLayout(integrationSelectionDialog_);
    auto* dialogScroll = new QScrollArea(integrationSelectionDialog_);
    dialogScroll->setWidgetResizable(true);
    dialogScroll->setFrameShape(QFrame::NoFrame);
    auto* dialogContent = new QWidget(dialogScroll);
    auto* dialogLayout = new QVBoxLayout(dialogContent);
    dialogScroll->setWidget(dialogContent);
    dialogRoot->addWidget(dialogScroll, 1);
    dialogLayout->setContentsMargins(20, 20, 20, 20);
    dialogLayout->setSpacing(14);
    integrationSelectionRow_ = new QWidget(integrationSelectionDialog_);
    integrationSelectionRow_->setObjectName(QStringLiteral("integrationSelectionForm"));
    auto* selectionLayout = new QVBoxLayout(integrationSelectionRow_);
    selectionLayout->setContentsMargins(0, 0, 0, 0);
    selectionLayout->setSpacing(8);
    auto [integrationRowWidget, integrationPathRow] = makeRow(
        QCoreApplication::translate("HarkenHub", "Integration Package Source Directory"), &integrationSourceLabel_);
    integrationSourceCombo_ = new IntegrationSourceComboBox(integrationRowWidget, [this]() {
        if (!operationRunning_) refreshLocalIntegrationSources();
    });
    integrationSourceCombo_->setObjectName(QStringLiteral("integrationSourceSelector"));
    integrationSourceCombo_->installEventFilter(this);
    integrationSourceCombo_->setEditable(true);
    integrationSourceCombo_->setInsertPolicy(QComboBox::NoInsert);
    integrationSourceCombo_->setCompleter(nullptr);
    integrationSourceCombo_->setItemDelegate(new IntegrationSourceDelegate(integrationSourceCombo_));
    integrationSourceCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    integrationPathEdit_ = integrationSourceCombo_->lineEdit();
    integrationPathEdit_->setObjectName(QStringLiteral("integrationSourcePath"));
    integrationPathEdit_->setPlaceholderText(
        QCoreApplication::translate("HarkenHub", "Runtime/Integration Package source path"));
    integrationPathEdit_->setReadOnly(true);
    integrationPathEdit_->setCursor(Qt::ArrowCursor);
    integrationPathEdit_->installEventFilter(this);
    integrationSourceBrowseButton_ = new QPushButton(
        QCoreApplication::translate("HarkenHub", "Choose Integration Package Folder..."), integrationRowWidget);
    integrationSourceBrowseButton_->setObjectName(QStringLiteral("integrationSourceBrowse"));
    integrationPathRow->addWidget(integrationSourceCombo_, 1);
    integrationPathRow->addWidget(integrationSourceBrowseButton_);
    selectionLayout->addWidget(integrationRowWidget);

    auto [installRowWidget, installRow] = makeRow(
        QCoreApplication::translate("HarkenHub", "Integration Package Install Location"), &integrationInstallLabel_);
    integrationInstallRow_ = installRowWidget;
    integrationInstallPathEdit_ = new QLineEdit(installRowWidget);
    integrationInstallPathEdit_->setObjectName(QStringLiteral("integrationInstallPath"));
    integrationInstallPathEdit_->setPlaceholderText(QStringLiteral("Packages/com.harken.core"));
    integrationInstallBrowseButton_ = new QPushButton(
        QCoreApplication::translate("HarkenHub", "Browse..."), installRowWidget);
    integrationInstallBrowseButton_->setObjectName(QStringLiteral("integrationInstallBrowse"));
    installRow->addWidget(integrationInstallPathEdit_, 1);
    installRow->addWidget(integrationInstallBrowseButton_);
    selectionLayout->addWidget(installRowWidget);

    auto* integrationActionRow = new QHBoxLayout();
    integrationActionRow->setContentsMargins(0, 0, 0, 0);
    integrationCheckButton_ = new QPushButton(QCoreApplication::translate("HarkenHub", "Check Integration Package Changes"), integrationSelectionDialog_);
    integrationCheckButton_->setObjectName(QStringLiteral("integrationChangeCheck"));
    integrationCheckButton_->setAutoDefault(false);
    integrationActionRow->addWidget(integrationCheckButton_);
    integrationActionRow->addStretch(1);
    integrateButton_ = new QPushButton(
        QCoreApplication::translate("HarkenHub", "Install Integration Package..."), integrationSelectionDialog_);
    integrateButton_->setObjectName(QStringLiteral("integrationPrimaryAction"));
    integrateButton_->setAutoDefault(false);
    cancelIntegrationButton_ = new QPushButton(QCoreApplication::translate("HarkenHub", "Cancel"), integrationSelectionDialog_);
    cancelIntegrationButton_->setAutoDefault(false);
    cancelIntegrationButton_->setObjectName(QStringLiteral("cancelIntegrationSelection"));
    const auto editorButtons = {engineBrowseButton_,
                                harkenBrowseButton_, integrationSourceBrowseButton_,
                                integrationInstallBrowseButton_, integrateButton_,
                                linkProjectsButton_, unlinkProjectsButton_};
    int buttonWidth = kPathButtonMinimumWidth;
    for (auto* button : editorButtons) {
        button->ensurePolished();
        buttonWidth = std::max(buttonWidth, button->sizeHint().width());
    }
    for (auto* button : editorButtons)
        button->setMinimumWidth(buttonWidth);
    auto* confirmActionRow = new QHBoxLayout();
    confirmActionRow->addStretch();
    confirmActionRow->addWidget(cancelIntegrationButton_);
    confirmActionRow->addWidget(integrateButton_);
    integrationSelectionContext_ = new DialogTextView(integrationSelectionDialog_);
    integrationSelectionContext_->setObjectName(QStringLiteral("integrationSelectionContext"));
    integrationSelectionContext_->hide();
    dialogLayout->addWidget(integrationSelectionContext_);
    dialogLayout->addWidget(integrationSelectionRow_);
    integrationProgress_ = new IntegrationTaskProgress(integrationSelectionDialog_);
    integrationProgress_->setObjectName(QStringLiteral("integrationInlineProgress"));
    integrationProgress_->hide();
    dialogLayout->addWidget(integrationProgress_);
    integrationSelectionSummary_ = new DialogTextView(integrationSelectionDialog_);
    integrationSelectionSummary_->setObjectName(QStringLiteral("integrationSelectionSummary"));
    dialogLayout->addWidget(integrationSelectionSummary_);
    integrationResultsTabs_ = new QTabWidget(integrationSelectionDialog_);
    integrationResultsTabs_->setObjectName(QStringLiteral("integrationCheckResults"));
    auto* changesPage = new QWidget(integrationResultsTabs_);
    auto* changesLayout = new QVBoxLayout(changesPage);
    changesLayout->setContentsMargins(12, 12, 12, 8);
    changesLayout->setSpacing(10);
    integrationDiffSummary_ = new DialogTextView(changesPage, Qt::RichText);
    integrationDiffSummary_->setObjectName(QStringLiteral("integrationDiffSummary"));
    connect(integrationDiffSummary_, &QTextBrowser::anchorClicked, this, [this]() {
        integrationResultsTabs_->setCurrentIndex(1);
    });
    changesLayout->addWidget(integrationDiffSummary_);
    integrationDiffTree_ = new QTreeWidget(changesPage);
    integrationDiffTree_->setObjectName(QStringLiteral("integrationFileChanges"));
    integrationDiffTree_->setHeaderHidden(true);
    integrationDiffTree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    integrationDiffTree_->setUniformRowHeights(true);
    integrationDiffTree_->setStyleSheet(QStringLiteral("QTreeWidget::item { padding: 4px; }"));
    changesLayout->addWidget(integrationDiffTree_, 1);
    integrationResultsTabs_->addTab(changesPage, QCoreApplication::translate("HarkenHub", "File Changes"));
    integrationSelectionDetails_ = new QTextBrowser(integrationResultsTabs_);
    integrationSelectionDetails_->setObjectName(QStringLiteral("integrationSelectionDetails"));
    integrationSelectionDetails_->setOpenLinks(false);
    integrationSelectionDetails_->setOpenExternalLinks(false);
    integrationResultsTabs_->addTab(integrationSelectionDetails_, QCoreApplication::translate("HarkenHub", "Check Log"));
    integrationSelectionPaths_ = new QPlainTextEdit(integrationResultsTabs_);
    integrationSelectionPaths_->setObjectName(QStringLiteral("integrationSelectionPaths"));
    integrationSelectionPaths_->setReadOnly(true);
    integrationResultsTabs_->addTab(integrationSelectionPaths_, QCoreApplication::translate("HarkenHub", "Project and Integration Package Paths"));
    integrationResultsTabs_->setMinimumHeight(160);
    dialogLayout->addWidget(integrationResultsTabs_, 1);
    integrationSafetyHint_ = new DialogTextView(integrationSelectionDialog_);
    integrationSafetyHint_->setObjectName(QStringLiteral("integrationEngineRunningHint"));
    integrationSafetyHint_->setStyleSheet(QStringLiteral("color: #FF6B6B;"));
    integrationSafetyHint_->hide();
    engineStatusTimer_ = new QTimer(this);
    engineStatusTimer_->setInterval(1000);
    connect(engineStatusTimer_, &QTimer::timeout, this, &EngineIntegrationsPage::refreshEngineRunningHint);
    dialogLayout->addWidget(integrationSafetyHint_);
    integrationReportButton_ = new QPushButton(QCoreApplication::translate("HarkenHub", "Open Report"), integrationSelectionDialog_);
    integrationReportButton_->setObjectName(QStringLiteral("integrationOpenReport"));
    integrationReportButton_->setAutoDefault(false);
    integrationReportButton_->hide();
    integrationActionRow->insertWidget(1, integrationReportButton_);
    connect(integrationReportButton_, &QPushButton::clicked, this, [this]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(integrationReportPath_));
    });
    dialogRoot->addLayout(integrationActionRow);
    dialogRoot->addLayout(confirmActionRow);
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
    connect(integrationInstallBrowseButton_, &QPushButton::clicked, this,
            &EngineIntegrationsPage::browseIntegrationInstallPath);
    connect(integrationInstallPathEdit_, &QLineEdit::textChanged, this, [this]() {
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
    connect(integrationCheckButton_, &QPushButton::clicked, this,
            &EngineIntegrationsPage::runCheck);
    connect(integrateButton_, &QPushButton::clicked, this,
            &EngineIntegrationsPage::runIntegration);
    connect(changeIntegrationButton_, &QPushButton::clicked, this,
            &EngineIntegrationsPage::beginIntegrationSelection);
    connect(cancelIntegrationButton_, &QPushButton::clicked, integrationSelectionDialog_, &QDialog::reject);
    connect(integrationSelectionDialog_, &QDialog::rejected, this,
            &EngineIntegrationsPage::cancelIntegrationSelection);
    connect(removeInstalledIntegrationButton_, &QPushButton::clicked, this,
            &EngineIntegrationsPage::runIntegrationRemoval);
    connect(unlinkProjectsAction_, &QAction::triggered, this, [this]() {
        unlinkProjects(editingIntegrationId_);
    });
    connect(linkProjectsButton_, &QPushButton::clicked, this,
            &EngineIntegrationsPage::linkProjects);
    connect(migrateIntegrationButton_, &QPushButton::clicked, this,
            &EngineIntegrationsPage::browseIntegrationInstallPath);
    connect(integrationSourceBrowseButton_, &QPushButton::clicked, this,
            &EngineIntegrationsPage::browseIntegrationSource);
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
    connect(integrationPathEdit_, &QLineEdit::textChanged, this, [this]() {
        integrationPathUserSelected_ = true;
        clearResults();
        updateIntegrationSourceState();
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
            inspection.observation.status = QCoreApplication::translate("HarkenHub", "Checking Integration Package status...");
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
                emit self->integrationStatusChanged(target.targetId);
            }, Qt::QueuedConnection);
        });
    }
    return inspection.observation;
}

bool EngineIntegrationsPage::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == integrationSelectionDialog_) {
        if (event->type() == QEvent::Show) {
            engineRunning_ = false;
            integrationSafetyHint_->hide();
            engineStatusTimer_->start();
            QTimer::singleShot(0, this, &EngineIntegrationsPage::refreshEngineRunningHint);
        } else if (event->type() == QEvent::Hide) {
            engineStatusTimer_->stop();
        }
    }
    if ((watched == integrationSourceCombo_ || watched == integrationPathEdit_)
        && integrationSourceCombo_->isEnabled()
        && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease)
        && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        if (event->type() == QEvent::MouseButtonPress)
            integrationSourceCombo_->setFocus(Qt::MouseFocusReason);
        else
            integrationSourceCombo_->showPopup();
        return true;
    }
    if (watched == integrationSelectionDialog_ && operationRunning_
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
    const auto unitySources = localIntegrationSources(EngineKind::Unity, versions);
    const auto unrealSources = localIntegrationSources(EngineKind::Unreal, versions);
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
                row, kIntegrationStatusColumn,
                new QTableWidgetItem(healthText(EngineTargetHealth::Missing)));
            table_->setItem(row, kIntegrationVersionColumn, new QTableWidgetItem(
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
            const QString contentText = integrationContentText(observation.integrationContentState);
            QString statusText = observation.status;
            if (statusText.isEmpty()
                && (observation.health != EngineTargetHealth::Managed || contentText.isEmpty()))
                statusText = healthText(observation.health);
            if (!contentText.isEmpty()) {
                if (!statusText.isEmpty()) statusText += QLatin1Char('\n');
                statusText += contentText;
            }
            auto* statusItem = new QTableWidgetItem(statusText);
            if (!observation.integrationInstallPath.isEmpty())
                statusItem->setToolTip(QCoreApplication::translate(
                    "HarkenHub", "Current Integration Package installation path:\n%1").arg(observation.integrationInstallPath));
            if (observation.integrationContentState != IntegrationContentState::NotChecked
                && observation.integrationContentState != IntegrationContentState::OptionalFilesMissing
                && observation.integrationContentState != IntegrationContentState::MatchesInstallation)
                statusItem->setForeground(QColor(QStringLiteral("#E3B341")));
            table_->setItem(row, kIntegrationStatusColumn, statusItem);
            const auto& sources = target->engineKind == EngineKind::Unity ? unitySources : unrealSources;
            QString versionText = observation.integrationVersion;
            if (!versionText.isEmpty())
                versionText = QCoreApplication::translate("HarkenHub", "Installed: %1").arg(versionText);
            if (versionText.isEmpty())
                versionText = observation.health == EngineTargetHealth::NotInstalled
                    ? QCoreApplication::translate("HarkenHub", "Not installed")
                    : QCoreApplication::translate("HarkenHub", "Unknown");
            QString newestVersion = observation.integrationVersion;
            for (const auto& source : sources) {
                if (compareIntegrationVersions(source.version, newestVersion) > 0)
                    newestVersion = source.version;
            }
            const bool newer = !observation.integrationVersion.isEmpty()
                && newestVersion != observation.integrationVersion;
            if (newer)
                versionText += QLatin1Char('\n') + QCoreApplication::translate(
                    "HarkenHub", "Update available: %1").arg(newestVersion);
            auto* versionItem = new QTableWidgetItem(versionText);
            if (newer) versionItem->setForeground(QColor(QStringLiteral("#00D9EF")));
            if (newer)
                versionItem->setToolTip(QCoreApplication::translate(
                    "HarkenHub", "Latest local Integration Package version: %1").arg(newestVersion));
            table_->setItem(row, kIntegrationVersionColumn, versionItem);
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
        const QString packageName = target == targets.end()
            ? QCoreApplication::translate("HarkenHub", "Integration Package")
            : integrationPackageName(target->engineKind);
        connect(menu, &QToolButton::clicked, this, [this, integrationId, harkenRoot, engineRoot, packageName, menu]() {
            if (operationRunning_) return;
            listMenuOpen_ = true;
            QMenu popup;
            popup.setToolTipsVisible(true);
            QAction* edit = popup.addAction(QCoreApplication::translate("HarkenHub", "Manage %1 and Project Links").arg(packageName));
            edit->setToolTip(QCoreApplication::translate("HarkenHub", "Manage project links, verify Integration Package files, change versions, move or uninstall the Integration Package, and view detailed reports."));
            auto* change = popup.addMenu(QCoreApplication::translate("HarkenHub", "Quick Change %1").arg(packageName));
            change->setObjectName(QStringLiteral("quickIntegrationSources"));
            change->setToolTipsVisible(true);
            const auto records = integrations_->list();
            const auto record = std::find_if(records.begin(), records.end(), [&](const auto& value) {
                return value.integrationId == integrationId;
            });
            const QString targetId = record == records.end() ? QString() : record->engineTargetId;
            const auto targets = targets_->list();
            for (const auto& target : targets)
                if (target.targetId == targetId) requestTargetObservation(target, true);
            populateQuickIntegrationMenu(change, integrationId);
            connect(this, &EngineIntegrationsPage::integrationStatusChanged, change,
                [this, change, integrationId, targetId](const QString& updatedId) {
                    if (updatedId == targetId) populateQuickIntegrationMenu(change, integrationId);
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
                changeIntegrationFromList(integrationId, sourcePath);
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


void EngineIntegrationsPage::populateQuickIntegrationMenu(QMenu* change, const QString& integrationId)
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
        auto sources = localIntegrationSources(target->engineKind, availableStudioVersions());
        const auto sourceGroup = [&](const LocalIntegrationSource& source) {
            const int comparison = compareIntegrationVersions(source.version, observation.integrationVersion);
            return comparison > 0 ? 0 : comparison < 0 ? 2 : 1;
        };
        std::stable_sort(sources.begin(), sources.end(), [&](const auto& left, const auto& right) {
            const int leftGroup = sourceGroup(left);
            const int rightGroup = sourceGroup(right);
            if (leftGroup != rightGroup) return leftGroup < rightGroup;
            const int comparison = QVersionNumber::compare(
                QVersionNumber::fromString(left.version), QVersionNumber::fromString(right.version));
            if (comparison != 0) return comparison > 0;
            return QVersionNumber::compare(QVersionNumber::fromString(left.studioVersion),
                                           QVersionNumber::fromString(right.studioVersion)) > 0;
        });
        if (!targetInspections_[target->targetId].ready) {
            change->addAction(observation.status)->setEnabled(false);
            change->addSeparator();
        } else if (!observation.integrationVersion.isEmpty()) {
            QString installed = QCoreApplication::translate("HarkenHub", "Installed %1: %2")
                .arg(integrationPackageName(target->engineKind), observation.integrationVersion);
            const QString contentText = integrationContentText(observation.integrationContentState);
            if (!contentText.isEmpty()) installed += QStringLiteral(" — ") + contentText;
            change->addAction(installed)->setEnabled(false);
            change->addSeparator();
        }
        int previousGroup = -1;
        for (const auto& source : sources) {
            const int group = sourceGroup(source);
            if (previousGroup != -1 && previousGroup != group)
                change->addSeparator();
            previousGroup = group;
            QString label = source.label;
            const bool sameVersion = source.version == observation.integrationVersion;
            const bool installedSource = sameVersion && !record->integrationSourcePath.isEmpty()
                && samePath(source.path, record->integrationSourcePath);
            const int comparison = compareIntegrationVersions(source.version, observation.integrationVersion);
            if (installedSource) {
                label += QCoreApplication::translate("HarkenHub", " (Installed source)");
                if (observation.integrationContentState != IntegrationContentState::NotChecked
                    && observation.integrationContentState != IntegrationContentState::OptionalFilesMissing
                    && observation.integrationContentState != IntegrationContentState::MatchesInstallation)
                    label += QStringLiteral(" — ") + integrationContentText(observation.integrationContentState);
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
            action->setEnabled(observation.canChangeIntegration
                && observation.health != EngineTargetHealth::NotInstalled);
            QString tooltip = action->isEnabled()
                ? QDir::toNativeSeparators(source.path) + QLatin1Char('\n')
                    + QCoreApplication::translate("HarkenHub", "Integration Package files will be checked before you confirm the change.")
                : observation.remediation;
            if (installedSource)
                tooltip += QLatin1Char('\n') + QCoreApplication::translate("HarkenHub", "This is the source recorded for the installed Integration Package. File verification compares the installed files with their installation record; the source folder may have changed since installation.");
            if (sameVersion)
                tooltip += QLatin1Char('\n') + QCoreApplication::translate("HarkenHub", "Integration Packages with the same version number can contain different files. Select a source to check its contents.");
            action->setToolTip(tooltip);
        }
        if (sources.isEmpty())
            change->addAction(QCoreApplication::translate(
                "HarkenHub", "No local Integration Package sources are available."))->setEnabled(false);
    }
    if (change->isEmpty())
        change->addAction(QCoreApplication::translate(
            "HarkenHub", "No local Integration Package sources are available."))->setEnabled(false);
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
    integrationSelectionDialog_->hide();
    integrationSelection_.reset();
    ++versionRequest_;
    studioVersion_.reset();
    versionResolutionPending_ = false;
    checkButton_->setEnabled(true);
    integrateButton_->setEnabled(true);
    editingIntegrationId_.clear();
    selectedHarkenProjectPath_.clear();
    selectedEngineTargetId_.clear();
    installPathTargetId_.clear();
    integrationPathUserSelected_ = false;
    {
        const QSignalBlocker harkenBlocker(harkenProjectEdit_);
        const QSignalBlocker engineBlocker(engineProjectEdit_);
        harkenProjectEdit_->clear();
        engineProjectEdit_->clear();
    }
    harkenProjectEdit_->setToolTip({});
    setIntegrationPath({}, false);
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
    integrationSelectionDialog_->hide();
    quickIntegrationChange_ = false;
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
        MessageDialog::warning(this, QCoreApplication::translate("HarkenHub", "Manage Integration"),
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
        MessageDialog::warning(this, QCoreApplication::translate("HarkenHub", "Manage Integration"),
                             QCoreApplication::translate("HarkenHub", "The engine project is no longer available."));
        showConflict(integrationId, QCoreApplication::translate("HarkenHub", "The stored engine project is missing."));
        return false;
    }

    integrationSelection_.reset();
    editingIntegrationId_ = integrationId;
    selectedHarkenProjectPath_ = integration->harkenProjectPath;
    selectedEngineTargetId_ = integration->engineTargetId;
    integrationPathUserSelected_ = false;
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
    if (!integration->integrationSourcePath.trimmed().isEmpty())
        setIntegrationPath(integration->integrationSourcePath, true);
    else
        setIntegrationPath({}, false);
    if (refreshObservation) {
        targetInspections_[target->targetId].ready = false;
        requestTargetObservation(*target, true);
    }
    updateRemovalButtonState(true);
    return true;
}

void EngineIntegrationsPage::changeIntegrationFromList(
    const QString& integrationId, const QString& sourcePath)
{
    if (operationRunning_ || !loadExistingIntegration(integrationId, false)) return;
    if (targetBlocked_ || !integrationInstalled_) {
        MessageDialog::warning(this, QCoreApplication::translate("HarkenHub", "Change %1").arg(selectedIntegrationPackageName()),
            changeIntegrationButton_->toolTip().isEmpty()
                ? QCoreApplication::translate("HarkenHub", "The installed Integration Package is no longer available. Open Manage Integration to review its status.")
                : changeIntegrationButton_->toolTip());
        refresh();
        return;
    }
    quickIntegrationChange_ = true;
    beginIntegrationSelection();
    setIntegrationPath(sourcePath, true);
    runCheck();
    refresh();
}

void EngineIntegrationsPage::showList()
{
    if (operationRunning_) return;
    cancelIntegrationSelection();
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
    if (integrationSelectionDialog_->isVisible()) {
        versionRefreshPending_ = true;
        if (!quickIntegrationChange_ && !integrationSourceCombo_->view()->isVisible())
            refreshLocalIntegrationSources();
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
    if (operationRunning_ || integrationSelection_ || integrationId.isEmpty()) return;
    const QString id = integrationId;
    setOperationRunning(true);
    const auto finish = qScopeGuard([this]() { setOperationRunning(false); });
    const auto answer = MessageDialog::question(
        this, QCoreApplication::translate("HarkenHub", "Unlink Projects"),
        QCoreApplication::translate("HarkenHub", "Unlink the game and Harken projects? This entry will disappear from Engine Integrations. Project files and the installed Integration Package will be kept."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;
    if (!integrations_->remove(id)) {
        MessageDialog::warning(this, QCoreApplication::translate("HarkenHub", "Unlink Projects"),
            QCoreApplication::translate("HarkenHub", "Could not unlink the projects.\n%1")
                .arg(localizeIntegrationText(integrations_->lastError())));
        return;
    }
    if (id == editingIntegrationId_) {
        editingIntegrationId_.clear();
        setEditorLocked(false);
        setSummaryText(QCoreApplication::translate("HarkenHub", "Projects unlinked. Project files and the installed Integration Package were kept."));
    }
    refresh();
}

void EngineIntegrationsPage::linkProjects()
{
    if (operationRunning_ || integrationSelection_ || !editingIntegrationId_.isEmpty()) return;
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
                + QStringLiteral("\n\n") + QCoreApplication::translate("HarkenHub", "Add this project pair to Engine Integrations. No project or Integration Package files are changed."),
            {}, QCoreApplication::translate("HarkenHub", "Link Projects"),
            QDialogButtonBox::AcceptRole)) return;
    if (persistIntegration(project.canonicalPath, target.targetId, integrationPathEdit_->text().trimmed())) {
        clearResults();
        setSummaryText(QCoreApplication::translate("HarkenHub", "Projects linked. Integration Package files were not changed."));
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

void EngineIntegrationsPage::browseIntegrationSource()
{
    if (!integrationSelection_ || operationRunning_) return;
    const QString start = integrationPathEdit_->text().trimmed().isEmpty()
        ? QCoreApplication::applicationDirPath()
        : integrationPathEdit_->text().trimmed();
    auto* dialog = new QFileDialog(
        integrationSelectionDialog_, QCoreApplication::translate("HarkenHub", "Choose %1 Folder").arg(selectedIntegrationPackageName()), start);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setOptions(QFileDialog::ShowDirsOnly
                       | QFileDialog::DontUseCustomDirectoryIcons);
    dialog->setFileMode(QFileDialog::Directory);
    connect(dialog, &QFileDialog::fileSelected, this, [this](const QString& path) {
        setIntegrationPath(path, true);
    });
    dialog->open();
}

void EngineIntegrationsPage::beginIntegrationSelection()
{
    if (operationRunning_ || integrationSelection_ || selectedEngineTargetId_.isEmpty()
        || selectedHarkenProjectPath_.isEmpty()
        || targetBlocked_) return;
    integrationSelection_ = IntegrationSelection{integrationPathEdit_->text(), integrationInstallPathEdit_->text(),
                                 integrationPathUserSelected_};
    integrationPhase_ = IntegrationPhase::Choosing;
    integrationPreviousVersion_ = integrationInstalled_ ? installedIntegrationVersion_
        : QCoreApplication::translate("HarkenHub", "Not installed");
    refreshLocalIntegrationSources();
    clearResults();
    updateRemovalButtonState();
    setSummaryText(QCoreApplication::translate("HarkenHub", "No integration check has run."));
    integrationSelectionDialog_->setWindowTitle(integrationInstalled_
        ? QCoreApplication::translate("HarkenHub", "Change %1").arg(selectedIntegrationPackageName())
        : QCoreApplication::translate("HarkenHub", "Install %1").arg(selectedIntegrationPackageName()));
    integrationSelectionDialog_->open();
    if (integrationSourceCombo_->count() > 0)
        integrationSourceCombo_->setFocus();
    else
        integrationSourceBrowseButton_->setFocus();
}

void EngineIntegrationsPage::cancelIntegrationSelection()
{
    if (operationRunning_) return;
    if (quickIntegrationChange_ || integrationPhase_ == IntegrationPhase::Finished) {
        quickIntegrationChange_ = false;
        integrationPhase_ = IntegrationPhase::Choosing;
        integrationSelection_.reset();
        integrationSelectionDialog_->hide();
        updatePrimaryAction();
        refresh();
        return;
    }
    if (!integrationSelection_) return;
    const IntegrationSelection original = *integrationSelection_;
    integrationSelection_.reset();
    integrationSelectionDialog_->hide();
    setIntegrationPath(original.sourcePath, original.userSelected);
    updateDefaultIntegrationPath();
    {
        const QSignalBlocker blocker(integrationInstallPathEdit_);
        integrationInstallPathEdit_->setText(original.installPath);
    }
    setEditorLocked(!editingIntegrationId_.isEmpty());
    updateRemovalButtonState();
    setSummaryText(QCoreApplication::translate("HarkenHub", "Cancelled. Integration Package files and project relationships were not changed."));
    if (versionRefreshPending_) refreshStudioVersion();
}

void EngineIntegrationsPage::browseIntegrationInstallPath()
{
    if (operationRunning_) return;
    const auto targets = targets_->list();
    const auto target = std::find_if(targets.begin(), targets.end(), [&](const auto& value) {
        return value.targetId == selectedEngineTargetId_;
    });
    if (target == targets.end()) return;
    const bool recover = hasPendingMigration(*target);
    if (recover || integrationInstallPathEdit_->isReadOnly()) {
        runIntegrationMigration(recover);
        return;
    }
    if (!integrationSelection_) return;
    const QString path = QFileDialog::getExistingDirectory(
        integrationSelectionDialog_, QCoreApplication::translate("HarkenHub", "Select %1 Installation Directory").arg(integrationPackageName(target->engineKind)),
        QDir(target->canonicalRoot).filePath(integrationInstallPathEdit_->text()));
    if (path.isEmpty()) return;
    QString resolved;
    QString error;
    const bool valid = target->engineKind == EngineKind::Unity
        ? VNX::UnityInstallation::resolveTarget(target->canonicalRoot, path, &resolved, &error)
        : ProjectIntegrationExecutor::resolveUnrealLocation(*target, path, &resolved, &error);
    if (!valid) {
        MessageDialog::warning(this, QCoreApplication::translate("HarkenHub", "Integration Package Install Location"), localizeIntegrationText(error));
        return;
    }
    integrationInstallPathEdit_->setText(QDir(target->canonicalRoot).relativeFilePath(resolved));
    integrationInstallPathEdit_->setToolTip(QDir::toNativeSeparators(resolved));
}

bool EngineIntegrationsPage::acceptHarkenProject(
    const QString& path, bool redirectOnConflict)
{
    selectedHarkenProjectPath_.clear();
    updateStudioVersionFields();
    const ProjectProbeResult probe = ProjectProbe::probe(path);
    if (!probe.ok) {
        MessageDialog::warning(
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
            MessageDialog::warning(
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
        MessageDialog::warning(
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
            MessageDialog::warning(
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
    updateDefaultIntegrationPath();
    updateIntegrationSourceState();
    updateRemovalButtonState(true);
    return true;
}

bool EngineIntegrationsPage::resolveSelection(
    ProjectRecord* project, EngineTargetRecord* target,
    bool registerTarget,
    bool redirectOnConflict, bool requireIntegrationSource)
{
    if (versionResolutionPending_)
        return false;
    integrations_->list();
    targets_->list();
    const QString storageError = integrations_->lastError().isEmpty()
        ? targets_->lastError() : integrations_->lastError();
    if (!storageError.isEmpty()) {
        MessageDialog::warning(this, QCoreApplication::translate("HarkenHub", "Manage Integration"),
            QCoreApplication::translate("HarkenHub", "Integration records could not be read.\n%1").arg(localizeIntegrationText(storageError)));
        return false;
    }
    const QString harkenInput = harkenProjectEdit_->text().trimmed();
    if (harkenInput.isEmpty()) {
        MessageDialog::warning(
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
        MessageDialog::warning(
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
                MessageDialog::warning(
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
    if (requireIntegrationSource && integrationPathEdit_->text().trimmed().isEmpty()) {
        MessageDialog::warning(
            integrationSelectionDialog_, QCoreApplication::translate("HarkenHub", "Manage Integration"),
            QCoreApplication::translate("HarkenHub", "Select a Runtime / Integration Package source before checking integration."));
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
    updateIntegrationSourceState();
    setSummaryText(QCoreApplication::translate("HarkenHub", "No integration check has run."));
    if (!integrationPathUserSelected_)
        setIntegrationPath({}, false);
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
                        self->updateIntegrationSourceState();
                        return;
                    }
                    self->studioVersion_ = version;
                    const QString displayName = versionLabel(version);
                    self->harkenProjectEdit_->setToolTip(
                        QCoreApplication::translate("HarkenHub", "Studio for opening this project: %1").arg(displayName));
                    self->updateDefaultIntegrationPath();
                    self->updateIntegrationSourceState();
                }, Qt::QueuedConnection);
        });
}

void EngineIntegrationsPage::updateDefaultIntegrationPath()
{
    if (integrationPathUserSelected_ || !studioVersion_)
        return;
    const auto targets = targets_->list();
    const auto target = std::find_if(
        targets.begin(), targets.end(), [&](const EngineTargetRecord& value) {
            return value.targetId == selectedEngineTargetId_;
        });
    setIntegrationPath(target == targets.end()
                   ? QFileInfo(studioVersion_->executablePath).absolutePath()
                   : studioIntegrationPath(*studioVersion_, target->engineKind), false);
}

void EngineIntegrationsPage::refreshLocalIntegrationSources()
{
    const auto targets = targets_->list();
    const auto target = std::find_if(targets.begin(), targets.end(), [this](const auto& value) {
        return value.targetId == selectedEngineTargetId_;
    });
    if (target == targets.end()) return;
    const QString selectedPath = integrationPathEdit_->text();
    const QSignalBlocker comboBlocker(integrationSourceCombo_);
    const QSignalBlocker editBlocker(integrationPathEdit_);
    integrationSourceCombo_->clear();
    auto sources = localIntegrationSources(target->engineKind, availableStudioVersions());
    std::stable_sort(sources.begin(), sources.end(), [](const auto& left, const auto& right) {
        const int comparison = compareSourceVersions(left.version, right.version);
        if (comparison != 0) return comparison > 0;
        return compareSourceVersions(left.studioVersion, right.studioVersion) > 0;
    });
    for (const auto& source : sources) {
        integrationSourceCombo_->addItem(source.path, source.path);
        integrationSourceCombo_->setItemData(integrationSourceCombo_->count() - 1, source.version, kIntegrationVersionRole);
        integrationSourceCombo_->setItemData(integrationSourceCombo_->count() - 1,
            integrationPackageName(target->engineKind), kIntegrationPackageNameRole);
        integrationSourceCombo_->setItemData(integrationSourceCombo_->count() - 1,
            source.label + QLatin1Char('\n') + QDir::toNativeSeparators(source.path), Qt::ToolTipRole);
    }
    integrationSourceCombo_->setEditText(selectedPath);
    updateIntegrationSourceState();
}

void EngineIntegrationsPage::setIntegrationPath(
    const QString& path, bool userSelected)
{
    const QSignalBlocker blocker(integrationPathEdit_);
    integrationPathEdit_->setText(path.trimmed().isEmpty()
                              ? QString()
                              : QDir::cleanPath(
                                    QFileInfo(path).absoluteFilePath()));
    integrationPathUserSelected_ = userSelected;
    clearResults();
    updateIntegrationSourceState();
    setSummaryText(QCoreApplication::translate("HarkenHub", "No integration check has run."));
}

void EngineIntegrationsPage::updateIntegrationSourceState()
{
    const auto targets = targets_->list();
    const auto target = std::find_if(targets.begin(), targets.end(), [this](const auto& value) {
        return value.targetId == selectedEngineTargetId_;
    });
    const EngineKind kind = target == targets.end() ? EngineKind::Unity : target->engineKind;
    const QString selectedPath = integrationPathEdit_->text().trimmed();
    int selectedIndex = -1;
    for (int index = 0; !selectedPath.isEmpty() && index < integrationSourceCombo_->count(); ++index) {
        if (samePath(selectedPath, integrationSourceCombo_->itemData(index).toString())) {
            selectedIndex = index;
            break;
        }
    }
    const QSignalBlocker comboBlocker(integrationSourceCombo_);
    const QSignalBlocker editBlocker(integrationPathEdit_);
    integrationSourceCombo_->setCurrentIndex(selectedIndex);
    integrationSourceCombo_->setEditText(selectedPath);
    integrationSourceCombo_->setToolTip(selectedIndex < 0 ? selectedPath
        : integrationSourceCombo_->currentData(Qt::ToolTipRole).toString());
    integrationSourceAvailable_ = !integrationVersion(selectedPath, kind).isEmpty();
    integrationPathEdit_->setToolTip(selectedPath);
    integrationPathEdit_->setCursorPosition(0);
    updatePrimaryAction();
}

QString EngineIntegrationsPage::selectedIntegrationPackageName() const
{
    const auto targets = targets_->list();
    const auto target = std::find_if(targets.begin(), targets.end(), [this](const auto& value) {
        return value.targetId == selectedEngineTargetId_;
    });
    return target == targets.end()
        ? QCoreApplication::translate("HarkenHub", "Integration Package")
        : integrationPackageName(target->engineKind);
}

void EngineIntegrationsPage::updatePrimaryAction()
{
    const QString packageName = selectedIntegrationPackageName();
    const bool current = integrationComparison_ == IntegrationComparison::Current;
    const bool linked = !editingIntegrationId_.isEmpty();
    const bool selecting = integrationSelection_.has_value();
    const auto inspection = targetInspections_.constFind(selectedEngineTargetId_);
    const bool inspecting = inspection != targetInspections_.cend()
        && !inspection->ready && inspection->cancelled;
    checkButton_->setEnabled(!operationRunning_ && !versionResolutionPending_ && !selecting && !inspecting);
    integrationCheckButton_->setVisible(selecting && (integrationPhase_ != IntegrationPhase::Choosing
        || integrationComparison_ != IntegrationComparison::Unknown));
    integrationCheckButton_->setText(QCoreApplication::translate("HarkenHub", "Check Again"));
    integrationCheckButton_->setEnabled(!operationRunning_ && !versionResolutionPending_ && selecting && !inspecting);
    changeIntegrationButton_->setText(integrationInstalled_
        ? QCoreApplication::translate("HarkenHub", "Change %1...").arg(packageName)
        : QCoreApplication::translate("HarkenHub", "Install %1...").arg(packageName));
    integrationSourceLabel_->setText(QCoreApplication::translate("HarkenHub", "%1 Source Directory").arg(packageName));
    integrationInstallLabel_->setText(QCoreApplication::translate("HarkenHub", "%1 Install Location").arg(packageName));
    integrationSourceBrowseButton_->setText(QCoreApplication::translate("HarkenHub", "Choose %1 Folder...").arg(packageName));
    changeIntegrationButton_->setEnabled(!operationRunning_ && !selecting
        && !selectedEngineTargetId_.isEmpty() && !selectedHarkenProjectPath_.isEmpty()
        && !targetBlocked_);
    integrationSelectionRow_->setVisible(selecting && integrationPhase_ != IntegrationPhase::Finished);
    integrationSelectionContext_->setVisible(selecting);
    integrationSafetyHint_->setVisible(selecting && integrationPhase_ != IntegrationPhase::Finished && engineRunning_);
    integrationReportButton_->setVisible(selecting && !integrationReportPath_.isEmpty());
    integrationReportButton_->setEnabled(!operationRunning_);
    updateIntegrationSelectionContext();
    integrationSourceBrowseButton_->setEnabled(selecting && !operationRunning_);
    cancelIntegrationButton_->setVisible(selecting || quickIntegrationChange_);
    cancelIntegrationButton_->setText(quickIntegrationChange_ || integrationPhase_ == IntegrationPhase::Finished
        ? QCoreApplication::translate("HarkenHub", "Close")
        : QCoreApplication::translate("HarkenHub", "Cancel"));
    cancelIntegrationButton_->setEnabled(!operationRunning_);
    integrateButton_->setVisible(selecting && integrationPhase_ != IntegrationPhase::Finished);
    integrateButton_->setText(integrationPhase_ == IntegrationPhase::Applying
        ? QCoreApplication::translate("HarkenHub", "Applying %1...").arg(packageName)
        : integrationPhase_ == IntegrationPhase::Checking
            ? QCoreApplication::translate("HarkenHub", "Checking %1...").arg(packageName)
        : current ? QCoreApplication::translate("HarkenHub", "Up to Date")
        : integrationPhase_ == IntegrationPhase::Reviewing
            ? (integrationInstalled_ ? QCoreApplication::translate("HarkenHub", "Confirm %1 Change").arg(packageName)
                             : QCoreApplication::translate("HarkenHub", "Confirm %1 Installation").arg(packageName))
            : QCoreApplication::translate("HarkenHub", "Check %1 Changes").arg(packageName));
    integrateButton_->setEnabled(!operationRunning_ && !versionResolutionPending_
        && !selectedHarkenProjectPath_.isEmpty() && !selectedEngineTargetId_.isEmpty()
        && integrationSourceAvailable_ && !targetBlocked_
        && integrationComparison_ != IntegrationComparison::Blocked && !current && selecting
        && integrationPhase_ != IntegrationPhase::Finished);
    integrateButton_->setToolTip(!integrationSourceAvailable_
        ? QCoreApplication::translate("HarkenHub", "Select a valid Integration Package source or run a check for details.")
        : integrationComparison_ == IntegrationComparison::Blocked
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
        unlinkProjectsButton_, changeIntegrationButton_, migrateIntegrationButton_, removeInstalledIntegrationButton_, integrationResolutionButton_};
    int actionWidth = kPathButtonMinimumWidth;
    for (auto* button : actionButtons)
        actionWidth = std::max(actionWidth, button->sizeHint().width());
    for (auto* button : actionButtons)
        button->setFixedWidth(actionWidth);
}

void EngineIntegrationsPage::updateIntegrationSelectionContext()
{
    if (!integrationSelection_) return;
    const auto targets = targets_->list();
    const auto target = std::find_if(targets.begin(), targets.end(), [this](const auto& value) {
        return value.targetId == selectedEngineTargetId_;
    });
    if (target == targets.end()) return;
    const QString version = integrationVersion(integrationPathEdit_->text(), target->engineKind);
    QString context = QCoreApplication::translate("HarkenHub", "Game project: %1 | Harken project: %2")
        .arg(target->displayName, QFileInfo(selectedHarkenProjectPath_).completeBaseName());
    if (!version.isEmpty()) {
        context += QLatin1Char('\n') + QCoreApplication::translate("HarkenHub", "%1 version: %2 → %3")
            .arg(integrationPackageName(target->engineKind), integrationPreviousVersion_, version);
        if (compareIntegrationVersions(version, integrationPreviousVersion_) < 0)
            context += QCoreApplication::translate("HarkenHub", " (Downgrade)");
    }
    integrationSelectionContext_->setText(context);
    integrationSelectionPaths_->setPlainText(QCoreApplication::translate("HarkenHub",
        "Harken project:\n%1\n\nGame project:\n%2\n\nIntegration Package source:\n%3\n\nIntegration Package installation location:\n%4")
        .arg(QDir::toNativeSeparators(selectedHarkenProjectPath_),
             QDir::toNativeSeparators(targetInputPath(*target)),
             QDir::toNativeSeparators(integrationPathEdit_->text()),
             QDir::toNativeSeparators(QDir(target->canonicalRoot).filePath(integrationInstallPathEdit_->text()))));
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
    setSummaryText(QCoreApplication::translate("HarkenHub", "Integration Package scan stopped. The check is incomplete; no Integration Package files were changed."));
}

EngineTargetObservation EngineIntegrationsPage::observeTarget(const EngineTargetRecord& target, bool verifyContent)
{
    const auto result = runIntegrationTask(this, [target, verifyContent]() {
        return EngineTargetProbe::observe(target, verifyContent);
    }, integrationPhase_ != IntegrationPhase::Applying, integrationSelection_ ? integrationProgress_ : nullptr);
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
    integrationComparison_ = IntegrationComparison::Unknown;
    stopped.status = QCoreApplication::translate("HarkenHub", "Integration Package scan stopped");
    stopped.remediation = QCoreApplication::translate("HarkenHub", "Run the Integration Package check again to finish inspecting this project.");
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
        readOnly, integrationSelection_ ? integrationProgress_ : nullptr);
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
    }, true, integrationSelection_ ? integrationProgress_ : nullptr);
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
    const bool pendingRestore = unity && VNX::UnityInstallation::hasPendingRestore(target->canonicalRoot);
    restoreIntegrationButton_->setVisible(unity);
    restoreIntegrationButton_->setText(pendingRestore
        ? QCoreApplication::translate("HarkenHub", "Recover Restoration...")
        : QCoreApplication::translate("HarkenHub", "Restore Current Version..."));
    restoreIntegrationButton_->setEnabled(unity && !operationRunning_ && !integrationSelection_);
    integrationHeading_->setText(target == targets.end() ? QCoreApplication::translate("HarkenHub", "Integration")
        : QCoreApplication::translate("HarkenHub", "%1 Integration").arg(engineName(target->engineKind)));
    removeInstalledIntegrationButton_->setVisible(true);
    removeInstalledIntegrationButton_->setText(unity && VNX::UnityInstallation::hasPendingRemoval(target->canonicalRoot)
        ? QCoreApplication::translate("HarkenHub", "Resume Integration Package Removal...")
        : QCoreApplication::translate("HarkenHub", "Uninstall Integration Package..."));
    migrateIntegrationButton_->setVisible(true);
    const EngineTargetObservation observation = target == targets.end()
        ? EngineTargetObservation{} : operationRunning_ ? observeTarget(*target)
                                                       : requestTargetObservation(*target);
    if (target != targets.end() && recoverLocation && !targetInspections_[target->targetId].ready)
        targetInspections_[target->targetId].recoverLocation = true;
    integrationInstalled_ = target != targets.end() && observation.validTarget
        && observation.health != EngineTargetHealth::NotInstalled;
    targetBlocked_ = target != targets.end() && !observation.canChangeIntegration;
    installedIntegrationVersion_ = target == targets.end()
        ? QCoreApplication::translate("HarkenHub", "No game project selected")
        : observation.integrationVersion.isEmpty() ? (observation.status.isEmpty()
            ? healthText(observation.health) : observation.status)
                                                  : observation.integrationVersion;
    installedIntegrationLabel_->setText(observation.integrationVersion.isEmpty() ? installedIntegrationVersion_
        : QCoreApplication::translate("HarkenHub", "(Integration Package Version: %1)").arg(installedIntegrationVersion_));
    installedIntegrationLabel_->setToolTip(localizeIntegrationText(observation.error));
    const QString installedPath = target == targets.end() || observation.integrationInstallPath.isEmpty()
        ? QString() : QDir::toNativeSeparators(QDir::cleanPath(
            QDir(target->canonicalRoot).absoluteFilePath(observation.integrationInstallPath)));
    installedIntegrationPath_->setText(installedPath);
    installedIntegrationPath_->setToolTip(installedPath);
    installedIntegrationPath_->setCursorPosition(0);
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
    integrationIssueLabel_->setText(issue);
    integrationIssueLabel_->setStyleSheet(observation.dependencyRestricted
        ? QStringLiteral("color: #9AA4B5;") : QStringLiteral("color: #F0C36A;"));
    integrationResolutionButton_->setText(observation.dependencyRestricted
        ? QCoreApplication::translate("HarkenHub", "View Dependencies...")
        : QCoreApplication::translate("HarkenHub", "View Resolution..."));
    integrationIssueRow_->setVisible(!issue.isEmpty());
    integrationResolutionButton_->setEnabled(!operationRunning_ && !integrationSelection_);
    changeIntegrationButton_->setToolTip(observation.canChangeIntegration ? QString() : observation.remediation);
    removeInstalledIntegrationButton_->setToolTip(observation.canRemoveIntegration ? QString() : observation.remediation);
    migrateIntegrationButton_->setToolTip(observation.canMoveIntegration ? QString() : observation.remediation);
    integrationInstallRow_->setVisible(target != targets.end() && !integrationInstalled_);
    updatePrimaryAction();
    if (target == targets.end()) {
        integrationInstallBrowseButton_->setText(QCoreApplication::translate("HarkenHub", "Browse..."));
        removeInstalledIntegrationButton_->setEnabled(!operationRunning_ && !integrationSelection_
            && observation.validTarget && observation.canRemoveIntegration);
        migrateIntegrationButton_->setText(QCoreApplication::translate("HarkenHub", "Move Integration Package..."));
        migrateIntegrationButton_->setEnabled(false);
        integrationInstallPathEdit_->setEnabled(false);
        integrationInstallBrowseButton_->setEnabled(false);
        integrationInstallBrowseButton_->hide();
        const QSignalBlocker blocker(integrationInstallPathEdit_);
        integrationInstallPathEdit_->clear();
        installPathTargetId_.clear();
        updatePrimaryAction();
        updateIdleSummary();
        return;
    }
    const bool installed = integrationInstalled_;
    const bool pendingMigration = hasPendingMigration(*target);
    integrationInstallBrowseButton_->setText(QCoreApplication::translate("HarkenHub", "Browse..."));
    integrationInstallBrowseButton_->setVisible(integrationSelection_.has_value() && !installed && !pendingMigration);
    integrationInstallPathEdit_->setEnabled(true);
    integrationInstallPathEdit_->setReadOnly(installed || !integrationSelection_);
    integrationInstallBrowseButton_->setEnabled(!operationRunning_
        && (!installed || pendingMigration || observation.health == EngineTargetHealth::Managed));
    migrateIntegrationButton_->setText(pendingMigration
        ? QCoreApplication::translate("HarkenHub", "Recover Integration Package Migration...")
        : QCoreApplication::translate("HarkenHub", "Move Integration Package..."));
    migrateIntegrationButton_->setEnabled(!operationRunning_ && !integrationSelection_
        && observation.canMoveIntegration);
    if (!operationRunning_ && (!observation.integrationInstallPath.isEmpty()
                              || installPathTargetId_ != target->targetId)) {
        const QSignalBlocker blocker(integrationInstallPathEdit_);
        integrationInstallPathEdit_->setText(!observation.integrationInstallPath.isEmpty()
            ? observation.integrationInstallPath
            : target->integrationInstallPath.isEmpty() ? (unity ? VNX::UnityInstallation::defaultRelativePath()
                                                       : QStringLiteral("Plugins/Harken"))
                                              : target->integrationInstallPath);
    }
    installPathTargetId_ = target->targetId;
    integrationInstallPathEdit_->setToolTip(observation.error.isEmpty()
        ? QDir::toNativeSeparators(QDir(target->canonicalRoot).filePath(integrationInstallPathEdit_->text()))
        : localizeIntegrationText(observation.error));
    if (recoverLocation && observation.health == EngineTargetHealth::Managed
        && observation.integrationInstallPath != target->integrationInstallPath
        && !targets_->updateIntegrationInstallPath(target->targetId, observation.integrationInstallPath)) {
        setSummaryText(QCoreApplication::translate(
            "HarkenHub", "The Integration Package was found, but its installation location could not be saved.\n%1")
                .arg(targets_->lastError()));
    }
    removeInstalledIntegrationButton_->setEnabled(
        !operationRunning_ && !integrationSelection_ && observation.validTarget
        && observation.canRemoveIntegration);
    updatePrimaryAction();
    updateIdleSummary();
}

void EngineIntegrationsPage::showIntegrationResolution()
{
    if (operationRunning_ || integrationSelection_) return;
    const auto targets = targets_->list();
    const auto target = std::find_if(targets.begin(), targets.end(), [this](const auto& value) {
        return value.targetId == selectedEngineTargetId_;
    });
    if (target == targets.end()) return;
    const auto observation = observeTarget(*target);
    if (inspectionStopped_) return;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("integrationResolutionDialog"));
    dialog.setWindowTitle(observation.dependencyRestricted
        ? QCoreApplication::translate("HarkenHub", "Integration Package Dependencies")
        : QCoreApplication::translate("HarkenHub", "Integration Package Resolution"));
    configureDialog(&dialog, QSize(680, 430), QSize(480, 300));
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
            return QCoreApplication::translate("HarkenHub", "No Integration Package installed");
        return supported ? QCoreApplication::translate("HarkenHub", "Available after safety checks")
                         : QCoreApplication::translate("HarkenHub", "Resolve the issue above first");
    };
    sections << QCoreApplication::translate("HarkenHub", "Change Integration Package: %1\nMove Integration Package: %2\nUninstall Integration Package: %3")
        .arg(availability(observation.canChangeIntegration),
             hasPendingMigration(*target)
                    ? QCoreApplication::translate("HarkenHub", "Use Recover Integration Package Migration first")
                    : availability(observation.canMoveIntegration),
             availability(observation.canRemoveIntegration));
    auto* details = new QPlainTextEdit(&dialog);
    details->setObjectName(QStringLiteral("integrationResolutionDetails"));
    details->setReadOnly(true);
    details->setPlainText(sections.join(QStringLiteral("\n\n")));
    layout->addWidget(details, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    const QString directory = observation.integrationInstallPath.isEmpty() ? target->canonicalRoot
        : QDir(target->canonicalRoot).filePath(observation.integrationInstallPath);
    auto* open = buttons->addButton(observation.integrationInstallPath.isEmpty()
        ? QCoreApplication::translate("HarkenHub", "Open Project Folder")
        : QCoreApplication::translate("HarkenHub", "Open Integration Package Folder"), QDialogButtonBox::ActionRole);
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
    if (engineStatusPending_ || operationRunning_ || !integrationSelectionDialog_->isVisible()
        || !integrationSelection_ || integrationPhase_ == IntegrationPhase::Finished) return;
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
            if (self->selectedEngineTargetId_ != target.targetId || !self->integrationSelectionDialog_->isVisible()) return;
            self->engineRunning_ = isOpen;
            self->integrationSafetyHint_->setText(isOpen ? localizeIntegrationText(error) : QString());
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
    integrationSelectionRow_->setEnabled(!running);
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

void EngineIntegrationsPage::setSummaryText(const QString& text, bool succeeded)
{
    const QString style = succeeded ? QStringLiteral("color: #69D391;") : QString();
    summaryLabel_->setStyleSheet(style);
    integrationSelectionSummary_->setStyleSheet(style);
    showingIdleSummary_ = text == QCoreApplication::translate(
        "HarkenHub", "No integration check has run.");
    if (showingIdleSummary_)
        updateIdleSummary();
    else
        summaryLabel_->setText(text);
    integrationSelectionSummary_->setText(summaryLabel_->text());
}

void EngineIntegrationsPage::updateIdleSummary()
{
    if (!showingIdleSummary_ || operationRunning_) return;
    QStringList hints;
    if (integrationSelection_ && !integrationPathEdit_->text().trimmed().isEmpty() && !integrationSourceAvailable_)
        hints << QCoreApplication::translate("HarkenHub", "The selected Integration Package source is unavailable.");
    if (integrationInstalled_)
        hints << (integrationSelection_
            ? QCoreApplication::translate("HarkenHub", "Installed Integration Package detected. Target Integration Package has not been compared yet.")
            : QCoreApplication::translate("HarkenHub", "Integration Package installed."));
    if (targetBlocked_)
        hints << installedIntegrationLabel_->toolTip();
    EngineTargetRecord target;
    target.canonicalRoot = engineProjectEdit_->text();
    if (VNX::UnityInstallation::hasPendingMigration(target.canonicalRoot)
        || ProjectIntegrationExecutor::hasPendingUnrealMigration(target))
        hints << QCoreApplication::translate("HarkenHub", "The Integration Package migration is unfinished. Choose \"Recover...\" before continuing.");
    summaryLabel_->setText(hints.isEmpty()
        ? QCoreApplication::translate("HarkenHub", "No integration check has run.")
        : hints.join(QLatin1Char('\n')));
    integrationSelectionSummary_->setText(summaryLabel_->text());
}

void EngineIntegrationsPage::clearResults()
{
    summaryLabel_->setStyleSheet(QString());
    integrationSelectionSummary_->setStyleSheet(QString());
    reviewedUnrealPlan_.reset();
    reviewedIntegrationHash_.clear();
    integrationReportPath_.clear();
    if (!operationRunning_) integrationPhase_ = IntegrationPhase::Choosing;
    integrationComparison_ = IntegrationComparison::Unknown;
    const QSignalBlocker blocker(stepsTable_);
    resultSteps_.clear();
    stepsTable_->clearContents();
    stepsTable_->setRowCount(0);
    detailEdit_->clear();
    integrationSelectionDetails_->clear();
    integrationDiffSummary_->clear();
    integrationDiffTree_->clear();
    integrationResultsTabs_->setTabText(1, QCoreApplication::translate("HarkenHub", "Check Log"));
    integrationResultsTabs_->setCurrentIndex(0);
    detailEdit_->hide();
    summaryLabel_->setToolTip(QString());
    updatePrimaryAction();
}

void EngineIntegrationsPage::runUnityRestoration()
{
    if (operationRunning_ || integrationSelection_) return;
    const auto targets = targets_->list();
    const auto selected = std::find_if(targets.begin(), targets.end(), [&](const auto& item) {
        return item.targetId == selectedEngineTargetId_ && item.engineKind == EngineKind::Unity;
    });
    if (selected == targets.end()) return;
    const auto target = *selected;
    setOperationRunning(true);
    const auto finish = qScopeGuard([this]() { setOperationRunning(false); });
    clearResults();
    UnityIntegrationRequest request;
    request.unityProjectPath = target.canonicalRoot;
    request.integrationInstallPath = integrationInstallPathEdit_->text().trimmed();
    request.restoreIntegration = true;
    request.recoverRestore = VNX::UnityInstallation::hasPendingRestore(target.canonicalRoot);
    request.checkOnly = true;
    OfficialUnitySourceResult source;
    if (!request.recoverRestore) {
        QString recordedSource;
        for (const auto& record : integrations_->list()) {
            if (record.engineTargetId == target.targetId) {
                recordedSource = record.integrationSourcePath;
                break;
            }
        }
        const auto sources = runIntegrationTask(this, [target, recordedSource]() {
            OfficialUnitySourceResult failed;
            const auto discovery = VNX::UnityInstallation::discover(target.canonicalRoot);
            if (!discovery.errors.isEmpty() || discovery.installations.size() != 1) {
                failed.error = QStringLiteral("A single managed Integration Package installation must be identified before restoration.");
                return failed;
            }
            const QString installed = discovery.installations.front().path;
            QString pathError;
            const QString statePath = QDir(installed).filePath(QStringLiteral(".harken-integration-state.json"));
            QFile file(statePath);
            if (!VNX::UnityInstallation::validatePath(statePath, &pathError)
                || !file.open(QIODevice::ReadOnly) || file.size() > PackageInventory::maximumBytes) {
                failed.error = QStringLiteral("The installation record could not be read safely. Restore the record from backup before continuing.");
                return failed;
            }
            const auto receipt = QJsonDocument::fromJson(file.readAll()).object();
            OfficialUnitySourceRequest sourceRequest;
            sourceRequest.buildId = receipt.value(QStringLiteral("source_release")).toObject().value(QStringLiteral("buildId")).toString();
            sourceRequest.packageVersion = receipt.value(QStringLiteral("package_version")).toString();
            for (const auto& studio : availableStudioVersions()) {
                const QString candidate = studioIntegrationPath(studio, EngineKind::Unity);
                if ((!sourceRequest.buildId.isEmpty() && studio.launchBuildId() == sourceRequest.buildId)
                    || (sourceRequest.buildId.isEmpty() && !recordedSource.isEmpty() && samePath(candidate, recordedSource))) {
                    sourceRequest.buildId = studio.launchBuildId();
                    sourceRequest.localSource = candidate;
                    break;
                }
            }
            if (sourceRequest.buildId.isEmpty()) {
                failed.error = QStringLiteral("The original release build could not be identified. Select the original release through Change Integration Package.");
                return failed;
            }
            return OfficialUnitySource::prepare(sourceRequest, TrustAnchors::manifestPublicKey());
        }, true);
        if (!sources) { scanStopped(); return; }
        source = *sources;
        if (!source.ok) {
            ResultStep failure;
            failure.id = QStringLiteral("official_source");
            failure.title = QCoreApplication::translate("HarkenHub", "Verify official Integration Package source");
            failure.status = ResultStatus::Failed;
            failure.cause = HubUnityIntegrationService::localizeText(source.error);
            showResults({failure}, QCoreApplication::translate("HarkenHub", "The original official package could not be verified. No project files were changed."));
            return;
        }
        request.integrationSourcePath = source.sourcePath;
        request.officialRelease = QJsonObject{{QStringLiteral("buildId"), source.release.buildId},
            {QStringLiteral("version"), source.release.version}, {QStringLiteral("channel"), source.release.channel},
            {QStringLiteral("platform"), source.release.platform}, {QStringLiteral("architecture"), source.release.architecture},
            {QStringLiteral("packageSha256"), source.release.sha256Hex},
            {QStringLiteral("packageManifestSha256"), source.inventoryHash}};
        const QString prefix = source.inventory.unityRoot + QLatin1Char('/');
        for (auto it = source.inventory.files.cbegin(); it != source.inventory.files.cend(); ++it)
            if (it.key().startsWith(prefix)) request.officialFileHashes.insert(it.key().mid(prefix.size()), it->sha256.toLatin1());
    }
    auto result = runUnityTask(request);
    if (result.cancelled) return;
    showResults(unityResultSteps(result, true), QCoreApplication::translate("HarkenHub", "Review the restoration plan before continuing."), QStringLiteral("restore"));
    if (!result.success || result.targetAlreadyCurrent) return;
    request.integrationInstallPath = result.integrationInstallPath;
    request.restorationBackupPath = result.restorationBackupPath;
    request.approvedPlanHash = result.planHash;
    if (!result.conflictingFiles.isEmpty()) {
        if (!confirmIntegrationAction(this,
                QCoreApplication::translate("HarkenHub", "Conflicting Project Files"),
                QCoreApplication::translate("HarkenHub", "These files are outside the installation record but occupy official package paths. Back them up and replace them during this restoration?"),
                result.conflictingFiles.join(QLatin1Char('\n')),
                QCoreApplication::translate("HarkenHub", "Back Up and Replace"), QDialogButtonBox::DestructiveRole)) return;
        request.confirmUnownedConflicts = true;
    }
    const QString title = request.recoverRestore
        ? QCoreApplication::translate("HarkenHub", "Recover Restoration")
        : QCoreApplication::translate("HarkenHub", "Restore Current Version");
    const QString summary = request.recoverRestore
        ? QCoreApplication::translate("HarkenHub", "Reconcile the interrupted restoration using its verified backup. Existing files and backup folders will be preserved.")
        : QCoreApplication::translate("HarkenHub", "Restore files from the official package for the installed version. Local changes to these files, including managed Unity metadata, will be replaced. Custom files outside these paths will be preserved. Review any conflicting files listed below.\n\nOfficial build: %1\nBackup: %2\n\nClose Unity and complete any required P4 checkout before continuing.")
            .arg(source.release.buildId, QDir::toNativeSeparators(result.restorationBackupPath));
    if (!confirmIntegrationAction(this, title, summary, confirmationDetails(), title, QDialogButtonBox::DestructiveRole)) return;
    request.checkOnly = false;
    request.confirmTargetChanges = true;
    result = runUnityTask(request);
    if (result.cancelled) return;
    showResults(unityResultSteps(result), result.success
        ? QCoreApplication::translate("HarkenHub", "Restoration completed. The safety backup has been retained.")
        : QCoreApplication::translate("HarkenHub", "Restoration did not complete. Review the results and preserve the listed backup folders."), QStringLiteral("restore"), result.success);
    emit integrationStatusChanged(target.targetId);
}

void EngineIntegrationsPage::runCheck()
{
    if (operationRunning_) return;
    if (!integrationSelection_) {
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
                step.title = QCoreApplication::translate("HarkenHub", "Verify installed Integration Package");
                step.status = success ? ResultStatus::CheckPassed : ResultStatus::Failed;
                step.cause = result.error;
                step.artifacts = {result.targetRoot};
                step.summary = success
                    ? QCoreApplication::translate("HarkenHub", "Installed Integration Package files match the installation record.")
                    : QCoreApplication::translate("HarkenHub", "Installed Integration Package files do not match the installation record or could not be read.");
                step.details = result.modifiedManagedFiles;
                if (!result.modifiedManagedFiles.isEmpty())
                    step.remediation = QCoreApplication::translate("HarkenHub", "Review the listed files and local changes. Restore missing files from backup or use Change Integration Package with a complete Integration Package.");
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
                ? QCoreApplication::translate("HarkenHub", "Installed Integration Package verification passed. No files were changed.")
                : QCoreApplication::translate("HarkenHub", "Installed Integration Package verification did not pass. Review the results; no files were changed."),
                QStringLiteral("verify"), success);
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
        target, integrationPathEdit_->text().trimmed());
    if (!success)
        stepsTable_->selectRow(std::max(0, stepsTable_->currentRow()));
}

void EngineIntegrationsPage::runIntegration()
{
    if (operationRunning_ || !integrationSelection_) return;
    if (integrationPhase_ != IntegrationPhase::Reviewing || reviewedIntegrationHash_.isEmpty()) {
        runCheck();
        return;
    }
    ProjectRecord project;
    EngineTargetRecord target;
    if (!resolveSelection(&project, &target, true, true)) return;
    const QString integrationPath = integrationPathEdit_->text().trimmed();
    const QByteArray approved = reviewedIntegrationHash_;
    setOperationRunning(true);
    const auto finish = qScopeGuard([this]() { setOperationRunning(false); });
    if (!runCheckForSelection(target, integrationPath)) return;
    if (integrationComparison_ == IntegrationComparison::Current) return;
    if (approved != reviewedIntegrationHash_) {
        setSummaryText(QCoreApplication::translate("HarkenHub",
            "Integration Package files changed. Review the refreshed results and confirm again. No Integration Package files were changed."));
        return;
    }
    const QString conflict = conflictingIntegration(project.canonicalPath, target.targetId);
    if (!conflict.isEmpty()) {
        integrationPhase_ = IntegrationPhase::Choosing;
        reviewedIntegrationHash_.clear();
        setSummaryText(QCoreApplication::translate("HarkenHub",
            "The selected project relationship was created while the check was running."));
        return;
    }
    integrationPhase_ = IntegrationPhase::Applying;
    integrationReviewInvalidated_ = false;
    updatePrimaryAction();
    const bool success = target.engineKind == EngineKind::Unity
        ? runUnity(target, integrationPath, false) : runUnreal(target, integrationPath, false);
    if (integrationReviewInvalidated_) {
        if (runCheckForSelection(target, integrationPath))
            setSummaryText(QCoreApplication::translate("HarkenHub",
                "Integration Package files changed. Review the refreshed results and confirm again. No Integration Package files were changed."));
        return;
    }
    integrationPhase_ = IntegrationPhase::Finished;
    if (success && !editingIntegrationId_.isEmpty())
        persistIntegration(project.canonicalPath, target.targetId, integrationPath);
    setEditorLocked(!editingIntegrationId_.isEmpty());
}

bool EngineIntegrationsPage::runCheckForSelection(
    const EngineTargetRecord& target, const QString& integrationPath)
{
    integrationPhase_ = IntegrationPhase::Checking;
    updatePrimaryAction();
    const bool success = target.engineKind == EngineKind::Unity
        ? runUnity(target, integrationPath, true) : runUnreal(target, integrationPath, true);
    integrationPhase_ = success && integrationComparison_ == IntegrationComparison::Different && !reviewedIntegrationHash_.isEmpty()
        ? IntegrationPhase::Reviewing : IntegrationPhase::Choosing;
    if (success) {
        integrationPreviousVersion_ = integrationInstalled_ ? installedIntegrationVersion_
            : QCoreApplication::translate("HarkenHub", "Not installed");
        if (integrationPhase_ == IntegrationPhase::Reviewing)
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
                            ? QCoreApplication::translate("HarkenHub", "Check report written. No Integration Package changes were executed.")
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
    showResults(steps, QCoreApplication::translate("HarkenHub", "Cancelled. Integration Package files and project relationships were not changed."), cancelledStep);
}

void EngineIntegrationsPage::runIntegrationMigration(bool recover)
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
    if (selected == targets.end() || operationRunning_ || integrationSelection_) return;
    const EngineTargetRecord target = *selected;
    setOperationRunning(true);
    const auto finish = qScopeGuard([this]() { setOperationRunning(false); });
    UnityIntegrationRequest request;
    request.unityProjectPath = target.canonicalRoot;
    request.integrationInstallPath = integrationInstallPathEdit_->text().trimmed();
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
    IntegrationMigrationPlan unrealPlan;
    const auto execute = [&]() -> MigrationResult {
        clearResults();
        setSummaryText(request.confirmMigration
            ? QCoreApplication::translate("HarkenHub", "The approved Integration Package migration is running...")
            : QCoreApplication::translate("HarkenHub", "Checking Integration Package migration safety..."));
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        if (target.engineKind == EngineKind::Unity) {
            const auto result = runUnityTask(request);
            if (!result.cancelled)
                showResults(unityResultSteps(result, !request.confirmMigration), summaryLabel_->text());
            return {result.cancelled, result.requiresConfirmation,
                    result.integrationMigrated || result.migrationRecovered,
                    result.integrationInstallPath, result.migrationTargetPath,
                    result.migrationBackupPath, result.migrationPlanHash};
        }
        ResultStep step;
        step.id = QStringLiteral("migrate_sdk");
        step.title = QCoreApplication::translate("HarkenHub", "Review Integration Package migration");
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
                ? QCoreApplication::translate("HarkenHub", "Safety check passed. No Integration Package files were moved.")
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
                ? QCoreApplication::translate("HarkenHub", "Integration Package migration completed. All installed files and project relationships were kept.")
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
        dialog.setWindowTitle(QCoreApplication::translate("HarkenHub", "Migrate Integration Package"));
        configureDialog(&dialog, QSize(640, 400), QSize(480, 300));
        auto* layout = new QVBoxLayout(&dialog);
        layout->addWidget(new QLabel(QCoreApplication::translate("HarkenHub", "New Integration Package installation location"), &dialog));
        auto* row = new QHBoxLayout();
        auto* path = new QLineEdit(request.integrationInstallPath, &dialog);
        path->setObjectName(QStringLiteral("integrationMigrationDestination"));
        auto* browse = new QPushButton(QCoreApplication::translate("HarkenHub", "Browse..."), &dialog);
        row->addWidget(path, 1);
        row->addWidget(browse);
        layout->addLayout(row);
        auto* safetyStatus = new DialogTextView(&dialog);
        safetyStatus->setText(QCoreApplication::translate("HarkenHub", "Safety check required."));
        safetyStatus->setObjectName(QStringLiteral("integrationMigrationSafetyStatus"));
        layout->addWidget(safetyStatus);
        auto* safetyDetails = new QPlainTextEdit(&dialog);
        safetyDetails->setObjectName(QStringLiteral("integrationMigrationSafetyDetails"));
        safetyDetails->setReadOnly(true);
        safetyDetails->setMinimumHeight(160);
        layout->addWidget(safetyDetails, 1);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
        auto* safetyCheck = buttons->addButton(QCoreApplication::translate("HarkenHub", "Safety Check"), QDialogButtonBox::ActionRole);
        safetyCheck->setObjectName(QStringLiteral("integrationMigrationSafetyCheck"));
        auto* next = buttons->addButton(QCoreApplication::translate("HarkenHub", "Continue..."), QDialogButtonBox::AcceptRole);
        next->setObjectName(QStringLiteral("integrationMigrationContinue"));
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
            safetyStatus->setText(QCoreApplication::translate("HarkenHub", "Checking Integration Package migration safety..."));
            safetyDetails->clear();
            request.migrationTargetPath = path->text().trimmed();
            reviewedResult = execute();
            safetyCheckRun = true;
            const bool passed = reviewedResult->requiresConfirmation
                && !reviewedResult->planHash.isEmpty();
            safetyStatus->setText(reviewedResult->cancelled ? summaryLabel_->text() : passed
                ? QCoreApplication::translate("HarkenHub", "Safety check passed. No Integration Package files were moved.")
                : QCoreApplication::translate("HarkenHub", "Safety check failed. No Integration Package files were moved."));
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
                QCoreApplication::translate("HarkenHub", "Select Integration Package Installation Directory"),
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
            ? QCoreApplication::translate("HarkenHub", "Restore the Integration Package to its previous location using the migration record. Project relationships will be kept.\n\nPrevious location:\n%1\n\nMigration destination:\n%2\n\nBackup:\n%3")
            : target.engineKind == EngineKind::Unity
                ? QCoreApplication::translate("HarkenHub", "Move the installed Integration Package with all existing files, modifications and Unity metadata. Project relationships will be kept. A verified backup will be created first.\n\nCurrent location:\n%1\n\nNew location:\n%2\n\nBackup:\n%3\n\nClose Unity before continuing. Any paths hard-coded in your project must be updated separately.")
                : QCoreApplication::translate("HarkenHub", "Move the installed Integration Package with all existing files and modifications. Project relationships will be kept. A verified backup will be created first.\n\nCurrent location:\n%1\n\nNew location:\n%2\n\nBackup:\n%3\n\nClose Unreal Editor before continuing. Any paths hard-coded in your project must be updated separately.");
        if (!confirmIntegrationAction(this,
                recover ? QCoreApplication::translate("HarkenHub", "Recover Integration Package Migration")
                        : QCoreApplication::translate("HarkenHub", "Migrate Integration Package"),
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
        if (!targets_->updateIntegrationInstallPath(target.targetId, result.sourcePath)) {
            setSummaryText(QCoreApplication::translate("HarkenHub", "The Integration Package location changed, but Hub could not save it. Reopen this integration to rediscover the location.\n%1")
                .arg(localizeIntegrationText(targets_->lastError())));
        } else {
            setSummaryText(recover
                ? QCoreApplication::translate("HarkenHub", "The Integration Package was restored to its previous location. Project relationships were kept.")
                : target.engineKind == EngineKind::Unity
                    ? QCoreApplication::translate("HarkenHub", "Integration Package migration completed. All installed files, Unity GUIDs and project relationships were kept.")
                    : QCoreApplication::translate("HarkenHub", "Integration Package migration completed. All installed files and project relationships were kept."));
        }
    } else {
        setSummaryText(hasPendingMigration(target)
            ? QCoreApplication::translate("HarkenHub", "The Integration Package migration is unfinished. Choose \"Recover...\" before continuing.")
            : QCoreApplication::translate("HarkenHub", "Integration Package migration failed. Select the failed step for details."));
    }
    if (!result.backupPath.isEmpty())
        summaryLabel_->setToolTip(QCoreApplication::translate("HarkenHub", "Safety backup: %1").arg(result.backupPath));
}

void EngineIntegrationsPage::runIntegrationRemoval()
{
    if (operationRunning_ || integrationSelection_) return;
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
        ? QCoreApplication::translate("HarkenHub", "Safety check passed. No Integration Package files were changed.") : plan.error;
    review.details = plan.preservedTargetFiles;
    review.artifacts = {plan.targetRoot};
    showResults({review}, review.summary);
    if (!plan.valid) return;
    if (!confirmIntegrationAction(this,
            QCoreApplication::translate("HarkenHub", "Uninstall Integration Package"),
            QCoreApplication::translate("HarkenHub", "Uninstall the Integration Package from this game project? A backup will be kept in Saved/HarkenHub/Backups. Project-specific files and the project relationship will be kept.\n\n%1")
                .arg(QDir::toNativeSeparators(plan.targetRoot)),
            confirmationDetails(), QCoreApplication::translate("HarkenHub", "Uninstall Integration Package"),
            QDialogButtonBox::DestructiveRole)) {
        cancelPendingOperation();
        return;
    }
    const auto result = *runIntegrationTask(this, [target, plan]() {
        return ProjectIntegrationExecutor::executeUnreal(target, plan, IntegrationOperation::Remove, true);
    });
    ResultStep removal;
    removal.id = QStringLiteral("remove_sdk");
    removal.title = QCoreApplication::translate("HarkenHub", "Uninstall Integration Package");
    removal.status = result.success ? ResultStatus::Succeeded : ResultStatus::Failed;
    removal.summary = result.success
        ? QCoreApplication::translate("HarkenHub", "Integration Package removed. Project-specific files and the project relationship were kept.")
        : result.error;
    if (!result.backupPath.isEmpty()) removal.artifacts = {result.backupPath};
    showResults({review, removal}, removal.summary, removal.id);
}

void EngineIntegrationsPage::runUnityRemoval()
{
    if (operationRunning_ || integrationSelection_) return;
    const auto targets = targets_->list();
    const auto selected = std::find_if(
        targets.begin(), targets.end(), [&](const EngineTargetRecord& value) {
            return value.targetId == selectedEngineTargetId_
                && samePath(targetInputPath(value),
                            engineProjectEdit_->text().trimmed());
        });
    if (selected == targets.end()
        || selected->engineKind != EngineKind::Unity) {
        MessageDialog::warning(
            this, QCoreApplication::translate("HarkenHub", "Uninstall Unity Integration Package"),
            QCoreApplication::translate("HarkenHub", "The selected Unity project is no longer available."));
        return;
    }
    const EngineTargetRecord target = *selected;
    setOperationRunning(true);
    const auto finish = qScopeGuard([this]() { setOperationRunning(false); });

    removeInstalledIntegrationButton_->setEnabled(false);

    UnityIntegrationRequest request;
    request.unityProjectPath = target.canonicalRoot;
    request.integrationInstallPath = integrationInstallPathEdit_->text().trimmed();
    request.removeIntegration = true;

    auto execute = [this, &request]() {
        clearResults();
        setSummaryText(request.confirmRemoval
            ? QCoreApplication::translate("HarkenHub", "Removing the approved Unity Integration Package installation...")
            : QCoreApplication::translate("HarkenHub", "Reviewing the installed Unity Integration Package and safety backup plan..."));
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
        request.approvedPlanHash = result.planHash;
        if (confirmIntegrationAction(
                this,
                QCoreApplication::translate(
                    "HarkenHub", "Uninstall Unity Integration Package"),
                QCoreApplication::translate(
                    "HarkenHub",
                    "A verified backup will be created before Harken-managed Unity Integration Package and runtime files are removed. Project-specific files and the project relationship will be kept.\n\nBackup folder:\n%1\n\nClose Unity before continuing.")
                    .arg(QDir::toNativeSeparators(result.removalBackupPath)),
                confirmationDetails(),
                QCoreApplication::translate(
                    "HarkenHub", "Uninstall Integration Package"),
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
                "The Unity Integration Package was backed up and removed, but the removal report could not be completed. Backup: %1")
                .arg(QDir::toNativeSeparators(result.removalBackupPath)));
        } else if (result.hasWarnings) {
            setSummaryText(QCoreApplication::translate(
                "HarkenHub",
                "The Unity Integration Package and runtimes were backed up and removed. Review is recommended for cleanup details. Backup: %1")
                .arg(QDir::toNativeSeparators(result.removalBackupPath)));
        } else {
            setSummaryText(QCoreApplication::translate(
                "HarkenHub",
                "The Unity Integration Package and runtimes were backed up and removed. %1 project-specific file(s) were preserved. Backup: %2")
                .arg(result.targetOnlyFilesPreserved)
                .arg(QDir::toNativeSeparators(result.removalBackupPath)));
        }
    } else if (result.success && !result.integrationFound) {
        setSummaryText(QCoreApplication::translate(
            "HarkenHub", "No Harken Unity Integration Package is installed in this project."));
    } else if (VNX::UnityInstallation::hasPendingRemoval(target.canonicalRoot)) {
        setSummaryText(QCoreApplication::translate("HarkenHub",
            "Integration Package removal is unfinished. The verified backup and removal record have been kept."));
    } else if (result.removalBackupCreated) {
        setSummaryText(QCoreApplication::translate(
            "HarkenHub",
            "The safety backup was created, but removal stopped before changing project files. Backup: %1")
            .arg(QDir::toNativeSeparators(result.removalBackupPath)));
    } else {
        setSummaryText(QCoreApplication::translate(
            "HarkenHub",
            "Unity Integration Package removal failed. Select the first failed step for details."));
    }
    if (!result.textReportPath.isEmpty())
        summaryLabel_->setToolTip(QCoreApplication::translate(
            "HarkenHub", "Removal report: %1").arg(result.textReportPath));
    updateRemovalButtonState();
}

bool EngineIntegrationsPage::runUnity(
    const EngineTargetRecord& target, const QString& integrationPath, bool checkOnly)
{
    const bool updating = integrationInstalled_;
    const QByteArray approved = reviewedIntegrationHash_;
    clearResults();
    setSummaryText(checkOnly
        ? QCoreApplication::translate("HarkenHub", "Checking only. No Integration Package files will be changed...")
        : QCoreApplication::translate("HarkenHub", "The approved Integration Package installation is running..."));
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    UnityIntegrationRequest request;
    request.unityProjectPath = target.canonicalRoot;
    request.integrationSourcePath = integrationPath;
    request.preserveUnownedTarget = true;
    request.integrationInstallPath = integrationInstallPathEdit_->text().trimmed();
    request.checkOnly = checkOnly;
    request.confirmTargetChanges = !checkOnly;
    if (!checkOnly) request.approvedPlanHash = approved;
    const UnityIntegrationResult result =
        runUnityTask(request);
    if (result.cancelled) return false;
    if (checkOnly && result.success) reviewedIntegrationHash_ = result.planHash;
    if (!checkOnly) integrationReviewInvalidated_ = result.requiresConfirmation;
    integrationReportPath_ = QFileInfo::exists(result.textReportPath) ? result.textReportPath : QString();
    const QVector<ResultStep> steps = unityResultSteps(result, checkOnly);
    integrationComparison_ = !result.success ? IntegrationComparison::Blocked
        : (checkOnly ? result.targetAlreadyCurrent : result.targetCurrent)
            ? IntegrationComparison::Current : IntegrationComparison::Different;

    QString summary;
    if (checkOnly) {
        if (!result.success)
            summary = QCoreApplication::translate("HarkenHub", "Integration Package check failed. Select the first failed step for details.");
        else if (!result.targetInstalled)
            summary = QCoreApplication::translate("HarkenHub", "Check completed. The selected Integration Package is valid and is not installed in this Unity project.");
        else if (result.changesDetected)
            summary = QCoreApplication::translate("HarkenHub", "Check completed. Differences were found; no Integration Package files were changed.");
        else if (result.hasWarnings)
            summary = QCoreApplication::translate("HarkenHub", "Check completed. Review is recommended for the highlighted steps.");
        else
            summary = QCoreApplication::translate("HarkenHub", "Check completed. The installed Unity Integration Package matches the selected source.");
    } else if (!result.success) {
        summary = QCoreApplication::translate("HarkenHub", "Integration Package installation failed. Select the first failed step for details.");
    } else if (result.targetAlreadyCurrent) {
        summary = QCoreApplication::translate("HarkenHub", "The Unity Integration Package is already current and passed verification.");
    } else if (result.hasWarnings) {
        summary = QCoreApplication::translate("HarkenHub", "Integration Package installation completed. Review is recommended for the highlighted steps.");
    } else {
        summary = updating
            ? QCoreApplication::translate("HarkenHub", "Integration Package update complete. Installed files passed verification.")
            : QCoreApplication::translate("HarkenHub", "Integration Package installation complete. Installed files passed verification.");
    }
    showResults(steps, summary, checkOnly ? QStringLiteral("compare_sdk")
                                        : QStringLiteral("install_sdk"),
                !checkOnly && result.success);
    if (!result.textReportPath.isEmpty())
        summaryLabel_->setToolTip(
            QCoreApplication::translate("HarkenHub", "Report: %1").arg(result.textReportPath));
    updateRemovalButtonState();
    if (!checkOnly && result.targetCurrent
        && !targets_->updateIntegrationInstallPath(target.targetId, result.integrationInstallPath)) {
        setSummaryText(summary + QLatin1Char('\n') + QCoreApplication::translate(
            "HarkenHub", "The Integration Package was installed, but its installation location could not be saved.\n%1")
                .arg(targets_->lastError()));
        return false;
    }
    return result.success;
}

bool EngineIntegrationsPage::runUnreal(
    const EngineTargetRecord& target, const QString& integrationPath, bool checkOnly)
{
    const auto reviewed = reviewedUnrealPlan_;
    clearResults();
    setSummaryText(checkOnly
        ? QCoreApplication::translate("HarkenHub", "Checking only. No Integration Package files will be changed...")
        : QCoreApplication::translate("HarkenHub", "The approved Integration Package installation is running..."));
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
        executionTarget.integrationInstallPath = integrationInstallPathEdit_->text().trimmed();
    const IntegrationPlan plan = checkOnly
        ? inspectUnrealTask(executionTarget, integrationPath, operation)
        : reviewed.value_or(IntegrationPlan{});
    if (plan.cancelled) return false;
    if (checkOnly && plan.valid) {
        reviewedUnrealPlan_ = plan;
        reviewedIntegrationHash_ = plan.fingerprint;
    }
    integrationComparison_ = !plan.valid ? IntegrationComparison::Blocked
        : plan.changes.empty() ? IntegrationComparison::Current : IntegrationComparison::Different;

    ResultStep sourceStep;
    sourceStep.id = QStringLiteral("validate_sdk");
    sourceStep.title = QCoreApplication::translate("HarkenHub", "Validate Integration Package source");
    sourceStep.status = plan.valid ? ResultStatus::Succeeded
                                   : ResultStatus::Failed;
    sourceStep.summary = plan.valid
        ? QCoreApplication::translate("HarkenHub", "Validated Unreal Integration Package %1.").arg(plan.version)
        : QCoreApplication::translate("HarkenHub", "The Unreal Integration Package source or installed plugin is not safe to use.");
    sourceStep.cause = localizeIntegrationText(plan.error);
    sourceStep.remediation = plan.valid
        ? QString()
        : QCoreApplication::translate("HarkenHub", "Select a complete Harken Unreal Integration Package source or repair the existing managed plugin, then check again.");
    sourceStep.artifacts = {integrationPath};
    steps.push_back(sourceStep);

    ResultStep compareStep;
    compareStep.id = QStringLiteral("compare_sdk");
    compareStep.title = QCoreApplication::translate("HarkenHub", "Compare installed Integration Package");
    compareStep.hasFileComparison = plan.valid;
    compareStep.modifiedFiles = plan.modifiedManagedFiles;
    compareStep.missingFiles = plan.missingManagedFiles;
    for (const auto& path : plan.missingManagedFiles) compareStep.modifiedFiles.removeAll(path);
    for (const auto& path : plan.unreadableManagedFiles) compareStep.modifiedFiles.removeAll(path);
    compareStep.attentionRequired = !compareStep.modifiedFiles.isEmpty();
    if (plan.valid) {
        compareStep.fileChanges = plan.changes;
        for (const QString& path : plan.preservedTargetFiles)
            compareStep.fileChanges.push_back({path, QStringLiteral("preserve")});
    }
    if (!plan.valid) {
        compareStep.status = ResultStatus::Skipped;
        compareStep.summary = QCoreApplication::translate("HarkenHub", "Comparison was skipped because Integration Package validation failed.");
    } else if (plan.changes.empty()) {
        compareStep.status = ResultStatus::Succeeded;
        compareStep.summary = QCoreApplication::translate("HarkenHub", "The installed Unreal Integration Package matches the selected source.");
    } else {
        compareStep.status = checkOnly || !compareStep.modifiedFiles.isEmpty()
            ? ResultStatus::Warning : ResultStatus::Succeeded;
        compareStep.summary = compareStep.modifiedFiles.isEmpty()
            ? QCoreApplication::translate("HarkenHub", "%1 Integration Package file change(s) were found.")
                  .arg(plan.changes.size())
            : QCoreApplication::translate("HarkenHub", "%1 Integration Package file change(s) were found, including %2 locally modified managed file(s) that require confirmation before overwrite.")
                  .arg(plan.changes.size())
                  .arg(compareStep.modifiedFiles.size());
        if (!compareStep.modifiedFiles.isEmpty()) {
            compareStep.details.push_back(
                QCoreApplication::translate("HarkenHub", "Locally modified managed files:\n%1")
                    .arg(compareStep.modifiedFiles.join(QLatin1Char('\n'))));
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
    actionStep.title = QCoreApplication::translate("HarkenHub", "Apply Integration Package installation");
    if (!plan.valid) {
        actionStep.status = ResultStatus::Skipped;
        actionStep.summary = QCoreApplication::translate("HarkenHub", "Integration was not run because validation failed.");
        steps.push_back(actionStep);
        showResults(steps, QCoreApplication::translate("HarkenHub", "Integration Package check failed. Select the failed step for details."));
        return false;
    }
    if (checkOnly) {
        actionStep.status = plan.changes.empty() ? ResultStatus::NotRun : ResultStatus::AwaitingConfirmation;
        actionStep.summary = QCoreApplication::translate("HarkenHub", "Check-only mode requested; no Integration Package files were changed.");
        steps.push_back(actionStep);
        for (auto& step : steps)
            if (step.status == ResultStatus::Succeeded) step.status = ResultStatus::CheckPassed;
        showResults(steps, plan.changes.empty()
            ? QCoreApplication::translate("HarkenHub", "Check completed. The installed Unreal Integration Package matches the selected source.")
            : QCoreApplication::translate("HarkenHub", "Check completed. Differences were found; no Integration Package files were changed."),
                    QStringLiteral("compare_sdk"));
        return true;
    }

    const IntegrationOperationResult result =
        *runIntegrationTask(this, [executionTarget, plan, operation]() {
            return ProjectIntegrationExecutor::executeUnreal(executionTarget, plan, operation, true);
        }, false, integrationSelection_ ? integrationProgress_ : nullptr);
    integrationReviewInvalidated_ = result.reviewRequired;
    integrationComparison_ = result.success ? IntegrationComparison::Current : IntegrationComparison::Blocked;
    actionStep.status = result.success ? ResultStatus::Succeeded
                                       : ResultStatus::Failed;
    actionStep.summary = result.success
        ? (plan.changes.empty()
               ? QCoreApplication::translate("HarkenHub", "The Unreal Integration Package is already current.")
               : QCoreApplication::translate("HarkenHub", "The Unreal Integration Package installation completed successfully."))
        : QCoreApplication::translate("HarkenHub", "The Unreal Integration Package installation failed.");
    actionStep.cause = localizeIntegrationText(result.error);
    actionStep.remediation = result.success
        ? QString()
        : QCoreApplication::translate("HarkenHub", "Close Unreal Editor, check project permissions and file locks, then try again.");
    if (!result.backupPath.isEmpty())
        actionStep.artifacts = {result.backupPath};
    steps.push_back(actionStep);
    showResults(steps, result.success
        ? QCoreApplication::translate("HarkenHub", "Integration Package installation completed successfully.")
        : QCoreApplication::translate("HarkenHub", "Integration Package installation failed. Select the failed step for details."),
                QStringLiteral("apply_sdk"), result.success);
    if (result.success && !targets_->updateIntegrationInstallPath(target.targetId, result.installedPath))
        setSummaryText(QCoreApplication::translate("HarkenHub", "The Integration Package location changed, but Hub could not save it. Reopen this integration to rediscover the location.\n%1")
            .arg(localizeIntegrationText(targets_->lastError())));
    return result.success;
}

void EngineIntegrationsPage::showResults(
    const QVector<ResultStep>& steps, const QString& summary,
    const QString& preferredStep, bool succeeded)
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
    setSummaryText(summary, succeeded);
    updateIntegrationCheckResults();
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

void EngineIntegrationsPage::updateIntegrationCheckResults()
{
    integrationDiffTree_->clear();
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
    integrationSelectionDetails_->setHtml(log);
    integrationResultsTabs_->setTabText(1, integrationPhase_ == IntegrationPhase::Applying
        ? QCoreApplication::translate("HarkenHub", "Results and Logs")
        : warnings > 0 ? QCoreApplication::translate("HarkenHub", "Check Log (review recommended: %1)").arg(warnings)
                       : QCoreApplication::translate("HarkenHub", "Check Log"));
    integrationResultsTabs_->setCurrentIndex(failure || !comparison ? 1 : 0);
    if (failure)
        integrationSelectionSummary_->setText(integrationPhase_ == IntegrationPhase::Applying
            ? QCoreApplication::translate("HarkenHub", "Integration Package installation failed. Review the log for details.")
            : QCoreApplication::translate("HarkenHub", "Integration Package check failed. Review the check log for details."));
    if (!comparison) {
        integrationDiffSummary_->clear();
        return;
    }

    QStringList summary;
    const bool applied = integrationPhase_ == IntegrationPhase::Applying && integrationComparison_ == IntegrationComparison::Current;
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
        auto* section = new QTreeWidgetItem(integrationDiffTree_, {title});
        section->setForeground(0, QColor(QStringLiteral("#F0C36A")));
        for (const auto& path : affected) new QTreeWidgetItem(section, {path});
        section->setExpanded(true);
    };
    addAttention(comparison->modifiedFiles, applied
        ? QT_TRANSLATE_NOOP("HarkenHub", "Locally modified files overwritten or removed: %1")
        : QT_TRANSLATE_NOOP("HarkenHub", "Locally modified files to overwrite or remove: %1"), true);
    addAttention(comparison->missingFiles, applied
        ? QT_TRANSLATE_NOOP("HarkenHub", "Integration Package files missing before the change: %1")
        : QT_TRANSLATE_NOOP("HarkenHub", "Missing installed Integration Package files: %1"), false);
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
        auto* section = new QTreeWidgetItem(integrationDiffTree_,
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
            QCoreApplication::translate("HarkenHub", "No Integration Package files need to be changed.").toHtmlEscaped());
    if (failure)
        summary.prepend(QStringLiteral("<span style='color:#FF8B8B'><b>%1</b></span>")
            .arg(failure->summary.toHtmlEscaped()));
    if (warnings > 0)
        summary << QStringLiteral("<a href='log' style='color:#F0C36A'>%1</a>").arg(
            QCoreApplication::translate("HarkenHub", "Review recommended for %1 item(s). View check log.")
                .arg(warnings).toHtmlEscaped());
    integrationDiffSummary_->setText(summary.join(QStringLiteral("<br>")));
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
            QCoreApplication::translate("HarkenHub", "Files that could not be opened for writing:\n%1").section(QLatin1Char('\n'), 0, 0),
            QCoreApplication::translate("HarkenHub", "Directories without required create or rename access:\n%1").section(QLatin1Char('\n'), 0, 0)};
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
            return QCoreApplication::translate("HarkenHub", "Review recommended");
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
    MessageDialog::information(this, QCoreApplication::translate("HarkenHub", "Integration Already Exists"),
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
    const QString& integrationSourcePath)
{
    if (!editingIntegrationId_.isEmpty()) {
        if (integrations_->updateIntegrationSourcePath(editingIntegrationId_,
                                               integrationSourcePath)) {
            return true;
        }
        MessageDialog::warning(
            this, QCoreApplication::translate("HarkenHub", "Manage Integration"),
            QCoreApplication::translate("HarkenHub", "The Integration Package was integrated, but the Runtime / Integration Package path could not be saved.\n\n%1")
                .arg(localizeIntegrationText(integrations_->lastError())));
        return false;
    }
    IntegrationRecord integration;
    integration.integrationId =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    integration.harkenProjectPath = harkenProjectPath;
    integration.engineTargetId = engineTargetId;
    integration.integrationSourcePath = integrationSourcePath;
    integration.createdAt =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    integration.updatedAt = integration.createdAt;
    if (!integrations_->add(integration, false)) {
        MessageDialog::warning(
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
