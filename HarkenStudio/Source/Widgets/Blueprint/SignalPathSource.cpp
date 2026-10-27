#include "SignalPathSource.h"

#include "BlueprintEdgeKind.h"
#include "SignalPathGainEditing.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/Components/VSound.h"
#include "Core/Components/VMixNode.h"
#include "Core/LanguageContextService.h"
#include "Core/PlayablePropertyAuthoringV1.h"
#include "Core/PlatformContextService.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Core/SoundSourceEditing.h"
#include "Core/SoundSourceSemantics.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>

#include <algorithm>
#include <functional>
#include <set>

namespace VNX {

namespace {

NodeContentInfo nodeContent(ProjectItem& item, PlatformScope scope)
{
    NodeContentInfo content;
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(item);
    const auto property = [&](const char* name) -> std::optional<double> {
        const auto* field = PlayablePropertyAuthoringV1::fieldByName(QLatin1String(name));
        if (!locator || !field) return {};
        const auto state = PlayablePropertyAuthoringV1::readProperty(item, *locator, field->id, scope);
        return state ? std::optional<double>{state->value.toDouble()} : std::nullopt;
    };
    content.volume = property("volume");
    content.pitch = property("pitch");
    if (const auto loop = property("loopCount"))
        content.loopCount = static_cast<uint32_t>(*loop);
    if (const auto* sound = item.comp<VSound>())
        content.offsetOfStart = sound->scheduleOffsetSec;
    return content;
}

QHash<QUuid, ProjectItem*> projectObjects(const ProjectData& project)
{
    QHash<QUuid, ProjectItem*> objects;
    const auto collect = [&](auto&& self, ProjectItem& item) -> void {
        objects.insert(item.guid, &item);
        for (const auto& child : item.children) self(self, *child);
    };
    for (const auto* binding : project.allFileBindings()) {
        if (binding->rootItem) collect(collect, *binding->rootItem);
    }
    return objects;
}

}

void SignalPathSource::loadGraph(const ProjectData& project, SignalPathGraph graph,
    std::shared_ptr<const FieldGroupEffectiveSnapshot> groups)
{
    graph_ = std::move(graph);
    nodes_.clear();
    edges_.clear();
    nodeOccurrences_.clear();
    if (!graph_.ok()) {
        emit sourceChanged();
        return;
    }

    const auto objects = projectObjects(project);

    const auto platform = graph_.platform ? fromFieldGroupGuid(*graph_.platform)
        : PlatformContextService::instance().context().primary;
    const PlatformScope scope = graph_.platform ? PlatformScope{platform} : std::nullopt;
    const auto& language = LanguageContextService::instance().language();
    for (size_t index = 0; index < graph_.nodes.size(); ++index) {
        const auto& node = graph_.nodes[index];
        auto* item = objects.value(node.key.objectGuid);
        Q_ASSERT(item);
        BlueprintNodeDesc desc;
        desc.id = node.objectID;
        desc.title = node.name;
        desc.itemType = node.type;
        desc.kind = node.type == ProjectItemType::Bus ? BlueprintNodeKind::Bus : BlueprintNodeKind::ProjectItem;
        desc.role = node.type == ProjectItemType::Bus ? BlueprintNodeRole::Bus
            : node.type == ProjectItemType::Sound ? BlueprintNodeRole::Source : BlueprintNodeRole::Parent;
        desc.draggable = true;
        desc.occurrenceIndex = index;
        desc.portPairCount = static_cast<int>(node.portPairs.size());
        desc.content = nodeContent(*item, scope);
        if (node.type == ProjectItemType::Sound && !project.projectPath.isEmpty()) {
            const auto previewLanguage = SoundSourceEditing::previewLanguage(*item, language);
            auto path = SoundSourceEditing::activeRelativePath(*item, platform, previewLanguage);
            if (const auto* source = SoundSourceEditing::activeSource(*item, platform, previewLanguage)) {
                const auto range = SoundSourceSemantics::effectiveRange(
                    *source, AssetRegistry::instance().getAsset(source->sourceId));
                if (range && range->trimEndFrame > range->trimStartFrame)
                    desc.waveformRange = WaveformFrameRange{range->trimStartFrame, range->trimEndFrame};
                else
                    path.clear();
            }
            if (!path.isEmpty()) {
                desc.mediaPath = QDir(QFileInfo(project.projectPath).absolutePath())
                    .filePath(QStringLiteral("AudioSources/") + path);
                desc.waveformMode = true;
            }
        }
        nodeOccurrences_[node.objectID].push_back(index);
        nodes_.push_back(std::move(desc));
    }

    if (!groups)
        groups = std::make_shared<FieldGroupEffectiveSnapshot>(
            buildFieldGroupEffectiveSnapshot(project, graph_.platform));
    for (size_t edgeIndex = 0; edgeIndex < graph_.edges.size(); ++edgeIndex) {
        const auto& edge = graph_.edges[edgeIndex];
        const auto& from = nodes_[edge.from.occurrenceIndex];
        const auto& to = nodes_[edge.to.occurrenceIndex];
        BlueprintEdgeDesc desc;
        desc.fromKind = from.kind;
        desc.fromId = from.id;
        desc.toKind = to.kind;
        desc.toId = to.id;
        desc.fromOccurrenceIndex = edge.from.occurrenceIndex;
        desc.toOccurrenceIndex = edge.to.occurrenceIndex;
        desc.fromPortPairIndex = static_cast<int>(edge.from.portPairIndex);
        desc.toPortPairIndex = static_cast<int>(edge.to.portPairIndex);
        switch (edge.kind) {
        case SignalPathEdgeKind::Through:
            desc.edgeKind = static_cast<uint8_t>(BlueprintEdgeKind::Through);
            break;
        case SignalPathEdgeKind::Output:
            desc.edgeKind = static_cast<uint8_t>(BlueprintEdgeKind::BusRoute);
            desc.gainContent.outputVolume = signalPathGainValue(*groups, graph_, edgeIndex);
            break;
        case SignalPathEdgeKind::Send:
            desc.edgeKind = static_cast<uint8_t>(BlueprintEdgeKind::Send);
            desc.gainContent.sendVolume = signalPathGainValue(*groups, graph_, edgeIndex);
            break;
        }
        edges_.push_back(std::move(desc));
    }
    emit sourceChanged();
}

std::span<const size_t> SignalPathSource::refreshNodeContents(ProjectItem& item, const QString& propertyName)
{
    const auto occurrences = nodeOccurrences_.constFind(item.objectID);
    if (occurrences == nodeOccurrences_.cend()) return {};
    const PlatformScope scope = graph_.platform
        ? PlatformScope{fromFieldGroupGuid(*graph_.platform)} : std::nullopt;
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(item);
    const auto* property = PlayablePropertyAuthoringV1::fieldByName(propertyName);
    QVariant value;
    if (locator && property) {
        const auto state = PlayablePropertyAuthoringV1::readProperty(item, *locator, property->id, scope);
        if (state) value = state->value.toVariant();
    } else if (propertyName == QLatin1String("scheduleOffsetSec")) {
        if (const auto* sound = item.comp<VSound>()) value = sound->scheduleOffsetSec;
    }
    if (!value.isValid()) return {};
    for (const size_t index : *occurrences) {
        auto& info = nodes_[index].content;
        if (propertyName == QLatin1String("volume")) info.volume = value.toFloat();
        else if (propertyName == QLatin1String("pitch")) info.pitch = value.toFloat();
        else if (propertyName == QLatin1String("scheduleOffsetSec")) info.offsetOfStart = value.toDouble();
        else if (propertyName == QLatin1String("loopCount")) info.loopCount = value.toUInt();
        else return {};
    }
    return *occurrences;
}

void SignalPathSource::clear()
{
    graph_ = {};
    nodes_.clear();
    edges_.clear();
    nodeOccurrences_.clear();
    emit sourceChanged();
}

namespace {

SignalPathLayout layoutGraph(const SignalPathGraph& graph_, const std::vector<QSizeF>& sizes,
                            CanvasFlowDirection direction,
                            const std::vector<std::optional<QPointF>>& fixedPositions,
                            size_t objectCount)
{
    const size_t count = graph_.nodes.size();
    if (!graph_.ok() || count == 0) return {};
    Q_ASSERT(sizes.size() == count);
    Q_ASSERT(fixedPositions.empty() || fixedPositions.size() == count);
    const bool vertical = direction == CanvasFlowDirection::TB || direction == CanvasFlowDirection::BT;
    const bool reverse = direction == CanvasFlowDirection::RL || direction == CanvasFlowDirection::BT;
    const auto along = [vertical](const QSizeF& size) { return vertical ? size.height() : size.width(); };
    const auto across = [vertical](const QSizeF& size) { return vertical ? size.width() : size.height(); };
    constexpr qreal columnGap = 96.0;
    constexpr qreal rowGap = 40.0;

    std::vector<std::vector<size_t>> predecessors(count);
    std::vector<size_t> remaining(count), distance(count), order;
    for (const auto& edge : graph_.edges) {
        predecessors[edge.to.occurrenceIndex].push_back(edge.from.occurrenceIndex);
        ++remaining[edge.from.occurrenceIndex];
    }
    for (size_t index = 0; index < count; ++index) {
        if (remaining[index] == 0) order.push_back(index);
    }
    for (size_t cursor = 0; cursor < order.size(); ++cursor) {
        const size_t node = order[cursor];
        for (size_t predecessor : predecessors[node]) {
            distance[predecessor] = std::max(distance[predecessor], distance[node] + 1);
            if (--remaining[predecessor] == 0) order.push_back(predecessor);
        }
    }
    Q_ASSERT(order.size() == count);
    const size_t depth = *std::max_element(distance.begin(), distance.end());
    std::vector<std::vector<size_t>> columns(depth + 1);
    std::vector<qreal> widths(depth + 1), crossPositions(count), crossSizes(count);
    qreal pathStride = 0.0;
    for (size_t index = 0; index < count; ++index) {
        const size_t rank = depth - distance[index];
        columns[rank].push_back(index);
        widths[rank] = std::max(widths[rank], along(sizes[index]));
        crossSizes[index] = across(sizes[index]);
        pathStride = std::max(pathStride, across(sizes[index]) + rowGap);
    }
    QHash<QUuid, size_t> sourceRanks;
    std::vector<size_t> pathRanks;
    for (const auto& path : graph_.paths) {
        if (!sourceRanks.contains(path.identity.sourceGuid))
            sourceRanks.insert(path.identity.sourceGuid, sourceRanks.size());
        pathRanks.push_back(sourceRanks.value(path.identity.sourceGuid));
    }
    for (size_t index = 0; index < count; ++index) {
        const auto& pairs = graph_.nodes[index].portPairs;
        qreal sum = 0.0;
        std::set<size_t> memberships;
        for (size_t pair = 0; pair < pairs.size(); ++pair) {
            const auto pin = NodeItem::pinCenter(NodePin::Input, static_cast<int>(pair),
                static_cast<int>(pairs.size()), sizes[index], direction, index >= objectCount);
            const qreal offset = vertical ? pin.x() : pin.y();
            for (size_t path : pairs[pair].pathIndices) {
                if (memberships.insert(pathRanks[path]).second)
                    sum += static_cast<qreal>(pathRanks[path]) * pathStride - offset;
            }
        }
        crossPositions[index] = memberships.empty() ? 0.0 : sum / static_cast<qreal>(memberships.size());
    }

    SignalPathLayout result;
    result.nodePositions.resize(count);
    qreal columnPosition = 0.0;
    for (size_t rank = 0; rank < columns.size(); ++rank) {
        auto& column = columns[rank];
        std::stable_sort(column.begin(), column.end(), [&](size_t a, size_t b) {
            return crossPositions[a] < crossPositions[b];
        });
        qreal shift = 0.0;
        for (size_t row = 1; row < column.size(); ++row) {
            const size_t index = column[row];
            const size_t previous = column[row - 1];
            const qreal packed = std::max(crossPositions[index],
                crossPositions[previous] + crossSizes[previous] + rowGap);
            shift += packed - crossPositions[index];
            crossPositions[index] = packed;
        }
        shift /= static_cast<qreal>(column.size());
        for (size_t index : column) {
            crossPositions[index] -= shift;
            const qreal major = reverse ? -columnPosition - along(sizes[index]) : columnPosition;
            const qreal minor = crossPositions[index];
            result.nodePositions[index] = vertical ? QPointF(minor, major) : QPointF(major, minor);
        }
        columnPosition += widths[rank] + columnGap;
    }
    if (std::none_of(fixedPositions.begin(), fixedPositions.end(),
            [](const auto& position) { return position.has_value(); })) return result;

    std::vector<qreal> majorPositions(count);
    for (size_t index = 0; index < count; ++index) {
        const auto position = fixedPositions[index].value_or(result.nodePositions[index]);
        const qreal major = vertical ? position.y() : position.x();
        majorPositions[index] = reverse ? -major - along(sizes[index]) : major;
        result.nodePositions[index] = position;
    }
    for (size_t node : order) {
        for (size_t predecessor : predecessors[node]) {
            if (!fixedPositions[predecessor])
                majorPositions[predecessor] = std::min(majorPositions[predecessor],
                    majorPositions[node] - along(sizes[predecessor]) - columnGap);
        }
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        if (fixedPositions[*it]) continue;
        for (size_t predecessor : predecessors[*it])
            majorPositions[*it] = std::max(majorPositions[*it],
                majorPositions[predecessor] + along(sizes[predecessor]) + columnGap);
    }

    std::vector<QRectF> occupied;
    for (size_t index = 0; index < count; ++index) {
        if (fixedPositions[index]) occupied.emplace_back(*fixedPositions[index], sizes[index]);
    }
    for (size_t node : order) {
        if (fixedPositions[node]) continue;
        auto preferred = result.nodePositions[node];
        const qreal major = reverse ? -majorPositions[node] - along(sizes[node]) : majorPositions[node];
        (vertical ? preferred.ry() : preferred.rx()) = major;
        const auto place = [&](bool forward) {
            QPointF position = preferred;
            bool moved;
            do {
                moved = false;
                for (const auto& obstacle : occupied) {
                    const QRectF bounds(position, sizes[node]);
                    if (!bounds.adjusted(-rowGap, -rowGap, rowGap, rowGap).intersects(obstacle)) continue;
                    if (vertical)
                        position.setX(forward ? obstacle.right() + rowGap
                            : obstacle.left() - sizes[node].width() - rowGap);
                    else
                        position.setY(forward ? obstacle.bottom() + rowGap
                            : obstacle.top() - sizes[node].height() - rowGap);
                    moved = true;
                }
            } while (moved);
            return position;
        };
        const auto forward = place(true);
        const auto backward = place(false);
        const auto forwardOffset = forward - preferred;
        const auto backwardOffset = backward - preferred;
        result.nodePositions[node] = QPointF::dotProduct(forwardOffset, forwardOffset)
                <= QPointF::dotProduct(backwardOffset, backwardOffset) ? forward : backward;
        occupied.emplace_back(result.nodePositions[node], sizes[node]);
    }
    return result;
}

}

SignalPathLayout SignalPathSource::layout(const std::vector<QSizeF>& sizes,
    CanvasFlowDirection direction, const std::vector<std::optional<QPointF>>& fixedPositions,
    const std::vector<QSizeF>& gainSizes,
    const std::vector<std::optional<QPointF>>& fixedGainPositions) const
{
    if (gainSizes.empty()) return layoutGraph(graph_, sizes, direction, fixedPositions, graph_.nodes.size());
    Q_ASSERT(gainSizes.size() == graph_.edges.size());
    Q_ASSERT(fixedGainPositions.empty() || fixedGainPositions.size() == graph_.edges.size());
    auto graph = graph_;
    auto expandedSizes = sizes;
    auto positions = fixedPositions;
    positions.resize(graph.nodes.size());
    std::vector<std::optional<size_t>> gains(graph.edges.size());
    const auto edgeCount = graph.edges.size();
    for (size_t index = 0; index < edgeCount; ++index) {
        if (graph.edges[index].kind == SignalPathEdgeKind::Through) continue;
        const auto nodeIndex = graph.nodes.size();
        gains[index] = nodeIndex;
        SignalPathNode node;
        node.portPairs.push_back({{}, graph.edges[index].pathIndices});
        graph.nodes.push_back(std::move(node));
        expandedSizes.push_back(gainSizes[index]);
        positions.push_back(fixedGainPositions.empty() ? std::nullopt : fixedGainPositions[index]);
        auto downstream = graph.edges[index];
        graph.edges[index].to = {nodeIndex, 0};
        downstream.from = {nodeIndex, 0};
        graph.edges.push_back(std::move(downstream));
    }
    auto result = layoutGraph(graph, expandedSizes, direction, positions, graph_.nodes.size());
    if (result.nodePositions.empty()) return result;
    result.gainPositions.resize(edgeCount);
    for (size_t index = 0; index < edgeCount; ++index)
        if (gains[index]) result.gainPositions[index] = result.nodePositions[*gains[index]];
    result.nodePositions.resize(graph_.nodes.size());
    return result;
}

}
