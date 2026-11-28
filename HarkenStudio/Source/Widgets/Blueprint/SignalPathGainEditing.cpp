#include "SignalPathGainEditing.h"

#include "Core/ActionManager.h"
#include "Core/AudioEngine.h"
#include "Core/BusRouteGraph.h"
#include "Core/Components/VMixNode.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/RuntimeSyncCoordinator.h"
#include "Core/FieldGroupDerivedView.h"
#include "Core/FieldGroupEditing.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Core/UndoCommands.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>

#include <algorithm>
#include <cmath>
#include <set>

namespace harken::studio {
namespace {

using Property = harkenplayableproperties::PlayablePropertyIdV1;

struct GroupTarget {
    QUuid provider;
    FieldGroupScope scope;
    FieldGroupPayload payload;
    bool editGain = true;
};

struct PropertySnapshot {
    PlatformScope scope;
    PlayablePropertyStateV1 state;
    QJsonValue effective;
};

struct Observation {
    QUuid guid;
    QUuid parent;
    DefinitionFieldGroups groups;
};

const ProjectItem* snapshotItem(const FieldGroupEffectiveSnapshot& snapshot, const QUuid& guid)
{
    const auto found = snapshot.objects.find(toFieldGroupGuid(guid));
    return found == snapshot.objects.end() ? nullptr : found->second;
}

const EffectiveDefinitionFieldGroups* effectiveGroups(
    const FieldGroupEffectiveSnapshot& snapshot, const QUuid& guid)
{
    const auto found = snapshot.definitions.find(toFieldGroupGuid(guid));
    return found == snapshot.definitions.end() ? nullptr : &found->second;
}

const FieldGroupSource& routingSource(
    const EffectiveDefinitionFieldGroups& groups, SignalPathEdgeKind kind, SendType type)
{
    if (kind == SignalPathEdgeKind::Output) return groups.outputSource;
    return type == SendType::Sidechain ? groups.sidechainSource : groups.sendSource;
}

std::optional<std::pair<harken::studio::project_schema::AuthoringGuid, float>> contentRoute(
    const EffectiveDefinitionFieldGroups& groups, const QUuid& target, SendType type, bool duckTarget)
{
    const auto targetGuid = toFieldGroupGuid(target);
    if (type == SendType::Send) {
        for (const auto& row : groups.send.sends)
            if (row.targetBusID == targetGuid) return std::pair{row.id, row.sendLevel};
        return {};
    }
    for (const auto& row : groups.sidechain.sidechains)
        if (row.targetBusID == targetGuid && row.duckTarget == duckTarget) return std::pair{row.id, row.sendLevel};
    return {};
}

std::optional<float> contentGain(const EffectiveDefinitionFieldGroups& groups,
                                SignalPathEdgeKind kind, const QUuid& target, SendType type, bool duckTarget)
{
    if (kind == SignalPathEdgeKind::Output)
        return groups.output.outputBusRef == toFieldGroupGuid(target)
            ? std::optional(groups.output.outputBusVolume) : std::nullopt;
    const auto route = contentRoute(groups, target, type, duckTarget);
    return route ? std::optional(VMixNode::dbToLinear(route->second)) : std::nullopt;
}

bool sharedEdit(const PlatformContext& context)
{
    return context.all || context.selected.isEmpty();
}

void setBusRouteLevel(BusRoute& row, float db, const PlatformContext& context)
{
    if (!sharedEdit(context)) {
        for (const auto& platform : context.selected) row.platforms[platform].sendLevelDb = db;
        return;
    }
    row.sendLevelDb = db;
    for (auto& [platform, cells] : row.platforms) cells.sendLevelDb.reset();
    std::erase_if(row.platforms, [](const auto& cell) { return cell.second == BusRouteCells{}; });
}

template <typename Row>
void setGroupRouteLevel(Row& row, float db, const PlatformContext& context)
{
    if (!sharedEdit(context)) {
        for (const auto& platform : context.selected) row.platforms[toFieldGroupGuid(platform)].sendLevel = db;
        return;
    }
    row.sendLevel = db;
    for (auto& [platform, cells] : row.platforms) cells.sendLevel.reset();
    std::erase_if(row.platforms, [](const auto& cell) {
        return cell.second == typename decltype(Row::platforms)::mapped_type{};
    });
}

std::optional<PropertySnapshot> readBusProperty(
    ProjectItem& item, uint8_t property, PlatformScope scope)
{
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(item);
    if (!locator) return {};
    const auto effective = PlayablePropertyAuthoringV1::readProperty(item, *locator, property, scope);
    const auto stored = scope
        ? PlayablePropertyAuthoringV1::readPlatformOverlay(item, *locator, property, *scope)
        : effective;
    if (!effective || !stored) return {};
    return PropertySnapshot{scope, *stored, effective->value};
}

std::vector<FieldGroupScope> selectedScopes(const ProjectData& project, const PlatformContext& context)
{
    std::vector<FieldGroupScope> scopes;
    if (context.all || context.selected.isEmpty()) {
        scopes.push_back({});
        for (const auto& platform : project.platforms) scopes.push_back(toFieldGroupGuid(platform.id));
    } else {
        for (const auto& platform : context.selected) scopes.push_back(toFieldGroupGuid(platform));
    }
    return scopes;
}

template <typename Payload>
void writeGainField(AuthoredGroup<Payload>& group, FieldGroupScope scope,
                    const Payload& effective, Property field)
{
    const auto bit = FieldGroupFieldMask(field);
    if (!scope) {
        if (!group.shared.localHistory) group.shared.localHistory = effective;
        CopyFieldGroupFields(*group.shared.localHistory, effective, bit);
        group.shared.localFields |= bit;
        return;
    }
    auto& overlay = group.platforms.try_emplace(*scope,
        GroupState<Payload>{FieldGroupMode::UseShared, {}}).first->second;
    if (!overlay.localHistory) {
        overlay.localHistory.emplace();
        overlay.localFields = 0;
    }
    CopyFieldGroupFields(*overlay.localHistory, effective, bit);
    overlay.localFields |= bit;
}

template <typename Payload>
void normalizeGainField(AuthoredGroup<Payload>& group, Property field,
                        const std::set<FieldGroupScope>& editedScopes)
{
    const auto bit = FieldGroupFieldMask(field);
    for (auto it = group.platforms.begin(); it != group.platforms.end();) {
        if (!editedScopes.contains(it->first)) { ++it; continue; }
        auto& state = it->second;
        if (state.localHistory && group.shared.localHistory
            && (FieldGroupDifferenceMask(*state.localHistory, *group.shared.localHistory) & bit) == 0)
            state.localFields &= ~bit;
        if (state.localHistory && state.localFields == 0) {
            state.localHistory.reset();
            state.localFields = FieldGroupPropertyMask(Payload::groupId);
        } else if (state.localHistory) {
            Payload fields;
            CopyFieldGroupFields(fields, *state.localHistory, state.localFields);
            state.localHistory = std::move(fields);
        }
        if (state.mode == FieldGroupMode::UseShared && !state.localHistory)
            it = group.platforms.erase(it);
        else
            ++it;
    }
}

class GroupGainCommand final : public TransactionalUndoCommand
{
public:
    GroupGainCommand(QString projectId, std::vector<FieldGroupDefinitionSnapshot> before,
                     std::vector<FieldGroupDefinitionSnapshot> after, const QObject* originator)
        : TransactionalUndoCommand(QObject::tr("Edit Route Volume")), projectId_(std::move(projectId)),
          before_(std::move(before)), after_(std::move(after)), originator_(originator) {}

