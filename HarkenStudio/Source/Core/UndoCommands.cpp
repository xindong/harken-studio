
#include "UndoCommands.h"
#include "ContainerBehaviorSemantics.h"
#include "FieldGroupDerivedView.h"
#include "FieldGroupEditing.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "AudioEngine.h"
#include "ProjectPlatformActivity.h"
#include "Components/VAction.h"
#include "Components/VMixNode.h"
#include "Components/VSelection.h"
#include "Components/VTrack.h"
#include "Timeline/TimelineEditing.h"
#include "Components/VBus.h"

#include <algorithm>
#include <cstring>
#include <typeinfo>

#include <QDebug>
#include <QScopedValueRollback>
#include <QUuid>

namespace VNX {

namespace {
std::optional<QSet<QUuid>> authoringWriteSet(const QUndoCommand* command)
{
    if (!command) return std::nullopt;
    const auto* action = dynamic_cast<const RuntimeActionCommand*>(command);
    const auto* writes = action ? action->authoringWriteSet() : nullptr;
    if (action && !writes) return std::nullopt;
    if (!action && (typeid(*command) != typeid(QUndoCommand) || command->childCount() == 0))
        return std::nullopt;
    QSet<QUuid> combined = writes ? *writes : QSet<QUuid>{};
    for (int i = 0; i < command->childCount(); ++i) {
        const auto childWrites = authoringWriteSet(command->child(i));
        if (!childWrites) return std::nullopt;
        combined |= *childWrites;
    }
    return combined;
}

bool canReplayCommand(const QUndoCommand* command)
{
    if (!command) return false;
    if (const auto* action = dynamic_cast<const RuntimeActionCommand*>(command)) {
        if (!action->canReplay()) return false;
    }
    for (int i = 0; i < command->childCount(); ++i) {
        if (!canReplayCommand(command->child(i))) return false;
    }
    return true;
}

ProjectFileBinding* findBindingByRootGuid(ProjectData* proj, const QString& guidStr)
{
    if (!proj || guidStr.isEmpty()) return nullptr;
    for (auto* binding : proj->allFileBindings()) {
        if (binding && binding->rootItem
            && binding->rootItem->guid.toString(QUuid::WithoutBraces) == guidStr) {
            return binding;
        }
    }
    return nullptr;
}

void recomputeObjectPathSubtree(ProjectItem* item)
{
    if (!item) return;
    const QString prefix = item->parent ? item->parent->objectPath : QString();
    item->objectPath = prefix + QStringLiteral("\\") + item->name;
    for (auto& child : item->children) {
        recomputeObjectPathSubtree(child.get());
    }
}
}


bool TransactionalUndoCommand::tryReplay(bool redo)
{
    if (applied_ == redo)
        return true;
    if (!apply(redo))
        return false;
    applied_ = redo;
    return true;
}

UndoRedoManager& UndoRedoManager::instance()
{
    static UndoRedoManager instance;
    return instance;
}

UndoRedoManager::UndoRedoManager()
{
    undoStack_.setUndoLimit(100);
    
    connect(&undoStack_, &QUndoStack::canUndoChanged, this, &UndoRedoManager::refreshReplayAvailability);
    connect(&undoStack_, &QUndoStack::canRedoChanged, this, &UndoRedoManager::refreshReplayAvailability);
    connect(&undoStack_, &QUndoStack::indexChanged, this, &UndoRedoManager::refreshReplayAvailability);
    connect(&ProjectPlatformActivity::instance(), &ProjectPlatformActivity::busyChanged,
            this, &UndoRedoManager::refreshReplayAvailability);
    connect(&undoStack_, &QUndoStack::cleanChanged, this, &UndoRedoManager::cleanChanged);
    connect(&undoStack_, &QUndoStack::indexChanged, this, &UndoRedoManager::indexChanged);
}

QAction* UndoRedoManager::createUndoAction(QObject* parent, const QString& prefix)
{
    return createReplayAction(parent, prefix, false);
}

QAction* UndoRedoManager::createRedoAction(QObject* parent, const QString& prefix)
{
    return createReplayAction(parent, prefix, true);
}

QAction* UndoRedoManager::createReplayAction(QObject* parent, const QString& prefix, bool redo)
{
    auto* action = new QAction(parent);
    const auto update = [this, action, prefix, redo]() {
        const auto label = prefix.isEmpty() ? (redo ? tr("Redo") : tr("Undo")) : prefix;
        const auto text = redo ? undoStack_.redoText() : undoStack_.undoText();
        action->setText(text.isEmpty() ? label : label + QChar(' ') + text);
        action->setEnabled(redo ? canRedo() : canUndo());
    };
    connect(this, &UndoRedoManager::canUndoChanged, action, update);
    connect(this, &UndoRedoManager::canRedoChanged, action, update);
    connect(&undoStack_, &QUndoStack::undoTextChanged, action, update);
    connect(&undoStack_, &QUndoStack::redoTextChanged, action, update);
    connect(action, &QAction::triggered, this, [this, redo]() {
        if (redo) this->redo();
        else this->undo();
    });
    update();
    return action;
}

bool UndoRedoManager::canUndo() const
{
    return undoStack_.canUndo() && canReplayCommand(undoStack_.command(undoStack_.index() - 1));
}

bool UndoRedoManager::canRedo() const
{
    return undoStack_.canRedo() && canReplayCommand(undoStack_.command(undoStack_.index()));
}

bool UndoRedoManager::undo()
{
    if (!canUndo()) return false;
    return replay(false);
}

bool UndoRedoManager::redo()
{
    if (!canRedo()) return false;
    return replay(true);
}

bool UndoRedoManager::replay(bool redo)
{
    auto* command = const_cast<QUndoCommand*>(
        undoStack_.command(undoStack_.index() - (redo ? 0 : 1)));
    const auto writes = authoringWriteSet(command);
    const QScopedValueRollback<const QSet<QUuid>*> scope(
        authoringWriteSet_, writes ? &*writes : nullptr);
    if (auto* transaction = dynamic_cast<TransactionalUndoCommand*>(command)) {
        if (!transaction->tryReplay(redo)) {
            if (transaction->isObsolete()) {
                undoStack_.clear();
                undoStack_.resetClean();
            }
            return false;
        }
    }
    if (redo) undoStack_.redo();
    else undoStack_.undo();
    return true;
}

void UndoRedoManager::refreshReplayAvailability()
{
    emit canUndoChanged(canUndo());
    emit canRedoChanged(canRedo());
}

void UndoRedoManager::push(QUndoCommand* command)
{
    const auto writes = authoringWriteSet(command);
    const QScopedValueRollback<const QSet<QUuid>*> scope(
        authoringWriteSet_, writes ? &*writes : nullptr);
    if (auto* transaction = dynamic_cast<TransactionalUndoCommand*>(command);
        transaction && !transaction->tryReplay(true)) {
        delete command;
        return;
    }
    if (!macroWriteSets_.empty()) {
        auto& macroWrites = macroWriteSets_.back();
        if (macroWrites && writes)
            *macroWrites |= *writes;
        else
            macroWrites.reset();
    }
    undoStack_.push(command);
}

void UndoRedoManager::beginMacro(const QString& text)
{
    const QScopedValueRollback<const QSet<QUuid>*> scope(authoringWriteSet_, nullptr);
    macroWriteSets_.emplace_back(QSet<QUuid>{});
    undoStack_.beginMacro(text);
}

void UndoRedoManager::endMacro()
{
    std::optional<QSet<QUuid>> writes;
    if (!macroWriteSets_.empty()) {
        writes = std::move(macroWriteSets_.back());
        macroWriteSets_.pop_back();
        if (!macroWriteSets_.empty()) {
            auto& parentWrites = macroWriteSets_.back();
            if (parentWrites && writes)
                *parentWrites |= *writes;
            else
                parentWrites.reset();
        }
    }
    const QScopedValueRollback<const QSet<QUuid>*> scope(
        authoringWriteSet_, writes ? &*writes : nullptr);
    undoStack_.endMacro();
}

void UndoRedoManager::clear()
{
    const QScopedValueRollback<const QSet<QUuid>*> scope(authoringWriteSet_, nullptr);
    macroWriteSets_.clear();
    undoStack_.clear();
}

void UndoRedoManager::clearHistory()
{
    const QSet<QUuid> writes;
    const QScopedValueRollback<const QSet<QUuid>*> scope(authoringWriteSet_, &writes);
    macroWriteSets_.clear();
    undoStack_.clear();
}


PropertyChangeCommand::PropertyChangeCommand(
    QObject* target,
    const char* propertyName,
    const QVariant& oldValue,
    const QVariant& newValue,
    const QString& text,
    QUndoCommand* parent)
    : QUndoCommand(text.isEmpty() ? QObject::tr("Change %1").arg(propertyName) : text, parent)
    , target_(target)
    , propertyName_(propertyName)
    , oldValue_(oldValue)
    , newValue_(newValue)
{
}

void PropertyChangeCommand::undo()
{
    if (target_)
        target_->setProperty(propertyName_.constData(), oldValue_);
}

void PropertyChangeCommand::redo()
{
    if (target_)
        target_->setProperty(propertyName_.constData(), newValue_);
}

bool PropertyChangeCommand::mergeWith(const QUndoCommand* other)
{
    if (other->id() != id())
        return false;

    auto cmd = static_cast<const PropertyChangeCommand*>(other);
    if (cmd->target_ != target_ || cmd->propertyName_ != propertyName_)
        return false;

    newValue_ = cmd->newValue_;
    return true;
}


ProjectItemCommand::ProjectItemCommand(
    Operation op,
    const QString& itemId,
    const QString& parentId,
    const QVariant& itemData,
    const QVariant& previousData,
    std::function<void(Operation, const QString&, const QString&, const QVariant&)> executeFn,
    const QString& text,
    QUndoCommand* parent)
    : QUndoCommand(text, parent)
    , op_(op)
    , itemId_(itemId)
    , parentId_(parentId)
    , itemData_(itemData)
    , previousData_(previousData)
    , executeFn_(std::move(executeFn))
{
    if (text.isEmpty())
    {
        switch (op)
        {
            case Add: setText(QObject::tr("Add Item")); break;
            case Remove: setText(QObject::tr("Remove Item")); break;
            case Rename: setText(QObject::tr("Rename Item")); break;
            case Move: setText(QObject::tr("Move Item")); break;
        }
    }
}

void ProjectItemCommand::undo()
{
    if (!executeFn_)
        return;
    
    switch (op_)
    {
        case Add:
            executeFn_(Remove, itemId_, parentId_, itemData_);
            break;
        case Remove:
            executeFn_(Add, itemId_, parentId_, itemData_);
            break;
        case Rename:
            executeFn_(Rename, itemId_, parentId_, previousData_);
            break;
        case Move:
            executeFn_(Move, itemId_, previousData_.toString(), itemData_);
            break;
    }
}

void ProjectItemCommand::redo()
{
    if (!executeFn_)
        return;
    
    executeFn_(op_, itemId_, parentId_, itemData_);
}


Scene3DCommand::Scene3DCommand(
    ObjectType objectType,
    int objectId,
    const QVector3D& oldPosition,
    const QVector3D& newPosition,
    std::function<void(ObjectType, int, const QVector3D&)> applyFn,
    const QString& text,
    QUndoCommand* parent)
    : QUndoCommand(text, parent)
    , objectType_(objectType)
    , objectId_(objectId)
    , oldPosition_(oldPosition)
    , newPosition_(newPosition)
    , applyFn_(std::move(applyFn))
{
    if (text.isEmpty())
    {
        QString typeName;
        switch (objectType)
        {
            case Emitter: typeName = QObject::tr("Emitter"); break;
            case Listener: typeName = QObject::tr("Listener"); break;
            case Obstruction: typeName = QObject::tr("Wall"); break;
        }
        setText(QObject::tr("Move %1").arg(typeName));
    }
}

void Scene3DCommand::undo()
{
    if (applyFn_)
        applyFn_(objectType_, objectId_, oldPosition_);
}

void Scene3DCommand::redo()
{
    if (applyFn_)
        applyFn_(objectType_, objectId_, newPosition_);
}

bool Scene3DCommand::mergeWith(const QUndoCommand* other)
{
    if (other->id() != id())
        return false;
    
    auto cmd = static_cast<const Scene3DCommand*>(other);
    if (cmd->objectType_ != objectType_ || cmd->objectId_ != objectId_)
        return false;
    
    newPosition_ = cmd->newPosition_;
    return true;
}


DeleteProjectItemCommand::DeleteProjectItemCommand(
    ProjectItem* item,
    QUndoCommand* parent)
    : QUndoCommand(parent)
    , parent_(item->parent)
    , originalIndex_(-1)
{
    if (ProjectData* project = ProjectManager::instance().currentProject()) {
        ProjectFileBinding* binding = findFileBindingForItem(*project, item);
        if (!binding && parent_) {
            binding = findFileBindingForItem(*project, parent_);
        }
        if (binding && binding->rootItem) {
            packGuid_ = binding->rootItem->guid.toString(QUuid::WithoutBraces);
        }
    }

    itemName_ = item->name;
    itemType_ = item->type;
    sourceID_ = item->sourceID;
    objectID_ = item->objectID;
    expanded_ = item->expanded;

    if (parent_) {
        for (size_t i = 0; i < parent_->children.size(); ++i) {
            if (parent_->children[i].get() == item) {
                originalIndex_ = static_cast<int>(i);
                savedItem_ = parent_->children[i];
                break;
            }
        }

        if (parent_->parent) {
            for (const auto& child : parent_->parent->children) {
                if (child.get() == parent_) {
                    parentRef_ = child;
                    break;
                }
            }
        }
    } else {
        if (ProjectData* project = ProjectManager::instance().currentProject()) {
            if (ProjectFileBinding* binding = ProjectManager::findRootBinding(project, item)) {
                isTopLevelBinding_ = true;
                savedItem_         = binding->rootItem;
            }
        }
    }

    setText(QObject::tr("Delete %1").arg(item->name));
}

bool DeleteProjectItemCommand::isPackValid() const
{
    return findBindingByRootGuid(ProjectManager::instance().currentProject(),
                                 packGuid_) != nullptr;
}

void DeleteProjectItemCommand::undo()
{
    if (isTopLevelBinding_) {
        if (!savedItem_) return;
        ProjectData* project = ProjectManager::instance().currentProject();
        if (!project) return;
        if (!ProjectManager::attachRootBinding(project, rootSlot_)) return;
        ProjectManager::restoreReferencesFromLog(refLog_, project);
        emit ProjectManager::instance().itemCreated(savedItem_.get());
        return;
    }

    if (!parent_ || !savedItem_) return;

    if (ProjectManager::instance().restoreItemInternal(
            parent_, savedItem_, originalIndex_,
            std::nullopt,
            parent_->type == ProjectItem::Type::Container
                ? &refLog_.containerChildOccurrences : nullptr, false))
    {
        ProjectManager::restoreReferencesFromLog(
            refLog_, ProjectManager::instance().currentProject());
        emit ProjectManager::instance().itemCreated(savedItem_.get());
    }
}

void DeleteProjectItemCommand::redo()
{
    if (isTopLevelBinding_) {
        if (!savedItem_) return;
        ProjectData* project = ProjectManager::instance().currentProject();
        if (!project) return;
        if (!ProjectManager::detachRootBinding(project, savedItem_.get(), &rootSlot_)) return;
        refLog_ = ProjectManager::cleanupReferencesToDeletedSubtree(
            savedItem_.get(), project);
        firstRedo_ = false;
        emit ProjectManager::instance().itemDeleted(savedItem_.get());
        return;
    }

    if (!parent_ || !savedItem_) return;

    if (firstRedo_) {
        firstRedo_ = false;
    }

    if (ProjectManager::instance().deleteItemInternal(
            parent_, savedItem_, nullptr, &refLog_)) {
        emit ProjectManager::instance().itemDeleted(savedItem_.get());
    }
}


namespace {
using ObjectDirtyState = std::vector<std::pair<ProjectItem*, bool>>;

void appendDirtyState(ProjectItem* item, ObjectDirtyState& state)
{
    if (!item) return;
    state.emplace_back(item, item->dirtyBit);
    for (const auto& child : item->children) appendDirtyState(child.get(), state);
}

ObjectDirtyState projectDirtyState(ProjectData& project)
{
    ObjectDirtyState result;
    for (const auto* binding : project.allFileBindings())
        appendDirtyState(binding->rootItem.get(), result);
    return result;
}

void restoreDirtyState(const ObjectDirtyState& state)
{
    auto& manager = ProjectManager::instance();
    for (const auto& [item, dirty] : state) {
        if (item->dirtyBit == dirty) continue;
        item->dirtyBit = dirty;
        emit manager.itemPropertyChanged(item, QStringLiteral("dirty"));
    }
}
}

CreateProjectItemCommand::CreateProjectItemCommand(
    ProjectItem* parent,
    std::shared_ptr<ProjectItem> newItem,
    QUndoCommand* undoParent)
    : QUndoCommand(undoParent)
    , parent_(parent)
    , item_(std::move(newItem))
{
    if (!parent_ && item_) {
        if (ProjectData* project = ProjectManager::instance().currentProject()) {
            if (ProjectManager::findRootBinding(project, item_.get())) {
                isTopLevelBinding_ = true;

            }
        }
    }
    setText(QObject::tr("Create %1").arg(item_->name));
}

void CreateProjectItemCommand::undo()
{
    if (isTopLevelBinding_) {
        if (!item_) return;
        ProjectData* project = ProjectManager::instance().currentProject();
        if (!project) return;
        if (!ProjectManager::detachRootBinding(project, item_.get(), &rootSlot_)) return;
        emit ProjectManager::instance().itemDeleted(item_.get());
        return;
    }

    if (!parent_ || !item_) return;

    if (ProjectManager::instance().deleteItemInternal(parent_, item_, nullptr, &refLog_)) {
        emit ProjectManager::instance().itemDeleted(item_.get());
    }
}

void CreateProjectItemCommand::redo()
{
    if (isTopLevelBinding_) {
        if (firstRedo_) {
            firstRedo_ = false;
            return;
        }
        if (!item_) return;
        ProjectData* project = ProjectManager::instance().currentProject();
        if (!project) return;
        if (!ProjectManager::attachRootBinding(project, rootSlot_)) return;
        emit ProjectManager::instance().itemCreated(item_.get());
        return;
    }

    if (!parent_ || !item_) return;

    if (firstRedo_) {
        firstRedo_ = false;
        return;
    }

    if (ProjectManager::instance().restoreItemInternal(
            parent_, item_, -1, std::nullopt,
            parent_->type == ProjectItem::Type::Container
                ? &refLog_.containerChildOccurrences : nullptr, false)) {
        ProjectManager::restoreReferencesFromLog(
            refLog_, ProjectManager::instance().currentProject());
        emit ProjectManager::instance().itemCreated(item_.get());
    }
}


RenameProjectItemCommand::RenameProjectItemCommand(
    ProjectItem* item,
    const QString& oldName,
    const QString& newName,
    QUndoCommand* undoParent)
    : TransactionalUndoCommand(QObject::tr("Rename %1").arg(oldName), undoParent)
    , item_(item)
    , oldName_(oldName)
    , newName_(newName)
{}

bool RenameProjectItemCommand::apply(bool redo)
{
    return ProjectManager::instance().renameItemInternal(item_, redo ? newName_ : oldName_);
}

SetItemColorCommand::SetItemColorCommand(
    ProjectItem* item,
    uint32_t oldColor,
    uint32_t newColor,
    QUndoCommand* undoParent)
    : QUndoCommand(undoParent),
      item_(item),
      oldColor_(oldColor),
      newColor_(newColor)
{
    setText(QStringLiteral("Set Color"));
}

void SetItemColorCommand::undo()
{
    if (!item_) return;
    item_->color = oldColor_;
    item_->markDirty();
    emit ProjectManager::instance().itemPropertyChanged(item_, QStringLiteral("color"));
}

void SetItemColorCommand::redo()
{
    if (!item_) return;
    if (firstRedo_) {
        firstRedo_ = false;
        return;
    }
    item_->color = newColor_;
    item_->markDirty();
    emit ProjectManager::instance().itemPropertyChanged(item_, QStringLiteral("color"));
}

namespace {
constexpr auto kPastePropertyMask = [] {
    using Property = harkenplayableproperties::PlayablePropertyIdV1;
    harkenplayableproperties::ChassisPresenceV1 presence;
    presence.Set(Property::Volume);
    presence.Set(Property::Pitch);
    presence.Set(Property::MakeUpGain);
    return presence.bits;
}();

QJsonObject grabPasteProps(const ProjectItem* it)
{
    QJsonObject o;
    if (!it) return o;
    if (const auto* m = it->comp<VMixNode>()) {
        o["v"] = m->volume;
        o["p"] = m->pitch;
        o["mug"] = m->makeUpGain;
        o["presence"] = QString::number(it->playablePropertyPresence.bits & kPastePropertyMask);
    }
    return o;
}

void putPasteProps(ProjectItem* it, const QJsonObject& o)
{
    if (!it) return;
    if (o.contains("v")) {
        auto* m = it->compOrCreate<VMixNode>();
        m->volume = static_cast<float>(o["v"].toDouble());
        m->pitch  = static_cast<float>(o["p"].toDouble());
    }
    if (o.contains("mug"))
        it->compOrCreate<VMixNode>()->makeUpGain = static_cast<float>(o["mug"].toDouble());
    it->playablePropertyPresence.bits = (it->playablePropertyPresence.bits & ~kPastePropertyMask)
        | o["presence"].toString().toULongLong();
    it->markDirty();
    emit ProjectManager::instance().itemPropertyChanged(it, QStringLiteral("pasteProperties"));
}
}

PastePropertiesCommand::PastePropertiesCommand(const std::vector<ProjectItem*>& targets,
                                               const ProjectItem* source,
                                               QUndoCommand* undoParent)
    : QUndoCommand(undoParent)
{
    const QJsonObject after = grabPasteProps(source);
    if (source && !after.isEmpty()) {
        for (ProjectItem* t : targets) {
            if (!t || !source || t->type != source->type) continue;
            const auto before = grabPasteProps(t);
            if (before != after)
                snaps_.push_back({ t, before, after });
        }
    }
    setText(QObject::tr("Paste Properties"));
}

void PastePropertiesCommand::undo()
{
    for (auto it = snaps_.rbegin(); it != snaps_.rend(); ++it) {
        putPasteProps(it->item, it->before);
    }
}

void PastePropertiesCommand::redo()
{
    for (auto& s : snaps_) {
        putPasteProps(s.item, s.after);
    }
}


static QString parentSlotGuid(ProjectItem* parent)
{
    if (!parent) return QString();

    ProjectFileBinding* ownerBinding = nullptr;
    if (ProjectData* project = ProjectManager::instance().currentProject()) {
        ownerBinding = findFileBindingForItem(*project, parent);
    }
    if (ownerBinding && ownerBinding->rootItem.get() == parent) {
        return ownerBinding->rootItem->guid.toString(QUuid::WithoutBraces);
    }

    if (!parent->guid.isNull()) {
        return parent->guid.toString(QUuid::WithoutBraces);
    }

    return (ownerBinding && ownerBinding->rootItem)
               ? ownerBinding->rootItem->guid.toString(QUuid::WithoutBraces)
               : QString();
}


namespace {
class MoveEntriesCommand final : public TransactionalUndoCommand
{
public:
    MoveEntriesCommand(std::vector<std::unique_ptr<TransactionalUndoCommand>> commands, const QString& text)
        : TransactionalUndoCommand(text, nullptr, false)
        , commands_(std::move(commands))
    {}

private:
    bool apply(bool redo) override
    {
        if (blocked_) {
            emit ProjectManager::instance().errorOccurred(QObject::tr("The move could not be recovered."));
            return false;
        }
        auto* project = ProjectManager::instance().currentProject();
        if (!project) return false;
        const auto dirty = projectDirtyState(*project);
        std::vector<TransactionalUndoCommand*> applied;
        for (size_t step = 0; step < commands_.size(); ++step) {
            auto* command = commands_[redo ? step : commands_.size() - step - 1].get();
            if (!command->tryReplay(redo)) {
                for (auto it = applied.crbegin(); it != applied.crend(); ++it)
                    if (!(*it)->tryReplay(!redo)) blocked_ = true;
                restoreDirtyState(dirty);
                return false;
            }
            applied.push_back(command);
        }
        return true;
    }

