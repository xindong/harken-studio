
#include "PackManagerPanel.h"
#include "PackStructureDialog.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "../../Core/ActionManager.h"
#include "../../Core/AppSettings.h"
#include "../../Core/AssetPipeline/PathUtils.h"
#include "../../Core/BaseDialog.h"
#include "../../Core/Components/VAction.h"
#include "../../Core/PackBuildPlan.h"
#include "../../Core/PackArtifactCache.h"
#include "../../Core/ProjectLanguageSemantics.h"
#include "../../Core/PlatformContextService.h"
#include "../../Core/ProjectPlatformActivity.h"
#include "../../Core/ProjectPackExporter.h"
#include "../../Core/ProjectManager.h"
#include "../../Core/ProjectTypes.h"
#include "../../Core/ThemeManager.h"
#include "../../Core/ToastManager.h"
#include "../Common/LayoutDebugTint.h"
#include "../Common/DialogFormLayout.h"
#include "../Common/ProportionalTreeWidget.h"
#include "../Common/TableItemDelegate.h"
#include "../Common/TableWidget.h"
#include "../Common/SearchField.h"
#include "../Common/MultiSelectComboBox.h"
#include "../Common/SplitButton.h"

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
#include <QJsonObject>
#include <QJsonArray>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QHash>
#include <QSet>
#include <QStyledItemDelegate>
#include <QMenu>
#include <QProcess>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QTimer>
#include <QShowEvent>
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
    kColCost = 5,
    kColFile     = 6,
    kColVersion  = 7,
    kPackTreeColumnCount = 8
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

    connect(&PackGenerationRecordStore::instance(), &PackGenerationRecordStore::changed,
            this, &PackManagerPanel::requestPackTableRefresh);
    connect(&PackArtifactCache::instance(), &PackArtifactCache::changed,
            this, &PackManagerPanel::requestPackTableRefresh);
    auto& pm = ProjectManager::instance();
    connect(&ProjectPlatformActivity::instance(), &ProjectPlatformActivity::busyChanged,
            this, [this] { updateExportButton(); });
    connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged,
            this, [this](const PlatformContext&) { requestPackTableRefresh(); });
    connect(&pm, &ProjectManager::actionPackTargetingChanged, this,
            [this]() { requestPackTableRefresh(); });
    connect(&pm, &ProjectManager::projectLanguagesChanged, this,
            [this]() { requestPackTableRefresh(); });
    connect(&pm, &ProjectManager::itemPropertyChanged, this,
            [this](ProjectItem* item, const QString& property) {
        static const QSet<QString> closureProperties = {
            QStringLiteral("distinguishLanguage"), QStringLiteral("selections"),
            QStringLiteral("transitions"),
            QStringLiteral("clips"), QStringLiteral("mode"), QStringLiteral("playType"),
            QStringLiteral("behavior")};
        if (item && (item->type == ProjectItem::Type::Action
                     || closureProperties.contains(property)))
            requestPackTableRefresh();
    });

    connect(&pm, &ProjectManager::itemCreated, this,
            [this]() { requestPackTableRefresh(); });
    connect(&pm, &ProjectManager::itemDeleted, this,
            [this]() { requestPackTableRefresh(); }, Qt::QueuedConnection);
    connect(&pm, &ProjectManager::itemMoved, this,
            [this]() { requestPackTableRefresh(); });
    connect(&pm, &ProjectManager::itemRenamed, this,
            [this]() { requestPackTableRefresh(); });

    connect(&pm, &ProjectManager::packBuildOutputDirChanged, this,
            [this](const QString&) { requestPackTableRefresh(); populateSettings(); });

    connect(&pm, &ProjectManager::packExportSelectedPlatformsChanged, this,
            [this](const QStringList&) { populatePlatformControls(); });
    connect(&pm, &ProjectManager::targetPlatformsChanged, this,
            [this](const QStringList&) {
        populatePlatformControls();
        requestPackTableRefresh();
    });
    connect(&pm, &ProjectManager::projectPlatformEdited, this, [this](const QString&) {
        requestPackTableRefresh();
    });

    connect(&pm, &ProjectManager::projectOpened, this, [this](ProjectData*) {
        static_cast<TableWidget*>(packTree_->parentWidget())->clearSelection();
        populatePlatformControls();
        requestPackTableRefresh();
        populateSettings();
    });
    connect(&pm, &ProjectManager::projectClosed, this, [this]() {
        static_cast<TableWidget*>(packTree_->parentWidget())->clearSelection();
        searchEdit_->clear();
        populatePlatformControls();
        requestPackTableRefresh();
        populateSettings();
    });

    connect(&AppSettings::instance(), &AppSettings::developerModeChanged,
            this, [this](bool) {
        applyDeveloperColumns();
        requestPackTableRefresh();
    });

    populatePlatformControls();
    requestPackTableRefresh();
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
            return new PackManagerTree({280, 70, 90, 150, 80, 80, 120, 60}, parent, themeManager);
        });
    packTree_->setHeaderLabels(
        {tr("Name"), tr("Built"), tr("Size"), tr("Modified"),
         tr("Result"), tr("Cost"), tr("File"), tr("Version")});
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
    toolbar->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto* row = new QHBoxLayout(toolbar);
    row->setSizeConstraint(QLayout::SetNoConstraint);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);

    auto* platformLabel = new QLabel(tr("Export Platforms:"));
    platformLabel->setObjectName("packPlatformLabel");
    platformLabel->setProperty("textRole", QStringLiteral("muted"));
    row->addWidget(platformLabel);

    platformCombo_ = new MultiSelectComboBox();
    platformCombo_->setObjectName("packPlatformCombo");
    platformCombo_->setMinimumContentsLength(12);
    platformCombo_->setAccessibleName(tr("Export Platforms"));
    connect(platformCombo_, &MultiSelectComboBox::selectedDataChanged, this,
            [this](const QVariantList& values) {
        QStringList platforms;
        for (const auto& value : values) platforms.append(value.toString());
        commitSelectedPlatforms(platforms);
    });
    row->addWidget(platformCombo_);
    row->addStretch(1);

    forceReexportBox_ = new QCheckBox(tr("Force Re-export"));
    forceReexportBox_->setObjectName("packForceReexportCheck");
    forceReexportBox_->setToolTip(tr("Rebuild selected packs even when their content is unchanged."));
    row->addWidget(forceReexportBox_);

    selfCleaningBox_ = new QCheckBox(tr("Self-cleaning"));
    selfCleaningBox_->setObjectName("packSelfCleaningCheck");
    selfCleaningBox_->setChecked(true);
    selfCleaningBox_->setToolTip(tr("Remove everything in the export directory that is not part of this export."));
    row->addWidget(selfCleaningBox_);

    exportBtn_ = new SplitButton();
    auto* primaryButton = exportBtn_->primaryButton();
    primaryButton->setObjectName("projectPackGenerateAllBtn");
    primaryButton->setToolTip(
        tr("Export checked packs to the selected platforms. Reuse unchanged packs."));
    exportBtn_->dropdownButton()->setAccessibleName(tr("More export actions"));
    exportBtn_->dropdownButton()->setToolTip(tr("More export actions"));
    exportAllAction_ = exportBtn_->addAction(
        tr("Export All"), this, [this] {
            requestExport({}, true, forceReexportBox_->isChecked(), selfCleaningBox_->isChecked());
        });
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

