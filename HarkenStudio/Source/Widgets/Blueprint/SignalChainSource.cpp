
#include "SignalChainSource.h"

#include "BlueprintConnectCommand.h"
#include "BlueprintEdgeKind.h"
#include "Core/Components/VBus.h"
#include "Core/Components/VMixNode.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/PlayablePropertyAuthoringV1.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectTypes.h"
#include "Core/ToastManager.h"
#include "Core/UndoCommands.h"
#include "NodeItem.h"

#include <QJsonObject>
#include <QPointer>
#include <QSettings>
#include <QString>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace VNX {

struct SignalChainSource::ViewCache {
    SignalChainView view;
};

namespace {

struct LayoutColumn {
    BlueprintNodeRole          role;
    int                        depth = 0;
    int                        branch = 0;
    std::vector<BlueprintNodeDesc> nodes;
};

inline bool layerOn(const std::array<bool, 3>& layers, NodeGraphLayer l)
{
    return layers[static_cast<size_t>(l)];
}

float contentHeight(const BlueprintNodeDesc& desc)
{
    return NodeItem::heightForContent(desc.content, false, desc.waveformMode, true);
}

float descHeight(const BlueprintNodeDesc& d)
{
    return d.height > 0.0f ? d.height : 60.0f;
}

// Resolves a non-bus project item; the bus bucket shares the objectID space
// but is edited through the bus branch, so it is skipped here.
ProjectItem* findNonBusItem(ProjectData* project, uint32_t id)
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
        if (binding->parentFolder ==
            QLatin1String(ItemTypeCatalog::kBussesFolderName)) continue;
        if (auto* hit = walk(binding->rootItem.get())) return hit;
    }
    return nullptr;
}

BlueprintNodeDesc descFromSignalChainNode(const SignalChainNode& n,
                                          BlueprintNodeRole role)
{
    BlueprintNodeDesc d;
    d.kind            = BlueprintNodeKind::ProjectItem;
    d.id              = n.objectID;
    d.title           = n.name;
    d.role            = role;
    d.itemType        = n.itemType;
    d.outputBusOverride = n.outputBusOverride;
    d.content.volume  = n.volume;
    d.content.pitch   = n.pitch;
    d.draggable       = false;
    if (role == BlueprintNodeRole::Source ||
        role == BlueprintNodeRole::Self) {
        if (!n.mediaPath.isEmpty()) {
            d.mediaPath    = n.mediaPath;
            d.waveformMode = true;
        }
    }
    d.height = contentHeight(d);
    return d;
}

BlueprintNodeDesc descFromBusChainNode(const BusChainNode& b)
{
    BlueprintNodeDesc d;
    d.kind     = BlueprintNodeKind::Bus;
    d.id       = b.busID;
    d.title    = b.name;
    d.itemType = ProjectItem::Type::Bus;
    d.role   = (b.type == QLatin1String("Receive")) ? BlueprintNodeRole::ReceiveBus
                                                    : BlueprintNodeRole::Bus;
    d.content.volume = b.volume;
    d.draggable  = false;
    d.height     = contentHeight(d);
    return d;
}

// Send levels are per-connection and cards can be shared by several edges,
// so the level lives on the edge label, never in the card title.
BlueprintNodeDesc descSendTarget(const SendViewInfo& send)
{
    BlueprintNodeDesc d;
    d.kind     = BlueprintNodeKind::Bus;
    d.id       = send.targetBusID;
    d.title    = send.targetBusName;
    d.itemType = ProjectItem::Type::Bus;
    d.role = BlueprintNodeRole::ReceiveBus;
    d.draggable = false;
    d.height    = contentHeight(d);
    return d;
}

