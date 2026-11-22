#include "ResourcesTreeWidget.h"

#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/AssetPipeline/AssetImporter.h"
#include "Core/AssetPipeline/AudioImportGate.h"
#include "Core/AssetPipeline/ImportAssetsCommand.h"
#include "Core/AssetPipeline/SourceFileSystem.h"
#include "Core/BaseDialog.h"
#include "Core/ThemeManager.h"
#include "Core/QtControlSkin.h"
#include "Core/ShortcutManager.h"
#include "Widgets/LogPanel/LogPanel.h"
#include "Widgets/Common/ProportionalTreeWidget.h"
#include "Widgets/Common/StatusTip.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Shared/RefIdMime.h"
#include "Widgets/Shared/TreeRowStyle.h"
#include "Widgets/Shared/InlineRenameEdit.h"

#include <QDesktopServices>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>

namespace harken::studio {
namespace {

constexpr auto kResourcePathsMime = "application/x-harken-source-paths";

QStringList resourcePaths(const QMimeData* mime)
{
    if (!mime || !mime->hasFormat(QLatin1String(kResourcePathsMime)))
        return {};
    const auto object = QJsonDocument::fromJson(mime->data(QLatin1String(kResourcePathsMime))).object();
    if (object.value(QStringLiteral("projectRoot")).toString() != AssetRegistry::instance().projectRoot())
        return {};
    QStringList paths;
    for (const auto& value : object.value(QStringLiteral("paths")).toArray()) {
        const QString path = value.toString();
        if (path.isEmpty() || SourceFileSystem::absolutePath(path).isEmpty())
            return {};
        paths.append(path);
    }
    return paths;
}

}

ResourcesTreeWidget::ResourcesTreeWidget(QWidget* parent)
    : TreeExpansionWidget(parent)
{
    auto* delegate = new TreeRowStyle::ItemRowDelegate(this);
    delegate->setRenameEditorFactory([this](QWidget* parent, const QModelIndex& index) {
        const QString path = index.data(kResourcePathRole).toString();
        const QString displayName = index.data(Qt::DisplayRole).toString();
        const QString hiddenExtension = QFileInfo(path).fileName().mid(displayName.size());
        return new InlineRenameEdit(displayName,
            [this, path, displayName, hiddenExtension](const QString& name, bool commit) {
                if (!SourceFileSystem::validName(name)) return tr("Enter a valid file or folder name.");
                const QString fileName = name + hiddenExtension;
                QString error;
                if (!SourceFileSystem::validateRename(path, fileName, &error)) return error;
                if (commit && name != displayName) return renameResource(path, fileName);
                return error;
            }, parent);
    });
    setItemDelegate(delegate);
    setEditTriggers(QAbstractItemView::EditKeyPressed);
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        showContextMenu(itemAt(position), viewport()->mapToGlobal(position));
    });
    ShortcutManager::instance().bindWidgetAction(Shortcuts::Delete, this,
        [this]() { deleteCurrentFolder(); }, Qt::WidgetWithChildrenShortcut);
}

QString ResourcesTreeWidget::renameResource(const QString& path, const QString& name)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    const QString prefix = slash < 0 ? QString() : path.left(slash + 1);
    auto& registry = AssetRegistry::instance();
    const auto* source = registry.getAssetByPath(path);
    const auto* missing = registry.findAssetByPath(SourceFileSystem::absolutePath(prefix + name));
    QString error;
    if (source && missing && source->sourceID != missing->sourceID
        && !QFileInfo::exists(missing->absolutePath)) {
        const uint32_t sourceID = source->sourceID;
        const uint32_t missingID = missing->sourceID;
        const QString destination = missing->relativePath;
        const QString projectRoot = registry.projectRoot();
        BaseDialog dialog(this);
        dialog.setObjectName(QStringLiteral("resourceRenameRestoreDialog"));
        dialog.setWindowTitle(tr("Restore Missing Audio Resource"));
        auto* layout = new QVBoxLayout(&dialog);
        auto* summary = new QLabel(tr("'%1' is missing.\n\n"
            "Rename '%2' to '%1' and use its audio to restore the missing source references?\n"
            "Existing references to both files will use the renamed audio, keeping their "
            "ranges, loop settings and Active selections.").arg(destination, path), &dialog);
        summary->setWordWrap(true);
        layout->addWidget(summary);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        buttons->button(QDialogButtonBox::Ok)->setText(tr("Rename and Restore"));
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        if (dialog.exec() != QDialog::Accepted) return {};
        source = registry.getAsset(sourceID);
        missing = registry.getAsset(missingID);
        if (registry.projectRoot() != projectRoot || !source || !missing
            || source->relativePath != path || missing->relativePath != destination)
            return tr("The resources changed while the rename was being confirmed. Review them and try again.");
        if (SourceFileSystem::restoreByRename(sourceID, missingID, &error)) selectPath(destination);
    } else {
        QString renamedName;
        if (SourceFileSystem::rename(path, name, &error, &renamedName)) selectPath(prefix + renamedName);
    }
    return error;
}

