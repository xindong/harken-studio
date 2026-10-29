
#include "SelectorWindow.h"

#include "RefDomainAdapter.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/NameValidator.h"
#include "Core/ProjectManager.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/SearchField.h"
#include "Widgets/Common/SwitchIndicator.h"
#include "Widgets/Shared/TreeRowStyle.h"
#include "Widgets/Shared/TreeExpansionWidget.h"

#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QScreen>
#include <QSet>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace VNX {

namespace {

class SelectorItemDelegate : public TreeRowStyle::ItemRowDelegate
{
public:
    using TreeRowStyle::ItemRowDelegate::ItemRowDelegate;

    static QRect switchRect(const QRect& cell)
    {
        return QRect(cell.left() + (cell.width() - SwitchIndicator::Width) / 2,
                     cell.top() + (cell.height() - SwitchIndicator::Height) / 2,
                     SwitchIndicator::Width, SwitchIndicator::Height);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        QStyleOptionViewItem opt = option;
        const auto objectIndex = index.siblingAtColumn(0);
        if (objectIndex.data(kRoleObjectID).isValid() && !objectIndex.data(kRoleSelectable).toBool())
            opt.state &= ~QStyle::State_Enabled;
        if (index.column() != 1) {
            TreeRowStyle::ItemRowDelegate::paint(painter, opt, index);
            return;
        }
        initStyleOption(&opt, index);
        opt.features &= ~QStyleOptionViewItem::HasCheckIndicator;
        QStyle* style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
        if (index.data(Qt::CheckStateRole).isValid()) {
            const auto& colors = ThemeManager::instance().colors();
            const bool enabled = index.flags().testFlag(Qt::ItemIsEnabled)
                && index.flags().testFlag(Qt::ItemIsUserCheckable);
            SwitchIndicator::draw(painter, switchRect(opt.rect), opt.checkState,
                                  enabled && opt.state.testFlag(QStyle::State_MouseOver),
                                  !enabled ? colors.textDisabled
                                      : opt.checkState == Qt::Unchecked ? colors.textPrimary : colors.accent);
        }
    }

    bool editorEvent(QEvent* event, QAbstractItemModel* model,
                     const QStyleOptionViewItem& option,
                     const QModelIndex& index) override
    {
        if (index.column() != 1 || !index.data(Qt::CheckStateRole).isValid())
            return TreeRowStyle::ItemRowDelegate::editorEvent(event, model, option, index);
        if (!index.flags().testFlag(Qt::ItemIsEnabled)
            || !index.flags().testFlag(Qt::ItemIsUserCheckable))
            return false;
        if (event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonRelease
            || event->type() == QEvent::MouseButtonDblClick) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() != Qt::LeftButton || !switchRect(option.rect).contains(mouse->pos()))
                return false;
            if (event->type() == QEvent::MouseButtonRelease)
                model->setData(index, index.data(Qt::CheckStateRole).toInt() == Qt::Checked
                                         ? Qt::Unchecked : Qt::Checked,
                               Qt::CheckStateRole);
            return true;
        }
        return false;
    }
};

class SelectorTreeWidget : public TreeExpansionWidget
{
public:
    using TreeExpansionWidget::TreeExpansionWidget;

protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        QTreeWidgetItem* item = currentItem();
        if (columnCount() == 2 && event->key() == Qt::Key_Space && item
            && item->data(1, Qt::CheckStateRole).isValid()
            && item->flags().testFlag(Qt::ItemIsEnabled)
            && item->flags().testFlag(Qt::ItemIsUserCheckable)) {
            item->setCheckState(1, item->checkState(1) == Qt::Checked ? Qt::Unchecked : Qt::Checked);
            event->accept();
            return;
        }
        QTreeWidget::keyPressEvent(event);
    }

    void drawRow(QPainter* painter, const QStyleOptionViewItem& option,
                 const QModelIndex& index) const override
    {
        if (index.data(TreeRowStyle::kBucketRole).toBool())
        {
            int depth = 0;
            for (QModelIndex p = index.parent(); p.isValid(); p = p.parent())
                ++depth;
            QStyleOptionViewItem opt = option;
            opt.rect.setLeft(opt.rect.left() + depth * indentation());
            TreeRowStyle::paintBucketRow(painter, opt, index, isExpanded(index));
            return;
        }
        QTreeWidget::drawRow(painter, option, index);
        if (selectionModel()->isSelected(index))
            TreeRowStyle::paintRowAccent(painter, option.rect);
    }

    void drawBranches(QPainter* painter, const QRect& rect,
                      const QModelIndex& index) const override
    {
        if (index.data(TreeRowStyle::kBucketRole).toBool())
            return;

        TreeRowStyle::paintBranches(painter, rect, index, model(), indentation(),
                                    isExpanded(index));
    }
};

