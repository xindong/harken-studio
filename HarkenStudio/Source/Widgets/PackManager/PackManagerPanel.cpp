
#include "PackManagerPanel.h"
#include "PackStructureDialog.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "../../Core/ActionManager.h"
#include "../../Core/AppSettings.h"
#include "../../Core/AssetPipeline/PathUtils.h"
#include "../../Core/BaseDialog.h"
#include "../../Core/Components/VAction.h"
#include "../../Core/PackBuildPlan.h"
#include "../../Core/ProjectLanguageSemantics.h"
#include "../../Core/PlatformContextService.h"
#include "../../Core/ProjectPackExporter.h"
#include "../../Core/ProjectManager.h"
#include "../../Core/ProjectTypes.h"
#include "../../Core/ThemeManager.h"
#include "../../Core/ToastManager.h"
#include "../../Core/UndoCommands.h"
#include "../Common/LayoutDebugTint.h"
#include "../Common/DialogFormLayout.h"
#include "../Common/ProportionalTreeWidget.h"
#include "../Common/TableItemDelegate.h"
#include "../Common/TableWidget.h"
#include "../Common/SearchField.h"
#include "../Common/SegmentedControl.h"
#include "../Common/SplitButton.h"
#include "../LogPanel/LogPanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QFileInfo>
#include <QDialogButtonBox>
#include <QDir>
#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QHash>
#include <QSet>
#include <QUndoStack>
#include <QStyledItemDelegate>
#include <QMenu>
#include <QProcess>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <algorithm>
#include <functional>

namespace VNX {

namespace {

enum PackTreeColumn {
    kColName     = 0,
    kColBuilt    = 1,
    kColSize     = 2,
    kColDate     = 3,
    kColResult   = 4,
    kColDuration = 5,
    kColFile     = 6,
    kColVersion  = 7,
    kColCrc      = 8,
    kPackTreeColumnCount = 9
};

enum class NodeKind {
    None,
    Section,
    Action,
    SystemPack
};

constexpr int kNodeKindRole  = Qt::UserRole + 1;
constexpr int kGroupNameRole = Qt::UserRole + 2;
constexpr int kStableIdRole  = Qt::UserRole + 3;
constexpr int kObjectIdRole  = Qt::UserRole + 4;
constexpr int kPackPathRole  = Qt::UserRole + 5;
constexpr int kLanguageRole = Qt::UserRole + 6;

QString resultKey(const QString& platform, const QString& language,
                  const QString& unitKey)
{
    return platform + QLatin1Char('|') + language + QLatin1Char('|') + unitKey;
}

QString unitKeyForResult(const QString& source, const QString& packName,
                         uint32_t objectID)
{
    if (source == QLatin1String("Action"))
        return QStringLiteral("a:") + QString::number(objectID);
    return packName;
}

NodeKind nodeKind(const QTreeWidgetItem* item)
{
    return item ? static_cast<NodeKind>(item->data(0, kNodeKindRole).toInt())
                : NodeKind::None;
}

QString treeItemStateKey(const QTreeWidgetItem* item)
{
    switch (nodeKind(item)) {
        case NodeKind::Section:
            return QStringLiteral("section:") + item->data(0, kGroupNameRole).toString();
        case NodeKind::Action:
            return QStringLiteral("s:")
                + objectIdString(item->data(0, kStableIdRole).value<harkenstudio::ObjectId>())
                + QLatin1Char('|') + item->data(0, kLanguageRole).toString();
        default:
            break;
    }
    return QString();
}

class PackTreeDelegate : public TableItemDelegate
{
public:
    using TableItemDelegate::TableItemDelegate;

    QRect checkBoxRect(const QStyleOptionViewItem& option, const QModelIndex& index) const
    {
        QStyleOptionViewItem styled = option;
        initStyleOption(&styled, index);
        return option.widget->style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator,
            &styled, option.widget);
    }

    bool editorEvent(QEvent* event, QAbstractItemModel* model,
                     const QStyleOptionViewItem& option,
                     const QModelIndex& index) override
    {
        const QVariant value = index.data(Qt::CheckStateRole);
        const Qt::ItemFlags flags = model->flags(index);
        const bool checkable = value.isValid()
            && flags.testFlag(Qt::ItemIsUserCheckable)
            && flags.testFlag(Qt::ItemIsEnabled);
        if (!checkable)
            return QStyledItemDelegate::editorEvent(event, model, option, index);

        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
            return false;
        default:
            return QStyledItemDelegate::editorEvent(event, model, option, index);
        }
    }

};

class PackManagerTree : public ProportionalTreeWidget
{
public:
    using ProportionalTreeWidget::ProportionalTreeWidget;

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (beginCheckDrag(event))
            return;
        ProportionalTreeWidget::mousePressEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        if (beginCheckDrag(event))
            return;
        ProportionalTreeWidget::mouseDoubleClickEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (checkDragActive_) {
            sweepCheckDrag(event->pos().y());
            event->accept();
            return;
        }
        ProportionalTreeWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (checkDragActive_ && event->button() == Qt::LeftButton) {
            checkDragActive_ = false;
            event->accept();
            return;
        }
        ProportionalTreeWidget::mouseReleaseEvent(event);
    }

