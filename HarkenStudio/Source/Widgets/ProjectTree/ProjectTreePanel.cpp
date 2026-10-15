
#include "ProjectTreePanel.h"
#include "Widgets/Shared/ProjectObjectLabels.h"
#include "Core/ThemeAssetCatalog.h"
#include "Widgets/Shared/ProjectItemMenuController.h"
#include "Widgets/Shared/TreeRowPing.h"
#include "Widgets/Shared/TreeRowStyle.h"
#include "Widgets/Shared/UiHiddenBucket.h"
#include "Core/ActionStepUtils.h"
#include "Core/AuthoringPolicy.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/ItemTypeIcons.h"
#include "Core/NameValidator.h"
#include "Core/LanguageContextService.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/ShortcutManager.h"
#include "Core/ThemeManager.h"
#include "Core/Components/VBus.h"
#include "Core/Components/VMixNode.h"
#include "Core/Components/VSound.h"

#include "Widgets/Shared/InlineRenameEdit.h"
#include <QMessageBox>
#include <QStyle>
#include <QApplication>
#include <QDebug>
#include <QProgressDialog>
#include <QColorDialog>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <functional>
#include <vector>

#include "Core/AssetPipeline/ImportAssetsCommand.h"
#include "Core/AssetPipeline/ImportSoundSourcesCommand.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/AssetPipeline/AudioImportGate.h"
#include "Core/AssetPipeline/MediaPool.h"
#include "Core/SoundSourceEditing.h"
#include "Core/UndoCommands.h"
#include "Core/AppSettings.h"

namespace VNX {

namespace {
constexpr int kBucketRole = TreeRowStyle::kBucketRole;

QString actionStepMenuLabel(VActionStep::Type type)
{
    switch (type) {
        case VActionStep::Type::Play:               return QStringLiteral("Play");
        case VActionStep::Type::Stop:               return QStringLiteral("Stop");
        case VActionStep::Type::SetVariable:           return QStringLiteral("Set Variable");
        case VActionStep::Type::PostAction:          return QStringLiteral("Trigger Action");
        default: {
            VActionStep action;
            action.type = type;
            return action.typeString();
        }
    }
}

constexpr VActionStep::Type kNewActionSteps[] = {
    VActionStep::Type::Play,
    VActionStep::Type::Stop,
    VActionStep::Type::SetVariable,
    VActionStep::Type::PostAction,
};


}

ProjectTreePanel::ProjectTreePanel(PanelId settingsPanelId, QWidget* parent)
    : QWidget(parent)
    , settingsPanelId_(settingsPanelId)
{
}

ProjectTreePanel::~ProjectTreePanel() = default;

void ProjectTreePanel::initPanel()
{
    setupUI();

    connect(&ProjectManager::instance(), &ProjectManager::projectOpened, this, &ProjectTreePanel::onProjectOpened);
    connect(&ProjectManager::instance(), &ProjectManager::projectClosed, this, &ProjectTreePanel::onProjectClosed);
    connect(&ProjectManager::instance(), &ProjectManager::projectSaved, this, &ProjectTreePanel::onProjectSaved);
    connect(&UndoRedoManager::instance(), &UndoRedoManager::indexChanged,
            this, &ProjectTreePanel::updateBucketRows);
    connect(&ProjectManager::instance(), &ProjectManager::itemCreated, this, &ProjectTreePanel::onItemCreated);
    connect(&ProjectManager::instance(), &ProjectManager::itemRenamed, this, &ProjectTreePanel::onItemRenamed);
    connect(&ProjectManager::instance(), &ProjectManager::itemDeleted, this, &ProjectTreePanel::onItemDeleted);
    connect(&ProjectManager::instance(), &ProjectManager::itemMoved, this, &ProjectTreePanel::onItemMoved);
    connect(&ProjectManager::instance(), &ProjectManager::itemPropertyChanged, this, &ProjectTreePanel::onItemPropertyChanged);
    connect(&ProjectManager::instance(), &ProjectManager::folderCreated, this, &ProjectTreePanel::onFolderCreated);
    connect(&ProjectManager::instance(), &ProjectManager::folderDeleted, this, &ProjectTreePanel::onFolderDeleted);
    connect(&ProjectManager::instance(), &ProjectManager::folderMoved, this, [this](PhysicalFolder*) {
        updateFolderExpansionPaths();
        rebuildTree();
    });
    connect(&ProjectManager::instance(), &ProjectManager::folderRenamed, this,
            [this](PhysicalFolder*, const QString&) {
                updateFolderExpansionPaths();
                applyVisibility();
            });
    const auto scheduleVisibilityRefresh = [this]() {
        if (visibilityRefreshPending_) return;
        visibilityRefreshPending_ = true;
        QTimer::singleShot(0, this, [this]() {
            visibilityRefreshPending_ = false;
            applyVisibility();
        });
    };
    connect(&ProjectManager::instance(), &ProjectManager::itemCreated, this, scheduleVisibilityRefresh);
    connect(&ProjectManager::instance(), &ProjectManager::itemDeleted, this, scheduleVisibilityRefresh);
    connect(&ProjectManager::instance(), &ProjectManager::itemMoved, this, scheduleVisibilityRefresh);
    connect(&LanguageContextService::instance(), &LanguageContextService::languageChanged,
            this, [this]() {
        for (auto it = itemMap_.cbegin(); it != itemMap_.cend(); ++it) {
            const auto* sound = it.value()->comp<VSound>();
            if (sound && sound->distinguishLanguage)
                updateTreeItem(it.key(), it.value());
        }
    });

    connect(&MediaPool::instance(), &MediaPool::statusCacheRefreshed,
            this, [this]() {
        for (auto it = itemMap_.cbegin(); it != itemMap_.cend(); ++it)
            if (it.value()->type == ProjectItem::Type::Sound)
                updateTreeItem(it.key(), it.value());
    }, Qt::QueuedConnection);

    connect(&ProjectManager::instance(), &ProjectManager::busChanged,
            this, [this](uint32_t busID, const QString& propertyName, const QObject*) {
        if (auto* item = ProjectManager::instance().findBusItem(busID))
            onItemPropertyChanged(item, propertyName);
        if ((propertyName == QLatin1String("send")
             || propertyName == QLatin1String("sends")) && treeWidget_)
            treeWidget_->viewport()->update();
    });
    connect(&AppSettings::instance(), &AppSettings::fontSizeChanged,
            this, &ProjectTreePanel::onFontSettingsChanged);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this]() {
                const int extent = ThemeManager::instance().metrics().objectIconExtent;
                treeWidget_->setIconSize(QSize(extent, extent));
                for (auto it = itemMap_.cbegin(); it != itemMap_.cend(); ++it)
                    updateTreeItem(it.key(), it.value());
                for (auto it = folderMap_.cbegin(); it != folderMap_.cend(); ++it)
                    updateFolderRow(it.key(), it.value());
                for (QTreeWidgetItem* folder : std::as_const(categoryFolders_)) {
                    if (folder)
                        folder->setIcon(0, ThemeManager::instance().resolveToggleIcon(
                            QString::fromLatin1(ThemeAssetIds::TreeFolderOpen),
                            QString::fromLatin1(ThemeAssetIds::TreeFolderClosed)).icon);
                }
                if (treeWidget_)
                    treeWidget_->viewport()->update();
            });

    int fontSize = AppSettings::instance().panelFontSize(settingsPanelId_);
    QFont font = treeWidget_->font();
    font.setPointSize(fontSize);
    treeWidget_->setFont(font);

    if (ProjectManager::instance().isProjectOpen())
    {
        onProjectOpened(ProjectManager::instance().currentProject());
    }
}

void ProjectTreePanel::setupUI()
{
    searchBox_ = new SearchField();
    searchBox_->setObjectName("projectSearchBox");
    connect(searchBox_, &QLineEdit::textChanged, this, &ProjectTreePanel::onSearchTextChanged);

    setupTree();

    setupPanelChrome();

    setupContextMenu();
}

void ProjectTreePanel::setupTree()
{
    treeWidget_ = new ProjectTreeWidget();
    treeWidget_->setPanel(this);
    treeWidget_->setHeaderHidden(true);
    treeWidget_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    treeWidget_->setDragEnabled(true);
    treeWidget_->setAcceptDrops(true);
    treeWidget_->setDropIndicatorShown(true);
    treeWidget_->setDragDropMode(QAbstractItemView::DragDrop);
    treeWidget_->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(treeWidget_, &ProjectTreeWidget::internalMoveRequested,
            this, &ProjectTreePanel::onInternalMoveRequested);

    connect(treeWidget_, &ProjectTreeWidget::externalFilesDropped,
            this, &ProjectTreePanel::onExternalFilesDropped);
    treeWidget_->setAnimated(false);
    const int iconExtent = ThemeManager::instance().metrics().objectIconExtent;
    treeWidget_->setIconSize(QSize(iconExtent, iconExtent));
    auto* delegate = new TreeRowStyle::ItemRowDelegate(treeWidget_);
    delegate->setRenameEditorFactory([this](QWidget* parent, const QModelIndex& index) {
        auto* row = treeWidget_->itemFromIndex(index);
        if (auto* folder = folderMap_.value(row))
            return new InlineRenameEdit(folder->name, InlineRenameEdit::folderHandler(folder), parent);
        auto* item = itemMap_.value(row, nullptr);
        return item ? new InlineRenameEdit(item->name,
            InlineRenameEdit::projectItemHandler(item->guid), parent) : nullptr;
    });
    treeWidget_->setItemDelegate(delegate);
    treeWidget_->setEditTriggers(QAbstractItemView::EditKeyPressed);
    treeWidget_->setExpandsOnDoubleClick(false);
    treeWidget_->setRootIsDecorated(false);

    connect(treeWidget_, &QTreeWidget::itemClicked, this, &ProjectTreePanel::onItemClicked);
    connect(treeWidget_, &QTreeWidget::itemDoubleClicked, this, &ProjectTreePanel::onItemDoubleClicked);
    connect(treeWidget_, &QTreeWidget::itemExpanded, this, &ProjectTreePanel::onItemExpanded);
    connect(treeWidget_, &QTreeWidget::itemCollapsed, this, &ProjectTreePanel::onItemCollapsed);
    connect(treeWidget_, &QTreeWidget::customContextMenuRequested, this, &ProjectTreePanel::onContextMenu);
    connect(treeWidget_, &QTreeWidget::itemSelectionChanged, this, &ProjectTreePanel::onSelectionChanged);
}

