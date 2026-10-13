
#include "PackManagerPanel.h"
#include "PackStructureDialog.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "../../Core/ActionManager.h"
#include "../../Core/AppSettings.h"
#include "../../Core/AssetPipeline/PathUtils.h"
#include "../../Core/BaseDialog.h"
#include "../../Core/Components/VAction.h"
#include "../../Core/PackBuildPlan.h"
#include "../../Core/ProjectPackExporter.h"
#include "../../Core/ProjectManager.h"
#include "../../Core/ProjectTypes.h"
#include "../../Core/ThemeManager.h"
#include "../../Core/ThemeAssetCatalog.h"
#include "../../Core/ToastManager.h"
#include "../../Core/UndoCommands.h"
#include "../Common/LayoutDebugTint.h"
#include "../Common/ProportionalTreeWidget.h"
#include "../Common/TableItemDelegate.h"
#include "../Common/TableWidget.h"
#include "../Common/SearchField.h"
#include "../Common/SegmentedControl.h"
#include "../Common/SwitchIndicator.h"
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
#include <QFormLayout>
#include <QStyledItemDelegate>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <algorithm>
#include <cstring>
#include <functional>

namespace VNX {

namespace {

const char kHarkenProjectItemMime[]  = "application/x-harken-project-item";
const char kHarkenProjectItemsMime[] = "application/x-harken-project-items";

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
    Group,
    Member,
    UnassignedAction,
    SystemPack
};

constexpr int kNodeKindRole  = Qt::UserRole + 1;
constexpr int kGroupNameRole = Qt::UserRole + 2;
constexpr int kStableIdRole  = Qt::UserRole + 3;
constexpr int kObjectIdRole  = Qt::UserRole + 4;
constexpr int kPackPathRole  = Qt::UserRole + 5;

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
    if (source == QLatin1String("Group"))
        return QStringLiteral("g:") + packName;
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
        case NodeKind::Group:
            return QStringLiteral("g:") + item->data(0, kGroupNameRole).toString();
        case NodeKind::Member:
        case NodeKind::UnassignedAction:
            return QStringLiteral("s:")
                + objectIdString(item->data(0, kStableIdRole).value<harkenstudio::ObjectId>());
        default:
            break;
    }
    return QString();
}

class PackTreeDelegate : public TableItemDelegate
{
public:
    using TableItemDelegate::TableItemDelegate;

    static constexpr int kSwitchWidth   = SwitchIndicator::Width;
    static constexpr int kSwitchHeight  = SwitchIndicator::Height;
    static constexpr int kSwitchTextGap = 6;

    static int switchLeft()
    {
        return ThemeManager::instance().metrics().table.cellPaddingX;
    }

    static int textIndent()
    {
        return switchLeft() + kSwitchWidth + kSwitchTextGap;
    }

    static QRect switchRect(const QRect& cell)
    {
        return QRect(cell.left() + switchLeft(),
                     cell.top() + (cell.height() - kSwitchHeight) / 2,
                     kSwitchWidth, kSwitchHeight);
    }

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                          const QModelIndex& index) const override
    {
        if (index.column() != kColName)
            return nullptr;
        QWidget* editor = TableItemDelegate::createEditor(parent, option, index);
        if (editor)
            editor->setObjectName(QStringLiteral("packManagerNameEditor"));
        return editor;
    }

    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                              const QModelIndex& index) const override
    {
        TableItemDelegate::updateEditorGeometry(editor, option, index);
        if (!index.data(Qt::CheckStateRole).isValid())
            return;
        QRect geom = editor->geometry();
        const int textLeft = switchRect(option.rect).right() + kSwitchTextGap;
        if (geom.left() < textLeft) {
            geom.setLeft(textLeft);
            editor->setGeometry(geom);
        }
    }

    void paintContents(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QWidget* widget = option.widget;
        QStyle* style = widget ? widget->style() : QApplication::style();

        if (!(opt.features & QStyleOptionViewItem::HasCheckIndicator)) {
            style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);
            drawExpandArrow(painter, option, index);
            return;
        }

        opt.features &= ~QStyleOptionViewItem::HasCheckIndicator;

        QStyleOptionViewItem bgOpt = opt;
        bgOpt.text.clear();
        style->drawControl(QStyle::CE_ItemViewItem, &bgOpt, painter, widget);

        const QRect sw = switchRect(opt.rect);
        SwitchIndicator::draw(painter, sw, opt.checkState,
                              opt.state.testFlag(QStyle::State_MouseOver));

        QStyleOptionViewItem textOpt = opt;
        textOpt.rect.setLeft(sw.right() + kSwitchTextGap);
        textOpt.state &= ~QStyle::State_HasFocus;
        style->drawControl(QStyle::CE_ItemViewItem, &textOpt, painter, widget);
        drawExpandArrow(painter, option, index);
    }

    bool editorEvent(QEvent* event, QAbstractItemModel* model,
                     const QStyleOptionViewItem& option,
                     const QModelIndex& index) override
    {
        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick: {
            const auto* me = static_cast<QMouseEvent*>(event);
            const QRect arrow = expandArrowRect(option, index);
            if (me->button() == Qt::LeftButton && arrow.isValid()
                && arrow.contains(me->pos())) {
                if (event->type() == QEvent::MouseButtonRelease) {
                    if (auto* view = qobject_cast<QTreeView*>(
                            const_cast<QWidget*>(option.widget)))
                        view->setExpanded(index, !view->isExpanded(index));
                }
                return true;
            }
            break;
        }
        default:
            break;
        }

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

private:
    static constexpr int kArrowTextGap = 4;

    QRect expandArrowRect(const QStyleOptionViewItem& option,
                          const QModelIndex& index) const
    {
        if (index.column() != kColName || !index.model()->hasChildren(index))
            return QRect();

        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        if (opt.features & QStyleOptionViewItem::HasCheckIndicator) {
            opt.features &= ~QStyleOptionViewItem::HasCheckIndicator;
            opt.rect.setLeft(switchRect(option.rect).right() + kSwitchTextGap);
        }
        const QWidget* widget = option.widget;
        QStyle* style = widget ? widget->style() : QApplication::style();
        const QRect textRect =
            style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget);
        const int textMargin =
            style->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, widget) + 1;
        const int chevronExtent = ThemeManager::instance().metrics().navigationChevronExtent;
        const int x = qMin(
            textRect.left() + textMargin
                + opt.fontMetrics.horizontalAdvance(opt.text) + kArrowTextGap,
            option.rect.right() - chevronExtent);
        return QRect(x,
                     option.rect.top() + (option.rect.height() - chevronExtent) / 2,
                     chevronExtent, chevronExtent);
    }

    void drawExpandArrow(QPainter* p, const QStyleOptionViewItem& option,
                         const QModelIndex& index) const
    {
        const QRect box = expandArrowRect(option, index);
        if (!box.isValid())
            return;
        const auto* view = qobject_cast<const QTreeView*>(option.widget);
        const bool open = view && view->isExpanded(index);
        const ThemeColors& c = ThemeManager::instance().colors();

        const QColor tint = option.state.testFlag(QStyle::State_MouseOver)
            ? c.textSecondary : c.textMuted;
        ThemeManager::instance().resolveIcon(
            QString::fromLatin1(open ? ThemeAssetIds::NavigationChevronDown
                                    : ThemeAssetIds::NavigationChevronRight),
            ThemeAssetVariant{QSize(), 1.0, 0, tint}).icon.paint(p, box);
    }

};