    std::vector<std::unique_ptr<TransactionalUndoCommand>> commands_;
    bool blocked_ = false;
};
}

bool pushMoveCommands(std::vector<std::unique_ptr<TransactionalUndoCommand>> commands,
                      const QString& text)
{
    auto command = makeMoveCommand(std::move(commands), text);
    if (!command) return true;
    if (!command->tryReplay(true)) return false;
    UndoRedoManager::instance().push(command.release());
    return true;
}

std::unique_ptr<TransactionalUndoCommand> makeMoveCommand(
    std::vector<std::unique_ptr<TransactionalUndoCommand>> commands, const QString& text)
{
    if (commands.empty()) return {};
    if (commands.size() == 1) return std::move(commands.front());
    return std::make_unique<MoveEntriesCommand>(std::move(commands), text);
}

CreateParentContainerCommand::CreateParentContainerCommand(
    const QList<ProjectItem*>& items, ProjectItem::Type type, const QString& name)
    : TransactionalUndoCommand(QObject::tr("New Parent"), nullptr, false)
    , items_(items), type_(type), name_(name)
    , parent_(items.isEmpty() ? nullptr : items.front()->parent)
{}

bool CreateParentContainerCommand::apply(bool redo)
{
    auto& manager = ProjectManager::instance();
    auto* project = manager.currentProject();
    if (!project || !parent_ || manager.findItemByGuid(parent_->guid) != parent_) return false;
    const auto dirty = projectDirtyState(*project);
    if (redo) {
        const bool first = !creation_;
        if (first) {
            creation_ = std::make_unique<QUndoCommand>();
            container_ = manager.createItem(parent_, name_, type_, {}, {}, creation_.get());
            if (!container_) {
                creation_.reset();
                restoreDirtyState(dirty);
                return false;
            }
            std::vector<std::unique_ptr<TransactionalUndoCommand>> commands;
            for (auto* item : items_)
                commands.push_back(std::make_unique<MoveItemCommand>(item, container_));
            moves_ = makeMoveCommand(std::move(commands), text());
        }
        creation_->redo();
        if (manager.findItemByGuid(container_->guid) != container_) {
            restoreDirtyState(dirty);
            return false;
        }
        if (moves_ && !moves_->tryReplay(true)) {
            creation_->undo();
            if (first) project->documentBindings.remove(container_->guid);
            restoreDirtyState(dirty);
            return false;
        }
        return true;
    }
    if (!creation_ || (moves_ && !moves_->tryReplay(false))) return false;
    creation_->undo();
    if (manager.findItemByGuid(container_->guid)) {
        if (moves_) moves_->tryReplay(true);
        restoreDirtyState(dirty);
        return false;
    }
    return true;
}


MoveItemCommand::MoveItemCommand(
    ProjectItem* item,
    ProjectItem* newParent,
    QUndoCommand* parent)
    : TransactionalUndoCommand(QObject::tr("Move %1").arg(item->name), parent, false)
    , item_(item)
    , oldParent_(item->parent)
    , newParent_(newParent)
    , originalIndex_(-1)
{
    if (oldParent_) {
        for (size_t i = 0; i < oldParent_->children.size(); ++i) {
            if (oldParent_->children[i].get() == item) {
                originalIndex_ = static_cast<int>(i);
                itemRef_ = oldParent_->children[i];
                break;
            }
        }
    } else if (ProjectData* project = ProjectManager::instance().currentProject()) {
        if (ProjectFileBinding* binding = ProjectManager::findRootBinding(project, item)) {
            wasTopLevelBinding_ = true;
            itemRef_            = binding->rootItem;

        }
    }

    oldParentGuid_ = parentSlotGuid(oldParent_);
    newParentGuid_ = parentSlotGuid(newParent_);
    prepareTrackTimeline(newParent_);

    if (item->type == ProjectItem::Type::Bus && newParent
        && newParent->type == ProjectItem::Type::Bus) {
        if (const auto* mix = item->comp<VMixNode>()) {
            for (const auto& send : mix->sends) {
                if (send.targetBusID == newParent->objectID) {
                    conflictSendTargetBusId_ = send.targetBusID;
                    conflictSendLevel_ = send.sendLevel;
                    break;
                }
            }
        }
    }

    setText(QObject::tr("Move %1").arg(item->name));
}

MoveItemCommand::MoveItemCommand(
    ProjectItem* item,
    const QString& bucketRootFolder,
    QUndoCommand* parent)
    : TransactionalUndoCommand(QObject::tr("Move %1").arg(item->name), parent, false)
    , item_(item)
    , oldParent_(item->parent)
    , newParent_(nullptr)
    , originalIndex_(-1)
    , bucketRootFolder_(bucketRootFolder)
    , savedParentRef_(item->parentRef)
{
    if (oldParent_) {
        for (size_t i = 0; i < oldParent_->children.size(); ++i) {
            if (oldParent_->children[i].get() == item) {
                originalIndex_ = static_cast<int>(i);
                itemRef_ = oldParent_->children[i];
                break;
            }
        }
    }

    oldParentGuid_ = parentSlotGuid(oldParent_);
    prepareTrackTimeline(nullptr);
    if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type)) {
        oldFieldGroups_ = item->fieldGroups;
        rootFieldGroups_ = ProjectManager::instance().captureRootFieldGroups(
            item, &fieldGroupConversionError_);
    }

    setText(QObject::tr("Move %1").arg(item->name));
}

