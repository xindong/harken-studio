
#include "ProjectTreeWidget.h"
#include "ProjectTreePanel.h"
#include "Widgets/Shared/ProjectItemMime.h"
#include "Widgets/Shared/TreeRowStyle.h"
#include "Core/ProjectManager.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/AssetPipeline/AudioImportGate.h"
#include "Core/ThemeManager.h"

#include <QMessageBox>
#include <QDrag>
#include <QDropEvent>
#include <QDragMoveEvent>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QPainter>
#include <QScrollBar>
#include <QFileInfo>
#include <QUrl>

#include <algorithm>

namespace VNX {

namespace {
constexpr int kBucketRole = TreeRowStyle::kBucketRole;

class PhysicalFolderMimeData final : public QMimeData
{
public:
    QList<PhysicalFolder*> folders;
};

bool canMoveFolder(const PhysicalFolder* folder, const PhysicalFolder* parent, const QString& bucket)
{
    const auto* project = ProjectManager::instance().currentProject();
    if (!project || project->folderBucket(folder) != bucket || folder->parent == parent)
        return false;
    for (auto* cursor = parent; cursor; cursor = cursor->parent)
        if (cursor == folder) return false;
    return true;
}
}

ProjectTreeWidget::ProjectTreeWidget(QWidget* parent)
    : TreeExpansionWidget(parent)
    , AudioDropTarget()
{
    setObjectName("ProjectTreeWidget");
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

    QScrollBar* scrollBar = verticalScrollBar();
    connect(scrollBar, &QScrollBar::rangeChanged, scrollBar,
            [scrollBar](int minimum, int maximum) {
                scrollBar->setEnabled(minimum < maximum);
            });
    scrollBar->setEnabled(scrollBar->minimum() < scrollBar->maximum());
}

bool ProjectTreeWidget::event(QEvent* event)
{
    if (event->type() == QEvent::ContextMenu
        && static_cast<QContextMenuEvent*>(event)->reason() == QContextMenuEvent::Keyboard) {
        QTreeWidgetItem* row = currentItem();
        if (row) scrollToItem(row);
        emit customContextMenuRequested(row ? visualItemRect(row).center() : QPoint(-1, -1));
        event->accept();
        return true;
    }
    return QTreeWidget::event(event);
}

QMimeData* ProjectTreeWidget::mimeData(const QList<QTreeWidgetItem*>& items) const
{
    auto* mime = new PhysicalFolderMimeData;
    if (!panel_) return mime;

    QList<uint32_t> ids;
    for (QTreeWidgetItem* treeItem : items) {
        ProjectItem* pi = panel_->projectItemForTreeItem(treeItem);
        if (pi && pi->objectID != 0)
            ids.append(pi->objectID);
        if (auto* folder = panel_->folderForTreeItem(treeItem))
            mime->folders.append(folder);
    }
    if (!ids.isEmpty()) ProjectItemMime::writeObjectIDs(mime, ids);
    if (!mime->folders.isEmpty()) mime->setData("application/x-harken-physical-folders", {});
    return mime;
}

void ProjectTreeWidget::startDrag(Qt::DropActions)
{
    const auto items = selectedItems();
    if (items.isEmpty()) return;
    QDrag drag(this);
    drag.setMimeData(mimeData(items));
    drag.setPixmap(items.first()->icon(0).pixmap(iconSize()));
    drag.exec(Qt::CopyAction | Qt::MoveAction, Qt::MoveAction);
}

QList<ProjectItem*> ProjectTreeWidget::draggedItems() const
{
    QList<ProjectItem*> items;
    if (typedDrag_ || !draggedObjectIDs_.isEmpty()) {
        for (uint32_t id : draggedObjectIDs_) {
            if (auto* item = ProjectManager::instance().findItemByObjectID(id))
                items.append(item);
        }
    } else if (panel_) {
        for (auto* row : selectedItems()) {
            if (auto* item = panel_->projectItemForTreeItem(row))
                items.append(item);
        }
    }
    const auto selected = items;
    const auto folders = draggedFolders();
    for (ProjectItem* item : selected) {
        auto* row = panel_->treeItemForProjectItem(item);
        for (auto* parent = row ? row->parent() : nullptr; parent; parent = parent->parent()) {
            if (selected.contains(panel_->projectItemForTreeItem(parent))
                || folders.contains(panel_->folderForTreeItem(parent))) {
                items.removeOne(item);
                break;
            }
        }
    }
    return items;
}

QList<PhysicalFolder*> ProjectTreeWidget::draggedFolders() const
{
    const auto* project = ProjectManager::instance().currentProject();
    if (!project || !panel_) return {};
    auto folders = typedDrag_ ? draggedFolders_ : panel_->selectedFolders();
    folders.removeIf([project](const PhysicalFolder* folder) {
        return project->folderBucket(folder).isEmpty();
    });
    const auto selected = folders;
    folders.removeIf([&](const PhysicalFolder* folder) {
        for (auto* parent = folder->parent; parent; parent = parent->parent)
            if (selected.contains(parent)) return true;
        return false;
    });
    return folders;
}

bool ProjectTreeWidget::canDropSelectionOn(ProjectItem* target) const
{
    if (!target || !panel_) return false;
    const auto dragged = draggedItems();
    if (dragged.isEmpty()) return false;

    for (ProjectItem* source : dragged) {
        if (ProjectTreePanel::wouldCreateCycle(source, target))
            continue;
        if (source->parent == target)
            continue;
        if (source == ProjectManager::instance().getMasterBusItem())
            continue;
        if (ProjectManager::canBeChildOf(source->type, target->type))
            return true;
    }
    return false;
}

bool ProjectTreeWidget::canDropSelectionIntoFolderRow(QTreeWidgetItem* row) const
{
    if (!row || !panel_) return false;

    const ProjectTreePanel::FolderRowAddress address = panel_->folderAddressForRow(row);
    if (!address.valid) return false;

    for (PhysicalFolder* source : draggedFolders())
        if (canMoveFolder(source, panel_->folderForTreeItem(row), address.bucketFolderName)) return true;

    for (ProjectItem* source : draggedItems()) {
        if (source == panel_->projectItemForTreeItem(row)) continue;
        if (ProjectTreePanel::canAddressItemAt(source, address)
            && (source->parent
                || ProjectManager::instance().currentProject()->itemFolderPath(source) != address.relPath))
            return true;
    }
    return false;
}

bool ProjectTreeWidget::canDropSelectionAtBucketRoot(const QString& bucketKey) const
{
    if (bucketKey.isEmpty() || !panel_) return false;
    if (bucketKey == QString::fromLatin1(ItemTypeCatalog::kBussesFolderName))
        return canDropSelectionOn(ProjectManager::instance().getMasterBusItem());

    const std::vector<ProjectItem::Type> allowed =
        ProjectManager::allowedTopLevelTypesForBucket(bucketKey);
    if (allowed.empty()) return false;

    const auto dragged = draggedItems();
    const auto folders = draggedFolders();
    if (dragged.isEmpty() && folders.isEmpty()) return false;

    bool anyMoved = false;
    for (PhysicalFolder* folder : folders) {
        if (ProjectManager::instance().currentProject()->folderBucket(folder) != bucketKey) return false;
        anyMoved |= folder->parent != nullptr;
    }
    for (ProjectItem* source : dragged) {
        if (source == ProjectManager::instance().getMasterBusItem())
            return false;
        if (std::find(allowed.begin(), allowed.end(), source->type) == allowed.end())
            return false;
        anyMoved |= source->parent != nullptr
            || (ItemTypeCatalog::isFolderAddressable(source->type)
                && ProjectManager::instance().currentProject()->folderForItem(source));
    }
    return anyMoved;
}

void ProjectTreeWidget::clearDropHighlight()
{
    if (highlightedItem_)
    {
        highlightedItem_ = nullptr;
        viewport()->update();
    }
}

void ProjectTreeWidget::setExternalDropHighlight(const QPoint& localPos, bool hasAudio)
{
    clearDropHighlight();

    if (!hasAudio || !panel_)
        return;

    QTreeWidgetItem* targetItem = itemAt(localPos);
    if (!targetItem)
    {
        return;
    }

    ProjectItem* target = panel_->projectItemForTreeItem(targetItem);
    if (AudioImportGate::canDropOnTarget(target))
    {
        highlightedItem_ = targetItem;
        viewport()->update();
    }
}

void ProjectTreeWidget::clearExternalDropHighlight()
{
    clearDropHighlight();
}

void ProjectTreeWidget::drawRow(QPainter* painter, const QStyleOptionViewItem& option,
                                const QModelIndex& index) const
{
    if (index.data(kBucketRole).toBool())
    {
        QStyleOptionViewItem opt = option;
        opt.font = qvariant_cast<QFont>(index.data(Qt::FontRole)).resolve(option.font);
        opt.fontMetrics = QFontMetrics(opt.font);
        TreeRowStyle::paintBucketRow(painter, opt, index, isExpanded(index));
    }
    else
    {
        QTreeWidget::drawRow(painter, option, index);
        if (selectionModel()->isSelected(index))
            TreeRowStyle::paintRowAccent(painter, option.rect);
    }

    if (highlightedItem_ && itemFromIndex(index) == highlightedItem_)
    {
        painter->save();
        painter->setCompositionMode(QPainter::CompositionMode_SourceOver);
        const auto& colors = ThemeManager::instance().colors();
        painter->fillRect(option.rect, colors.dropHighlight);
        painter->fillRect(QRect(option.rect.left(), option.rect.top(), 3, option.rect.height()),
                          colors.dropAccent);
        painter->restore();
    }
}

void ProjectTreeWidget::drawBranches(QPainter* painter, const QRect& rect,
                                     const QModelIndex& index) const
{
    if (index.data(kBucketRole).toBool())
        return;

    TreeRowStyle::paintBranches(painter, rect, index, model(), indentation(), isExpanded(index));
}

ProjectItem* ProjectTreeWidget::projectItemAt(const QPoint& localPos) const
{
    if (!panel_) return nullptr;
    QTreeWidgetItem* treeItem = itemAt(localPos);
    if (!treeItem) return nullptr;
    return panel_->projectItemForTreeItem(treeItem);
}

ContextTopic ProjectTreeWidget::contextTopicAt(const QPoint& pos) const
{
    const QPoint local = viewport()->mapFrom(this, pos);
    if (panel_) {
        if (const auto* folder = panel_->folderForTreeItem(itemAt(local))) {
            const auto* project = ProjectManager::instance().currentProject();
            return ContextTopic::forFolder(folder->name,
                project->folderBucket(folder) + QLatin1Char('/') + project->folderRelPath(folder));
        }
    }
    ProjectItem* item = projectItemAt(local);
    if (!item) return {};
    return ContextTopic::forObject(item->guid, item->type, item->name);
}

void ProjectTreeWidget::dragEnterEvent(QDragEnterEvent* event)
{
    draggedObjectIDs_.clear();
    draggedFolders_.clear();
    typedDrag_ = false;
    if (const auto* mime = dynamic_cast<const PhysicalFolderMimeData*>(event->mimeData()))
        draggedFolders_ = mime->folders;
    if (panel_ && (ProjectItemMime::hasObjectIDs(event->mimeData()) || !draggedFolders_.isEmpty())) {
        typedDrag_ = true;
        draggedObjectIDs_ = ProjectItemMime::readObjectIDs(event->mimeData());
        if (draggedItems().isEmpty() && draggedFolders().isEmpty()) {
            event->ignore();
        } else {
            event->setDropAction(Qt::MoveAction);
            event->accept();
        }
        return;
    }
    if (canAcceptAudioDrop(event->mimeData()))
    {
        event->setDropAction(Qt::CopyAction);
        event->accept();
        return;
    }
    if (event->mimeData()->hasUrls())
    {
        for (const QUrl& url : event->mimeData()->urls())
        {
            if (QFileInfo(url.toLocalFile()).isDir())
            {
                event->setDropAction(Qt::CopyAction);
                event->accept();
                return;
            }
        }
    }
    QTreeWidget::dragEnterEvent(event);
}

void ProjectTreeWidget::dragMoveEvent(QDragMoveEvent* event)
{
    clearDropHighlight();
    QTreeWidgetItem* targetItem = itemAt(event->position().toPoint());

    bool isExternalAudioDrag = canAcceptAudioDrop(event->mimeData());
    if (!isExternalAudioDrag && event->mimeData()->hasUrls())
    {
        for (const QUrl& url : event->mimeData()->urls())
        {
            if (QFileInfo(url.toLocalFile()).isDir())
            { isExternalAudioDrag = true; break; }
        }
    }
    if (event->mimeData()->hasUrls() && isExternalAudioDrag)
    {
        clearDropHighlight();

        if (!targetItem)
        {
            event->setDropAction(Qt::CopyAction);
            event->accept();
            setCursor(Qt::DragCopyCursor);
            return;
        }

        if (!panel_)
        {
            event->ignore();
            return;
        }

        if (targetItem->data(0, kBucketRole).toBool())
        {
            if (AudioImportGate::bucketAcceptsAudioDrop(
                    panel_->bucketKeyForTreeItem(targetItem)))
            {
                event->setDropAction(Qt::CopyAction);
                event->accept();
                highlightedItem_ = targetItem;
                viewport()->update();
                setCursor(Qt::DragCopyCursor);
            }
            else
            {
                event->ignore();
                setCursor(Qt::ForbiddenCursor);
            }
            return;
        }

        if (panel_->folderForTreeItem(targetItem)) {
            event->ignore();
            setCursor(Qt::ForbiddenCursor);
            return;
        }
        ProjectItem* target = panel_->projectItemForTreeItem(targetItem);
        if (AudioImportGate::canDropOnTarget(target))
        {
            event->setDropAction(Qt::CopyAction);
            event->accept();
            highlightedItem_ = targetItem;
            viewport()->update();
            setCursor(Qt::DragCopyCursor);
        }
        else
        {
            event->ignore();
            setCursor(Qt::ForbiddenCursor);
        }
        return;
    }

    if (!panel_)
    {
        QTreeWidget::dragMoveEvent(event);
        return;
    }

    ProjectItem* target = targetItem ? panel_->projectItemForTreeItem(targetItem)
                                     : nullptr;
    if (!target)
    {
        const bool canDrop = targetItem && targetItem->data(0, kBucketRole).toBool()
            ? canDropSelectionAtBucketRoot(panel_->bucketKeyForTreeItem(targetItem))
            : canDropSelectionIntoFolderRow(targetItem);
        if (canDrop)
        {
            event->setDropAction(Qt::MoveAction);
            event->accept();
            highlightedItem_ = targetItem;
            viewport()->update();
            unsetCursor();
        }
        else
        {
            event->ignore();
            setCursor(Qt::ForbiddenCursor);
        }
        return;
    }

    if (canDropSelectionOn(target) || canDropSelectionIntoFolderRow(targetItem))
    {
        event->setDropAction(Qt::MoveAction);
        event->accept();
        highlightedItem_ = targetItem;
        viewport()->update();
        unsetCursor();
    }
    else
    {
        event->ignore();
        setCursor(Qt::ForbiddenCursor);
    }
}

void ProjectTreeWidget::dragLeaveEvent(QDragLeaveEvent* event)
{
    draggedObjectIDs_.clear();
    draggedFolders_.clear();
    typedDrag_ = false;
    clearDropHighlight();
    unsetCursor();
    QTreeWidget::dragLeaveEvent(event);
}

void ProjectTreeWidget::dropEvent(QDropEvent* event)
{
    clearDropHighlight();
    unsetCursor();

    QTreeWidgetItem* targetItem = itemAt(event->position().toPoint());

    if (event->mimeData()->hasUrls())
    {
        bool hasAudioOrFolder = canAcceptAudioDrop(event->mimeData());

        if (!hasAudioOrFolder)
        {
            for (const QUrl& url : event->mimeData()->urls())
            {
                if (QFileInfo(url.toLocalFile()).isDir())
                {
                    hasAudioOrFolder = true;
                    break;
                }
            }
        }

        if (hasAudioOrFolder)
        {
            if (panel_ && panel_->folderForTreeItem(targetItem)) {
                event->ignore();
                return;
            }
            QStringList audioFiles = AudioImportGate::extractAudioFiles(event->mimeData());

            if (!audioFiles.isEmpty())
            {
                event->setDropAction(Qt::CopyAction);
                event->accept();
                ProjectItem* targetProjItem = panel_ ? panel_->projectItemForTreeItem(targetItem) : nullptr;
                const bool bucketRowTarget = targetItem && !targetProjItem
                                             && targetItem->data(0, kBucketRole).toBool();
                const QString bucketKey = (bucketRowTarget && panel_)
                    ? panel_->bucketKeyForTreeItem(targetItem)
                    : QString();
                emit externalFilesDropped(audioFiles, targetItem, bucketKey);
            }
            else
            {
                event->ignore();
            }
            return;
        }
    }

    const auto items = draggedItems();
    const auto folders = draggedFolders();

    if ((items.isEmpty() && folders.isEmpty()) || !panel_) {
        draggedObjectIDs_.clear();
        draggedFolders_.clear();
        typedDrag_ = false;
        event->ignore();
        return;
    }

    ProjectItem* targetProjItem = panel_->projectItemForTreeItem(targetItem);
    const ProjectTreePanel::FolderRowAddress folderTarget =
        panel_->folderAddressForRow(targetItem);
    const bool bucketRowTarget = targetItem && !targetProjItem
                                 && targetItem->data(0, kBucketRole).toBool();
    const QString bucketKey = bucketRowTarget ? panel_->bucketKeyForTreeItem(targetItem) : QString();
    const bool bucketDropAllowed = !bucketRowTarget || canDropSelectionAtBucketRoot(bucketKey);
    draggedObjectIDs_.clear();
    draggedFolders_.clear();
    typedDrag_ = false;
    if (!bucketDropAllowed) {
        event->ignore();
        return;
    }
    if (bucketKey == QString::fromLatin1(ItemTypeCatalog::kBussesFolderName))
        targetProjItem = ProjectManager::instance().getMasterBusItem();
    if (!targetProjItem && !bucketRowTarget && !folderTarget.valid) {
        event->ignore();
        return;
    }
    const std::vector<ProjectItem::Type> allowedAtRoot = bucketRowTarget
        ? ProjectManager::allowedTopLevelTypesForBucket(
              panel_->bucketKeyForTreeItem(targetItem))
        : std::vector<ProjectItem::Type>{};

    QStringList rejected;
    QList<ProjectItem*> acceptable;
    QList<PhysicalFolder*> acceptableFolders;
    PhysicalFolder* targetFolder = panel_->folderForTreeItem(targetItem);
    for (PhysicalFolder* folder : folders) {
        if (folderTarget.valid && canMoveFolder(folder, targetFolder, folderTarget.bucketFolderName))
            acceptableFolders.append(folder);
        else if (!folderTarget.valid || ProjectManager::instance().currentProject()->folderBucket(folder)
                     != folderTarget.bucketFolderName)
            rejected << tr("Folder '%1' cannot be moved to this location.").arg(folder->name);
    }
    for (ProjectItem* sourceProjItem : items) {
        if (sourceProjItem == targetProjItem
            || (targetProjItem && sourceProjItem->parent == targetProjItem))
            continue;
        if (sourceProjItem == ProjectManager::instance().getMasterBusItem()) {
            rejected << QString("'%1' is the fixed root of the Bus hierarchy and cannot be moved.")
                            .arg(sourceProjItem->name);
            continue;
        }
        if (ProjectTreePanel::canAddressItemAt(sourceProjItem, folderTarget)) {
            if (sourceProjItem->parent
                || ProjectManager::instance().currentProject()->itemFolderPath(sourceProjItem)
                != folderTarget.relPath)
                acceptable.append(sourceProjItem);
            continue;
        }
        if (bucketRowTarget && !targetProjItem) {
            if (!sourceProjItem->parent)
                continue;
            if (std::find(allowedAtRoot.begin(), allowedAtRoot.end(),
                          sourceProjItem->type) == allowedAtRoot.end()) {
                rejected << QString("'%1' cannot be placed at the top level of '%2'.")
                                .arg(sourceProjItem->name)
                                .arg(panel_->bucketKeyForTreeItem(targetItem));
                continue;
            }
            acceptable.append(sourceProjItem);
            continue;
        }
        if (!targetProjItem) {
            rejected << QString("'%1' cannot be placed in '%2'.")
                            .arg(sourceProjItem->name)
                            .arg(targetItem->text(0));
            continue;
        }
        if (!ProjectManager::canBeChildOf(sourceProjItem->type, targetProjItem->type)) {
            rejected << QString("'%1' cannot be a child of '%2'.")
                            .arg(sourceProjItem->name)
                            .arg(targetProjItem->name);
            continue;
        }
        QString structuralReason;
        if (sourceProjItem->parent != targetProjItem
            && !ProjectManager::instance().validateStructuralPlacement(
                   sourceProjItem->type, targetProjItem, &structuralReason, sourceProjItem)) {
            rejected << structuralReason;
            continue;
        }
        acceptable.append(sourceProjItem);
    }

    if (!rejected.isEmpty()) {
        QMessageBox::warning(this, tr("Cannot Move Item"),
            tr("The following moves were rejected by the hierarchy nesting rules:\n\n%1")
                .arg(rejected.join("\n")));
    }

    if (acceptable.isEmpty() && acceptableFolders.isEmpty()) {
        event->ignore();
        return;
    }

    if (folderTarget.valid && std::all_of(acceptable.cbegin(), acceptable.cend(),
            [&](const ProjectItem* item) { return ProjectTreePanel::canAddressItemAt(item, folderTarget); })) {
        if (ProjectManager::instance().moveEntriesToFolder(acceptable, acceptableFolders,
                folderTarget.bucketFolderName, folderTarget.relPath)) {
            event->setDropAction(Qt::MoveAction);
            event->accept();
        } else {
            event->ignore();
        }
        return;
    }

    if (ProjectManager::instance().moveItems(acceptable, targetProjItem)) {
        event->setDropAction(Qt::MoveAction);
        event->accept();
    } else {
        event->ignore();
    }
}

}