void ProjectTreePanel::setupContextMenu()
{
    contextMenu_ = new QMenu(this);

    ProjectItemMenuController::Callbacks cb;
    cb.getSelected     = [this]() { return selectedItem(); };
    cb.getSelection    = [this]() { return selectedItems(); };
    cb.selectResult    = [this](ProjectItem* item) { selectProjectItem(item); };
    cb.selectFolderResult = [this](PhysicalFolder* folder) { selectFolder(folder); };
    cb.folderPath = [this](const QString& bucketFolderName) {
        return deriveFolderPathForBucket(bucketFolderName);
    };
    cb.currentBucketName = [this]() -> QString {
        QTreeWidgetItem* curItem = treeWidget_ ? treeWidget_->currentItem() : nullptr;
        return curItem ? folderAddressForRow(curItem).bucketFolderName : QString();
    };
    menuController_ = std::make_unique<ProjectItemMenuController>(std::move(cb), this);
    menuController_->buildNewChildMenu(contextMenu_);

    contextMenu_->addSeparator();

    buildItemMenu();

    ShortcutManager& shortcuts = ShortcutManager::instance();
    shortcuts.bindWidgetAction(Shortcuts::Cut, treeWidget_, [this]() { onCut(); },
                               Qt::WidgetWithChildrenShortcut);
    shortcuts.bindWidgetAction(Shortcuts::Copy, treeWidget_, [this]() { onCopy(); },
                               Qt::WidgetWithChildrenShortcut);
    shortcuts.bindWidgetAction(Shortcuts::Paste, treeWidget_, [this]() { onPaste(); },
                               Qt::WidgetWithChildrenShortcut);
    shortcuts.bindWidgetAction(Shortcuts::Delete, treeWidget_, [this]() { onDelete(); },
                               Qt::WidgetWithChildrenShortcut);
    shortcuts.bindWidgetAction(Shortcuts::Duplicate, treeWidget_,
                               [this]() { onDuplicate(); },
                               Qt::WidgetWithChildrenShortcut);
}

void ProjectTreePanel::buildItemMenu()
{
    using PT   = ProjectItem::Type;
    using Spec = ProjectItemMenuController::StandardItemSpec;
    using Ctx  = ProjectItemMenuController::MenuContext;
    auto anyTarget = [this](auto matches) {
        return [this, matches](const Ctx& ctx) {
            const auto items = resolveMenuItems(ctx.selectionGuids);
            return std::any_of(items.begin(), items.end(), matches);
        };
    };
    const auto hasObjects = [this](const Ctx& ctx) {
        return !resolveMenuItems(ctx.selectionGuids).isEmpty() || !menuFolders().isEmpty();
    };
    const auto hasClipboardObjects = anyTarget(&ProjectManager::isClipboardEligible);
    const auto hasEditors = anyTarget([](const ProjectItem*) { return true; });

    std::vector<Spec> stdItems;

    {
        Spec s;
        s.label = tr("New Parent");
        s.buildSubmenu = [this](QMenu* sub, const Ctx& ctx) {
            const auto items = resolveMenuItems(ctx.selectionGuids);
            for (PT c : newParentCandidateTypes(items)) {
                QAction* action = sub->addAction(ProjectItemMenuController::typeDisplayName(c), this,
                                                 [this, c]() { onNewParent(c); });
                action->setEnabled(menuFolders().isEmpty()
                    && ProjectManager::instance().canCreateParentContainer(items, c));
            }
        };
        stdItems.push_back(std::move(s));
    }

    {
        Spec s;
        s.label = tr("New Action");
        s.buildSubmenu = [this](QMenu* sub, const Ctx& ctx) {
            const auto items = resolveMenuItems(ctx.selectionGuids);
            auto& pm = ProjectManager::instance();
            for (VActionStep::Type t : kNewActionSteps) {
                const auto& types = EditorUtils::actionStepTargetTypes(t);
                if (std::none_of(items.begin(), items.end(), [&](const ProjectItem* item) {
                        return std::find(types.begin(), types.end(), item->type) != types.end();
                    })) continue;
                QAction* action = sub->addAction(actionStepMenuLabel(t), this,
                                                 [this, t]() { onNewAction(t); });
                action->setEnabled(menuFolders().isEmpty() && pm.canCreateActionsTargeting(items, t));
            }
        };
        s.separatorAfter = true;
        stdItems.push_back(std::move(s));
    }

    {
        Spec s;
        s.label = tr("Duplicate");
        s.shortcutHintId = Shortcuts::Duplicate;
        s.canShow = hasClipboardObjects;
        s.canEnable = [this](const Ctx&) {
            return menuFolders().isEmpty() && ProjectManager::instance().canDuplicateItems(uniqueMenuItems());
        };
        s.handler = [this]() { onDuplicate(); };
        stdItems.push_back(std::move(s));
    }

    {
        Spec s;
        s.label = tr("Rename");
        s.canShow = hasObjects;
        s.canEnable = [this](const Ctx& ctx) { return hasSingleObjectTarget(ctx) || hasSingleFolderTarget(ctx); };
        s.handler = [this]() { onRename(); };
        stdItems.push_back(std::move(s));
    }

    {
        Spec s;
        s.label = tr("Delete");
        s.shortcutHintId = Shortcuts::Delete;
        s.canShow = hasObjects;
        s.canEnable = [this](const Ctx&) {
            const auto items = uniqueMenuItems();
            return ProjectManager::instance().canDeleteEntries(items, menuFolders());
        };
        s.handler = [this]() { onDelete(); };
        s.separatorAfter = true;
        stdItems.push_back(std::move(s));
    }

    {
        Spec s;
        s.label = tr("Cut");
        s.shortcutHintId = Shortcuts::Cut;
        s.canShow = hasClipboardObjects;
        s.canEnable = [this](const Ctx&) {
            return menuFolders().isEmpty() && ProjectManager::instance().canSetClipboard(uniqueMenuItems());
        };
        s.handler = [this]() { onCut(); };
        stdItems.push_back(std::move(s));
    }
    {
        Spec s;
        s.label = tr("Copy");
        s.shortcutHintId = Shortcuts::Copy;
        s.canShow = hasClipboardObjects;
        s.canEnable = [this](const Ctx&) {
            return menuFolders().isEmpty() && ProjectManager::instance().canSetClipboard(uniqueMenuItems());
        };
        s.handler = [this]() { onCopy(); };
        stdItems.push_back(std::move(s));
    }
    {
        Spec s;
        s.label = tr("Paste");
        s.shortcutHintId = Shortcuts::Paste;
        s.canShow = [hasDestination = anyTarget([](const ProjectItem* item) {
            return ItemTypeCatalog::bucketOf(item->type) == ItemTypeCatalog::Bucket::Layers;
        })](const Ctx& ctx) {
            if (ctx.kind == ProjectItemMenuController::ContextKind::BucketRoot) {
                const auto* bucket = ItemTypeCatalog::bucketFactsForFolder(ctx.bucketFolderName);
                return bucket && bucket->bucket != ItemTypeCatalog::Bucket::Busses;
            }
            return hasDestination(ctx);
        };
        s.canEnable = [this](const Ctx& ctx) { return canPasteAtContext(ctx); };
        s.handler = [this]() { onPaste(); };
        stdItems.push_back(std::move(s));
    }
    {
        Spec s;
        s.label = tr("Paste Properties");
        s.canShow = anyTarget([](const ProjectItem* item) {
            return ProjectManager::isClipboardEligible(item) && item->comp<VMixNode>();
        });
        s.canEnable = [this](const Ctx&) {
            return menuFolders().isEmpty()
                && ProjectManager::instance().canPastePropertiesFromClipboard(uniqueMenuItems());
        };
        s.handler = [this]() { onPasteProperties(); };
        s.separatorAfter = true;
        stdItems.push_back(std::move(s));
    }

    {
        Spec s;
        s.label = tr("Open Containing Folder");
        s.canShow = anyTarget([](const ProjectItem* item) { return item->type == PT::Sound; });
        auto canOpen = [this](const Ctx& ctx) {
            if (!hasSingleObjectTarget(ctx))
                return false;
            ProjectItem* item = menuAnchorItem();
            if (!item || item->type != PT::Sound) return false;
            const auto sourceId = SoundSourceEditing::activeSourceId(
                *item, PlatformContextService::instance().context().primary,
                SoundSourceEditing::previewLanguage(*item, LanguageContextService::instance().language()));
            const auto* asset = AssetRegistry::instance().getAsset(sourceId);
            return asset && QFileInfo(asset->absolutePath).isFile();
        };
        s.canEnable = canOpen;
        s.handler = [this, canOpen]() {
            if (!canOpen(captureCommandContext())) return;
            if (ProjectItem* item = menuAnchorItem())
                emit openContainingFolderRequested(item);
        };
        s.separatorAfter = true;
        stdItems.push_back(std::move(s));
    }

    {
        Spec s;
        s.label = tr("Open in New Tab");
        s.canShow = hasEditors;
        s.canEnable = [this](const Ctx&) {
            const auto items = uniqueMenuItems();
            if (items.isEmpty() || !menuFolders().isEmpty())
                return false;
            for (ProjectItem* item : items) {
                if (!item)
                    return false;
            }
            return true;
        };
        s.handler = [this]() {
            const QList<ProjectItem*> items = uniqueMenuItems();
            if (!menuFolders().isEmpty()) return;
            for (ProjectItem* item : items) {
                if (item)
                    emit openInNewTabRequested(item);
            }
        };
        stdItems.push_back(std::move(s));
    }
    {
        Spec s;
        s.label = tr("Open in New Window");
        s.canShow = hasEditors;
        s.canEnable = [this](const Ctx& ctx) {
            if (!hasSingleObjectTarget(ctx))
                return false;
            ProjectItem* item = menuAnchorItem();
            return item != nullptr;
        };
        s.handler = [this]() {
            if (!hasSingleObjectTarget(captureCommandContext())) return;
            if (ProjectItem* item = menuAnchorItem())
                emit openInNewWindowRequested(item);
        };
        s.separatorAfter = true;
        stdItems.push_back(std::move(s));
    }

    {
        Spec s;
        s.label = tr("Set Color...");
        s.canShow = [this, hasObjects](const Ctx& ctx) {
            return AuthoringPolicy::supportsObjectColorEditing() && hasObjects(ctx)
                && !uniqueMenuItems().isEmpty();
        };
        s.canEnable = [this](const Ctx&) { return !uniqueMenuItems().isEmpty(); };
        s.handler = [this]() { setItemColorViaDialog(uniqueMenuItems()); };
        s.separatorAfter = true;
        stdItems.push_back(std::move(s));
    }

    {
        Spec s;
        s.label = tr("Operation");
        s.canEnable = [](const Ctx&) { return true; };
        s.buildSubmenu = [this](QMenu* sub, const Ctx&) {
            const auto expansionStates = [](const QList<QTreeWidgetItem*>& roots) {
                std::pair<bool, bool> states{false, false};
                std::function<void(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* row) {
                    if (row->childCount() == 0) return;
                    states.first |= !row->isExpanded();
                    states.second |= row->isExpanded();
                    for (int i = 0; i < row->childCount(); ++i) visit(row->child(i));
                };
                for (QTreeWidgetItem* row : roots) visit(row);
                return states;
            };
            auto* expandAll = sub->addAction(tr("Expand All"), this, [this]() {
                for (QTreeWidgetItem* node : expandTargetRows())
                    setSubtreeExpanded(node, true);
            });
            auto* collapseAll = sub->addAction(tr("Collapse All"), this, [this]() {
                for (QTreeWidgetItem* node : expandTargetRows())
                    setSubtreeExpanded(node, false);
            });
            const auto [canExpandAll, canCollapseAll] = expansionStates(expandTargetRows());
            expandAll->setEnabled(canExpandAll);
            collapseAll->setEnabled(canCollapseAll);
        };
        stdItems.push_back(std::move(s));
    }

    menuController_->buildStandardItems(contextMenu_, std::move(stdItems));
}

