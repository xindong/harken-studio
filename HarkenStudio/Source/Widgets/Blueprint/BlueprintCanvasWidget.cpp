
#include "BlueprintCanvasWidget.h"
#include "SignalPathPlayback.h"
#include "SignalPathFocus.h"
#include "SignalPathEditing.h"
#include "SignalPathGainEditing.h"
#include "Core/AudioEngine.h"

#include "BlueprintEdgeKind.h"
#include "BlueprintConnectCommand.h"
#include "Core/PropertyEditSession.h"
#include "NodeContentEditor.h"
#include "BlueprintImportUtils.h"
#include "SignalChainSource.h"
#include "Core/ActionManager.h"
#include "Core/AssetPipeline/AudioImportGate.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/AssetPipeline/MediaPool.h"
#include "Core/Components/VMixNode.h"
#include "Core/Components/VSelection.h"
#include "Core/FontManager.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/LanguageContextService.h"
#include "Core/PlayablePropertyAuthoringV1.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectTypes.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Core/UndoCommands.h"
#include "Widgets/Common/ToastPresenter.h"
#include "Widgets/LogPanel/LogPanel.h"
#include "Core/ThemeManager.h"
#include "Core/TransitionRehearsal.h"
#include "Core/VariableRehearsal.h"
#include "NodeItem.h"
#include "Widgets/Shared/LocationPingAnimation.h"
#include "Widgets/Shared/RenameClickHandler.h"
#include "ConnectionPath.h"
#include "Widgets/Shared/ProjectItemMenuController.h"
#include "Widgets/Shared/ProjectItemMime.h"

#include <QDir>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsProxyWidget>
#include <QJsonArray>
#include <QPainterPath>
#include <QJsonObject>
#include <QMimeData>
#include <QMouseEvent>
#include <QHelpEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineF>
#include <QToolTip>
#include <QPainter>
#include <QPointer>
#include <QColor>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QStringList>
#include <QUrl>
#include <QVariantAnimation>
#include <QAction>
#include <QApplication>
#include <QMenu>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <map>
#include <unordered_map>

namespace VNX {

namespace {

constexpr bool kTransitionWatchVisible = false;

ProjectItem* canvasFindItem(ProjectData* project, uint32_t id)
{
    if (!project || id == 0) return nullptr;
    std::function<ProjectItem*(ProjectItem*)> walk = [&](ProjectItem* node) -> ProjectItem* {
        if (!node) return nullptr;
        if (node->objectID == id) return node;
        for (auto const& child : node->children) {
            if (auto* hit = walk(child.get())) return hit;
        }
        return nullptr;
    };
    for (const auto* binding : project->allFileBindings()) {
        if (!binding || !binding->rootItem) continue;
        if (auto* hit = walk(binding->rootItem.get())) return hit;
    }
    return nullptr;
}

NodeRole translateRole(BlueprintNodeRole r)
{
    switch (r) {
    case BlueprintNodeRole::Source:   return NodeRole::Source;
    case BlueprintNodeRole::Self:     return NodeRole::Self;
    case BlueprintNodeRole::Parent:   return NodeRole::Parent;
    case BlueprintNodeRole::Bus:        return NodeRole::Bus;
    case BlueprintNodeRole::ReceiveBus: return NodeRole::ReceiveBus;
    }
    return NodeRole::Self;
}

// Pin "signal domain" is a pure UI inference (never sourced from the engine):
// absolute-leaf items receive a Control trigger and emit Audio; everything else
// routes Audio straight through.
NodePin::Domain inputDomainForDesc(const BlueprintNodeDesc& d)
{
    if (d.kind == BlueprintNodeKind::ProjectItem &&
        !ItemTypeCatalog::canAcceptAnyChild(d.itemType)) {
        return NodePin::Domain::Control;
    }
    return NodePin::Domain::Audio;
}

QPainterPath buildTempBezier(const QPointF& a, const QPointF& b,
                            CanvasFlowDirection direction, bool reverse)
{
    const bool vertical = direction == CanvasFlowDirection::TB || direction == CanvasFlowDirection::BT;
    const qreal delta = vertical ? b.y() - a.y() : b.x() - a.x();
    const qreal distance = std::max<qreal>(30.0, std::abs(delta) * 0.3);
    const bool positive = direction == CanvasFlowDirection::LR || direction == CanvasFlowDirection::TB;
    const qreal offset = positive != reverse ? distance : -distance;
    const QPointF tangent = vertical ? QPointF(0, offset) : QPointF(offset, 0);
    QPainterPath p(a);
    p.cubicTo(a + tangent, b - tangent, b);
    return p;
}

// Topmost NodePin under a scene point, skipping the drag preview wire (which
// sits above everything) and node bodies so target detection stays accurate.
NodePin* firstPinAt(QGraphicsScene* scene, const QPointF& scenePoint)
{
    if (scene == nullptr) return nullptr;
    const auto items = scene->items(scenePoint, Qt::IntersectsItemShape,
                                    Qt::DescendingOrder, QTransform());
    for (QGraphicsItem* it : items) {
        if (auto* pin = dynamic_cast<NodePin*>(it)) return pin;
    }
    return nullptr;
}

VNX::NodeItem* findOwningNodeItem(QGraphicsItem* hit)
{
    while (hit) {
        if (auto* node = dynamic_cast<VNX::NodeItem*>(hit)) {
            return node;
        }
        hit = hit->parentItem();
    }
    return nullptr;
}

constexpr const char* kFlowSettingsKey = "Blueprint/CanvasFlowDirection";

const char* flowDirectionToString(CanvasFlowDirection d)
{
    switch (d) {
    case CanvasFlowDirection::LR: return "LR";
    case CanvasFlowDirection::RL: return "RL";
    case CanvasFlowDirection::TB: return "TB";
    case CanvasFlowDirection::BT: return "BT";
    }
    return "LR";
}

CanvasFlowDirection flowDirectionFromString(const QString& s)
{
    const QString u = s.trimmed().toUpper();
    if (u == QLatin1String("RL")) return CanvasFlowDirection::RL;
    if (u == QLatin1String("TB")) return CanvasFlowDirection::TB;
    if (u == QLatin1String("BT")) return CanvasFlowDirection::BT;
    return CanvasFlowDirection::LR;
}

QString pathPositionKey(const QString& projectId, CanvasFlowDirection direction,
                        const SignalPathNodeKey& key)
{
    QByteArray identity;
    QDataStream stream(&identity, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_8);
    stream << projectId << QString::fromLatin1(flowDirectionToString(direction));
    stream << key.objectGuid;
    return QStringLiteral("Blueprint/RouteGainLayout/Nodes/")
        + QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}

QByteArray pathEdgeIdentity(const SignalPathGraph& graph, size_t index)
{
    const auto& edge = graph.edges[index];
    QByteArray identity;
    QDataStream stream(&identity, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_8);
    stream << graph.nodes[edge.from.occurrenceIndex].key.objectGuid
        << static_cast<quint64>(edge.from.portPairIndex)
        << graph.nodes[edge.to.occurrenceIndex].key.objectGuid
        << static_cast<quint64>(edge.to.portPairIndex) << static_cast<int>(edge.kind)
        << edge.routingSource.has_value();
    if (edge.routingSource) {
        stream << edge.routingSource->objectGuid << edge.routingSource->scope.has_value();
        if (edge.routingSource->scope) stream << fromFieldGroupGuid(*edge.routingSource->scope);
    }
    return identity;
}

bool passesViewportFilter(const SignalPathNode& node, bool soundsFiltered)
{
    return !soundsFiltered || node.type != ProjectItemType::Sound;
}

struct BusTree
{
    QSet<QUuid> buses;
    QSet<QUuid> upstreamBuses;
    QSet<size_t> paths;
};

BusTree busTree(const SignalPathGraph& graph, const QSet<QUuid>& roots, bool soundsFiltered = false)
{
    BusTree result{roots, {}, {}};
    if (roots.empty()) return result;
    std::vector<std::vector<size_t>> inputs(graph.nodes.size());
    for (const auto& edge : graph.edges) {
        if (graph.nodes[edge.from.occurrenceIndex].type == ProjectItemType::Bus
            && graph.nodes[edge.to.occurrenceIndex].type == ProjectItemType::Bus
            && edge.kind != SignalPathEdgeKind::Through)
            inputs[edge.to.occurrenceIndex].push_back(edge.from.occurrenceIndex);
    }
    std::vector<size_t> pending;
    for (size_t index = 0; index < graph.nodes.size(); ++index) {
        if (graph.nodes[index].type == ProjectItemType::Bus
            && roots.contains(graph.nodes[index].key.objectGuid))
            pending.push_back(index);
    }
    for (size_t cursor = 0; cursor < pending.size(); ++cursor) {
        for (size_t index : inputs[pending[cursor]]) {
            const auto& guid = graph.nodes[index].key.objectGuid;
            result.upstreamBuses.insert(guid);
            if (result.buses.contains(guid)) continue;
            result.buses.insert(guid);
            pending.push_back(index);
        }
    }
    for (const auto& edge : graph.edges) {
        if (!result.buses.contains(graph.nodes[edge.to.occurrenceIndex].key.objectGuid)) continue;
        if (!passesViewportFilter(graph.nodes[edge.from.occurrenceIndex], soundsFiltered)) continue;
        for (size_t index : edge.pathIndices) result.paths.insert(index);
    }
    return result;
}

struct TreeVisibility
{
    std::vector<bool> nodes;
    std::vector<bool> edges;
    std::vector<bool> paths;
};

std::vector<std::vector<size_t>> viewportInputs(const SignalPathGraph& graph, bool soundsFiltered)
{
    std::vector<std::vector<size_t>> inputs(graph.nodes.size());
    for (size_t index = 0; index < graph.edges.size(); ++index) {
        const auto& edge = graph.edges[index];
        if (passesViewportFilter(graph.nodes[edge.from.occurrenceIndex], soundsFiltered)
            && passesViewportFilter(graph.nodes[edge.to.occurrenceIndex], soundsFiltered))
            inputs[edge.to.occurrenceIndex].push_back(index);
    }
    return inputs;
}

TreeVisibility treeVisibility(const SignalPathGraph& graph, const ProjectData& project,
                              const QSet<QUuid>& hiddenHierarchyRoots, const QSet<QUuid>& hiddenBuses,
                              const auto& reveals, bool soundsFiltered = false)
{
    QSet<QUuid> hiddenObjects;
    std::vector<QUuid> ancestors;
    const auto collect = [&](const ProjectItem& item, const auto& visit) -> void {
        const bool hidden = std::any_of(ancestors.begin(), ancestors.end(), [&](const QUuid& guid) {
            const auto found = reveals.find(guid);
            return found == reveals.end() || !found->second.nodes.contains(item.guid);
        });
        if (hidden) hiddenObjects.insert(item.guid);
        const bool root = hiddenHierarchyRoots.contains(item.guid);
        if (root) ancestors.push_back(item.guid);
        for (const auto& child : item.children) visit(*child, visit);
        if (root) ancestors.pop_back();
    };
    for (const auto* binding : project.allFileBindings()) {
        const auto* item = binding->rootItem.get();
        if (item && ItemTypeCatalog::bucketOf(item->type) == ItemTypeCatalog::Bucket::Layers)
            collect(*item, collect);
    }
    BusTree busExclusions;
    for (const auto& guid : hiddenBuses) {
        auto tree = busTree(graph, {guid}, soundsFiltered);
        if (const auto found = reveals.find(guid); found != reveals.end()) {
            tree.upstreamBuses.subtract(found->second.nodes);
            tree.paths.removeIf([&](size_t index) {
                return found->second.sources.contains(graph.paths[index].identity.sourceGuid);
            });
        }
        busExclusions.upstreamBuses.unite(tree.upstreamBuses);
        busExclusions.paths.unite(tree.paths);
    }
    TreeVisibility result;
    result.paths.reserve(graph.paths.size());
    for (size_t index = 0; index < graph.paths.size(); ++index) {
        const auto& path = graph.paths[index];
        const bool retainedRoot = std::any_of(path.computation.begin(), path.computation.end(),
            [&hiddenHierarchyRoots, &graph, &hiddenObjects](const SignalPathEndpoint& endpoint) {
                const auto& guid = graph.nodes[endpoint.occurrenceIndex].key.objectGuid;
                return hiddenHierarchyRoots.contains(guid) && !hiddenObjects.contains(guid);
            });
        result.paths.push_back((!hiddenObjects.contains(path.identity.sourceGuid) || retainedRoot)
            && !busExclusions.paths.contains(index));
    }
    result.nodes.reserve(graph.nodes.size());
    for (const auto& node : graph.nodes) {
        bool hasPaths = false;
        bool hasVisiblePath = false;
        for (const auto& pair : node.portPairs) {
            for (size_t pathIndex : pair.pathIndices) {
                hasPaths = true;
                hasVisiblePath |= !busExclusions.paths.contains(pathIndex);
            }
        }
        result.nodes.push_back(!hiddenObjects.contains(node.key.objectGuid)
            && !busExclusions.upstreamBuses.contains(node.key.objectGuid)
            && passesViewportFilter(node, soundsFiltered)
            && (node.type == ProjectItemType::Bus || !hasPaths || hasVisiblePath));
    }
    result.edges.reserve(graph.edges.size());
    for (const auto& edge : graph.edges) {
        const bool busEdge = graph.nodes[edge.from.occurrenceIndex].type == ProjectItemType::Bus
            && graph.nodes[edge.to.occurrenceIndex].type == ProjectItemType::Bus;
        result.edges.push_back(result.nodes[edge.from.occurrenceIndex]
            && result.nodes[edge.to.occurrenceIndex]
            && (busEdge || edge.pathIndices.empty()
                || std::any_of(edge.pathIndices.begin(), edge.pathIndices.end(),
                    [&result](size_t pathIndex) { return result.paths[pathIndex]; })));
    }
    return result;
}

struct UpstreamScope
{
    QSet<size_t> edges;
    QSet<size_t> paths;
};

UpstreamScope upstreamScope(const SignalPathGraph& graph, size_t root,
                            const std::vector<std::vector<size_t>>& inputs)
{
    UpstreamScope result;
    for (size_t index : inputs[root])
        for (size_t pathIndex : graph.edges[index].pathIndices) result.paths.insert(pathIndex);
    std::vector<size_t> pending{root};
    QSet<size_t> visited{root};
    for (size_t cursor = 0; cursor < pending.size(); ++cursor) {
        for (size_t index : inputs[pending[cursor]]) {
            const auto& edge = graph.edges[index];
            const bool busEdge = graph.nodes[edge.from.occurrenceIndex].type == ProjectItemType::Bus
                && graph.nodes[edge.to.occurrenceIndex].type == ProjectItemType::Bus;
            if (!busEdge && !std::any_of(edge.pathIndices.begin(), edge.pathIndices.end(),
                    [&result](size_t pathIndex) { return result.paths.contains(pathIndex); })) continue;
            result.edges.insert(index);
            if (visited.contains(edge.from.occurrenceIndex)) continue;
            visited.insert(edge.from.occurrenceIndex);
            pending.push_back(edge.from.occurrenceIndex);
        }
    }
    return result;
}

}

size_t BlueprintCanvasWidget::NodeKeyHash::operator()(NodeKey const& k) const noexcept
{
    const auto objectHash = std::hash<uint64_t>()(
        (static_cast<uint64_t>(static_cast<uint8_t>(k.kind)) << 32) ^
        static_cast<uint64_t>(k.id));
    return objectHash ^ (std::hash<std::optional<size_t>>{}(k.occurrenceIndex) << 1);
}

BlueprintCanvasWidget::BlueprintCanvasWidget(QWidget* parent)
    : BlueprintCanvasView(parent)
    , chainSource_(std::make_unique<SignalChainSource>(nullptr))
    , pathSource_(std::make_unique<SignalPathSource>(nullptr))
{
    scene_ = new QGraphicsScene(this);
    setScene(scene_);
    initializeNodeEditors_();
    toast_ = new ToastPresenter(this);

    setRenderHint(QPainter::Antialiasing);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setSceneRect(QRectF(-25000.0, -25000.0, 50000.0, 50000.0));

    setAcceptDrops(true);
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    setAttribute(Qt::WA_Hover);
    viewport()->setAttribute(Qt::WA_Hover);

    locationPing_ = new QVariantAnimation(this);
    locationPing_->setObjectName(QStringLiteral("blueprintLocationPing"));
    locationPing_->setStartValue(0.0);
    locationPing_->setEndValue(1.0);
    locationPing_->setDuration(LocationPingAnimation::kDurationMs);
    connect(locationPing_, &QVariantAnimation::valueChanged, this, [this] { viewport()->update(); });
    connect(locationPing_, &QVariantAnimation::finished, this, [this] {
        locationPingObjectID_ = 0;
        viewport()->update();
    });

    connectHint_ = new QLabel(viewport());
    connectHint_->setObjectName(QStringLiteral("blueprintConnectHint"));
    connectHint_->setAttribute(Qt::WA_TransparentForMouseEvents);
    connectHint_->setFocusPolicy(Qt::NoFocus);
    connectHint_->setTextFormat(Qt::PlainText);
    connectHint_->setTextInteractionFlags(Qt::NoTextInteraction);
    connectHint_->setIndent(0);
    connectHint_->setFont(FontManager::instance().getFont(FontRole::Label, 9));
    connectHint_->hide();

    loadPersistedFlowDirection_();
    loadPersistedLayerVisibility_();
    gridSnapEnabled_ = QSettings().value(QStringLiteral("Blueprint/GridSnapEnabled"), true).toBool();
    nodeContentFields_ = NodeContentFields::fromInt(QSettings().value(
        QStringLiteral("Blueprint/NodeContentFields"), kAllNodeContentFields.toInt()).toInt()) & kAllNodeContentFields;

    activeSource_ = pathSource_.get();
    connect(&AudioEngine::instance(), &AudioEngine::playbackPathsChanged,
            this, &BlueprintCanvasWidget::refreshPlaybackActivity_);
    connect(chainSource_.get(), &IBlueprintSource::sourceChanged,
            this, [this]() {
        if (activeSource_ == chainSource_.get()) materializeFromSource_();
    });
    connect(pathSource_.get(), &IBlueprintSource::sourceChanged,
            this, [this]() {
        if (activeSource_ == pathSource_.get()) materializeFromSource_();
    });

    connect(scene_, &QGraphicsScene::selectionChanged,
            this, &BlueprintCanvasWidget::onSceneSelectionChanged);

    new RenameClickHandler(this, viewport(), [this](const QPoint& point)
        -> RenameClickHandler::Target {
        if (interactionMode() != InteractionMode::Select || nodeFieldEditing_() || dragSourcePin_)
            return {};
        auto* node = findOwningNodeItem(itemAt(point));
        if (!node || !node->isSelected() || scene_->selectedItems().size() != 1
            || !node->titleRect().contains(node->mapFromScene(mapToScene(point))))
            return {};
        const auto* item = ProjectManager::instance().findItemByObjectID(node->objectID());
        if (!item) return {};
        return {QVariantList{item->guid, QVariant::fromValue(reinterpret_cast<quintptr>(node)),
                             node->pos()}, [node] { node->beginTitleEditing(); }};
    });

    auto& pm = ProjectManager::instance();
    connect(&pm, &ProjectManager::projectOpened,
            this, &BlueprintCanvasWidget::onProjectOpened);
    connect(&pm, &ProjectManager::projectClosed,
            this, &BlueprintCanvasWidget::onProjectClosed);
    connect(&pm, &ProjectManager::projectSaved,
            this, &BlueprintCanvasWidget::refreshNodeTitles_);
    connect(&MediaPool::instance(), &MediaPool::statusCacheRefreshed,
            this, &BlueprintCanvasWidget::refreshNodeTitles_, Qt::QueuedConnection);
    connect(&MediaPool::instance(), &MediaPool::mediaStatusChanged,
            this, &BlueprintCanvasWidget::refreshNodeTitles_, Qt::QueuedConnection);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &BlueprintCanvasWidget::refreshNodeTitles_);
    connect(&pm, &ProjectManager::activePropertyEditsFinishing,
            this, &BlueprintCanvasWidget::finishNodeEdits_);
    connect(this, &BlueprintCanvasView::interactionModeChanged, this, [this]() {
        cancelConnection_();
        finishNodeEdits_(true);
        refreshChainPins_();
    });
    connect(this, &BlueprintCanvasView::zoomChanged, this, &BlueprintCanvasWidget::updateNodeGridSnap_);
    connect(&pm, &ProjectManager::itemCreated,
            this, &BlueprintCanvasWidget::onItemCreated);
    connect(&pm, &ProjectManager::itemDeleted,
            this, &BlueprintCanvasWidget::onItemDeleted);
    connect(&pm, &ProjectManager::itemMoved,
            this, &BlueprintCanvasWidget::onItemMoved);
    connect(&pm, &ProjectManager::itemRenamed,
            this, &BlueprintCanvasWidget::onItemRenamed);
    connect(&pm, &ProjectManager::itemPropertyChanged,
            this, &BlueprintCanvasWidget::onItemPropertyChanged);
    connect(&pm, &ProjectManager::itemPropertyPreviewed,
            this, &BlueprintCanvasWidget::refreshNodeContent_);
    connect(&pm, &ProjectManager::fieldGroupsPreviewed,
            this, &BlueprintCanvasWidget::refreshOutputVolumePreview_);
    connect(&pm, &ProjectManager::busChanged,
            this, &BlueprintCanvasWidget::onBusChanged);
    const auto refreshSource = [this]() {
        if (activeSource_ == pathSource_.get()) schedulePathRefresh_();
        else scheduleChainRefresh_();
    };
    connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged,
            this, [this, refreshSource]() {
        cancelConnection_();
        refreshSource();
    });
    connect(&LanguageContextService::instance(), &LanguageContextService::languageChanged,
            this, [this, refreshSource]() {
        refreshNodeTitles_();
        refreshSource();
    });
    connect(&AssetRegistry::instance(), &AssetRegistry::assetModified,
            this, refreshSource);
    connect(&AssetRegistry::instance(), &AssetRegistry::assetModified,
            this, &BlueprintCanvasWidget::refreshNodeWaveforms_);
    connect(&MediaPool::instance(), &MediaPool::waveformReady,
            this, &BlueprintCanvasWidget::refreshNodeWaveforms_);
    connect(&pm, &ProjectManager::projectPlatformEdited,
            this, &BlueprintCanvasWidget::schedulePathRefresh_);
    connect(&pm.variableRehearsal(), &VariableRehearsal::rehearsalChanged,
            this, [this](const QUuid&) { refreshTransitionWatch_(); });
    connect(&pm.transitionRehearsal(), &TransitionRehearsal::rehearsalChanged,
            this, [this](const QUuid& containerId) {
        auto* source = activeSignalChainSource();
        if (source && source->currentItem()
            && source->currentItem()->guid == containerId) {
            refreshTransitionWatch_();
        }
    });

    if (ProjectManager::instance().currentProject() != nullptr) {
        onProjectOpened(ProjectManager::instance().currentProject());
    }
}