QTreeWidgetItem* findSelectableRow(QTreeWidget* tree, uint32_t objectID)
{
    if (objectID == 0)
        return nullptr;
    QTreeWidgetItem* found = nullptr;
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* item) {
        if (found || !item)
            return;
        if (item->data(0, kRoleObjectID).toUInt() == objectID
            && item->data(0, kRoleSelectable).toBool()) {
            found = item;
            return;
        }
        for (int i = 0; i < item->childCount(); ++i)
            walk(item->child(i));
    };
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        walk(tree->topLevelItem(i));
    return found;
}

}

SelectorWindow* SelectorWindow::s_instance = nullptr;

SelectorWindow* SelectorWindow::open(const SelectorRequest& request, QWidget* anchor,
                                    std::optional<QRect> globalAnchorRect)
{
    if (!s_instance)
        s_instance = new SelectorWindow();

    QObject::disconnect(s_instance->anchorDestroyed_);
    s_instance->anchor_ = anchor;
    if (anchor)
        s_instance->anchorDestroyed_ = connect(anchor, &QObject::destroyed, s_instance,
                                               [] { dismiss(); });
    s_instance->applyRequest(request);
    s_instance->globalAnchorRect_ = globalAnchorRect.value_or(anchor
        ? QRect(anchor->mapToGlobal(QPoint()), anchor->size())
        : QRect(QCursor::pos(), QSize(1, 1)));
    QScreen* screen = QApplication::screenAt(s_instance->globalAnchorRect_.center());
    if (!screen)
        screen = anchor ? anchor->screen() : s_instance->screen();
    s_instance->setScreen(screen);
    if (s_instance->isVisible())
        s_instance->positionAtAnchor();
    else
        s_instance->move(screen->availableGeometry().topLeft());
    s_instance->show();
    s_instance->raise();
    s_instance->activateWindow();
    if (request.creation) {
        s_instance->nameEdit_->setFocus();
        s_instance->nameEdit_->selectAll();
    } else {
        s_instance->searchBox_->setFocus();
    }
    return s_instance;
}

void SelectorWindow::showEvent(QShowEvent* event)
{
    BaseDialog::showEvent(event);
    positionAtAnchor();
}

void SelectorWindow::positionAtAnchor()
{
    layout()->activate();
    const QRect available = screen()->availableGeometry();
    const QSize frameExtra = frameGeometry().size() - size();
    resize(size().boundedTo(available.size() - frameExtra));
    const QSize frameSize = frameGeometry().size();
    QPoint position(globalAnchorRect_.left(), globalAnchorRect_.bottom() + 1);
    if (position.y() + frameSize.height() > available.bottom() + 1)
        position.setY(globalAnchorRect_.top() - frameSize.height());
    position.setX(std::clamp(position.x(), available.left(),
        std::max(available.left(), available.right() - frameSize.width() + 1)));
    position.setY(std::clamp(position.y(), available.top(),
        std::max(available.top(), available.bottom() - frameSize.height() + 1)));
    move(position);
}