std::vector<LayoutColumn>
composeColumns(const SignalChainView& view, const std::array<bool, 3>& layers)
{
    std::vector<LayoutColumn> cols;

    if (!view.children.empty() && layerOn(layers, NodeGraphLayer::Children)) {
        LayoutColumn col;
        col.role = BlueprintNodeRole::Source;
        col.depth = 0;
        for (const auto& child : view.children) {
            col.nodes.push_back(descFromSignalChainNode(child, BlueprintNodeRole::Source));
        }
        cols.push_back(std::move(col));
    }

    if (view.self.objectID != 0) {
        LayoutColumn col;
        col.role = BlueprintNodeRole::Self;
        col.depth = 1;
        col.nodes.push_back(descFromSignalChainNode(view.self, BlueprintNodeRole::Self));
        cols.push_back(std::move(col));
    }

    if (layerOn(layers, NodeGraphLayer::Parents)) {
        const int placementBranch = view.organizationChain.empty() ? 0 : -1;
        const int organizationBranch = view.placementChain.empty() ? 0 : 1;
        for (size_t index = 0; index < view.placementChain.size(); ++index) {
            const auto& parent = view.placementChain[index];
            LayoutColumn col; col.role = BlueprintNodeRole::Parent;
            col.depth = 2 + static_cast<int>(index);
            col.branch = placementBranch;
            col.nodes.push_back(descFromSignalChainNode(parent, BlueprintNodeRole::Parent));
            cols.push_back(std::move(col));
        }
        for (size_t index = 0; index < view.organizationChain.size(); ++index) {
            const auto& parent = view.organizationChain[index];
            LayoutColumn col; col.role = BlueprintNodeRole::Parent;
            col.depth = 2 + static_cast<int>(index);
            col.branch = organizationBranch;
            col.nodes.push_back(descFromSignalChainNode(parent, BlueprintNodeRole::Parent));
            cols.push_back(std::move(col));
        }
    }

    if (!view.busChain.empty() && layerOn(layers, NodeGraphLayer::Bus)) {
        const int busDepth = 2 + static_cast<int>(
            std::max(view.placementChain.size(), view.organizationChain.size()));
        for (size_t index = 0; index < view.busChain.size(); ++index) {
            const auto& bus = view.busChain[index];
            LayoutColumn col; col.role = BlueprintNodeRole::Bus;
            col.depth = busDepth + static_cast<int>(index);
            col.nodes.push_back(descFromBusChainNode(bus));
            cols.push_back(std::move(col));
        }
    }

    return cols;
}

void layoutColumns(std::vector<LayoutColumn>& cols)
{
    const float colStride = SignalChainSource::kNodeWidth +
                            SignalChainSource::kColumnSpacing;
    for (auto& col : cols) {
        float totalHeight = 0.0f;
        for (auto const& d : col.nodes) totalHeight += descHeight(d);
        if (col.nodes.size() > 1) {
            totalHeight += static_cast<float>(col.nodes.size() - 1) *
                           SignalChainSource::kRowSpacing;
        }
        const float x = static_cast<float>(col.depth) * colStride;
        float y = -totalHeight / 2.0f + static_cast<float>(col.branch) * 180.0f;
        for (auto& desc : col.nodes) {
            desc.position    = QPointF(x, y);
            desc.hasPosition = true;
            y += descHeight(desc) + SignalChainSource::kRowSpacing;
        }
    }
}

// The canvas maps every emitted node by {kind, id}, so a bus may only appear
// as one card. usedBusIDs carries the bus IDs already materialized (the chain
// itself, then send targets); duplicates keep their edges but skip the card.
std::vector<BlueprintNodeDesc>
buildSendTargetNodes(const SignalChainView& view,
                     const std::array<bool, 3>& layers,
                     const std::vector<LayoutColumn>& cols,
                     std::unordered_set<uint32_t>& usedBusIDs)
{
    std::vector<BlueprintNodeDesc> out;
    if (!layerOn(layers, NodeGraphLayer::Bus)) return out;

    std::vector<std::pair<uint32_t, QPointF>> busPositions;
    for (auto const& col : cols) {
        if (col.role != BlueprintNodeRole::Bus) continue;
        for (auto const& d : col.nodes) {
            busPositions.emplace_back(d.id, d.position);
        }
    }

    auto findBusPos = [&](uint32_t id, QPointF& out) -> bool {
        for (auto const& kv : busPositions) {
            if (kv.first == id) { out = kv.second; return true; }
        }
        return false;
    };

    for (auto const& busData : view.busChain) {
        if (busData.sends.empty()) continue;
        QPointF busPos;
        if (!findBusPos(busData.busID, busPos)) continue;

        int sendIndex = 0;
        for (auto const& send : busData.sends) {
            if (!usedBusIDs.insert(send.targetBusID).second) continue;
            BlueprintNodeDesc d = descSendTarget(send);
            const float sendH = descHeight(d);
            const float sendX = busPos.x() +
                                SignalChainSource::kNodeWidth * 0.6f;
            const float sendY = busPos.y() - sendH -
                                SignalChainSource::kRowSpacing -
                                sendIndex * (sendH +
                                             SignalChainSource::kRowSpacing);
            d.position    = QPointF(sendX, sendY);
            d.hasPosition = true;
            out.push_back(d);
            ++sendIndex;
        }
    }
    return out;
}

bool busInChain(const SignalChainView& view, uint32_t busID)
{
    for (auto const& b : view.busChain) {
        if (b.busID == busID) return true;
    }
    return false;
}

QString incomingLevelText(float linearLevel)
{
    if (linearLevel <= 0.0f) return QStringLiteral("-inf dB");
    const float db = 20.0f * std::log10(linearLevel);
    if (db <= -96.0f) return QStringLiteral("-inf dB");
    return QString::number(db, 'f', 1) + QStringLiteral(" dB");
}