BlueprintCanvasWidget::~BlueprintCanvasWidget()
{
    disconnect(scene_, nullptr, this, nullptr);
    clearScene();
}

void BlueprintCanvasWidget::rebuildFromProject()
{
    projectView_ = true;
    activeSource_ = pathSource_.get();
    setTransitionWatch({});
    refreshProjectPaths_();
}

void BlueprintCanvasWidget::clearScene()
{
    pathSceneState_.reset();
    locationPing_->stop();
    locationPingObjectID_ = 0;
    chainRefreshPending_ = false;
    finishNodeEdits_(false);
    QToolTip::hideText();
    connectHint_->hide();
    connectHint_->clear();
    connectHintText_.clear();
    if (tempPath_ != nullptr) {
        scene_->removeItem(tempPath_);
        delete tempPath_;
        tempPath_ = nullptr;
    }
    dragSourcePin_   = nullptr;
    viewport()->unsetCursor();
    connectHoverPin_ = nullptr;
    connectHoverNode_ = nullptr;
    nodeDragOrigin_ = nullptr;
    nodeDragSources_.clear();
    hoverDropTarget_ = nullptr;
    externalDragActive_ = false;
    externalAudioFiles_.clear();

    // Empty the non-owning pointer caches before destroying the items they track,
    // and block the scene's signals during teardown. scene_->clear() deletes items
    // one by one, and removeItem emits selectionChanged synchronously when a selected
    // node is removed — that would re-enter onSceneSelectionChanged mid-teardown.
    nodeMap_.clear();
    waveformNodes_.clear();
    inputVisibility_.clear();
    edges_.clear();
    gainNodes_.clear();
    gainOutputEdges_.clear();
    pathPositionKeys_.clear();
    pathDragStartPositions_.clear();
    gainPositionKeys_.clear();
    gainDragStartPositions_.clear();
    if (scene_ != nullptr) {
        const QSignalBlocker blocker(scene_);
        scene_->clear();
    }
}

void BlueprintCanvasWidget::materializeFromSource_()
{
    if (!activeSource_) return;
    if (activeSource_ == chainSource_.get() && chainSource_) {
        // The cached chain view is rebuilt here so routing/property edits made
        // outside the canvas (Inspector, undo/redo, IPC) are reflected, not
        // just edits the canvas initiated itself.
        chainSource_->refreshView();
    }
    const bool reusePathScene = activeSource_ == pathSource_.get() && pathSceneState_
        && pathSceneState_->projectId == pathProjectId_ && pathSource_->graph().ok();
    QHash<QUuid, NodeItem*> previousNodes;
    QHash<QByteArray, size_t> previousEdgeIndices;
    std::vector<ConnectionPath*> previousEdges;
    std::vector<NodeItem*> previousGains;
    std::vector<ConnectionPath*> previousGainOutputs;
    QSignalBlocker sceneBlocker(scene_);
    if (reusePathScene) {
        if (locationPing_) locationPing_->stop();
        locationPingObjectID_ = 0;
        const auto& graph = pathSceneState_->graph;
        for (size_t index = 0; index < graph.nodes.size(); ++index) {
            const auto& node = graph.nodes[index];
            const auto kind = node.type == ProjectItemType::Bus ? NodeKind::Bus : NodeKind::ProjectItem;
            previousNodes.insert(node.key.objectGuid, nodeMap_.at({kind, node.objectID, index}));
        }
        for (size_t index = 0; index < graph.edges.size(); ++index)
            previousEdgeIndices.insert(pathEdgeIdentity(graph, index), index);
        previousEdges = std::move(edges_);
        previousGains = std::move(gainNodes_);
        previousGainOutputs = std::move(gainOutputEdges_);
        nodeMap_.clear();
        waveformNodes_.clear();
        pathPositionKeys_.clear();
        gainPositionKeys_.clear();
    } else {
        clearScene();
    }

    auto nodes = activeSource_->buildNodes();
    auto* project = ProjectManager::instance().currentProject();
    QHash<uint32_t, ProjectItem*> objects;
    if (project) {
        const auto collect = [&](auto&& self, ProjectItem* item) -> void {
            if (!item) return;
            if (!objects.contains(item->objectID)) objects.insert(item->objectID, item);
            for (const auto& child : item->children) self(self, child.get());
        };
        for (const auto* binding : project->allFileBindings()) {
            if (binding) collect(collect, binding->rootItem.get());
        }
    }
    NodeContentFields shownFields;
    for (const auto& desc : nodes) shownFields |= desc.content.fields() & nodeContentFields_;
    const int labelWidth = NodeContentEditor::labelWidth(shownFields);
    const bool labelWidthChanged = nodeContentLabelWidth_ != labelWidth;
    nodeContentLabelWidth_ = labelWidth;
    int globalIndex = 0;
    for (auto const& d : nodes) {
        NodeItem* item = nullptr;
        if (reusePathScene && d.occurrenceIndex) {
            const auto guid = pathSource_->graph().nodes[*d.occurrenceIndex].key.objectGuid;
            auto* previous = previousNodes.value(guid);
            if (previous && previous->objectID() == d.id && previous->itemType() == d.itemType
                && previous->inputCount() == d.portPairCount && previous->outputCount() == d.portPairCount
                && previous->contentInfo().fields() == d.content.fields())
                item = previousNodes.take(guid);
        }
        const bool created = !item;
        if (created) item = new NodeItem(d.title, nullptr);
        else item->setTitle(d.title);
        item->setObjectID(d.id);
        item->setRole(translateRole(d.role));
        item->setCanvasFlowDirection(flowDir_);
        item->setContentFields(nodeContentFields_);

        item->setSubtitle(d.subtitle);

        item->setItemType(d.itemType);
        for (int pair = 0; created && pair < d.portPairCount; ++pair) {
            item->addInputPin();
            item->addOutputPin();
            item->inputPin(pair)->setDomain(d.occurrenceIndex
                ? NodePin::Domain::Audio : inputDomainForDesc(d));
            item->outputPin(pair)->setDomain(NodePin::Domain::Audio);
        }

        if (item->contentInfo() != d.content) item->setContentInfo(d.content);
        if (created && d.occurrenceIndex && d.content.fields())
            installNodeEditor_(item, d);
        else if (d.occurrenceIndex) {
            if (labelWidthChanged) {
                if (auto* editor = qobject_cast<NodeContentEditor*>(item->contentWidget()))
                    editor->setLabelWidth(labelWidth);
                item->refreshContentPresentation();
            }
            if (auto* editor = qobject_cast<NodeContentEditor*>(item->contentWidget()))
                tagContentEditor_(editor, ProjectManager::instance().findItemByGuid(
                    pathSource_->graph().nodes[*d.occurrenceIndex].key.objectGuid));
        }

        // A user-dragged card persists its scene position in canvasPos (an
        // editor-only project field, never read by the runtime); honour it as an
        // override, otherwise fall back to the auto-arranged column position.
        const bool isRealNode = (d.id != 0);
        ProjectItem* owner = nullptr;
        if (isRealNode && !d.occurrenceIndex) {
            owner = (d.kind == BlueprintNodeKind::ProjectItem)
                        ? objects.value(d.id)
                        : ProjectManager::instance().findBusItem(d.id);
        }

        QPointF pos = d.hasPosition ? d.position : defaultLayoutPos(globalIndex);
        if (owner != nullptr &&
            (owner->canvasPos.x() != 0.0 || owner->canvasPos.y() != 0.0)) {
            pos = owner->canvasPos;
        }
        if (created) {
            scene_->addItem(item);
            item->setPos(pos);
        }
        item->setOpacity(d.opacity);

        if (isRealNode) {
            item->setFlag(QGraphicsItem::ItemIsMovable, !d.occurrenceIndex || d.draggable);
            item->setFlag(QGraphicsItem::ItemIsSelectable, true);
            item->setBlueprintGridSnap(!d.occurrenceIndex && gridSnapEnabled_, gridSpacing());
            const auto* definition = d.kind == BlueprintNodeKind::ProjectItem
                ? objects.value(d.id) : ProjectManager::instance().findBusItem(d.id);
            if (definition) {
                item->setTitleRenameHandler(InlineRenameEdit::projectItemHandler(definition->guid));
                item->setTitleEditFinishedCallback(
                    [this, item, guid = definition->guid](const QString&, bool) {
                        auto& manager = ProjectManager::instance();
                        if (auto* current = manager.findItemByGuid(guid)) {
                            item->setTitle(current->name);
                            item->setTitleDirty(current->isDirtyObject());
                        }
                        if (pathRefreshPending_) {
                            queuePathRefresh_(pathContentOnlyRefresh_);
                        }
                        if (chainRefreshPending_) scheduleChainRefresh_();
                    });
            }
            const uint32_t nid = d.id;
            if (!d.occurrenceIndex) item->setDragCompleteCallback(
                [nid](const QPointF& oldPos, const QPointF& newPos) {
                    if (oldPos == newPos) return;
                    UndoRedoManager::instance().push(
                        new BlueprintNodePositionCommand(nid, oldPos, newPos));
                });
        } else {
            // Nodes without a backing object stay inert.
            item->setFlag(QGraphicsItem::ItemIsMovable, false);
            item->setFlag(QGraphicsItem::ItemIsSelectable, false);
        }

        item->setBlueprintWaveformMode(d.waveformMode);
        if (d.waveformMode)
            waveformNodes_[d.mediaPath].push_back({item, d.waveformRange});
        else if (!created)
            item->setWaveformPeakData({});

        if (d.overrideBadge >= 0) {
            item->setOverrideBadge(d.overrideBadge != 0);
        }
        if (d.receiveBadge >= 0) {
            item->setReceiveBadge(d.receiveBadge);
        }

        const NodeKind nk = (d.kind == BlueprintNodeKind::ProjectItem)
                                ? NodeKind::ProjectItem : NodeKind::Bus;
        nodeMap_[NodeKey{nk, d.id, d.occurrenceIndex}] = item;
        ++globalIndex;
    }

    auto edges = activeSource_->buildEdges();
    if (activeSource_ == pathSource_.get()) {
        gainNodes_.resize(edges.size());
        gainOutputEdges_.resize(edges.size());
        gainPositionKeys_.resize(edges.size());
        for (size_t index = 0; index < nodes.size(); ++index) {
            pathPositionKeys_.push_back(pathPositionKey(pathProjectId_, flowDir_,
                pathSource_->graph().nodes[index].key));
        }
        for (size_t index = 0; index < edges.size(); ++index) {
            if (pathSource_->graph().edges[index].kind == SignalPathEdgeKind::Through) continue;
            NodeItem* gain = nullptr;
            const auto previous = previousEdgeIndices.constFind(pathEdgeIdentity(pathSource_->graph(), index));
            if (previous != previousEdgeIndices.cend()) {
                auto*& candidate = previousGains[*previous];
                if (candidate && candidate->contentInfo().fields() == edges[index].gainContent.fields())
                    gain = std::exchange(candidate, nullptr);
            }
            const bool created = !gain;
            if (created) {
                gain = new NodeItem(QString());
                gain->setRouteGainMode(true);
                gain->addInputPin();
                gain->addOutputPin();
                gain->setFlag(QGraphicsItem::ItemIsSelectable);
                gain->setFlag(QGraphicsItem::ItemIsMovable);
                scene_->addItem(gain);
            }
            gain->setCanvasFlowDirection(flowDir_);
            gain->setContentFields(edges[index].gainContent.fields());
            if (gain->contentInfo() != edges[index].gainContent)
                gain->setContentInfo(edges[index].gainContent);
            gainNodes_[index] = gain;
            if (created) installGainEditor_(gain);
            else if (auto* editor = qobject_cast<NodeContentEditor*>(gain->contentWidget()))
                tagContentEditor_(editor, routeGainTopicItem_(index));
            const auto key = signalPathEdgeKey_(index);
            QByteArray identity;
            QDataStream stream(&identity, QIODevice::WriteOnly);
            stream.setVersion(QDataStream::Qt_6_8);
            stream << pathProjectId_ << QString::fromLatin1(flowDirectionToString(flowDir_))
                << key.from << key.to << static_cast<int>(key.kind)
                << key.routingSource.value_or(QUuid());
            gainPositionKeys_[index] = QStringLiteral("Blueprint/RouteGainLayout/Gains/")
                + QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
        }
        layoutPathNodes_();
    }

    for (size_t edgeIndex = 0; edgeIndex < edges.size(); ++edgeIndex) {
        const auto& e = edges[edgeIndex];
        const NodeKind fk = (e.fromKind == BlueprintNodeKind::ProjectItem)
                                ? NodeKind::ProjectItem : NodeKind::Bus;
        const NodeKind tk = (e.toKind == BlueprintNodeKind::ProjectItem)
                                ? NodeKind::ProjectItem : NodeKind::Bus;
        auto fromIt = nodeMap_.find(NodeKey{fk, e.fromId, e.fromOccurrenceIndex});
        auto toIt   = nodeMap_.find(NodeKey{tk, e.toId, e.toOccurrenceIndex});
        if (fromIt == nodeMap_.end() || toIt == nodeMap_.end()) continue;
        NodeItem* fromNode = fromIt->second;
        NodeItem* toNode   = toIt->second;
        if (!fromNode || !toNode) continue;

        NodePin* fromPin = fromNode->outputPin(e.fromPortPairIndex);
        NodePin* toPin   = toNode->inputPin(e.toPortPairIndex);
        if (!fromPin || !toPin) continue;

        auto* gain = signalPathGainNode(edgeIndex);
        ConnectionPath* path = nullptr;
        ConnectionPath* downstream = nullptr;
        if (reusePathScene) {
            const auto previous = previousEdgeIndices.constFind(pathEdgeIdentity(pathSource_->graph(), edgeIndex));
            if (previous != previousEdgeIndices.cend()) {
                auto*& candidate = previousEdges[*previous];
                if (candidate && candidate->startPin() == fromPin
                    && candidate->endPin() == (gain ? gain->inputPin() : toPin))
                    path = std::exchange(candidate, nullptr);
                auto*& output = previousGainOutputs[*previous];
                if (gain && output && output->startPin() == gain->outputPin() && output->endPin() == toPin)
                    downstream = std::exchange(output, nullptr);
            }
        }
        if (!path) {
            path = new ConnectionPath(fromPin, gain ? gain->inputPin() : toPin);
            scene_->addItem(path);
        }
        path->setBlueprintEdgeKind(static_cast<BlueprintEdgeKind>(e.edgeKind));
        path->setBlueprintFlowDirection(flowDir_);
        path->setParallelOffset(0.0);

        path->setLabel(e.label);

        edges_.push_back(path);
        if (gain) {
            if (!downstream) {
                downstream = new ConnectionPath(gain->outputPin(), toPin);
                scene_->addItem(downstream);
            }
            downstream->setBlueprintEdgeKind(static_cast<BlueprintEdgeKind>(e.edgeKind));
            downstream->setBlueprintFlowDirection(flowDir_);
            gainOutputEdges_[edgeIndex] = downstream;
        }
    }
    for (auto* edge : previousEdges) delete edge;
    for (auto* edge : previousGainOutputs) delete edge;
    const auto removeNode = [](NodeItem* node) {
        if (!node) return;
        node->finishTitleEditing(false);
        if (auto* editor = qobject_cast<NodeContentEditor*>(node->contentWidget()))
            editor->finishEditing(false);
        delete node;
    };
    for (auto* gain : previousGains) removeNode(gain);
    for (auto* node : previousNodes) removeNode(node);
    if (activeSource_ == pathSource_.get()) {
        std::map<std::pair<size_t, size_t>, std::vector<size_t>> parallel;
        const auto& graph = pathSource_->graph();
        for (size_t index = 0; index < graph.edges.size(); ++index) {
            const auto& edge = graph.edges[index];
            if (!gainNodes_[index])
                parallel[{edge.from.occurrenceIndex, edge.to.occurrenceIndex}].push_back(index);
        }
        for (const auto& [endpoints, group] : parallel) {
            const bool vertical = flowDir_ == CanvasFlowDirection::TB || flowDir_ == CanvasFlowDirection::BT;
            const QFontMetrics metrics(FontManager::instance().getFont(FontRole::Label, 8));
            qreal spacing = 12.0;
            for (const auto index : group) {
                if (!edges[index].label.isEmpty())
                    spacing = std::max(spacing, static_cast<qreal>(vertical
                        ? metrics.horizontalAdvance(edges[index].label) + 16 : metrics.height() + 8));
            }
            for (size_t lane = 0; lane < group.size(); ++lane)
                edges_[group[lane]]->setParallelOffset(
                    (static_cast<qreal>(lane) - (static_cast<qreal>(group.size()) - 1.0) / 2.0) * spacing);
        }
    }
    applyTreeVisibility_(false);
    if (selectedObjectGuids_) {
        applyProjectItemSelection_();
    } else if (activeSource_ == pathSource_.get()
        && (!selectedPathKeys_.empty() || !selectedPathEdgeKeys_.empty())) {
        std::vector<SignalPathNodeKey> restored;
        std::vector<PathEdgeKey> restoredEdges;
        {
            const QSignalBlocker blocker(scene_);
            const auto& graph = pathSource_->graph();
            for (size_t index = 0; index < graph.nodes.size(); ++index) {
                const auto& key = graph.nodes[index].key;
                if (signalPathNode(index)->isVisible()
                    && std::find(selectedPathKeys_.begin(), selectedPathKeys_.end(), key)
                    != selectedPathKeys_.end()) {
                    signalPathNode(index)->setSelected(true);
                    restored.push_back(key);
                }
            }
            for (size_t index = 0; index < graph.edges.size(); ++index) {
                const auto key = signalPathEdgeKey_(index);
                if (edges_[index]->isVisible()
                    && std::find(selectedPathEdgeKeys_.begin(), selectedPathEdgeKeys_.end(), key)
                    != selectedPathEdgeKeys_.end()) {
                    setSignalPathEdgeSelected_(index, true);
                    restoredEdges.push_back(key);
                }
            }
        }
        selectedPathKeys_ = std::move(restored);
        selectedPathEdgeKeys_ = std::move(restoredEdges);
    }
    refreshNodeTitles_();
    refreshNodeWaveforms_();
    refreshPlaybackActivity_();
    refreshPathFocus_();
    if (activeSource_ == pathSource_.get() && pathSource_->graph().ok()) {
        pathSceneState_ = PathSceneState{pathProjectId_, pathSource_->graph()};
        QGraphicsProxyWidget* previous = nullptr;
        const auto linkFocus = [&previous](NodeItem* node) {
            auto* proxy = node ? node->contentProxy() : nullptr;
            if (!proxy) return;
            if (previous) QGraphicsWidget::setTabOrder(previous, proxy);
            previous = proxy;
        };
        for (size_t index = 0; index < pathSource_->graph().nodes.size(); ++index)
            linkFocus(signalPathNode(index));
        for (auto* node : gainNodes_) linkFocus(node);
    }
    sceneBlocker.unblock();
    emit signalChainViewChanged();
}