bool MoveItemCommand::apply(bool redo)
{
    auto& manager = ProjectManager::instance();
    auto* project = manager.currentProject();
    if (blocked_ || !project || !item_ || manager.findItemByGuid(item_->guid) != item_) {
        emit manager.errorOccurred(QObject::tr("The object can no longer be moved."));
        return false;
    }
    const bool firstRedo = firstRedo_;
    ProjectItem* previousParent = item_->parent;
    if (redo && firstRedo_ && oldParent_) {
        const auto position = std::find_if(oldParent_->children.begin(), oldParent_->children.end(),
            [&](const auto& child) { return child.get() == item_; });
        if (position == oldParent_->children.end()) return false;
        originalIndex_ = static_cast<int>(std::distance(oldParent_->children.begin(), position));
    }
    const auto dirty = projectDirtyState(*project);
    if (!(redo ? redoHierarchy() : undoHierarchy())) {
        if (item_->parent != previousParent && !(redo ? undoHierarchy() : redoHierarchy()))
            blocked_ = true;
        firstRedo_ = firstRedo;
        restoreDirtyState(dirty);
        return false;
    }
    emit manager.itemMoved(item_, previousParent, item_->parent);
    return true;
}

bool MoveItemCommand::doMove(
    ProjectItem* item,
    ProjectItem* targetParent,
    int targetIndex,
    const std::optional<SelectionEntry>& restoredDestination,
    std::optional<SelectionEntry>* removedSource,
    std::optional<SelectionEntry>* installedDestination,
    std::vector<SelectionEntry>* removedSources,
    bool restoreEmptySourceOccurrences,
    DeletedSubtreeRefLog* outLog,
    bool createDefaultClip)
{
    return ProjectManager::instance().moveItemInternal(
        item, targetParent, targetIndex, restoredDestination,
        removedSource, installedDestination, removedSources,
        restoreEmptySourceOccurrences, outLog, createDefaultClip);
}