std::vector<BlueprintNodeDesc>
buildIncomingSendNodes(const SignalChainView& view,
                       const std::array<bool, 3>& layers,
                       const std::vector<LayoutColumn>& cols,
                       std::unordered_set<uint32_t>& usedBusIDs)
{
    std::vector<BlueprintNodeDesc> out;
    if (!layerOn(layers, NodeGraphLayer::Bus)) return out;
    if (view.incomingSends.empty() || view.busChain.empty()) return out;

    const uint32_t headID = view.busChain.front().busID;
    QPointF headPos;
    float headH = 60.0f;
    bool headFound = false;
    for (auto const& col : cols) {
        if (col.role != BlueprintNodeRole::Bus) continue;
        for (auto const& d : col.nodes) {
            if (d.id == headID) { headPos = d.position; headH = descHeight(d); headFound = true; break; }
        }
        if (headFound) break;
    }
    if (!headFound) return out;

    int inIndex = 0;
    for (auto const& in : view.incomingSends) {
        if (!usedBusIDs.insert(in.fromBusID).second) continue;
        BlueprintNodeDesc d;
        d.kind      = BlueprintNodeKind::Bus;
        d.id        = in.fromBusID;
        d.title     = in.fromName;
        d.itemType  = ProjectItem::Type::Bus;
        d.role      = BlueprintNodeRole::Bus;
        d.draggable = false;
        d.height    = contentHeight(d);
        const float inH = descHeight(d);
        const float inX = headPos.x() - SignalChainSource::kNodeWidth * 0.6f;
        const float inY = headPos.y() + headH +
                          SignalChainSource::kRowSpacing +
                          inIndex * (inH +
                                     SignalChainSource::kRowSpacing);
        d.position    = QPointF(inX, inY);
        d.hasPosition = true;
        out.push_back(d);
        ++inIndex;
    }
    return out;
}

}

SignalChainSource::SignalChainSource(QObject* parent)
    : IBlueprintSource(parent)
    , viewCache_(new ViewCache)
{
    // The viewed item is held as a raw pointer; when it (or an ancestor) is
    // deleted, drop the view instead of rebuilding from a detached subtree.
    connect(&ProjectManager::instance(), &ProjectManager::itemDeleted,
            this, [this](ProjectItem* item) {
        if (currentItem_ == nullptr || item == nullptr) return;
        std::function<bool(ProjectItem*)> contains = [&](ProjectItem* n) -> bool {
            if (!n) return false;
            if (n == currentItem_) return true;
            for (auto const& c : n->children) {
                if (contains(c.get())) return true;
            }
            return false;
        };
        if (contains(item)) clear();
    });
}

SignalChainSource::~SignalChainSource()
{
    delete viewCache_;
    viewCache_ = nullptr;
}

const SignalChainView& SignalChainSource::cachedView() const
{
    return viewCache_->view;
}

void SignalChainSource::loadItem(ProjectItem* item)
{
    currentItem_  = item;
    currentItemGuid_ = item ? item->guid : QUuid{};
    currentBusID_ = 0;
    currentItemObjectID_ = (item != nullptr) ? item->objectID : 0;
    placementContext_.reset();
    rebuildViewForCurrentTarget();
    emit sourceChanged();
}

void SignalChainSource::loadItem(ProjectItem* item,
                                 const PlacementContext& placement)
{
    currentItem_ = item;
    currentItemGuid_ = item ? item->guid : QUuid{};
    currentBusID_ = 0;
    currentItemObjectID_ = item ? item->objectID : 0;
    placementContext_ = placement;
    rebuildViewForCurrentTarget();
    emit sourceChanged();
}

void SignalChainSource::loadBus(uint32_t busID)
{
    currentItem_         = nullptr;
    currentItemGuid_     = QUuid{};
    currentItemObjectID_ = 0;
    currentBusID_        = busID;
    placementContext_.reset();
    rebuildViewForCurrentTarget();
    emit sourceChanged();
}

void SignalChainSource::clear()
{
    currentItem_         = nullptr;
    currentItemGuid_     = QUuid{};
    currentItemObjectID_ = 0;
    currentBusID_        = 0;
    placementContext_.reset();
    viewCache_->view = SignalChainView{};
    emit sourceChanged();
}

void SignalChainSource::rebuildViewForCurrentTarget()
{
    auto& pm = ProjectManager::instance();
    if (!currentItemGuid_.isNull()) {
        currentItem_ = pm.findItemByGuid(currentItemGuid_);
        currentItemObjectID_ = currentItem_ ? currentItem_->objectID : 0;
        if (!currentItem_) {
            viewCache_->view = SignalChainView{};
            return;
        }
        viewCache_->view = placementContext_.has_value()
            ? pm.buildSignalChainView(currentItem_, *placementContext_)
            : pm.buildSignalChainView(currentItem_);
        return;
    }
    if (currentBusID_ != 0) {
        viewCache_->view = pm.buildSignalChainViewForBus(currentBusID_);
        return;
    }
    viewCache_->view = SignalChainView{};
}