ConnectionPath* BlueprintCanvasWidget::signalPathEdge(size_t edgeIndex) const
{
    return activeSignalPathSource() && edgeIndex < edges_.size() ? edges_[edgeIndex] : nullptr;
}

NodeItem* BlueprintCanvasWidget::signalPathGainNode(size_t edgeIndex) const
{
    return activeSignalPathSource() && edgeIndex < gainNodes_.size() ? gainNodes_[edgeIndex] : nullptr;
}

ConnectionPath* BlueprintCanvasWidget::signalPathGainOutputEdge(size_t edgeIndex) const
{
    return activeSignalPathSource() && edgeIndex < gainOutputEdges_.size() ? gainOutputEdges_[edgeIndex] : nullptr;
}

void BlueprintCanvasWidget::refreshPlaybackActivity_()
{
    const auto* source = activeSignalPathSource();
    const auto* project = ProjectManager::instance().currentProject();
    if (!source || !project || !source->graph().ok()) return;
    const auto activity = projectSignalPathActivity(*project, source->graph(), AudioEngine::instance().playbackPaths());
    for (size_t index = 0; index < activity.portPairs.size(); ++index) {
        auto* node = signalPathNode(index);
        for (size_t pair = 0; pair < activity.portPairs[index].size(); ++pair) {
            node->inputPin(static_cast<int>(pair))->setPlaybackActive(activity.portPairs[index][pair]);
            node->outputPin(static_cast<int>(pair))->setPlaybackActive(activity.portPairs[index][pair]);
        }
        node->setPlaybackActive(std::any_of(activity.portPairs[index].begin(), activity.portPairs[index].end(),
            [](bool active) { return active; }));
    }
    for (size_t index = 0; index < activity.edges.size(); ++index) {
        edges_[index]->setPlaybackActive(activity.edges[index]);
        if (auto* gain = signalPathGainNode(index)) {
            gain->setPlaybackActive(activity.edges[index]);
            gain->inputPin()->setPlaybackActive(activity.edges[index]);
            gain->outputPin()->setPlaybackActive(activity.edges[index]);
            gainOutputEdges_[index]->setPlaybackActive(activity.edges[index]);
        }
    }
}

void BlueprintCanvasWidget::refreshPathFocus_()
{
    const auto* source = activeSignalPathSource();
    if (!source) return;
    std::vector<size_t> selected;
    for (size_t index = 0; index < source->graph().nodes.size(); ++index)
        if (signalPathNode(index)->isSelected()) selected.push_back(index);
    const auto focus = projectSignalPathFocus(source->graph(), selected, selectedSignalPathEdges_(false));
    for (size_t index = 0; index < focus.nodes.size(); ++index)
        signalPathNode(index)->setFocusDimmed(focus.active && !focus.nodes[index]);
    for (size_t index = 0; index < focus.edges.size(); ++index) {
        edges_[index]->setFocusDimmed(focus.active && !focus.edges[index]);
        if (auto* gain = signalPathGainNode(index)) {
            gain->setFocusDimmed(focus.active && !focus.edges[index]);
            gainOutputEdges_[index]->setFocusDimmed(focus.active && !focus.edges[index]);
        }
    }
}

QPointF BlueprintCanvasWidget::defaultLayoutPos(int index)
{
    const int cols = 8;
    const int r = index / cols;
    const int c = index % cols;
    return QPointF(-800.0 + c * 280.0, -500.0 + r * 160.0);
}

void BlueprintCanvasWidget::onProjectOpened(ProjectData* )
{
    collapseReveals_.clear();
    if (!hiddenHierarchyRoots_.empty()) {
        hiddenHierarchyRoots_.clear();
        emit hierarchyCollapseChanged();
    }
    if (!hiddenBuses_.empty()) {
        hiddenBuses_.clear();
        emit busCollapseChanged();
    }
    clear();
    rebuildFromProject();
    auto* master = ProjectManager::instance().getMasterBusItem();
    if (!master) return;
    const auto& nodes = pathSource_->graph().nodes;
    for (size_t index = 0; index < nodes.size(); ++index) {
        if (nodes[index].key.objectGuid == master->guid) {
            setZoomFactor(1.0);
            centerOn(signalPathNode(index));
            break;
        }
    }
}

void BlueprintCanvasWidget::onProjectClosed()
{
    collapseReveals_.clear();
    if (!hiddenHierarchyRoots_.empty()) {
        hiddenHierarchyRoots_.clear();
        emit hierarchyCollapseChanged();
    }
    if (!hiddenBuses_.empty()) {
        hiddenBuses_.clear();
        emit busCollapseChanged();
    }
    clear();
}

void BlueprintCanvasWidget::onItemCreated(ProjectItem* )
{
    if (activeSource_ == pathSource_.get()) { schedulePathRefresh_(); return; }
    scheduleChainRefresh_();
}

void BlueprintCanvasWidget::onItemDeleted(ProjectItem* )
{
    for (const auto& [key, node] : nodeMap_) {
        if (node->isTitleEditing() && !ProjectManager::instance().findItemByObjectID(key.id))
            node->finishTitleEditing(false);
    }
    if (activeSource_ == pathSource_.get()) { schedulePathRefresh_(); return; }
    scheduleChainRefresh_();
}

void BlueprintCanvasWidget::onItemMoved(ProjectItem* ,
                                        ProjectItem* ,
                                        ProjectItem* )
{
    if (activeSource_ == pathSource_.get()) { schedulePathRefresh_(); return; }
    scheduleChainRefresh_();
}

void BlueprintCanvasWidget::onItemRenamed(ProjectItem* ,
                                          const QString& )
{
    refreshNodeTitles_();
    if (activeSource_ == pathSource_.get()) { schedulePathRefresh_(); return; }
    scheduleChainRefresh_();
}

void BlueprintCanvasWidget::onItemPropertyChanged(ProjectItem* ,
                                                  const QString& propertyName)
{
    refreshNodeTitles_();
    if (activeSource_ == pathSource_.get()) {
        if (propertyName == QLatin1String("dirty")) return;
        queuePathRefresh_(propertyName == QLatin1String("volume")
            || propertyName == QLatin1String("pitch")
            || propertyName == QLatin1String("loopCount")
            || propertyName == QLatin1String("scheduleOffsetSec"));
        return;
    }
    static const QSet<QString> structuralProps = {
        QStringLiteral("outputBus"), QStringLiteral("output_bus"),
        QStringLiteral("outputBusId"),
        QStringLiteral("parent"),
        QStringLiteral("send"),
        QStringLiteral("sends"),
        QStringLiteral("sendLevel"),
        QStringLiteral("spatialAttenuationRef"),
        QStringLiteral("attenuationRef"),
        QStringLiteral("clips"),
        QStringLiteral("canvasPos"),
        QStringLiteral("pasteProperties"),
        QStringLiteral("mode"),
        QStringLiteral("playType"),
        QStringLiteral("selections"),
        QStringLiteral("transitions"),
    };
    if (!structuralProps.contains(propertyName)) return;
    scheduleChainRefresh_();
}

void BlueprintCanvasWidget::onBusChanged(uint32_t ,
                                         const QString& propertyName)
{
    refreshNodeTitles_();
    if (activeSource_ == pathSource_.get()) { schedulePathRefresh_(); return; }
    static const QSet<QString> structuralProps = {
        QStringLiteral("send"),
        QStringLiteral("sends"),
    };
    if (!structuralProps.contains(propertyName)) return;
    scheduleChainRefresh_();
}

BlueprintCanvasWidget::ConnectResult
BlueprintCanvasWidget::connectNodes(uint32_t fromId, uint32_t toId, QString edgeType,
                                    QString fromKind, QString toKind)
{
    ConnectResult r;
    if (!activeSource_) { r.error = QStringLiteral("no_source"); return r; }
    if (!activeSource_->supportsEditing()) { r.error = QStringLiteral("unsupported"); return r; }
    r.success = activeSource_->onConnect(fromId, toId, edgeType, fromKind, toKind);
    if (activeSource_ == chainSource_.get()) r = chainSource_->lastConnectResult();
    if (r.success) {
        materializeFromSource_();
    } else {
        if (r.error.isEmpty()) r.error = QStringLiteral("unsupported");
    }
    return r;
}

BlueprintCanvasWidget::ConnectResult BlueprintCanvasWidget::connectSignalPathNodes(
    SignalPathEndpoint from, SignalPathEndpoint to, bool addSend)
{
    ConnectResult result;
    if (activeSource_ != pathSource_.get()) {
        result.error = tr("The signal path is unavailable");
        return result;
    }
    auto plan = prepareSignalPathConnect(pathSource_->graph(), from, to, addSend);
    result.error = plan.error;
    if (plan.ok()) {
        result.success = applyConnectPlan_(std::move(plan));
        if (result.success) schedulePathRefresh_();
        else {
            result.error = tr("The objects changed and could not be connected");
            result.operationFailed = true;
        }
    }
    return result;
}

bool BlueprintCanvasWidget::applyConnectPlan_(SignalPathConnectPlan plan)
{
    const auto targetGuid = plan.to;
    if (!applySignalPathConnect(std::move(plan))) return false;
    if (!hiddenHierarchyRoots_.empty() || !hiddenBuses_.empty()) refreshProjectPaths_();
    const auto* target = ProjectManager::instance().findItemByGuid(targetGuid);
    if (target && supportsHierarchyCollapse(target->type)
        && hierarchyVisibility(target->objectID) != Qt::Checked)
        setHierarchyVisible(target->objectID, true);
    else if (target && target->type == ProjectItemType::Bus
        && busVisibility(target->objectID) != Qt::Checked)
        setBusVisible(target->objectID, true);
    return true;
}

bool BlueprintCanvasWidget::disconnectNodes(uint32_t fromId, uint32_t toId, QString edgeType,
                                            QString fromKind, QString toKind)
{
    if (!activeSource_ || !activeSource_->supportsEditing()) return false;
    const bool ok = activeSource_->onDisconnect(fromId, toId, edgeType, fromKind, toKind);
    if (ok) materializeFromSource_();
    return ok;
}

BlueprintCanvasWidget::ConnectResult BlueprintCanvasWidget::disconnectSignalPathEdges(
    const std::vector<size_t>& edgeIndices)
{
    ConnectResult result;
    if (activeSource_ != pathSource_.get()) {
        result.error = tr("Signal path view is unavailable");
        return result;
    }
    const auto& graph = pathSource_->graph();
    if (std::any_of(edgeIndices.begin(), edgeIndices.end(),
            [this](size_t index) { return !signalPathEdgeDeletable_(index); })) {
        result.error = tr("Master Output connections cannot be deleted");
        return result;
    }
    auto plan = prepareSignalPathDisconnect(graph, edgeIndices);
    if (!plan.ok()) {
        result.error = plan.error;
        return result;
    }
    result.success = applySignalPathConnect(std::move(plan));
    if (result.success) schedulePathRefresh_();
    else {
        result.error = tr("The connections could not be deleted");
        result.operationFailed = true;
    }
    return result;
}

bool BlueprintCanvasWidget::setNodeOverride(uint32_t itemId, bool override)
{
    if (!activeSource_ || !activeSource_->supportsEditing()) return false;
    auto* project = ProjectManager::instance().currentProject();
    if (!project) return false;
    auto* item = canvasFindItem(project, itemId);
    if (!item) return false;
    if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type))
        return false;

    uint32_t autoRoute = 0;
    const auto* mc = item->comp<VMixNode>();
    if (override && (mc ? mc->outputBusId : 0) == 0) {
        autoRoute = VNX::kMasterBusID;
    }

    if (autoRoute != 0
        && !ProjectManager::instance().outputBusSelectable(itemId, autoRoute)) {
        return false;
    }

    auto& undoMgr = UndoRedoManager::instance();
    undoMgr.beginMacro(QStringLiteral("Blueprint Override Toggle"));
    undoMgr.push(new BlueprintOverrideToggleCommand(itemId, override, autoRoute));
    undoMgr.endMacro();

    materializeFromSource_();
    return true;
}

void BlueprintCanvasWidget::dragEnterEvent(QDragEnterEvent* event)
{
    clearExternalDrop_();
    if (!event) return;
    event->ignore();
    if (!activeSource_ || !activeSource_->supportsEditing() || !event->mimeData()) return;
    if (ProjectItemMime::hasObjectIDs(event->mimeData())) {
        if (activeSource_ != pathSource_.get()
            || ProjectItemMime::readObjectIDs(event->mimeData()).isEmpty()
            || !event->possibleActions().testFlag(Qt::MoveAction)) return;
        event->setDropAction(Qt::MoveAction);
    } else {
        externalAudioFiles_ = AudioImportGate::extractAudioFiles(event->mimeData());
        if (externalAudioFiles_.isEmpty()
            || !event->possibleActions().testFlag(Qt::CopyAction)) return;
        event->setDropAction(Qt::CopyAction);
    }
    finishNodeEdits_(true);
    externalDragActive_ = true;
    event->accept();
}

void BlueprintCanvasWidget::dragMoveEvent(QDragMoveEvent* event)
{
    if (!event) return;
    event->ignore();
    if (!externalDragActive_ || !activeSource_ || !activeSource_->supportsEditing()
        || !event->mimeData()) return;

    NodePin* pin = nullptr;
    auto* target = connectTargetAt_(event->position().toPoint(), pin);
    const bool objects = ProjectItemMime::hasObjectIDs(event->mimeData());
    bool valid = false;
    if (objects && !pin) {
        const auto plan = prepareObjectDrop_(event->mimeData(), target);
        valid = plan.ok();
    } else if (!objects && !pin && !externalAudioFiles_.isEmpty()) {
        valid = canImportAudioAt_(target);
    }
    auto* feedbackTarget = pin || (target && target->routeGainMode()) ? nullptr : target;
    if (feedbackTarget != hoverDropTarget_) {
        if (hoverDropTarget_) hoverDropTarget_->setDropFeedback(NodeItem::DropFeedback::None);
        hoverDropTarget_ = feedbackTarget;
    }
    if (feedbackTarget) feedbackTarget->setDropFeedback(valid
        ? NodeItem::DropFeedback::Valid : NodeItem::DropFeedback::Invalid);
    if (valid) {
        event->setDropAction(objects ? Qt::MoveAction : Qt::CopyAction);
        event->accept();
    }
}

void BlueprintCanvasWidget::dragLeaveEvent(QDragLeaveEvent* event)
{
    clearExternalDrop_();
    if (event) event->accept();
}