void ProjectTreePanel::prepareItemContextMenu(QTreeWidgetItem* row, ProjectItem* item)
{
    PhysicalFolder* folder = folderMap_.value(row, nullptr);
    if (item || folder) {
        if (!row->isSelected()) {
            const QSignalBlocker blocker(treeWidget_);
            treeWidget_->clearSelection();
            row->setSelected(true);
        }
        treeWidget_->setCurrentItem(row, 0, QItemSelectionModel::NoUpdate);
    } else {
        selectNone();
        treeWidget_->setCurrentItem(row, 0, QItemSelectionModel::NoUpdate);
    }
    contextItem_ = item;
    if (menuController_) menuController_->clearMenuContext();
    const auto context = captureCommandContext();
    contextBucketFolder_ = context.bucketFolderName;
    applyMenuContext(context);
}

void ProjectTreePanel::applyMenuContext(const ProjectItemMenuController::MenuContext& context)
{
    if (menuController_)
        menuController_->setMenuContext(context);
}

void ProjectTreePanel::onNewParent(ProjectItem::Type containerType)
{
    if (!menuFolders().isEmpty()) return;
    const auto ctx = captureCommandContext();
    QList<ProjectItem*> sel = resolveMenuItems(ctx.selectionGuids);
    auto& pm = ProjectManager::instance();
    if (!pm.canCreateParentContainer(sel, containerType)) return;
    QString typeName = ProjectItemMenuController::typeDisplayName(containerType);
    const QString name = QStringLiteral("New_%1").arg(QString(typeName).remove(' '));
    ProjectItem* container = pm.createParentContainer(sel, containerType, name);
    if (container) selectResultItems({container});
}

void ProjectTreePanel::onNewAction(VActionStep::Type actionType)
{
    if (!menuFolders().isEmpty()) return;
    const QList<ProjectItem*> targets = uniqueMenuItems();
    auto& pm = ProjectManager::instance();
    if (!pm.canCreateActionsTargeting(targets, actionType))
        return;
    const std::vector<ProjectItem*> created = pm.createActionsTargeting(targets, actionType);
    QList<ProjectItem*> results;
    for (ProjectItem* item : created)
        results.append(item);
    if (!results.isEmpty())
        selectResultItems(results);
}

std::vector<ProjectItem::Type>
ProjectTreePanel::newParentCandidateTypes(const QList<ProjectItem*>& selection) const
{
    using PT = ProjectItem::Type;
    std::vector<PT> out;
    for (PT c : ItemTypeCatalog::allTypes()) {
        if (ItemTypeCatalog::isUserFacingType(c)
            && std::any_of(selection.begin(), selection.end(), [c](const ProjectItem* item) {
                return ItemTypeCatalog::canBeChildOf(item->type, c);
            }))
            out.push_back(c);
    }
    return out;
}

QIcon ProjectTreePanel::decorateItemIcon(const QIcon& icon, const ProjectItem*) const
{
    return icon;
}

void ProjectTreePanel::populateCreatedSubtree(ProjectItem* item, QTreeWidgetItem* row)
{
    std::function<void(ProjectItem*, QTreeWidgetItem*)> buildChildren =
        [&](ProjectItem* node, QTreeWidgetItem* nodeTree) {
            for (auto& child : node->children) {
                QTreeWidgetItem* childTree = createTreeItem(child.get());
                nodeTree->addChild(childTree);
                addToItemMap(childTree, child.get());
                buildChildren(child.get(), childTree);
            }
        };
    buildChildren(item, row);
}

QList<ProjectItem*> ProjectTreePanel::resolveMenuItems(const QList<QUuid>& guids) const
{
    QList<ProjectItem*> items;
    QSet<QUuid> seen;
    auto& pm = ProjectManager::instance();
    for (const QUuid& guid : guids) {
        if (guid.isNull()) return {};
        if (seen.contains(guid))
            continue;
        if (ProjectItem* item = pm.findItemByGuid(guid)) {
            seen.insert(guid);
            items.append(item);
        } else return {};
    }
    return items;
}

ProjectItemMenuController::MenuContext ProjectTreePanel::captureCommandContext() const
{
    if (menuController_ && menuController_->hasMenuContext())
        return menuController_->menuContext();
    ProjectItemMenuController::MenuContext ctx;
    QTreeWidgetItem* current = treeWidget_ ? treeWidget_->currentItem() : nullptr;
    const FolderRowAddress address = folderAddressForRow(current);
    ctx.bucketFolderName = address.valid ? address.bucketFolderName : bucketKeyForTreeItem(current);
    ctx.folderPath = address.valid ? address.relPath : QString();
    for (ProjectItem* item : itemsInTreeOrder(selectedItems()))
        ctx.selectionGuids.append(item->guid);
    ctx.selectionFolders = selectedFolders();
    if (PhysicalFolder* folder = folderMap_.value(current)) {
        ctx.kind = ProjectItemMenuController::ContextKind::Folder;
        ctx.contextFolder = folder;
    } else if (ProjectItem* item = itemMap_.value(current)) {
        ctx.kind = ProjectItemMenuController::ContextKind::Object;
        ctx.contextItemGuid = item->guid;
    } else if (current && address.valid && address.isBucketRoot) {
        ctx.kind = ProjectItemMenuController::ContextKind::BucketRoot;
    } else {
        ctx.kind = ProjectItemMenuController::ContextKind::Empty;
        ctx.bucketFolderName.clear();
        ctx.folderPath.clear();
    }
    return ctx;
}

bool ProjectTreePanel::hasSingleObjectTarget(const ProjectItemMenuController::MenuContext& ctx) const
{
    return ctx.selectionFolders.isEmpty() && ctx.selectionGuids.size() == 1
        && !ctx.contextItemGuid.isNull() && ctx.selectionGuids.front() == ctx.contextItemGuid
        && ProjectManager::instance().findItemByGuid(ctx.contextItemGuid)
        && ctx.kind == ProjectItemMenuController::ContextKind::Object;
}

bool ProjectTreePanel::hasSingleFolderTarget(const ProjectItemMenuController::MenuContext& ctx) const
{
    const auto* project = ProjectManager::instance().currentProject();
    return project && ctx.selectionGuids.isEmpty() && ctx.selectionFolders.size() == 1
        && ctx.contextFolder == ctx.selectionFolders.front()
        && !project->folderBucket(ctx.contextFolder).isEmpty()
        && ctx.kind == ProjectItemMenuController::ContextKind::Folder;
}

QList<PhysicalFolder*> ProjectTreePanel::menuFolders() const
{
    const auto* project = ProjectManager::instance().currentProject();
    if (!project) return {};
    const auto folders = captureCommandContext().selectionFolders;
    for (const PhysicalFolder* folder : folders)
        if (project->folderBucket(folder).isEmpty()) return {};
    return folders;
}

QList<ProjectItem*> ProjectTreePanel::itemsInTreeOrder(const QList<ProjectItem*>& items) const
{
    QList<ProjectItem*> ordered;
    QSet<ProjectItem*> wanted;
    for (ProjectItem* item : items) {
        if (item)
            wanted.insert(item);
    }
    if (wanted.isEmpty() || !treeWidget_)
        return ordered;

    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* row) {
        if (!row)
            return;
        if (ProjectItem* item = itemMap_.value(row, nullptr)) {
            if (wanted.contains(item))
                ordered.append(item);
        }
        for (int i = 0; i < row->childCount(); ++i)
            walk(row->child(i));
    };
    for (int i = 0; i < treeWidget_->topLevelItemCount(); ++i)
        walk(treeWidget_->topLevelItem(i));
    return ordered;
}

QList<QTreeWidgetItem*> ProjectTreePanel::expandTargetRows() const
{
    const auto ctx = captureCommandContext();
    QList<QTreeWidgetItem*> rows;
    QSet<QTreeWidgetItem*> seen;
    auto addRow = [&](QTreeWidgetItem* row) {
        if (!row || seen.contains(row))
            return;
        for (QTreeWidgetItem* ancestor = row->parent(); ancestor; ancestor = ancestor->parent()) {
            if (seen.contains(ancestor))
                return;
        }
        seen.insert(row);
        rows.append(row);
    };

    if (ctx.kind == ProjectItemMenuController::ContextKind::Object
        || ctx.kind == ProjectItemMenuController::ContextKind::Folder) {
        const QList<ProjectItem*> items = uniqueMenuItems();
        for (PhysicalFolder* folder : menuFolders()) addRow(reverseFolderMap_.value(folder));
        for (ProjectItem* item : itemsInTreeOrder(items)) addRow(findTreeItem(item));
        return rows;
    }

    if (treeWidget_ && ctx.kind == ProjectItemMenuController::ContextKind::BucketRoot
        && !ctx.bucketFolderName.isEmpty()) {
        addRow(categoryFolders_.value(ctx.bucketFolderName, nullptr));
        return rows;
    }

    return rows;
}