QMimeData* ResourcesTreeWidget::mimeData(const QList<QTreeWidgetItem*>& items) const
{
    auto* mime = new QMimeData;
    QList<uint32_t> ids;
    QList<QUrl> urls;
    QJsonArray paths;
    for (auto* item : items) {
        if (!item)
            continue;
        bool ancestorSelected = false;
        for (auto* parent = item->parent(); parent; parent = parent->parent())
            ancestorSelected |= items.contains(parent);
        if (ancestorSelected) continue;
        const uint32_t id = item->data(0, kResourceIdRole).toUInt();
        if (id != 0)
            ids.append(id);
        const QString path = item->data(0, kResourcePathRole).toString();
        const QString absolute = SourceFileSystem::absolutePath(path);
        if (!path.isEmpty() && !absolute.isEmpty()) {
            paths.append(path);
            if (QFileInfo::exists(absolute))
                urls.append(QUrl::fromLocalFile(absolute));
        }
    }
    RefIdMime::write(mime, RefDomain::AudioAsset, ids);
    if (!urls.isEmpty())
        mime->setUrls(urls);
    if (!paths.isEmpty())
        mime->setData(QLatin1String(kResourcePathsMime), QJsonDocument(QJsonObject{
            {QStringLiteral("projectRoot"), AssetRegistry::instance().projectRoot()},
            {QStringLiteral("paths"), paths}}).toJson(QJsonDocument::Compact));
    return mime;
}

void ResourcesTreeWidget::startDrag(Qt::DropActions)
{
    const auto items = selectedItems();
    if (items.isEmpty())
        return;
    QDrag drag(this);
    drag.setMimeData(mimeData(items));
    drag.setPixmap(items.first()->icon(0).pixmap(iconSize()));
    drag.exec(Qt::CopyAction | Qt::MoveAction, Qt::CopyAction);
    stopDragNavigation();
    clearDropHighlight();
}

QString ResourcesTreeWidget::folderForItem(QTreeWidgetItem* item) const
{
    if (!item)
        return {};
    if (item->data(0, kResourceFolderRole).toBool())
        return item->data(0, kResourcePathRole).toString();
    return folderForItem(item->parent());
}

QString ResourcesTreeWidget::currentFolder() const
{
    return folderForItem(currentItem());
}