void PackManagerPanel::requestPackTableRefresh()
{
    packTableDirty_ = true;
    if (!isVisible() || packTableRefreshQueued_) return;
    packTableRefreshQueued_ = true;
    QTimer::singleShot(0, this, [this] {
        packTableRefreshQueued_ = false;
        if (isVisible() && packTableDirty_) populateProjectPackTable();
    });
}

void PackManagerPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (packTableDirty_) populateProjectPackTable();
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
    if (!platformCombo_)
        return;

    auto* project = ProjectManager::instance().currentProject();
    const bool open = project != nullptr;
    const QStringList platforms = effectivePlatforms(project);
    const QStringList selected = generationPlatforms();

    const QSignalBlocker blocker(platformCombo_);
    QList<QPair<QString, QVariant>> options;
    for (const auto& id : platforms) options.append({platformName(id), id});
    platformCombo_->setOptions(options);
    platformCombo_->setEnabled(open);
    QVariantList selectedValues;
    for (const auto& id : selected) selectedValues.append(id);
    platformCombo_->setSelectedData(selectedValues);
    for (int i = 0; i < platforms.size(); ++i) {
        const QString platform = platforms.at(i);
        const auto* definition = ProjectPlatformSemantics::find(project->platforms, QUuid(platform));
        platformCombo_->setItemData(i, tr("Include %1 (%2) in export").arg(
            definition->name, ProjectPlatformSemantics::basePlatformName(definition->basePlatform)),
            Qt::ToolTipRole);
    }

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