    bool execute() { return apply(true); }

protected:
    bool apply(bool redo) override
    {
        auto& manager = ProjectManager::instance();
        auto* project = manager.currentProject();
        if (!project || project->projectId != projectId_) return false;
        const auto& expected = redo ? before_ : after_;
        const auto& replacement = redo ? after_ : before_;
        for (const auto& entry : expected) {
            const auto* item = manager.findItemByGuid(entry.definition);
            if (!item || item->fieldGroups != entry.groups) return false;
        }
        if (!validateFieldGroupProject(*project, replacement).ok()) return false;
        for (const auto& entry : replacement)
            manager.findItemByGuid(entry.definition)->fieldGroups = entry.groups;
        if (!refreshFieldGroupDerivedViews(*project)) {
            for (const auto& entry : expected)
                manager.findItemByGuid(entry.definition)->fieldGroups = entry.groups;
            refreshFieldGroupDerivedViews(*project);
            return false;
        }
        for (const auto& entry : replacement) {
            auto* item = manager.findItemByGuid(entry.definition);
            item->markDirty();
            emit manager.itemPropertyChanged(item, QStringLiteral("fieldGroups"), originator_);
        }
        originator_.clear();
        return true;
    }

private:
    QString projectId_;
    std::vector<FieldGroupDefinitionSnapshot> before_;
    std::vector<FieldGroupDefinitionSnapshot> after_;
    QPointer<const QObject> originator_;
};

bool busStateMatches(ProjectItem& item, uint8_t property, const std::vector<PropertySnapshot>& snapshots)
{
    for (const auto& entry : snapshots) {
        const auto current = readBusProperty(item, property, entry.scope);
        if (!current || current->state != entry.state) return false;
    }
    return true;
}

bool restoreBusState(ProjectItem& item, uint8_t property, const std::vector<PropertySnapshot>& snapshots)
{
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(item);
    if (!locator) return false;
    for (const auto& entry : snapshots)
        if (!PlayablePropertyAuthoringV1::restoreProperty(item, *locator, property, entry.state, entry.scope))
            return false;
    return true;
}

void notifyBusGain(ProjectItem& item, const QString& name, const QObject* originator, bool preview)
{
    auto& manager = ProjectManager::instance();
    auto& engine = AudioEngine::instance();
    engine.runtimeSyncCoordinator().markCurrentWorldMutation();
    if (preview) emit manager.itemPropertyPreviewed(&item, name, originator);
    else {
        item.markDirty();
        emit manager.itemPropertyChanged(&item, name, originator);
    }
}

class BusGainCommand final : public TransactionalUndoCommand
{
public:
    BusGainCommand(QString projectId, QUuid bus, uint8_t property,
                   std::vector<PropertySnapshot> before, std::vector<PropertySnapshot> after,
                   const QObject* originator)
        : TransactionalUndoCommand(QObject::tr("Edit Route Volume")), projectId_(std::move(projectId)),
          bus_(bus), property_(property), before_(std::move(before)), after_(std::move(after)),
          originator_(originator) {}