bool ResourcesTreeWidget::importDroppedAudio(const QMimeData* mimeData, const QString& folder)
{
    if (SourceFileSystem::absolutePath({}).isEmpty()
        || RefIdMime::has(mimeData, RefDomain::AudioAsset))
        return false;
    const QStringList files = AudioImportGate::extractAudioFiles(mimeData);
    if (files.isEmpty()) {
        showStatusTip(this, tr("No supported audio files were found in the dropped files or folders."));
        return false;
    }
    const QString projectRoot = AssetRegistry::instance().projectRoot();
    struct ImportGroup {
        QString relativePath;
        QString currentHash;
        QStringList files;
        QStringList hashes;
        bool exists = false;
        QTreeWidgetItem* row = nullptr;
        QComboBox* choices = nullptr;
    };
    QList<ImportGroup> groups;
    QMap<QString, qsizetype> indices;
    for (const QString& file : files) {
        QString relative = folder.isEmpty() ? QFileInfo(file).fileName()
            : folder + QLatin1Char('/') + QFileInfo(file).fileName();
        const QString destination = SourceFileSystem::absolutePath(relative);
        if (destination.isEmpty() || (!folder.isEmpty() && !QFileInfo(destination).dir().exists())
            || QFileInfo(destination).isDir()) {
            showStatusTip(this, tr("'%1' is not a valid destination for an audio file.").arg(relative));
            return false;
        }
        QString key = relative;
#ifdef Q_OS_WIN
        key = key.toCaseFolded();
#endif
        if (!indices.contains(key)) {
            ImportGroup group;
            const auto* asset = AssetRegistry::instance().findAssetByPath(destination);
            group.relativePath = asset ? asset->relativePath : relative;
            group.exists = QFileInfo::exists(destination);
            if (QFileInfo::exists(destination)) {
                group.currentHash = AssetImporter::computeContentHash(destination);
                if (group.currentHash.isEmpty()) {
                    showError(tr("Could not read '%1'.").arg(destination));
                    return false;
                }
            }
            indices.insert(key, groups.size());
            groups.append(group);
        }
        auto& group = groups[indices.value(key)];
        const QString hash = AssetImporter::computeContentHash(file);
        if (hash.isEmpty()) {
            showError(tr("Could not read '%1'.").arg(file));
            return false;
        }
        if (!group.hashes.contains(hash)) {
            group.files.append(file);
            group.hashes.append(hash);
        }
    }

    BaseDialog dialog(this);
    dialog.setObjectName(QStringLiteral("resourceImportConflictDialog"));
    dialog.setWindowTitle(tr("Import Audio Resources"));
    const auto& metrics = ThemeManager::instance().metrics().dialog;
    dialog.resize(metrics.wideWidth, metrics.preferredHeight);
    auto* layout = new QVBoxLayout(&dialog);
    auto* summary = new QLabel(tr("Choose which files to replace. Uncheck a file to skip it.\n"
        "When several dropped files have the same name, choose one incoming version."), &dialog);
    summary->setWordWrap(true);
    layout->addWidget(summary);
    auto* table = new TableWidget(TableOptions::display(), &dialog);
    auto* conflicts = table->createItemView(QStringLiteral("sourceImportConflicts"),
        {tr("Replace"), tr("Incoming file")}, {2, 3});
    layout->addWidget(table, 1);
    for (auto& group : groups) {
        if (group.files.size() == 1 && (!group.exists || group.hashes.first() == group.currentHash))
            continue;
        group.row = new QTreeWidgetItem(conflicts, {group.relativePath});
        group.row->setFlags(group.row->flags() | Qt::ItemIsUserCheckable);
        group.row->setCheckState(0, Qt::Checked);
        if (group.files.size() > 1) {
            group.choices = new QComboBox(conflicts);
            for (const auto& file : group.files)
                group.choices->addItem(QDir::toNativeSeparators(file));
            conflicts->setItemWidget(group.row, 1, group.choices);
        } else {
            const QString file = QDir::toNativeSeparators(group.files.first());
            group.row->setText(1, file);
            group.row->setToolTip(1, file);
        }
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Import"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (conflicts->topLevelItemCount() > 0 && dialog.exec() != QDialog::Accepted)
        return false;
    if (projectRoot != AssetRegistry::instance().projectRoot())
        return false;

    QStringList selected;
    QMap<QString, QString> replacements;
    for (const auto& group : groups) {
        if (group.row && group.row->checkState(0) != Qt::Checked)
            continue;
        const int choice = group.choices ? group.choices->currentIndex() : 0;
        const QString file = group.files[choice];
        const QString destination = SourceFileSystem::absolutePath(group.relativePath);
        const QString currentHash = QFileInfo::exists(destination)
            ? AssetImporter::computeContentHash(destination) : QString();
        if (currentHash != group.currentHash
            || AssetImporter::computeContentHash(file) != group.hashes[choice]) {
            showError(tr("'%1' changed while import was being confirmed. Import it again to review the changes.")
                .arg(group.relativePath));
            return false;
        }
        selected.append(file);
        if (group.exists && group.hashes[choice] != group.currentHash)
            replacements.insert(group.relativePath, file);
    }
    if (selected.isEmpty())
        return true;
    auto* command = ImportAssetsCommand::pushImportResources(selected, folder, replacements);
    return command->successCount() > 0 || command->duplicateCount() > 0;
}

bool ResourcesTreeWidget::moveDroppedResources(const QMimeData* mime, const QString& folder)
{
    const QStringList paths = resourcePaths(mime);
    if (paths.isEmpty())
        return false;
    QString error;
    const bool moved = SourceFileSystem::move(paths, folder, &error);
    if (moved) {
        clearSelection();
        setCurrentItem(nullptr);
        for (const QString& path : paths) {
            const QString destination = folder.isEmpty() ? QFileInfo(path).fileName()
                : folder + QLatin1Char('/') + QFileInfo(path).fileName();
            for (QTreeWidgetItemIterator it(this); *it; ++it) {
                if ((*it)->data(0, kResourcePathRole).toString() == destination) {
                    for (auto* parent = (*it)->parent(); parent; parent = parent->parent())
                        parent->setExpanded(true);
                    (*it)->setSelected(true);
                    setCurrentItem(*it, 0, QItemSelectionModel::NoUpdate);
                    break;
                }
            }
        }
        if (currentItem())
            scrollToItem(currentItem());
    }
    showError(error);
    return moved;
}

bool ResourcesTreeWidget::acceptsMime(const QMimeData* mime) const
{
    if (!mime || SourceFileSystem::absolutePath({}).isEmpty())
        return false;
    if (mime->hasFormat(QLatin1String(kResourcePathsMime)))
        return !resourcePaths(mime).isEmpty();
    if (RefIdMime::has(mime, RefDomain::AudioAsset))
        return false;
    if (AudioImportGate::mimeHasAudio(mime))
        return true;
    for (const auto& url : mime->urls())
        if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isDir())
            return true;
    return false;
}