private:
    QTreeWidgetItem* checkableItemAt(const QPoint& pos) const
    {
        QTreeWidgetItem* item = itemAt(pos);
        if (!item || item->data(0, TableGroupRole).toBool()
            || !item->data(kColName, Qt::CheckStateRole).isValid())
            return nullptr;
        const Qt::ItemFlags flags = item->flags();
        return flags.testFlag(Qt::ItemIsUserCheckable)
                && flags.testFlag(Qt::ItemIsEnabled) ? item : nullptr;
    }

    bool beginCheckDrag(QMouseEvent* event)
    {
        if (event->button() != Qt::LeftButton)
            return false;
        const QPoint pos = event->pos();
        QTreeWidgetItem* item = checkableItemAt(pos);
        if (!item)
            return false;
        const auto index = indexFromItem(item, kColName);
        QStyleOptionViewItem option;
        initViewItemOption(&option);
        option.rect = visualRect(index);
        const auto* delegate = static_cast<const PackTreeDelegate*>(itemDelegateForIndex(index));
        if (!delegate->checkBoxRect(option, index).contains(pos))
            return false;
        checkDragActive_ = true;
        checkDragState_ = item->checkState(kColName) == Qt::Checked
            ? Qt::Unchecked : Qt::Checked;
        checkDragY_ = pos.y();
        item->setCheckState(kColName, checkDragState_);
        event->accept();
        return true;
    }

    void sweepCheckDrag(int targetY)
    {
        const int x = header()->sectionViewportPosition(kColName)
            + header()->sectionSize(kColName) / 2;
        const int step = qMax(1, sizeHintForRow(0) / 2);
        int y = checkDragY_;
        for (;;) {
            if (QTreeWidgetItem* item = checkableItemAt(QPoint(x, y)))
                if (item->checkState(kColName) != checkDragState_)
                    item->setCheckState(kColName, checkDragState_);
            if (y == targetY)
                break;
            y = y < targetY ? qMin(y + step, targetY) : qMax(y - step, targetY);
        }
        checkDragY_ = targetY;
    }

    bool checkDragActive_ = false;
    Qt::CheckState checkDragState_ = Qt::Unchecked;
    int checkDragY_ = 0;
};

class PanelChildDialog : public BaseDialog
{
public:
    using BaseDialog::BaseDialog;
    using BaseDialog::setupStandardButtons;
    using BaseDialog::buttonBox;
};

}

PackManagerPanel::PackManagerPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("PackManagerPanel");
    setupUI();

    auto& pm = ProjectManager::instance();
    connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged,
            this, [this](const PlatformContext&) { populateProjectPackTable(); });
    connect(&pm, &ProjectManager::actionPackTargetingChanged, this,
            [this]() { populateProjectPackTable(); });
    connect(&pm, &ProjectManager::projectLanguagesChanged, this,
            [this]() { populateProjectPackTable(); });
    connect(&pm, &ProjectManager::itemPropertyChanged, this,
            [this](ProjectItem* item, const QString& property) {
        static const QSet<QString> closureProperties = {
            QStringLiteral("distinguishLanguage"), QStringLiteral("selections"),
            QStringLiteral("transitions"),
            QStringLiteral("clips"), QStringLiteral("mode"), QStringLiteral("playType"),
            QStringLiteral("behavior")};
        if (item && (item->type == ProjectItem::Type::Action
                     || closureProperties.contains(property)))
            populateProjectPackTable();
    });

    connect(&pm, &ProjectManager::itemCreated, this,
            [this]() { populateProjectPackTable(); });
    connect(&pm, &ProjectManager::itemDeleted, this,
            [this]() { populateProjectPackTable(); }, Qt::QueuedConnection);
    connect(&pm, &ProjectManager::itemMoved, this,
            [this]() { populateProjectPackTable(); });
    connect(&pm, &ProjectManager::itemRenamed, this,
            [this]() { populateProjectPackTable(); });

    connect(&pm, &ProjectManager::packBuildOutputDirChanged, this,
            [this](const QString&) { refreshPackList(); });

    connect(&pm, &ProjectManager::packExportSelectedPlatformsChanged, this,
            [this](const QStringList&) { populatePlatformControls(); });
    connect(&pm, &ProjectManager::targetPlatformsChanged, this,
            [this](const QStringList&) {
        populatePlatformControls();
        populateProjectPackTable();
    });
    connect(&pm, &ProjectManager::projectPlatformEdited, this, [this](const QString& platformId) {
        for (auto& record : lastResults_) {
            if (record.platform == platformId) record.status = tr("Needs rebuild");
        }
        populateProjectPackTable();
    });

    connect(&pm, &ProjectManager::projectOpened, this, [this](ProjectData*) {
        static_cast<TableWidget*>(packTree_->parentWidget())->clearSelection();
        populatePlatformControls();
        refreshPackList();
    });
    connect(&pm, &ProjectManager::projectClosed, this, [this]() {
        static_cast<TableWidget*>(packTree_->parentWidget())->clearSelection();
        searchEdit_->clear();
        lastResults_.clear();
        populatePlatformControls();
        refreshPackList();
    });

    connect(&AppSettings::instance(), &AppSettings::developerModeChanged,
            this, [this](bool) {
        applyDeveloperColumns();
        populateProjectPackTable();
    });

    populatePlatformControls();
    populateProjectPackTable();
}

PackManagerPanel::~PackManagerPanel() = default;