    bool execute() { return apply(true); }

protected:
    bool apply(bool redo) override
    {
        auto& manager = ProjectManager::instance();
        const auto* project = manager.currentProject();
        auto* item = manager.findItemByGuid(bus_);
        if (!project || project->projectId != projectId_ || !item) return false;
        const auto& expected = redo ? before_ : after_;
        const auto& replacement = redo ? after_ : before_;
        if (!busStateMatches(*item, property_, expected)) return false;
        if (!restoreBusState(*item, property_, replacement)) {
            restoreBusState(*item, property_, expected);
            return false;
        }
        notifyBusGain(*item, QStringLiteral("outputBusVolume"), originator_, false);
        originator_.clear();
        return true;
    }

private:
    QString projectId_;
    QUuid bus_;
    uint8_t property_;
    std::vector<PropertySnapshot> before_;
    std::vector<PropertySnapshot> after_;
    QPointer<const QObject> originator_;
};

}

std::optional<float> signalPathGainValue(
    const ProjectData& project, const SignalPathGraph& graph, size_t edgeIndex,
    const ProjectItem* previewItem, const DefinitionFieldGroups* previewGroups)
{
    const auto snapshot = buildFieldGroupEffectiveSnapshot(project, graph.platform,
        [&](const ProjectItem& item) {
            return previewItem && previewGroups && item.guid == previewItem->guid
                ? previewGroups : &item.fieldGroups;
        });
    return signalPathGainValue(snapshot, graph, edgeIndex, previewItem && previewGroups);
}

std::optional<float> signalPathGainValue(
    const FieldGroupEffectiveSnapshot& snapshot, const SignalPathGraph& graph,
    size_t edgeIndex, bool previewing)
{
    if (edgeIndex >= graph.edges.size()) return {};
    const auto& edge = graph.edges[edgeIndex];
    if (edge.kind == SignalPathEdgeKind::Through || !edge.routingSource
        || edge.from.occurrenceIndex >= graph.nodes.size() || edge.to.occurrenceIndex >= graph.nodes.size())
        return {};
    if (!snapshot.ok()) return {};
    const auto target = graph.nodes[edge.to.occurrenceIndex].key.objectGuid;
    const auto* provider = snapshotItem(snapshot, edge.routingSource->objectGuid);
    const auto* bus = snapshotItem(snapshot, target);
    if (!provider || !bus || bus->type != ProjectItemType::Bus) return {};
    if (provider->type == ProjectItemType::Bus) {
        const PlatformScope scope = graph.platform
            ? PlatformScope(fromFieldGroupGuid(*graph.platform)) : std::nullopt;
        if (edge.kind == SignalPathEdgeKind::Output) {
            if (provider->parent != bus) return {};
            const auto value = readBusProperty(*const_cast<ProjectItem*>(provider),
                static_cast<uint8_t>(Property::OutputBusVolume), scope);
            return value ? std::optional(static_cast<float>(value->effective.toDouble())) : std::nullopt;
        }
        const auto sends = effectiveBusSends(*provider, scope);
        if (!sends) return {};
        for (const auto& send : *sends)
            if (send.targetBusID == bus->objectID && send.type == edge.type && send.duckTarget == edge.duckTarget)
                return send.sendLevel;
        return {};
    }
    if (edge.pathIndices.empty()) return {};
    std::optional<float> value;
    for (const auto index : edge.pathIndices) {
        if (index >= graph.paths.size()) return {};
        const auto* groups = effectiveGroups(snapshot, graph.paths[index].identity.sourceGuid);
        if (!groups) return {};
        const auto& source = routingSource(*groups, edge.kind, edge.type);
        if (source.isDefault || fromFieldGroupGuid(source.definition) != edge.routingSource->objectGuid
            || (!previewing && source.scope != edge.routingSource->scope)) return {};
        const auto current = contentGain(*groups, edge.kind, target, edge.type, edge.duckTarget);
        if (!current || (value && *value != *current)) return {};
        value = current;
    }
    return value;
}

struct SignalPathGainEditSession::State {
    bool active = false;
    bool previewed = false;
    QString projectId;
    PlatformContext context;
    QPointer<const QObject> originator;
    SignalPathEdgeKind kind = SignalPathEdgeKind::Through;
    SendType type = SendType::Send;
    bool duckTarget = false;
    QUuid target;
    QUuid bus;
    uint32_t targetBusID = 0;
    uint8_t property = 0;
    std::vector<GroupTarget> targets;
    std::vector<std::pair<QUuid, harken::studio::project_schema::AuthoringGuid>> routes;
    std::optional<BusSendState> routesBefore;
    std::optional<BusSendState> routesAfter;
    QUuid busRow;
    std::vector<Observation> observed;
    std::vector<FieldGroupDefinitionSnapshot> before;
    std::vector<FieldGroupDefinitionSnapshot> after;
    std::vector<PropertySnapshot> busBefore;
    std::vector<PropertySnapshot> busAfter;