void SignalChainSource::refreshView()
{
    rebuildViewForCurrentTarget();
}

void SignalChainSource::setCachedView(const SignalChainView& view)
{
    viewCache_->view = view;
    emit sourceChanged();
}

void SignalChainSource::loadPersistedLayerVisibility()
{
    QSettings s;
    auto read = [&s](const char* key, const char* legacy) {
        if (s.contains(QLatin1String(key))) return s.value(QLatin1String(key)).toBool();
        return s.value(QLatin1String(legacy), true).toBool();
    };
    const bool children = read("Blueprint/LayerChildren", "NodeGraph/LayerChildren");
    const bool parents = read("Blueprint/LayerParents", "NodeGraph/LayerParents");
    const bool bus = read("Blueprint/LayerBus", "NodeGraph/LayerBus");
    setLayerVisible(NodeGraphLayer::Children, children);
    setLayerVisible(NodeGraphLayer::Parents, parents);
    setLayerVisible(NodeGraphLayer::Bus, bus);
}

void SignalChainSource::persistLayerVisibility() const
{
    QSettings s;
    s.setValue(QStringLiteral("Blueprint/LayerChildren"),
               isLayerVisible(NodeGraphLayer::Children));
    s.setValue(QStringLiteral("Blueprint/LayerParents"),
               isLayerVisible(NodeGraphLayer::Parents));
    s.setValue(QStringLiteral("Blueprint/LayerBus"),
               isLayerVisible(NodeGraphLayer::Bus));
}

void SignalChainSource::setLayerVisible(NodeGraphLayer layer, bool visible)
{
    const auto idx = static_cast<size_t>(layer);
    if (idx >= layerVisibility_.size()) return;
    if (layerVisibility_[idx] == visible) return;
    layerVisibility_[idx] = visible;
    persistLayerVisibility();
    emit sourceChanged();
}

bool SignalChainSource::isLayerVisible(NodeGraphLayer layer) const
{
    const auto idx = static_cast<size_t>(layer);
    if (idx >= layerVisibility_.size()) return true;
    return layerVisibility_[idx];
}

std::vector<BlueprintNodeDesc> SignalChainSource::buildNodes()
{
    return nodesFromView(viewCache_->view, layerVisibility_);
}

std::vector<BlueprintEdgeDesc> SignalChainSource::buildEdges()
{
    return edgesFromView(viewCache_->view, layerVisibility_);
}

std::vector<BlueprintNodeDesc>
SignalChainSource::nodesFromView(const SignalChainView& view,
                                 const std::array<bool, 3>& layers)
{
    auto cols = composeColumns(view, layers);
    layoutColumns(cols);

    std::vector<BlueprintNodeDesc> out;
    for (auto const& col : cols) {
        for (auto const& d : col.nodes) {
            out.push_back(d);
        }
    }
    std::unordered_set<uint32_t> usedBusIDs;
    for (auto const& b : view.busChain) usedBusIDs.insert(b.busID);
    auto sendTargets = buildSendTargetNodes(view, layers, cols, usedBusIDs);
    for (auto const& d : sendTargets) out.push_back(d);
    auto incoming = buildIncomingSendNodes(view, layers, cols, usedBusIDs);
    for (auto const& d : incoming) out.push_back(d);
    return out;
}

