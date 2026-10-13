#include "Core/Components/VSelection.h"
#include "SignalPathEditing.h"
#include "BlueprintConnectCommand.h"

#include "Core/BusRouteGraph.h"
#include "Core/Components/VMixNode.h"
#include "Core/Components/VTrack.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/PlatformContextService.h"
#include "Core/PhysicalFolderOperations.h"
#include "Core/ProjectManager.h"
#include "Core/FieldGroupDerivedView.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Core/UndoCommands.h"
#include <QScopeGuard>

#include <algorithm>
#include <map>
#include <set>

namespace VNX {
namespace {

const SignalPathPortPair* port(const SignalPathGraph& graph, SignalPathEndpoint endpoint)
{
    if (endpoint.occurrenceIndex >= graph.nodes.size()) return nullptr;
    const auto& pairs = graph.nodes[endpoint.occurrenceIndex].portPairs;
    return endpoint.portPairIndex < pairs.size() ? &pairs[endpoint.portPairIndex] : nullptr;
}

std::vector<FieldGroupScope> editScopes(const ProjectData& project)
{
    const auto& context = PlatformContextService::instance().context();
    std::vector<FieldGroupScope> result;
    if (context.all || context.selected.isEmpty()) {
        result.push_back({});
        for (const auto& platform : project.platforms)
            result.push_back(toFieldGroupGuid(platform.id));
    } else {
        for (const auto platform : context.selected)
            result.push_back(toFieldGroupGuid(platform));
    }
    return result;
}

template <typename Payload>
void writeLocal(AuthoredGroup<Payload>& group, FieldGroupScope scope, const Payload& payload)
{
    GroupState<Payload> state{FieldGroupMode::Local, payload};
    if (!scope) {
        group.shared = std::move(state);
        return;
    }
    if (group.shared.mode == FieldGroupMode::Local && group.shared.localHistory) {
        state.mode = FieldGroupMode::UseShared;
        state.localFields = FieldGroupDifferenceMask(payload, *group.shared.localHistory);
        if (state.localFields == 0) {
            group.platforms.erase(*scope);
            return;
        }
        Payload fields;
        CopyFieldGroupFields(fields, payload, state.localFields);
        state.localHistory = std::move(fields);
    }
    group.platforms[*scope] = std::move(state);
}

bool pathsStillCurrent(const ProjectData& project, const SignalPathGraph& graph,
                       const SignalPathPortPair& from, const SignalPathPortPair& to)
{
    std::set<size_t> indices(from.pathIndices.begin(), from.pathIndices.end());
    if (to.routing) indices.insert(to.pathIndices.begin(), to.pathIndices.end());
    std::vector<SignalPathIdentity> identities;
    for (const auto index : indices) {
        if (index >= graph.paths.size()) return false;
        identities.push_back(graph.paths[index].identity);
    }
    const auto current = buildSignalPathGraph(project, identities, graph.platform);
    if (!current.ok() || current.paths.size() != indices.size()) return false;
    size_t next = 0;
    for (const auto index : indices) {
        const auto& before = graph.paths[index];
        const auto& after = current.paths[next++];
        if (before.identity != after.identity || before.routing != after.routing
            || before.computation.size() != after.computation.size()) return false;
        for (size_t step = 0; step < before.computation.size(); ++step)
            if (graph.nodes[before.computation[step].occurrenceIndex].key
                != current.nodes[after.computation[step].occurrenceIndex].key) return false;
    }
    return true;
}

bool validBusRouting(const SignalPathConnectPlan& plan, bool redo)
{
    auto& manager = ProjectManager::instance();
    const auto* master = manager.getMasterBusItem();
    if (!master) return false;
    BusRouteSnapshot routes;
    std::vector<const ProjectItem*> pending{master};
    while (!pending.empty()) {
        const auto* item = pending.back();
        pending.pop_back();
        for (const auto& child : item->children) pending.push_back(child.get());
        if (item->type != ProjectItemType::Bus) continue;
        const auto* mix = item->comp<VMixNode>();
        if (!mix) return false;
        const auto reset = std::find_if(plan.busOutputResets.begin(), plan.busOutputResets.end(),
            [&](const auto& entry) { return entry.definition == item->guid; });
        const auto* parent = reset == plan.busOutputResets.end() ? item->parent
            : redo ? master : manager.findItemByGuid(reset->parent);
        const auto move = std::find_if(plan.moves.begin(), plan.moves.end(),
            [&](const auto& entry) { return entry.object == item->guid; });
        if (move != plan.moves.end())
            parent = manager.findItemByGuid(redo ? move->afterParent : move->beforeParent);
        if (item != master && (!parent || parent->type != ProjectItemType::Bus)) return false;
        const auto output = item == master ? 0u : parent->objectID;
        routes.parentOf.insert(item->objectID, output);
        auto& targets = routes.sendTargets[item->objectID];
        for (const auto& send : mix->sends) {
            if (plan.busSend && !redo && item->guid == plan.from) {
                const auto* target = manager.findItemByGuid(plan.to);
                if (target && target->objectID == send.targetBusID) continue;
            }
            const bool removed = std::any_of(plan.busSendRemovals.begin(), plan.busSendRemovals.end(),
                [&](const auto& entry) {
                    const auto* target = manager.findItemByGuid(entry.to);
                    return entry.from == item->guid && target && target->objectID == send.targetBusID;
                });
            if (redo && removed) continue;
            if (send.targetBusID == output || std::find(targets.begin(), targets.end(), send.targetBusID) != targets.end())
                return false;
            targets.push_back(send.targetBusID);
        }
        if (plan.busSend && redo && item->guid == plan.from) {
            const auto* target = manager.findItemByGuid(plan.to);
            if (!target || target->objectID == output
                || std::find(targets.begin(), targets.end(), target->objectID) != targets.end()) return false;
            targets.push_back(target->objectID);
        }
        if (!redo) {
            for (const auto& removal : plan.busSendRemovals) {
                if (removal.from != item->guid) continue;
                const auto* target = manager.findItemByGuid(removal.to);
                if (!target || target->objectID == output
                    || std::find(targets.begin(), targets.end(), target->objectID) != targets.end()) return false;
                targets.push_back(target->objectID);
            }
        }
    }
    for (const auto& targets : routes.sendTargets)
        for (const auto target : targets) if (!routes.parentOf.contains(target)) return false;
    return findBusRouteCycle(routes).empty();
}

class SignalPathConnectCommand : public TransactionalUndoCommand
{
public:
    explicit SignalPathConnectCommand(SignalPathConnectPlan plan)
        : TransactionalUndoCommand(plan.description, nullptr, false), plan_(std::move(plan))
    {}

protected:
    bool apply(bool redo) override
    {
        auto& manager = ProjectManager::instance();
        auto* project = manager.currentProject();
        auto* from = manager.findItemByGuid(plan_.from);
        auto* to = manager.findItemByGuid(plan_.to);
        if (!project || !from || !to) return false;
        const auto& expected = redo ? plan_.before : plan_.after;
        const auto& replacement = redo ? plan_.after : plan_.before;
        for (const auto& snapshot : expected) {
            const auto* item = manager.findItemByGuid(snapshot.definition);
            if (!item || item->fieldGroups != snapshot.groups) return false;
        }
        std::vector<FieldGroupParentSnapshot> parents;
        for (const auto& snapshot : plan_.observedGroups) {
            const auto changed = std::find_if(expected.begin(), expected.end(),
                [&](const auto& entry) { return entry.definition == snapshot.definition; });
            const auto* item = manager.findItemByGuid(snapshot.definition);
            if (!item || item->fieldGroups != (changed == expected.end() ? snapshot.groups : changed->groups))
                return false;
        }
        for (const auto& snapshot : plan_.observedParents) {
            const auto changed = std::find_if(plan_.moves.begin(), plan_.moves.end(),
                [&](const auto& entry) { return entry.object == snapshot.definition; });
            const auto parent = !redo && changed != plan_.moves.end() ? changed->afterParent : snapshot.parent;
            const auto* item = manager.findItemByGuid(snapshot.definition);
            if (!item || (item->parent ? item->parent->guid : QUuid{}) != parent) return false;
        }
        for (size_t index = 0; index < plan_.observedClips.size(); ++index) {
            const auto& snapshot = plan_.observedClips[index];
            const auto* owner = manager.findItemByGuid(snapshot.owner);
            const auto* timeline = owner ? owner->comp<VTrack>() : nullptr;
            if (!timeline || timeline->clips.items != (redo ? snapshot.clips.items : appliedClips_[index].items))
                return false;
        }
        for (size_t index = 0; index < plan_.observedSelections.size(); ++index) {
            const auto& snapshot = plan_.observedSelections[index];
            const auto* owner = manager.findItemByGuid(snapshot.owner);
            const auto* selection = owner ? owner->comp<VSelection>() : nullptr;
            if (!selection || selection->mode != snapshot.mode
                || selection->selections != (redo ? snapshot.selections : appliedSelections_[index].first)
                || selection->behavior != (redo ? snapshot.behavior : appliedSelections_[index].second)) return false;
        }
        for (const auto& move : plan_.moves) {
            const auto* item = manager.findItemByGuid(move.object);
            const auto destinationGuid = redo ? move.afterParent : move.beforeParent;
            const auto* destination = manager.findItemByGuid(destinationGuid);
            if (!item || (!destinationGuid.isNull() && !destination)) return false;
            QString reason;
            if (destination && !manager.validateMoveConstraints(item, destination, &reason)) return false;
            if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type))
                parents.push_back({item->guid, destinationGuid});
        }
        if (plan_.busSend) {
            const auto* mix = from->comp<VMixNode>();
            if (!mix) return false;
            const auto send = std::find_if(mix->sends.begin(), mix->sends.end(),
                [&](const auto& entry) { return entry.targetBusID == to->objectID; });
            if (redo ? send != mix->sends.end()
                     : send == mix->sends.end() || send->sendLevel != 1.0f) return false;
        }
        if (!validateFieldGroupProject(*project, replacement, parents).ok()
            || !validBusRouting(plan_, redo)) return false;