bool ProjectTreePanel::canPasteAtContext(const ProjectItemMenuController::MenuContext& ctx) const
{
    if (ctx.kind == ProjectItemMenuController::ContextKind::BucketRoot
        && ctx.selectionGuids.isEmpty())
        return ProjectManager::instance().canPasteClipboardAtBucketRoot(ctx.bucketFolderName);
    if (ctx.kind != ProjectItemMenuController::ContextKind::Object) {
        return false;
    }
    if (!hasSingleObjectTarget(ctx))
        return false;
    ProjectItem* target = ProjectManager::instance().findItemByGuid(ctx.contextItemGuid);
    return ProjectManager::instance().canPasteClipboard(target);
}

ProjectItem* ProjectTreePanel::menuAnchorItem() const
{
    const auto ctx = captureCommandContext();
    if (ctx.kind == ProjectItemMenuController::ContextKind::Object
        || ctx.kind == ProjectItemMenuController::ContextKind::Folder) {
        return ProjectManager::instance().findItemByGuid(ctx.contextItemGuid);
    }
    return nullptr;
}

QList<ProjectItem*> ProjectTreePanel::menuAnchorItems() const
{
    return uniqueMenuItems();
}

QList<ProjectItem*> ProjectTreePanel::uniqueMenuItems() const
{
    const auto context = captureCommandContext();
    if (!context.selectionFolders.isEmpty() && menuFolders().isEmpty()) return {};
    return resolveMenuItems(context.selectionGuids);
}

QList<ProjectItem*> ProjectTreePanel::subtreeMenuItems() const
{
    return ProjectManager::instance().normalizeSubtreeItems(uniqueMenuItems());
}

ProjectItem* ProjectTreePanel::projectItemForTreeItem(QTreeWidgetItem* treeItem) const
{
    return itemMap_.value(treeItem, nullptr);
}

QTreeWidgetItem* ProjectTreePanel::treeItemForProjectItem(ProjectItem* item) const
{
    return reverseItemMap_.value(item, nullptr);
}

QString ProjectTreePanel::bucketKeyForTreeItem(QTreeWidgetItem* treeItem) const
{
    return categoryFolders_.key(treeItem);
}

QTreeWidgetItem* ProjectTreePanel::ensureCategoryFolder(const QString& folderName)
{
    auto it = categoryFolders_.find(folderName);
    if (it != categoryFolders_.end()) return it.value();

    QTreeWidgetItem* item = TreeRowStyle::makeBucketRow(folderName);
    treeWidget_->addTopLevelItem(item);
    item->setExpanded(true);
    categoryFolders_[folderName] = item;
    return item;
}

void ProjectTreePanel::updateBucketRows()
{
    const auto changed = ProjectManager::instance().bucketsWithPendingFileChanges();
    for (auto it = categoryFolders_.cbegin(); it != categoryFolders_.cend(); ++it) {
        const bool dirty = changed.contains(it.key());
        const QString name = ProjectManager::bucketDisplayName(it.key());
        it.value()->setText(0, dirty ? name + QStringLiteral(" *") : name);
        QFont font = it.value()->font(0);
        font.setItalic(dirty);
        it.value()->setFont(0, font);
    }
}

QTreeWidgetItem* ProjectTreePanel::ensureFolderRow(const PhysicalFolder* folder)
{
    auto* mutableFolder = const_cast<PhysicalFolder*>(folder);
    if (QTreeWidgetItem* row = reverseFolderMap_.value(mutableFolder)) return row;
    QTreeWidgetItem* parentRow = folder->parent
        ? ensureFolderRow(folder->parent)
        : ensureCategoryFolder(ProjectManager::instance().currentProject()->folderBucket(folder));
    auto* row = new QTreeWidgetItem(parentRow);
    row->setFlags(row->flags() | Qt::ItemIsEditable);
    folderMap_.insert(row, mutableFolder);
    reverseFolderMap_.insert(mutableFolder, row);
    const auto* project = ProjectManager::instance().currentProject();
    const QString path = project->folderBucket(folder) + QLatin1Char('/') + project->folderRelPath(folder);
    folderRowPaths_.insert(folder, path);
    updateFolderRow(row, mutableFolder);
    row->setExpanded(!collapsedFolderPaths_.contains(path));
    return row;
}

void ProjectTreePanel::updateFolderRow(QTreeWidgetItem* row, PhysicalFolder* folder)
{
    const bool dirty = ProjectManager::instance().isFolderDirty(folder);
    const QString name = objectLabels_ && searchBox_ && !searchBox_->text().isEmpty()
        ? objectLabels_->label(*folder) : folder->name;
    row->setText(0, dirty ? name + QStringLiteral(" *") : name);
    QFont font = row->font(0);
    font.setItalic(dirty);
    row->setFont(0, font);
    row->setIcon(0, ThemeManager::instance().resolveToggleIcon(
        QString::fromLatin1(ThemeAssetIds::TreeFolderOpen),
        QString::fromLatin1(ThemeAssetIds::TreeFolderClosed)).icon);
    const auto* project = ProjectManager::instance().currentProject();
    row->setToolTip(0, tr("Folder: %1").arg(project
        ? project->folderBucket(folder) + QLatin1Char('/') + project->folderRelPath(folder)
        : folder->name));
}

void ProjectTreePanel::setFolderExpanded(PhysicalFolder* folder, bool expanded)
{
    const QString path = folderRowPaths_.value(folder);
    if (path.isEmpty()) return;
    if (expanded) collapsedFolderPaths_.remove(path);
    else collapsedFolderPaths_.insert(path);
    saveFolderExpansion();
}

void ProjectTreePanel::updateFolderExpansionPaths()
{
    const auto* project = ProjectManager::instance().currentProject();
    if (!project) return;
    bool changed = false;
    for (auto it = folderRowPaths_.begin(); it != folderRowPaths_.end(); ++it) {
        const QString path = project->folderBucket(it.key()) + QLatin1Char('/') + project->folderRelPath(it.key());
        if (path == it.value()) continue;
        const bool collapsed = collapsedFolderPaths_.remove(it.value());
        collapsedFolderPaths_.remove(path);
        if (collapsed) collapsedFolderPaths_.insert(path);
        it.value() = path;
        changed = true;
    }
    if (changed) saveFolderExpansion();
}

void ProjectTreePanel::saveFolderExpansion() const
{
    if (folderExpansionSettingsKey_.isEmpty()) return;
    QStringList paths = collapsedFolderPaths_.values();
    paths.sort();
    QSettings settings;
    settings.setValue(folderExpansionSettingsKey_, paths);
}

PhysicalFolder* ProjectTreePanel::folderForTreeItem(QTreeWidgetItem* row) const
{
    return folderMap_.value(row, nullptr);
}

QTreeWidgetItem* ProjectTreePanel::treeItemForFolder(PhysicalFolder* folder) const
{
    return reverseFolderMap_.value(folder, nullptr);
}

void ProjectTreePanel::onFolderCreated(PhysicalFolder* folder)
{
    const auto add = [this](auto&& self, PhysicalFolder* node) -> void {
        ensureFolderRow(node);
        for (const auto& child : node->children) self(self, child.get());
    };
    add(add, folder);
    applyVisibility();
    if (auto* row = reverseFolderMap_.value(folder)) {
        if (row->parent()) row->parent()->setExpanded(true);
        row->setExpanded(true);
        treeWidget_->setCurrentItem(row);
    }
}

void ProjectTreePanel::onFolderDeleted(PhysicalFolder* folder)
{
    if (menuController_) menuController_->clearMenuContext();
    if (QTreeWidgetItem* row = reverseFolderMap_.value(folder)) {
        const QSignalBlocker blocker(treeWidget_);
        const auto unmap = [this](auto&& self, QTreeWidgetItem* node) -> void {
            for (int i = 0; i < node->childCount(); ++i) self(self, node->child(i));
            removeFromItemMap(node);
            if (auto* physical = folderMap_.take(node)) {
                reverseFolderMap_.remove(physical);
                folderRowPaths_.remove(physical);
            }
        };
        unmap(unmap, row);
        delete row;
    }
    applyVisibility();
    onSelectionChanged();
}

void ProjectTreePanel::onProjectOpened(ProjectData* project)
{
    folderExpansionSettingsKey_ = QStringLiteral("ProjectTree/Folders/%1/%2/collapsed")
        .arg(project->projectId, QString::fromLatin1(PanelRegistry::key(settingsPanelId_)));
    const QSettings settings;
    const QStringList paths = settings.value(folderExpansionSettingsKey_).toStringList();
    collapsedFolderPaths_ = QSet<QString>(paths.begin(), paths.end());
    rebuildTree();
}

void ProjectTreePanel::onProjectClosed()
{
    clear();
    folderExpansionSettingsKey_.clear();
    collapsedFolderPaths_.clear();
}

