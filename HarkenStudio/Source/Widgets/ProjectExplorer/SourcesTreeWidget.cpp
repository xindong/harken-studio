#include "SourcesTreeWidget.h"

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

namespace VNX {
namespace {

constexpr auto kSourcePathsMime = "application/x-harken-source-paths";

QStringList sourcePaths(const QMimeData* mime)
{
    if (!mime || !mime->hasFormat(QLatin1String(kSourcePathsMime)))
        return {};
    const auto object = QJsonDocument::fromJson(mime->data(QLatin1String(kSourcePathsMime))).object();
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

SourcesTreeWidget::SourcesTreeWidget(QWidget* parent)
    : TreeExpansionWidget(parent)
{
    auto* delegate = new TreeRowStyle::ItemRowDelegate(this);
    delegate->setRenameEditorFactory([this](QWidget* parent, const QModelIndex& index) {
        const QString path = index.data(kSourcePathRole).toString();
        const QString original = QFileInfo(path).fileName();
        auto* editor = new InlineRenameEdit(original,
            [this, path, original](const QString& name, bool commit) {
                QString error;
                if (!SourceFileSystem::validateRename(path, name, &error)) return error;
                if (commit && name != original) return renameSource(path, name);
                return error;
            }, parent);
        return editor;
    });
    setItemDelegate(delegate);
    setEditTriggers(QAbstractItemView::EditKeyPressed);
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        showContextMenu(itemAt(position), viewport()->mapToGlobal(position));
    });
    ShortcutManager::instance().bindWidgetAction(Shortcuts::Delete, this,
        [this]() { deleteCurrentFolder(); }, Qt::WidgetWithChildrenShortcut);
    expandTimer_ = new QTimer(this);
    expandTimer_->setSingleShot(true);
    expandTimer_->setInterval(600);
    connect(expandTimer_, &QTimer::timeout, this, [this]() {
        if (dropFolder_.isValid())
            expand(dropFolder_);
    });
}

bool SourcesTreeWidget::edit(const QModelIndex& index, EditTrigger trigger, QEvent* event)
{
    if (!QTreeWidget::edit(index, trigger, event)) return false;
    if (!index.data(kSourceFolderRole).toBool()) {
        if (auto* editor = qobject_cast<InlineRenameEdit*>(indexWidget(index))) {
            const QString name = QFileInfo(index.data(kSourcePathRole).toString()).completeBaseName();
            editor->setSelection(0, name.size());
        }
    }
    return true;
}

QString SourcesTreeWidget::renameSource(const QString& path, const QString& name)
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
        dialog.setObjectName(QStringLiteral("sourceRenameRestoreDialog"));
        dialog.setWindowTitle(tr("Restore Missing Audio Source"));
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
            return tr("The sources changed while the rename was being confirmed. Review them and try again.");
        if (SourceFileSystem::restoreByRename(sourceID, missingID, &error)) selectPath(destination);
    } else {
        QString renamedName;
        if (SourceFileSystem::rename(path, name, &error, &renamedName)) selectPath(prefix + renamedName);
    }
    return error;
}

QMimeData* SourcesTreeWidget::mimeData(const QList<QTreeWidgetItem*>& items) const
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
        const uint32_t id = item->data(0, kSourceIdRole).toUInt();
        if (id != 0)
            ids.append(id);
        const QString path = item->data(0, kSourcePathRole).toString();
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
        mime->setData(QLatin1String(kSourcePathsMime), QJsonDocument(QJsonObject{
            {QStringLiteral("projectRoot"), AssetRegistry::instance().projectRoot()},
            {QStringLiteral("paths"), paths}}).toJson(QJsonDocument::Compact));
    return mime;
}

void SourcesTreeWidget::startDrag(Qt::DropActions)
{
    const auto items = selectedItems();
    if (items.isEmpty())
        return;
    QDrag drag(this);
    drag.setMimeData(mimeData(items));
    drag.setPixmap(items.first()->icon(0).pixmap(iconSize()));
    drag.exec(Qt::CopyAction | Qt::MoveAction, Qt::CopyAction);
}