void BlueprintCanvasWidget::dropEvent(QDropEvent* event)
{
    if (!event) { clearExternalDrop_(); return; }
    event->ignore();
    if (!externalDragActive_ || !activeSource_ || !activeSource_->supportsEditing()
        || !event->mimeData()) {
        clearExternalDrop_();
        return;
    }

    NodePin* pin = nullptr;
    auto* target = connectTargetAt_(event->position().toPoint(), pin);
    if (pin || (target && target->routeGainMode())) {
        clearExternalDrop_();
        return;
    }
    if (ProjectItemMime::hasObjectIDs(event->mimeData())) {
        auto plan = prepareObjectDrop_(event->mimeData(), target);
        clearExternalDrop_();
        if (!plan.ok()) return;
        if (applyConnectPlan_(std::move(plan))) {
            event->setDropAction(Qt::MoveAction);
            event->accept();
            schedulePathRefresh_();
        } else {
            LogPanel::logError(LogCategory::GUI, tr("The objects changed and could not be moved"));
        }
        return;
    }

    const auto files = externalAudioFiles_;
    const bool valid = !files.isEmpty() && canImportAudioAt_(target);
    const uint32_t targetContainerID = target ? target->objectID() : 0;
    clearExternalDrop_();
    if (!valid) return;

    const QPointF scenePos = mapToScene(event->position().toPoint());
    const QPointF snapped = gridSnapEnabled_ ? Blueprint::snapToGrid(scenePos, gridSpacing()) : scenePos;

    QJsonArray filesJson;
    for (const QString& p : files) filesJson.append(p);
    QJsonObject args;
    args.insert(QStringLiteral("files"),  filesJson);
    args.insert(QStringLiteral("drop_x"), snapped.x());
    args.insert(QStringLiteral("drop_y"), snapped.y());
    if (targetContainerID != 0) {
        args.insert(QStringLiteral("target_container_id"),
                    static_cast<qint64>(targetContainerID));
    }
    const ActionResult result =
        ActionManager::instance().execute(QStringLiteral("import_audio_to_canvas"), args);
    if (!result.success) {
        if (result.errorCode != QStringLiteral("IMPORT_FAILED"))
            LogPanel::logError(LogCategory::Import, result.errorMessage);
        return;
    }
    const auto failed = result.data.value(QStringLiteral("fail_count")).toInt();
    if (failed > 0) LogPanel::logWarning(LogCategory::Import, tr("%1 imported; %2 failed")
        .arg(result.data.value(QStringLiteral("success_count")).toInt()).arg(failed));

    QList<ProjectItem*> created;
    for (const auto& value : result.data.value(QStringLiteral("created_authoring_handles")).toArray()) {
        if (auto* item = ProjectManager::instance().findItemByObjectID(
                static_cast<uint32_t>(value.toInteger())))
            created.push_back(item);
    }
    if (!created.isEmpty()) {
        rebuildFromProject();
        if (targetContainerID == 0) {
            PathPositions positions;
            auto position = gridSnapEnabled_ ? snapped : scenePos;
            const bool vertical = flowDir_ == CanvasFlowDirection::TB || flowDir_ == CanvasFlowDirection::BT;
            const auto& nodes = pathSource_->graph().nodes;
            for (const auto* item : created) {
                const auto found = std::find_if(nodes.begin(), nodes.end(),
                    [item](const auto& node) { return node.key.objectGuid == item->guid; });
                if (found == nodes.end()) continue;
                const auto index = static_cast<size_t>(found - nodes.begin());
                positions.emplace_back(pathPositionKeys_[index], position);
                const auto size = signalPathNode(index)->shape().boundingRect().size();
                position += vertical ? QPointF(size.width() + kGridSpacing * 2.0, 0.0)
                    : QPointF(0.0, size.height() + kGridSpacing * 2.0);
            }
            applyPathPositions_(positions);
        }
        setSelectedProjectItems(created);
        onSceneSelectionChanged();
    }

    event->setDropAction(Qt::CopyAction);
    event->accept();
}

SignalPathConnectPlan BlueprintCanvasWidget::prepareObjectDrop_(const QMimeData* mime, NodeItem* target) const
{
    SignalPathConnectPlan plan;
    if (activeSource_ != pathSource_.get() || !target) {
        plan.error = tr("Drop onto an object");
        return plan;
    }
    const auto endpoint = pathEndpoint_(target->inputPin());
    if (!endpoint) {
        plan.error = tr("Gain nodes cannot receive objects");
        return plan;
    }
    const auto ids = ProjectItemMime::readObjectIDs(mime);
    const auto& graph = pathSource_->graph();
    std::vector<SignalPathEndpoint> sources;
    for (const auto id : ids) {
        const auto found = std::find_if(graph.nodes.begin(), graph.nodes.end(),
            [id](const auto& node) { return node.objectID == id; });
        if (found == graph.nodes.end()) {
            plan.error = tr("The dragged objects are not supported in this blueprint");
            return plan;
        }
        sources.push_back({static_cast<size_t>(found - graph.nodes.begin()), 0});
    }
    return prepareSignalPathNodeDrop(graph, sources, *endpoint);
}

bool BlueprintCanvasWidget::canImportAudioAt_(NodeItem* target) const
{
    auto* project = ProjectManager::instance().currentProject();
    if (!project) return false;
    ProjectItem* context = nullptr;
    if (target) {
        if (target->routeGainMode()) return false;
        context = canvasFindItem(project, target->objectID());
        if (!context) return false;
    }
    return Blueprint::resolveAudioDropTarget(context).ok();
}

void BlueprintCanvasWidget::clearExternalDrop_()
{
    if (hoverDropTarget_) hoverDropTarget_->setDropFeedback(NodeItem::DropFeedback::None);
    hoverDropTarget_ = nullptr;
    externalDragActive_ = false;
    externalAudioFiles_.clear();
    if (pathRefreshPending_) {
        queuePathRefresh_(pathContentOnlyRefresh_);
    }
}

void BlueprintCanvasWidget::onSceneSelectionChanged()
{
    selectionClickObjectID_ = 0;
    if (activeSource_ == pathSource_.get()) {
        const QSignalBlocker blocker(scene_);
        for (size_t index = 0; index < edges_.size(); ++index) {
            if (!gainNodes_[index]) continue;
            const auto key = signalPathEdgeKey_(index);
            const bool previouslySelected = std::find(selectedPathEdgeKeys_.begin(),
                selectedPathEdgeKeys_.end(), key) != selectedPathEdgeKeys_.end();
            const bool selected = previouslySelected
                ? edges_[index]->isSelected() && gainNodes_[index]->isSelected()
                    && gainOutputEdges_[index]->isSelected()
                : edges_[index]->isSelected() || gainNodes_[index]->isSelected()
                    || gainOutputEdges_[index]->isSelected();
            setSignalPathEdgeSelected_(index, selected);
        }
    }
    selectedObjectGuids_.reset();
    selectedPathKeys_.clear();
    selectedPathEdgeKeys_.clear();
    if (activeSource_ == pathSource_.get()) {
        for (const auto& [key, node] : nodeMap_) {
            if (node->isSelected() && key.occurrenceIndex)
                selectedPathKeys_.push_back(pathSource_->graph().nodes[*key.occurrenceIndex].key);
        }
        for (size_t index = 0; index < edges_.size(); ++index)
            if (edges_[index]->isSelected()) selectedPathEdgeKeys_.push_back(signalPathEdgeKey_(index));
    }
    if (scene_ == nullptr) {
        emit selectionCleared();
        return;
    }
    refreshPathFocus_();
    const auto items = scene_->selectedItems();
    if (items.isEmpty()) {
        emit selectionCleared();
        return;
    }
    NodeItem* anchor = nullptr;
    for (auto* it : items) {
        if (auto* n = dynamic_cast<NodeItem*>(it); n && n->objectID() != 0) {
            anchor = n;
            break;
        }
    }
    if (anchor == nullptr || anchor->objectID() == 0) {
        emit selectionCleared();
        return;
    }
    emit nodeSelected(anchor->objectID());
}

ContextTopic BlueprintCanvasWidget::contentFieldTopic_(NodeContentField field,
                                                       ProjectItem* item) const
{
    const QUuid guid = item ? item->guid : QUuid();
    const auto type = item ? item->type : ProjectItem::Type::Sound;
    const QString name = item ? item->name : QString();
    switch (field) {
    case NodeContentField::Volume:
        return ContextTopic::forField(ComponentType::MixNode, QStringLiteral("volume"),
                                       QStringLiteral("Volume"), guid, type, name);
    case NodeContentField::Pitch:
        return ContextTopic::forField(ComponentType::MixNode, QStringLiteral("pitch"),
                                       QStringLiteral("Pitch"), guid, type, name);
    case NodeContentField::OffsetOfStart:
        return ContextTopic::forField(ComponentType::Sound, QStringLiteral("scheduleOffsetSec"),
                                       QStringLiteral("Offset of Start"), guid, type, name);
    case NodeContentField::Loop:
        return ContextTopic::forField(ComponentType::Audio, QStringLiteral("loopCount"),
                                       QStringLiteral("Loop"), guid, type, name);
    case NodeContentField::OutputVolume:
        return ContextTopic::forField(ComponentType::MixNode, QStringLiteral("outputBusVolume"),
                                       QStringLiteral("Output Volume"), guid, type, name);
    case NodeContentField::SendVolume:
        return ContextTopic::forField(ComponentType::MixNode, QStringLiteral("sends"),
                                       QStringLiteral("Sends"), guid, type, name);
    case NodeContentField::Waveform:
        break;
    }
    return item ? ContextTopic::forObject(guid, type, name) : ContextTopic::forPanel(QStringLiteral("Blueprint"));
}

void BlueprintCanvasWidget::tagContentEditor_(NodeContentEditor* editor, ProjectItem* item)
{
    if (!editor)
        return;
    const NodeContentField fields[] = {
        NodeContentField::Volume,
        NodeContentField::Pitch,
        NodeContentField::OffsetOfStart,
        NodeContentField::Loop,
        NodeContentField::OutputVolume,
        NodeContentField::SendVolume,
    };
    for (const NodeContentField field : fields)
        editor->setFieldTopic(field, contentFieldTopic_(field, item));
}

ProjectItem* BlueprintCanvasWidget::routeGainTopicItem_(size_t edgeIndex) const
{
    auto& manager = ProjectManager::instance();
    if (pathSource_ && edgeIndex < pathSource_->graph().edges.size()) {
        const auto& source = pathSource_->graph().edges[edgeIndex].routingSource;
        if (source)
            if (auto* item = manager.findItemByGuid(source->objectGuid))
                return item;
    }
    if (!activeSource_)
        return nullptr;
    const auto edges = activeSource_->buildEdges();
    if (edgeIndex >= edges.size())
        return nullptr;
    const bool bus = edges[edgeIndex].fromKind == BlueprintNodeKind::Bus;
    return bus ? manager.findBusItem(edges[edgeIndex].fromId)
               : manager.findItemByObjectID(edges[edgeIndex].fromId);
}

ContextTopic BlueprintCanvasWidget::contextTopicAt(const QPoint& pos) const
{
    const QPoint viewPos = viewport()->mapFrom(this, pos);
    QGraphicsItem* hit = itemAt(viewPos);
    NodeItem* node = findOwningNodeItem(hit);

    auto itemForId = [](uint32_t objectId, bool bus) -> ProjectItem* {
        auto& pm = ProjectManager::instance();
        return bus ? pm.findBusItem(objectId)
                   : canvasFindItem(pm.currentProject(), objectId);
    };

    if (node != nullptr) {
        ProjectItem* item = nullptr;
        if (node->objectID() != 0) {
            const bool bus = node->role() == NodeRole::Bus || node->role() == NodeRole::ReceiveBus;
            item = itemForId(node->objectID(), bus);
        } else if (node->routeGainMode()) {
            const auto found = std::find(gainNodes_.begin(), gainNodes_.end(), node);
            if (found != gainNodes_.end())
                item = routeGainTopicItem_(static_cast<size_t>(found - gainNodes_.begin()));
        }

        const QPointF local = node->mapFromScene(mapToScene(viewPos));
        if (const auto field = node->contentFieldAt(local))
            return contentFieldTopic_(*field, item);
        if (item)
            return ContextTopic::forObject(item->guid, item->type, item->name);
        if (node->routeGainMode())
            return ContextTopic::forField(ComponentType::MixNode, QStringLiteral("outputBusVolume"),
                                           QStringLiteral("Output Volume"), QUuid(),
                                           ProjectItem::Type::Sound, QString());
    }

    if (auto* edge = dynamic_cast<ConnectionPath*>(hit)) {
        const auto primary = std::find(edges_.begin(), edges_.end(), edge);
        if (primary != edges_.end() && activeSource_) {
            const size_t index = static_cast<size_t>(primary - edges_.begin());
            const auto described = activeSource_->buildEdges();
            if (index < described.size()) {
                const auto& describedEdge = described[index];
                ProjectItem* item = activeSource_ == pathSource_.get()
                    ? routeGainTopicItem_(index) : nullptr;
                if (!item) {
                    const bool bus = describedEdge.fromKind == BlueprintNodeKind::Bus;
                    item = itemForId(describedEdge.fromId, bus);
                }
                const auto kind = static_cast<BlueprintEdgeKind>(describedEdge.edgeKind);
                if (kind == BlueprintEdgeKind::Send)
                    return contentFieldTopic_(NodeContentField::SendVolume, item);
                if (kind == BlueprintEdgeKind::BusRoute || kind == BlueprintEdgeKind::Override)
                    return contentFieldTopic_(NodeContentField::OutputVolume, item);
                if (item)
                    return ContextTopic::forObject(item->guid, item->type, item->name);
            }
        }
    }

    return ContextTopic::forPanel(QStringLiteral("Blueprint"));
}

void BlueprintCanvasWidget::populateContextMenu(QMenu& menu, QContextMenuEvent* event)
{
    auto* hit = event ? itemAt(event->pos()) : nullptr;
    auto* node = findOwningNodeItem(hit);
    if (node != nullptr && node->objectID() != 0) {
        populateNodeContextMenu_(menu, node);
        return;
    }
    auto* edge = dynamic_cast<ConnectionPath*>(hit);
    if (node && node->routeGainMode()) {
        const auto found = std::find(gainNodes_.begin(), gainNodes_.end(), node);
        if (found != gainNodes_.end()) edge = edges_[static_cast<size_t>(found - gainNodes_.begin())];
    }
    if (edge && activeSource_ == pathSource_.get()) {
        if (!edge->isSelected()) {
            const QSignalBlocker blocker(scene_);
            scene_->clearSelection();
            const auto output = std::find(gainOutputEdges_.begin(), gainOutputEdges_.end(), edge);
            const auto primary = std::find(edges_.begin(), edges_.end(), edge);
            if (output != gainOutputEdges_.end())
                setSignalPathEdgeSelected_(static_cast<size_t>(output - gainOutputEdges_.begin()), true);
            else if (primary != edges_.end())
                setSignalPathEdgeSelected_(static_cast<size_t>(primary - edges_.begin()), true);
        }
        onSceneSelectionChanged();
        const auto indices = selectedSignalPathEdges_(true);
        auto plan = prepareSignalPathDisconnect(pathSource_->graph(), indices);
        auto* action = menu.addAction(tr("Delete"));
        action->setEnabled(plan.ok());
        action->setToolTip(indices.empty() ? tr("Master Output connections cannot be deleted")
            : plan.ok() ? plan.description : plan.error);
        connect(action, &QAction::triggered, this, [this, plan = std::move(plan)] {
            if (applySignalPathConnect(plan)) schedulePathRefresh_();
            else LogPanel::logError(LogCategory::GUI,
                tr("The connections changed and could not be deleted"));
        });
        return;
    }
    populateCreationMenu_(menu, mapToScene(event ? event->pos() : viewport()->rect().center()));
}

BlueprintCanvasWidget::PathEdgeKey BlueprintCanvasWidget::signalPathEdgeKey_(size_t index) const
{
    const auto& graph = pathSource_->graph();
    const auto& edge = graph.edges[index];
    return {graph.nodes[edge.from.occurrenceIndex].key.objectGuid,
        graph.nodes[edge.to.occurrenceIndex].key.objectGuid, edge.kind,
        edge.routingSource ? std::make_optional(edge.routingSource->objectGuid) : std::nullopt};
}

void BlueprintCanvasWidget::setSignalPathEdgeSelected_(size_t index, bool selected)
{
    edges_[index]->setSelected(selected);
    if (auto* gain = signalPathGainNode(index)) {
        gain->setSelected(selected);
        gainOutputEdges_[index]->setSelected(selected);
    }
}

bool BlueprintCanvasWidget::signalPathEdgeDeletable_(size_t index) const
{
    const auto* source = activeSignalPathSource();
    if (!source || index >= source->graph().edges.size()) return false;
    const auto& graph = source->graph();
    const auto& edge = graph.edges[index];
    const auto* master = ProjectManager::instance().getMasterBusItem();
    return edge.kind != SignalPathEdgeKind::Output
        || (master && graph.nodes[edge.to.occurrenceIndex].key.objectGuid != master->guid);
}

std::vector<size_t> BlueprintCanvasWidget::selectedSignalPathEdges_(bool deletableOnly) const
{
    std::vector<size_t> result;
    if (activeSource_ != pathSource_.get()) return result;
    for (size_t index = 0; index < edges_.size(); ++index) {
        if (edges_[index]->isSelected()
            && (!deletableOnly || signalPathEdgeDeletable_(index)))
            result.push_back(index);
    }
    return result;
}

void BlueprintCanvasWidget::populateNodeContextMenu_(QMenu& menu, NodeItem* node)
{
    auto& pm = ProjectManager::instance();
    const uint32_t id = node->objectID();
    const bool isBus = (node->role() == NodeRole::Bus ||
                        node->role() == NodeRole::ReceiveBus);
    ProjectItem* target = isBus ? pm.findBusItem(id)
                                : canvasFindItem(pm.currentProject(), id);
    if (target == nullptr) return;

    const QUuid guid = target->guid;
    ProjectItemMenuController::Callbacks callbacks;
    callbacks.selectResult = [this](ProjectItem* item) { showCreatedItem_(item, std::nullopt); };
    auto* controller = new ProjectItemMenuController(std::move(callbacks), &menu);
    controller->buildNewChildMenu(&menu);
    controller->updateActionVisibility(target, {});
    if (controller->newChildMenu()->menuAction()->isVisible()) menu.addSeparator();

    QAction* renameAct = menu.addAction(tr("Rename"));
    connect(renameAct, &QAction::triggered, this, [this, node, guid]() {
        const auto* current = ProjectManager::instance().findItemByGuid(guid);
        if (!current) return;
        for (const auto& [key, candidate] : nodeMap_) {
            if (candidate == node && key.id == current->objectID) {
                candidate->beginTitleEditing();
                return;
            }
        }
    });

    menu.addSeparator();

    QAction* deleteAct = menu.addAction(tr("Delete"));
    deleteAct->setEnabled(target != pm.getMasterBusItem());
    connect(deleteAct, &QAction::triggered, this, [guid]() {
        auto& manager = ProjectManager::instance();
        if (auto* item = manager.findItemByGuid(guid)) manager.deleteItem(item);
    });
}

void BlueprintCanvasWidget::populateCreationMenu_(QMenu& menu, const QPointF& position)
{
    if (!ProjectManager::instance().isProjectOpen()) return;
    ProjectItemMenuController::Callbacks callbacks;
    callbacks.creationParent = [](ProjectItem::Type type) -> ProjectItem* {
        return type == ProjectItem::Type::Bus ? ProjectManager::instance().getMasterBusItem() : nullptr;
    };
    callbacks.selectResult = [this, position](ProjectItem* item) { showCreatedItem_(item, position); };
    auto* controller = new ProjectItemMenuController(std::move(callbacks), &menu);
    auto types = ItemTypeCatalog::userFacingChildTypesOf(ProjectItem::Type::Layer);
    const auto busTypes = ItemTypeCatalog::userFacingChildTypesOf(ProjectItem::Type::Bus);
    types.insert(types.end(), busTypes.begin(), busTypes.end());
    controller->buildCreationMenu(menu.addMenu(tr("Create")), types);
}

