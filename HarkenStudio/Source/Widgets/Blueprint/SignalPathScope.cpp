#include "Core/Components/VSelection.h"
#include "SignalPathScope.h"

#include "Core/Components/VTrack.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Core/PlaybackReferenceValidation.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <functional>

namespace VNX {
namespace {

using Occurrences = std::vector<PlayablePropertyLocatorV1>;

Occurrences playbackOccurrences(const ProjectItem& owner, PlatformScope scope)
{
    Occurrences result;
    if (const auto* timeline = owner.comp<VTrack>()) {
        for (const auto& clip : timeline->clips.items) {
            const auto* target = findDirectPlaybackChild(owner, clip.targetGuid);
            if (validateDirectPlaybackTarget(owner, clip.targetGuid, target)) {
                if (auto locator = PlayablePropertyAuthoringV1::trackClipLocator(owner, clip.clipGuid))
                    result.push_back(*locator);
            }
        }
    } else if (owner.type == ProjectItemType::Container) {
        for (const auto& row : owner.comp<VSelection>()->activeRows(scope)) {
            const auto* target = findDirectPlaybackChild(owner, row.childGuid);
            if (validateDirectPlaybackTarget(owner, row.childGuid, target)) {
                if (auto locator = PlayablePropertyAuthoringV1::containerChildLocator(
                        owner, row.stableOccurrenceOrdinal, scope))
                    result.push_back(*locator);
            }
        }
    }
    return result;
}

void collectPaths(const ProjectItem& item, const Occurrences& outer,
                  std::vector<SignalPathIdentity>& result, PlatformScope scope)
{
    if (!PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item.type)) return;
    const size_t before = result.size();
    QSet<QUuid> placed;
    for (const auto& occurrence : playbackOccurrences(item, scope)) {
        Occurrences uses{occurrence};
        uses.insert(uses.end(), outer.begin(), outer.end());
        collectPaths(*findDirectPlaybackChild(item, occurrence.targetGuid), uses, result, scope);
        placed.insert(occurrence.targetGuid);
    }
    for (const auto& child : item.children) {
        if (!placed.contains(child->guid)) collectPaths(*child, outer, result, scope);
    }
    if (result.size() == before) result.push_back({item.guid, outer});
}

std::vector<Occurrences> ancestorOccurrences(const ProjectItem& item, const Occurrences& constraints,
                                            PlatformScope scope)
{
    std::vector<Occurrences> result(1);
    size_t constrained = 0;
    for (const auto* child = &item; child->parent; child = child->parent) {
        const auto& owner = *child->parent;
        Occurrences uses = playbackOccurrences(owner, scope);
        std::erase_if(uses, [&](const auto& use) { return use.targetGuid != child->guid; });
        if (constrained < constraints.size() && constraints[constrained].ownerGuid == owner.guid) {
            const auto& required = constraints[constrained++];
            std::erase_if(uses, [&](const auto& use) { return use != required; });
            if (uses.empty()) return {};
        }
        if (uses.empty()) continue;
        std::vector<Occurrences> expanded;
        for (const auto& previous : result) {
            for (const auto& use : uses) {
                auto next = previous;
                next.push_back(use);
                expanded.push_back(std::move(next));
            }
        }
        result = std::move(expanded);
    }
    return constrained == constraints.size() ? result : std::vector<Occurrences>{};
}

}

SignalPathGraph buildProjectSignalPathGraph(const ProjectData& project, FieldGroupScope platform,
    std::shared_ptr<const FieldGroupEffectiveSnapshot> groups)
{
    const PlatformScope scope = platform ? PlatformScope{fromFieldGroupGuid(*platform)} : std::nullopt;
    std::vector<SignalPathIdentity> sources;
    std::vector<uint32_t> busRoots;
    for (const auto* binding : project.allFileBindings()) {
        const auto* root = binding->rootItem.get();
        if (!root) continue;
        const auto bucket = ItemTypeCatalog::bucketOf(root->type);
        if (bucket == ItemTypeCatalog::Bucket::Layers) {
            collectPaths(*root, {}, sources, scope);
        } else if (bucket == ItemTypeCatalog::Bucket::Busses) {
            busRoots.push_back(root->objectID);
        }
    }
    return buildSignalPathGraph(project, sources, platform, busRoots, std::move(groups));
}

SignalPathGraph buildSignalPathGraphForScope(
    const ProjectData& project, const SignalPathScope& scope, FieldGroupScope platform)
{
    return buildSignalPathGraphForScopes(project, {scope}, platform);
}

SignalPathGraph buildSignalPathGraphForScopes(
    const ProjectData& project, const std::vector<SignalPathScope>& scopes, FieldGroupScope platform)
{
    const PlatformScope platformScope = platform ? PlatformScope{fromFieldGroupGuid(*platform)} : std::nullopt;
    QHash<QUuid, const ProjectItem*> objects;
    std::vector<SignalPathIdentity> allSources;
    const std::function<void(const ProjectItem&)> collect = [&](const ProjectItem& item) {
        objects.insert(item.guid, &item);
        for (const auto& child : item.children) collect(*child);
    };
    for (const auto* binding : project.allFileBindings()) {
        if (binding->rootItem) {
            collect(*binding->rootItem);
            collectPaths(*binding->rootItem, {}, allSources, platformScope);
        }
    }
    std::vector<SignalPathIdentity> sources;
    std::vector<uint32_t> busRoots;
    QSet<QUuid> busGuids;
    for (const auto& scope : scopes) {
        const auto* root = objects.value(scope.objectGuid);
        if (!root) continue;
        if (root->type == ProjectItemType::Bus) {
            busRoots.push_back(root->objectID);
            busGuids.insert(root->guid);
            continue;
        }
        const auto contexts = ancestorOccurrences(*root, scope.occurrences, platformScope);
        if (contexts.empty()) {
            SignalPathGraph graph;
            graph.platform = platform;
            graph.diagnostics.push_back({{}, QStringLiteral("Signal path placement is unavailable.")});
            return graph;
        }
        for (const auto& outer : contexts) collectPaths(*root, outer, sources, platformScope);
    }
    if (!busRoots.empty()) {
        auto graph = buildSignalPathGraph(project, allSources, platform);
        if (!graph.ok()) return graph;
        for (const auto& path : graph.paths) {
            const bool reachesBus = std::any_of(path.edgeIndices.begin(), path.edgeIndices.end(),
                [&](size_t index) {
                    return busGuids.contains(graph.nodes[graph.edges[index].to.occurrenceIndex].key.objectGuid);
                });
            if (reachesBus) sources.push_back(path.identity);
        }
    }

    return buildSignalPathGraph(project, sources, platform, busRoots);
}

PlacementContext signalPathPlacementContext(const Occurrences& occurrences)
{
    std::vector<PlacementOccurrence> result;
    for (const auto& use : occurrences) {
        if (use.kind == PlayablePropertyTargetKindV1::TrackClip)
            result.push_back({use.ownerGuid, use.occurrenceGuid, use.targetGuid});
    }
    return PlacementContext(std::move(result));
}

}