QString SourcesTreeWidget::folderForItem(QTreeWidgetItem* item) const
{
    if (!item)
        return {};
    if (item->data(0, kSourceFolderRole).toBool())
        return item->data(0, kSourcePathRole).toString();
    return folderForItem(item->parent());
}

QString SourcesTreeWidget::currentFolder() const
{
    return folderForItem(currentItem());
}

bool SourcesTreeWidget::importDroppedAudio(const QMimeData* mimeData, const QString& folder)
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
    dialog.setObjectName(QStringLiteral("sourceImportConflictDialog"));
    dialog.setWindowTitle(tr("Import Audio Sources"));
    dialog.resize(760, 400);
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
    auto* command = ImportAssetsCommand::pushImportSources(selected, folder, replacements);
    return command->successCount() > 0 || command->duplicateCount() > 0;
}

bool SourcesTreeWidget::moveDroppedSources(const QMimeData* mime, const QString& folder)
{
    const QStringList paths = sourcePaths(mime);
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
                if ((*it)->data(0, kSourcePathRole).toString() == destination) {
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

bool SourcesTreeWidget::acceptsMime(const QMimeData* mime) const
{
    if (!mime || SourceFileSystem::absolutePath({}).isEmpty())
        return false;
    if (mime->hasFormat(QLatin1String(kSourcePathsMime)))
        return !sourcePaths(mime).isEmpty();
    if (RefIdMime::has(mime, RefDomain::AudioAsset))
        return false;
    if (AudioImportGate::mimeHasAudio(mime))
        return true;
    for (const auto& url : mime->urls())
        if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isDir())
            return true;
    return false;
}

Qt::DropAction SourcesTreeWidget::dropActionFor(const QMimeData* mime, const QString& folder) const
{
    if (!acceptsMime(mime))
        return Qt::IgnoreAction;
    if (mime->hasFormat(QLatin1String(kSourcePathsMime)))
        return SourceFileSystem::canMove(sourcePaths(mime), folder) ? Qt::MoveAction : Qt::IgnoreAction;
    return Qt::CopyAction;
}

void SourcesTreeWidget::setRootDropTarget(QWidget* target)
{
    rootDropTarget_ = target;
    target->setAcceptDrops(true);
    target->installEventFilter(this);
}

bool SourcesTreeWidget::eventFilter(QObject* watched, QEvent* event)
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

void SourcesTreeWidget::clearDropHighlight()
{
    dropFolder_ = QPersistentModelIndex();
    expandTimer_->stop();
    if (rootDropTarget_)
        QtControlSkin::setSemanticProperty(rootDropTarget_, "sourceDropTarget", false);
    viewport()->update();
}

void SourcesTreeWidget::dragEnterEvent(QDragEnterEvent* event)
{
    if (!acceptsMime(event->mimeData())) {
        event->ignore();
        return;
    }
    event->setDropAction(event->mimeData()->hasFormat(QLatin1String(kSourcePathsMime))
        ? Qt::MoveAction : Qt::CopyAction);
    event->accept();
}

void SourcesTreeWidget::dragMoveEvent(QDragMoveEvent* event)
{
    auto* target = itemAt(event->position().toPoint());
    if (target && !target->data(0, kSourceFolderRole).toBool())
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
        expandTimer_->start();
        viewport()->update();
    }
    if (rootDropTarget_)
        QtControlSkin::setSemanticProperty(rootDropTarget_, "sourceDropTarget", !target);
    event->setDropAction(action);
    event->accept();
}

void SourcesTreeWidget::dragLeaveEvent(QDragLeaveEvent* event)
{
    clearDropHighlight();
    QTreeWidget::dragLeaveEvent(event);
}

void SourcesTreeWidget::dropEvent(QDropEvent* event)
{
    performDrop(event, folderForItem(itemAt(event->position().toPoint())));
}