class PackManagerTree : public ProportionalTreeWidget
{
public:
    using ProportionalTreeWidget::ProportionalTreeWidget;

protected:
    QStringList mimeTypes() const override
    {
        return {QLatin1String(kHarkenProjectItemsMime)};
    }

    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override
    {
        QByteArray payload;
        for (const QTreeWidgetItem* it : items) {
            const uint32_t objectID = it ? it->data(0, kObjectIdRole).toUInt() : 0u;
            if (objectID != 0)
                payload.append(reinterpret_cast<const char*>(&objectID), sizeof(uint32_t));
        }
        if (payload.isEmpty())
            return nullptr;
        auto* mime = new QMimeData();
        mime->setData(QLatin1String(kHarkenProjectItemsMime), payload);
        return mime;
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (beginSwitchDrag(event))
            return;
        ProportionalTreeWidget::mousePressEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        if (beginSwitchDrag(event))
            return;
        ProportionalTreeWidget::mouseDoubleClickEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (switchDragActive_) {
            sweepSwitchDrag(event->pos().y());
            event->accept();
            return;
        }
        ProportionalTreeWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (switchDragActive_ && event->button() == Qt::LeftButton) {
            switchDragActive_ = false;
            event->accept();
            return;
        }
        ProportionalTreeWidget::mouseReleaseEvent(event);
    }

private:
    QTreeWidgetItem* switchableItemAt(const QPoint& pos) const
    {
        QTreeWidgetItem* item = itemAt(pos);
        if (!item || !item->data(kColName, Qt::CheckStateRole).isValid())
            return nullptr;
        const Qt::ItemFlags flags = item->flags();
        return flags.testFlag(Qt::ItemIsUserCheckable)
                && flags.testFlag(Qt::ItemIsEnabled) ? item : nullptr;
    }

    bool beginSwitchDrag(QMouseEvent* event)
    {
        if (event->button() != Qt::LeftButton)
            return false;
        const QPoint pos = event->pos();
        QTreeWidgetItem* item = switchableItemAt(pos);
        if (!item)
            return false;
        const QRect cell = visualRect(indexFromItem(item, kColName));
        if (!PackTreeDelegate::switchRect(cell).contains(pos))
            return false;
        switchDragActive_ = true;
        switchDragState_ = item->checkState(kColName) == Qt::Checked
            ? Qt::Unchecked : Qt::Checked;
        switchDragY_ = pos.y();
        item->setCheckState(kColName, switchDragState_);
        event->accept();
        return true;
    }

    void sweepSwitchDrag(int targetY)
    {
        const int x = header()->sectionViewportPosition(kColName)
            + PackTreeDelegate::switchLeft()
            + PackTreeDelegate::kSwitchWidth / 2;
        const int step = qMax(1, sizeHintForRow(0) / 2);
        int y = switchDragY_;
        for (;;) {
            if (QTreeWidgetItem* item = switchableItemAt(QPoint(x, y)))
                if (item->checkState(kColName) != switchDragState_)
                    item->setCheckState(kColName, switchDragState_);
            if (y == targetY)
                break;
            y = y < targetY ? qMin(y + step, targetY) : qMax(y - step, targetY);
        }
        switchDragY_ = targetY;
    }

    bool switchDragActive_ = false;
    Qt::CheckState switchDragState_ = Qt::Unchecked;
    int switchDragY_ = 0;
};

class PanelChildDialog : public BaseDialog
{
public:
    using BaseDialog::BaseDialog;
    using BaseDialog::setupStandardButtons;
    using BaseDialog::buttonBox;
};

QHash<uint32_t, harkenstudio::ObjectId> buildObjectIdToStableId(const ProjectData& project)
{
    QHash<uint32_t, harkenstudio::ObjectId> map;
    for (const auto& unit : ProjectPackExporter::CollectActionPackUnits(project))
        if (unit.objectID != 0)
            map.insert(unit.objectID, unit.stableId);
    return map;
}

QSet<uint32_t> actionObjectIds(const ProjectData& project)
{
    QSet<uint32_t> out;
    for (const auto& unit : ProjectPackExporter::CollectActionPackUnits(project))
        if (unit.objectID != 0)
            out.insert(unit.objectID);
    return out;
}

}

PackManagerPanel::PackManagerPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("PackManagerPanel");
    setAcceptDrops(true);
    setupUI();

    auto& pm = ProjectManager::instance();
    connect(&pm, &ProjectManager::packGroupsChanged, this,
            [this]() { populateProjectPackTable(); });
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

    connect(&pm, &ProjectManager::packExportCurrentPlatformChanged, this,
            [this](const QString&) { populatePlatformControls(); });
    connect(&pm, &ProjectManager::packExportSelectedPlatformsChanged, this,
            [this](const QStringList&) {
        populatePlatformControls();
        populateProjectPackTable();
    });
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
        viewPlatforms_.clear();
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

    mainLayout->addWidget(createToolbar());

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

    viewFilterBar_ = new QWidget();
    viewFilterBar_->setObjectName("packViewFilterBar");
    DebugTint::tintContainer(viewFilterBar_);
    auto* viewBarLayout = new QHBoxLayout(viewFilterBar_);
    viewBarLayout->setContentsMargins(0, 0, 0, 0);
    viewBarLayout->setSpacing(10);
    searchEdit_ = new SearchField();
    searchEdit_->setObjectName("packSearchEdit");
    searchEdit_->setMaximumWidth(240);
    connect(searchEdit_, &QLineEdit::textChanged,
            this, &PackManagerPanel::applyNameFilter);
    viewBarLayout->addWidget(searchEdit_, 1);
    viewBarLayout->addStretch(1);
    viewButtonsRow_ = new QWidget();
    viewButtonsRow_->setObjectName("packViewFilterButtons");
    viewButtonsRow_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    DebugTint::tintContainer(viewButtonsRow_);
    viewButtonsRow_->setMinimumWidth(1);
    auto* viewButtonsLayout = new QHBoxLayout(viewButtonsRow_);
    viewButtonsLayout->setContentsMargins(0, 0, 0, 0);
    viewButtonsLayout->setSpacing(0);
    viewBarLayout->addWidget(viewButtonsRow_);
    viewFilterBar_->setVisible(false);
    contentLayout->addWidget(viewFilterBar_);

    auto* table = new TableWidget(TableOptions::display(), content);
    packTree_ = table->createCustomView(QStringLiteral("packTree"),
        [](QWidget* parent, ThemeManager* themeManager) {
            return new PackManagerTree({280, 70, 90, 150, 80, 80, 120, 60, 80}, parent, themeManager);
        });
    packTree_->setHeaderLabels(
        {tr("Name"), tr("Built"), tr("Size"), tr("Modified"),
         tr("Result"), tr("Duration"), tr("File"), tr("Version"), tr("CRC32")});
    packTree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    packTree_->setEditTriggers(QAbstractItemView::DoubleClicked
                                   | QAbstractItemView::EditKeyPressed);
    packTree_->setItemDelegate(new PackTreeDelegate(packTree_));
    packTree_->setIndentation(PackTreeDelegate::textIndent());
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, packTree_, [this] {
        packTree_->setIndentation(PackTreeDelegate::textIndent());
    });
    packTree_->setDragEnabled(true);
    packTree_->setDragDropMode(QAbstractItemView::DragOnly);
    packTree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(packTree_, &QTreeWidget::customContextMenuRequested,
            this, &PackManagerPanel::onPackTreeContextMenu);
    connect(packTree_, &QTreeWidget::itemChanged,
            this, &PackManagerPanel::onPackItemChanged);
    contentLayout->addWidget(table, 1);

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
    toolbar->setProperty("surfaceRole", QStringLiteral("secondary"));
    auto* row = new QHBoxLayout(toolbar);
    row->setContentsMargins(4, 4, 4, 4);
    row->setSpacing(8);

    auto* platformLabel = new QLabel(tr("Platforms:"));
    platformLabel->setObjectName("packPlatformLabel");
    platformLabel->setProperty("textRole", QStringLiteral("muted"));
    row->addWidget(platformLabel);

    platformChipsRow_ = new SegmentedControl();
    platformChipsRow_->setObjectName("packPlatformChips");
    platformChipsRow_->setAdaptiveSizing(true);
    row->addWidget(platformChipsRow_);
    row->addStretch(1);

    exportBtn_ = new QPushButton(tr("Export All"));
    exportBtn_->setObjectName("projectPackGenerateAllBtn");
    exportBtn_->setProperty("buttonRole", QStringLiteral("accent"));
    exportBtn_->setProperty("squared", true);
    exportBtn_->setToolTip(
        tr("Export every pack targeted to the selected platform(s). "
           "Init is always rebuilt."));
    connect(exportBtn_, &QPushButton::clicked,
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

    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignLeft);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(12);

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
    populateViewFilter();
    updateExportButton();
}