void BlueprintCanvasWidget::showCreatedItem_(ProjectItem* item, std::optional<QPointF> position)
{
    if (item->type == ProjectItemType::Sound) setSoundsFiltered(false);
    rebuildFromProject();
    const auto& nodes = pathSource_->graph().nodes;
    const auto found = std::find_if(nodes.begin(), nodes.end(),
        [&](const auto& node) { return node.key.objectGuid == item->guid; });
    if (found == nodes.end()) return;
    const auto index = static_cast<size_t>(found - nodes.begin());
    auto* node = signalPathNode(index);
    if (node && !node->isVisible()) revealChain_(*item, RevealScope::Node);
    if (!node || !node->isVisible()) return;
    selectedPathKeys_ = {found->key};
    selectedPathEdgeKeys_.clear();
    if (position) applyPathPositions_({{pathPositionKeys_[index], *position}});
    {
        const QSignalBlocker blocker(scene_);
        scene_->clearSelection();
        node->setSelected(true);
    }
    onSceneSelectionChanged();
    QTimer::singleShot(0, this, [this, guid = item->guid] {
        if (!projectView_ || activeSource_ != pathSource_.get()) return;
        auto* created = ProjectManager::instance().findItemByGuid(guid);
        if (!created) return;
        const auto& nodes = pathSource_->graph().nodes;
        for (size_t index = 0; index < nodes.size(); ++index) {
            if (nodes[index].key.objectGuid != guid) continue;
            auto* node = signalPathNode(index);
            if (!node || !node->isSelected()) continue;
            setFocus(Qt::OtherFocusReason);
            node->beginTitleEditing();
            const int margin = ThemeManager::instance().metrics().spacingSmall;
            ensureVisible(node->titleEditor()->graphicsProxyWidget(), margin, margin);
            return;
        }
    });
}

void BlueprintCanvasWidget::resetNodeLayout()
{
    if (activeSource_ == pathSource_.get()) {
        PathPositions positions;
        QSettings settings;
        for (const auto& key : pathPositionKeys_) {
            if (settings.contains(key)) positions.emplace_back(key, QVariant{});
        }
        for (const auto& key : gainPositionKeys_)
            if (!key.isEmpty() && settings.contains(key)) positions.emplace_back(key, QVariant());
        commitPathPositions_(positions, tr("Reset Blueprint Layout"));
        return;
    }
    auto& pm = ProjectManager::instance();
    auto* project = pm.currentProject();
    if (project == nullptr) return;
    std::vector<std::pair<uint32_t, QPointF>> toReset;
    for (auto const& kv : nodeMap_) {
        if (kv.second == nullptr || kv.first.id == 0) continue;
        ProjectItem* owner = (kv.first.kind == NodeKind::ProjectItem)
                                 ? canvasFindItem(project, kv.first.id)
                                 : pm.findBusItem(kv.first.id);
        if (owner != nullptr &&
            (owner->canvasPos.x() != 0.0 || owner->canvasPos.y() != 0.0)) {
            toReset.emplace_back(kv.first.id, owner->canvasPos);
        }
    }
    if (toReset.empty()) return;
    auto& undo = UndoRedoManager::instance();
    undo.beginMacro(QStringLiteral("Reset Blueprint Layout"));
    for (auto const& entry : toReset) {
        undo.push(new BlueprintNodePositionCommand(
            entry.first, entry.second, QPointF(0.0, 0.0)));
    }
    undo.endMacro();
}

void BlueprintCanvasWidget::zoomToFit()
{
    QRectF bounds;
    if (scene_) {
        for (const auto* item : scene_->items())
            if (item->isVisible()) bounds |= item->sceneBoundingRect();
    }
    if (bounds.isEmpty()) {
        setZoomFactor(1.0);
        return;
    }
    fitInView(bounds.adjusted(-60.0, -60.0, 60.0, 60.0), Qt::KeepAspectRatio);
    const double fitted = transform().m11();
    const double clamped = std::clamp(fitted, kMinZoom, 1.0);
    if (!qFuzzyCompare(fitted, clamped)) {
        setZoomFactor(clamped);
        return;
    }
    emit zoomChanged(fitted);
}

void BlueprintCanvasWidget::loadPersistedFlowDirection_()
{
    QSettings settings;
    const QString stored = settings.value(QString::fromLatin1(kFlowSettingsKey),
                                          QStringLiteral("LR")).toString();
    flowDir_ = flowDirectionFromString(stored);
}

void BlueprintCanvasWidget::persistFlowDirection_() const
{
    QSettings settings;
    settings.setValue(QString::fromLatin1(kFlowSettingsKey),
                      QString::fromLatin1(flowDirectionToString(flowDir_)));
}

void BlueprintCanvasWidget::loadPersistedLayerVisibility_()
{
    if (chainSource_) chainSource_->loadPersistedLayerVisibility();
}

void BlueprintCanvasWidget::setFlowDirection(CanvasFlowDirection dir)
{
    if (dir == flowDir_) return;
    if (activeSource_ == pathSource_.get()) {
        cancelConnection_();
        finishNodeEdits_(false);
    }
    flowDir_ = dir;

    persistFlowDirection_();

    if (activeSource_ == pathSource_.get()) {
        materializeFromSource_();
        emit flowDirectionChanged(flowDir_);
        return;
    }

    for (auto const& kv : nodeMap_) {
        if (kv.second != nullptr) {
            kv.second->setCanvasFlowDirection(flowDir_);
        }
    }
    for (auto* edge : edges_) {
        if (edge != nullptr) {
            edge->setBlueprintFlowDirection(flowDir_);
        }
    }

    emit flowDirectionChanged(flowDir_);
}

CanvasFlowDirection BlueprintCanvasWidget::flowDirection() const
{
    return flowDir_;
}

void BlueprintCanvasWidget::setGridSnapEnabled(bool enabled)
{
    if (gridSnapEnabled_ == enabled) return;
    gridSnapEnabled_ = enabled;
    QSettings().setValue(QStringLiteral("Blueprint/GridSnapEnabled"), enabled);
    updateNodeGridSnap_();
    emit gridSnapEnabledChanged(enabled);
}

void BlueprintCanvasWidget::updateNodeGridSnap_()
{
    const double cell = gridSpacing();
    for (const auto& [key, node] : nodeMap_)
        node->setBlueprintGridSnap(!key.occurrenceIndex && gridSnapEnabled_, cell);
}

void BlueprintCanvasWidget::setNodeContentFields(NodeContentFields fields)
{
    fields &= kAllNodeContentFields;
    if (nodeContentFields_ == fields) return;
    cancelConnection_();
    finishNodeEdits_(false);
    nodeContentFields_ = fields;
    QSettings().setValue(QStringLiteral("Blueprint/NodeContentFields"), fields.toInt());
    materializeFromSource_();
    emit nodeContentFieldsChanged(fields);
}

bool BlueprintCanvasWidget::supportsHierarchyCollapse(ProjectItemType type)
{
    return ItemTypeCatalog::bucketOf(type) == ItemTypeCatalog::Bucket::Layers
        && ItemTypeCatalog::canAcceptAnyChild(type);
}

void BlueprintCanvasWidget::setSoundsFiltered(bool filtered)
{
    if (soundsFiltered_ == filtered) return;
    cancelConnection_();
    finishNodeEdits_(true);
    soundsFiltered_ = filtered;
    applyTreeVisibility_();
    emit viewportFilterChanged();
}

void BlueprintCanvasWidget::setHierarchyVisible(uint32_t objectId, bool visible)
{
    const auto* root = ProjectManager::instance().findItemByObjectID(objectId);
    if (!root || !supportsHierarchyCollapse(root->type) || !canCollapseChain(objectId)) return;
    if (visible) {
        revealChain_(*root, RevealScope::Inputs);
        return;
    }
    for (const auto* ancestor = root->parent; ancestor; ancestor = ancestor->parent)
        if (hiddenHierarchyRoots_.contains(ancestor->guid)
            && hierarchyVisibility(objectId) == Qt::Unchecked) return;
    auto hidden = hiddenHierarchyRoots_;
    hidden.insert(root->guid);
    if (hiddenHierarchyRoots_ == hidden && !collapseReveals_.contains(root->guid)) return;
    cancelConnection_();
    finishNodeEdits_(true);
    hiddenHierarchyRoots_ = std::move(hidden);
    collapseReveals_.erase(root->guid);
    applyTreeVisibility_();
    emit hierarchyCollapseChanged();
}

Qt::CheckState BlueprintCanvasWidget::hierarchyVisibility(uint32_t objectId) const
{
    const auto* root = ProjectManager::instance().findItemByObjectID(objectId);
    if (!root || !supportsHierarchyCollapse(root->type)) return Qt::Unchecked;
    if (const auto entry = inputVisibility_.find(objectId); entry != inputVisibility_.end())
        return entry->second;
    return Qt::Unchecked;
}

bool BlueprintCanvasWidget::hasCollapsedHierarchy() const
{
    auto& manager = ProjectManager::instance();
    return std::any_of(hiddenHierarchyRoots_.begin(), hiddenHierarchyRoots_.end(), [&manager](const QUuid& guid) {
        const auto* item = manager.findItemByGuid(guid);
        return item && supportsHierarchyCollapse(item->type);
    });
}

void BlueprintCanvasWidget::setBusVisible(uint32_t busObjectId, bool visible)
{
    const auto* bus = ProjectManager::instance().findBusItem(busObjectId);
    if (!bus || !canCollapseChain(busObjectId)) return;
    if (visible) {
        revealChain_(*bus, RevealScope::Inputs);
        return;
    }
    const auto& graph = pathSource_->graph();
    if (busTree(graph, hiddenBuses_).buses.contains(bus->guid)
        && busVisibility(busObjectId) == Qt::Unchecked) return;
    auto hidden = hiddenBuses_;
    hidden.insert(bus->guid);
    if (hiddenBuses_ == hidden && !collapseReveals_.contains(bus->guid)) return;
    cancelConnection_();
    finishNodeEdits_(true);
    hiddenBuses_ = std::move(hidden);
    collapseReveals_.erase(bus->guid);
    applyTreeVisibility_();
    emit busCollapseChanged();
}

Qt::CheckState BlueprintCanvasWidget::busVisibility(uint32_t busObjectId) const
{
    const auto* bus = ProjectManager::instance().findBusItem(busObjectId);
    if (!bus) return Qt::Unchecked;
    if (const auto entry = inputVisibility_.find(busObjectId); entry != inputVisibility_.end())
        return entry->second;
    return Qt::Unchecked;
}

bool BlueprintCanvasWidget::hasCollapsedBuses() const
{
    auto& manager = ProjectManager::instance();
    return std::any_of(hiddenBuses_.begin(), hiddenBuses_.end(), [&manager](const QUuid& guid) {
        const auto* item = manager.findItemByGuid(guid);
        return item && item->type == ProjectItemType::Bus;
    });
}

void BlueprintCanvasWidget::setChainVisible(uint32_t objectId, bool visible, bool recursive)
{
    const auto* item = ProjectManager::instance().findItemByObjectID(objectId);
    if (!item || !canCollapseChain(objectId)) return;
    if (visible) {
        revealChain_(*item, recursive ? RevealScope::Upstream : RevealScope::Inputs);
        return;
    }
    if (!recursive) {
        if (item->type == ProjectItemType::Bus) setBusVisible(objectId, false);
        else setHierarchyVisible(objectId, false);
        return;
    }
    const auto& graph = pathSource_->graph();
    const auto inputs = viewportInputs(graph, soundsFiltered_);
    QSet<size_t> targets;
    for (size_t index = 0; index < graph.nodes.size(); ++index) {
        if (graph.nodes[index].objectID != objectId) continue;
        targets.insert(index);
        for (size_t edgeIndex : upstreamScope(graph, index, inputs).edges)
            targets.insert(graph.edges[edgeIndex].from.occurrenceIndex);
    }
    cancelConnection_();
    finishNodeEdits_(true);
    for (size_t index : targets) {
        const auto& node = graph.nodes[index];
        if (!canCollapseChain(node.objectID)) continue;
        if (node.type == ProjectItemType::Bus) hiddenBuses_.insert(node.key.objectGuid);
        else hiddenHierarchyRoots_.insert(node.key.objectGuid);
        collapseReveals_.erase(node.key.objectGuid);
    }
    applyTreeVisibility_();
    emit hierarchyCollapseChanged();
    emit busCollapseChanged();
}

Qt::CheckState BlueprintCanvasWidget::chainVisibility(uint32_t objectId) const
{
    const auto& manager = ProjectManager::instance();
    const auto* item = manager.findItemByObjectID(objectId);
    if (!item || (!supportsHierarchyCollapse(item->type) && item->type != ProjectItemType::Bus))
        return Qt::Unchecked;
    const auto entry = inputVisibility_.find(objectId);
    if (entry == inputVisibility_.end()) return Qt::Unchecked;
    if (entry->second != Qt::Checked)
        return entry->second;
    const auto* source = activeSignalPathSource();
    const auto* project = manager.currentProject();
    if (!source || !project) return Qt::Unchecked;
    const auto& graph = source->graph();
    for (size_t index = 0; index < graph.nodes.size(); ++index) {
        if (graph.nodes[index].objectID != objectId) continue;
        const auto inputs = viewportInputs(graph, soundsFiltered_);
        const auto scope = upstreamScope(graph, index, inputs);
        const auto visibility = treeVisibility(graph, *project, hiddenHierarchyRoots_, hiddenBuses_,
            collapseReveals_, soundsFiltered_);
        const bool partial = std::any_of(scope.edges.begin(), scope.edges.end(),
                [&visibility](size_t edgeIndex) { return !visibility.edges[edgeIndex]; })
            || std::any_of(scope.paths.begin(), scope.paths.end(),
                [&visibility](size_t pathIndex) { return !visibility.paths[pathIndex]; });
        return partial ? Qt::PartiallyChecked : Qt::Checked;
    }
    return Qt::Unchecked;
}

bool BlueprintCanvasWidget::canCollapseChain(uint32_t objectId) const
{
    return inputVisibility_.contains(objectId);
}

bool BlueprintCanvasWidget::viewportEvent(QEvent* event)
{
    switch (event->type()) {
    case QEvent::StyleChange:
    case QEvent::PaletteChange:
    case QEvent::FontChange:
    case QEvent::DevicePixelRatioChange:
        scheduleContentPresentationRefresh_();
        break;
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
        activateNodeEditorAt_(static_cast<QMouseEvent*>(event)->position().toPoint());
        scheduleContentRetirement_();
        break;
    case QEvent::Wheel:
        activateNodeEditorAt_(static_cast<QWheelEvent*>(event)->position().toPoint());
        break;
    case QEvent::ToolTip:
        activateNodeEditorAt_(static_cast<QHelpEvent*>(event)->pos());
        break;
    case QEvent::MouseButtonRelease:
    case QEvent::Leave:
    case QEvent::Hide:
        scheduleContentRetirement_();
        break;
    default: break;
    }
    if (locationPing_ && event->type() == QEvent::Hide) {
        locationPing_->stop();
        locationPingObjectID_ = 0;
    }
    if (dragSourcePin_ && event->type() == QEvent::ToolTip) {
        event->accept();
        return true;
    }
    const bool handled = BlueprintCanvasView::viewportEvent(event);
    if (dragSourcePin_ && event->type() == QEvent::Resize) positionConnectHint_();
    return handled;
}

void BlueprintCanvasWidget::mousePressEvent(QMouseEvent* event)
{
    selectionClickObjectID_ = 0;
    if (dragSourcePin_) {
        if (event->button() == Qt::RightButton) cancelConnection_();
        event->accept();
        return;
    }
    if (interactionMode() == InteractionMode::Pan) {
        BlueprintCanvasView::mousePressEvent(event);
        return;
    }
    if (auto* proxy = dynamic_cast<QGraphicsProxyWidget*>(itemAt(event->pos()))) {
        const auto* editor = qobject_cast<NodeContentEditor*>(proxy->widget());
        if (!editor || !editor->isDragHandleAt(proxy->mapFromScene(mapToScene(event->pos())).toPoint())) {
            BlueprintCanvasView::mousePressEvent(event);
            return;
        }
    }
    if (event->button() == Qt::LeftButton) finishNodeEdits_(true);
    if (event->button() == Qt::LeftButton && activeSource_ == pathSource_.get()
        && findOwningNodeItem(itemAt(event->pos()))) {
        nodeDragOrigin_ = findOwningNodeItem(itemAt(event->pos()));
        nodeDragStart_ = event->pos();
        nodeDragSources_.clear();
        pathDragStartPositions_.clear();
        for (size_t index = 0; index < pathPositionKeys_.size(); ++index)
            pathDragStartPositions_.push_back(signalPathNode(index)->pos());
        gainDragStartPositions_.resize(gainNodes_.size());
        for (size_t index = 0; index < gainNodes_.size(); ++index)
            if (gainNodes_[index]) gainDragStartPositions_[index] = gainNodes_[index]->pos();
    }
    if (event->button() == Qt::LeftButton && activeSource_
        && activeSource_->supportsEditing()) {
        const QPointF scenePoint = mapToScene(event->pos());
        NodePin* pin = firstPinAt(scene_, scenePoint);
        if (pin != nullptr) {
            pathDragStartPositions_.clear();
            gainDragStartPositions_.clear();
            nodeDragOrigin_ = nullptr;
            if (activeSource_ == pathSource_.get() && !pathEndpoint_(pin)) {
                event->accept();
                return;
            }
            dragSourcePin_ = pin;
            dragAddsSend_ = event->modifiers().testFlag(Qt::AltModifier);
            connectStartPosition_ = event->pos();
            connectPosition_ = event->pos();
            if (!pin->chainVisibility()) beginConnectionDrag_(event->pos());
            event->accept();
            return;
        }
    }
    if (event->button() == Qt::LeftButton && event->modifiers() == Qt::NoModifier) {
        auto* node = dynamic_cast<NodeItem*>(itemAt(event->pos()));
        if (node && node->isSelected() && !node->routeGainMode()) {
            auto& manager = ProjectManager::instance();
            auto* item = manager.findItemByObjectID(node->objectID());
            if (!item) item = manager.findBusItem(node->objectID());
            if (item && ItemTypeCatalog::isUserFacingType(item->type)) {
                selectionClickObjectID_ = node->objectID();
                selectionClickStart_ = event->pos();
            }
        }
    }
    BlueprintCanvasView::mousePressEvent(event);
    if (nodeDragOrigin_ && !nodeDragOrigin_->routeGainMode() && nodeDragOrigin_->isSelected()) {
        for (const auto& [key, node] : nodeMap_)
            if (key.occurrenceIndex && node->isSelected())
                nodeDragSources_.push_back({*key.occurrenceIndex, 0});
        std::sort(nodeDragSources_.begin(), nodeDragSources_.end(), [](const auto& a, const auto& b) {
            return a.occurrenceIndex < b.occurrenceIndex;
        });
    }
}

void BlueprintCanvasWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (selectionClickObjectID_
        && (event->pos() - selectionClickStart_).manhattanLength() >= QApplication::startDragDistance())
        selectionClickObjectID_ = 0;
    if (dragSourcePin_ != nullptr) {
        if (!tempPath_) {
            if ((event->pos() - connectStartPosition_).manhattanLength() < QApplication::startDragDistance()) {
                event->accept();
                return;
            }
            beginConnectionDrag_(event->pos());
        }
        if (dragAddsSend_ != event->modifiers().testFlag(Qt::AltModifier)) {
            dragAddsSend_ = !dragAddsSend_;
            clearConnectTarget_();
        }
        updateConnectPreview_(event->pos());
        event->accept();
        return;
    }
    BlueprintCanvasView::mouseMoveEvent(event);
    if (!nodeDragSources_.empty() && event->buttons().testFlag(Qt::LeftButton)
        && (event->pos() - nodeDragStart_).manhattanLength() >= QApplication::startDragDistance())
        updateNodeDropPreview_(event->pos());
}

void BlueprintCanvasWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (dragSourcePin_ && !tempPath_ && event->button() == Qt::LeftButton
        && (event->pos() - connectStartPosition_).manhattanLength() >= QApplication::startDragDistance())
        beginConnectionDrag_(event->pos());
    if (dragSourcePin_ && !tempPath_) {
        if (event->button() != Qt::LeftButton) { event->accept(); return; }
        auto* pin = dragSourcePin_;
        const auto state = pin->chainVisibility();
        const bool clicked = (event->pos() - connectStartPosition_).manhattanLength() < QApplication::startDragDistance()
            && firstPinAt(scene_, mapToScene(event->pos())) == pin;
        auto* node = findOwningNodeItem(pin);
        cancelConnection_();
        if (clicked && state && node) {
            setChainVisible(node->objectID(), *state != Qt::Checked,
                event->modifiers().testFlag(Qt::ControlModifier));
        }
        event->accept();
        return;
    }
    if (tempPath_ != nullptr) {
        if (event->button() != Qt::LeftButton) { event->accept(); return; }
        NodePin* targetPin = nullptr;
        NodeItem* targetNode = connectTargetAt_(event->pos(), targetPin);
        NodePin* sourcePin = dragSourcePin_;
        const bool addSend = event->modifiers().testFlag(Qt::AltModifier);
        if (targetNode && targetNode->routeGainMode()) {
            cancelConnection_();
            event->accept();
            return;
        }
        if (activeSource_ == pathSource_.get()) {
            auto plan = prepareConnect_(sourcePin, targetNode, targetPin, addSend);
            cancelConnection_();
            if (targetNode) {
                if (!plan.ok()) toast_->showMessage(plan.error);
                else if (applyConnectPlan_(std::move(plan))) schedulePathRefresh_();
                else LogPanel::logError(LogCategory::GUI,
                    tr("The objects changed and could not be connected"));
            }
            event->accept();
            return;
        }
        cancelConnection_();

        if (targetPin != nullptr && sourcePin != nullptr && targetPin != sourcePin) {
            QString reason;
            if (!evaluateConnect_(sourcePin, targetPin, reason, addSend)) {
                toast_->showMessage(reason);
            } else {
                auto* srcNode = dynamic_cast<NodeItem*>(sourcePin->parentItem());
                auto* dstNode = dynamic_cast<NodeItem*>(targetPin->parentItem());
                const bool srcIsOutput = (sourcePin->pinType() == NodePin::Output);
                NodeItem* fromNode = srcIsOutput ? srcNode : dstNode;
                NodeItem* toNode   = srcIsOutput ? dstNode : srcNode;
                const uint32_t fromId = fromNode ? fromNode->objectID() : 0;
                const uint32_t toId   = toNode   ? toNode->objectID()   : 0;
                if (fromId != 0 && toId != 0) {
                    ConnectResult r = connectNodes(fromId, toId);
                    if (r.operationFailed) {
                        LogPanel::logError(LogCategory::GUI, tr("The connection could not be applied"));
                    } else if (!r.success) {
                        toast_->showMessage(r.feedback.isEmpty() ? connectErrorText(r.error) : r.feedback);
                    } else if (!r.feedback.isEmpty()) {
                        toast_->showMessage(r.feedback);
                    }
                }
            }
        }
        event->accept();
        return;
    }
    BlueprintCanvasView::mouseReleaseEvent(event);
    const uint32_t selectedObjectID = selectionClickObjectID_;
    selectionClickObjectID_ = 0;
    if (selectedObjectID && event->button() == Qt::LeftButton
        && (event->pos() - selectionClickStart_).manhattanLength() < QApplication::startDragDistance()) {
        auto* node = dynamic_cast<NodeItem*>(itemAt(event->pos()));
        if (node && node->isSelected() && node->objectID() == selectedObjectID)
            emit nodeSelected(selectedObjectID);
    }
    if (event->button() == Qt::LeftButton && !pathDragStartPositions_.empty()) {
        NodePin* pin = nullptr;
        NodeItem* target = !nodeDragSources_.empty()
            && (event->pos() - nodeDragStart_).manhattanLength() >= QApplication::startDragDistance()
            ? connectTargetAt_(event->pos(), pin, true) : nullptr;
        if (target && target->routeGainMode()) {
            cancelConnection_();
            return;
        }
        if (target) {
            SignalPathConnectPlan plan;
            const auto endpoint = pathEndpoint_(target->inputPin());
            if (pin) plan.error = tr("Drop onto the node body");
            else if (endpoint) plan = prepareSignalPathNodeDrop(pathSource_->graph(), nodeDragSources_, *endpoint);
            else plan.error = tr("Gain nodes cannot be used as connection targets");
            restoreNodeDragPositions_();
            clearConnectTarget_();
            nodeDragOrigin_ = nullptr;
            nodeDragSources_.clear();
            pathDragStartPositions_.clear();
            gainDragStartPositions_.clear();
            QToolTip::hideText();
            unsetCursor();
            viewport()->unsetCursor();
            if (!plan.ok()) toast_->showMessage(plan.error);
            else if (!applyConnectPlan_(std::move(plan))) LogPanel::logError(LogCategory::GUI,
                tr("The objects changed and could not be moved"));
            pathRefreshPending_ = false;
            schedulePathRefresh_();
            return;
        }
        clearConnectTarget_();
        if (!nodeDragSources_.empty()) {
            QToolTip::hideText();
            unsetCursor();
            viewport()->unsetCursor();
        }
        nodeDragOrigin_ = nullptr;
        nodeDragSources_.clear();
        PathPositions positions;
        const double cell = gridSpacing();
        for (size_t index = 0; index < pathDragStartPositions_.size(); ++index) {
            const auto pos = signalPathNode(index)->pos();
            if (pos != pathDragStartPositions_[index])
                positions.emplace_back(pathPositionKeys_[index], gridSnapEnabled_
                    ? Blueprint::snapToGrid(pos, cell) : pos);
        }
        for (size_t index = 0; index < gainDragStartPositions_.size(); ++index) {
            if (!gainNodes_[index]) continue;
            const auto pos = gainNodes_[index]->pos();
            if (pos != gainDragStartPositions_[index]) {
                positions.emplace_back(gainPositionKeys_[index], gridSnapEnabled_
                    ? Blueprint::snapToGrid(pos, cell) : pos);
            }
        }
        if (!positions.empty()) {
            const QSettings settings;
            for (size_t index = 0; index < pathDragStartPositions_.size(); ++index)
                if (signalPathNode(index)->pos() == pathDragStartPositions_[index]
                    && !settings.contains(pathPositionKeys_[index]))
                    positions.emplace_back(pathPositionKeys_[index], pathDragStartPositions_[index]);
            for (size_t index = 0; index < gainDragStartPositions_.size(); ++index)
                if (gainNodes_[index] && gainNodes_[index]->pos() == gainDragStartPositions_[index]
                    && !settings.contains(gainPositionKeys_[index]))
                    positions.emplace_back(gainPositionKeys_[index], gainDragStartPositions_[index]);
        }
        pathDragStartPositions_.clear();
        gainDragStartPositions_.clear();
        commitPathPositions_(positions, tr("Move Blueprint Nodes"));
        if (pathRefreshPending_) {
            queuePathRefresh_(pathContentOnlyRefresh_);
        }
    }
}

void BlueprintCanvasWidget::layoutPathNodes_()
{
    if (activeSource_ != pathSource_.get()) return;
    std::vector<QSizeF> sizes;
    std::vector<QSizeF> gainSizes(gainNodes_.size());
    std::vector<std::optional<QPointF>> fixedPositions(pathPositionKeys_.size());
    std::vector<std::optional<QPointF>> fixedGainPositions(gainNodes_.size());
    QSettings settings;
    for (size_t index = 0; index < pathPositionKeys_.size(); ++index) {
        sizes.push_back(signalPathNode(index)->shape().boundingRect().size());
        const auto stored = settings.value(pathPositionKeys_[index]);
        if (stored.canConvert<QPointF>()) fixedPositions[index] = stored.toPointF();
    }
    for (size_t index = 0; index < gainNodes_.size(); ++index) {
        if (!gainNodes_[index]) continue;
        gainSizes[index] = gainNodes_[index]->shape().boundingRect().size();
        const auto stored = settings.value(gainPositionKeys_[index]);
        if (stored.canConvert<QPointF>()) fixedGainPositions[index] = stored.toPointF();
    }
    const auto layout = pathSource_->layout(sizes, flowDir_, fixedPositions, gainSizes, fixedGainPositions);
    for (size_t index = 0; index < layout.nodePositions.size(); ++index)
        signalPathNode(index)->setPos(layout.nodePositions[index]);
    for (size_t index = 0; index < layout.gainPositions.size(); ++index)
        if (layout.gainPositions[index]) gainNodes_[index]->setPos(*layout.gainPositions[index]);
}

void BlueprintCanvasWidget::applyPathPositions_(const PathPositions& positions)
{
    QSettings settings;
    for (const auto& [key, position] : positions) {
        if (position.isValid()) settings.setValue(key, position);
        else settings.remove(key);
    }
    layoutPathNodes_();
}

void BlueprintCanvasWidget::commitPathPositions_(const PathPositions& positions, const QString& description)
{
    if (positions.empty()) return;
    QSettings settings;
    PathPositions previous;
    for (const auto& [key, position] : positions)
        previous.emplace_back(key, settings.value(key));
    applyPathPositions_(positions);
    const QPointer<BlueprintCanvasWidget> canvas(this);
    UndoRedoManager::instance().push(new RuntimeActionCommand(description,
        [canvas, previous]() { if (canvas) canvas->applyPathPositions_(previous); },
        [canvas, positions]() { if (canvas) canvas->applyPathPositions_(positions); },
        0, nullptr, [canvas]() { return !canvas.isNull(); }));
}

bool BlueprintCanvasWidget::evaluateConnect_(NodePin* source, NodePin* target,
                                             QString& reason, bool addSend) const
{
    if (activeSource_ == pathSource_.get()) {
        const auto plan = prepareConnect_(source, target ? findOwningNodeItem(target) : nullptr, target, addSend);
        reason = plan.error;
        return plan.ok();
    }
    if (source == nullptr || target == nullptr) {
        reason = tr("Invalid connection");
        return false;
    }

    auto* srcNode = dynamic_cast<NodeItem*>(source->parentItem());
    auto* dstNode = dynamic_cast<NodeItem*>(target->parentItem());
    if (srcNode == nullptr || dstNode == nullptr) {
        reason = tr("Invalid connection");
        return false;
    }
    if (srcNode == dstNode) {
        reason = tr("Cannot connect a node to itself");
        return false;
    }
    if (srcNode->objectID() == 0 || dstNode->objectID() == 0) {
        reason = tr("Collapsed placeholders cannot be connected");
        return false;
    }

    const bool orientationOk =
        (source->pinType() == NodePin::Output && target->pinType() == NodePin::Input) ||
        (source->pinType() == NodePin::Input  && target->pinType() == NodePin::Output);
    if (!orientationOk) {
        reason = tr("Connect an output pin to an input pin");
        return false;
    }

    if (source->domain() != target->domain()) {
        auto label = [](NodePin::Domain d) -> QString {
            switch (d) {
            case NodePin::Domain::Audio:   return tr("Audio");
            case NodePin::Domain::Control: return tr("Control");
            case NodePin::Domain::Value:   return tr("Value");
            }
            return tr("Audio");
        };
        reason = tr("Pin type mismatch: %1 → %2")
                     .arg(label(source->domain()), label(target->domain()));
        return false;
    }

    const uint32_t aId = srcNode->objectID();
    const uint32_t bId = dstNode->objectID();
    for (const ConnectionPath* e : edges_) {
        if (e == nullptr) continue;
        auto* efrom = e->startPin() ? dynamic_cast<NodeItem*>(e->startPin()->parentItem()) : nullptr;
        auto* eto   = e->endPin()   ? dynamic_cast<NodeItem*>(e->endPin()->parentItem())   : nullptr;
        if (efrom == nullptr || eto == nullptr) continue;
        const uint32_t f = efrom->objectID();
        const uint32_t t = eto->objectID();
        if ((f == aId && t == bId) || (f == bId && t == aId)) {
            reason = tr("Connection already exists");
            return false;
        }
    }

    reason.clear();
    return true;
}

std::optional<SignalPathEndpoint> BlueprintCanvasWidget::pathEndpoint_(NodePin* pin) const
{
    if (!pin) return {};
    for (const auto& [key, node] : nodeMap_) {
        if (!key.occurrenceIndex || pin->parentItem() != node) continue;
        for (int index = 0; index < node->inputCount(); ++index)
            if (node->inputPin(index) == pin || node->outputPin(index) == pin)
                return SignalPathEndpoint{*key.occurrenceIndex, static_cast<size_t>(index)};
    }
    return {};
}

NodeItem* BlueprintCanvasWidget::connectTargetAt_(const QPoint& position, NodePin*& pin, bool nodeDrop) const
{
    pin = nullptr;
    for (auto* hit : scene_->items(mapToScene(position), Qt::IntersectsItemShape, Qt::DescendingOrder)) {
        auto* node = findOwningNodeItem(hit);
        if (!node || (nodeDrop && node->isSelected())) continue;
        pin = dynamic_cast<NodePin*>(hit);
        return node;
    }
    return nullptr;
}

SignalPathConnectPlan BlueprintCanvasWidget::prepareConnect_(NodePin* source, NodeItem* target,
    NodePin* targetPin, bool addSend) const
{
    SignalPathConnectPlan plan;
    if (!source || !target) {
        plan.error = tr("Select a connection target");
        return plan;
    }
    const auto from = pathEndpoint_(source);
    const auto to = pathEndpoint_(target->inputPin());
    if (!from || !to) {
        plan.error = tr("Gain nodes cannot be used as connection targets");
        return plan;
    }
    return prepareSignalPathConnect(pathSource_->graph(), {*from, *to,
        source->pinType() == NodePin::Input ? SignalPathConnectGesture::Input : SignalPathConnectGesture::Output,
        !targetPin ? SignalPathConnectTarget::Body
            : targetPin->pinType() == NodePin::Input ? SignalPathConnectTarget::Input : SignalPathConnectTarget::Output,
        addSend});
}

QString BlueprintCanvasWidget::connectPreviewText_(const NodeItem* target, const SignalPathConnectPlan& plan) const
{
    const bool reverse = dragSourcePin_->pinType() == NodePin::Input;
    if (!target) {
        if (dragAddsSend_) return reverse ? tr("Receive from") : tr("Send to");
        return reverse ? tr("Input/Add from") : tr("Output/Move to");
    }
    if (!plan.ok()) return {};
    const auto& manager = ProjectManager::instance();
    const auto* from = manager.findItemByGuid(plan.from);
    const auto* to = manager.findItemByGuid(plan.to);
    const auto& targetName = reverse ? from->name : to->name;
    if (dragAddsSend_)
        return (reverse ? tr("Receive from %1") : tr("Send to %1")).arg(targetName);
    if (plan.moves.empty())
        return (reverse ? tr("Input from %1") : tr("Output to %1")).arg(targetName);

    if (from->type == ProjectItemType::Bus)
        return (reverse ? tr("Add %1 and Input from %1")
                        : tr("Move to %1 and Output to %1")).arg(targetName);

    return (reverse ? tr("Add %1") : tr("Move to %1")).arg(targetName);
}

void BlueprintCanvasWidget::beginConnectionDrag_(const QPoint& position)
{
    dragSourcePin_->setConnectionDragging(true);
    tempPath_ = new QGraphicsPathItem();
    tempPath_->setZValue(1000.0);
    tempPath_->setAcceptedMouseButtons(Qt::NoButton);
    scene_->addItem(tempPath_);
    updateConnectPreview_(position);
}

void BlueprintCanvasWidget::updateConnectPreview_(const QPoint& position)
{
    if (!tempPath_ || !dragSourcePin_) return;
    connectPosition_ = position;
    const auto point = mapToScene(position);
    NodePin* target = nullptr;
    auto* node = connectTargetAt_(position, target);
    if (target == dragSourcePin_) { target = nullptr; node = nullptr; }
    const auto end = target ? target->scenePos() : point;
    QPen pen(ConnectionPath::defaultColor(), 2.0);
    if (dragAddsSend_) pen.setDashPattern({6, 3});
    tempPath_->setPen(pen);
    tempPath_->setPath(buildTempBezier(dragSourcePin_->scenePos(), end,
        flowDir_, dragSourcePin_->pinType() == NodePin::Input));
    if (!node || target != connectHoverPin_ || node != connectHoverNode_) {
        bool valid = !node;
        SignalPathConnectPlan plan;
        if (node && activeSource_ == pathSource_.get()) {
            plan = prepareConnect_(dragSourcePin_, node, target, dragAddsSend_);
            valid = plan.ok();
        } else if (target) {
            QString reason;
            valid = evaluateConnect_(dragSourcePin_, target, reason, dragAddsSend_);
        }
        clearConnectTarget_();
        connectHoverNode_ = node;
        if (node && !target && !node->routeGainMode()) node->setDropFeedback(valid
            ? NodeItem::DropFeedback::Valid : NodeItem::DropFeedback::Invalid);
        if (target) {
            if (!node->routeGainMode())
                target->setConnectState(valid ? NodePin::ConnectState::Ok : NodePin::ConnectState::Bad);
            connectHoverPin_ = target;
        }
        connectHintText_ = connectPreviewText_(node, plan);
        viewport()->setCursor(valid ? Qt::CrossCursor : Qt::ForbiddenCursor);
    }
    positionConnectHint_();
    connectHint_->setVisible(!connectHintText_.isEmpty());
    connectHint_->raise();
    QToolTip::hideText();
}

void BlueprintCanvasWidget::positionConnectHint_()
{
    const auto area = viewport()->rect().adjusted(8, 8, -8, -8);
    const int padding = 2 * (ThemeManager::instance().metrics().spacingSmall + 1);
    connectHint_->setText(connectHint_->fontMetrics().elidedText(
        connectHintText_, Qt::ElideRight, std::max(1, area.width() - padding)));
    connectHint_->adjustSize();
    QPoint position = connectPosition_ + QPoint(16, -connectHint_->height() - 12);
    position.setX(std::clamp(position.x(), area.left(), std::max(area.left(), area.right() - connectHint_->width())));
    position.setY(std::clamp(position.y(), area.top(), std::max(area.top(), area.bottom() - connectHint_->height())));
    connectHint_->move(position);
}