void PackManagerPanel::setupUI()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    auto* content = new QWidget();
    DebugTint::tintContainer(content);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(8, 8, 8, 8);
    contentLayout->setSpacing(6);

    objectSendWarningLabel_ = new QLabel();
    objectSendWarningLabel_->setObjectName("objectSendWarningLabel");
    objectSendWarningLabel_->setProperty("textRole", QStringLiteral("warning"));
    objectSendWarningLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    objectSendWarningLabel_->setVisible(false);
    contentLayout->addWidget(objectSendWarningLabel_);

    searchEdit_ = new SearchField();
    searchEdit_->setObjectName("packSearchEdit");
    connect(searchEdit_, &QLineEdit::textChanged,
            this, &PackManagerPanel::applyNameFilter);
    contentLayout->addWidget(searchEdit_);

    auto* table = new TableWidget(TableOptions::display(), content);
    packTree_ = table->createCustomView(QStringLiteral("packTree"),
        [](QWidget* parent, ThemeManager* themeManager) {
            return new PackManagerTree({280, 70, 90, 150, 80, 80, 120, 60, 80}, parent, themeManager);
        });
    packTree_->setHeaderLabels(
        {tr("Name"), tr("Built"), tr("Size"), tr("Modified"),
         tr("Result"), tr("Duration"), tr("File"), tr("Version"), tr("CRC32")});
    packTree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    packTree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    packTree_->setItemDelegate(new PackTreeDelegate(packTree_));
    packTree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(packTree_, &QTreeWidget::customContextMenuRequested,
            this, &PackManagerPanel::onPackTreeContextMenu);
    connect(packTree_, &QTreeWidget::itemChanged,
            this, &PackManagerPanel::onPackItemChanged);
    connect(packTree_, &ProportionalTreeWidget::groupCheckStateRequested, this,
        [this](QTreeWidgetItem* group, Qt::CheckState state) {
            QList<QTreeWidgetItem*> items;
            for (int i = 0; i < group->childCount(); ++i) {
                auto* item = group->child(i);
                if (item->flags().testFlag(Qt::ItemIsUserCheckable)) items.append(item);
            }
            toggleUnitPlatforms(items, state == Qt::Checked);
        });
    contentLayout->addWidget(table, 1);
    contentLayout->addWidget(createToolbar());

    applyDeveloperColumns();

    mainLayout->addWidget(content, 1);

    createSettingsDialog();

    populateSettings();
    updateExportButton();
}

QWidget* PackManagerPanel::createToolbar()
{
    auto* toolbar = new QWidget();
    toolbar->setObjectName("packToolbar");
    auto* row = new QHBoxLayout(toolbar);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);

    auto* platformLabel = new QLabel(tr("Export Platforms:"));
    platformLabel->setObjectName("packPlatformLabel");
    platformLabel->setProperty("textRole", QStringLiteral("muted"));
    row->addWidget(platformLabel);

    platformChipsRow_ = new SegmentedControl();
    platformChipsRow_->setObjectName("packPlatformChips");
    platformChipsRow_->setAdaptiveSizing(true);
    row->addWidget(platformChipsRow_);
    row->addStretch(1);

    exportBtn_ = new SplitButton();
    auto* primaryButton = exportBtn_->primaryButton();
    primaryButton->setObjectName("projectPackGenerateAllBtn");
    primaryButton->setToolTip(
        tr("Export checked packs to the selected platforms. "
           "Init is always rebuilt."));
    exportBtn_->dropdownButton()->setAccessibleName(tr("More export actions"));
    exportBtn_->dropdownButton()->setToolTip(tr("More export actions"));
    exportAllAction_ = exportBtn_->addAction(
        tr("Export All"), this, [this] { requestExport({}, true); });
    connect(primaryButton, &QPushButton::clicked,
            this, &PackManagerPanel::onExportClicked);
    row->addWidget(exportBtn_);

    return toolbar;
}

void PackManagerPanel::createSettingsDialog()
{
    auto* dialog = new PanelChildDialog(this);
    dialog->setObjectName("packSettingsDialog");
    dialog->setWindowTitle(tr("Export Settings"));
    dialog->setMinimumWidth(460);
    settingsDialog_ = dialog;

    auto* dialogLayout = new QVBoxLayout(dialog);
    dialogLayout->setContentsMargins(16, 16, 16, 16);
    dialogLayout->setSpacing(12);

    auto* form = new DialogFormLayout();

    buildOutputDirEdit_ = new QLineEdit();
    buildOutputDirEdit_->setObjectName("packSettingsBuildOutputDirEdit");
    buildOutputDirEdit_->setReadOnly(true);
    buildOutputDirEdit_->setToolTip(tr("Change this directory in Project Settings > Build."));
    form->addRow(tr("Pack build output:"), buildOutputDirEdit_);

    conversionFailurePolicyCombo_ = new QComboBox();
    conversionFailurePolicyCombo_->setObjectName("packSettingsConversionFailureCombo");
    conversionFailurePolicyCombo_->addItem(
        tr("Abort build"), static_cast<int>(ConversionFailurePolicy::AbortBuild));
    conversionFailurePolicyCombo_->addItem(
        tr("Skip and warn"), static_cast<int>(ConversionFailurePolicy::SkipAndWarn));
    connect(conversionFailurePolicyCombo_,
            QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (settingsUpdating_)
            return;
        auto& pm = ProjectManager::instance();
        if (!pm.isProjectOpen())
            return;
        pm.setConversionFailurePolicy(static_cast<ConversionFailurePolicy>(
            conversionFailurePolicyCombo_->currentData().toInt()));
    });
    form->addRow(tr("On conversion failure:"), conversionFailurePolicyCombo_);

    dialogLayout->addLayout(form);
    dialogLayout->addStretch(1);

    dialog->setupStandardButtons(QDialogButtonBox::Close);
    dialogLayout->addWidget(dialog->buttonBox());
}

void PackManagerPanel::refreshPackList()
{
    populateProjectPackTable();
    populateSettings();
}

void PackManagerPanel::populateSettings()
{
    auto& pm = ProjectManager::instance();
    const bool open = pm.isProjectOpen();

    settingsUpdating_ = true;
    if (buildOutputDirEdit_) {
        buildOutputDirEdit_->setEnabled(open);
        buildOutputDirEdit_->setText(open
            ? PathUtils::packBuildOutputRoot(pm.currentProject()->projectPath,
                                             pm.packBuildOutputDir()) : QString());
    }
    if (conversionFailurePolicyCombo_) {
        conversionFailurePolicyCombo_->setEnabled(open);
        conversionFailurePolicyCombo_->setCurrentIndex(
            open && pm.getConversionFailurePolicy() == ConversionFailurePolicy::SkipAndWarn
                ? 1 : 0);
    }
    settingsUpdating_ = false;
}