        const auto setGroups = [&](const auto& snapshots) {
            for (const auto& snapshot : snapshots)
                manager.findItemByGuid(snapshot.definition)->fieldGroups = snapshot.groups;
        };
        std::vector<std::function<void()>> rollbackSteps;
        auto rollback = qScopeGuard([&] {
            for (auto it = rollbackSteps.rbegin(); it != rollbackSteps.rend(); ++it) (*it)();
            refreshFieldGroupDerivedViews(*project);
        });
        if (moves_.empty()) {
            for (const auto& move : plan_.moves) {
                auto* item = manager.findItemByGuid(move.object);
                moves_.push_back(move.afterParent.isNull()
                    ? std::make_unique<MoveItemCommand>(item, QString::fromLatin1(ItemTypeCatalog::kLayersFolderName))
                    : std::make_unique<MoveItemCommand>(item, manager.findItemByGuid(move.afterParent)));
            }
        }
        if (!redo) {
            for (const auto& move : plan_.moves)
                if (move.rootGroups) manager.findItemByGuid(move.object)->fieldGroups = *move.rootGroups;
            rollbackSteps.push_back([&] { setGroups(expected); });
        }
        for (size_t step = 0; step < moves_.size(); ++step) {
            const auto index = redo ? step : moves_.size() - step - 1;
            const auto& move = plan_.moves[index];
            auto* item = manager.findItemByGuid(move.object);
            const auto parentBefore = item->parent;
            if (redo) moves_[index]->redo();
            else moves_[index]->undo();
            if (item->parent != parentBefore) {
                rollbackSteps.push_back([&, index] {
                    if (redo) moves_[index]->undo();
                    else moves_[index]->redo();
                });
            }
            if ((item->parent ? item->parent->guid : QUuid{}) != (redo ? move.afterParent : move.beforeParent))
                return false;
        }
        std::vector<FieldGroupDefinitionSnapshot> movedGroups;
        for (const auto& snapshot : replacement)
            movedGroups.push_back({snapshot.definition, manager.findItemByGuid(snapshot.definition)->fieldGroups});
        setGroups(replacement);
        rollbackSteps.push_back([&, movedGroups] { setGroups(movedGroups); });
        if (!refreshFieldGroupDerivedViews(*project)) return false;
        if (plan_.busSend) {
            if (!send_) send_ = std::make_unique<BlueprintSendConnectCommand>(from->objectID, to->objectID);
            if (redo) send_->redo();
            else send_->undo();
            const auto& sends = from->comp<VMixNode>()->sends;
            const bool exists = std::any_of(sends.begin(), sends.end(),
                [&](const auto& entry) { return entry.targetBusID == to->objectID; });
            if (exists != redo) return false;
        }
        if (redo) {
            appliedClips_.clear();
            for (const auto& snapshot : plan_.observedClips)
                appliedClips_.push_back(manager.findItemByGuid(snapshot.owner)->comp<VTrack>()->clips);
            appliedSelections_.clear();
            for (const auto& snapshot : plan_.observedSelections) {
                const auto* owner = manager.findItemByGuid(snapshot.owner);
                appliedSelections_.emplace_back(owner->comp<VSelection>()->selections, owner->comp<VSelection>()->behavior);
            }
        }
        rollback.dismiss();
        for (const auto& snapshot : replacement) {
            auto* item = manager.findItemByGuid(snapshot.definition);
            item->markDirty();
            emit manager.itemPropertyChanged(item, QStringLiteral("fieldGroups"));
        }
        return true;
    }

private:
    SignalPathConnectPlan plan_;
    std::vector<std::unique_ptr<MoveItemCommand>> moves_;
    std::unique_ptr<BlueprintSendConnectCommand> send_;
    std::vector<ClipLedger> appliedClips_;
    std::vector<std::pair<SelectionData, SelectionBehaviorDef>> appliedSelections_;
};