QStringList PackManagerPanel::effectiveSelectedPlatforms(const ProjectData* project,
                                                         const QStringList& selectedPlatforms)
{
    const QStringList avail = effectivePlatforms(project);
    QStringList out;
    if (project) {
        for (const auto& p : selectedPlatforms)
            if (avail.contains(p) && !out.contains(p))
                out << p;
    }
    return out;
}

QStringList PackManagerPanel::generationPlatforms() const
{
    const auto& manager = ProjectManager::instance();
    return effectiveSelectedPlatforms(manager.currentProject(), manager.getPackExportSelectedPlatforms());
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

    packTableDirty_ = false;
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
    const auto languagesByAction = PackBuildPlanning::actionLanguages(*project);
    const auto selection = pm.packExportSelection();

    auto applyCached = [&](QTreeWidgetItem* item, const QString& language,
                           const QString& name, const QString& unitKey) {
        if (viewPlatforms.isEmpty()) return;
        const auto& platform = viewPlatforms.front();
        const auto* record = PackGenerationRecordStore::instance().find(platform, language, unitKey);
        if (record) {
            item->setText(kColResult, record->status.isEmpty()
                ? (record->success ? tr("Built") : tr("Failed")) : record->status);
            item->setData(kColResult, TableTextRole, static_cast<int>(record->success
                ? TableTextState::Success : TableTextState::Error));
            QStringList details;
            if (!record->errors.isEmpty()) details.append(record->errors);
            if (!record->warnings.isEmpty()) details.append(record->warnings);
            item->setToolTip(kColResult, details.join(QLatin1Char('\n')));
            if (record->elapsedMs >= 0)
                item->setText(kColCost, tr("%1 ms").arg(record->elapsedMs));
        }
        const QString outputDir = outputDirs.value(platform);
        if (outputDir.isEmpty()) return;
        const QString path = QDir(outputDir).filePath(
            PackBuildPlanning::packFileName(language, name, ext));
        const auto* artifact = PackArtifactCache::instance().find(platform, language, unitKey, path);
        if (!artifact) return;
        item->setText(kColBuilt, tr("Yes"));
        item->setText(kColSize, humanBytes(artifact->bytes));
        item->setText(kColDate, artifact->modified.toString("yyyy-MM-dd hh:mm:ss"));
        item->setText(kColFile, QDir(outputDir).relativeFilePath(artifact->outputPath));
        item->setText(kColVersion, QStringLiteral("V%1").arg(artifact->version));
        item->setToolTip(kColBuilt, tr("Recorded at the last successful export.\n%1")
            .arg(QDir::toNativeSeparators(artifact->outputPath)));
        item->setData(0, kPackPathRole, artifact->outputPath);
    };

    auto* init = new QTreeWidgetItem(packTree_);
    init->setText(kColName, tr("Init"));
    init->setData(0, kNodeKindRole, static_cast<int>(NodeKind::SystemPack));
    init->setFlags(Qt::ItemIsEnabled);
    init->setToolTip(kColName, tr("Buses, sends and attenuations."));
    applyCached(init, {}, QStringLiteral("Init"), QStringLiteral("Init"));

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
                return selection.includesActionPack(unit.stableId, platform, language);
            };
            if (matrixEditable) {
                const auto state = editCheckState(included);
                item->setCheckState(kColName, state);
                if (state == Qt::PartiallyChecked) item->setToolTip(kColName, partialTooltip(included));
            }
            const bool excluded = !viewPlatforms.isEmpty() && !included(viewPlatforms.front());
            QStringList nameDetails;
            if (!item->toolTip(kColName).isEmpty()) nameDetails.append(item->toolTip(kColName));
            const auto nameError = PackBuildPlanning::packNameError(unit.name);
            if (!nameError.isEmpty()) nameDetails.append(nameError);
            if (missingLanguages)
                nameDetails.append(tr("Configure a target language or turn off Language for the referenced Sounds."));
            item->setToolTip(kColName, nameDetails.join(QLatin1Char('\n')));
            if (!missingLanguages)
                applyCached(item, language, unit.name, QStringLiteral("a:") + objectIdString(unit.stableId));
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
        packTree_->setColumnsHidden({kColFile, kColVersion}, !AppSettings::instance().developerMode());
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
}