void PackManagerPanel::populatePlatformControls()
{
    if (!platformChipsRow_)
        return;

    auto* project = ProjectManager::instance().currentProject();
    const bool open = project != nullptr;
    const QStringList platforms = effectivePlatforms(project);
    const QStringList selected = effectiveSelectedPlatforms(project);

    platformUpdating_ = true;

    QList<QPair<QString, QVariant>> options;
    for (const auto& id : platforms) options.append({platformName(id), id});
    platformChipsRow_->setOptions(options, false);
    platformChipsRow_->setEnabled(open);
    for (int i = 0; i < platforms.size(); ++i) {
        const QString platform = platforms.at(i);
        auto* chip = platformChipsRow_->button(i);
        chip->setObjectName(QStringLiteral("packExportPlatform_") + platform);
        chip->setChecked(selected.contains(platform));
        chip->setFocusPolicy(Qt::NoFocus);
        const auto* definition = ProjectPlatformSemantics::find(project->platforms, QUuid(platform));
        chip->setToolTip(tr("Include %1 (%2) in export").arg(
            definition->name, ProjectPlatformSemantics::basePlatformName(definition->basePlatform)));
        connect(chip, &QToolButton::toggled, this, [this](bool) {
            if (platformUpdating_) return;
            QStringList ordered;
            for (int i = 0; auto* button = platformChipsRow_->button(i); ++i)
                if (button->isChecked()) ordered.append(button->property("segmentValue").toString());
            const auto project = ProjectManager::instance().currentProjectLease();
            QTimer::singleShot(0, this, [this, ordered, project]() {
                if (ProjectManager::instance().currentProject() == project.get())
                    commitSelectedPlatforms(ordered);
            });
        });
    }

    platformUpdating_ = false;
    updateExportButton();
}

void PackManagerPanel::applyNameFilter()
{
    if (!packTree_)
        return;
    const QString needle = searchEdit_ ? searchEdit_->text().trimmed() : QString();
    auto matches = [&needle](const QTreeWidgetItem* item) {
        return needle.isEmpty()
            || item->text(kColName).contains(needle, Qt::CaseInsensitive);
    };

    std::function<bool(QTreeWidgetItem*, bool)> filterItem =
        [&](QTreeWidgetItem* item, bool inheritedMatch) {
        const bool selfMatch = inheritedMatch || matches(item);
        bool anyChildVisible = false;
        for (int c = 0; c < item->childCount(); ++c) {
            QTreeWidgetItem* child = item->child(c);
            const bool childVisible = filterItem(child, selfMatch);
            anyChildVisible = anyChildVisible || childVisible;
        }
        const bool visible = selfMatch || anyChildVisible;
        item->setHidden(!visible);
        return visible;
    };
    for (int i = 0; i < packTree_->topLevelItemCount(); ++i)
        filterItem(packTree_->topLevelItem(i), false);
}

void PackManagerPanel::commitSelectedPlatforms(const QStringList& platforms)
{
    if (platforms == ProjectManager::instance().getPackExportSelectedPlatforms()) {
        populatePlatformControls();
        return;
    }
    QJsonArray arr;
    for (const auto& p : platforms)
        arr.append(p);
    ActionManager::instance().execute(
        QStringLiteral("set_pack_export_selected_platforms"),
        QJsonObject{{QStringLiteral("platform_ids"), arr}},
        ActionSource::GUI);
}

QStringList PackManagerPanel::effectivePlatforms(const ProjectData* project)
{
    return project ? project->platformIds() : QStringList();
}

QStringList PackManagerPanel::effectiveSelectedPlatforms(const ProjectData* project)
{
    const QStringList avail = effectivePlatforms(project);
    QStringList out;
    if (project) {
        for (const auto& p : project->packExportSelectedPlatformIds)
            if (avail.contains(p) && !out.contains(p))
                out << p;
    }
    return out;
}

QStringList PackManagerPanel::generationPlatforms() const
{
    return effectiveSelectedPlatforms(ProjectManager::instance().currentProject());
}

QString PackManagerPanel::platformName(const QString& id)
{
    const auto* project = ProjectManager::instance().currentProject();
    const auto* platform = project ? ProjectPlatformSemantics::find(project->platforms, QUuid(id)) : nullptr;
    return platform ? platform->name : tr("Removed platform");
}

QString PackManagerPanel::platformOutputDirFor(const QString& platform) const
{
    auto* project = ProjectManager::instance().currentProject();
    if (!project || platform.isEmpty())
        return QString();
    const auto* definition = ProjectPlatformSemantics::find(project->platforms, QUuid::fromString(platform));
    if (!definition) return {};
    return ProjectPackExporter::platformOutputDir(
        PathUtils::packBuildOutputRoot(project->projectPath, project->packBuildOutputDir),
        *definition);
}