class SignalPathDisconnectCommand : public TransactionalUndoCommand
{
public:
    explicit SignalPathDisconnectCommand(SignalPathConnectPlan plan)
        : TransactionalUndoCommand(plan.description, nullptr, false), plan_(std::move(plan))
    {
        auto& manager = ProjectManager::instance();
        for (const auto& removal : plan_.busSendRemovals)
            sends_.push_back(std::make_unique<BlueprintSendDisconnectCommand>(
                manager.findItemByGuid(removal.from)->objectID,
                manager.findItemByGuid(removal.to)->objectID));
    }

protected:
    bool apply(bool redo) override
    {
        auto& manager = ProjectManager::instance();
        auto* project = manager.currentProject();
        const auto* master = manager.getMasterBusItem();
        if (!project || !master) return false;
        if (redo) {
            for (const auto& locator : plan_.observedOccurrences) {
                const auto* owner = manager.findItemByGuid(locator.ownerGuid);
                if (!owner) return false;
                const auto current = locator.kind == PlayablePropertyTargetKindV1::TrackClip
                    ? PlayablePropertyAuthoringV1::trackClipLocator(*owner, locator.occurrenceGuid)
                    : PlayablePropertyAuthoringV1::containerChildLocator(
                        *owner, locator.stableOccurrenceOrdinal, plan_.occurrenceScope);
                if (!current || *current != locator) return false;
            }
        }
        const auto& expected = redo ? plan_.before : plan_.after;
        const auto& replacement = redo ? plan_.after : plan_.before;
        for (const auto& snapshot : plan_.observedGroups) {
            const auto changed = std::find_if(expected.begin(), expected.end(),
                [&](const auto& entry) { return entry.definition == snapshot.definition; });
            const auto& groups = changed == expected.end() ? snapshot.groups : changed->groups;
            const auto* item = manager.findItemByGuid(snapshot.definition);
            if (!item || item->fieldGroups != groups) return false;
        }
        for (const auto& snapshot : plan_.observedParents) {
            const auto detached = std::find_if(plan_.detachments.begin(), plan_.detachments.end(),
                [&](const auto& entry) { return entry.object == snapshot.definition; });
            const auto reset = std::find_if(plan_.busOutputResets.begin(), plan_.busOutputResets.end(),
                [&](const auto& entry) { return entry.definition == snapshot.definition; });
            const auto parent = !redo && detached != plan_.detachments.end() ? QUuid{}
                : !redo && reset != plan_.busOutputResets.end() ? master->guid : snapshot.parent;
            const auto* item = manager.findItemByGuid(snapshot.definition);
            if (!item || (item->parent ? item->parent->guid : QUuid{}) != parent) return false;
        }
        for (const auto& snapshot : expected) {
            const auto* item = manager.findItemByGuid(snapshot.definition);
            if (!item || item->fieldGroups != snapshot.groups) return false;
        }
        for (size_t index = 0; index < plan_.observedClips.size(); ++index) {
            const auto& snapshot = plan_.observedClips[index];
            const auto* owner = manager.findItemByGuid(snapshot.owner);
            const auto* timeline = owner ? owner->comp<VTrack>() : nullptr;
            if (!timeline || timeline->clips.items != (redo ? snapshot.clips.items : appliedClips_[index].items))
                return false;
        }
        for (size_t index = 0; index < plan_.observedSelections.size(); ++index) {
            const auto& snapshot = plan_.observedSelections[index];
            const auto* owner = manager.findItemByGuid(snapshot.owner);
            const auto* selection = owner ? owner->comp<VSelection>() : nullptr;
            if (!selection || selection->mode != snapshot.mode
                || selection->selections != (redo ? snapshot.selections : appliedSelections_[index].first)
                || selection->behavior != (redo ? snapshot.behavior : appliedSelections_[index].second))
                return false;
        }
        std::vector<FieldGroupParentSnapshot> parents;
        for (const auto& detach : plan_.detachments) {
            const auto* item = manager.findItemByGuid(detach.object);
            const auto* parent = manager.findItemByGuid(detach.parent);
            if (!item || !parent || item->parent != (redo ? parent : nullptr)) return false;
            parents.push_back({detach.object, redo ? QUuid{} : detach.parent});
        }
        for (const auto& reset : plan_.busOutputResets) {
            const auto* item = manager.findItemByGuid(reset.definition);
            const auto* parent = manager.findItemByGuid(reset.parent);
            if (!item || !parent || item->type != ProjectItemType::Bus
                || item->parent != (redo ? parent : master)) return false;
        }
        for (const auto& removal : plan_.busSendRemovals) {
            const auto* from = manager.findItemByGuid(removal.from);
            const auto* to = manager.findItemByGuid(removal.to);
            const auto* mix = from ? from->comp<VMixNode>() : nullptr;
            if (!mix || !to || from->type != ProjectItemType::Bus
                || to->type != ProjectItemType::Bus) return false;
            const auto send = std::find_if(mix->sends.begin(), mix->sends.end(),
                [&](const auto& entry) { return entry.targetBusID == to->objectID; });
            if (redo ? send == mix->sends.end() || !qFuzzyCompare(send->sendLevel, removal.level)
                     : send != mix->sends.end()) return false;
        }
        if (!validateFieldGroupProject(*project, replacement, parents).ok()
            || !validBusRouting(plan_, redo)) return false;

        const auto setGroups = [&](const auto& snapshots) {
            for (const auto& snapshot : snapshots)
                manager.findItemByGuid(snapshot.definition)->fieldGroups = snapshot.groups;
        };
        std::vector<std::function<void()>> rollbackSteps;
        auto rollback = qScopeGuard([&] {
            for (auto it = rollbackSteps.rbegin(); it != rollbackSteps.rend(); ++it) (*it)();
            refreshFieldGroupDerivedViews(*project);
        });
        const auto changeSends = [&](bool remove) {
            for (size_t index = 0; index < sends_.size(); ++index) {
                if (remove) sends_[index]->redo();
                else sends_[index]->undo();
                rollbackSteps.push_back([&, index, remove] {
                    if (remove) sends_[index]->undo();
                    else sends_[index]->redo();
                });
                const auto& removal = plan_.busSendRemovals[index];
                const auto* from = manager.findItemByGuid(removal.from);
                const auto* to = manager.findItemByGuid(removal.to);
                const auto& sends = from->comp<VMixNode>()->sends;
                const auto send = std::find_if(sends.begin(), sends.end(),
                    [&](const auto& entry) { return entry.targetBusID == to->objectID; });
                if (remove ? send != sends.end()
                           : send == sends.end() || !qFuzzyCompare(send->sendLevel, removal.level)) return false;
            }
            return true;
        };
        const auto changeMoves = [&](bool forward) {
            if (moves_.empty()) {
                for (const auto& detach : plan_.detachments)
                    moves_.push_back(std::make_unique<MoveItemCommand>(
                        manager.findItemByGuid(detach.object),
                        QString::fromLatin1(ItemTypeCatalog::kLayersFolderName)));
                for (const auto& reset : plan_.busOutputResets)
                    moves_.push_back(std::make_unique<MoveItemCommand>(
                        manager.findItemByGuid(reset.definition), manager.getMasterBusItem()));
            }
            for (size_t step = 0; step < moves_.size(); ++step) {
                const auto index = forward ? step : moves_.size() - step - 1;
                if (forward) moves_[index]->redo();
                else moves_[index]->undo();
                rollbackSteps.push_back([&, index, forward] {
                    if (forward) moves_[index]->undo();
                    else moves_[index]->redo();
                });
                const bool detach = index < plan_.detachments.size();
                const auto guid = detach ? plan_.detachments[index].object
                    : plan_.busOutputResets[index - plan_.detachments.size()].definition;
                const auto parent = forward ? (detach ? QUuid{} : master->guid)
                    : detach ? plan_.detachments[index].parent
                             : plan_.busOutputResets[index - plan_.detachments.size()].parent;
                const auto* item = manager.findItemByGuid(guid);
                if (!item || (item->parent ? item->parent->guid : QUuid{}) != parent) return false;
            }
            return true;
        };
        if (redo) {
            if (!changeSends(true)) return false;
            setGroups(replacement);
            rollbackSteps.push_back([&] { setGroups(expected); });
            if (!changeMoves(true)) return false;
        } else {
            if (!changeMoves(false)) return false;
            setGroups(replacement);
            rollbackSteps.push_back([&] { setGroups(expected); });
        }
        setGroups(replacement);
        if (!refreshFieldGroupDerivedViews(*project)) return false;
        if (!redo && !changeSends(false)) return false;
        if (redo) {
            appliedClips_.clear();
            for (const auto& snapshot : plan_.observedClips)
                appliedClips_.push_back(manager.findItemByGuid(snapshot.owner)->comp<VTrack>()->clips);
            appliedSelections_.clear();
            for (const auto& snapshot : plan_.observedSelections) {
                const auto* owner = manager.findItemByGuid(snapshot.owner);
                appliedSelections_.emplace_back(owner->comp<VSelection>()->selections, owner->comp<VSelection>()->behavior);
            }
        }
        rollback.dismiss();
        for (const auto& snapshot : replacement) {
            auto* item = manager.findItemByGuid(snapshot.definition);
            item->markDirty();
            emit manager.itemPropertyChanged(item, QStringLiteral("fieldGroups"));
        }
        return true;
    }

private:
    SignalPathConnectPlan plan_;
    std::vector<std::unique_ptr<MoveItemCommand>> moves_;
    std::vector<std::unique_ptr<BlueprintSendDisconnectCommand>> sends_;
    std::vector<ClipLedger> appliedClips_;
    std::vector<std::pair<SelectionData, SelectionBehaviorDef>> appliedSelections_;
};

}