std::vector<BlueprintEdgeDesc>
SignalChainSource::edgesFromView(const SignalChainView& view,
                                 const std::array<bool, 3>& layers)
{
    std::vector<BlueprintEdgeDesc> out;
    auto cols = composeColumns(view, layers);

    auto findItemNode = [&cols](uint32_t objectID) -> const BlueprintNodeDesc* {
        for (auto const& col : cols) {
            if (col.role == BlueprintNodeRole::Bus) continue;
            for (auto const& d : col.nodes) {
                if (d.kind == BlueprintNodeKind::ProjectItem &&
                    d.id == objectID)
                    return &d;
            }
        }
        return nullptr;
    };
    auto findBusNode = [&cols](uint32_t busID) -> const BlueprintNodeDesc* {
        for (auto const& col : cols) {
            if (col.role != BlueprintNodeRole::Bus) continue;
            for (auto const& d : col.nodes)
                if (d.kind == BlueprintNodeKind::Bus && d.id == busID)
                    return &d;
        }
        return nullptr;
    };
    auto pushEdge = [&out](const BlueprintNodeDesc& from,
                           const BlueprintNodeDesc& to,
                           BlueprintEdgeKind kind,
                           BlueprintNodeRole styleRole,
                           const QString& label = QString()) {
        BlueprintEdgeDesc edge;
        edge.fromKind = from.kind;
        edge.fromId = from.id;
        edge.toKind = to.kind;
        edge.toId = to.id;
        edge.edgeKind = static_cast<uint8_t>(kind);
        edge.label = label;
        edge.styleRole = styleRole;
        out.push_back(std::move(edge));
    };

    const BlueprintNodeDesc* self = findItemNode(view.self.objectID);
    if (self && layerOn(layers, NodeGraphLayer::Children)) {
        for (const auto& child : view.children) {
            if (const BlueprintNodeDesc* node = findItemNode(child.objectID))
                pushEdge(*node, *self, BlueprintEdgeKind::Containment,
                         BlueprintNodeRole::Self);
        }
    }

    auto appendChain = [&](const std::vector<SignalChainNode>& chain,
                           BlueprintEdgeKind kind) {
        const BlueprintNodeDesc* previous = self;
        if (!previous || !layerOn(layers, NodeGraphLayer::Parents)) return;
        for (const SignalChainNode& entry : chain) {
            const BlueprintNodeDesc* current = findItemNode(entry.objectID);
            if (!current) continue;
            pushEdge(*previous, *current, kind, BlueprintNodeRole::Parent);
            previous = current;
        }
    };
    appendChain(view.placementChain, BlueprintEdgeKind::Placement);
    appendChain(view.organizationChain, BlueprintEdgeKind::Organization);

    if (layerOn(layers, NodeGraphLayer::Bus) && !view.busChain.empty()) {
        const BlueprintNodeDesc* busSlotOwner =
            findItemNode(view.busSlotOwnerObjectID);
        if (!busSlotOwner) {
            if (view.busSlotOwnerObjectID == 0) {
                if (!view.organizationChain.empty())
                    busSlotOwner = findItemNode(view.organizationChain.back().objectID);
                if (!busSlotOwner && !view.placementChain.empty())
                    busSlotOwner = findItemNode(view.placementChain.back().objectID);
            }
            if (!busSlotOwner) busSlotOwner = self;
        }
        const BlueprintNodeDesc* firstBus = findBusNode(view.busChain.front().busID);
        if (busSlotOwner && firstBus) {
            QString via;
            for (const auto& source : view.slotSources) {
                if (source.slot == QLatin1String("outputBus")) {
                    via = source.via;
                    break;
                }
            }
            pushEdge(*busSlotOwner, *firstBus,
                     busSlotOwner->outputBusOverride
                         ? BlueprintEdgeKind::Override
                         : BlueprintEdgeKind::BusRoute,
                     firstBus->role, via);
        }
        for (size_t index = 0; index + 1 < view.busChain.size(); ++index) {
            const BlueprintNodeDesc* from = findBusNode(view.busChain[index].busID);
            const BlueprintNodeDesc* to = findBusNode(view.busChain[index + 1].busID);
            if (from && to)
                pushEdge(*from, *to, BlueprintEdgeKind::BusRoute, to->role);
        }
    }

    if (layerOn(layers, NodeGraphLayer::Bus)) {
        for (auto const& busData : view.busChain) {
            for (auto const& send : busData.sends) {
                BlueprintEdgeDesc e;
                e.fromKind   = BlueprintNodeKind::Bus;
                e.fromId     = busData.busID;
                e.toKind     = BlueprintNodeKind::Bus;
                e.toId       = send.targetBusID;
                e.edgeKind   = static_cast<uint8_t>(BlueprintEdgeKind::Send);
                e.label      = (send.sendLevel != 0.0f)
                                   ? QStringLiteral("Send (%1 dB)")
                                         .arg(send.sendLevel, 0, 'f', 1)
                                   : QStringLiteral("Send");
                e.styleRole  = BlueprintNodeRole::ReceiveBus;
                out.push_back(e);
            }
        }

        if (!view.busChain.empty()) {
            const uint32_t headID = view.busChain.front().busID;
            for (auto const& in : view.incomingSends) {
                if (busInChain(view, in.fromBusID)) continue;
                BlueprintEdgeDesc e;
                e.fromKind  = BlueprintNodeKind::Bus;
                e.fromId    = in.fromBusID;
                e.toKind    = BlueprintNodeKind::Bus;
                e.toId      = headID;
                e.edgeKind  = static_cast<uint8_t>(BlueprintEdgeKind::Send);
                e.label     = QStringLiteral("Send (%1)")
                                  .arg(incomingLevelText(in.level));
                e.styleRole = BlueprintNodeRole::ReceiveBus;
                out.push_back(e);
            }
        }
    }

    return out;
}

bool SignalChainSource::onConnect(uint32_t fromId, uint32_t toId,
                                  const QString& edgeType,
                                  const QString& fromKind,
                                  const QString& toKind)
{
    auto r = doConnect(fromId, toId, edgeType, fromKind, toKind);
    if (r.success) {
        lastError_.clear();
        rebuildViewForCurrentTarget();
        emit sourceChanged();
    } else {
        lastError_ = r.error;
    }
    return r.success;
}