void PackManagerPanel::populateProjectPackTable()
{
    if (!packTree_ || packTreeUpdating_)
        return;

    QSet<QString> collapsedKeys;
    QSet<QString> selectedKeys;
    for (QTreeWidgetItemIterator it(packTree_); *it; ++it) {
        const QString key = treeItemStateKey(*it);
        if (key.isEmpty())
            continue;
        if (((*it)->childCount() > 0 || (*it)->data(0, TableGroupCollapsibleRole).toBool())
            && !(*it)->isExpanded())
            collapsedKeys.insert(key);
        if ((*it)->isSelected())
            selectedKeys.insert(key);
    }

    packTreeUpdating_ = true;
    packTree_->clear();

    auto& pm = ProjectManager::instance();
    auto* project = pm.currentProject();
    const bool open = project != nullptr;
    packTree_->setVisible(open);
    searchEdit_->setVisible(open);
    if (!project) {
        packTreeUpdating_ = false;
        if (objectSendWarningLabel_)
            objectSendWarningLabel_->setVisible(false);
        updateExportButton();
        return;
    }

    const QString ext = HarkenEditor::Bridge::packArtifactExtension();
    const auto& context = PlatformContextService::instance().context();
    const auto viewScope = currentWorldPlatformScope(context);
    const QStringList viewPlatforms = viewScope
        ? QStringList{ProjectPlatformSemantics::idString(*viewScope)} : QStringList{};
    const auto& editPlatforms = context.selected;
    QHash<QString, QString> outputDirs;
    for (const QString& platform : viewPlatforms)
        outputDirs.insert(platform, platformOutputDirFor(platform));

    const std::vector<ProjectPackExporter::ActionPackUnit> units =
        ProjectPackExporter::CollectActionPackUnits(*project);
    const auto buildPlan = PackBuildPlanning::build(*project, viewPlatforms, {}, true);
    QHash<harkenstudio::ObjectId, QStringList> languagesByAction;
    for (const auto& variant : buildPlan.variants)
        if (!languagesByAction[variant.actionStableId].contains(variant.variant.language))
            languagesByAction[variant.actionStableId].append(variant.variant.language);

    const bool devMode = AppSettings::instance().developerMode();
    QHash<QString, QHash<QString, QString>> publishedActionPaths;

    struct DiskCell {
        QString built;
        QString size;
        QString date;
        QString path;
        QString result;
        QString duration;
        QString resultTip;
        bool    failed = false;
        QString version;
        QString crc;
    };
    auto diskCell = [&](const QString& platform, const QString& language,
                        const QString& relativeBase, bool included,
                        const QString& unitKey,
                        const std::vector<harkenstudio::ObjectId>& actionStableIds) -> DiskCell {
        DiskCell cell;

        const auto it = lastResults_.constFind(
            resultKey(platform, language, unitKey));
        if (it != lastResults_.constEnd()) {
            cell.result = !it->status.isEmpty()
                ? it->status
                : (it->success ? tr("Built") : tr("Failed"));
            cell.failed = !it->success;
            if (it->elapsedMs >= 0)
                cell.duration = tr("%1 ms").arg(it->elapsedMs);
            QStringList tip;
            if (!it->errors.isEmpty())
                tip << it->errors;
            if (!it->warnings.isEmpty())
                tip << it->warnings;
            cell.resultTip = tip.join(QLatin1Char('\n'));
        }

        if (!included) {
            cell.built = tr("No");
            return cell;
        }
        const QString dir = outputDirs.value(platform);
        if (dir.isEmpty()) {
            return cell;
        }
        const QFileInfo fi(QDir(dir).filePath(relativeBase + ext));
        if (!fi.exists()) {
            cell.built = tr("No");
            return cell;
        }
        if (!actionStableIds.empty()) {
            const QString manifestPath = QDir(dir).filePath(PackBuildPlanning::packFileName(
                language, HarkenEditor::Bridge::packManifestFileName(), {}));
            auto paths = publishedActionPaths.constFind(manifestPath);
            if (paths == publishedActionPaths.constEnd()) {
                QFile manifestFile(manifestPath);
                QJsonObject manifest;
                if (manifestFile.open(QIODevice::ReadOnly))
                    manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
                QHash<QString, QString> packPaths;
                for (const auto& value : manifest.value(QStringLiteral("packs")).toArray()) {
                    const auto row = value.toObject();
                    packPaths.insert(row.value(QStringLiteral("packStableId")).toString(),
                                     row.value(QStringLiteral("path")).toString());
                }
                QHash<QString, QString> actionPaths;
                const auto catalog = manifest.value(QStringLiteral("gameplayCatalog")).toObject();
                for (const auto& value : catalog.value(QStringLiteral("actions")).toArray()) {
                    const auto row = value.toObject();
                    actionPaths.insert(row.value(QStringLiteral("actionStableId")).toString(),
                        packPaths.value(row.value(QStringLiteral("ownerPackStableId")).toString()));
                }
                publishedActionPaths.insert(manifestPath, std::move(actionPaths));
                paths = publishedActionPaths.constFind(manifestPath);
            }
            if (std::count(paths->cbegin(), paths->cend(), relativeBase + ext)
                    != static_cast<qsizetype>(actionStableIds.size())
                || std::any_of(actionStableIds.begin(), actionStableIds.end(),
                    [&](harkenstudio::ObjectId actionStableId) {
                        return paths->value(objectIdString(actionStableId)) != relativeBase + ext;
                    })) {
                cell.built = tr("No");
                cell.result = tr("Pack membership changed");
                cell.failed = true;
                cell.resultTip = tr("The published manifest does not match the Actions assigned to this pack.");
                return cell;
            }
        }
        cell.built = tr("Yes");
        cell.size = humanBytes(fi.size());
        cell.date = fi.lastModified().toString("yyyy-MM-dd hh:mm:ss");
        cell.path = fi.absoluteFilePath();
        if (devMode) {
            QFile file(cell.path);
            PackFileHeader hdr;
            if (file.open(QIODevice::ReadOnly)
                && PackStructureDialog::readPackHeader(file, hdr)) {
                cell.version = QStringLiteral("V%1").arg(hdr.version);
                cell.crc = QStringLiteral("0x%1")
                    .arg(hdr.crc32, 8, 16, QChar('0')).toUpper();
            } else {
                cell.version = QStringLiteral("?");
                cell.crc = QStringLiteral("?");
            }
        }
        return cell;
    };

    auto applyDisk = [&](QTreeWidgetItem* item, const QString& language,
                         const QString& name, const QString& unitKey, bool included,
                         const std::vector<harkenstudio::ObjectId>& actionIds = {}) {
        const QString nameError = PackBuildPlanning::packNameError(name);
        if (!actionIds.empty() && !nameError.isEmpty()) {
            item->setText(kColBuilt, tr("No"));
            item->setText(kColResult, tr("Invalid pack name"));
            item->setData(kColResult, TableTextRole, static_cast<int>(TableTextState::Error));
            item->setToolTip(kColResult, nameError);
            return;
        }
        if (viewPlatforms.isEmpty()) return;
        const QString relativeBase = PackBuildPlanning::packFileName(language, name, {});
        if (devMode) item->setText(kColFile, relativeBase + ext);
        const DiskCell cell = diskCell(viewPlatforms.front(), language, relativeBase, included, unitKey, actionIds);
        item->setText(kColBuilt, cell.built);
        item->setText(kColSize, cell.size);
        item->setText(kColDate, cell.date);
        item->setText(kColResult, cell.result);
        item->setText(kColDuration, cell.duration);
        item->setText(kColVersion, cell.version);
        item->setText(kColCrc, cell.crc);
        item->setData(kColResult, TableTextRole, static_cast<int>(cell.result.isEmpty()
            ? TableTextState::Normal : cell.failed ? TableTextState::Error : TableTextState::Success));
        item->setToolTip(kColResult, cell.resultTip);
        item->setToolTip(kColBuilt, cell.path.isEmpty() ? tr("Not built") : QDir::toNativeSeparators(cell.path));
        item->setData(0, kPackPathRole, cell.path);
    };

    auto* init = new QTreeWidgetItem(packTree_);
    init->setText(kColName, tr("Init"));
    init->setData(0, kNodeKindRole, static_cast<int>(NodeKind::SystemPack));
    init->setFlags(Qt::ItemIsEnabled);
    init->setToolTip(kColName, tr("Buses, sends and attenuations. Rebuilt on every export."));
    applyDisk(init, {}, QStringLiteral("Init"), QStringLiteral("Init"), true);

    QHash<QString, QTreeWidgetItem*> sections;
    QStringList sectionLanguages = ProjectLanguageSemantics::buildLanguages(*project);
    sectionLanguages.prepend(QString());
    for (const auto& language : sectionLanguages) {
        auto* section = packTree_->createGroup(language.isEmpty() ? tr("General") : language, {true, true});
        section->setData(0, kNodeKindRole, static_cast<int>(NodeKind::Section));
        section->setData(0, kGroupNameRole, language);
        sections.insert(language, section);
    }

    auto editCheckState = [&editPlatforms](
            const std::function<bool(const QString&)>& included) {
        int onCount = 0;
        for (const auto& platform : editPlatforms)
            if (included(ProjectPlatformSemantics::idString(platform)))
                ++onCount;
        return onCount == 0 ? Qt::Unchecked
            : onCount == editPlatforms.size() ? Qt::Checked
                                              : Qt::PartiallyChecked;
    };
    auto partialTooltip = [&editPlatforms](
            const std::function<bool(const QString&)>& included) {
        QStringList inc;
        QStringList exc;
        for (const auto& platform : editPlatforms) {
            const QString id = ProjectPlatformSemantics::idString(platform);
            (included(id) ? inc : exc) << platformName(id);
        }
        return tr("Included: %1\nExcluded: %2")
            .arg(inc.join(QStringLiteral(", ")), exc.join(QStringLiteral(", ")));
    };

    const bool matrixEditable = !editPlatforms.isEmpty();
    for (const auto& unit : units) {
        QStringList languages = languagesByAction.value(unit.stableId);
        const bool missingLanguages = languages.isEmpty();
        if (missingLanguages) languages.append(QString());
        for (const auto& language : languages) {
            auto* item = new QTreeWidgetItem(sections.value(language));
            item->setText(kColName, unit.name);
            item->setData(0, kNodeKindRole, static_cast<int>(NodeKind::Action));
            item->setData(0, kStableIdRole, QVariant::fromValue(unit.stableId));
            item->setData(0, kObjectIdRole, unit.objectID);
            item->setData(0, kLanguageRole, language);
            Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
            if (matrixEditable) flags |= Qt::ItemIsUserCheckable;
            item->setFlags(flags);
            const auto included = [&](const QString& platform) {
                return project->includesActionPack(unit.stableId, platform, language);
            };
            if (matrixEditable) {
                const auto state = editCheckState(included);
                item->setCheckState(kColName, state);
                if (state == Qt::PartiallyChecked) item->setToolTip(kColName, partialTooltip(included));
            }
            const bool excluded = !viewPlatforms.isEmpty() && !included(viewPlatforms.front());
            if (missingLanguages) {
                item->setText(kColBuilt, tr("No"));
                item->setText(kColResult, tr("No target languages"));
                item->setData(kColResult, TableTextRole, static_cast<int>(TableTextState::Error));
                item->setToolTip(kColResult, tr("Configure a target language or turn off Language for the referenced Sounds."));
            } else {
                applyDisk(item, language, unit.name, QStringLiteral("a:") + QString::number(unit.objectID),
                    !excluded, {unit.stableId});
            }
            if (excluded)
                for (int c = 0; c < kPackTreeColumnCount; ++c)
                    item->setData(c, TableTextRole, static_cast<int>(TableTextState::Disabled));
        }
    }

    for (auto* section : sections) {
        int checkableCount = 0;
        int checkedCount = 0;
        int uncheckedCount = 0;
        for (int i = 0; i < section->childCount(); ++i) {
            const auto* item = section->child(i);
            if (!item->flags().testFlag(Qt::ItemIsUserCheckable)) continue;
            ++checkableCount;
            checkedCount += item->checkState(kColName) == Qt::Checked;
            uncheckedCount += item->checkState(kColName) == Qt::Unchecked;
        }
        packTree_->setGroupCheckState(section,
            uncheckedCount == checkableCount ? Qt::Unchecked
                : checkedCount == checkableCount ? Qt::Checked : Qt::PartiallyChecked,
            checkableCount > 0);
    }

    for (QTreeWidgetItemIterator it(packTree_); *it; ++it) {
        const QString key = treeItemStateKey(*it);
        if (key.isEmpty())
            continue;
        if (collapsedKeys.contains(key))
            (*it)->setExpanded(false);
        if (selectedKeys.contains(key))
            (*it)->setSelected(true);
    }

    packTreeUpdating_ = false;

    applyNameFilter();

    updateExportButton();
}