    bool current() const
    {
        auto& manager = ProjectManager::instance();
        const auto* project = manager.currentProject();
        if (!project || project->projectId != projectId
            || PlatformContextService::instance().context() != context) return false;
        for (const auto& entry : observed) {
            const auto* item = manager.findItemByGuid(entry.guid);
            if (!item || (item->parent ? item->parent->guid : QUuid{}) != entry.parent
                || item->fieldGroups != entry.groups) return false;
        }
        if (!bus.isNull()) {
            auto* item = manager.findItemByGuid(bus);
            if (!item || (routesAfter ? captureBusSendState(*item) != routesAfter
                                      : !busStateMatches(*item, property, busAfter))) return false;
        }
        return true;
    }
};

SignalPathGainEditSession::SignalPathGainEditSession(
    const SignalPathGraph& graph, size_t edgeIndex, const QObject* originator)
    : state_(std::make_unique<State>())
{
    auto& manager = ProjectManager::instance();
    auto* project = manager.currentProject();
    if (!project || !signalPathGainValue(*project, graph, edgeIndex)) return;
    auto& state = *state_;
    const auto& edge = graph.edges[edgeIndex];
    state.projectId = project->projectId;
    state.context = PlatformContextService::instance().context();
    state.originator = originator;
    state.kind = edge.kind;
    state.type = edge.type;
    state.duckTarget = edge.duckTarget;
    state.target = graph.nodes[edge.to.occurrenceIndex].key.objectGuid;
    auto* target = manager.findItemByGuid(state.target);
    auto* provider = manager.findItemByGuid(edge.routingSource->objectGuid);
    if (!provider || !target) return;
    state.targetBusID = target->objectID;
    const auto scopes = selectedScopes(*project, state.context);
    std::set<QUuid> observed;
    const auto observe = [&](const ProjectItem* item) {
        for (; item; item = item->parent)
            if (observed.insert(item->guid).second)
                state.observed.push_back({item->guid, item->parent ? item->parent->guid : QUuid{}, item->fieldGroups});
    };
    observe(target);
    if (provider->type == ProjectItemType::Bus && state.kind == SignalPathEdgeKind::Send) {
        state.bus = provider->guid;
        observe(provider);
        const auto routes = captureBusSendState(*provider);
        const PlatformScope platform = graph.platform
            ? PlatformScope(fromFieldGroupGuid(*graph.platform)) : std::nullopt;
        const auto& rows = state.type == SendType::Sidechain ? routes->sidechains : routes->sends;
        const auto row = std::find_if(rows.begin(), rows.end(), [&](const BusRoute& entry) {
            const auto route = entry.forPlatform(platform);
            return route.targetBusID == target->objectID && route.duckTarget == state.duckTarget;
        });
        if (row == rows.end()) return;
        state.busRow = row->id;
        state.routesBefore = routes;
        state.routesAfter = routes;
    } else if (provider->type == ProjectItemType::Bus) {
        state.bus = provider->guid;
        state.property = static_cast<uint8_t>(Property::OutputBusVolume);
        observe(provider);
        for (const auto scope : scopes) {
            const auto value = readBusProperty(*provider, state.property,
                scope ? PlatformScope(fromFieldGroupGuid(*scope)) : std::nullopt);
            if (!value) return;
            state.busBefore.push_back(*value);
        }
        state.busAfter = state.busBefore;
    } else if (state.kind == SignalPathEdgeKind::Send) {
        const auto snapshot = buildFieldGroupEffectiveSnapshot(*project, graph.platform);
        if (!snapshot.ok()) return;
        for (const auto index : edge.pathIndices) {
            const auto origin = graph.paths[index].identity.sourceGuid;
            const auto* groups = effectiveGroups(snapshot, origin);
            const auto* item = snapshotItem(snapshot, origin);
            if (!groups || !item) return;
            observe(item);
            const auto& source = routingSource(*groups, state.kind, state.type);
            const auto route = contentRoute(*groups, state.target, state.type, state.duckTarget);
            if (source.isDefault || !route) continue;
            const auto guid = fromFieldGroupGuid(source.definition);
            observe(snapshotItem(snapshot, guid));
            const std::pair entry{guid, route->first};
            if (std::find(state.routes.begin(), state.routes.end(), entry) != state.routes.end()) continue;
            state.routes.push_back(entry);
            if (std::none_of(state.before.begin(), state.before.end(),
                    [&](const auto& snapshotEntry) { return snapshotEntry.definition == guid; }))
                state.before.push_back({guid, manager.findItemByGuid(guid)->fieldGroups});
        }
        if (state.routes.empty()) return;
        state.after = state.before;
    } else {
        std::set<std::pair<QUuid, FieldGroupScope>> targets;
        for (const auto scope : scopes) {
            const auto snapshot = buildFieldGroupEffectiveSnapshot(*project, scope);
            if (!snapshot.ok()) return;
            for (const auto index : edge.pathIndices) {
                const auto origin = graph.paths[index].identity.sourceGuid;
                const auto* groups = effectiveGroups(snapshot, origin);
                const auto* item = snapshotItem(snapshot, origin);
                if (!groups || !item) return;
                observe(item);
                const auto& source = routingSource(*groups, state.kind, state.type);
                if (source.isDefault) continue;
                const bool editsGain = contentGain(*groups, state.kind, state.target, state.type, state.duckTarget).has_value();
                if (!editsGain && ((!state.context.all && !state.context.selected.isEmpty()) || !scope)) continue;
                const auto guid = fromFieldGroupGuid(source.definition);
                observe(snapshotItem(snapshot, guid));
                if (!targets.emplace(guid, scope).second) continue;
                state.targets.push_back({guid, scope, FieldGroupPayload(groups->output), editsGain});
            }
        }
        std::set<QUuid> sharedProviders;
        for (const auto& entry : state.targets)
            if (!entry.scope && entry.editGain) sharedProviders.insert(entry.provider);
        std::erase_if(state.targets, [&](const auto& entry) {
            return !entry.editGain && !sharedProviders.contains(entry.provider);
        });
        if (state.targets.empty()) return;
        std::set<QUuid> providers;
        for (const auto& targetEntry : state.targets)
            if (providers.insert(targetEntry.provider).second)
                state.before.push_back({targetEntry.provider, manager.findItemByGuid(targetEntry.provider)->fieldGroups});
        state.after = state.before;
    }
    state.active = true;
    connect(&manager, &ProjectManager::activePropertyEditsFinishing, this,
        [this](bool commitChanges) { if (commitChanges) commit(); else cancel(); });
    connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged,
        this, [this] { cancel(); });
    connect(&manager, &ProjectManager::projectClosed, this, [this] { cancel(); });
}

