
#pragma once

#include "IBlueprintSource.h"
#include "BlueprintEditTypes.h"
#include "Core/PlacementContext.h"
#include "NodeGraphLayer.h"
#include "NodeItem.h"

#include <QJsonObject>
#include <QPointF>
#include <QString>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace VNX {

struct ProjectItem;
struct SignalChainView;
class  VBus;

class SignalChainSource : public IBlueprintSource {
    Q_OBJECT
public:
    explicit SignalChainSource(QObject* parent = nullptr);
    ~SignalChainSource() override;

    Scope                          scope() const override { return Scope::SingleObject; }
    std::vector<BlueprintNodeDesc> buildNodes() override;
    std::vector<BlueprintEdgeDesc> buildEdges() override;
    bool                           supportsEditing() const override { return true; }
    bool onConnect(uint32_t fromId, uint32_t toId, const QString& edgeType,
                   const QString& fromKind, const QString& toKind) override;
    bool onDisconnect(uint32_t fromId, uint32_t toId, const QString& edgeType,
                      const QString& fromKind, const QString& toKind) override;
    bool onDelete(BlueprintNodeKind kind, uint32_t id) override;

    void                   loadItem(ProjectItem* item);
    void                   loadItem(ProjectItem* item, const PlacementContext& placement);
    void                   clear();
    ProjectItem*           currentItem() const { return currentItem_; }
    uint32_t               currentItemObjectID() const { return currentItemObjectID_; }
    const PlacementContext* placementContext() const
    {
        return placementContext_ ? &*placementContext_ : nullptr;
    }
    const SignalChainView& cachedView() const;

    void                   setCachedView(const SignalChainView& view);

    // Rebuilds the cached view from the current project state without emitting
    // sourceChanged (the caller materializes). Keeps the canvas in sync with
    // routing/property edits made outside the source (Inspector, undo, IPC).
    void                   refreshView();

    void setLayerVisible(NodeGraphLayer layer, bool visible);
    bool isLayerVisible(NodeGraphLayer layer) const;
    void loadPersistedLayerVisibility();

    void           loadBus(uint32_t busID);
    uint32_t       currentBusID() const { return currentBusID_; }

    QString        lastError() const { return lastError_; }

    static std::vector<BlueprintNodeDesc>
    nodesFromView(const SignalChainView& view, const std::array<bool, 3>& layers);

    static std::vector<BlueprintEdgeDesc>
    edgesFromView(const SignalChainView& view, const std::array<bool, 3>& layers);

    static constexpr float kColumnSpacing = 56.0f;
    static constexpr float kNodeWidth     = NodeItem::kDefaultWidth;
    static constexpr float kRowSpacing    = 16.0f;

private:
    BlueprintConnectResult doConnect(uint32_t fromId, uint32_t toId,
                                     const QString& edgeType,
                                     const QString& fromKind,
                                     const QString& toKind);
    bool                   doDisconnect(uint32_t fromId, uint32_t toId,
                                        const QString& edgeType,
                                        const QString& fromKind,
                                        const QString& toKind);
    void                   rebuildViewForCurrentTarget();
    void                   persistLayerVisibility() const;
    std::array<bool, 3> layerVisibility_ {true, true, true};

    struct ViewCache;
    ViewCache* viewCache_ = nullptr;

    ProjectItem*  currentItem_         = nullptr;
    QUuid         currentItemGuid_;
    uint32_t      currentItemObjectID_ = 0;
    uint32_t      currentBusID_        = 0;
    std::optional<PlacementContext> placementContext_;
    QString       lastError_;
};

}