void PackManagerPanel::applyDeveloperColumns()
{
    if (packTree_)
        packTree_->setColumnsHidden({kColFile, kColVersion, kColCrc}, !AppSettings::instance().developerMode());
}

QString PackManagerPanel::humanBytes(qint64 bytes)
{
    const bool neg = bytes < 0;
    const qint64 v = neg ? -bytes : bytes;
    QString out;
    if (v < 1024)
        out = tr("%1 B").arg(v);
    else if (v < 1024 * 1024)
        out = tr("%1 KB").arg(QString::number(v / 1024.0, 'f', 1));
    else if (v < 1024LL * 1024 * 1024)
        out = tr("%1 MB").arg(QString::number(v / (1024.0 * 1024.0), 'f', 1));
    else
        out = tr("%1 GB").arg(QString::number(v / (1024.0 * 1024.0 * 1024.0), 'f', 1));
    return neg ? (QLatin1Char('-') + out) : out;
}

void PackManagerPanel::addGenerationResult(const PackGenerationResult& result)
{
    PackGenerationRecordStore::instance().add(result);
    lastResults_.insert(
        resultKey(result.platform, result.language,
                  unitKeyForResult(result.source, result.packName, result.objectID)),
        result);

    QStringList details;
    if (!result.platform.isEmpty())
        details << (result.platformName.isEmpty() ? platformName(result.platform) : result.platformName);
    if (!result.language.isEmpty())
        details << result.language;
    if (result.bytes > 0)
        details << humanBytes(result.bytes);
    if (result.elapsedMs >= 0)
        details << tr("%1 ms").arg(result.elapsedMs);
    const QString suffix = details.isEmpty()
        ? QString()
        : QStringLiteral(" (%1)").arg(details.join(QStringLiteral(", ")));

    if (!result.success) {
        LogPanel::logError(LogCategory::Pack,
            tr("Export failed: %1%2 — %3").arg(result.packName, suffix,
                result.errors.isEmpty() ? tr("(no detail)") : result.errors));
    } else if (!result.warnings.isEmpty()) {
        LogPanel::logWarning(LogCategory::Pack,
            tr("Built %1%2 — %3").arg(result.packName, suffix, result.warnings));
    } else {
        LogPanel::logInfo(LogCategory::Pack,
            tr("Built %1%2").arg(result.packName, suffix));
    }
}