Qt::DropAction ResourcesTreeWidget::dropActionFor(const QMimeData* mime, const QString& folder) const
{
    if (!acceptsMime(mime))
        return Qt::IgnoreAction;
    if (mime->hasFormat(QLatin1String(kResourcePathsMime)))
        return SourceFileSystem::canMove(resourcePaths(mime), folder) ? Qt::MoveAction : Qt::IgnoreAction;
    return Qt::CopyAction;
}

void ResourcesTreeWidget::setRootDropTarget(QWidget* target)
{
    rootDropTarget_ = target;
    target->setAcceptDrops(true);
    target->installEventFilter(this);
}

bool ResourcesTreeWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != rootDropTarget_)
        return QTreeWidget::eventFilter(watched, event);
    if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
        auto* drag = static_cast<QDragMoveEvent*>(event);
        const Qt::DropAction action = dropActionFor(drag->mimeData(), {});
        if (action == Qt::IgnoreAction) {
            drag->ignore();
        } else {
            drag->setDropAction(action);
            drag->accept();
        }
        QtControlSkin::setSemanticProperty(rootDropTarget_, "sourceDropTarget", drag->isAccepted());
        return true;
    }
    if (event->type() == QEvent::DragLeave) {
        clearDropHighlight();
        event->accept();
        return true;
    }
    if (event->type() == QEvent::Drop) {
        performDrop(static_cast<QDropEvent*>(event), {});
        return true;
    }
    return QTreeWidget::eventFilter(watched, event);
}

void ResourcesTreeWidget::clearDropHighlight()
{
    dropFolder_ = QPersistentModelIndex();
    if (rootDropTarget_)
        QtControlSkin::setSemanticProperty(rootDropTarget_, "sourceDropTarget", false);
    viewport()->update();
}

void ResourcesTreeWidget::dragEnterEvent(QDragEnterEvent* event)
{
    if (!acceptsMime(event->mimeData())) {
        event->ignore();
        return;
    }
    event->setDropAction(event->mimeData()->hasFormat(QLatin1String(kResourcePathsMime))
        ? Qt::MoveAction : Qt::CopyAction);
    event->accept();
}

void ResourcesTreeWidget::dragMoveEvent(QDragMoveEvent* event)
{
    auto* target = itemAt(event->position().toPoint());
    if (target && !target->data(0, kResourceFolderRole).toBool())
        target = target->parent();
    const QString folder = folderForItem(target);
    const Qt::DropAction action = dropActionFor(event->mimeData(), folder);
    if (action == Qt::IgnoreAction) {
        clearDropHighlight();
        event->ignore();
        return;
    }
    const QPersistentModelIndex index(indexFromItem(target));
    if (index != dropFolder_) {
        dropFolder_ = index;
        viewport()->update();
    }
    if (rootDropTarget_)
        QtControlSkin::setSemanticProperty(rootDropTarget_, "sourceDropTarget", !target);
    event->setDropAction(action);
    event->accept();
}

void ResourcesTreeWidget::dragLeaveEvent(QDragLeaveEvent* event)
{
    clearDropHighlight();
    QTreeWidget::dragLeaveEvent(event);
}

void ResourcesTreeWidget::dropEvent(QDropEvent* event)
{
    performDrop(event, folderForItem(itemAt(event->position().toPoint())));
}

void ResourcesTreeWidget::performDrop(QDropEvent* event, const QString& folder)
{
    clearDropHighlight();
    const Qt::DropAction action = dropActionFor(event->mimeData(), folder);
    if (action == Qt::IgnoreAction
        || !(action == Qt::MoveAction ? moveDroppedResources(event->mimeData(), folder)
                                     : importDroppedAudio(event->mimeData(), folder))) {
        event->ignore();
        return;
    }
    event->setDropAction(action);
    event->accept();
    emit contentRevealRequested();
}