void SelectorWindow::dismiss(QWidget* anchor)
{
    if (!s_instance || (anchor && s_instance->anchor_ != anchor))
        return;
    s_instance->request_ = {};
    s_instance->anchor_.clear();
    QObject::disconnect(s_instance->anchorDestroyed_);
    s_instance->close();
}

void SelectorWindow::refresh(QWidget* anchor)
{
    if (!s_instance || !s_instance->isVisible() || s_instance->refreshPending_
        || (anchor && s_instance->anchor_ != anchor))
        return;
    s_instance->refreshPending_ = true;
    QTimer::singleShot(0, s_instance, [] {
        s_instance->refreshPending_ = false;
        if (s_instance->isVisible())
            s_instance->rebuildTree(true);
    });
}

SelectorWindow::SelectorWindow()
    : BaseDialog(nullptr)
{
    setWindowTitle(tr("Select"));
    resize(320, 420);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    searchBox_ = new SearchField(this);
    layout->addWidget(searchBox_);

    tree_ = new SelectorTreeWidget(this);
    tree_->setObjectName("SelectorTreeWidget");
    tree_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

    QScrollBar* scrollBar = tree_->verticalScrollBar();
    scrollBar->style()->unpolish(scrollBar);
    scrollBar->style()->polish(scrollBar);
    connect(scrollBar, &QScrollBar::rangeChanged, scrollBar,
            [scrollBar](int minimum, int maximum) {
                scrollBar->setEnabled(minimum < maximum);
            });
    scrollBar->setEnabled(scrollBar->minimum() < scrollBar->maximum());

    tree_->setHeaderHidden(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setUniformRowHeights(true);
    const auto updateIconSize = [this]() {
        const int extent = ThemeManager::instance().metrics().objectIconExtent;
        tree_->setIconSize(QSize(extent, extent));
    };
    updateIconSize();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, updateIconSize);
    tree_->setItemDelegate(new SelectorItemDelegate(tree_));
    tree_->setRootIsDecorated(false);
    tree_->setExpandsOnDoubleClick(false);
    tree_->setMouseTracking(true);
    layout->addWidget(tree_, 1);

    creationForm_ = new QWidget(this);
    auto* creationLayout = new QVBoxLayout(creationForm_);
    creationLayout->setContentsMargins(0, 0, 0, 0);
    auto* nameLabel = new QLabel(tr("Name"), creationForm_);
    nameEdit_ = new QLineEdit(creationForm_);
    nameEdit_->setObjectName(QStringLiteral("selectorCreationName"));
    nameEdit_->setAccessibleName(nameLabel->text());
    nameLabel->setBuddy(nameEdit_);
    creationLayout->addWidget(nameLabel);
    creationLayout->addWidget(nameEdit_);
    creationError_ = new QLabel(creationForm_);
    creationError_->setObjectName(QStringLiteral("selectorCreationError"));
    creationError_->setTextFormat(Qt::PlainText);
    creationError_->setWordWrap(true);
    creationLayout->addWidget(creationError_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, creationForm_);
    createButton_ = buttons->button(QDialogButtonBox::Ok);
    createButton_->setText(tr("Create"));
    createButton_->setDefault(true);
    creationLayout->addWidget(buttons);
    layout->addWidget(creationForm_);
    connect(buttons, &QDialogButtonBox::accepted, this, &SelectorWindow::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &SelectorWindow::reject);
    connect(nameEdit_, &QLineEdit::textChanged, this, &SelectorWindow::refreshCreationValidation);
    connect(tree_, &QTreeWidget::currentItemChanged, this, &SelectorWindow::refreshCreationValidation);

    auto& manager = ProjectManager::instance();
    const auto refreshLocations = [this] {
        if (request_.creation && isVisible()) refresh();
    };
    connect(&manager, &ProjectManager::folderCreated, this, refreshLocations);
    connect(&manager, &ProjectManager::folderRenamed, this, refreshLocations);
    connect(&manager, &ProjectManager::folderMoved, this, refreshLocations);
    connect(&manager, &ProjectManager::folderDeleted, this, refreshLocations);
    connect(&manager, &ProjectManager::itemCreated, this, &SelectorWindow::refreshCreationValidation);
    connect(&manager, &ProjectManager::itemRenamed, this, &SelectorWindow::refreshCreationValidation);
    connect(&manager, &ProjectManager::itemDeleted, this, &SelectorWindow::refreshCreationValidation,
            Qt::QueuedConnection);
    connect(&manager, &ProjectManager::projectClosed, this, [this] {
        if (request_.creation) dismiss();
    });

    connect(searchBox_, &QLineEdit::textChanged,
            this, &SelectorWindow::applySearchFilter);
    const auto toggleBucket = [this](QTreeWidgetItem* item, int) {
        if (!request_.creation && item && item->data(0, TreeRowStyle::kBucketRole).toBool())
            item->setExpanded(!item->isExpanded());
    };
    connect(tree_, &QTreeWidget::itemClicked, this, toggleBucket);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, toggleBucket);
    connect(tree_, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem* item, int) {
                if (request_.creation) {
                    if (item) item->setExpanded(!item->isExpanded());
                    nameEdit_->setFocus();
                    nameEdit_->selectAll();
                    return;
                }
                if (!request_.onToggled && item && item->data(0, kRoleSelectable).toBool())
                    accept();
            });
    connect(tree_, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem* item, int column) {
                if (column != 1 || !request_.onToggled || !item
                    || !item->data(0, kRoleSelectable).toBool())
                    return;
                const uint32_t objectID = item->data(0, kRoleObjectID).toUInt();
                if (request_.canToggle && !request_.canToggle(objectID)) {
                    refreshSwitches();
                    return;
                }
                const bool checked = item->checkState(1) == Qt::Checked;
                const auto onToggled = request_.onToggled;
                tree_->setCurrentItem(item, column);
                onToggled(objectID, checked);
                refreshSwitches();
            });
}