void ProjectTreePanel::rebuildTree()
{
    const auto selection = selectedItems();
    const auto folderSelection = selectedFolders();
    auto* current = selectedItem();
    auto* currentFolder = selectedFolder();
    const QSignalBlocker blocker(treeWidget_);
    clear();

    ProjectData* project = ProjectManager::instance().currentProject();
    if (!project) return;

    for (const QString& cat : ProjectManager::getStandardDirectories())
    {
        if (VNX::isUiHiddenBucket(cat)) continue;
        ensureCategoryFolder(cat);
    }


    std::function<void(ProjectItem*, QTreeWidgetItem*)> addChildren =
        [this, &addChildren](ProjectItem* item, QTreeWidgetItem* treeItem) {
        for (const auto& child : item->children)
        {
            if (!child) continue;
            if (child->type == ProjectItem::Type::Bus)
                continue;

            QTreeWidgetItem* childTreeItem = createTreeItem(child.get());
            treeItem->addChild(childTreeItem);
            addToItemMap(childTreeItem, child.get());
            childTreeItem->setExpanded(child->expanded);

            addChildren(child.get(), childTreeItem);
        }
    };

    for (const ProjectFolderBinding& binding : project->folders)
    {
        if (!binding.rootFolder) continue;
        if (VNX::isUiHiddenBucket(binding.parentFolder)) continue;
        const auto addFolders = [this](auto&& self, PhysicalFolder* folder) -> void {
            ensureFolderRow(folder);
            for (const auto& child : folder->children) self(self, child.get());
        };
        addFolders(addFolders, binding.rootFolder.get());
    }

    for (const auto* binding : project->allFileBindings())
    {
        if (!binding || !binding->rootItem) continue;
        if (VNX::isUiHiddenBucket(binding->parentFolder)) continue;
        const auto* root = binding->rootItem.get();

        if (root->type == ProjectItem::Type::Bus)
            continue;

        QTreeWidgetItem* parentItem = ensureCategoryFolder(binding->parentFolder);

        const PhysicalFolder* folder = project->folderForItem(root);
        QTreeWidgetItem* slotParent = folder ? ensureFolderRow(folder) : parentItem;

        QTreeWidgetItem* rootTreeItem = createTreeItem(const_cast<ProjectItem*>(root));
        slotParent->addChild(rootTreeItem);
        addToItemMap(rootTreeItem, const_cast<ProjectItem*>(root));
        rootTreeItem->setExpanded(true);

        addChildren(const_cast<ProjectItem*>(root), rootTreeItem);
    }

    rebuildBusTree();
    applyVisibility();
    setSelectedEntries(selection, folderSelection);
    if (auto* row = reverseItemMap_.value(current, nullptr))
        treeWidget_->setCurrentItem(row, 0, QItemSelectionModel::NoUpdate);
    else if (auto* row = reverseFolderMap_.value(currentFolder, nullptr))
        treeWidget_->setCurrentItem(row, 0, QItemSelectionModel::NoUpdate);
}

void ProjectTreePanel::rebuildBusTree()
{
    bussesFolder_ = categoryFolders_.value(ItemTypeCatalog::kBussesFolderName, nullptr);
    if (!bussesFolder_) return;
    const auto selection = selectedItems();
    const auto folderSelection = selectedFolders();
    auto* current = selectedItem();
    auto* currentFolder = selectedFolder();
    const QSignalBlocker blocker(treeWidget_);

    if (busTreeRootItem_) {
        std::function<void(QTreeWidgetItem*)> unmapRecursive =
            [this, &unmapRecursive](QTreeWidgetItem* row) {
            removeFromItemMap(row);
            for (int i = 0; i < row->childCount(); ++i)
                unmapRecursive(row->child(i));
        };
        unmapRecursive(busTreeRootItem_);
        if (QTreeWidgetItem* parent = busTreeRootItem_->parent())
            parent->removeChild(busTreeRootItem_);
        delete busTreeRootItem_;
        busTreeRootItem_ = nullptr;
    }

    auto& pm = ProjectManager::instance();
    if (!pm.currentProject()) return;
    ProjectItem* master = pm.getMasterBusItem();
    if (!master) return;

    QTreeWidgetItem* busParent = bussesFolder_;
    for (int i = 0; i < bussesFolder_->childCount(); ++i) {
        QTreeWidgetItem* child = bussesFolder_->child(i);
        if (itemMap_.contains(child)) {
            busParent = child;
            break;
        }
    }

    std::function<QTreeWidgetItem*(ProjectItem*, QTreeWidgetItem*)> addBus =
        [&](ProjectItem* item, QTreeWidgetItem* parentRow) -> QTreeWidgetItem* {
        const auto* bc = item->comp<VBus>();
        if (!bc) return nullptr;

        QTreeWidgetItem* row = createTreeItem(item);
        parentRow->addChild(row);
        addToItemMap(row, item);
        row->setExpanded(true);

        for (const auto& child : item->children)
            if (child) addBus(child.get(), row);
        return row;
    };
    busTreeRootItem_ = addBus(master, busParent);

    bussesFolder_->setExpanded(true);

    bussesFolder_->setHidden(false);
    applyVisibility();
    setSelectedEntries(selection, folderSelection);
    if (auto* row = reverseItemMap_.value(current, nullptr))
        treeWidget_->setCurrentItem(row, 0, QItemSelectionModel::NoUpdate);
    else if (auto* row = reverseFolderMap_.value(currentFolder, nullptr))
        treeWidget_->setCurrentItem(row, 0, QItemSelectionModel::NoUpdate);
}

void ProjectTreePanel::selectBusInTree(uint32_t busID)
{
    ProjectItem* busItem = ProjectManager::instance().findBusItem(busID);
    QTreeWidgetItem* row = busItem ? reverseItemMap_.value(busItem, nullptr) : nullptr;
    if (row) {
        treeWidget_->blockSignals(true);
        treeWidget_->setCurrentItem(row, 0, row->isSelected()
            ? QItemSelectionModel::NoUpdate : QItemSelectionModel::ClearAndSelect);
        treeWidget_->scrollToItem(row);
        treeWidget_->blockSignals(false);
    }
}

bool ProjectTreePanel::selectProjectItem(ProjectItem* item)
{
    if (!item) {
        return false;
    }

    QTreeWidgetItem* treeItem = findTreeItem(item);
    if (!treeItem) {
        return false;
    }

    treeWidget_->blockSignals(true);
    treeWidget_->clearSelection();
    treeWidget_->setCurrentItem(treeItem);
    treeItem->setSelected(true);
    treeWidget_->scrollToItem(treeItem);
    treeWidget_->blockSignals(false);

    onItemClicked(treeItem, 0);
    onSelectionChanged();
    return true;
}

bool ProjectTreePanel::pingProjectItem(ProjectItem* item)
{
    if (!item) return false;
    QTreeWidgetItem* treeItem = findTreeItem(item);
    if (!treeItem) return false;
    for (auto* row = treeItem; row; row = row->parent()) {
        if (row->isHidden()) {
            searchBox_->clear();
            break;
        }
    }
    prepareItemReveal(treeItem);
    TreeRowPing::ping(treeWidget_, treeItem);
    return true;
}

void ProjectTreePanel::setSelectedProjectItems(const QList<ProjectItem*>& items)
{
    setSelectedEntries(items, {});
}

void ProjectTreePanel::setSelectedEntries(const QList<ProjectItem*>& items, const QList<PhysicalFolder*>& folders)
{
    const QSignalBlocker blocker(treeWidget_);
    const QList<ProjectItem*> ordered = itemsInTreeOrder(items);
    QList<QTreeWidgetItem*> rows;
    for (auto* item : ordered) {
        if (auto* row = reverseItemMap_.value(item, nullptr)) rows.push_back(row);
    }
    for (auto* folder : folders)
        if (auto* row = reverseFolderMap_.value(folder)) rows.push_back(row);
    treeWidget_->clearSelection();
    treeWidget_->setCurrentItem(rows.isEmpty() ? nullptr : rows.front(), 0,
                                QItemSelectionModel::NoUpdate);
    for (auto* row : rows) {
        row->setSelected(true);
        for (auto* parent = row->parent(); parent; parent = parent->parent())
            parent->setExpanded(true);
    }
    if (!rows.isEmpty()) treeWidget_->scrollToItem(rows.front());
}

void ProjectTreePanel::selectResultItems(const QList<ProjectItem*>& items)
{
    QList<ProjectItem*> live;
    for (ProjectItem* item : items) {
        if (item && ProjectManager::instance().findItemByGuid(item->guid) == item)
            live.append(item);
    }
    if (live.isEmpty()) {
        selectNone();
        emit itemSelected(nullptr);
        emit objectSelectionChanged({});
        return;
    }
    setSelectedProjectItems(live);
    const QList<ProjectItem*> selected = selectedItems();
    emit itemSelected(selected.value(0));
    emit objectSelectionChanged(selected);
}

void ProjectTreePanel::setItemColorViaDialog(const QList<ProjectItem*>& items)
{
    if (items.isEmpty()) return;

    auto& pm = ProjectManager::instance();
    QList<QUuid> guids;
    for (ProjectItem* item : items) {
        if (!item) return;
        guids.append(item->guid);
    }
    const uint32_t initialColor = items.front()->color;
    QColor initial = initialColor != 0
        ? QColor::fromRgba(initialColor)
        : ThemeManager::instance().colors().accent;
    QColor color = QColorDialog::getColor(initial, this, tr("Select Color"));
    if (!color.isValid())
        return;

    const auto targets = resolveMenuItems(guids);
    const auto* project = pm.currentProject();
    if (!project) return;
    AutoComplexUndo macro(tr("Set Color"));
    if (targets.size() > 1) macro.open();
    if (!targets.isEmpty())
        pm.setItemsColor(targets, color.rgba());
}

void ProjectTreePanel::onItemCreated(ProjectItem* item)
{
    if (!item) return;

    // Buses render in the dedicated Busses region with specialized styling; the
    // lifecycle signals now drive that region through a single rebuild path
    // rather than the generic per-item tree ops used for other types.
    if (item->type == ProjectItem::Type::Bus) {
        rebuildBusTree();
        return;
    }

    QTreeWidgetItem* parentTreeItem = nullptr;
    if (!item->parent)
    {
        ProjectData* project = ProjectManager::instance().currentProject();
        if (!project) return;

        QString parentFolder;
        const PhysicalFolder* folder = nullptr;
        if (ProjectFileBinding* binding = findFileBindingForItem(*project, item)) {
            parentFolder = binding->parentFolder;
            folder = project->folderForItem(item);
        }
        if (parentFolder.isEmpty())
        {
            qWarning() << "[ProjectTreePanel] onItemCreated: top-level item"
                       << item->name << "has no bucket owner; tree out of sync";
            return;
        }

        QTreeWidgetItem* categoryItem = ensureCategoryFolder(parentFolder);
        if (!categoryItem) return;

        parentTreeItem = folder ? ensureFolderRow(folder) : categoryItem;
    } else {
        parentTreeItem = findTreeItem(item->parent);
    }
    if (!parentTreeItem) return;

    QTreeWidgetItem* newItem = createTreeItem(item);
    parentTreeItem->addChild(newItem);
    addToItemMap(newItem, item);

    populateCreatedSubtree(item, newItem);

    parentTreeItem->setExpanded(true);
    newItem->setExpanded(true);
    treeWidget_->setCurrentItem(newItem);
}