void ResourcesTreeWidget::drawBranches(QPainter* painter, const QRect& rect, const QModelIndex& index) const
{
    TreeRowStyle::paintBranches(painter, rect, index, model(), indentation(), isExpanded(index),
                               rootIsDecorated());
}

void ResourcesTreeWidget::drawRow(QPainter* painter, const QStyleOptionViewItem& options,
                              const QModelIndex& index) const
{
    QTreeWidget::drawRow(painter, options, index);
    if (selectionModel()->isSelected(index))
        TreeRowStyle::paintRowAccent(painter, options.rect);
    if (index == dropFolder_)
        TreeRowStyle::paintDropHighlight(painter, options.rect);
}

void ResourcesTreeWidget::showError(const QString& message)
{
    if (!message.isEmpty())
        LogPanel::logError(LogCategory::File, message);
}

void ResourcesTreeWidget::selectPath(const QString& path)
{
    for (QTreeWidgetItemIterator it(this); *it; ++it) {
        if ((*it)->data(0, kResourcePathRole).toString() == path) {
            setCurrentItem(*it);
            scrollToItem(*it);
            return;
        }
    }
}

void ResourcesTreeWidget::newFolder()
{
    const QString parent = currentFolder();
    const QString absolute = SourceFileSystem::absolutePath(parent);
    if (absolute.isEmpty())
        return;
    QString error;
    QString name;
    if (SourceFileSystem::createFolder(parent, QStringLiteral("New Folder"), &error, &name)) {
        selectPath(parent.isEmpty() ? name : parent + QLatin1Char('/') + name);
        emit contentRevealRequested();
        const QPersistentModelIndex index(currentIndex());
        QTimer::singleShot(0, this, [this, index] {
            if (!index.isValid() || currentIndex() != index) return;
            setFocus(Qt::OtherFocusReason);
            editItem(itemFromIndex(index), 0);
        });
    }
    showError(error);
}

void ResourcesTreeWidget::renameCurrent()
{
    if (currentItem()) editItem(currentItem(), 0);
}

void ResourcesTreeWidget::deleteCurrentFolder()
{
    if (!currentItem() || !currentItem()->data(0, kResourceFolderRole).toBool())
        return;
    QString error;
    SourceFileSystem::removeEmptyFolder(currentItem()->data(0, kResourcePathRole).toString(), &error);
    showError(error);
}

void ResourcesTreeWidget::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_F2) {
        renameCurrent();
        event->accept();
    } else {
        QTreeWidget::keyPressEvent(event);
    }
}

void ResourcesTreeWidget::showRootContextMenu(const QPoint& globalPosition)
{
    showContextMenu(nullptr, globalPosition);
}

void ResourcesTreeWidget::showContextMenu(QTreeWidgetItem* item, const QPoint& globalPosition)
{
    if (item && !item->isSelected())
        setCurrentItem(item);
    else if (!item) {
        clearSelection();
        setCurrentItem(nullptr);
    }
    QMenu menu(this);
    menu.addAction(tr("New Folder"), this, &ResourcesTreeWidget::newFolder)
        ->setEnabled(!SourceFileSystem::absolutePath({}).isEmpty());
    if (item) {
        menu.addAction(tr("Rename"), this, &ResourcesTreeWidget::renameCurrent);
        if (item->data(0, kResourceFolderRole).toBool()) {
            const QString absolute = SourceFileSystem::absolutePath(item->data(0, kResourcePathRole).toString());
            menu.addAction(tr("Delete Folder"), this, &ResourcesTreeWidget::deleteCurrentFolder)
                ->setEnabled(!absolute.isEmpty() && QDir(absolute).isEmpty());
        }
        menu.addAction(tr("Move to Resources Root"), this, [this]() {
            QStringList paths;
            for (auto* selected : selectedItems())
                paths.append(selected->data(0, kResourcePathRole).toString());
            QString error;
            SourceFileSystem::move(paths, {}, &error);
            showError(error);
        });
    }
    menu.addSeparator();
    menu.addAction(tr("Expand All"), this, &QTreeWidget::expandAll);
    menu.addAction(tr("Collapse All"), this, &QTreeWidget::collapseAll);
    menu.addAction(tr("Open Containing Folder"), this, [this]() {
        const QString path = SourceFileSystem::absolutePath(currentFolder());
        if (!path.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    })->setEnabled(!SourceFileSystem::absolutePath({}).isEmpty());
    menu.exec(globalPosition);
}

}