void PackManagerPanel::onExportClicked()
{
    requestExport({});
}

void PackManagerPanel::requestExport(const QStringList& packPaths, bool includeUnchecked)
{
    const auto* project = ProjectManager::instance().currentProject();
    if (!project)
        return;
    const QStringList platforms = includeUnchecked
        ? effectivePlatforms(project) : generationPlatforms();
    if (platforms.isEmpty()) {
        ToastManager::instance().showToast(
            tr("Select at least one platform to export"), 2000, ToastType::Info);
        return;
    }
    emit exportRequested(platforms, packPaths, includeUnchecked);
}

QStringList PackManagerPanel::selectedPackPaths() const
{
    QStringList out;
    if (!packTree_)
        return out;
    for (QTreeWidgetItem* sel : packTree_->selectedItems()) {
        if (nodeKind(sel) != NodeKind::Action) continue;
        const QString path = PackBuildPlanning::packFileName(
            sel->data(0, kLanguageRole).toString(), sel->text(kColName), {});
        if (!out.contains(path)) out.append(path);
    }
    return out;
}

void PackManagerPanel::updateExportButton()
{
    if (!exportBtn_)
        return;
    const auto* project = ProjectManager::instance().currentProject();
    const QStringList platforms = effectivePlatforms(project);
    const QStringList selectedPlatforms = effectiveSelectedPlatforms(project);
    bool partial = selectedPlatforms.size() != platforms.size();
    if (project && !partial) {
        for (QTreeWidgetItemIterator it(packTree_); *it && !partial; ++it) {
            const auto* item = *it;
            if (nodeKind(item) != NodeKind::Action) continue;
            const auto id = item->data(0, kStableIdRole).value<harkenstudio::ObjectId>();
            const auto language = item->data(0, kLanguageRole).toString();
            partial = std::any_of(platforms.begin(), platforms.end(), [&](const auto& platform) {
                return !project->includesActionPack(id, platform, language);
            });
        }
    }
    exportBtn_->primaryButton()->setText(partial ? tr("Export Checked") : tr("Export All"));
    exportBtn_->setEnabled(project && !platforms.isEmpty());
    exportBtn_->primaryButton()->setEnabled(!selectedPlatforms.isEmpty());
    exportAllAction_->setVisible(partial);
}