namespace {

SignalPathConnectPlan prepareObjectConnections(const SignalPathGraph& graph,
    const std::vector<SignalPathEndpoint>& endpoints, SignalPathEndpoint target, bool addSend, bool nodeDrop)
{
    SignalPathConnectPlan plan;
    const auto fail = [&](const QString& error) { plan.error = error; return plan; };
    auto& manager = ProjectManager::instance();
    auto* project = manager.currentProject();
    const auto* master = manager.getMasterBusItem();
    const auto* targetPort = port(graph, target);
    if (!project || !master || !graph.ok() || endpoints.empty() || !targetPort)
        return fail(QObject::tr("The signal path is unavailable"));
    plan.to = graph.nodes[target.occurrenceIndex].key.objectGuid;
    auto* to = manager.findItemByGuid(plan.to);
    if (!to) return fail(QObject::tr("Object no longer exists"));
    const auto supported = [](const ProjectItem* item) {
        return item->type == ProjectItemType::Bus
            || PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type);
    };
    if (!supported(to)) return fail(QObject::tr("This object cannot be connected"));
    if (addSend && to->type != ProjectItemType::Bus)
        return fail(QObject::tr("A Send must target a Bus"));
    const bool toBus = to->type == ProjectItemType::Bus;
    std::vector<ProjectItem*> sources;
    SignalPathPortPair affectedPaths = *targetPort;
    std::set<QUuid> selected;
    for (const auto endpoint : endpoints) {
        const auto* sourcePort = port(graph, endpoint);
        if (!sourcePort) return fail(QObject::tr("The selected interface is unavailable"));
        auto* source = manager.findItemByGuid(graph.nodes[endpoint.occurrenceIndex].key.objectGuid);
        if (!source) return fail(QObject::tr("Object no longer exists"));
        if (!supported(source)) return fail(QObject::tr("This object cannot be connected"));
        if (source == to) return fail(QObject::tr("Cannot connect an object to itself"));
        if (selected.insert(source->guid).second) sources.push_back(source);
        affectedPaths.pathIndices.insert(affectedPaths.pathIndices.end(),
            sourcePort->pathIndices.begin(), sourcePort->pathIndices.end());
    }
    if (!pathsStillCurrent(*project, graph, affectedPaths, {}))
        return fail(QObject::tr("The signal path changed during the connection; try again"));
    if (nodeDrop) {
        std::erase_if(sources, [&](const auto* item) {
            for (auto* parent = item->parent; parent; parent = parent->parent)
                if (selected.contains(parent->guid)) return true;
            return false;
        });
    }
    const auto view = buildFieldGroupProjectView(*project);
    if (!view.ok()) return fail(view.diagnostics.front().message);
    const FieldGroupResolver resolver(view.definitions, toFieldGroupGuid(master->guid));
    const auto scopes = editScopes(*project);
    std::set<QUuid> observed;
    const auto observe = [&](const ProjectItem* item) {
        for (; item; item = item->parent) {
            if (!observed.insert(item->guid).second) continue;
            plan.observedParents.push_back({item->guid, item->parent ? item->parent->guid : QUuid{}});
            if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type))
                plan.observedGroups.push_back({item->guid, item->fieldGroups});
            if (const auto* timeline = item->comp<VTrack>())
                plan.observedClips.push_back({item->guid, timeline->clips});
            if (const auto* selection = item->comp<VSelection>())
                plan.observedSelections.push_back({item->guid, item->comp<VSelection>()->selections,
                    selection->mode, selection->behavior});
        }
    };
    observe(to);
    for (auto* from : sources) {
        if (plan.from.isNull()) plan.from = from->guid;
        observe(from);
        const bool fromBus = from->type == ProjectItemType::Bus;
        if (fromBus && !toBus) return fail(QObject::tr("A Bus must output to another Bus"));
        if (fromBus && addSend) {
            if (!manager.sendTargetSelectable(from->objectID, to->objectID))
                return fail(QObject::tr("Send rejected: duplicate target, main output conflict, or Bus cycle"));
            plan.busSend = true;
            continue;
        }
        const bool root = nodeDrop && toBus && !fromBus;
        auto* destination = root ? nullptr : to;
        const bool move = !addSend && (!toBus || fromBus || root) && from->parent != destination;
        DefinitionFieldGroups groups = from->fieldGroups;
        if (move) {
            if (from == master) return fail(QObject::tr("Master Bus cannot be reparented"));
            SignalPathReparent change{from->guid, from->parent ? from->parent->guid : QUuid{},
                destination ? destination->guid : QUuid{}, {}};
            if (root) {
                const auto allowed = ProjectManager::allowedTopLevelTypesForBucket(
                    QString::fromLatin1(ItemTypeCatalog::kLayersFolderName));
                if (std::find(allowed.begin(), allowed.end(), from->type) == allowed.end())
                    return fail(QObject::tr("This object cannot be placed at the Layers root"));
                change.rootGroups = manager.captureRootFieldGroups(from, &plan.error);
                if (!change.rootGroups) return fail(plan.error);
                groups = *change.rootGroups;
            } else if (!manager.validateMoveConstraints(from, destination, &plan.error)) return plan;
            plan.moves.push_back(std::move(change));
        }
        if (fromBus || !toBus) continue;
        const auto guid = toFieldGroupGuid(from->guid);
        const auto bus = toFieldGroupGuid(to->guid);
        for (const auto scope : scopes) {
            if (addSend) {
                const auto resolved = resolver.Resolve(guid, scope, FieldGroupId::Send);
                if (!resolved.ok()) return fail(QObject::tr("The inherited Send configuration cannot be resolved"));
                auto sends = std::get<SendFieldGroup>(resolved.value->payload);
                if (std::any_of(sends.sends.begin(), sends.sends.end(),
                        [&](const auto& send) { return send.targetBusID == bus; }))
                    return fail(QObject::tr("Send already exists on this object"));
                sends.sends.push_back({bus, 0.0f});
                writeLocal(groups.send, scope, sends);
            } else {
                const auto resolved = resolver.Resolve(guid, scope, FieldGroupId::MainOutput);
                if (!resolved.ok()) return fail(QObject::tr("The inherited Output configuration cannot be resolved"));
                auto output = std::get<MainOutputFieldGroup>(resolved.value->payload);
                if (output.mainOutputBusRef != bus) {
                    output.mainOutputBusRef = bus;
                    writeLocal(groups.mainOutput, scope, output);
                }
            }
        }
        if (groups != from->fieldGroups) {
            plan.before.push_back({from->guid, from->fieldGroups});
            plan.after.push_back({from->guid, std::move(groups)});
        }
    }
    std::vector<FieldGroupParentSnapshot> parents;
    for (const auto& move : plan.moves)
        if (manager.findItemByGuid(move.object)->type != ProjectItemType::Bus)
            parents.push_back({move.object, move.afterParent});
    const auto validation = validateFieldGroupProject(*project, plan.after, parents);
    if (!validation.ok()) return fail(validation.diagnostics.front().message);
    if (!validBusRouting(plan, true)) return fail(QObject::tr("Connection rejected: main output conflict or Bus cycle"));
    if (plan.moves.empty() && plan.after.empty() && !plan.busSend)
        return fail(QObject::tr("Connection already exists"));
    plan.description = addSend ? QObject::tr("Add Send")
        : nodeDrop && toBus ? QObject::tr("Move Objects and Change Output")
        : !toBus ? QObject::tr("Change Parent") : QObject::tr("Change Output");
    return plan;
}

}