SignalPathGainEditSession::~SignalPathGainEditSession()
{
    cancel();
}

bool SignalPathGainEditSession::isActive() const
{
    return state_->active;
}

void SignalPathGainEditSession::preview(double value)
{
    auto& state = *state_;
    if (!state.active) return;
    if (!state.current()) { cancel(); return; }
    if (!std::isfinite(value)) return;
    if (state.kind == SignalPathEdgeKind::Output ? value < -96.0 || value > 24.0
        : value < 0.0 || value > VMixNode::dbToLinear(24.0f)) return;
    const auto db = state.kind == SignalPathEdgeKind::Output
        ? static_cast<float>(value) : std::clamp(VMixNode::linearToDb(static_cast<float>(value)), -96.0f, 24.0f);
    auto& manager = ProjectManager::instance();
    if (state.routesBefore) {
        auto next = *state.routesBefore;
        for (auto& row : state.type == SendType::Sidechain ? next.sidechains : next.sends)
            if (row.id == state.busRow) setBusRouteLevel(row, db, state.context);
        auto* item = manager.findItemByGuid(state.bus);
        if (!restoreBusSendState(*item, next)) return;
        state.routesAfter = std::move(next);
        state.previewed = true;
        notifyBusGain(*item, state.type == SendType::Sidechain ? QStringLiteral("sidechains") : QStringLiteral("sends"),
                      state.originator, true);
        return;
    }
    if (!state.routes.empty()) {
        auto next = state.before;
        for (const auto& [provider, rowId] : state.routes) {
            auto& groups = std::find_if(next.begin(), next.end(), [&](const auto& item) {
                return item.definition == provider;
            })->groups;
            if (state.type == SendType::Send && groups.send.shared.localHistory)
                for (auto& row : groups.send.shared.localHistory->sends)
                    if (row.id == rowId) setGroupRouteLevel(row, db, state.context);
            if (state.type == SendType::Sidechain && groups.sidechain.shared.localHistory)
                for (auto& row : groups.sidechain.shared.localHistory->sidechains)
                    if (row.id == rowId) setGroupRouteLevel(row, db, state.context);
        }
        if (!validateFieldGroupProject(*manager.currentProject(), next).ok()) return;
        state.after = std::move(next);
        state.previewed = true;
        for (const auto& entry : state.after)
            emit manager.fieldGroupsPreviewed(manager.findItemByGuid(entry.definition), entry.groups);
        return;
    }
    if (!state.bus.isNull()) {
        auto next = state.busBefore;
        QJsonValue shared;
        bool writesShared = false;
        for (auto& entry : next) {
            const QJsonValue replacement = db;
            if (!entry.scope) { shared = replacement; writesShared = true; }
            if (entry.scope && writesShared && replacement == shared)
                entry.state = {false, {}};
            else if (replacement != entry.effective)
                entry.state = {true, replacement};
        }
        auto* item = manager.findItemByGuid(state.bus);
        if (!restoreBusState(*item, state.property, next)) {
            restoreBusState(*item, state.property, state.busAfter);
            return;
        }
        for (auto& entry : next) {
            const auto applied = readBusProperty(*item, state.property, entry.scope);
            if (!applied) {
                restoreBusState(*item, state.property, state.busAfter);
                return;
            }
            entry = *applied;
        }
        state.busAfter = std::move(next);
        state.previewed = true;
        notifyBusGain(*item, QStringLiteral("outputBusVolume"), state.originator, true);
        return;
    }
    auto next = state.before;
    for (const auto& entry : state.targets) {
        auto& groups = std::find_if(next.begin(), next.end(), [&](const auto& item) {
            return item.definition == entry.provider;
        })->groups;
        auto output = std::get<OutputFieldGroup>(entry.payload);
        if (entry.editGain) output.outputBusVolume = db;
        writeGainField(groups.output, entry.scope, output, Property::OutputBusVolume);
    }
    for (auto& entry : next) {
        std::set<FieldGroupScope> editedScopes;
        for (const auto& target : state.targets)
            if (target.provider == entry.definition) editedScopes.insert(target.scope);
        normalizeGainField(entry.groups.output, Property::OutputBusVolume, editedScopes);
    }
    if (!validateFieldGroupProject(*manager.currentProject(), next).ok()) return;
    state.after = std::move(next);
    state.previewed = true;
    for (const auto& entry : state.after)
        emit manager.fieldGroupsPreviewed(manager.findItemByGuid(entry.definition), entry.groups);
}