void ProjectTreePanel::onItemRenamed(ProjectItem* item, const QString& oldName)
{
    Q_UNUSED(oldName)

    if (item && item->type == ProjectItem::Type::Bus) {
        rebuildBusTree();
        return;
    }

    QTreeWidgetItem* treeItem = findTreeItem(item);
    if (treeItem)
    {
        updateTreeItem(treeItem, item);
    }
    applyVisibility();
}

void ProjectTreePanel::onItemPropertyChanged(ProjectItem* item, const QString& propertyName)
{
    QTreeWidgetItem* treeItem = findTreeItem(item);
    if (treeItem)
    {
        if (propertyName == QLatin1String("childOrder")) {
            const auto selection = selectedItems();
            const auto folderSelection = selectedFolders();
            auto* currentFolder = selectedFolder();
            auto* current = selectedItem();
            const QSignalBlocker blocker(treeWidget_);
            for (int targetIndex = 0;
                 targetIndex < static_cast<int>(item->children.size());
                 ++targetIndex) {
                QTreeWidgetItem* childTree = findTreeItem(
                    item->children[static_cast<size_t>(targetIndex)].get());
                if (!childTree || childTree->parent() != treeItem) continue;
                const int currentIndex = treeItem->indexOfChild(childTree);
                if (currentIndex == targetIndex) continue;
                treeItem->takeChild(currentIndex);
                treeItem->insertChild(targetIndex, childTree);
            }
            setSelectedEntries(selection, folderSelection);
            if (auto* row = reverseItemMap_.value(current, nullptr))
                treeWidget_->setCurrentItem(row, 0, QItemSelectionModel::NoUpdate);
            else if (auto* row = reverseFolderMap_.value(currentFolder, nullptr))
                treeWidget_->setCurrentItem(row, 0, QItemSelectionModel::NoUpdate);
        }
        updateTreeItem(treeItem, item);
    }
}

void ProjectTreePanel::onProjectSaved()
{
    updateBucketRows();
    for (auto it = itemMap_.constBegin(); it != itemMap_.constEnd(); ++it)
    {
        updateTreeItem(it.key(), it.value());
    }
    for (auto it = folderMap_.cbegin(); it != folderMap_.cend(); ++it)
        updateFolderRow(it.key(), it.value());
}

void ProjectTreePanel::onItemDeleted(ProjectItem* item)
{
    if (item && contextItem_) {
        for (ProjectItem* p = contextItem_; p; p = p->parent) {
            if (p == item) {
                contextItem_ = nullptr;
                break;
            }
        }
    }

    const auto previousSelection = selectedItems();
    const auto folderSelection = selectedFolders();
    auto* currentFolder = selectedFolder();
    auto* current = selectedItem();
    auto selection = previousSelection;
    selection.removeIf([item](const ProjectItem* selected) {
        for (auto* ancestor = selected; ancestor; ancestor = ancestor->parent)
            if (ancestor == item) return true;
        return false;
    });
    {
        const QSignalBlocker blocker(treeWidget_);
        if (item && item->type == ProjectItem::Type::Bus) {
            rebuildBusTree();
        } else if (auto* treeItem = findTreeItem(item)) {
            std::function<void(QTreeWidgetItem*)> removeRecursive =
                [this, &removeRecursive](QTreeWidgetItem* row) {
                for (int i = 0; i < row->childCount(); ++i)
                    removeRecursive(row->child(i));
                removeFromItemMap(row);
            };
            removeRecursive(treeItem);
            delete treeItem;
        }
        setSelectedEntries(selection, folderSelection);
        if (auto* row = reverseItemMap_.value(current, nullptr))
            treeWidget_->setCurrentItem(row, 0, QItemSelectionModel::NoUpdate);
        else if (auto* row = reverseFolderMap_.value(currentFolder, nullptr))
            treeWidget_->setCurrentItem(row, 0, QItemSelectionModel::NoUpdate);
    }
    if (current != selectedItem()) {
        emit itemSelected(selection.value(0));
    } else if (selection != previousSelection) {
        if (selection.isEmpty()) {
            emit itemSelected(nullptr);
        } else {
            emit objectSelectionChanged(selection);
        }
    }
}

void ProjectTreePanel::onItemMoved(ProjectItem* item, ProjectItem* oldParent, ProjectItem* newParent)
{
    if (!item) return;

    if (item->type == ProjectItem::Type::Bus) {
        rebuildBusTree();
        return;
    }

    if (!newParent)
    {
        rebuildTree();
        return;
    }

    QTreeWidgetItem* treeItem = findTreeItem(item);
    QTreeWidgetItem* newParentTreeItem = findTreeItem(newParent);

    if (treeItem && newParentTreeItem)
    {
        const auto selection = selectedItems();
        const auto folderSelection = selectedFolders();
        auto* currentFolder = selectedFolder();
        auto* current = selectedItem();
        const QSignalBlocker blocker(treeWidget_);
        QTreeWidgetItem* oldParentTreeItem = treeItem->parent();
        if (oldParentTreeItem)
        {
            int index = oldParentTreeItem->indexOfChild(treeItem);
            if (index >= 0)
            {
                oldParentTreeItem->takeChild(index);
            }
        }
        else
        {
            int index = treeWidget_->indexOfTopLevelItem(treeItem);
            if (index >= 0)
            {
                treeWidget_->takeTopLevelItem(index);
            }
        }

        if (oldParent == newParent) {
            int targetIndex = 0;
            for (int i = 0; i < static_cast<int>(newParent->children.size()); ++i) {
                if (newParent->children[static_cast<size_t>(i)].get() == item) {
                    targetIndex = i;
                    break;
                }
            }
            newParentTreeItem->insertChild(targetIndex, treeItem);
        } else {
            newParentTreeItem->addChild(treeItem);
        }
        newParentTreeItem->setExpanded(true);

        setSelectedEntries(selection, folderSelection);
        if (auto* row = reverseItemMap_.value(current, nullptr))
            treeWidget_->setCurrentItem(row, 0, QItemSelectionModel::NoUpdate);
        else if (auto* row = reverseFolderMap_.value(currentFolder, nullptr))
            treeWidget_->setCurrentItem(row, 0, QItemSelectionModel::NoUpdate);
    }
}

QTreeWidgetItem* ProjectTreePanel::findTreeItem(ProjectItem* item) const
{
    auto it = reverseItemMap_.find(item);
    if (it != reverseItemMap_.end()) {
        return it.value();
    }
    return nullptr;
}

void ProjectTreePanel::setSubtreeExpanded(QTreeWidgetItem* item, bool expanded)
{
    if (!item) return;
    item->setExpanded(expanded);
    for (int i = 0; i < item->childCount(); ++i)
        setSubtreeExpanded(item->child(i), expanded);
}

void ProjectTreePanel::addToItemMap(QTreeWidgetItem* treeItem, ProjectItem* projectItem)
{
    itemMap_[treeItem] = projectItem;
    reverseItemMap_[projectItem] = treeItem;
}

void ProjectTreePanel::removeFromItemMap(QTreeWidgetItem* treeItem)
{
    auto it = itemMap_.find(treeItem);
    if (it != itemMap_.end()) {
        reverseItemMap_.remove(it.value());
        itemMap_.erase(it);
    }
}

void ProjectTreePanel::newProject(const QString& name)
{
    Q_UNUSED(name)
}

bool ProjectTreePanel::loadProject(const QString& path)
{
    return ProjectManager::instance().loadProject(path);
}

bool ProjectTreePanel::saveProject(const QString& path)
{
    Q_UNUSED(path)
    return ProjectManager::instance().saveProject();
}

void ProjectTreePanel::clear()
{
    objectLabels_.reset();
    contextItem_ = nullptr;
    contextBucketFolder_.clear();
    treeWidget_->clear();
    itemMap_.clear();
    reverseItemMap_.clear();
    folderMap_.clear();
    reverseFolderMap_.clear();
    folderRowPaths_.clear();
    categoryFolders_.clear();
    busTreeRootItem_ = nullptr;
}

QString ProjectTreePanel::deriveFolderPathForBucket(const QString& bucketFolderName) const
{
    const FolderRowAddress address =
        folderAddressForRow(treeWidget_ ? treeWidget_->currentItem() : nullptr);
    if (!address.valid || address.bucketFolderName != bucketFolderName) return {};
    return address.relPath;
}

ProjectTreePanel::FolderRowAddress
ProjectTreePanel::folderAddressForRow(QTreeWidgetItem* row) const
{
    FolderRowAddress address;
    if (!row) return address;

    QStringList segments;
    QTreeWidgetItem* walk = row;
    while (walk && bucketKeyForTreeItem(walk).isEmpty()) {
        ProjectItem* item = itemMap_.value(walk, nullptr);
        if (item) return address;
        const PhysicalFolder* folder = folderMap_.value(walk, nullptr);
        if (!folder) return address;
        const QString segment = folder->name;
        // A segment reaches mkpath verbatim once the address is saved, so a
        // name the filesystem would read as a traversal disqualifies the whole
        // row rather than silently relocating the object.
        if (!NameValidator::validateFileName(segment)
            || segment == QStringLiteral(".") || segment == QStringLiteral("..")) {
            return address;
        }
        segments.prepend(segment);
        walk = walk->parent();
    }
    if (!walk) return address;

    address.valid            = true;
    address.isBucketRoot     = segments.isEmpty();
    address.bucketFolderName = bucketKeyForTreeItem(walk);
    address.relPath          = segments.join(QChar('/'));
    return address;
}

bool ProjectTreePanel::canAddressItemAt(const ProjectItem* item,
                                        const FolderRowAddress& address)
{
    if (!item || !address.valid) return false;
    if (!ItemTypeCatalog::isFolderAddressable(item->type)) return false;
    return ItemTypeCatalog::folderNameOf(ItemTypeCatalog::bucketOf(item->type))
           == address.bucketFolderName;
}

ProjectItem* ProjectTreePanel::selectedItem() const
{
    QTreeWidgetItem* currentItem = treeWidget_->currentItem();
    if (!currentItem) return nullptr;

    return itemMap_.value(currentItem, nullptr);
}

