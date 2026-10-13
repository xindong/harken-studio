#pragma once

#include "IBlueprintSource.h"
#include "SignalPathGraph.h"
#include "NodeItem.h"

namespace VNX {

struct SignalPathLayout
{
    std::vector<QPointF> nodePositions;
    std::vector<std::optional<QPointF>> gainPositions;
};

class SignalPathSource : public IBlueprintSource
{
public:
    explicit SignalPathSource(QObject* parent = nullptr) : IBlueprintSource(parent) {}

    Scope scope() const override { return Scope::Focus; }
    std::vector<BlueprintNodeDesc> buildNodes() override { return nodes_; }
    std::vector<BlueprintEdgeDesc> buildEdges() override { return edges_; }
    bool supportsEditing() const override { return true; }

    void loadGraph(const ProjectData& project, SignalPathGraph graph);
    void clear();
    const SignalPathGraph& graph() const { return graph_; }

    SignalPathLayout layout(const std::vector<QSizeF>& sizes,
                            CanvasFlowDirection direction,
                            const std::vector<std::optional<QPointF>>& fixedPositions = {},
                            const std::vector<QSizeF>& gainSizes = {},
                            const std::vector<std::optional<QPointF>>& fixedGainPositions = {}) const;

private:
    SignalPathGraph graph_;
    std::vector<BlueprintNodeDesc> nodes_;
    std::vector<BlueprintEdgeDesc> edges_;
};

}