void SelectorWindow::applyRequest(const SelectorRequest& request)
{
    request_ = request;
    const auto* project = ProjectManager::instance().currentProject();
    creationProjectId_ = request.creation && project ? project->projectId : QString{};
    creationForm_->setVisible(request.creation.has_value());
    nameEdit_->setText(request.creation ? request.creation->suggestedName : QString{});
    lastNotifiedID_ = request.currentObjectID;
    setWindowTitle(request.title.isEmpty() ? tr("Select") : request.title);
    tree_->setColumnCount(request.onToggled ? 2 : 1);
    tree_->header()->setStretchLastSection(!request.onToggled);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    if (request.onToggled) {
        tree_->header()->setSectionResizeMode(1, QHeaderView::Fixed);
        tree_->header()->resizeSection(1, SwitchIndicator::Width + 16);
    }
    searchBox_->clear();
    rebuildTree();
}

void SelectorWindow::rebuildTree(bool preserveState)
{
    if (request_.creation) {
        rebuildLocations(preserveState);
        return;
    }
    locationFolders_.clear();
    QSet<uint32_t> collapsedObjects;
    QSet<QString> collapsedBuckets;
    uint32_t currentID = request_.currentObjectID;
    const int scrollPosition = tree_->verticalScrollBar()->value();
    if (preserveState) {
        if (QTreeWidgetItem* current = tree_->currentItem())
            currentID = current->data(0, kRoleObjectID).toUInt();
        for (QTreeWidgetItemIterator it(tree_); *it; ++it) {
            const auto* item = *it;
            if (item->childCount() == 0 || item->isExpanded())
                continue;
            if (item->data(0, TreeRowStyle::kBucketRole).toBool())
                collapsedBuckets.insert(item->text(0));
            else
                collapsedObjects.insert(item->data(0, kRoleObjectID).toUInt());
        }
    }
    const QSignalBlocker blocker(tree_);
    tree_->clear();
    refDomainAdapter(request_.domain).populate(tree_, request_);
    tree_->expandAll();
    refreshSwitches();
    if (preserveState) {
        for (QTreeWidgetItemIterator it(tree_); *it; ++it) {
            auto* item = *it;
            if (item->data(0, TreeRowStyle::kBucketRole).toBool()
                    ? collapsedBuckets.contains(item->text(0))
                    : collapsedObjects.contains(item->data(0, kRoleObjectID).toUInt()))
                item->setExpanded(false);
        }
    }
    applySearchFilter(searchBox_->text());
    if (QTreeWidgetItem* currentRow = findSelectableRow(tree_, currentID)) {
        tree_->setCurrentItem(currentRow);
        if (!preserveState)
            tree_->scrollToItem(currentRow);
    }
    if (preserveState)
        tree_->verticalScrollBar()->setValue(scrollPosition);
}