void MoveItemCommand::prepareTrackTimeline(ProjectItem* targetParent)
{
    const auto* timeline = item_ ? item_->comp<VTrack>() : nullptr;
    if (!timeline) return;
    const SegmentTimeSpec sourceTime = TimelineEditing::timeSpecForTrackParent(oldParent_);
    const SegmentTimeSpec destinationTime =
        TimelineEditing::timeSpecForTrackParent(targetParent);
    if (sourceTime == destinationTime) return;
    oldTrackClips_ = timeline->clips;
    auto converted = TimelineEditing::convertClipLedger(
        timeline->clips, sourceTime, destinationTime, &timelineConversionError_);
    if (converted) newTrackClips_ = std::move(*converted);
}

void MoveItemCommand::applyTrackTimeline(const std::optional<ClipLedger>& clips)
{
    if (!clips || !item_) return;
    auto* timeline = item_->comp<VTrack>();
    if (!timeline) return;
    timeline->clips = *clips;
    item_->markDirty();
    emit ProjectManager::instance().itemPropertyChanged(item_, QStringLiteral("clips"));
}

void MoveItemCommand::captureSelections()
{
    if (const auto* selection = oldParent_ ? oldParent_->comp<VSelection>() : nullptr)
        oldParentSelections_ = selection->selections;
    if (const auto* selection = newParent_ ? newParent_->comp<VSelection>() : nullptr)
        newParentSelections_ = selection->selections;
}