SignalPathConnectPlan prepareSignalPathConnect(const SignalPathGraph& graph,
    SignalPathEndpoint from, SignalPathEndpoint to, bool addSend)
{
    return prepareSignalPathConnect(graph, {from, to, SignalPathConnectGesture::Output,
        SignalPathConnectTarget::Input, addSend});
}

SignalPathConnectPlan prepareSignalPathConnect(const SignalPathGraph& graph,
    const SignalPathConnectRequest& request)
{
    const bool reverse = request.gesture == SignalPathConnectGesture::Input;
    if (request.targetPart != SignalPathConnectTarget::Body
        && request.targetPart != (reverse ? SignalPathConnectTarget::Output : SignalPathConnectTarget::Input)) {
        SignalPathConnectPlan plan;
        plan.error = QObject::tr("Connect an output pin to an input pin");
        return plan;
    }
    if (request.addSend && reverse && port(graph, request.source)
        && graph.nodes[request.source.occurrenceIndex].type != ProjectItemType::Bus) {
        SignalPathConnectPlan plan;
        plan.error = QObject::tr("Start a Send from the object's output pin");
        return plan;
    }
    auto plan = prepareObjectConnections(graph, {reverse ? request.target : request.source},
        reverse ? request.source : request.target,
        request.addSend && request.gesture != SignalPathConnectGesture::Node,
        request.gesture == SignalPathConnectGesture::Node);
    if (plan.ok() && request.addSend && reverse) plan.description = QObject::tr("Add Receive");
    return plan;
}