void SourcesTreeWidget::performDrop(QDropEvent* event, const QString& folder)
{
    clearDropHighlight();
    const Qt::DropAction action = dropActionFor(event->mimeData(), folder);
    if (action == Qt::IgnoreAction
        || !(action == Qt::MoveAction ? moveDroppedSources(event->mimeData(), folder)
                                     : importDroppedAudio(event->mimeData(), folder))) {
        event->ignore();
        return;
    }
    event->setDropAction(action);
    event->accept();
    emit contentRevealRequested();
}

void SourcesTreeWidget::drawBranches(QPainter* painter, const QRect& rect, const QModelIndex& index) const
{
    TreeRowStyle::paintBranches(painter, rect, index, model(), indentation(), isExpanded(index),
                               rootIsDecorated());
}

void SourcesTreeWidget::drawRow(QPainter* painter, const QStyleOptionViewItem& options,
                              const QModelIndex& index) const
{
    QTreeWidget::drawRow(painter, options, index);
    if (selectionModel()->isSelected(index))
        TreeRowStyle::paintRowAccent(painter, options.rect);
    if (index == dropFolder_) {
        painter->save();
        painter->fillRect(options.rect, ThemeManager::instance().colors().dropHighlight);
        painter->fillRect(QRect(options.rect.left(), options.rect.top(), 3, options.rect.height()),
                          ThemeManager::instance().colors().dropAccent);
        painter->restore();
    }
}

void SourcesTreeWidget::showError(const QString& message)
{
    if (!message.isEmpty())
        LogPanel::logError(LogCategory::File, message);
}

void SourcesTreeWidget::selectPath(const QString& path)
{
    for (QTreeWidgetItemIterator it(this); *it; ++it) {
        if ((*it)->data(0, kSourcePathRole).toString() == path) {
            setCurrentItem(*it);
            scrollToItem(*it);
            return;
        }
    }
}

void SourcesTreeWidget::newFolder()
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
    }
    showError(error);
}

void SourcesTreeWidget::renameCurrent()
{
    if (currentItem()) editItem(currentItem(), 0);
}

void SourcesTreeWidget::deleteCurrentFolder()
{
    if (!currentItem() || !currentItem()->data(0, kSourceFolderRole).toBool())
        return;
    QString error;
    SourceFileSystem::removeEmptyFolder(currentItem()->data(0, kSourcePathRole).toString(), &error);
    showError(error);
}

void SourcesTreeWidget::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_F2) {
        renameCurrent();
        event->accept();
    } else {
        QTreeWidget::keyPressEvent(event);
    }
}

void SourcesTreeWidget::showRootContextMenu(const QPoint& globalPosition)
{
    showContextMenu(nullptr, globalPosition);
}

void SourcesTreeWidget::showContextMenu(QTreeWidgetItem* item, const QPoint& globalPosition)
{
    if (item && !item->isSelected())
        setCurrentItem(item);
    else if (!item) {
        clearSelection();
        setCurrentItem(nullptr);
    }
    QMenu menu(this);
    menu.addAction(tr("New Folder"), this, &SourcesTreeWidget::newFolder)
        ->setEnabled(!SourceFileSystem::absolutePath({}).isEmpty());
    if (item) {
        menu.addAction(tr("Rename"), this, &SourcesTreeWidget::renameCurrent);
        if (item->data(0, kSourceFolderRole).toBool()) {
            const QString absolute = SourceFileSystem::absolutePath(item->data(0, kSourcePathRole).toString());
            menu.addAction(tr("Delete Folder"), this, &SourcesTreeWidget::deleteCurrentFolder)
                ->setEnabled(!absolute.isEmpty() && QDir(absolute).isEmpty());
        }
        menu.addAction(tr("Move to Sources Root"), this, [this]() {
            QStringList paths;
            for (auto* selected : selectedItems())
                paths.append(selected->data(0, kSourcePathRole).toString());
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