void SelectorWindow::rebuildLocations(bool preserveState)
{
    const bool hadLocation = locationFolders_.contains(tree_->currentItem());
    const auto* selected = locationFolders_.value(tree_->currentItem());
    QSet<const PhysicalFolder*> collapsed;
    if (preserveState) {
        for (auto it = locationFolders_.cbegin(); it != locationFolders_.cend(); ++it)
            if (!it.key()->isExpanded()) collapsed.insert(it.value());
    }
    const QSignalBlocker blocker(tree_);
    tree_->clear();
    locationFolders_.clear();
    const auto* project = ProjectManager::instance().currentProject();
    const QString bucket = ItemTypeCatalog::folderNameOf(ItemTypeCatalog::bucketOf(request_.creation->type));
    if (project && project->projectId == creationProjectId_
        && ItemTypeCatalog::isFolderAddressable(request_.creation->type)
        && ProjectManager::instance().canCreateItem(nullptr, request_.creation->type, bucket)) {
        auto* root = TreeRowStyle::makeBucketRow(bucket);
        root->setFlags(root->flags() | Qt::ItemIsSelectable);
        root->setData(0, kRoleSelectable, true);
        root->setData(0, kRoleSearchPath, bucket);
        tree_->addTopLevelItem(root);
        locationFolders_.insert(root, nullptr);
        const auto addFolder = [&](auto&& self, const PhysicalFolder* folder, QTreeWidgetItem* parent) -> void {
            auto* row = TreeRowStyle::makeFolderRow(folder->name);
            row->setData(0, kRoleSelectable, true);
            row->setData(0, kRoleSearchPath, bucket + QLatin1Char('/') + project->folderRelPath(folder));
            parent->addChild(row);
            locationFolders_.insert(row, folder);
            for (const auto& child : folder->children) self(self, child.get(), row);
        };
        for (const auto* folder : project->folderRootsForBucket(bucket)) addFolder(addFolder, folder, root);
        for (auto it = locationFolders_.cbegin(); it != locationFolders_.cend(); ++it) {
            it.key()->setExpanded(!collapsed.contains(it.value()));
            if ((!preserveState && !it.value()) || (preserveState && hadLocation && it.value() == selected))
                tree_->setCurrentItem(it.key());
        }
    }
    applySearchFilter(searchBox_->text());
    refreshCreationValidation();
}

QString SelectorWindow::creationError() const
{
    const auto& creation = *request_.creation;
    auto& manager = ProjectManager::instance();
    const auto* project = manager.currentProject();
    if (!project || project->projectId != creationProjectId_)
        return tr("The project is no longer open.");
    const auto* row = tree_->currentItem();
    if (!row || !locationFolders_.contains(tree_->currentItem()))
        return tr("Select a location for the new object.");
    const auto* folder = locationFolders_.value(tree_->currentItem());
    const QString bucket = ItemTypeCatalog::folderNameOf(ItemTypeCatalog::bucketOf(creation.type));
    if (folder && project->folderBucket(folder) != bucket)
        return tr("The selected location is no longer available.");
    NameValidator::Result result;
    const QString name = nameEdit_->text();
    if (!NameValidator::validateItemName(name, creation.type, &result))
        return NameValidator::resultMessage(result);
    if (const auto* conflict = manager.findNameConflict(name, creation.type))
        return ProjectManager::nameConflictMessage(name, creation.type, *conflict);
    if (manager.suggestUniqueName(creation.type, name, nullptr, {}, nullptr, project->folderRelPath(folder))
        != name.normalized(QString::NormalizationForm_C))
        return tr("The name \"%1\" is already used at this location.").arg(name);
    return {};
}