void BlueprintCanvasWidget::updateNodeDropPreview_(const QPoint& position)
{
    NodePin* pin = nullptr;
    auto* target = connectTargetAt_(position, pin, true);
    if (target == connectHoverNode_ && pin == connectHoverPin_) return;
    clearConnectTarget_();
    connectHoverNode_ = target;
    connectHoverPin_ = pin;
    if (!target) {
        QToolTip::hideText();
        viewport()->unsetCursor();
        return;
    }
    SignalPathConnectPlan plan;
    const auto endpoint = pathEndpoint_(target->inputPin());
    if (pin) plan.error = tr("Drop onto the node body");
    else if (endpoint) plan = prepareSignalPathNodeDrop(pathSource_->graph(), nodeDragSources_, *endpoint);
    else plan.error = tr("Gain nodes cannot be used as connection targets");
    if (!pin && !target->routeGainMode()) target->setDropFeedback(plan.ok()
        ? NodeItem::DropFeedback::Valid : NodeItem::DropFeedback::Invalid);
    viewport()->setCursor(plan.ok() ? Qt::DragMoveCursor : Qt::ForbiddenCursor);
    QToolTip::hideText();
}

void BlueprintCanvasWidget::restoreNodeDragPositions_()
{
    for (size_t index = 0; index < pathDragStartPositions_.size(); ++index)
        signalPathNode(index)->setPos(pathDragStartPositions_[index]);
    for (size_t index = 0; index < gainDragStartPositions_.size(); ++index)
        if (gainNodes_[index]) gainNodes_[index]->setPos(gainDragStartPositions_[index]);
}

void BlueprintCanvasWidget::cancelConnection_()
{
    clearConnectTarget_();
    connectHint_->hide();
    connectHint_->clear();
    connectHintText_.clear();
    delete tempPath_;
    tempPath_ = nullptr;
    if (dragSourcePin_) dragSourcePin_->setConnectionDragging(false);
    dragSourcePin_ = nullptr;
    if (!pathDragStartPositions_.empty()) {
        if (auto* grabber = scene_->mouseGrabberItem()) grabber->ungrabMouse();
        restoreNodeDragPositions_();
        pathDragStartPositions_.clear();
        gainDragStartPositions_.clear();
        nodeDragOrigin_ = nullptr;
        nodeDragSources_.clear();
    }
    unsetCursor();
    viewport()->unsetCursor();
    QToolTip::hideText();
    if (pathRefreshPending_) {
        queuePathRefresh_(pathContentOnlyRefresh_);
    }
}

void BlueprintCanvasWidget::keyPressEvent(QKeyEvent* event)
{
    if (dragSourcePin_) {
        if (event->key() == Qt::Key_Escape) {
            cancelConnection_();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Alt) {
            dragAddsSend_ = true;
            clearConnectTarget_();
            updateConnectPreview_(connectPosition_);
            event->accept();
            return;
        }
    }
    if (event->key() == Qt::Key_Escape && nodeFieldEditing_()) {
        finishNodeEdits_(false);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_F2 && !dragSourcePin_ && !nodeFieldEditing_()) {
        const auto selected = scene_->selectedItems();
        if (selected.size() == 1) {
            if (auto* node = dynamic_cast<NodeItem*>(selected.first())) {
                node->beginTitleEditing();
                event->accept();
                return;
            }
        }
    }
    if (event->key() == Qt::Key_Escape && !pathDragStartPositions_.empty()) {
        cancelConnection_();
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)
        && !dragSourcePin_ && !nodeFieldEditing_() && !selectedSignalPathEdges_(false).empty()) {
        const auto indices = selectedSignalPathEdges_(true);
        if (!indices.empty()) {
            const auto result = disconnectSignalPathEdges(indices);
            if (result.operationFailed)
                LogPanel::logError(LogCategory::GUI, result.error);
            else if (!result.success)
                toast_->showMessage(result.error);
        }
        event->accept();
        return;
    }
    BlueprintCanvasView::keyPressEvent(event);
}

void BlueprintCanvasWidget::keyReleaseEvent(QKeyEvent* event)
{
    if (dragSourcePin_ && event->key() == Qt::Key_Alt) {
        dragAddsSend_ = false;
        clearConnectTarget_();
        updateConnectPreview_(connectPosition_);
        event->accept();
        return;
    }
    BlueprintCanvasView::keyReleaseEvent(event);
}

void BlueprintCanvasWidget::clearConnectTarget_()
{
    if (connectHoverNode_) {
        connectHoverNode_->setDropFeedback(NodeItem::DropFeedback::None);
        connectHoverNode_ = nullptr;
    }
    if (connectHoverPin_ != nullptr) {
        connectHoverPin_->setConnectState(NodePin::ConnectState::None);
        connectHoverPin_ = nullptr;
    }
}

QString BlueprintCanvasWidget::connectErrorText(const QString& code) const
{
    if (code == QLatin1String("duplicate")) {
        return tr("Connection already exists");
    }
    if (code == QLatin1String("send_cycle")) {
        return tr("Send would create a cycle; connection rejected");
    }
    if (code == QLatin1String("send_rejected")) {
        return tr("Send target is the main output bus; connection rejected");
    }
    if (code == QLatin1String("output_rejected")) {
        return tr("Target bus already receives a send from this item; connection rejected");
    }
    if (code == QLatin1String("self_loop")) {
        return tr("Cannot connect a node to itself");
    }
    if (code == QLatin1String("containment_target_invalid")) {
        return tr("Target cannot contain this object");
    }
    if (code == QLatin1String("invalid_bus_target") ||
        code == QLatin1String("bus_route_target_not_bus") ||
        code == QLatin1String("override_target_not_bus") ||
        code == QLatin1String("send_target_not_bus")) {
        return tr("Target is not a bus");
    }
    if (code == QLatin1String("send_from_not_bus")) {
        return tr("Only buses can create sends");
    }
    if (code == QLatin1String("from_not_found") ||
        code == QLatin1String("to_not_found")) {
        return tr("Object no longer exists");
    }
    return tr("Connection is not allowed here");
}

void BlueprintCanvasWidget::drawForeground(QPainter* painter, const QRectF& rect)
{
    BlueprintCanvasView::drawForeground(painter, rect);
    if (painter == nullptr) return;

    paintTransitionWatch_(painter);
    paintLocationPing_(painter);
}

void BlueprintCanvasWidget::setNavigationViewportMargins(const QMargins& margins)
{
    if (navigationViewportMargins_ == margins) return;
    navigationViewportMargins_ = margins;
    toast_->setViewportMargins(margins);
    viewport()->update();
}

bool BlueprintCanvasWidget::pingProjectItem(ProjectItem* item)
{
    locationPing_->stop();
    locationPingObjectID_ = 0;
    viewport()->update();
    if (!item) return false;
    const bool found = std::any_of(nodeMap_.begin(), nodeMap_.end(), [item](const auto& entry) {
        return entry.first.id == item->objectID && entry.second->isVisible();
    });
    if (!found) return false;
    locationPingObjectID_ = item->objectID;
    locationPing_->start();
    return true;
}