QList<ProjectItem*> ProjectTreePanel::selectedItems() const
{
    QList<ProjectItem*> result;
    for (QTreeWidgetItem* item : treeWidget_->selectedItems())
    {
        if (ProjectItem* projItem = itemMap_.value(item, nullptr))
        {
            result.append(projItem);
        }
    }
    return result;
}

PhysicalFolder* ProjectTreePanel::selectedFolder() const
{
    return folderMap_.value(treeWidget_->currentItem(), nullptr);
}

QList<PhysicalFolder*> ProjectTreePanel::selectedFolders() const
{
    QList<PhysicalFolder*> folders;
    for (QTreeWidgetItem* row : treeWidget_->selectedItems())
        if (PhysicalFolder* folder = folderMap_.value(row)) folders.append(folder);
    return folders;
}

bool ProjectTreePanel::selectFolder(PhysicalFolder* folder)
{
    auto* row = reverseFolderMap_.value(folder);
    if (!row) return false;
    setSelectedEntries({}, {folder});
    onItemClicked(row, 0);
    onSelectionChanged();
    return true;
}

void ProjectTreePanel::selectAll()
{
    treeWidget_->selectAll();
}

void ProjectTreePanel::selectNone()
{
    treeWidget_->clearSelection();
}

void ProjectTreePanel::batchDelete()
{
    ProjectManager::instance().deleteEntries(selectedItems(), selectedFolders());
}

void ProjectTreePanel::batchMove(const QString& targetFolder)
{
    QList<ProjectItem*> items = selectedItems();
    if (items.isEmpty())
    {
        QMessageBox::information(this, tr("Batch Move"), tr("Please select items to move."));
        return;
    }

    ProjectItem* targetParent = nullptr;
    if (auto* project = ProjectManager::instance().currentProject()) {
        targetParent = project->findItemBySelector(targetFolder);
    }
    if (!targetParent || targetParent->parent)
    {
        QMessageBox::warning(this, tr("Batch Move"),
            tr("Target Pack is missing or ambiguous: %1. Select it by GUID.").arg(targetFolder));
        return;
    }

    items.removeIf([targetParent](const ProjectItem* item) { return item->parent == targetParent; });
    if (!items.isEmpty() && ProjectManager::instance().moveItems(items, targetParent))
    {
        qDebug() << "[ProjectTreePanel] Moved" << items.size() << "items to" << targetFolder;
    }
}

void ProjectTreePanel::updateSearchLabels()
{
    const auto* project = ProjectManager::instance().currentProject();
    objectLabels_ = project ? std::make_unique<ProjectObjectLabels>(*project) : nullptr;
    if (!project) return;
    for (auto it = itemMap_.cbegin(); it != itemMap_.cend(); ++it)
        updateTreeItem(it.key(), it.value());
    for (auto it = folderMap_.cbegin(); it != folderMap_.cend(); ++it)
        updateFolderRow(it.key(), it.value());
}

void ProjectTreePanel::applyVisibility()
{
    updateBucketRows();
    updateSearchLabels();
    const QString searchText = searchBox_ ? searchBox_->text() : QString();
    const bool hasSearch = !searchText.isEmpty();

    QSet<QTreeWidgetItem*> categoryRoots;
    for (auto it = categoryFolders_.cbegin(); it != categoryFolders_.cend(); ++it) {
        categoryRoots.insert(it.value());
    }

    std::function<bool(QTreeWidgetItem*)> apply = [&](QTreeWidgetItem* item) -> bool {
        ProjectItem* projItem = itemMap_.value(item, nullptr);
        const bool isCategoryRoot = categoryRoots.contains(item);
        const bool isScaffold = (!projItem && !folderMap_.contains(item) && !isCategoryRoot);

        bool hasVisibleChild = false;
        for (int i = 0; i < item->childCount(); ++i) {
            if (apply(item->child(i))) hasVisibleChild = true;
        }

        if (isCategoryRoot) {
            item->setHidden(false);
            return true;
        }

        bool visible;
        if (!hasSearch) {
            visible = true;
        } else if (isScaffold) {
            visible = hasVisibleChild;
        } else {
            visible = item->text(0).contains(searchText, Qt::CaseInsensitive)
                    || hasVisibleChild;
        }

        item->setHidden(!visible);
        if (visible && hasSearch) {
            item->setExpanded(true);
        }
        return visible;
    };

    for (int i = 0; i < treeWidget_->topLevelItemCount(); ++i) {
        apply(treeWidget_->topLevelItem(i));
    }
}

void ProjectTreePanel::onItemClicked(QTreeWidgetItem* item, int column)
{
    Q_UNUSED(column)

    if (item && item->data(0, kBucketRole).toBool())
    {
        item->setExpanded(!item->isExpanded());
        return;
    }

    if (ProjectItem* projItem = itemMap_.value(item, nullptr))
    {
        const auto items = selectedItems();
        emit itemSelected(items.contains(projItem) ? projItem : items.value(0));

        switch (projItem->type)
        {
            case ProjectItem::Type::Action:
                emit actionSelected(projItem->name);
                break;
            case ProjectItem::Type::Sound:
                emit audioFileSelected(
                    SoundSourceEditing::activeRelativePath(
                        *projItem,
                        PlatformContextService::instance().context().primary,
                        SoundSourceEditing::previewLanguage(
                            *projItem,
                            LanguageContextService::instance().language())));
                break;
            default:
                break;
        }
    }
    else if (folderMap_.contains(item)) {
        emit itemSelected(nullptr);
    }
}

void ProjectTreePanel::onItemDoubleClicked(QTreeWidgetItem* item, int column)
{
    Q_UNUSED(column)

    if (item && item->data(0, kBucketRole).toBool()) {
        item->setExpanded(!item->isExpanded());
        return;
    }

    if (ProjectItem* projItem = itemMap_.value(item, nullptr))
        emit itemDoubleClicked(projItem);
    else if (folderMap_.contains(item))
        item->setExpanded(!item->isExpanded());
}

void ProjectTreePanel::onItemExpanded(QTreeWidgetItem* item)
{
    if (PhysicalFolder* folder = folderMap_.value(item)) setFolderExpanded(folder, true);
    if (ProjectItem* projItem = itemMap_.value(item, nullptr))
    {
        projItem->expanded = true;
    }
}

void ProjectTreePanel::onItemCollapsed(QTreeWidgetItem* item)
{
    if (PhysicalFolder* folder = folderMap_.value(item)) setFolderExpanded(folder, false);
    if (ProjectItem* projItem = itemMap_.value(item, nullptr))
    {
        projItem->expanded = false;
    }
}

void ProjectTreePanel::onSearchTextChanged(const QString& )
{
    applyVisibility();
}

void ProjectTreePanel::onContextMenu(const QPoint& pos)
{
    QTreeWidgetItem* item = treeWidget_->itemAt(pos);
    ProjectItem* projItem = item ? itemMap_.value(item, nullptr) : nullptr;
    prepareItemContextMenu(item, projItem);
    contextMenu_->exec(treeWidget_->viewport()->mapToGlobal(pos));
    if (menuController_)
        menuController_->clearMenuContext();
}

void ProjectTreePanel::importFilesToTarget(const QStringList& files, ProjectItem* contextItem, const QString& bucketKey)
{
    ProjectData* project = ProjectManager::instance().currentProject();
    if (!project)
        return;

    ProjectItem* targetSound = contextItem && contextItem->type == ProjectItem::Type::Sound
        ? contextItem : nullptr;

    ProjectFileBinding* targetBinding = nullptr;
    ProjectItem* importParent = nullptr;
    bool rootLevelImport = false;

    if (targetSound)
    {
        importParent = targetSound;
        targetBinding = findFileBindingForItem(*project, targetSound);
    }
    else if (contextItem)
    {
        const auto placement = AudioImportGate::resolveImportParent(contextItem);
        if (placement.kind == ImportPlacementKind::Rejected)
        {
            QMessageBox::warning(this, tr("Import Audio"),
                tr("'%1' cannot contain imported audio.").arg(contextItem->name));
            return;
        }
        importParent = placement.parent;
        targetBinding = findFileBindingForItem(*project, importParent);
    }
    else if (bucketKey.isEmpty() || bucketKey == QString::fromLatin1(ProjectManager::kLayers))
    {
        rootLevelImport = true;
    }
    else
    {
        QMessageBox::warning(this, tr("Import Audio"),
            tr("'%1' cannot contain imported audio.").arg(bucketKey));
        return;
    }

    if (!rootLevelImport && (!targetBinding || !importParent))
    {
        QMessageBox::warning(this, tr("Import Audio"),
            tr("Could not resolve an import target for the dropped files."));
        return;
    }

    auto dispatchImport = [&](int& successCount, int& failCount) {
        if (targetSound) {
            auto* command = ImportSoundSourcesCommand::pushImport(
                files, targetBinding, targetSound);
            successCount = command->successCount();
            failCount = command->failCount();
        } else if (rootLevelImport) {
            auto* command = ImportAssetsCommand::pushImportAtRoot(files);
            successCount = command->successCount();
            failCount = command->failCount();
        } else {
            auto* command = ImportAssetsCommand::pushImport(
                files, targetBinding, importParent);
            successCount = command->successCount();
            failCount = command->failCount();
        }
    };

    int successCount = 0;
    int failCount = 0;

    if (files.size() > 5)
    {
        QProgressDialog progress(tr("Importing %1 audio files...").arg(files.size()),
                                 QString(), 0, 0, this);
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(0);
        progress.show();
        QApplication::processEvents();

        dispatchImport(successCount, failCount);

        progress.close();
    }
    else
    {
        dispatchImport(successCount, failCount);
    }

    if (failCount > 0)
    {
        QMessageBox::warning(this, tr("Import Audio"),
            tr("Imported %1 file(s), %2 failed.\nSee log for details.")
                .arg(successCount)
                .arg(failCount));
    }

    if (importParent)
    {
        QTreeWidgetItem* parentTreeItem = findTreeItem(importParent);
        if (parentTreeItem)
        {
            parentTreeItem->setExpanded(true);
        }
    }
}

void ProjectTreePanel::onExternalFilesDropped(const QStringList& filePaths, QTreeWidgetItem* targetItem, const QString& bucketKey)
{
    if (folderMap_.contains(targetItem)) return;
    ProjectItem* contextItem = nullptr;

    if (targetItem)
    {
        contextItem = itemMap_.value(targetItem, nullptr);
    }

    importFilesToTarget(filePaths, contextItem, bucketKey);
}