void PackManagerPanel::onPackItemChanged(QTreeWidgetItem* item, int column)
{
    if (packTreeUpdating_ || !item || column != kColName)
        return;

    if (nodeKind(item) != NodeKind::Action) return;
    const QVariant checkData = item->data(kColName, Qt::CheckStateRole);
    if (checkData.isValid())
        toggleUnitPlatforms({item}, checkData.toInt() == Qt::Checked);
}

void PackManagerPanel::toggleUnitPlatforms(const QList<QTreeWidgetItem*>& items, bool include)
{
    auto* project = ProjectManager::instance().currentProject();
    const auto& editPlatforms = PlatformContextService::instance().context().selected;
    if (!project || editPlatforms.isEmpty())
        return;

    const QStringList allPlatforms = effectivePlatforms(project);
    QList<QPair<QString, QJsonObject>> commands;
    for (const auto* item : items) {
        if (nodeKind(item) != NodeKind::Action) continue;
        const auto stableId = item->data(0, kStableIdRole).value<harkenstudio::ObjectId>();
        const QString language = item->data(0, kLanguageRole).toString();
        QStringList current;
        for (const auto& platform : allPlatforms)
            if (project->includesActionPack(stableId, platform, language)) current.append(platform);
        const QString actionId = QStringLiteral("set_action_pack_targeting");
        QJsonObject payload{{QStringLiteral("stableId"), objectIdString(stableId)},
                            {QStringLiteral("language"), language}};

        const QSet<QString> currentSet(current.cbegin(), current.cend());
        QSet<QString> next = currentSet;
        for (const auto& platform : editPlatforms) {
            const QString id = ProjectPlatformSemantics::idString(platform);
            if (include) next.insert(id);
            else next.remove(id);
        }
        if (next == currentSet) continue;
        QJsonArray platforms;
        for (const QString& platform : allPlatforms)
            if (next.contains(platform)) platforms.append(platform);
        const bool all = platforms.size() == allPlatforms.size();
        payload.insert(QStringLiteral("allPlatforms"), all);
        payload.insert(QStringLiteral("platform_ids"), all ? QJsonArray() : platforms);
        commands.append({actionId, payload});
    }
    if (commands.isEmpty()) return;

    QTimer::singleShot(0, this, [this, commands] {
        {
            const QScopedValueRollback guard(packTreeUpdating_, true);
            auto* stack = UndoRedoManager::instance().undoStack();
            const bool macro = stack && commands.size() > 1;
            if (macro) stack->beginMacro(tr("Set Pack Targeting"));
            for (const auto& command : commands) {
                const auto result = ActionManager::instance().execute(command.first, command.second, ActionSource::GUI);
                if (!result.success) {
                    ToastManager::instance().showToast(result.errorMessage.isEmpty()
                        ? tr("Could not update pack platform targeting") : result.errorMessage,
                        3000, ToastType::Warning);
                    break;
                }
            }
            if (macro) stack->endMacro();
        }
        populateProjectPackTable();
    });
}

void PackManagerPanel::clearGenerationResults()
{
    lastResults_.clear();
    PackGenerationRecordStore::instance().clear();
}

void PackManagerPanel::onOpenSettingsDialog()
{
    if (!settingsDialog_)
        return;
    populateSettings();
    settingsDialog_->show();
    settingsDialog_->raise();
    settingsDialog_->activateWindow();
}

void PackManagerPanel::showFileInExplorer(const QString& path)
{
    if (path.isEmpty())
        return;
    const QFileInfo info(path);
    if (!info.exists()) {
        ToastManager::instance().showToast(
            tr("File no longer exists: %1").arg(info.fileName()),
            3000, ToastType::Warning);
        return;
    }
#ifdef Q_OS_WIN
    QProcess process;
    process.setProgram(QStringLiteral("explorer.exe"));
    process.setNativeArguments(QStringLiteral("/select,\"%1\"")
        .arg(QDir::toNativeSeparators(info.absoluteFilePath())));
    process.startDetached();
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()));
#endif
}

void PackManagerPanel::onPackTreeContextMenu(const QPoint& pos)
{
    if (!packTree_)
        return;
    QTreeWidgetItem* item = packTree_->itemAt(pos);
    const NodeKind kind = nodeKind(item);

    QMenu menu(this);

    const QStringList units = kind == NodeKind::SystemPack
        ? QStringList() : selectedPackPaths();
    if (!units.isEmpty()) {
        const QString label = units.size() == 1
            ? tr("Export \"%1\"").arg(units.first())
            : tr("Export Selected (%1)").arg(units.size());
        menu.addAction(label, this, [this, units]() { requestExport(units); });
    }

    const uint32_t objectID = item ? item->data(0, kObjectIdRole).toUInt() : 0u;
    if (kind == NodeKind::Action && objectID != 0)
        menu.addAction(tr("Select in Explorer"), this,
            [this, objectID]() { emit revealActionRequested(objectID); });
    if (kind == NodeKind::SystemPack || kind == NodeKind::Action) {
        const QString packPath = item->data(0, kPackPathRole).toString();
        QAction* show = menu.addAction(tr("Show Pack in Explorer"), this,
            [this, packPath]() { showFileInExplorer(packPath); });
        show->setEnabled(!packPath.isEmpty());
        if (AppSettings::instance().developerMode()) {
            QAction* preview = menu.addAction(tr("Preview Pack Structure..."), this, [this, packPath]() {
                (new PackStructureDialog(packPath, this))->show();
            });
            preview->setEnabled(!packPath.isEmpty() && QFileInfo::exists(packPath));
        }
    }

    menu.addSeparator();
    menu.addAction(tr("Export Settings..."), this,
                   &PackManagerPanel::onOpenSettingsDialog);

    menu.exec(packTree_->viewport()->mapToGlobal(pos));
}

}