SignalPathConnectPlan prepareSignalPathNodeDrop(const SignalPathGraph& graph,
    const std::vector<SignalPathEndpoint>& sources, SignalPathEndpoint target)
{
    return prepareObjectConnections(graph, sources, target, false, true);
}

SignalPathConnectPlan prepareSignalPathDisconnect(const SignalPathGraph& graph,
    const std::vector<size_t>& edgeIndices)
{
    SignalPathConnectPlan plan;
    plan.occurrenceScope = graph.platform
        ? PlatformScope{fromFieldGroupGuid(*graph.platform)} : std::nullopt;
    const auto fail = [&](const QString& error) { plan.error = error; return plan; };
    auto& manager = ProjectManager::instance();
    auto* project = manager.currentProject();
    const auto* master = manager.getMasterBusItem();
    if (!project || !master || !graph.ok())
        return fail(QObject::tr("The signal path is unavailable"));
    if (edgeIndices.empty()) return fail(QObject::tr("Select a connection to delete"));
    const auto view = buildFieldGroupProjectView(*project);
    if (!view.ok()) return fail(view.diagnostics.front().message);
    const FieldGroupResolver resolver(view.definitions, toFieldGroupGuid(master->guid));
    const auto scopes = editScopes(*project);
    using ProviderScope = std::pair<QUuid, FieldGroupScope>;
    std::map<ProviderScope, std::set<QUuid>> removedSends;
    std::map<ProviderScope, MainOutputFieldGroup> resetOutputs;
    std::set<QUuid> playbackOwners;
    std::set<QUuid> detachments;
    std::set<QUuid> resetBuses;
    std::set<std::pair<QUuid, QUuid>> busSends;
    std::set<QUuid> observed;
    SignalPathPortPair affectedPaths;
    for (const auto index : std::set<size_t>(edgeIndices.begin(), edgeIndices.end())) {
        if (index >= graph.edges.size())
            return fail(QObject::tr("The selected connection is unavailable"));
        const auto& edge = graph.edges[index];
        if (!port(graph, edge.from) || !port(graph, edge.to))
            return fail(QObject::tr("The selected interface is unavailable"));
        const auto fromGuid = graph.nodes[edge.from.occurrenceIndex].key.objectGuid;
        const auto toGuid = graph.nodes[edge.to.occurrenceIndex].key.objectGuid;
        auto* from = manager.findItemByGuid(fromGuid);
        auto* to = manager.findItemByGuid(toGuid);
        if (!from || !to) return fail(QObject::tr("Object no longer exists"));
        if (plan.from.isNull()) {
            plan.from = fromGuid;
            plan.to = toGuid;
        }
        observed.insert(fromGuid);
        observed.insert(toGuid);
        affectedPaths.pathIndices.insert(affectedPaths.pathIndices.end(),
            edge.pathIndices.begin(), edge.pathIndices.end());
        if (edge.kind == SignalPathEdgeKind::Through) {
            if (from->parent != to
                || ItemTypeCatalog::bucketOf(from->type) != ItemTypeCatalog::Bucket::Layers)
                return fail(QObject::tr("The parent connection has changed"));
            const auto allowed = manager.allowedTopLevelTypesForBucket(
                QString::fromLatin1(ItemTypeCatalog::kLayersFolderName));
            if (std::find(allowed.begin(), allowed.end(), from->type) == allowed.end())
                return fail(QObject::tr("This object cannot be moved to the root level"));
            detachments.insert(fromGuid);
            playbackOwners.insert(toGuid);
            continue;
        }
        if (to->type != ProjectItemType::Bus)
            return fail(QObject::tr("The routing connection is unavailable"));
        if (edge.kind == SignalPathEdgeKind::Output && to == master)
            return fail(QObject::tr("Output to Master cannot be deleted"));
        if (from->type == ProjectItemType::Bus) {
            const auto* mix = from->comp<VMixNode>();
            if (!mix) return fail(QObject::tr("The Bus routing is unavailable"));
            if (edge.kind == SignalPathEdgeKind::Output) {
                if (from == master || from->parent != to)
                    return fail(QObject::tr("The Bus output connection has changed"));
                if (!manager.validateMoveConstraints(from, master, &plan.error)) return plan;
                if (resetBuses.insert(fromGuid).second)
                    plan.busOutputResets.push_back({fromGuid, toGuid});
            } else {
                const auto send = std::find_if(mix->sends.begin(), mix->sends.end(),
                    [&](const auto& entry) { return entry.targetBusID == to->objectID; });
                if (send == mix->sends.end() || !qFuzzyCompare(send->sendLevel, edge.sendLevel))
                    return fail(QObject::tr("The Send connection has changed"));
                if (busSends.emplace(fromGuid, toGuid).second)
                    plan.busSendRemovals.push_back({fromGuid, toGuid, send->sendLevel});
            }
            continue;
        }
        if (edge.pathIndices.empty() || !edge.routingSource)
            return fail(QObject::tr("The routing connection has no configuration source"));
        const auto group = edge.kind == SignalPathEdgeKind::Output
            ? FieldGroupId::MainOutput : FieldGroupId::Send;
        for (const auto pathIndex : edge.pathIndices) {
            if (pathIndex >= graph.paths.size())
                return fail(QObject::tr("The signal path has changed"));
            const auto origin = toFieldGroupGuid(graph.paths[pathIndex].identity.sourceGuid);
            const auto current = resolver.Resolve(origin, graph.platform, group);
            if (!current.ok() || current.value->source.isDefault
                || fromFieldGroupGuid(current.value->source.definition) != edge.routingSource->objectGuid
                || current.value->source.scope != edge.routingSource->scope)
                return fail(QObject::tr("The routing configuration source has changed"));
            for (const auto scope : scopes) {
                const auto resolved = resolver.Resolve(origin, scope, group);
                if (!resolved.ok()) return fail(QObject::tr("The routing configuration cannot be resolved"));
                if (resolved.value->source.isDefault) continue;
                const auto provider = fromFieldGroupGuid(resolved.value->source.definition);
                observed.insert(provider);
                if (edge.kind == SignalPathEdgeKind::Output) {
                    auto output = std::get<MainOutputFieldGroup>(resolved.value->payload);
                    output.mainOutputBusRef = toFieldGroupGuid(master->guid);
                    resetOutputs[{provider, scope}] = output;
                } else {
                    const auto& sends = std::get<SendFieldGroup>(resolved.value->payload).sends;
                    if (std::any_of(sends.begin(), sends.end(),
                            [&](const auto& send) { return send.targetBusID == toFieldGroupGuid(toGuid); }))
                        removedSends[{provider, scope}].insert(toGuid);
                }
            }
        }
    }
    if (!pathsStillCurrent(*project, graph, affectedPaths, {}))
        return fail(QObject::tr("The signal path changed; select the connection again"));
    for (const auto index : affectedPaths.pathIndices) {
        for (const auto& endpoint : graph.paths[index].computation)
            observed.insert(graph.nodes[endpoint.occurrenceIndex].key.objectGuid);
        for (const auto& occurrence : graph.paths[index].identity.occurrences)
            if (std::find(plan.observedOccurrences.begin(), plan.observedOccurrences.end(), occurrence)
                == plan.observedOccurrences.end()) plan.observedOccurrences.push_back(occurrence);
    }
    std::map<QUuid, DefinitionFieldGroups> candidates;
    for (const auto& [source, output] : resetOutputs) {
        const auto& [guid, scope] = source;
        const auto* provider = manager.findItemByGuid(guid);
        if (!provider) return fail(QObject::tr("The routing configuration source no longer exists"));
        auto& groups = candidates.try_emplace(guid, provider->fieldGroups).first->second;
        writeLocal(groups.mainOutput, scope, output);
    }
    for (const auto& [source, targets] : removedSends) {
        const auto& [guid, scope] = source;
        const auto* provider = manager.findItemByGuid(guid);
        if (!provider) return fail(QObject::tr("The routing configuration source no longer exists"));
        const auto resolved = resolver.Resolve(toFieldGroupGuid(guid), scope, FieldGroupId::Send);
        if (!resolved.ok()) return fail(QObject::tr("The Send configuration cannot be resolved"));
        auto sends = std::get<SendFieldGroup>(resolved.value->payload);
        std::erase_if(sends.sends, [&](const auto& send) {
            return targets.contains(fromFieldGroupGuid(send.targetBusID));
        });
        auto& groups = candidates.try_emplace(guid, provider->fieldGroups).first->second;
        writeLocal(groups.send, scope, sends);
    }
    std::vector<FieldGroupDefinitionSnapshot> routedGroups;
    for (const auto& [guid, groups] : candidates) routedGroups.push_back({guid, groups});
    for (const auto& guid : detachments) {
        auto* item = manager.findItemByGuid(guid);
        const auto rootGroups = manager.captureRootFieldGroups(item, &plan.error, routedGroups);
        if (!rootGroups) return plan;
        candidates[guid] = *rootGroups;
        plan.detachments.push_back({guid, item->parent->guid, *rootGroups});
    }
    const auto beforeInHierarchy = [&](const QUuid& first, const QUuid& second) {
        const auto* left = manager.findItemByGuid(first);
        const auto* right = manager.findItemByGuid(second);
        const auto depth = [](const ProjectItem* item) {
            size_t value = 0;
            for (; item->parent; item = item->parent) ++value;
            return value;
        };
        if (depth(left) != depth(right)) return depth(left) > depth(right);
        if (left->parent != right->parent) return left->parent->guid < right->parent->guid;
        const auto& children = left->parent->children;
        const auto position = [&](const auto* item) {
            return std::find_if(children.begin(), children.end(),
                [&](const auto& child) { return child.get() == item; });
        };
        return position(left) > position(right);
    };
    std::sort(plan.detachments.begin(), plan.detachments.end(),
        [&](const auto& a, const auto& b) { return beforeInHierarchy(a.object, b.object); });
    std::sort(plan.busOutputResets.begin(), plan.busOutputResets.end(),
        [&](const auto& a, const auto& b) { return beforeInHierarchy(a.definition, b.definition); });
    for (const auto& guid : playbackOwners) {
        const auto* owner = manager.findItemByGuid(guid);
        if (const auto* timeline = owner->comp<VTrack>())
            plan.observedClips.push_back({guid, timeline->clips});
        if (const auto* selection = owner->comp<VSelection>())
            plan.observedSelections.push_back({guid, owner->comp<VSelection>()->selections,
                selection->mode, selection->behavior});
    }
    for (const auto& guid : observed) {
        const auto* item = manager.findItemByGuid(guid);
        if (!item) return fail(QObject::tr("Path object no longer exists"));
        plan.observedGroups.push_back({guid, item->fieldGroups});
        plan.observedParents.push_back({guid, item->parent ? item->parent->guid : QUuid{}});
    }
    for (const auto& [guid, groups] : candidates) {
        const auto& original = manager.findItemByGuid(guid)->fieldGroups;
        if (groups == original) continue;
        plan.before.push_back({guid, original});
        plan.after.push_back({guid, groups});
    }
    if (plan.before.empty() && plan.detachments.empty()
        && plan.busOutputResets.empty() && plan.busSendRemovals.empty())
        return fail(QObject::tr("The connection has no editable configuration in the selected platforms"));
    std::vector<FieldGroupParentSnapshot> parents;
    for (const auto& detach : plan.detachments) parents.push_back({detach.object, {}});
    const auto validation = validateFieldGroupProject(*project, plan.after, parents);
    if (!validation.ok()) return fail(validation.diagnostics.front().message);
    if (!validBusRouting(plan, true))
        return fail(QObject::tr("Output reset conflicts with a remaining Send or Bus cycle"));
    plan.disconnect = true;
    plan.description = edgeIndices.size() == 1
        ? QObject::tr("Delete Connection") : QObject::tr("Delete Connections");
    return plan;
}