void SignalPathGainEditSession::commit()
{
    auto& state = *state_;
    if (!state.active) return;
    if (!state.current()) { cancel(); return; }
    state.active = false;
    auto& manager = ProjectManager::instance();
    if (state.routesBefore) {
        auto* item = manager.findItemByGuid(state.bus);
        if (*state.routesAfter == *state.routesBefore || !restoreBusSendState(*item, *state.routesBefore)) return;
        const bool sidechain = state.type == SendType::Sidechain;
        ActionManager::instance().execute(QStringLiteral("set_item_property"),
            QJsonObject{{QStringLiteral("authoring_handle"), static_cast<qint64>(item->objectID)},
                        {QStringLiteral("property"), sidechain ? QStringLiteral("sidechains") : QStringLiteral("sends")},
                        {QStringLiteral("value"), VMixNode::routesToVariant(
                             sidechain ? state.routesAfter->sidechains : state.routesAfter->sends, sidechain).toJsonArray()}},
            ActionSource::Internal, state.originator);
        return;
    }
    if (!state.bus.isNull()) {
        auto* item = manager.findItemByGuid(state.bus);
        const auto changed = !busStateMatches(*item, state.property, state.busBefore);
        if (!changed) return;
        if (!restoreBusState(*item, state.property, state.busBefore)) return;
        auto command = std::make_unique<BusGainCommand>(state.projectId, state.bus, state.property,
            state.busBefore, state.busAfter, state.originator);
        if (command->execute()) UndoRedoManager::instance().push(command.release());
        else notifyBusGain(*item, QStringLiteral("outputBusVolume"), state.originator, true);
        return;
    }
    std::vector<FieldGroupDefinitionSnapshot> before, after;
    for (size_t index = 0; index < state.before.size(); ++index) {
        if (state.before[index].groups == state.after[index].groups) continue;
        before.push_back(state.before[index]);
        after.push_back(state.after[index]);
    }
    if (!before.empty()) {
        auto command = std::make_unique<GroupGainCommand>(state.projectId, std::move(before), std::move(after), state.originator);
        if (command->execute()) UndoRedoManager::instance().push(command.release());
    }
    if (state.previewed)
        for (const auto& entry : state.before) {
            auto* item = manager.findItemByGuid(entry.definition);
            if (item) emit manager.fieldGroupsPreviewed(item, item->fieldGroups);
        }
}

void SignalPathGainEditSession::cancel()
{
    auto& state = *state_;
    if (!state.active) return;
    state.active = false;
    auto& manager = ProjectManager::instance();
    const auto* project = manager.currentProject();
    if (!project || project->projectId != state.projectId || !state.previewed) return;
    if (state.routesBefore) {
        auto* item = manager.findItemByGuid(state.bus);
        if (item && captureBusSendState(*item) == state.routesAfter && restoreBusSendState(*item, *state.routesBefore))
            notifyBusGain(*item, state.type == SendType::Sidechain ? QStringLiteral("sidechains") : QStringLiteral("sends"),
                          state.originator, true);
        return;
    }
    if (!state.bus.isNull()) {
        auto* item = manager.findItemByGuid(state.bus);
        if (item && busStateMatches(*item, state.property, state.busAfter)
            && restoreBusState(*item, state.property, state.busBefore))
            notifyBusGain(*item, QStringLiteral("outputBusVolume"), state.originator, true);
        return;
    }
    for (const auto& entry : state.before) {
        auto* item = manager.findItemByGuid(entry.definition);
        if (item) emit manager.fieldGroupsPreviewed(item, item->fieldGroups);
    }
}

}