bool SignalChainSource::onDisconnect(uint32_t fromId, uint32_t toId,
                                     const QString& edgeType,
                                     const QString& fromKind,
                                     const QString& toKind)
{
    const bool ok = doDisconnect(fromId, toId, edgeType, fromKind, toKind);
    if (ok) {
        lastError_.clear();
        rebuildViewForCurrentTarget();
        emit sourceChanged();
    } else {
        lastError_ = QStringLiteral("no_matching_edge");
    }
    return ok;
}

// Routing semantics mirror the retired topology editor: item->item re-parents
// (containment), item->bus routes the item's main output (bus_route, upgraded
// to an override when the item already lives in a parent container), bus->bus
// creates a send. All edits go through the undo stack.
BlueprintConnectResult
SignalChainSource::doConnect(uint32_t fromId, uint32_t toId,
                             const QString& edgeType,
                             const QString& fromKind,
                             const QString& toKind)
{
    BlueprintConnectResult r;
    auto& pm = ProjectManager::instance();
    auto* project = pm.currentProject();
    if (!project) { r.error = QStringLiteral("no_project"); return r; }
    if (fromId == toId) { r.error = QStringLiteral("self_loop"); return r; }
    if (fromId == 0 || toId == 0) { r.error = QStringLiteral("invalid_args"); return r; }

    ProjectItem* fromItem = nullptr;
    ProjectItem* fromBus  = nullptr;
    const QString fk = fromKind.toLower();
    if (fk == QStringLiteral("bus")) {
        fromBus = pm.findBusItem(fromId);
    } else if (fk == QStringLiteral("item")) {
        fromItem = findNonBusItem(project, fromId);
    } else {
        fromItem = findNonBusItem(project, fromId);
        if (!fromItem) fromBus = pm.findBusItem(fromId);
    }
    const bool fromIsBus = (fromItem == nullptr && fromBus != nullptr);
    if (!fromItem && !fromIsBus) { r.error = QStringLiteral("from_not_found"); return r; }

    ProjectItem* toItem = nullptr;
    ProjectItem* toBus  = nullptr;
    const QString tk = toKind.toLower();
    if (tk == QStringLiteral("bus")) {
        toBus = pm.findBusItem(toId);
    } else if (tk == QStringLiteral("item")) {
        toItem = findNonBusItem(project, toId);
    } else {
        toItem = findNonBusItem(project, toId);
        if (!toItem) toBus = pm.findBusItem(toId);
    }
    if (!toItem && !toBus) { r.error = QStringLiteral("to_not_found"); return r; }

    QString eff = edgeType.isEmpty() ? QStringLiteral("normal") : edgeType.toLower();
    if (eff == QStringLiteral("normal") && toBus != nullptr) {
        // Bus->bus defaults to a send; reparenting requires explicit bus_route.
        eff = fromIsBus ? QStringLiteral("send") : QStringLiteral("bus_route");
    }

    auto& undoMgr = UndoRedoManager::instance();

    if (fromIsBus) {
        if (eff == QStringLiteral("send")) {
            if (!toBus) { r.error = QStringLiteral("send_target_not_bus"); return r; }
            if (pm.sendWouldCycle(fromId, toId)) {
                ToastManager::instance().showToast(
                    tr("Send would create a cycle; connection rejected"),
                    3000, ToastType::Warning);
                r.error = QStringLiteral("send_cycle");
                return r;
            }
            const auto* fromMix = fromBus->comp<VMixNode>();
            const bool dupSend = fromMix && std::any_of(
                fromMix->sends.begin(), fromMix->sends.end(),
                [&](const VNX::Send& s) { return s.targetBusID == toId; });
            if (dupSend) {
                r.error = QStringLiteral("duplicate");
                r.effectiveType = QStringLiteral("send");
                return r;
            }
            if (!pm.sendTargetSelectable(fromId, toId)) {
                ToastManager::instance().showToast(
                    tr("Send target is the main output bus; connection rejected"),
                    3000, ToastType::Warning);
                r.error = QStringLiteral("send_rejected");
                return r;
            }
            undoMgr.beginMacro(QStringLiteral("Blueprint Connect (Send)"));
            undoMgr.push(new BlueprintSendConnectCommand(fromId, toId));
            undoMgr.endMacro();
            r.success = true;
            r.effectiveType = QStringLiteral("send");
            return r;
        }
        if (eff == QStringLiteral("bus_route")) {
            if (!toBus) { r.error = QStringLiteral("invalid_bus_target"); return r; }
            const uint32_t oldParentId =
                fromBus->parent ? fromBus->parent->objectID : 0;
            QString oldParentName;
            if (oldParentId != 0 && oldParentId != toId) {
                if (auto* op = pm.findBusItem(oldParentId)) oldParentName = op->name;
            }
            undoMgr.beginMacro(QStringLiteral("Blueprint Connect (Bus Route)"));
            const bool moved = pm.moveItem(fromBus, toBus);
            undoMgr.endMacro();
            if (!moved) {
                r.error = QStringLiteral("invalid_bus_target");
                return r;
            }
            if (!oldParentName.isEmpty()) {
                ToastManager::instance().showToast(
                    tr("Previous bus parent %1 was disconnected.").arg(oldParentName),
                    3000, ToastType::Info);
            }
            r.success = true;
            r.effectiveType = QStringLiteral("bus_route");
            return r;
        }
        r.error = QStringLiteral("invalid_bus_target");
        return r;
    }

    if (eff == QStringLiteral("normal")) {
        if (!toItem) { r.error = QStringLiteral("containment_target_invalid"); return r; }
        if (fromItem->parent == toItem) {
            r.error = QStringLiteral("duplicate");
            r.effectiveType = QStringLiteral("containment");
            return r;
        }
        const bool fieldGroups = PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(fromItem->type);
        if (!fieldGroups)
            undoMgr.beginMacro(QStringLiteral("Blueprint Connect (Containment)"));
        const bool moved = pm.moveItem(fromItem, toItem);
        if (moved && !fieldGroups) {
            undoMgr.push(new BlueprintConnectCommand(
                fromId, toId, BlueprintConnectCommand::Effective::Containment));
        }
        if (!fieldGroups) undoMgr.endMacro();
        if (!moved) {
            r.error = QStringLiteral("containment_target_invalid");
            return r;
        }
        r.success = true;
        r.effectiveType = QStringLiteral("containment");
        return r;
    }

    if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(fromItem->type)
        && (eff == QStringLiteral("bus_route") || eff == QStringLiteral("override"))) {
        if (!toBus) { r.error = QStringLiteral("bus_route_target_not_bus"); return r; }
        auto* command = new BlueprintConnectCommand(
            fromId, toId, BlueprintConnectCommand::Effective::BusRoute);
        if (command->isObsolete()) {
            r.error = command->error();
            delete command;
            return r;
        }
        undoMgr.push(command);
        r.success = true;
        r.effectiveType = QStringLiteral("bus_route");
        return r;
    }

    if (eff == QStringLiteral("bus_route")) {
        if (!toBus) { r.error = QStringLiteral("bus_route_target_not_bus"); return r; }
        if (!pm.outputBusSelectable(fromId, toId)) {
            ToastManager::instance().showToast(
                tr("Target bus already receives a send from this item; connection rejected"),
                3000, ToastType::Warning);
            r.error = QStringLiteral("output_rejected");
            return r;
        }
        const bool hasParent =
            (fromItem->parent != nullptr && fromItem->parent->objectID != 0);
        const auto* fromMc = fromItem->comp<VMixNode>();
        const uint32_t curBusId = fromMc ? fromMc->outputBusId : 0;
        if (hasParent) {
            if (curBusId == toId && fromItem->isOutputBusOverride()) {
                r.error = QStringLiteral("duplicate");
                r.effectiveType = QStringLiteral("override");
                return r;
            }
            undoMgr.beginMacro(QStringLiteral("Blueprint Connect (Override Auto-Upgrade)"));
            undoMgr.push(new BlueprintConnectCommand(
                fromId, toId, BlueprintConnectCommand::Effective::Override));
            undoMgr.endMacro();
            ToastManager::instance().showToast(
                tr("Existing parent container: routing via main output override."),
                3000, ToastType::Info);
            r.success = true;
            r.effectiveType = QStringLiteral("override");
            return r;
        }
        if (curBusId == toId && !fromItem->isOutputBusOverride()) {
            r.error = QStringLiteral("duplicate");
            r.effectiveType = QStringLiteral("bus_route");
            return r;
        }
        undoMgr.beginMacro(QStringLiteral("Blueprint Connect (Bus Route)"));
        undoMgr.push(new BlueprintConnectCommand(
            fromId, toId, BlueprintConnectCommand::Effective::BusRoute));
        undoMgr.endMacro();
        r.success = true;
        r.effectiveType = QStringLiteral("bus_route");
        return r;
    }

    if (eff == QStringLiteral("override")) {
        if (!toBus) { r.error = QStringLiteral("override_target_not_bus"); return r; }
        if (!pm.outputBusSelectable(fromId, toId)) {
            ToastManager::instance().showToast(
                tr("Target bus already receives a send from this item; connection rejected"),
                3000, ToastType::Warning);
            r.error = QStringLiteral("output_rejected");
            return r;
        }
        const auto* fromMc = fromItem->comp<VMixNode>();
        if ((fromMc ? fromMc->outputBusId : 0) == toId && fromItem->isOutputBusOverride()) {
            r.error = QStringLiteral("duplicate");
            r.effectiveType = QStringLiteral("override");
            return r;
        }
        undoMgr.beginMacro(QStringLiteral("Blueprint Connect (Override)"));
        undoMgr.push(new BlueprintConnectCommand(
            fromId, toId, BlueprintConnectCommand::Effective::Override));
        undoMgr.endMacro();
        r.success = true;
        r.effectiveType = QStringLiteral("override");
        return r;
    }

    if (eff == QStringLiteral("send")) {
        r.error = QStringLiteral("send_from_not_bus");
        return r;
    }

    r.error = QStringLiteral("unknown_edge_type");
    return r;
}