bool applySignalPathConnect(SignalPathConnectPlan plan)
{
    if (!plan.ok()) return false;
    auto& manager = ProjectManager::instance();
    const auto* project = manager.currentProject();
    if (!project) return false;
    std::vector<PhysicalItemMove> physicalMoves;
    const auto addMove = [&](const QUuid& guid, const QUuid& parentGuid) {
        auto* item = manager.findItemByGuid(guid);
        const auto* parent = manager.findItemByGuid(parentGuid);
        if (!item || (!parentGuid.isNull() && !parent)) return false;
        if (ItemTypeCatalog::isFolderAddressable(item->type))
            physicalMoves.push_back({item, project->objectFolder(parent)});
        return true;
    };
    for (const auto& move : plan.moves)
        if (!addMove(move.object, move.afterParent)) return false;
    for (const auto& detach : plan.detachments)
        if (!addMove(detach.object, {})) return false;
    if (plan.disconnect) {
        for (const auto& detach : plan.detachments)
            if (!ProjectManager::instance().findItemByGuid(detach.object)) return false;
        for (const auto& removal : plan.busSendRemovals)
            if (!ProjectManager::instance().findItemByGuid(removal.from)
                || !ProjectManager::instance().findItemByGuid(removal.to)) return false;
        return executePhysicalMoves(physicalMoves, {},
            std::make_unique<SignalPathDisconnectCommand>(std::move(plan)));
    }
    return executePhysicalMoves(physicalMoves, {},
        std::make_unique<SignalPathConnectCommand>(std::move(plan)));
}

}