void PackManagerPanel::onExportClicked()
{
    requestExport({}, false, forceReexportBox_->isChecked(), selfCleaningBox_->isChecked());
}

void PackManagerPanel::requestExport(const QStringList& packPaths, bool includeUnchecked,
                                     bool forceReexport, bool selfCleaning)
{
    const auto* project = ProjectManager::instance().currentProject();
    if (!project || ProjectPlatformActivity::instance().isBusy())
        return;
    const QStringList platforms = includeUnchecked
        ? effectivePlatforms(project) : generationPlatforms();
    if (platforms.isEmpty()) {
        ToastManager::instance().showToast(
            tr("Select at least one platform to export"), 2000, ToastType::Info);
        return;
    }
    emit exportRequested(platforms, packPaths, includeUnchecked,
                         forceReexport, selfCleaning);
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
    const QStringList selectedPlatforms = generationPlatforms();
    const auto selection = ProjectManager::instance().packExportSelection();
    bool partial = selectedPlatforms.size() != platforms.size();
    if (project && !partial) {
        for (QTreeWidgetItemIterator it(packTree_); *it && !partial; ++it) {
            const auto* item = *it;
            if (nodeKind(item) != NodeKind::Action) continue;
            const auto id = item->data(0, kStableIdRole).value<harkenstudio::ObjectId>();
            const auto language = item->data(0, kLanguageRole).toString();
            partial = std::any_of(platforms.begin(), platforms.end(), [&](const auto& platform) {
                return !selection.includesActionPack(id, platform, language);
            });
        }
    }
    exportBtn_->primaryButton()->setText(partial ? tr("Export Checked") : tr("Export All"));
    const bool enabled = project && !platforms.isEmpty()
        && !ProjectPlatformActivity::instance().isBusy();
    exportBtn_->setEnabled(enabled);
    exportBtn_->primaryButton()->setEnabled(!selectedPlatforms.isEmpty());
    exportAllAction_->setVisible(partial);

    forceReexportBox_->setEnabled(enabled);
    selfCleaningBox_->setEnabled(enabled);
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
    const auto project = ProjectManager::instance().currentProjectLease();
    const auto& editPlatforms = PlatformContextService::instance().context().selected;
    if (!project || editPlatforms.isEmpty())
        return;

    const QStringList allPlatforms = effectivePlatforms(project.get());
    const auto selection = ProjectManager::instance().packExportSelection();
    QList<QPair<QString, QJsonObject>> commands;
    for (const auto* item : items) {
        if (nodeKind(item) != NodeKind::Action) continue;
        const auto stableId = item->data(0, kStableIdRole).value<harkenstudio::ObjectId>();
        const QString language = item->data(0, kLanguageRole).toString();
        QStringList current;
        for (const auto& platform : allPlatforms)
            if (selection.includesActionPack(stableId, platform, language)) current.append(platform);
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

    QTimer::singleShot(0, this, [this, commands, project] {
        if (ProjectManager::instance().currentProject() != project.get()) return;
        {
            const QScopedValueRollback guard(packTreeUpdating_, true);
            for (const auto& command : commands) {
                const auto result = ActionManager::instance().execute(command.first, command.second, ActionSource::GUI);
                if (!result.success) {
                    ToastManager::instance().showToast(result.errorMessage.isEmpty()
                        ? tr("Could not update pack platform targeting") : result.errorMessage,
                        3000, ToastType::Warning);
                    break;
                }
            }
        }
        populateProjectPackTable();
    });
}

void PackManagerPanel::clearGenerationResults()
{
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
        const QString label = units.size() == 1 ? tr("Export")
            : tr("Export Selected (%1)").arg(units.size());
        menu.addAction(label, this, [this, units]() { requestExport(units); });
        const QString forceLabel = units.size() == 1 ? tr("Force Re-export")
            : tr("Force Re-export Selected (%1)").arg(units.size());
        menu.addAction(forceLabel, this, [this, units]() { requestExport(units, false, true); });
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
