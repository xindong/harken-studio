
#include "ProjectItemMenuController.h"

#include <QMenu>
#include <QAction>
#include <QWidget>
#include <algorithm>

#include "Core/ItemTypeCatalog.h"
#include "Core/ProjectManager.h"
#include "Core/ShortcutManager.h"

namespace VNX {

namespace {

constexpr struct { const char* label; SelectionMode mode; } kContainerModes[] = {
    { "Random",           SelectionMode::Random },
    { "Sequence",         SelectionMode::Sequence },
    { "Match",            SelectionMode::Match },
    { "Blend",            SelectionMode::Blend },
};

void refreshSeparators(QMenu* menu)
{
    if (!menu) return;
    QAction* separator = nullptr;
    bool hasVisibleItem = false;
    for (QAction* action : menu->actions()) {
        if (action->isSeparator()) {
            action->setVisible(false);
            if (hasVisibleItem && !separator) separator = action;
        } else if (action->isVisible()) {
            if (separator) separator->setVisible(true);
            separator = nullptr;
            hasVisibleItem = true;
            refreshSeparators(action->menu());
        }
    }
}

}

const std::vector<ProjectItemMenuController::ItemCreationSpec>&
ProjectItemMenuController::specs()
{
    static const std::vector<ItemCreationSpec> kSpecs = [] {
        std::vector<ItemCreationSpec> out;
        for (const ItemTypeCatalog::CreationSpec& spec : ItemTypeCatalog::creationSpecs()) {
            out.push_back({ spec.type, spec.defaultName,
                            spec.parentPolicy, spec.requiredParent,
                            spec.selectAfter, spec.separatorAfter, nullptr,
                            spec.comingSoon });
        }
        return out;
    }();
    return kSpecs;
}

ProjectItemMenuController::ProjectItemMenuController(Callbacks cb, QObject* parent)
    : QObject(parent), cb_(std::move(cb))
{
    ShortcutManager& shortcuts = ShortcutManager::instance();
    connect(&shortcuts, &ShortcutManager::shortcutChanged, this,
            [this](const QString& id, const QKeySequence&) { refreshShortcutHints(id); });
    connect(&shortcuts, &ShortcutManager::shortcutsReset, this,
            [this]() { refreshShortcutHints(); });
}

void ProjectItemMenuController::refreshShortcutHints(const QString& id)
{
    ShortcutManager& shortcuts = ShortcutManager::instance();
    for (size_t i = 0; i < standardSpecs_.size() && i < standardActions_.size(); ++i) {
        const StandardItemSpec& spec = standardSpecs_[i];
        QAction* action = standardActions_[i];
        if (!action || spec.shortcutHintId.isEmpty()
            || (!id.isEmpty() && spec.shortcutHintId != id))
            continue;

        const QString hint = shortcuts.shortcut(spec.shortcutHintId)
                                 .toString(QKeySequence::NativeText);
        action->setText(hint.isEmpty() ? spec.label
                                       : spec.label + QLatin1Char('\t') + hint);
    }
}

void ProjectItemMenuController::buildNewChildMenu(QMenu* parentMenu)
{
    if (!parentMenu) return;

    newChildMenu_ = parentMenu->addMenu(tr("New Child"));
    const auto& types = ItemTypeCatalog::allTypes();
    buildCreationMenu(newChildMenu_, {types.begin(), types.end()});
    newFolderAction_ = newChildMenu_->addAction(tr("Folder"), this, [this]() { createFolder(); });
}

void ProjectItemMenuController::buildCreationMenu(QMenu* menu, const std::vector<ProjectItem::Type>& types)
{
    if (!menu) return;

    const auto& kSpecs = specs();
    newChildActions_.assign(kSpecs.size(), nullptr);

    for (size_t i = 0; i < kSpecs.size(); ++i) {
        const auto& spec = kSpecs[i];
        if (spec.comingSoon || std::find(types.begin(), types.end(), spec.type) == types.end()) continue;
        QAction* action = nullptr;
        if (spec.type == ProjectItem::Type::Container) {
            QMenu* sub = menu->addMenu(typeDisplayName(spec.type));
            for (const auto& entry : kContainerModes) {
                sub->addAction(tr(entry.label), this, [this, mode = entry.mode]() {
                    createContainerWithMode(mode);
                });
            }
            action = sub->menuAction();
        } else if (spec.type == ProjectItem::Type::Variable) {
            QMenu* sub = menu->addMenu(typeDisplayName(spec.type));
            addVariableKindActions(sub, [this, i](VariableKind kind) {
                createFromSpec(specs()[i], kind);
            });
            action = sub->menuAction();
        } else {
            action = menu->addAction(typeDisplayName(spec.type), this, [this, i]() {
                createFromSpec(specs()[i]);
            });
        }
        newChildActions_[i] = action;
        if (spec.separatorAfter) menu->addSeparator();
    }
}

void ProjectItemMenuController::addVariableKindActions(
    QMenu* menu, const std::function<void(VariableKind)>& create)
{
    menu->addAction(tr("Smooth"), menu, [create] { create(VariableKind::Smooth); });
    menu->addAction(tr("Step"), menu, [create] { create(VariableKind::Step); });
}

void ProjectItemMenuController::buildStandardItems(QMenu* parentMenu,
                                                   std::vector<StandardItemSpec> items)
{
    if (!parentMenu) return;

    standardMenu_ = parentMenu;
    standardSpecs_ = std::move(items);
    standardActions_.assign(standardSpecs_.size(), nullptr);
    standardSubmenus_.assign(standardSpecs_.size(), nullptr);

    for (size_t i = 0; i < standardSpecs_.size(); ++i) {
        const StandardItemSpec& spec = standardSpecs_[i];
        QAction* action = nullptr;
        if (spec.buildSubmenu) {
            QMenu* sub = parentMenu->addMenu(spec.label);
            standardSubmenus_[i] = sub;
            action = sub->menuAction();
        } else {
            action = parentMenu->addAction(spec.label);
            if (spec.handler) {
                auto handler = spec.handler;
                connect(action, &QAction::triggered, this, [handler]() { handler(); });
            }
        }
        action->setToolTip(QString());
        standardActions_[i] = action;
        if (spec.separatorAfter) parentMenu->addSeparator();
    }
    refreshShortcutHints();
}

void ProjectItemMenuController::setMenuContext(MenuContext context)
{
    menuContext_ = std::move(context);
    hasMenuContext_ = true;
    applyMenuContext();
}

void ProjectItemMenuController::updateActionVisibility(ProjectItem* contextItem,
                                                       const QString& bucketFolderName,
                                                       const QString& folderPath)
{
    MenuContext context;
    context.contextItemGuid = contextItem ? contextItem->guid : QUuid{};
    context.bucketFolderName = bucketFolderName;
    context.folderPath = folderPath;
    context.useSelectionForCreation = false;
    if (cb_.getSelection) {
        for (ProjectItem* item : cb_.getSelection()) {
            if (item && !item->guid.isNull())
                context.selectionGuids.append(item->guid);
        }
    }
    if (contextItem) {
        context.kind = ContextKind::Object;
    } else if (!bucketFolderName.isEmpty() && folderPath.isEmpty()) {
        context.kind = ContextKind::BucketRoot;
    } else if (!bucketFolderName.isEmpty()) {
        context.kind = ContextKind::Folder;
        if (const auto* project = ProjectManager::instance().currentProject())
            context.contextFolder = const_cast<PhysicalFolder*>(
                project->findFolderByPath(bucketFolderName, folderPath));
    } else {
        context.kind = ContextKind::Empty;
    }
    setMenuContext(std::move(context));
}

void ProjectItemMenuController::applyMenuContext()
{
    using PType = ProjectItem::Type;
    auto& pm = ProjectManager::instance();
    ProjectItem* contextItem = pm.findItemByGuid(menuContext_.contextItemGuid);
    const QString& bucketFolderName = menuContext_.bucketFolderName;
    const QString& folderPath = menuContext_.folderPath;
    const bool useSelection = menuContext_.useSelectionForCreation;

    const bool folderAddressRow = !bucketFolderName.isEmpty()
        && !contextItem;
    const bool bucketRow = menuContext_.kind == ContextKind::BucketRoot
        || (!contextItem && !bucketFolderName.isEmpty() && folderPath.isEmpty());
    const bool busBucket = bucketRow && bucketFolderName == QLatin1String(ItemTypeCatalog::kBussesFolderName);

    std::vector<PType> allowedSet;
    if (useSelection && !menuContext_.selectionGuids.isEmpty()) {
        for (const QUuid& guid : menuContext_.selectionGuids) {
            ProjectItem* item = pm.findItemByGuid(guid);
            if (!item) {
                allowedSet.clear();
                break;
            }
            const auto types = ProjectManager::allowedChildTypesOf(item->type);
            allowedSet.insert(allowedSet.end(), types.begin(), types.end());
        }
    } else if (busBucket) {
        allowedSet = ProjectManager::allowedChildTypesOf(PType::Bus);
    } else if (folderAddressRow) {
        allowedSet = ProjectManager::allowedTopLevelTypesForBucket(bucketFolderName);
    } else if (contextItem) {
        allowedSet = ProjectManager::allowedChildTypesOf(contextItem->type);
    }

    auto hasType = [&](PType t) {
        return std::find(allowedSet.begin(), allowedSet.end(), t) != allowedSet.end();
    };

    auto computeEnabled = [&](const ItemCreationSpec& spec) {
        if (!hasType(spec.type) || spec.comingSoon)
            return false;
        if (useSelection)
            return canCreateFromSpec(spec);
        if ((menuContext_.kind == ContextKind::Object || menuContext_.kind == ContextKind::Folder)
            && menuContext_.selectionGuids.size() > 1) {
            return false;
        }
        switch (spec.parentPolicy) {
            case CreateParentPolicy::DefaultForType:
                return true;
            case CreateParentPolicy::SelectedItem:
                if (busBucket && spec.type == PType::Bus)
                    return ProjectManager::instance().getMasterBusItem() != nullptr;
                if (contextItem != nullptr) return true;
                return false;
            case CreateParentPolicy::SelectedGuarded:
                return contextItem != nullptr && contextItem->type == spec.requiredParent;
        }
        return true;
    };

    const auto& kSpecs = specs();
    bool anyChildVisible = false;
    bool anyChildEnabled = false;
    for (size_t i = 0; i < kSpecs.size() && i < newChildActions_.size(); ++i) {
        const ItemCreationSpec& spec = kSpecs[i];
        const bool allowed = hasType(spec.type) && !spec.comingSoon;
        const bool enabled = computeEnabled(spec);
        if (QAction* act = newChildActions_[i]) {
            act->setVisible(allowed);
            act->setEnabled(enabled);
            act->setToolTip(QString());
            if (act->menu()) {
                const QList<QAction*> modeActions = act->menu()->actions();
                for (QAction* modeAct : modeActions) {
                    if (!modeAct || modeAct->isSeparator()) continue;
                    modeAct->setVisible(allowed);
                    modeAct->setEnabled(enabled);
                    modeAct->setToolTip(QString());
                }
            }
            anyChildVisible |= allowed;
            anyChildEnabled |= allowed && enabled;
        }
    }

    if (newFolderAction_) {
        const bool visible = folderAddressRow && ProjectManager::bucketAllowsFolder(bucketFolderName);
        const bool enabled = visible && (!useSelection
            || (menuContext_.selectionGuids.isEmpty() && menuContext_.selectionFolders.size() <= 1))
            && (!menuContext_.contextFolder || (pm.currentProject()
                && !pm.currentProject()->folderBucket(menuContext_.contextFolder).isEmpty()));
        newFolderAction_->setVisible(visible);
        newFolderAction_->setEnabled(enabled);
        anyChildVisible |= visible;
        anyChildEnabled |= enabled;
    }

    if (newChildMenu_) {
        newChildMenu_->setTitle(bucketRow ? tr("Create") : tr("New Child"));
        newChildMenu_->menuAction()->setVisible(useSelection ? anyChildVisible : anyChildEnabled);
        newChildMenu_->menuAction()->setEnabled(anyChildEnabled);
        newChildMenu_->menuAction()->setToolTip(QString());
        refreshSeparators(newChildMenu_);
    }

    refreshStandardSubmenus();
}

void ProjectItemMenuController::refreshStandardSubmenus()
{
    for (size_t i = 0; i < standardSpecs_.size() && i < standardActions_.size(); ++i) {
        QAction* act = standardActions_[i];
        if (!act) continue;
        const StandardItemSpec& spec = standardSpecs_[i];
        const bool visible = !spec.canShow || spec.canShow(menuContext_);
        act->setVisible(visible);
        act->setToolTip(QString());
        const bool enabled = visible && (!spec.canEnable || spec.canEnable(menuContext_));
        if (spec.buildSubmenu && i < standardSubmenus_.size()) {
            if (QMenu* sub = standardSubmenus_[i]) {
                sub->clear();
                spec.buildSubmenu(sub, menuContext_);
                bool anyVisible = false;
                bool anyEnabled = false;
                const QList<QAction*> entries = sub->actions();
                for (QAction* entry : entries) {
                    if (!entry || entry->isSeparator()) continue;
                    entry->setToolTip(QString());
                    anyVisible |= entry->isVisible();
                    anyEnabled |= entry->isVisible() && entry->isEnabled();
                }
                act->setVisible(visible && anyVisible);
                act->setEnabled(enabled && anyEnabled);
            } else {
                act->setEnabled(enabled);
            }
        } else {
            act->setEnabled(enabled);
        }
    }

    refreshSeparators(standardMenu_);
}

void ProjectItemMenuController::clearMenuContext()
{
    menuContext_ = {};
    hasMenuContext_ = false;
}

ProjectItem* ProjectItemMenuController::resolveParent(const ItemCreationSpec& spec) const
{
    if (cb_.creationParent) return cb_.creationParent(spec.type);
    if (hasMenuContext_ && menuContext_.contextItemGuid.isNull() && spec.type == ProjectItem::Type::Bus
        && menuContext_.bucketFolderName == QLatin1String(ItemTypeCatalog::kBussesFolderName))
        return ProjectManager::instance().getMasterBusItem();
    ProjectItem* anchor = hasMenuContext_
        ? ProjectManager::instance().findItemByGuid(menuContext_.contextItemGuid)
        : (cb_.getSelected ? cb_.getSelected() : nullptr);
    switch (spec.parentPolicy) {
        case CreateParentPolicy::DefaultForType:
            return resolveDefaultParentForType(spec.type, anchor);
        case CreateParentPolicy::SelectedItem:
            return anchor;
        case CreateParentPolicy::SelectedGuarded:
            return (anchor && anchor->type == spec.requiredParent) ? anchor : nullptr;
    }
    return nullptr;
}

bool ProjectItemMenuController::canCreateFromSpec(const ItemCreationSpec& spec) const
{
    auto& pm = ProjectManager::instance();
    const auto& ctx = menuContext_;
    if (spec.comingSoon || !pm.isProjectOpen()) return false;
    ProjectItem* anchor = pm.findItemByGuid(ctx.contextItemGuid);
    if (ctx.kind == ContextKind::Object) {
        if (!anchor || ctx.selectionGuids.size() != 1
            || ctx.selectionGuids.front() != anchor->guid
            || !ctx.selectionFolders.isEmpty()) return false;
    } else if (ctx.kind == ContextKind::Folder) {
        if (!ctx.contextFolder || !ctx.selectionGuids.isEmpty()
            || ctx.selectionFolders.size() != 1
            || ctx.selectionFolders.front() != ctx.contextFolder
            || pm.currentProject()->folderBucket(ctx.contextFolder).isEmpty()) return false;
    } else if (ctx.kind != ContextKind::BucketRoot || !ctx.selectionGuids.isEmpty()
               || !ctx.selectionFolders.isEmpty()) {
        return false;
    }
    const bool busBucket = ctx.kind == ContextKind::BucketRoot
        && ctx.bucketFolderName == QLatin1String(ItemTypeCatalog::kBussesFolderName);
    const auto allowed = busBucket
        ? ProjectManager::allowedChildTypesOf(ProjectItem::Type::Bus)
        : !anchor
            ? ProjectManager::allowedTopLevelTypesForBucket(ctx.bucketFolderName)
            : ProjectManager::allowedChildTypesOf(anchor->type);
    return std::find(allowed.begin(), allowed.end(), spec.type) != allowed.end()
        && pm.canCreateItem(resolveParent(spec), spec.type, ctx.bucketFolderName);
}

ProjectItem* ProjectItemMenuController::resolveDefaultParentForType(ProjectItem::Type type,
                                                                    ProjectItem* selected)
{
    if (selected) {
        if (ProjectManager::canBeChildOf(type, selected->type)) {
            return selected;
        }
    }
    return nullptr;
}

ProjectItem* ProjectItemMenuController::createFromSpec(const ItemCreationSpec& spec, VariableKind variableKind)
{
    if (spec.comingSoon) return nullptr;
    const bool selectionContext = hasMenuContext_ && menuContext_.useSelectionForCreation;
    if (selectionContext && !canCreateFromSpec(spec)) return nullptr;
    if (hasMenuContext_ && !menuContext_.contextItemGuid.isNull()
        && !ProjectManager::instance().findItemByGuid(menuContext_.contextItemGuid)) return nullptr;
    if (hasMenuContext_ && menuContext_.contextFolder) {
        const auto* project = ProjectManager::instance().currentProject();
        if (!project || project->folderBucket(menuContext_.contextFolder).isEmpty()) return nullptr;
    }

    ProjectItem* parent = resolveParent(spec);

    const QString name = QString::fromLatin1(spec.defaultNameKey);
    if (spec.parentPolicy == CreateParentPolicy::SelectedGuarded && !parent) return nullptr;

    QString folderPath;
    if (!parent && ItemTypeCatalog::isFolderAddressable(spec.type)) {
        const QString bucketFolderName =
            ItemTypeCatalog::folderNameOf(ItemTypeCatalog::bucketOf(spec.type));
        if (hasMenuContext_ && menuContext_.bucketFolderName == bucketFolderName) {
            folderPath = menuContext_.contextFolder
                ? ProjectManager::instance().currentProject()->folderRelPath(menuContext_.contextFolder)
                : menuContext_.folderPath;
        } else if (cb_.folderPath) {
            folderPath = cb_.folderPath(bucketFolderName);
        }
    }

    ProjectItem* item = ProjectManager::instance().createItem(parent, name, spec.type, folderPath, {},
                                                            nullptr, variableKind);
    if (!item) return nullptr;

    if (spec.postCreate) spec.postCreate(item, cb_);
    if (spec.selectAfter && cb_.selectResult) cb_.selectResult(item);
    return item;
}

ProjectItem* ProjectItemMenuController::createByType(ProjectItem::Type type)
{
    const auto& kSpecs = specs();
    auto it = std::find_if(kSpecs.begin(), kSpecs.end(),
        [type](const ItemCreationSpec& s) { return s.type == type; });
    if (it == kSpecs.end()) return nullptr;
    return createFromSpec(*it);
}

PhysicalFolder* ProjectItemMenuController::createFolder()
{
    auto& manager = ProjectManager::instance();
    const auto* project = manager.currentProject();
    if (!project || (menuContext_.useSelectionForCreation
        && (!menuContext_.selectionGuids.isEmpty() || menuContext_.selectionFolders.size() > 1)))
        return nullptr;
    PhysicalFolder* parent = menuContext_.contextFolder;
    if (menuContext_.kind == ContextKind::Folder && !parent) return nullptr;
    const QString bucket = parent ? project->folderBucket(parent) : menuContext_.bucketFolderName;
    if (bucket.isEmpty() || !ProjectManager::bucketAllowsFolder(bucket)) return nullptr;
    if (menuContext_.kind != ContextKind::Folder && menuContext_.kind != ContextKind::BucketRoot)
        return nullptr;
    PhysicalFolder* folder = manager.createFolder(parent, QStringLiteral("New Folder"), bucket);
    if (folder && cb_.selectFolderResult) cb_.selectFolderResult(folder);
    return folder;
}

ProjectItem* ProjectItemMenuController::createContainerWithMode(SelectionMode mode)
{
    const auto& kSpecs = specs();
    auto it = std::find_if(kSpecs.begin(), kSpecs.end(),
        [](const ItemCreationSpec& s) { return s.type == ProjectItem::Type::Container; });
    if (it == kSpecs.end()) return nullptr;

    // The create command keeps the item's shared_ptr across undo/redo, so a
    // component write here survives the round trip without its own command.
    ItemCreationSpec spec = *it;
    spec.postCreate = [mode](ProjectItem* created, const Callbacks&) {
        if (auto* sel = created->comp<VSelection>()) {
            sel->mode = mode;
            created->markDirty();
            ProjectManager::resyncItemToRuntime(created);
            emit ProjectManager::instance().itemPropertyChanged(
                created, QStringLiteral("mode"));
        }
    };
    return createFromSpec(spec);
}

QString ProjectItemMenuController::typeDisplayName(ProjectItem::Type type)
{
    return ItemTypeCatalog::displayName(type);
}

}