void MoveItemCommand::applySelections(ProjectItem* parent, const std::optional<SelectionData>& value)
{
    auto* selection = parent ? parent->comp<VSelection>() : nullptr;
    if (!selection || !value) return;
    selection->selections = *value;
    parent->markDirty();
    emit ProjectManager::instance().itemPropertyChanged(parent, QStringLiteral("selections"));
}

void MoveItemCommand::finishSelectionMove()
{
    if (!capturedMovedSelections_) {
        if (const auto* selection = oldParent_ ? oldParent_->comp<VSelection>() : nullptr)
            movedOldParentSelections_ = selection->selections;
        if (const auto* selection = newParent_ ? newParent_->comp<VSelection>() : nullptr)
            movedNewParentSelections_ = selection->selections;
        capturedMovedSelections_ = true;
    }
    applySelections(oldParent_, movedOldParentSelections_);
    applySelections(newParent_, movedNewParentSelections_);
}

bool MoveItemCommand::undoHierarchy()
{
    if (!item_) return false;

    if (!bucketRootFolder_.isEmpty()) {
        if (!oldParent_ || !itemRef_) return false;
        const auto previousGroups = item_->fieldGroups;
        if (rootFieldGroups_ && previousGroups != *rootFieldGroups_) return false;
        if (oldFieldGroups_) item_->fieldGroups = *oldFieldGroups_;
        if (ProjectManager::instance().restoreItemInternal(
                oldParent_, itemRef_, originalIndex_, std::nullopt,
                oldParent_->type == ProjectItem::Type::Container
                    ? &oldOccurrences_ : nullptr, false)) {
            if (createdBinding_) {
                if (ProjectData* project = ProjectManager::instance().currentProject()) {
                    RootBindingSlot discard;
                    ProjectManager::detachRootBinding(project, item_, &discard);
                }
            }
            applyTrackTimeline(oldTrackClips_);
            if (ProjectData* project = ProjectManager::instance().currentProject())
                ProjectManager::restoreReferencesFromLog(playbackRefLog_, project);
            applySelections(oldParent_, oldParentSelections_);
            recomputeObjectPathSubtree(item_);

            return true;
        } else {
            item_->fieldGroups = previousGroups;
        }
        return false;
    }

    if (wasTopLevelBinding_) {
        ProjectItem* currentParent = item_->parent;
        ProjectData* project = ProjectManager::instance().currentProject();
        if (!project || !savedBindingSlot_.valid()) return false;
        const ParentRef previousParentRef = item_->parentRef;
        const auto* parentSelection = currentParent ? currentParent->comp<VSelection>() : nullptr;
        const auto previousSelections = parentSelection ? std::optional(parentSelection->selections) : std::nullopt;
        int previousIndex = -1;
        std::optional<SelectionEntry> removedOccurrence;
        if (currentParent) {
            const auto child = std::find_if(currentParent->children.begin(),
                currentParent->children.end(), [this](const auto& entry) {
                    return entry.get() == item_;
                });
            if (child == currentParent->children.end()) return false;
            previousIndex = static_cast<int>(std::distance(currentParent->children.begin(), child));
            if (currentParent->type == ProjectItem::Type::Container) {
                auto removed =
                    PlayablePropertyAuthoringV1::removeContainerChildOccurrences(
                        *currentParent, item_->guid);
                if (!removed.empty())
                    removedOccurrence = removed.front();
            }
            currentParent->children.erase(child);
        }
        item_->parent = nullptr;
        if (ItemTypeCatalog::bucketOf(item_->type) == ItemTypeCatalog::Bucket::Layers) {
            item_->parentRef = ParentRef{};
            item_->parentRef.kind = ParentRef::Kind::Root;
        }
        if (!ProjectManager::attachRootBinding(project, savedBindingSlot_)) {
            item_->parent = currentParent;
            item_->parentRef = previousParentRef;
            if (currentParent) {
                currentParent->children.insert(currentParent->children.begin() + previousIndex, itemRef_);
                if (previousSelections) currentParent->comp<VSelection>()->selections = *previousSelections;
            }
            return false;
        }
        if (removedOccurrence) newOccurrence_ = std::move(removedOccurrence);
        if (currentParent) currentParent->markDirty();
        redoPlaybackRefLog_ = ProjectManager::cleanupIllegalPlaybackReferences(project);
        applySelections(newParent_, newParentSelections_);
        applyTrackTimeline(oldTrackClips_);
        recomputeObjectPathSubtree(item_);

        return true;
    }

    if (!oldParent_) return false;
    ProjectItem* currentParent = item_->parent;
    const std::optional<SelectionEntry> restoredDestination =
        !oldOccurrences_.empty() ? std::optional<SelectionEntry>(
            oldOccurrences_.front())
                                 : oldOccurrence_;
    if (currentParent && doMove(
            item_, oldParent_, originalIndex_, restoredDestination,
            nullptr, nullptr, nullptr,
            capturedOldOccurrences_ && oldOccurrences_.empty() && !oldOccurrence_,
            &redoPlaybackRefLog_, false)) {
        if (oldParent_->type == ProjectItem::Type::Container) {
            for (size_t i = 1; i < oldOccurrences_.size(); ++i) {
                if (!PlayablePropertyAuthoringV1::addContainerChildOccurrence(
                        *oldParent_, item_->guid, oldOccurrences_[i]))
                    return false;
            }
        }
        applySelections(oldParent_, oldParentSelections_);
        applySelections(newParent_, newParentSelections_);
        if (ProjectData* project = ProjectManager::instance().currentProject())
            ProjectManager::restoreReferencesFromLog(playbackRefLog_, project);
        applyTrackTimeline(oldTrackClips_);
        item_->markDirty();

        if (conflictSendTargetBusId_ != 0) {
            ProjectManager::instance().upsertSendInternal(
                item_->objectID, conflictSendTargetBusId_, conflictSendLevel_);
        }
        return true;
    }
    return false;
}