void BlueprintCanvasWidget::paintLocationPing_(QPainter* painter)
{
    if (locationPingObjectID_ == 0) return;
    const QRectF available = viewport()->rect().marginsRemoved(navigationViewportMargins_);
    if (available.isEmpty()) return;
    const auto target = std::find_if(nodeMap_.begin(), nodeMap_.end(), [this](const auto& entry) {
        return entry.first.id == locationPingObjectID_ && entry.second->isVisible();
    });
    if (target == nodeMap_.end()) return;
    const auto* node = target->second;
    const QRectF bounds = viewportTransform().mapRect(node->mapRectToScene(node->shape().boundingRect()));
    const double progress = locationPing_->currentValue().toDouble();
    const double pop = LocationPingAnimation::popProgress(progress);
    const double fade = LocationPingAnimation::fadeOpacity(progress);
    const QColor accent = ThemeManager::instance().colors().accent;
    QColor edge = accent;
    edge.setAlphaF(fade);
    QColor fill = accent;
    fill.setAlphaF(0.22 * pop * fade);
    painter->save();
    painter->resetTransform();
    painter->setClipRect(available, Qt::IntersectClip);
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(edge, 2.0));
    painter->setBrush(fill);
    if (bounds.intersects(available)) {
        const double inset = 4.0 + 12.0 * (1.0 - pop);
        const QRectF capsule = bounds.adjusted(-inset, -inset, inset, inset);
        const double radius = std::min(18.0, capsule.height() / 2.0);
        painter->drawRoundedRect(capsule, radius, radius);
    } else {
        const QRectF rim = available.adjusted(3.0, 3.0, -3.0, -3.0);
        if (rim.isEmpty()) {
            painter->restore();
            return;
        }
        const QPointF direction = bounds.center() - available.center();
        const double horizontal = std::abs(direction.x()) / (rim.width() / 2.0);
        const double vertical = std::abs(direction.y()) / (rim.height() / 2.0);
        const QPointF position = available.center() + direction / std::max(horizontal, vertical);
        const double scale = 1.6 - 0.6 * pop;
        const double radius = 68.0 * scale;
        QRadialGradient glow(position, radius);
        glow.setColorAt(0.0, fill);
        QColor transparent = accent;
        transparent.setAlpha(0);
        glow.setColorAt(1.0, transparent);
        painter->setPen(Qt::NoPen);
        painter->setBrush(glow);
        painter->drawEllipse(position, radius, radius);

        QLineF border;
        if (horizontal >= vertical) {
            border = direction.x() >= 0.0
                ? QLineF(rim.topRight(), rim.bottomRight())
                : QLineF(rim.bottomLeft(), rim.topLeft());
        } else {
            border = direction.y() >= 0.0
                ? QLineF(rim.bottomRight(), rim.bottomLeft())
                : QLineF(rim.topLeft(), rim.topRight());
        }
        const QPointF tangent = (border.p2() - border.p1()) / border.length();
        const QPointF inward(-tangent.y(), tangent.x());
        const double length = std::min({33.0 * scale, rim.width(), rim.height()});
        const double before = QPointF::dotProduct(position - border.p1(), tangent);
        const double after = border.length() - before;
        QPainterPath band;
        if (before < length) {
            band.moveTo(border.p1() + inward * (length - before));
            band.lineTo(border.p1());
        } else {
            band.moveTo(position - tangent * length);
        }
        if (after < length) {
            band.lineTo(border.p2());
            band.lineTo(border.p2() + inward * (length - after));
        } else {
            band.lineTo(position + tangent * length);
        }
        painter->setPen(QPen(edge, 3.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(band);
    }
    painter->restore();
}

void BlueprintCanvasWidget::paintTransitionWatch_(QPainter* painter)
{
    if (!kTransitionWatchVisible || transitionWatch_.containerGuid.isNull()) return;

    auto* source = activeSignalChainSource();
    if (!source || !source->currentItem()) return;
    if (source->currentItem()->guid != transitionWatch_.containerGuid) return;

    const auto theme = ThemeManager::instance().snapshot();
    QColor accentColor;
    switch (transitionWatch_.direction) {
    case TransitionSemantics::TransitionWatchDirection::UpPlacement:
        accentColor = theme->domains.blueprint.placementChain;
        break;
    case TransitionSemantics::TransitionWatchDirection::UpOrganization:
        accentColor = theme->domains.blueprint.organizationChain;
        break;
    case TransitionSemantics::TransitionWatchDirection::Down:
    case TransitionSemantics::TransitionWatchDirection::None:
    default:
        accentColor = theme->domains.pinDomain.audio;
        break;
    }

    if (transitionWatch_.found()) {
        auto& pm = ProjectManager::instance();
        auto resolveNode = [&](const QUuid& guid) -> NodeItem* {
            ProjectItem* item = pm.findItemByGuid(guid);
            if (!item) return nullptr;
            auto it = nodeMap_.find(NodeKey{NodeKind::ProjectItem, item->objectID});
            return it != nodeMap_.end() ? it->second : nullptr;
        };

        std::vector<NodeItem*> pathNodes;
        pathNodes.reserve(transitionWatch_.path.size());
        for (const QUuid& guid : transitionWatch_.path) {
            pathNodes.push_back(resolveNode(guid));
        }

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setBrush(Qt::NoBrush);

        for (NodeItem* node : pathNodes) {
            if (!node) continue;
            const QRectF r = node->mapRectToScene(node->shape().boundingRect());
            painter->setPen(QPen(accentColor, 2.0, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
            painter->drawRect(r.adjusted(-2.0, -2.0, 2.0, 2.0));
        }

        for (ConnectionPath* edge : edges_) {
            if (!edge->startPin() || !edge->endPin()) continue;
            auto* from = static_cast<NodeItem*>(edge->startPin()->parentItem());
            auto* to   = static_cast<NodeItem*>(edge->endPin()->parentItem());
            bool adjacent = false;
            for (size_t i = 0; i + 1 < pathNodes.size(); ++i) {
                if ((pathNodes[i] == from && pathNodes[i + 1] == to) ||
                    (pathNodes[i] == to && pathNodes[i + 1] == from)) {
                    adjacent = true;
                    break;
                }
            }
            if (!adjacent) continue;
            painter->setPen(QPen(accentColor, 3.0));
            painter->drawPath(edge->path());
        }

        if (transitionWatch_.referencePiece) {
            if (NodeItem* refNode = resolveNode(*transitionWatch_.referencePiece)) {
                const QRectF r = refNode->sceneBoundingRect();
                const QFont chipFont = FontManager::instance().getFont(
                    FontRole::Caption, 9, QFont::Bold);
                const QFontMetrics chipMetrics(chipFont);
                const QString chipLabel = QStringLiteral("quantize ref");
                const QRectF chipRect(
                    r.right() - chipMetrics.horizontalAdvance(chipLabel) - 18.0,
                    r.bottom() - chipMetrics.height() - 10.0,
                    chipMetrics.horizontalAdvance(chipLabel) + 14.0,
                    chipMetrics.height() + 6.0);
                painter->setPen(QPen(theme->domains.blueprint.canvasBackground, 2.0));
                painter->setBrush(theme->domains.pinDomain.targetOk);
                painter->drawRoundedRect(chipRect, 6.0, 6.0);
                painter->setPen(theme->ui.colors.accentText);
                painter->setFont(chipFont);
                painter->drawText(chipRect, Qt::AlignCenter, chipLabel);
            }
        }

        painter->restore();
    }

    if (transitionWatch_.caption.isEmpty()) return;

    painter->save();
    painter->resetTransform();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const QRect vp = viewport()->rect();
    QFont capFont = FontManager::instance().getFont(FontRole::Body, 10);
    painter->setFont(capFont);
    QFontMetrics fm(capFont);
    const int maxTextW = vp.width() - 24;
    const QString elided = fm.elidedText(transitionWatch_.caption, Qt::ElideRight, maxTextW);
    const int textW = qMin(maxTextW, fm.horizontalAdvance(elided) + 20);
    const QRect textRect(vp.width() / 2 - textW / 2, vp.height() - 36, textW, 24);

    QColor bg = theme->domains.blueprint.canvasBackground;
    bg.setAlpha(220);
    painter->setPen(Qt::NoPen);
    painter->setBrush(bg);
    painter->drawRoundedRect(textRect, 6.0, 6.0);

    painter->setPen(accentColor);
    painter->drawText(textRect, Qt::AlignCenter, elided);
    painter->restore();
}

void BlueprintCanvasWidget::refreshTransitionWatch_()
{
    if (!kTransitionWatchVisible) {
        setTransitionWatch({});
        return;
    }

    auto* source = activeSignalChainSource();
    if (!source || !source->currentItem()
        || source->currentItem()->type != ProjectItem::Type::Container) {
        setTransitionWatch(TransitionSemantics::TransitionWatchView{});
        return;
    }

    const ProjectItem& container = *source->currentItem();
    if (TransitionSemantics::transitionMountState(container,
            currentWorldPlatformScope(PlatformContextService::instance().context()))
        == TransitionMountState::None) {
        setTransitionWatch(TransitionSemantics::TransitionWatchView{});
        return;
    }

    auto& pm = ProjectManager::instance();
    const TimelineItemResolver resolve = [&pm](const QUuid& guid) {
        return pm.findItemByGuid(guid);
    };
    const auto winner = TransitionRehearsal::winnerForContainer(
        container, pm.variableRehearsal(), resolve,
        currentWorldPlatformScope(PlatformContextService::instance().context()));
    const TransitionSemantics::TransitionWatchView view =
        TransitionSemantics::buildTransitionWatchView(
            container, winner, source->placementContext(), resolve);
    setTransitionWatch(view);
}

void BlueprintCanvasWidget::useSignalChain(ProjectItem* item)
{
    projectView_ = false;
    selectedPathKeys_.clear();
    selectedPathEdgeKeys_.clear();
    selectedObjectGuids_.reset();
    if (!chainSource_) return;
    activeSource_ = chainSource_.get();
    if (item == nullptr) {
        chainSource_->clear();
        refreshTransitionWatch_();
        return;
    }
    chainSource_->loadItem(item);
    refreshTransitionWatch_();
}

void BlueprintCanvasWidget::useSignalChain(
    ProjectItem* item, const PlacementContext& placement)
{
    projectView_ = false;
    selectedPathKeys_.clear();
    selectedPathEdgeKeys_.clear();
    selectedObjectGuids_.reset();
    if (!chainSource_) return;
    activeSource_ = chainSource_.get();
    if (item == nullptr) {
        chainSource_->clear();
        refreshTransitionWatch_();
        return;
    }
    chainSource_->loadItem(item, placement);
    refreshTransitionWatch_();
}

IBlueprintSource::Scope BlueprintCanvasWidget::currentScope() const
{
    return activeSource_ ? activeSource_->scope() : IBlueprintSource::Scope::SingleObject;
}

void BlueprintCanvasWidget::useSignalChainForBus(uint32_t busID)
{
    projectView_ = false;
    selectedPathKeys_.clear();
    selectedPathEdgeKeys_.clear();
    selectedObjectGuids_.reset();
    if (!chainSource_) return;
    activeSource_ = chainSource_.get();
    if (busID == 0) {
        chainSource_->clear();
        refreshTransitionWatch_();
        return;
    }
    chainSource_->loadBus(busID);
    refreshTransitionWatch_();
}

void BlueprintCanvasWidget::clear()
{
    toast_->dismiss();
    pathSceneState_.reset();
    pathRefreshPending_ = false;
    pathContentOnlyRefresh_ = false;
    projectView_ = true;
    selectedPathKeys_.clear();
    selectedPathEdgeKeys_.clear();
    selectedObjectGuids_.reset();
    if (chainSource_) chainSource_->clear();
    if (pathSource_) pathSource_->clear();
    setTransitionWatch(TransitionSemantics::TransitionWatchView{});
}

SignalChainSource* BlueprintCanvasWidget::activeSignalChainSource() const
{
    if (!chainSource_) return nullptr;
    return (activeSource_ == chainSource_.get()) ? chainSource_.get() : nullptr;
}

void BlueprintCanvasWidget::useSignalPaths(const ProjectData& project, SignalPathGraph graph)
{
    pathSceneState_.reset();
    projectView_ = false;
    selectedPathKeys_.clear();
    selectedPathEdgeKeys_.clear();
    selectedObjectGuids_.reset();
    pathProjectId_ = project.projectId;
    activeSource_ = pathSource_.get();
    setTransitionWatch({});
    pathSource_->loadGraph(project, std::move(graph));
}

bool BlueprintCanvasWidget::focusItem(ProjectItem* item, const PlacementContext*)
{
    if (!item || (soundsFiltered_ && item->type == ProjectItemType::Sound)) return false;
    const auto bucket = ItemTypeCatalog::bucketOf(item->type);
    if (bucket != ItemTypeCatalog::Bucket::Layers && bucket != ItemTypeCatalog::Bucket::Busses) return false;
    if (!projectView_ || activeSource_ != pathSource_.get()) rebuildFromProject();
    else if (pathRefreshPending_) refreshProjectPaths_();
    const auto& nodes = pathSource_->graph().nodes;
    const auto found = std::find_if(nodes.begin(), nodes.end(),
        [&](const auto& node) { return node.key.objectGuid == item->guid; });
    if (found == nodes.end()) return false;
    auto* target = signalPathNode(static_cast<size_t>(found - nodes.begin()));
    if (target && !target->isVisible()) revealChain_(*item, RevealScope::Node);
    if (!target || !target->isVisible()) return false;
    selectedObjectGuids_.reset();
    {
        const QSignalBlocker blocker(scene_);
        scene_->clearSelection();
        target->setSelected(true);
    }
    selectedPathKeys_ = {found->key};
    selectedPathEdgeKeys_.clear();
    refreshPathFocus_();
    setZoomFactor(1.0);
    centerOn(target);
    return true;
}

void BlueprintCanvasWidget::scheduleChainRefresh_()
{
    if (activeSource_ != chainSource_.get()) return;
    chainRefreshPending_ = true;
    if (chainRefreshScheduled_) return;
    chainRefreshScheduled_ = true;
    QMetaObject::invokeMethod(this, [this]() {
        chainRefreshScheduled_ = false;
        if (activeSource_ != chainSource_.get() || !chainRefreshPending_ || nodeFieldEditing_()) return;
        chainRefreshPending_ = false;
        materializeFromSource_();
        refreshTransitionWatch_();
    }, Qt::QueuedConnection);
}

void BlueprintCanvasWidget::schedulePathRefresh_()
{
    queuePathRefresh_(false);
}

void BlueprintCanvasWidget::queuePathRefresh_(bool contentOnly)
{
    if (activeSource_ != pathSource_.get() || !projectView_) return;
    pathContentOnlyRefresh_ = contentOnly && (!pathRefreshPending_ || pathContentOnlyRefresh_);
    pathRefreshPending_ = true;
    if (pathRefreshScheduled_) return;
    pathRefreshScheduled_ = true;
    QMetaObject::invokeMethod(this, [this]() {
        pathRefreshScheduled_ = false;
        if (!pathRefreshPending_) return;
        if (!pathDragStartPositions_.empty() || dragSourcePin_ || externalDragActive_ || nodeFieldEditing_()) return;
        if (pathContentOnlyRefresh_) refreshPathContents_();
        else refreshProjectPaths_();
    }, Qt::QueuedConnection);
}

void BlueprintCanvasWidget::refreshPathContents_()
{
    if (activeSource_ != pathSource_.get() || !projectView_) return;
    const auto* project = ProjectManager::instance().currentProject();
    if (!project || !pathSource_->graph().ok()) {
        refreshProjectPaths_();
        return;
    }
    pathRefreshPending_ = false;
    pathContentOnlyRefresh_ = false;
    pathSource_->refreshNodeContents(*project);
    const auto nodes = pathSource_->buildNodes();
    for (size_t index = 0; index < nodes.size(); ++index)
        signalPathNode(index)->setContentInfo(nodes[index].content);
    layoutPathNodes_();
    refreshNodeTitles_();
    refreshPlaybackActivity_();
    refreshPathFocus_();
    emit signalChainViewChanged();
}

void BlueprintCanvasWidget::refreshProjectPaths_()
{
    if (activeSource_ != pathSource_.get() || !projectView_) return;
    pathRefreshPending_ = false;
    pathContentOnlyRefresh_ = false;
    const auto* project = ProjectManager::instance().currentProject();
    if (!project) { clear(); return; }
    pathProjectId_ = project->projectId;
    const auto platform = toFieldGroupScope(
        currentWorldPlatformScope(PlatformContextService::instance().context()));
    const auto groups = std::make_shared<FieldGroupEffectiveSnapshot>(
        buildFieldGroupEffectiveSnapshot(*project, platform));
    pathSource_->loadGraph(*project, buildProjectSignalPathGraph(*project, platform, groups), groups);
}

void BlueprintCanvasWidget::applyTreeVisibility_(bool publishSelection)
{
    const auto* source = activeSignalPathSource();
    const auto* project = ProjectManager::instance().currentProject();
    if (!source || !project) return;
    const auto& graph = source->graph();
    const auto inputs = viewportInputs(graph, soundsFiltered_);
    bool hierarchyChanged = false;
    bool busesChanged = false;
    if (projectView_) {
        QSet<QUuid> withInputs;
        for (const auto& edge : graph.edges)
            withInputs.insert(graph.nodes[edge.to.occurrenceIndex].key.objectGuid);
        hierarchyChanged = hiddenHierarchyRoots_.removeIf(
            [&withInputs](const QUuid& guid) { return !withInputs.contains(guid); }) > 0;
        busesChanged = hiddenBuses_.removeIf(
            [&withInputs](const QUuid& guid) { return !withInputs.contains(guid); }) > 0;
        const auto fullyRevealed = [&](const QUuid& guid, bool bus) {
            if (!collapseReveals_.contains(guid)) return false;
            const auto remaining = treeVisibility(graph, *project,
                bus ? QSet<QUuid>{} : QSet<QUuid>{guid},
                bus ? QSet<QUuid>{guid} : QSet<QUuid>{}, collapseReveals_);
            return std::all_of(remaining.nodes.begin(), remaining.nodes.end(), [](bool value) { return value; })
                && std::all_of(remaining.paths.begin(), remaining.paths.end(), [](bool value) { return value; });
        };
        hierarchyChanged |= hiddenHierarchyRoots_.removeIf(
            [&](const QUuid& guid) { return fullyRevealed(guid, false); }) > 0;
        busesChanged |= hiddenBuses_.removeIf(
            [&](const QUuid& guid) { return fullyRevealed(guid, true); }) > 0;
    }
    std::erase_if(collapseReveals_, [&](const auto& entry) {
        return !hiddenHierarchyRoots_.contains(entry.first) && !hiddenBuses_.contains(entry.first);
    });
    const auto visibility = treeVisibility(graph, *project, hiddenHierarchyRoots_, hiddenBuses_,
        collapseReveals_, soundsFiltered_);
    inputVisibility_.clear();
    for (size_t index = 0; index < graph.nodes.size(); ++index) {
        const auto& node = graph.nodes[index];
        if (inputs[index].empty()
            || (!supportsHierarchyCollapse(node.type) && node.type != ProjectItemType::Bus)) continue;
        const size_t visibleCount = std::count_if(inputs[index].begin(), inputs[index].end(),
            [&visibility](size_t edgeIndex) { return visibility.edges[edgeIndex]; });
        const auto state = visibleCount == inputs[index].size() ? Qt::Checked
            : visibleCount == 0 ? Qt::Unchecked : Qt::PartiallyChecked;
        inputVisibility_.emplace(node.objectID, state);
    }
    bool selectionChanged = false;
    const auto setVisible = [&selectionChanged](QGraphicsItem* item, bool visible) {
        if (!item) return;
        if (!visible && item->isSelected()) {
            selectionChanged = true;
            item->setSelected(false);
        }
        item->setVisible(visible);
    };
    {
        const QSignalBlocker blocker(scene_);
        for (size_t index = 0; index < graph.nodes.size(); ++index) {
            setVisible(signalPathNode(index), visibility.nodes[index]);
        }
        for (size_t index = 0; index < graph.edges.size(); ++index) {
            const bool visible = visibility.edges[index];
            setVisible(edges_[index], visible);
            setVisible(signalPathGainNode(index), visible);
            setVisible(signalPathGainOutputEdge(index), visible);
        }
    }
    if (selectionChanged && publishSelection) onSceneSelectionChanged();
    refreshChainPins_();
    if (hierarchyChanged) emit hierarchyCollapseChanged();
    if (busesChanged) emit busCollapseChanged();
}

void BlueprintCanvasWidget::revealChain_(const ProjectItem& item, RevealScope revealScope)
{
    const auto* project = ProjectManager::instance().currentProject();
    if (!project) return;
    const auto& graph = pathSource_->graph();
    auto hierarchy = hiddenHierarchyRoots_;
    auto buses = hiddenBuses_;
    auto reveals = collapseReveals_;
    if (revealScope != RevealScope::Node) {
        hierarchy.remove(item.guid);
        buses.remove(item.guid);
        reveals.erase(item.guid);
    }
    const auto current = treeVisibility(graph, *project, hierarchy, buses, reveals, soundsFiltered_);
    const auto inputs = viewportInputs(graph, soundsFiltered_);
    std::vector<std::vector<size_t>> outputs(graph.nodes.size());
    for (const auto& edges : inputs)
        for (size_t index : edges)
            outputs[graph.edges[index].from.occurrenceIndex].push_back(index);
    const auto busEdge = [&](const SignalPathEdge& edge) {
        return graph.nodes[edge.from.occurrenceIndex].type == ProjectItemType::Bus
            && graph.nodes[edge.to.occurrenceIndex].type == ProjectItemType::Bus;
    };
    UpstreamScope scope;
    std::vector<size_t> pending;
    QSet<size_t> visited;
    const auto addNode = [&](size_t index) {
        if (visited.contains(index)) return;
        visited.insert(index);
        pending.push_back(index);
    };
    for (size_t index = 0; index < graph.nodes.size(); ++index) {
        if (graph.nodes[index].objectID != item.objectID) continue;
        addNode(index);
        if (revealScope == RevealScope::Node) {
            for (const auto& pair : graph.nodes[index].portPairs)
                for (size_t pathIndex : pair.pathIndices) scope.paths.insert(pathIndex);
        } else if (revealScope == RevealScope::Upstream) {
            scope = upstreamScope(graph, index, inputs);
        } else {
            for (size_t edgeIndex : inputs[index]) {
                if (current.edges[edgeIndex]) continue;
                scope.edges.insert(edgeIndex);
                const auto& edge = graph.edges[edgeIndex];
                if (!busEdge(edge))
                    for (size_t pathIndex : edge.pathIndices) scope.paths.insert(pathIndex);
            }
        }
        break;
    }
    for (size_t index : scope.edges) {
        addNode(graph.edges[index].from.occurrenceIndex);
        addNode(graph.edges[index].to.occurrenceIndex);
    }
    for (size_t cursor = 0; cursor < pending.size(); ++cursor) {
        if (current.nodes[pending[cursor]]) continue;
        for (size_t index : outputs[pending[cursor]]) {
            const auto& edge = graph.edges[index];
            if (busEdge(edge) || std::any_of(edge.pathIndices.begin(), edge.pathIndices.end(),
                    [&](size_t pathIndex) { return scope.paths.contains(pathIndex); }))
                addNode(edge.to.occurrenceIndex);
        }
    }
    const std::map<QUuid, CollapseReveal> noReveals;
    const auto restore = [&](const QSet<QUuid>& roots, bool bus) {
        bool changed = false;
        for (const auto& guid : roots) {
            const auto excluded = treeVisibility(graph, *project,
                bus ? QSet<QUuid>{} : QSet<QUuid>{guid},
                bus ? QSet<QUuid>{guid} : QSet<QUuid>{}, noReveals, soundsFiltered_);
            auto restored = reveals.contains(guid) ? reveals.at(guid) : CollapseReveal{};
            const auto previous = restored;
            for (size_t index : visited) {
                const auto& node = graph.nodes[index];
                if (!excluded.nodes[index] && (!bus || node.type == ProjectItemType::Bus))
                    restored.nodes.insert(node.key.objectGuid);
            }
            if (bus) {
                for (size_t index : scope.paths)
                    if (!excluded.paths[index]) restored.sources.insert(graph.paths[index].identity.sourceGuid);
            }
            if (restored == previous) continue;
            reveals[guid] = std::move(restored);
            changed = true;
        }
        return changed;
    };
    const bool hierarchyChanged = restore(hierarchy, false) || hiddenHierarchyRoots_ != hierarchy;
    const bool busesChanged = restore(buses, true) || hiddenBuses_ != buses;
    if (!hierarchyChanged && !busesChanged) return;
    cancelConnection_();
    finishNodeEdits_(true);
    hiddenHierarchyRoots_ = std::move(hierarchy);
    hiddenBuses_ = std::move(buses);
    collapseReveals_ = std::move(reveals);
    applyTreeVisibility_();
    if (hierarchyChanged) emit hierarchyCollapseChanged();
    if (busesChanged) emit busCollapseChanged();
}

void BlueprintCanvasWidget::refreshChainPins_()
{
    auto& manager = ProjectManager::instance();
    std::vector<uint32_t> hierarchyIds;
    if (activeSource_ == pathSource_.get()) {
        for (const auto& [key, node] : nodeMap_)
            if (canCollapseChain(key.id) && node->itemType()
                && supportsHierarchyCollapse(*node->itemType()))
                hierarchyIds.push_back(key.id);
    }
    const auto hierarchies = manager.findItemsByObjectID(hierarchyIds);
    QHash<uint32_t, bool> validHierarchies;
    for (size_t index = 0; index < hierarchyIds.size(); ++index)
        validHierarchies.insert(hierarchyIds[index], hierarchies[index]
            && supportsHierarchyCollapse(hierarchies[index]->type));
    for (const auto& [key, node] : nodeMap_) {
        std::optional<Qt::CheckState> state;
        if (activeSource_ == pathSource_.get() && canCollapseChain(key.id)) {
            if (node->itemType() && supportsHierarchyCollapse(*node->itemType()))
                state = validHierarchies.value(key.id) ? inputVisibility_.at(key.id) : Qt::Unchecked;
            else if (node->itemType() == ProjectItemType::Bus)
                state = busVisibility(key.id);
        }
        for (int index = 0; index < node->inputCount(); ++index)
            node->inputPin(index)->setChainVisibility(state, interactionMode() == InteractionMode::Select);
    }
}

std::optional<PlacementContext> BlueprintCanvasWidget::selectedPlacementContext() const
{
    return {};
}

QList<ProjectItem*> BlueprintCanvasWidget::selectedProjectItems() const
{
    QList<ProjectItem*> items;
    auto& manager = ProjectManager::instance();
    for (auto* selected : scene_->selectedItems()) {
        const auto* node = dynamic_cast<NodeItem*>(selected);
        if (!node || node->routeGainMode()) continue;
        auto* item = manager.findItemByObjectID(node->objectID());
        if (!item) item = manager.findBusItem(node->objectID());
        if (item && !items.contains(item)) items.push_back(item);
    }
    return items;
}

void BlueprintCanvasWidget::setSelectedProjectItems(const QList<ProjectItem*>& items,
                                                    const PlacementContext*)
{
    selectedObjectGuids_.emplace();
    for (const auto* item : items) {
        if (item) selectedObjectGuids_->insert(item->guid);
    }
    applyProjectItemSelection_();
}

void BlueprintCanvasWidget::applyProjectItemSelection_()
{
    const QSignalBlocker blocker(scene_);
    scene_->clearSelection();
    selectedPathKeys_.clear();
    selectedPathEdgeKeys_.clear();
    auto& manager = ProjectManager::instance();
    for (const auto& [key, node] : nodeMap_) {
        bool selected = false;
        if (activeSource_ == pathSource_.get() && key.occurrenceIndex) {
            const auto& pathKey = pathSource_->graph().nodes[*key.occurrenceIndex].key;
            selected = node->isVisible() && selectedObjectGuids_->contains(pathKey.objectGuid);
            if (selected) selectedPathKeys_.push_back(pathKey);
        } else {
            auto* item = manager.findItemByObjectID(node->objectID());
            if (!item) item = manager.findBusItem(node->objectID());
            selected = item && selectedObjectGuids_->contains(item->guid);
        }
        node->setSelected(selected && node->isVisible());
    }
    refreshPathFocus_();
}

void BlueprintCanvasWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (interactionMode() == InteractionMode::Pan) {
        event->accept();
        return;
    }
    const auto point = event->position().toPoint();
    if (dragSourcePin_) {
        cancelConnection_();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && firstPinAt(scene_, mapToScene(point))) {
        mousePressEvent(event);
        return;
    }
    auto* hit = itemAt(point);
    auto* node = findOwningNodeItem(hit);
    if (event->button() == Qt::LeftButton) {
        nodeDragOrigin_ = nullptr;
        nodeDragSources_.clear();
        if (node) {
            if (auto* content = qobject_cast<NodeContentEditor*>(node->contentWidget())) {
                const auto local = content->graphicsProxyWidget()->mapFromScene(mapToScene(point)).toPoint();
                if (content->beginValueEditingAt(local)) {
                    pathDragStartPositions_.clear();
                    gainDragStartPositions_.clear();
                    event->accept();
                    return;
                }
            }
            if (node->isTitleEditing()) {
                BlueprintCanvasView::mouseDoubleClickEvent(event);
                return;
            }
        }
        if (auto* proxy = dynamic_cast<QGraphicsProxyWidget*>(hit)) {
            const auto* content = qobject_cast<NodeContentEditor*>(proxy->widget());
            if (!content || !content->isDragHandleAt(proxy->mapFromScene(mapToScene(point)).toPoint())) {
                BlueprintCanvasView::mouseDoubleClickEvent(event);
                return;
            }
        }
        if (node && node->objectID() != 0) {
            const auto id = node->objectID();
            {
                const QSignalBlocker blocker(scene_);
                scene_->clearSelection();
                node->setSelected(true);
            }
            onSceneSelectionChanged();
            emit nodeDoubleClicked(id);
            event->accept();
            return;
        }
    }
    BlueprintCanvasView::mouseDoubleClickEvent(event);
}

SignalPathSource* BlueprintCanvasWidget::activeSignalPathSource() const
{
    return activeSource_ == pathSource_.get() ? pathSource_.get() : nullptr;
}

NodeItem* BlueprintCanvasWidget::signalPathNode(size_t occurrenceIndex) const
{
    const auto* source = activeSignalPathSource();
    if (!source || occurrenceIndex >= source->graph().nodes.size()) return nullptr;
    const auto& node = source->graph().nodes[occurrenceIndex];
    const NodeKind kind = node.type == ProjectItemType::Bus ? NodeKind::Bus : NodeKind::ProjectItem;
    const auto found = nodeMap_.find({kind, node.objectID, occurrenceIndex});
    return found == nodeMap_.end() ? nullptr : found->second;
}

void BlueprintCanvasWidget::setTransitionWatch(const TransitionSemantics::TransitionWatchView& view)
{
    if (transitionWatch_ == view) return;
    transitionWatch_ = view;
    viewport()->update();
}

}