void ProjectTreePanel::onRename()
{
    const auto ctx = captureCommandContext();
    if (hasSingleFolderTarget(ctx)) {
        if (auto* row = reverseFolderMap_.value(ctx.contextFolder)) {
            treeWidget_->scrollToItem(row);
            treeWidget_->editItem(row, 0);
        }
        return;
    }
    if (!hasSingleObjectTarget(ctx))
        return;
    ProjectItem* item = menuAnchorItem();
    if (!item) return;
    if (auto* row = findTreeItem(item)) {
        treeWidget_->scrollToItem(row);
        treeWidget_->editItem(row, 0);
    }
}

void ProjectTreePanel::onDelete()
{
    const QList<ProjectItem*> items = uniqueMenuItems();
    const auto folders = menuFolders();
    ProjectManager::instance().deleteEntries(items, folders);
}

void ProjectTreePanel::onDuplicate()
{
    if (!menuFolders().isEmpty()) return;
    const QList<ProjectItem*> items = uniqueMenuItems();
    auto& pm = ProjectManager::instance();
    if (!pm.canDuplicateItems(items))
        return;
    const std::vector<ProjectItem*> created = pm.duplicateItems(items);
    QList<ProjectItem*> results;
    for (ProjectItem* item : created)
        results.append(item);
    if (!results.isEmpty())
        selectResultItems(results);
}

void ProjectTreePanel::onCut()
{
    if (!menuFolders().isEmpty()) return;
    const QList<ProjectItem*> items = uniqueMenuItems();
    if (!ProjectManager::instance().canSetClipboard(items))
        return;
    ProjectManager::instance().setClipboard(items, ProjectManager::ClipboardOp::Cut);
}

void ProjectTreePanel::onCopy()
{
    if (!menuFolders().isEmpty()) return;
    const QList<ProjectItem*> items = uniqueMenuItems();
    if (!ProjectManager::instance().canSetClipboard(items))
        return;
    ProjectManager::instance().setClipboard(items, ProjectManager::ClipboardOp::Copy);
}

void ProjectTreePanel::onPasteProperties()
{
    if (!menuFolders().isEmpty()) return;
    const QList<ProjectItem*> items = uniqueMenuItems();
    if (!ProjectManager::instance().canPastePropertiesFromClipboard(items))
        return;
    ProjectManager::instance().pastePropertiesFromClipboard(items);
}

void ProjectTreePanel::onPaste()
{
    const auto ctx = captureCommandContext();
    if (!canPasteAtContext(ctx))
        return;
    std::vector<ProjectItem*> created;
    auto& pm = ProjectManager::instance();
    ProjectItem* target = ctx.kind == ProjectItemMenuController::ContextKind::BucketRoot
        ? nullptr : menuAnchorItem();
    if (ctx.kind == ProjectItemMenuController::ContextKind::BucketRoot) {
        created = pm.pasteClipboardAtBucketRoot(ctx.bucketFolderName);
    } else {
        if (!target || pm.clipboardEmpty())
            return;
        created = pm.pasteClipboard(target);
    }
    QList<ProjectItem*> results;
    for (ProjectItem* item : created)
        results.append(item);
    if (!results.isEmpty())
        selectResultItems(results);
}

void ProjectTreePanel::pasteInto(ProjectItem* target)
{
    auto& pm = ProjectManager::instance();
    if (!target || pm.clipboardEmpty()) return;
    std::vector<ProjectItem*> created = pm.pasteClipboard(target);
    QList<ProjectItem*> results;
    for (ProjectItem* item : created)
        results.append(item);
    if (!results.isEmpty())
        selectResultItems(results);
}

void ProjectTreePanel::onSelectionChanged()
{
    if (!treeWidget_)
        return;
    if (selectedItems().isEmpty())
        emit itemSelected(nullptr);
    emit objectSelectionChanged(selectedItems());
}

void ProjectTreePanel::onSelectAll()
{
    selectAll();
}

void ProjectTreePanel::onSelectNone()
{
    selectNone();
}

bool ProjectTreePanel::wouldCreateCycle(const ProjectItem* item, const ProjectItem* target)
{
    if (!item || !target) return false;

    for (const ProjectItem* cursor = target; cursor; cursor = cursor->parent) {
        if (cursor == item) {
            return true;
        }
    }
    return false;
}

void ProjectTreePanel::onInternalMoveRequested(ProjectItem* projectItem,
                                               ProjectItem* targetParent,
                                               const QString& bucketKey,
                                               const QString& folderPath,
                                               bool folderAddressValid)
{
    if (!projectItem) return;

    if (folderAddressValid
        && ItemTypeCatalog::isFolderAddressable(projectItem->type)
        && ItemTypeCatalog::folderNameOf(ItemTypeCatalog::bucketOf(projectItem->type))
               == bucketKey) {
        if (!projectItem->parent
            && ProjectManager::instance().currentProject()->itemFolderPath(projectItem) == folderPath)
            return;
        ProjectManager::instance().moveEntriesToFolder({projectItem}, {}, bucketKey, folderPath);
        return;
    }

    if (!targetParent) {
        if (bucketKey.isEmpty()) {
            qWarning() << "[ProjectTreePanel] Invalid drop target";
            return;
        }
        ProjectManager::instance().moveItemToBucketRoot(projectItem, bucketKey);
        return;
    }

    if (projectItem == targetParent) return;

    if (wouldCreateCycle(projectItem, targetParent)) {
        qWarning() << "[ProjectTreePanel] Cannot move item to its own descendant";
        return;
    }

    qDebug() << "[ProjectTreePanel] Moving item:" << projectItem->name
             << "to:" << targetParent->name;

    ProjectManager::instance().moveItems({projectItem}, targetParent);
}

void ProjectTreePanel::updateTreeItem(QTreeWidgetItem* treeItem, ProjectItem* projectItem)
{
    if (!treeItem || !projectItem) return;

    const bool dirty = projectItem->isDirtyObject();
    const QString name = objectLabels_ && searchBox_ && !searchBox_->text().isEmpty()
        ? objectLabels_->label(*projectItem) : projectItem->name;
    treeItem->setText(0, dirty ? name + QStringLiteral(" *") : name);

    QString tooltip = QString("%1 - %2").arg(typeToString(projectItem->type), projectItem->name);
    const QString language = SoundSourceEditing::previewLanguage(
        *projectItem, LanguageContextService::instance().language());
    const uint32_t activeSourceID = projectItem->type == ProjectItem::Type::Sound
        ? SoundSourceEditing::activeSourceId(
              *projectItem, PlatformContextService::instance().context().primary, language)
        : 0;
    if (projectItem->type == ProjectItem::Type::Sound)
        treeItem->setData(0, Qt::ForegroundRole, QVariant());
    const QString activePath = projectItem->type == ProjectItem::Type::Sound
        ? SoundSourceEditing::activeRelativePath(
              *projectItem, PlatformContextService::instance().context().primary, language)
        : QString{};
    if (!activePath.isEmpty())
    {
        tooltip = tr("Sound: %1\nSource: %2").arg(projectItem->name, activePath);
    }

    QColor statusColor;
    if (activeSourceID != 0)
    {
        MediaStatus mediaStatus = MediaPool::instance().cachedStatus(activeSourceID);
        const auto& tc = ThemeManager::instance().colors();
        QString statusLabel;

        switch (mediaStatus) {
            case MediaStatus::Converted:
                statusLabel = tr("Converted");
                break;
            case MediaStatus::Imported:
                statusLabel = tr("Not Converted");
                break;
            case MediaStatus::Outdated:
                statusColor = tc.warning;
                statusLabel = tr("Outdated");
                break;
            case MediaStatus::Missing:
                statusColor = tc.error;
                statusLabel = tr("Missing");
                break;
            case MediaStatus::Orphaned:
                statusColor = tc.textMuted;
                statusLabel = tr("Orphaned");
                break;
            case MediaStatus::Converting:
                statusLabel = tr("Converting...");
                break;
            case MediaStatus::Error:
                statusColor = tc.error;
                statusLabel = tr("Error");
                break;
        }

        if (statusColor.isValid())
            treeItem->setForeground(0, statusColor);
        else
            treeItem->setData(0, Qt::ForegroundRole, QVariant());
        tooltip += QStringLiteral("\nMedia Status: ") + statusLabel;

        const AssetInfo* asset = AssetRegistry::instance().getAsset(activeSourceID);
        if (asset) {
            tooltip += QStringLiteral("\nFormat: ") + asset->metadata.originalFormat;
            tooltip += QStringLiteral("\nDuration: ") +
                       QString::number(asset->metadata.durationSec, 'f', 2) + QStringLiteral("s");
            tooltip += QStringLiteral("\nSample Rate: ") +
                       QString::number(asset->metadata.sampleRate) + QStringLiteral(" Hz");
            tooltip += QStringLiteral("\nChannels: ") +
                       QString::number(asset->metadata.channels);
        }
    }

    treeItem->setIcon(0, decorateItemIcon(iconForItem(*projectItem, statusColor), projectItem));

    if (const auto* project = ProjectManager::instance().currentProject()) {
        QString path = objectLabels_ ? objectLabels_->path(*projectItem) : QString{};
        if (path.isEmpty()) path = project->itemPath(projectItem);
        tooltip += QStringLiteral("\nPath: ") + path;
    }
    tooltip += QStringLiteral("\nGUID: ") + projectItem->guid.toString(QUuid::WithoutBraces);
    treeItem->setToolTip(0, tooltip);

    QFont font = treeItem->font(0);

    font.setItalic(dirty);

    treeItem->setFont(0, font);
}

QTreeWidgetItem* ProjectTreePanel::createTreeItem(ProjectItem* item)
{
    QTreeWidgetItem* treeItem = new QTreeWidgetItem();
    treeItem->setFlags(treeItem->flags() | Qt::ItemIsEditable);

    updateTreeItem(treeItem, item);

    return treeItem;
}

QString ProjectTreePanel::typeToString(ProjectItem::Type type) const
{
    return ItemTypeCatalog::displayName(type);
}

void ProjectTreePanel::onFontSettingsChanged(PanelId panelId, int size)
{
    if (panelId == settingsPanelId_)
    {
        QFont font = treeWidget_->font();
        font.setPointSize(size);
        treeWidget_->setFont(font);
    }
}

}