bool MoveItemCommand::redoHierarchy()
{
    if (!item_) return false;
    if (!timelineConversionError_.isEmpty()) {
        emit ProjectManager::instance().errorOccurred(timelineConversionError_);
        return false;
    }
    if (!fieldGroupConversionError_.isEmpty()) {
        emit ProjectManager::instance().errorOccurred(fieldGroupConversionError_);
        return false;
    }
    if (!capturedMovedSelections_) captureSelections();

    if (!bucketRootFolder_.isEmpty()) {
        firstRedo_ = false;
        ProjectItem* currentParent = item_->parent;
        if (!currentParent || !itemRef_) return false;

        ProjectData* project = ProjectManager::instance().currentProject();
        auto* bucket = project
            ? project->getBucketForFolder(bucketRootFolder_) : nullptr;
        if (!bucket) return false;
        if (oldFieldGroups_ && item_->fieldGroups != *oldFieldGroups_) {
                return false;
        }

        auto& siblings = currentParent->children;
        const auto it = std::find_if(siblings.begin(), siblings.end(),
            [this](const auto& entry) { return entry.get() == item_; });
        if (it == siblings.end()) {
                return false;
        }
        const auto previousIndex = std::distance(siblings.begin(), it);
        const auto* parentSelection = currentParent->comp<VSelection>();
        const auto previousSelections = parentSelection ? std::optional(parentSelection->selections) : std::nullopt;
        const ParentRef previousParentRef = item_->parentRef;
        std::vector<SelectionEntry> removedOccurrences;
        if (currentParent->type == ProjectItem::Type::Container) {
            removedOccurrences =
                PlayablePropertyAuthoringV1::removeContainerChildOccurrences(
                    *currentParent, item_->guid);
        }
        siblings.erase(it);

        if (rootFieldGroups_) item_->fieldGroups = *rootFieldGroups_;
        item_->parent = nullptr;
        if (ItemTypeCatalog::bucketOf(item_->type) == ItemTypeCatalog::Bucket::Layers) {
            item_->parentRef = ParentRef{};
            item_->parentRef.kind = ParentRef::Kind::Root;
        }

        bool attached = false;
        bool valid = true;
        if (!ProjectManager::findRootBinding(project, item_)) {
            RootBindingSlot slot;
            slot.binding.parentFolder = bucketRootFolder_;
            slot.binding.rootItem     = itemRef_;
            slot.index = -1;
            valid = ProjectManager::attachRootBinding(project, slot);
            attached = valid;
        } else if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item_->type)) {
            valid = validateFieldGroupProject(*project).ok()
                && refreshFieldGroupDerivedViews(*project);
        }
        if (!valid) {
            siblings.insert(siblings.begin() + previousIndex, itemRef_);
            if (previousSelections) currentParent->comp<VSelection>()->selections = *previousSelections;
            item_->parent = currentParent;
            item_->parentRef = previousParentRef;
            if (oldFieldGroups_) item_->fieldGroups = *oldFieldGroups_;
                return false;
        }

        if (attached) createdBinding_ = true;
        if (!removedOccurrences.empty()) oldOccurrence_ = removedOccurrences.front();
        oldOccurrences_ = std::move(removedOccurrences);
        capturedOldOccurrences_ = true;
        currentParent->markDirty();
        item_->markDirty();
        applyTrackTimeline(newTrackClips_);
        playbackRefLog_ = ProjectManager::cleanupIllegalPlaybackReferences(project);
        finishSelectionMove();
        recomputeObjectPathSubtree(item_);

        return true;
    }

    if (!newParent_) return false;

    ProjectItem* currentParent = item_->parent;
    const bool createDefaultClip = firstRedo_;
    if (firstRedo_) {
        firstRedo_ = false;
    }

    if (wasTopLevelBinding_ && currentParent == nullptr) {
        if (!itemRef_) return false;
        QString reason;
        if (!ProjectManager::instance().validateMoveConstraints(item_, newParent_, &reason)) {
            emit ProjectManager::instance().errorOccurred(reason);
                return false;
        }
        ProjectData* project = ProjectManager::instance().currentProject();
        if (!project || !ProjectManager::findRootBinding(project, item_)) return false;
        const bool wasDirty = item_->dirtyBit;
        if (!ProjectManager::detachRootBinding(project, item_, &savedBindingSlot_)) return false;
        if (!ProjectManager::instance().restoreItemInternal(
                newParent_, itemRef_, -1, newOccurrence_, nullptr,
                createDefaultClip)) {
            ProjectManager::attachRootBinding(project, savedBindingSlot_);
            item_->dirtyBit = wasDirty;
                return false;
        }
        if (newParent_->type == ProjectItem::Type::Container
            && !newParent_->comp<VSelection>()->selections.shared.occurrenceRows().empty())
            newOccurrence_ = newParent_->comp<VSelection>()->selections.shared.occurrenceRows().back();
        ProjectManager::restoreReferencesFromLog(redoPlaybackRefLog_, project);
        finishSelectionMove();
        applyTrackTimeline(newTrackClips_);
        recomputeObjectPathSubtree(item_);

        return true;
    }

    std::optional<SelectionEntry> removedSource;
    std::optional<SelectionEntry> installedDestination;
    std::vector<SelectionEntry> removedSources;
    ProjectData* project = ProjectManager::instance().currentProject();
    DeletedSubtreeRefLog playbackLog;
    if (doMove(item_, newParent_, -1, newOccurrence_,
               &removedSource, &installedDestination, &removedSources,
               false, &playbackLog, createDefaultClip)) {
        if (!capturedOldOccurrences_) {
            oldOccurrences_ = removedSources;
            capturedOldOccurrences_ = true;
        }
        if (!oldOccurrence_ && !removedSources.empty())
            oldOccurrence_ = removedSources.front();
        if (!newOccurrence_) newOccurrence_ = std::move(installedDestination);
        playbackRefLog_ = std::move(playbackLog);
        ProjectManager::restoreReferencesFromLog(redoPlaybackRefLog_, project);
        finishSelectionMove();
        applyTrackTimeline(newTrackClips_);
        item_->markDirty();

        if (conflictSendTargetBusId_ != 0
            && ProjectManager::instance().removeSendInternal(
                   item_->objectID, conflictSendTargetBusId_)) {
            qWarning() << "[MoveItemCommand] Removed send" << item_->objectID << "->"
                       << conflictSendTargetBusId_
                       << "because the reparent made the target its main output";
        }
        return true;
    }
    return false;
}