void PackManagerPanel::populateViewFilter()
{
    if (!viewFilterBar_ || !viewButtonsRow_)
        return;

    const QStringList scope =
        effectiveSelectedPlatforms(ProjectManager::instance().currentProject());
    QStringList pruned;
    for (const QString& platform : scope)
        if (viewPlatforms_.contains(platform))
            pruned << platform;
    viewPlatforms_ = pruned.isEmpty() ? scope : pruned;

    viewUpdating_ = true;
    auto* buttonsLayout = static_cast<QHBoxLayout*>(viewButtonsRow_->layout());
    while (QLayoutItem* item = buttonsLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    if (!scope.isEmpty()) {
        QList<QPair<QString, QVariant>> options;
        for (const QString& platform : scope)
            options.append({platformName(platform), platform});
        auto* control = new SegmentedControl(viewButtonsRow_);
        control->setAdaptiveSizing(true);
        control->setOptions(options, false);
        buttonsLayout->addWidget(control);
        for (int i = 0; i < scope.size(); ++i) {
            const QString platform = scope.at(i);
            auto* button = control->button(i);
            button->setObjectName(QStringLiteral("packViewFilter_") + platform);
            button->setChecked(viewPlatforms_.contains(platform));
            button->setFocusPolicy(Qt::NoFocus);
            button->setToolTip(tr("Show %1 in table").arg(platformName(platform)));
            connect(button, &QToolButton::toggled, this,
                    [this, platform, button](bool on) {
                        if (viewUpdating_)
                            return;
                        if (!on && viewPlatforms_.size() <= 1) {
                            viewUpdating_ = true;
                            button->setChecked(true);
                            viewUpdating_ = false;
                            return;
                        }
                        const QStringList scopeNow = effectiveSelectedPlatforms(
                            ProjectManager::instance().currentProject());
                        QStringList next;
                        for (const QString& p : scopeNow)
                            if (p == platform ? on : viewPlatforms_.contains(p))
                                next << p;
                        if (next == viewPlatforms_)
                            return;
                        viewPlatforms_ = next;
                        populateProjectPackTable();
                    });
        }
    }
    viewUpdating_ = false;

    viewFilterBar_->setVisible(!scope.isEmpty());
    if (scope.isEmpty() && searchEdit_ && !searchEdit_->text().isEmpty())
        searchEdit_->clear();
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
    const bool filtering = !needle.isEmpty();
    QTreeWidgetItem* section = nullptr;
    bool sectionHasVisibleRows = false;
    for (int i = 0; i < packTree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = packTree_->topLevelItem(i);
        if (nodeKind(item) == NodeKind::Section) {
            if (section)
                section->setHidden(filtering && !sectionHasVisibleRows);
            section = item;
            sectionHasVisibleRows = false;
            continue;
        }
        sectionHasVisibleRows = filterItem(item, false) || sectionHasVisibleRows;
    }
    if (section)
        section->setHidden(filtering && !sectionHasVisibleRows);
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
    if (!packTree_)
        return;

    QSet<QString> collapsedKeys;
    QSet<QString> selectedKeys;
    for (QTreeWidgetItemIterator it(packTree_); *it; ++it) {
        const QString key = treeItemStateKey(*it);
        if (key.isEmpty())
            continue;
        if ((*it)->childCount() > 0 && !(*it)->isExpanded())
            collapsedKeys.insert(key);
        if ((*it)->isSelected())
            selectedKeys.insert(key);
    }

    packTreeUpdating_ = true;
    packTree_->clear();
    dropHoverItem_ = nullptr;

    auto& pm = ProjectManager::instance();
    auto* project = pm.currentProject();
    const bool open = project != nullptr;
    packTree_->setVisible(open);
    if (!project) {
        packTreeUpdating_ = false;
        if (objectSendWarningLabel_)
            objectSendWarningLabel_->setVisible(false);
        updateExportButton();
        return;
    }

    const QString ext = HarkenEditor::Bridge::packArtifactExtension();
    const QStringList viewPlatforms = viewPlatforms_;
    QHash<QString, QString> outputDirs;
    for (const QString& platform : viewPlatforms)
        outputDirs.insert(platform, platformOutputDirFor(platform));
    const QString dash = tr("—");

    const std::vector<ProjectPackExporter::ActionPackUnit> units =
        ProjectPackExporter::CollectActionPackUnits(*project);
    QHash<harkenstudio::ObjectId, QString> nameByStableId;
    QHash<harkenstudio::ObjectId, uint32_t> objectIdByStableId;
    QSet<harkenstudio::ObjectId> packableActionIds;
    for (const auto& unit : units) {
        nameByStableId.insert(unit.stableId, unit.name);
        objectIdByStableId.insert(unit.stableId, unit.objectID);
        const auto* item = pm.findItemByObjectID(unit.objectID);
        const auto* action = item ? item->comp<VAction>() : nullptr;
        if (action && !action->steps.empty())
            packableActionIds.insert(unit.stableId);
    }
    const auto buildPlan = PackBuildPlanning::build(*project);
    QHash<QString, QStringList> languagesByUnit;
    for (const auto& unit : buildPlan.units)
        if (!languagesByUnit[unit.artifactKey].contains(unit.variant.language))
            languagesByUnit[unit.artifactKey].append(unit.variant.language);

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
        cell.version = dash;
        cell.crc = dash;

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
            cell.size = dash;
            cell.date = dash;
            return cell;
        }
        const QString dir = outputDirs.value(platform);
        if (dir.isEmpty()) {
            cell.built = dash;
            cell.size = dash;
            cell.date = dash;
            return cell;
        }
        const QFileInfo fi(QDir(dir).filePath(relativeBase + ext));
        if (!fi.exists()) {
            cell.built = tr("No");
            cell.size = dash;
            cell.date = dash;
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
                cell.size = dash;
                cell.date = dash;
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

    auto applyDisk = [&](QTreeWidgetItem* item,
                         const std::function<QString(const QString&, const QString&)>& baseName,
                         const QStringList& languages, const QString& unitKey,
                         const std::function<bool(const QString&)>& included,
                         const QString& nameError = QString(),
                         const std::vector<harkenstudio::ObjectId>& actionStableIds = {}) {
        if (nameError.isEmpty() && languages.isEmpty()
            && std::none_of(viewPlatforms.begin(), viewPlatforms.end(), included)) {
            item->setText(kColBuilt, viewPlatforms.isEmpty() ? dash : tr("No"));
            for (int column : {kColSize, kColDate, kColFile, kColVersion, kColCrc})
                item->setText(column, dash);
            return;
        }
        if (!nameError.isEmpty() || languages.isEmpty()) {
            const QString reason = nameError.isEmpty()
                ? tr("Configure a target language or turn off Language for the referenced Sounds.")
                : nameError;
            item->setText(kColBuilt, tr("No"));
            item->setText(kColResult, nameError.isEmpty()
                ? tr("No target languages") : tr("Invalid pack name"));
            item->setData(kColResult, TableTextRole, static_cast<int>(TableTextState::Error));
            item->setToolTip(kColResult, reason);
            item->setToolTip(kColBuilt, reason);
            for (int column : {kColSize, kColDate, kColFile, kColVersion, kColCrc})
                item->setText(column, dash);
            return;
        }
        const int variantCount = viewPlatforms.size() * languages.size();
        if (devMode) {
            if (variantCount == 1) {
                item->setText(kColFile,
                    baseName(viewPlatforms.front(), languages.front()) + ext);
            } else {
                item->setText(kColFile, dash);
                QStringList files;
                for (const QString& platform : viewPlatforms)
                    for (const QString& language : languages)
                        files << QStringLiteral("%1 / %2: %3")
                            .arg(platformName(platform), language,
                                 baseName(platform, language) + ext);
                item->setToolTip(kColFile, files.join(QLatin1Char('\n')));
            }
        }
        if (viewPlatforms.isEmpty()) {
            item->setText(kColBuilt, dash);
            item->setText(kColSize, dash);
            item->setText(kColDate, dash);
            if (devMode) {
                item->setText(kColVersion, dash);
                item->setText(kColCrc, dash);
            }
            return;
        }
        QList<DiskCell> cells;
        QStringList variantLabels;
        for (const QString& platform : viewPlatforms) {
            for (const QString& language : languages) {
                cells << diskCell(platform, language, baseName(platform, language),
                                  included(platform), unitKey, actionStableIds);
                variantLabels << (language.isEmpty()
                    ? platformName(platform) : QStringLiteral("%1 / %2").arg(platformName(platform), language));
            }
        }

        auto merge = [&](int column, QString DiskCell::* field) {
            bool same = true;
            for (const DiskCell& cell : cells)
                if (cell.*field != cells.first().*field) {
                    same = false;
                    break;
                }
            if (same) {
                item->setText(column, cells.first().*field);
                item->setToolTip(column, QString());
                return;
            }
            item->setText(column, dash);
            QStringList lines;
            for (int i = 0; i < cells.size(); ++i)
                lines << QStringLiteral("%1: %2")
                    .arg(variantLabels.at(i), cells.at(i).*field);
            item->setToolTip(column, lines.join(QLatin1Char('\n')));
        };
        merge(kColBuilt, &DiskCell::built);
        merge(kColSize, &DiskCell::size);
        merge(kColDate, &DiskCell::date);
        merge(kColResult, &DiskCell::result);
        merge(kColDuration, &DiskCell::duration);
        if (devMode) {
            merge(kColVersion, &DiskCell::version);
            merge(kColCrc, &DiskCell::crc);
        }

        bool sameResult = true;
        for (const DiskCell& cell : cells)
            if (cell.result != cells.first().result) {
                sameResult = false;
                break;
            }
        if (sameResult && !cells.first().result.isEmpty())
            item->setData(kColResult, TableTextRole,
                static_cast<int>(cells.first().failed ? TableTextState::Error : TableTextState::Success));
        else
            item->setData(kColResult, TableTextRole, static_cast<int>(TableTextState::Normal));

        QStringList resultTip;
        if (cells.size() > 1) {
            for (int i = 0; i < cells.size(); ++i) {
                const DiskCell& cell = cells.at(i);
                if (cell.result.isEmpty() && cell.resultTip.isEmpty())
                    continue;
                QString line = QStringLiteral("%1: %2").arg(variantLabels.at(i),
                    cell.result.isEmpty() ? tr("not exported") : cell.result);
                if (!cell.resultTip.isEmpty())
                    line += QStringLiteral(" — ") + cell.resultTip;
                resultTip << line;
            }
        } else if (!cells.first().resultTip.isEmpty()) {
            resultTip << cells.first().resultTip;
        }
        item->setToolTip(kColResult, resultTip.join(QLatin1Char('\n')));

        QStringList builtTip;
        QString packPath;
        for (int i = 0; i < cells.size(); ++i) {
            const DiskCell& cell = cells.at(i);
            builtTip << QStringLiteral("%1: %2").arg(variantLabels.at(i),
                cell.path.isEmpty() ? tr("not built")
                                    : QDir::toNativeSeparators(cell.path));
            if (packPath.isEmpty())
                packPath = cell.path;
        }
        item->setToolTip(kColBuilt, builtTip.join(QLatin1Char('\n')));
        item->setData(0, kPackPathRole, packPath);
    };

    auto* systemSection = new QTreeWidgetItem(packTree_);
    systemSection->setText(kColName, tr("System"));
    systemSection->setData(0, kNodeKindRole, static_cast<int>(NodeKind::Section));
    systemSection->setData(0, TableSectionRole, true);
    systemSection->setFlags(Qt::ItemIsEnabled);

    auto addSystemPack = [&](const QString& label, const QString& baseName,
                             const QString& tooltip) {
        auto* item = new QTreeWidgetItem(packTree_);
        item->setText(kColName, label);
        item->setData(0, kNodeKindRole, static_cast<int>(NodeKind::SystemPack));
        item->setFlags(Qt::ItemIsEnabled);
        item->setToolTip(kColName, tooltip);
        const QString language;
        applyDisk(item,
            [baseName](const QString&, const QString&) { return baseName; },
            {language}, baseName, [](const QString&) { return true; });
    };
    addSystemPack(tr("Init"), QStringLiteral("Init"),
        tr("Buses, sends and attenuations. Rebuilt on every export."));

    auto* contentSection = new QTreeWidgetItem(packTree_);
    contentSection->setText(kColName, tr("Content"));
    contentSection->setData(0, kNodeKindRole, static_cast<int>(NodeKind::Section));
    contentSection->setData(0, TableSectionRole, true);
    contentSection->setFlags(Qt::ItemIsEnabled);

    const std::vector<ActionPackTargeting> targeting = pm.getActionPackTargeting();
    auto actionIncludes = [&targeting](harkenstudio::ObjectId stableId, const QString& platform) {
        for (const ActionPackTargeting& entry : targeting)
            if (entry.stableId == stableId)
                return entry.includesPlatform(platform);
        return true;
    };

    auto viewCheckState = [&viewPlatforms](
            const std::function<bool(const QString&)>& included) {
        int onCount = 0;
        for (const QString& platform : viewPlatforms)
            if (included(platform))
                ++onCount;
        return onCount == 0 ? Qt::Unchecked
            : onCount == viewPlatforms.size() ? Qt::Checked
                                              : Qt::PartiallyChecked;
    };
    auto partialTooltip = [&viewPlatforms](
            const std::function<bool(const QString&)>& included) {
        QStringList inc;
        QStringList exc;
        for (const QString& platform : viewPlatforms)
            (included(platform) ? inc : exc) << platformName(platform);
        return tr("Included: %1\nExcluded: %2")
            .arg(inc.join(QStringLiteral(", ")), exc.join(QStringLiteral(", ")));
    };

    const std::vector<PackGroup> groups = pm.getPackGroups();

    QSet<harkenstudio::ObjectId> groupedIds;
    for (const PackGroup& g : groups)
        for (harkenstudio::ObjectId id : g.actionIds)
            groupedIds.insert(id);

    const bool matrixEditable = !viewPlatforms.isEmpty();

    for (const PackGroup& g : groups) {
        std::vector<harkenstudio::ObjectId> members;
        for (harkenstudio::ObjectId memberId : g.actionIds)
            if (packableActionIds.contains(memberId))
                members.push_back(memberId);
        const bool exportable = !members.empty();

        auto* groupItem = new QTreeWidgetItem(packTree_);
        groupItem->setText(kColName, g.name);
        groupItem->setData(0, kNodeKindRole, static_cast<int>(NodeKind::Group));
        groupItem->setData(0, kGroupNameRole, g.name);
        Qt::ItemFlags groupFlags =
            Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable;
        if (exportable && matrixEditable)
            groupFlags |= Qt::ItemIsUserCheckable;
        groupItem->setFlags(groupFlags);
        const auto groupIncludes = [&g](const QString& platform) {
            return g.includesPlatform(platform);
        };
        const Qt::CheckState checkState = matrixEditable
            ? viewCheckState(groupIncludes) : Qt::Unchecked;
        const bool excluded = exportable && matrixEditable
            && checkState == Qt::Unchecked;
        if (exportable && matrixEditable) {
            groupItem->setCheckState(kColName, checkState);
            if (checkState == Qt::PartiallyChecked)
                groupItem->setToolTip(kColName, partialTooltip(groupIncludes));
        }
        applyDisk(groupItem,
            [&g](const QString&, const QString& language) {
                return PackBuildPlanning::packFileName(language, g.name, {});
            }, languagesByUnit.value(QStringLiteral("group_") + g.name),
            QStringLiteral("g:") + g.name,
            [&g, exportable](const QString& platform) {
                return exportable && g.includesPlatform(platform);
            }, PackBuildPlanning::packNameError(g.name, true), members);
        groupItem->setExpanded(true);
        if (excluded)
            for (int c = 0; c < kPackTreeColumnCount; ++c)
                groupItem->setData(c, TableTextRole, static_cast<int>(TableTextState::Disabled));

        if (!exportable) {
            groupItem->setData(kColName, TableTextRole, static_cast<int>(TableTextState::Muted));
            groupItem->setToolTip(kColName,
                tr("No Actions with steps — nothing to export"));
        }

        for (harkenstudio::ObjectId memberId : g.actionIds) {
            auto* memberItem = new QTreeWidgetItem(groupItem);
            memberItem->setData(0, kNodeKindRole, static_cast<int>(NodeKind::Member));
            memberItem->setData(0, kGroupNameRole, g.name);
            memberItem->setData(0, kStableIdRole, QVariant::fromValue(memberId));
            auto nameIt = nameByStableId.find(memberId);
            if (nameIt != nameByStableId.end()) {
                memberItem->setText(kColName, nameIt.value());
                memberItem->setData(0, kObjectIdRole, objectIdByStableId.value(memberId, 0u));
                memberItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable
                                     | Qt::ItemIsDragEnabled);
                memberItem->setToolTip(kColName, packableActionIds.contains(memberId)
                    ? tr("Included in pack '%1'").arg(g.name)
                    : tr("Action has no steps; not included in the pack"));
                for (int column = kColBuilt; column < kPackTreeColumnCount; ++column)
                    memberItem->setText(column, dash);
                if (excluded)
                    memberItem->setData(kColName, TableTextRole, static_cast<int>(TableTextState::Disabled));
            } else {
                memberItem->setText(kColName, tr("missing action (id %1)").arg(objectIdString(memberId)));
                memberItem->setData(kColName, TableTextRole, static_cast<int>(TableTextState::Error));
                memberItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            }
        }
    }

    for (const auto& unit : units) {
        if (groupedIds.contains(unit.stableId))
            continue;
        auto* item = new QTreeWidgetItem(packTree_);
        item->setText(kColName, unit.name);
        item->setData(0, kNodeKindRole, static_cast<int>(NodeKind::UnassignedAction));
        item->setData(0, kStableIdRole, QVariant::fromValue(unit.stableId));
        item->setData(0, kObjectIdRole, unit.objectID);
        Qt::ItemFlags itemFlags = Qt::ItemIsEnabled | Qt::ItemIsSelectable
                                  | Qt::ItemIsDragEnabled;
        if (matrixEditable)
            itemFlags |= Qt::ItemIsUserCheckable;
        item->setFlags(itemFlags);
        const auto unitIncludes = [&actionIncludes, &unit](const QString& platform) {
            return actionIncludes(unit.stableId, platform);
        };
        const Qt::CheckState checkState = matrixEditable
            ? viewCheckState(unitIncludes) : Qt::Unchecked;
        const bool excluded = matrixEditable && checkState == Qt::Unchecked;
        if (matrixEditable) {
            item->setCheckState(kColName, checkState);
            if (checkState == Qt::PartiallyChecked)
                item->setToolTip(kColName, partialTooltip(unitIncludes));
        }
        applyDisk(item,
            [&unit](const QString&, const QString& language) {
                return PackBuildPlanning::packFileName(language, unit.name, {});
            }, languagesByUnit.value(PackBuildPlanning::stableArtifactKey(unit.stableId)),
            QStringLiteral("a:") + QString::number(unit.objectID), unitIncludes,
            PackBuildPlanning::packNameError(unit.name), {unit.stableId});
        if (excluded)
            for (int c = 0; c < kPackTreeColumnCount; ++c)
                item->setData(c, TableTextRole, static_cast<int>(TableTextState::Disabled));
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

QList<uint32_t> PackManagerPanel::objectIDsFromMime(const QMimeData* mime)
{
    QList<uint32_t> ids;
    if (!mime) return ids;

    QByteArray payload;
    if (mime->hasFormat(QLatin1String(kHarkenProjectItemsMime)))
        payload = mime->data(QLatin1String(kHarkenProjectItemsMime));
    else if (mime->hasFormat(QLatin1String(kHarkenProjectItemMime)))
        payload = mime->data(QLatin1String(kHarkenProjectItemMime));

    const int n = payload.size() / static_cast<int>(sizeof(uint32_t));
    ids.reserve(n);
    for (int i = 0; i < n; ++i) {
        uint32_t id = 0;
        std::memcpy(&id, payload.constData() + i * sizeof(uint32_t), sizeof(uint32_t));
        if (id != 0) ids.append(id);
    }
    return ids;
}

void PackManagerPanel::applyActionObjectsToGroup(const QList<uint32_t>& actionObjectIDs,
                                                 const QString& groupName)
{
    auto* project = ProjectManager::instance().currentProject();
    if (!project || groupName.isEmpty() || actionObjectIDs.isEmpty())
        return;

    const QHash<uint32_t, harkenstudio::ObjectId> idMap = buildObjectIdToStableId(*project);
    QJsonArray ids;
    for (uint32_t objectID : actionObjectIDs) {
        auto it = idMap.constFind(objectID);
        if (it != idMap.constEnd())
            ids.append(objectIdString(it.value()));
    }
    if (ids.isEmpty())
        return;

    ActionManager::instance().execute(
        QStringLiteral("add_actions_to_pack_group"),
        QJsonObject{{QStringLiteral("groupName"), groupName},
                    {QStringLiteral("actionIds"), ids}},
        ActionSource::GUI);
}

void PackManagerPanel::detachActionObjects(const QList<uint32_t>& actionObjectIDs)
{
    auto* project = ProjectManager::instance().currentProject();
    if (!project || actionObjectIDs.isEmpty())
        return;

    const QHash<uint32_t, harkenstudio::ObjectId> idMap = buildObjectIdToStableId(*project);
    QHash<harkenstudio::ObjectId, QString> groupByStableId;
    for (const PackGroup& g : project->packGroups)
        for (harkenstudio::ObjectId id : g.actionIds)
            groupByStableId.insert(id, g.name);

    QList<QPair<QString, harkenstudio::ObjectId>> members;
    for (uint32_t objectID : actionObjectIDs) {
        auto it = idMap.constFind(objectID);
        if (it == idMap.constEnd())
            continue;
        auto owner = groupByStableId.constFind(it.value());
        if (owner != groupByStableId.constEnd())
            members.append({owner.value(), it.value()});
    }
    removeMembersFromGroups(members);
}

void PackManagerPanel::removeMembersFromGroups(const QList<QPair<QString, harkenstudio::ObjectId>>& members)
{
    if (members.isEmpty())
        return;

    QHash<QString, QJsonArray> idsByGroup;
    for (const auto& member : members)
        idsByGroup[member.first].append(objectIdString(member.second));

    QModelIndex removedIndex;
    for (QTreeWidgetItemIterator it(packTree_); *it; ++it) {
        const auto* item = *it;
        if (nodeKind(item) != NodeKind::Member || item->isHidden()
            || item->parent()->isHidden() || !item->parent()->isExpanded())
            continue;
        if (members.contains({item->data(0, kGroupNameRole).toString(),
                              item->data(0, kStableIdRole).value<harkenstudio::ObjectId>()})) {
            removedIndex = packTree_->indexFromItem(item);
            break;
        }
    }

    const auto remove = [&idsByGroup, &members] {
        QUndoStack* stack = UndoRedoManager::instance().undoStack();
        const bool macro = idsByGroup.size() > 1 && stack;
        if (macro)
            stack->beginMacro(tr("Remove %1 Action(s) from Pack Groups").arg(members.size()));

        bool changed = false;
        for (auto it = idsByGroup.constBegin(); it != idsByGroup.constEnd(); ++it) {
            const auto result = ActionManager::instance().execute(
                QStringLiteral("remove_actions_from_pack_group"),
                QJsonObject{{QStringLiteral("groupName"), it.key()},
                            {QStringLiteral("actionIds"), it.value()}},
                ActionSource::GUI);
            changed = changed || result.success;
        }

        if (macro)
            stack->endMacro();
        return changed;
    };
    if (removedIndex.isValid())
        static_cast<TableWidget*>(packTree_->parentWidget())->removeSelectedRow(remove, removedIndex);
    else
        remove();
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

void PackManagerPanel::requestExport(const QStringList& unitNames)
{
    if (!ProjectManager::instance().currentProject())
        return;
    const QStringList platforms = generationPlatforms();
    if (platforms.isEmpty()) {
        ToastManager::instance().showToast(
            tr("Select at least one platform to export"), 2000, ToastType::Info);
        return;
    }
    emit exportRequested(platforms, unitNames);
}

QStringList PackManagerPanel::selectedUnitNames() const
{
    QStringList out;
    if (!packTree_)
        return out;
    for (QTreeWidgetItem* sel : packTree_->selectedItems()) {
        QString name;
        switch (nodeKind(sel)) {
            case NodeKind::Group:
            case NodeKind::Member:
                name = sel->data(0, kGroupNameRole).toString();
                break;
            case NodeKind::UnassignedAction:
                name = sel->text(kColName);
                break;
            default:
                break;
        }
        if (!name.isEmpty() && !out.contains(name))
            out << name;
    }
    return out;
}

void PackManagerPanel::updateExportButton()
{
    if (!exportBtn_)
        return;
    auto* project = ProjectManager::instance().currentProject();
    exportBtn_->setEnabled(project
        && !effectiveSelectedPlatforms(project).isEmpty());
}

void PackManagerPanel::onPackItemChanged(QTreeWidgetItem* item, int column)
{
    if (packTreeUpdating_ || !item || column != kColName)
        return;

    const NodeKind kind = nodeKind(item);
    if (kind != NodeKind::Group && kind != NodeKind::UnassignedAction)
        return;

    const QVariant checkData = item->data(kColName, Qt::CheckStateRole);
    if (checkData.isValid())
        toggleUnitPlatform(item, checkData.toInt() == Qt::Checked);

    if (kind != NodeKind::Group)
        return;

    const QString oldName = item->data(0, kGroupNameRole).toString();
    const QString newName = item->text(kColName).trimmed();
    if (newName == oldName) {
        if (item->text(kColName) != oldName) {
            packTreeUpdating_ = true;
            item->setText(kColName, oldName);
            packTreeUpdating_ = false;
        }
        return;
    }
    if (newName.isEmpty()) {
        populateProjectPackTable();
        return;
    }

    QTimer::singleShot(0, this, [this, oldName, newName]() {
        const ActionResult result = ActionManager::instance().execute(
            QStringLiteral("rename_pack_group"),
            QJsonObject{{QStringLiteral("oldName"), oldName},
                        {QStringLiteral("newName"), newName}},
            ActionSource::GUI);
        if (!result.success) {
            ToastManager::instance().showToast(
                result.errorMessage.isEmpty()
                    ? tr("Could not rename pack group") : result.errorMessage,
                3000, ToastType::Warning);
            populateProjectPackTable();
        }
    });
}

void PackManagerPanel::toggleUnitPlatform(QTreeWidgetItem* item, bool include)
{
    auto* project = ProjectManager::instance().currentProject();
    if (!project || viewPlatforms_.isEmpty())
        return;

    const QStringList allPlatforms = effectivePlatforms(project);
    const NodeKind kind = nodeKind(item);

    QStringList current;
    QString groupName;
    harkenstudio::ObjectId stableId = {};
    if (kind == NodeKind::Group) {
        groupName = item->data(0, kGroupNameRole).toString();
        const PackGroup* group = nullptr;
        for (const PackGroup& g : project->packGroups)
            if (g.name == groupName) { group = &g; break; }
        if (!group)
            return;
        current = group->allPlatforms ? allPlatforms : group->platformIds;
    } else if (kind == NodeKind::UnassignedAction) {
        stableId = item->data(0, kStableIdRole).value<harkenstudio::ObjectId>();
        current = allPlatforms;
        for (const ActionPackTargeting& entry : project->actionPackTargeting) {
            if (entry.stableId != stableId)
                continue;
            current = entry.allPlatforms ? allPlatforms : entry.platformIds;
            break;
        }
    } else {
        return;
    }

    const QSet<QString> currentSet(current.cbegin(), current.cend());
    QSet<QString> next = currentSet;
    for (const QString& platform : viewPlatforms_) {
        if (include)
            next.insert(platform);
        else
            next.remove(platform);
    }
    if (next == currentSet)
        return;
    QJsonArray platformArr;
    int count = 0;
    for (const QString& p : allPlatforms) {
        if (!next.contains(p))
            continue;
        platformArr.append(p);
        ++count;
    }
    const bool nextAll = count == allPlatforms.size();
    if (nextAll)
        platformArr = QJsonArray();

    QTimer::singleShot(0, this, [this, kind, groupName, stableId,
                                 nextAll, platformArr]() {
        QJsonObject payload{{QStringLiteral("allPlatforms"), nextAll},
                            {QStringLiteral("platform_ids"), platformArr}};
        QString actionId;
        if (kind == NodeKind::Group) {
            actionId = QStringLiteral("set_pack_group_platforms");
            payload.insert(QStringLiteral("groupName"), groupName);
        } else {
            actionId = QStringLiteral("set_action_pack_targeting");
            payload.insert(QStringLiteral("stableId"),
                           objectIdString(stableId));
        }
        const ActionResult result =
            ActionManager::instance().execute(actionId, payload, ActionSource::GUI);
        if (!result.success) {
            ToastManager::instance().showToast(
                result.errorMessage.isEmpty()
                    ? tr("Could not update pack platform targeting")
                    : result.errorMessage,
                3000, ToastType::Warning);
            populateProjectPackTable();
        }
    });
}

void PackManagerPanel::clearGenerationResults()
{
    lastResults_.clear();
    PackGenerationRecordStore::instance().clear();
}

PackManagerPanel::DropTarget
PackManagerPanel::dropTargetAtPanelPos(const QPoint& panelPos) const
{
    DropTarget target;
    if (!packTree_ || !packTree_->isVisible())
        return target;
    QWidget* vp = packTree_->viewport();
    const QPoint p = vp->mapFrom(this, panelPos);
    if (!vp->rect().contains(p))
        return target;
    QTreeWidgetItem* item = packTree_->itemAt(p);
    if (!item) {
        target.kind = DropKind::Root;
        return target;
    }
    switch (nodeKind(item)) {
        case NodeKind::Group:
        case NodeKind::Member:
            target.kind = DropKind::Group;
            target.groupName = item->data(0, kGroupNameRole).toString();
            break;
        case NodeKind::UnassignedAction:
            target.kind = DropKind::Action;
            target.actionObjectId = item->data(0, kObjectIdRole).toUInt();
            break;
        default:
            break;
    }
    return target;
}

void PackManagerPanel::setDropHoverItem(QTreeWidgetItem* item)
{
    QTreeWidgetItem* target = nullptr;
    if (item) {
        switch (nodeKind(item)) {
            case NodeKind::Group:
            case NodeKind::UnassignedAction:
                target = item;
                break;
            case NodeKind::Member:
                target = item->parent();
                break;
            default:
                break;
        }
    }
    if (target == dropHoverItem_)
        return;
    QSignalBlocker blocker(packTree_);
    auto paint = [](QTreeWidgetItem* it, bool on) {
        if (!it) return;
        it->setData(0, TableDropTargetRole, on);
    };
    paint(dropHoverItem_, false);
    paint(target, true);
    dropHoverItem_ = target;
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

void PackManagerPanel::dragEnterEvent(QDragEnterEvent* event)
{
    dragActionIds_.clear();
    const QMimeData* m = event->mimeData();
    if (!m->hasFormat(QLatin1String(kHarkenProjectItemsMime)) &&
        !m->hasFormat(QLatin1String(kHarkenProjectItemMime)))
        return;
    auto* project = ProjectManager::instance().currentProject();
    if (!project)
        return;
    const QSet<uint32_t> actions = actionObjectIds(*project);
    for (uint32_t id : objectIDsFromMime(m))
        if (actions.contains(id))
            dragActionIds_.append(id);
    if (dragActionIds_.isEmpty())
        return;
    event->acceptProposedAction();
}

void PackManagerPanel::dragMoveEvent(QDragMoveEvent* event)
{
    const DropTarget target = dropTargetAtPanelPos(event->position().toPoint());
    if (target.kind == DropKind::None || dragActionIds_.isEmpty()) {
        event->ignore();
        setDropHoverItem(nullptr);
        return;
    }
    event->acceptProposedAction();
    QTreeWidgetItem* hover = nullptr;
    if (packTree_ && packTree_->isVisible()) {
        QWidget* vp = packTree_->viewport();
        const QPoint p = vp->mapFrom(this, event->position().toPoint());
        if (vp->rect().contains(p))
            hover = packTree_->itemAt(p);
    }
    setDropHoverItem(hover);
}

void PackManagerPanel::dragLeaveEvent(QDragLeaveEvent* )
{
    setDropHoverItem(nullptr);
    dragActionIds_.clear();
}

void PackManagerPanel::dropEvent(QDropEvent* event)
{
    setDropHoverItem(nullptr);

    if (dragActionIds_.isEmpty())
        return;

    const DropTarget target = dropTargetAtPanelPos(event->position().toPoint());
    if (target.kind == DropKind::Group)
        applyActionObjectsToGroup(dragActionIds_, target.groupName);
    else if (target.kind == DropKind::Action) {
        QList<uint32_t> ids = dragActionIds_;
        if (target.actionObjectId != 0 && !ids.contains(target.actionObjectId))
            ids.append(target.actionObjectId);
        if (ids.size() < 2) {
            dragActionIds_.clear();
            return;
        }
        groupActionObjects(ids);
    }
    else if (target.kind == DropKind::Root)
        detachActionObjects(dragActionIds_);
    else {
        dragActionIds_.clear();
        return;
    }
    event->acceptProposedAction();
    dragActionIds_.clear();
}

void PackManagerPanel::onPackTreeContextMenu(const QPoint& pos)
{
    if (!packTree_)
        return;
    QTreeWidgetItem* item = packTree_->itemAt(pos);
    const NodeKind kind = nodeKind(item);

    QMenu menu(this);

    const QStringList units = kind == NodeKind::SystemPack
        ? QStringList() : selectedUnitNames();
    if (!units.isEmpty()) {
        const QString label = units.size() == 1
            ? tr("Export \"%1\"").arg(units.first())
            : tr("Export Selected (%1)").arg(units.size());
        menu.addAction(label, this, [this, units]() { requestExport(units); });
    }

    if (kind == NodeKind::SystemPack) {
        const QString packPath = item->data(0, kPackPathRole).toString();
        QAction* show = menu.addAction(tr("Show Pack in Explorer"), this,
            [this, packPath]() { showFileInExplorer(packPath); });
        show->setEnabled(!packPath.isEmpty());
        if (AppSettings::instance().developerMode()) {
            QAction* preview = menu.addAction(tr("Preview Pack Structure..."),
                this, [this, packPath]() {
                (new PackStructureDialog(packPath, this))->show();
            });
            preview->setEnabled(!packPath.isEmpty() && QFileInfo::exists(packPath));
        }
    }

    QList<uint32_t> selectedActionIds;
    if (kind != NodeKind::SystemPack) {
        for (QTreeWidgetItem* sel : packTree_->selectedItems()) {
            const NodeKind selKind = nodeKind(sel);
            if (selKind != NodeKind::Member && selKind != NodeKind::UnassignedAction)
                continue;
            const uint32_t id = sel->data(0, kObjectIdRole).toUInt();
            if (id != 0 && !selectedActionIds.contains(id))
                selectedActionIds.append(id);
        }
    }
    if (selectedActionIds.size() >= 2) {
        menu.addAction(tr("Group %1 Actions").arg(selectedActionIds.size()),
                       this, [this, selectedActionIds]() {
            groupActionObjects(selectedActionIds);
        });
    }

    const uint32_t objectID = item ? item->data(0, kObjectIdRole).toUInt() : 0u;
    if ((kind == NodeKind::Member || kind == NodeKind::UnassignedAction)
        && objectID != 0) {
        menu.addAction(tr("Select in Explorer"), this,
            [this, objectID]() { emit revealActionRequested(objectID); });
    }
    if (kind == NodeKind::Group || kind == NodeKind::UnassignedAction) {
        const QString packPath = item->data(0, kPackPathRole).toString();
        QAction* show = menu.addAction(tr("Show Pack in Explorer"), this,
            [this, packPath]() { showFileInExplorer(packPath); });
        show->setEnabled(!packPath.isEmpty());
        if (AppSettings::instance().developerMode()) {
            QAction* preview = menu.addAction(tr("Preview Pack Structure..."),
                this, [this, packPath]() {
                (new PackStructureDialog(packPath, this))->show();
            });
            preview->setEnabled(!packPath.isEmpty()
                                && QFileInfo::exists(packPath));
        }
    }

    if (kind == NodeKind::Group) {
        const QString groupName = item->data(0, kGroupNameRole).toString();
        menu.addSeparator();
        menu.addAction(tr("Rename Group"), this,
                       [this, item]() { packTree_->editItem(item, kColName); });
        menu.addAction(tr("Delete Group"), this, [this, groupName]() {
            if (QMessageBox::question(
                    this, tr("Delete Pack Group"),
                    tr("Delete pack group \"%1\"? Its Actions return to individual Action packs.")
                        .arg(groupName)) != QMessageBox::Yes)
                return;
            QModelIndex removedIndex;
            for (int i = 0; i < packTree_->topLevelItemCount(); ++i) {
                const auto* group = packTree_->topLevelItem(i);
                if (nodeKind(group) == NodeKind::Group
                    && group->data(0, kGroupNameRole).toString() == groupName) {
                    removedIndex = packTree_->indexFromItem(group);
                    break;
                }
            }
            if (!removedIndex.isValid()) return;
            static_cast<TableWidget*>(packTree_->parentWidget())->removeSelectedRow([groupName] {
                return ActionManager::instance().execute(
                    QStringLiteral("delete_pack_group"),
                    QJsonObject{{QStringLiteral("name"), groupName}},
                    ActionSource::GUI).success;
            }, removedIndex);
        });
    } else if (kind == NodeKind::Member) {
        menu.addSeparator();
        menu.addAction(tr("Remove from Group"), this, [this]() {
            QList<QPair<QString, harkenstudio::ObjectId>> members;
            for (QTreeWidgetItem* sel : packTree_->selectedItems()) {
                if (nodeKind(sel) != NodeKind::Member)
                    continue;
                members.append({sel->data(0, kGroupNameRole).toString(),
                                sel->data(0, kStableIdRole).value<harkenstudio::ObjectId>()});
            }
            removeMembersFromGroups(members);
        });
    } else if (kind == NodeKind::UnassignedAction) {
        const std::vector<PackGroup> groups = ProjectManager::instance().getPackGroups();
        if (!groups.empty()) {
            menu.addSeparator();
            QMenu* addMenu = menu.addMenu(tr("Add to Group"));
            for (const PackGroup& g : groups) {
                const QString groupName = g.name;
                addMenu->addAction(groupName, this, [this, groupName]() {
                    QList<uint32_t> ids;
                    for (QTreeWidgetItem* sel : packTree_->selectedItems())
                        if (nodeKind(sel) == NodeKind::UnassignedAction)
                            ids.append(sel->data(0, kObjectIdRole).toUInt());
                    applyActionObjectsToGroup(ids, groupName);
                });
            }
        }
    }

    menu.addSeparator();
    menu.addAction(tr("Export Settings..."), this,
                   &PackManagerPanel::onOpenSettingsDialog);

    menu.exec(packTree_->viewport()->mapToGlobal(pos));
}

QString PackManagerPanel::nextAutoGroupName() const
{
    QSet<QString> names;
    for (const PackGroup& g : ProjectManager::instance().getPackGroups())
        names.insert(g.name);
    int index = 1;
    while (names.contains(QStringLiteral("Group %1").arg(index)))
        ++index;
    return QStringLiteral("Group %1").arg(index);
}

void PackManagerPanel::groupActionObjects(const QList<uint32_t>& actionObjectIDs)
{
    auto* project = ProjectManager::instance().currentProject();
    if (!project || actionObjectIDs.isEmpty())
        return;

    const QString name = nextAutoGroupName();
    QUndoStack* stack = UndoRedoManager::instance().undoStack();
    if (stack)
        stack->beginMacro(tr("Group %1 Action(s)").arg(actionObjectIDs.size()));
    const ActionResult created = ActionManager::instance().execute(
        QStringLiteral("create_pack_group"),
        QJsonObject{{QStringLiteral("name"), name}},
        ActionSource::GUI);
    if (created.success)
        applyActionObjectsToGroup(actionObjectIDs, name);
    if (stack)
        stack->endMacro();
    if (!created.success)
        ToastManager::instance().showToast(
            created.errorMessage.isEmpty()
                ? tr("Could not create pack group") : created.errorMessage,
            3000, ToastType::Warning);
}

}