bool SignalChainSource::doDisconnect(uint32_t fromId, uint32_t toId,
                                     const QString& edgeType,
                                     const QString& fromKind,
                                     const QString& toKind)
{
    auto& pm = ProjectManager::instance();
    auto* project = pm.currentProject();
    if (!project) return false;
    if (fromId == 0 || toId == 0) return false;

    ProjectItem* fromItem = nullptr;
    ProjectItem* fromBus  = nullptr;
    const QString fk = fromKind.toLower();
    if (fk == QStringLiteral("bus")) {
        fromBus = pm.findBusItem(fromId);
    } else if (fk == QStringLiteral("item")) {
        fromItem = findNonBusItem(project, fromId);
    } else {
        fromItem = findNonBusItem(project, fromId);
        if (!fromItem) fromBus = pm.findBusItem(fromId);
    }
    if (!fromItem && !fromBus) return false;

    ProjectItem* toBus = nullptr;
    if (toKind.toLower() != QStringLiteral("item")) {
        toBus = pm.findBusItem(toId);
    }

    QString kind = edgeType.toLower();
    auto& undoMgr = UndoRedoManager::instance();

    if (fromBus != nullptr && fromItem == nullptr) {
        if (kind.isEmpty() || kind == QStringLiteral("send")) {
            const auto* fromMix = fromBus->comp<VMixNode>();
            const bool hasSend = fromMix && std::any_of(
                fromMix->sends.begin(), fromMix->sends.end(),
                [&](const VNX::Send& s) { return s.targetBusID == toId; });
            if (hasSend) {
                undoMgr.beginMacro(QStringLiteral("Blueprint Disconnect (Send)"));
                undoMgr.push(new BlueprintSendDisconnectCommand(fromId, toId));
                undoMgr.endMacro();
                return true;
            }
        }
        return false;
    }

    if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(fromItem->type)) {
        if (kind.isEmpty()) {
            if (toBus) kind = QStringLiteral("bus_route");
            else if (fromItem->parent && fromItem->parent->objectID == toId)
                kind = QStringLiteral("containment");
        }
        if (kind == QStringLiteral("containment")) {
            if (ProjectItem* packRoot = findPackRoot(fromItem);
                packRoot && packRoot != fromItem)
                return pm.moveItem(fromItem, packRoot);
            return false;
        }
        if (kind != QStringLiteral("bus_route") && kind != QStringLiteral("override"))
            return false;
        auto* command = new BlueprintDisconnectCommand(
            fromId, toId, BlueprintDisconnectCommand::Kind::BusRoute);
        if (command->isObsolete()) {
            delete command;
            return false;
        }
        undoMgr.push(command);
        return true;
    }

    if (kind.isEmpty()) {
        const auto* fromMc = fromItem->comp<VMixNode>();
        if (toBus != nullptr && (fromMc ? fromMc->outputBusId : 0) == toId) {
            kind = fromItem->isOutputBusOverride() ? QStringLiteral("override")
                                                   : QStringLiteral("bus_route");
        } else if (fromItem->parent && fromItem->parent->objectID == toId) {
            kind = QStringLiteral("containment");
        }
    }

    BlueprintDisconnectCommand::Kind k = BlueprintDisconnectCommand::Kind::Containment;
    if (kind == QStringLiteral("bus_route"))     k = BlueprintDisconnectCommand::Kind::BusRoute;
    else if (kind == QStringLiteral("override")) k = BlueprintDisconnectCommand::Kind::Override;
    else if (kind != QStringLiteral("containment")) return false;

    undoMgr.beginMacro(QStringLiteral("Blueprint Disconnect"));
    undoMgr.push(new BlueprintDisconnectCommand(fromId, toId, k));
    if (k == BlueprintDisconnectCommand::Kind::Containment) {
        if (ProjectItem* packRoot = findPackRoot(fromItem);
            packRoot != nullptr && packRoot != fromItem) {
            pm.moveItem(fromItem, packRoot);
        }
    }
    undoMgr.endMacro();
    return true;
}

bool SignalChainSource::onDelete(BlueprintNodeKind , uint32_t )
{
    return false;
}

}