ContainerPropertyChangeCommand::ContainerPropertyChangeCommand(
    ProjectItem* item,
    const QString& propertyName,
    const QVariant& oldValue,
    const QVariant& newValue,
    std::function<void(ProjectItem*, const QVariant&)> applyFn,
    const QString& text,
    QUndoCommand* parent)
    : QUndoCommand(text.isEmpty() ? QObject::tr("Change Container %1").arg(propertyName) : text, parent)
    , item_(item)
    , propertyName_(propertyName)
    , oldValue_(oldValue)
    , newValue_(newValue)
    , applyFn_(std::move(applyFn))
{
}

void ContainerPropertyChangeCommand::undo()
{
    if (item_ && applyFn_)
    {
        applyFn_(item_, oldValue_);
        emit ProjectManager::instance().itemPropertyChanged(item_, propertyName_);
    }
}

void ContainerPropertyChangeCommand::redo()
{
    if (firstRedo_)
    {
        firstRedo_ = false;
        return;
    }
    
    if (item_ && applyFn_)
    {
        applyFn_(item_, newValue_);
        emit ProjectManager::instance().itemPropertyChanged(item_, propertyName_);
    }
}

bool ContainerPropertyChangeCommand::mergeWith(const QUndoCommand* other)
{
    if (other->id() != id())
        return false;
    
    auto cmd = static_cast<const ContainerPropertyChangeCommand*>(other);
    if (cmd->item_ != item_ || cmd->propertyName_ != propertyName_)
        return false;
    
    newValue_ = cmd->newValue_;
    return true;
}