void SelectorWindow::refreshCreationValidation()
{
    if (!request_.creation || creating_) return;
    const QString error = creationError();
    creationError_->setText(error);
    creationError_->setVisible(!error.isEmpty());
    nameEdit_->setToolTip(error);
    QtControlSkin::setSemanticProperty(nameEdit_, "inputInvalid", !error.isEmpty());
    createButton_->setEnabled(error.isEmpty());
}

void SelectorWindow::refreshSwitches()
{
    if (!request_.onToggled)
        return;
    const QSignalBlocker blocker(tree_);
    for (QTreeWidgetItemIterator it(tree_); *it; ++it) {
        auto* item = *it;
        if (!item->data(0, kRoleSelectable).toBool()) {
            item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
            continue;
        }
        const uint32_t objectID = item->data(0, kRoleObjectID).toUInt();
        item->setFlags(request_.canToggle && !request_.canToggle(objectID)
            ? item->flags() & ~Qt::ItemIsUserCheckable : item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(1, request_.checkState ? request_.checkState(objectID) : Qt::Checked);
    }
}

void SelectorWindow::applySearchFilter(const QString& text)
{
    const QString needle = text.trimmed();

    std::function<bool(QTreeWidgetItem*)> apply = [&](QTreeWidgetItem* item) -> bool {
        bool childVisible = false;
        for (int i = 0; i < item->childCount(); ++i)
            childVisible = apply(item->child(i)) || childVisible;

        const bool selfMatch = needle.isEmpty()
            || ((request_.creation || !item->data(0, TreeRowStyle::kBucketRole).toBool())
                && (item->text(0).contains(needle, Qt::CaseInsensitive)
                    || item->data(0, kRoleSearchPath).toString().contains(needle, Qt::CaseInsensitive)));
        const bool visible = selfMatch || childVisible;
        item->setHidden(!visible);
        if (!needle.isEmpty() && visible)
            item->setExpanded(true);
        return visible;
    };

    for (int i = 0; i < tree_->topLevelItemCount(); ++i)
        apply(tree_->topLevelItem(i));
}

void SelectorWindow::notifyItem(QTreeWidgetItem* item)
{
    if (!item || !item->data(0, kRoleSelectable).toBool())
        return;
    const uint32_t id = item->data(0, kRoleObjectID).toUInt();
    if (id == lastNotifiedID_)
        return;
    lastNotifiedID_ = id;
    const auto onSelected = request_.onSelected;
    if (onSelected)
        onSelected(id);
}

void SelectorWindow::accept()
{
    if (!isVisible())
        return;
    if (request_.creation) {
        if (creating_) return;
        refreshCreationValidation();
        if (!creationError().isEmpty()) return;
        const auto creation = *request_.creation;
        const QString folderPath = ProjectManager::instance().currentProject()->folderRelPath(
            locationFolders_.value(tree_->currentItem()));
        const QScopedValueRollback<bool> guard(creating_, true);
        const QString error = creation.onCreate(folderPath, nameEdit_->text());
        if (!error.isEmpty()) {
            creationError_->setText(error);
            creationError_->show();
            return;
        }
        BaseDialog::accept();
        return;
    }
    if (!request_.onToggled)
        notifyItem(tree_->currentItem());
    BaseDialog::accept();
}

bool SelectorWindow::event(QEvent* e)
{
    if (e->type() == QEvent::WindowDeactivate && isVisible() && !creating_)
        close();
    return BaseDialog::event(e);
}

}