BusPropertyChangeCommand::BusPropertyChangeCommand(
    uint32_t busID,
    const QString& propertyName,
    const QVariant& oldValue,
    const QVariant& newValue,
    std::function<void(uint32_t, const QVariant&)> applyFn,
    const QString& text,
    QUndoCommand* parent)
    : QUndoCommand(text.isEmpty() ? QObject::tr("Change Bus %1").arg(propertyName) : text, parent)
    , objectID_(busID)
    , propertyName_(propertyName)
    , oldValue_(oldValue)
    , newValue_(newValue)
    , applyFn_(std::move(applyFn))
{
}

void BusPropertyChangeCommand::undo()
{
    if (applyFn_)
        applyFn_(objectID_, oldValue_);
}

void BusPropertyChangeCommand::redo()
{
    if (firstRedo_) {
        firstRedo_ = false;
        return;
    }
    if (applyFn_)
        applyFn_(objectID_, newValue_);
}

bool BusPropertyChangeCommand::mergeWith(const QUndoCommand* other)
{
    if (other->id() != id())
        return false;
    
    auto cmd = static_cast<const BusPropertyChangeCommand*>(other);
    if (cmd->objectID_ != objectID_ || cmd->propertyName_ != propertyName_)
        return false;
    
    newValue_ = cmd->newValue_;
    return true;
}

BusSendCommand::BusSendCommand(
    uint32_t busID, uint32_t targetBusID,
    float sendLevel, bool isAdd,
    std::function<void(uint32_t, uint32_t, float)> addFn,
    std::function<void(uint32_t, uint32_t)> removeFn,
    const QString& text, QUndoCommand* parent)
    : QUndoCommand(text.isEmpty() ? (isAdd ? "Add Send" : "Remove Send") : text, parent)
    , objectID_(busID), targetBusId_(targetBusID)
    , sendLevel_(sendLevel)
    , isAdd_(isAdd)
    , addFn_(std::move(addFn)), removeFn_(std::move(removeFn))
{
}

void BusSendCommand::undo()
{
    if (isAdd_) {
        removeFn_(objectID_, targetBusId_);
    } else {
        addFn_(objectID_, targetBusId_, sendLevel_);
    }
}

void BusSendCommand::redo()
{
    if (firstRedo_) {
        firstRedo_ = false;
        return;
    }

    if (isAdd_) {
        addFn_(objectID_, targetBusId_, sendLevel_);
    } else {
        removeFn_(objectID_, targetBusId_);
    }
}


SequenceReorderCommand::SequenceReorderCommand(
    ProjectItem* item,
    const std::vector<std::shared_ptr<ProjectItem>>& oldOrder,
    const std::vector<std::shared_ptr<ProjectItem>>& newOrder,
    QUndoCommand* parent)
    : QUndoCommand(item && item->type == ProjectItem::Type::Segment
        ? QObject::tr("Reorder Tracks") : QObject::tr("Reorder Sequence Playlist"), parent)
    , item_(item)
    , oldOrder_(oldOrder)
    , newOrder_(newOrder)
{
}

void SequenceReorderCommand::undo()
{
    applyOrder(oldOrder_);
}

void SequenceReorderCommand::redo()
{
    if (firstRedo_)
    {
        firstRedo_ = false;
        return;
    }
    applyOrder(newOrder_);
}

void SequenceReorderCommand::applyOrder(const std::vector<std::shared_ptr<ProjectItem>>& order)
{
    if (!item_) return;

    if (!ProjectManager::instance().reorderItemChildrenInternal(item_, order)) {
        return;
    }
}

SpatialPropertyChangeCommand::SpatialPropertyChangeCommand(
    ProjectItem* item, const QString& propertyName,
    const QVariant& oldValue, const QVariant& newValue,
    std::function<void(ProjectItem*, const QVariant&)> applyFn,
    const QString& text, QUndoCommand* parent)
    : QUndoCommand(text.isEmpty()
          ? QString("Change %1 '%2'").arg(propertyName, item ? item->name : "")
          : text, parent)
    , item_(item), propertyName_(propertyName)
    , oldValue_(oldValue), newValue_(newValue), applyFn_(applyFn)
{}

void SpatialPropertyChangeCommand::undo()
{
    if (!item_ || !applyFn_) return;
    applyFn_(item_, oldValue_);
    item_->markDirty();
}

void SpatialPropertyChangeCommand::redo()
{
    if (firstRedo_) { firstRedo_ = false; return; }
    if (!item_ || !applyFn_) return;
    applyFn_(item_, newValue_);
    item_->markDirty();
}

bool SpatialPropertyChangeCommand::mergeWith(const QUndoCommand* other)
{
    auto cmd = dynamic_cast<const SpatialPropertyChangeCommand*>(other);
    if (!cmd) return false;
    if (cmd->item_ != item_ || cmd->propertyName_ != propertyName_) return false;
    newValue_ = cmd->newValue_;
    return true;
}

}
