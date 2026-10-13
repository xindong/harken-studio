
#include "BlueprintEdgeKind.h"

#include "../../Core/Components/VMixNode.h"
#include "../../Core/Components/VBus.h"
#include "../../Core/Components/VMixNode.h"
#include "../../Core/ProjectManager.h"

#include <unordered_set>

namespace VNX {

namespace {

void appendItemEdges(const ProjectItem& item,
                     const ProjectItem* parent,
                     std::vector<BlueprintEdgeView>& out)
{
    const uint32_t fromId = item.objectID;
    if (fromId == 0)
    {
        return;
    }

    if (parent != nullptr && parent->objectID != 0)
    {
        BlueprintEdgeView e;
        e.fromItemId = fromId;
        e.toItemId   = parent->objectID;
        e.kind       = BlueprintEdgeKind::Containment;
        out.push_back(e);
    }

    const auto* mc = item.comp<VMixNode>();
    const uint32_t outputBusId = mc ? mc->outputBusId : 0;
    if (outputBusId != 0)
    {
        BlueprintEdgeView e;
        e.fromItemId = fromId;
        e.toItemId   = outputBusId;
        e.kind = item.isOutputBusOverride()
                     ? BlueprintEdgeKind::Override
                     : BlueprintEdgeKind::BusRoute;
        out.push_back(e);
    }
}

void walk(const ProjectItem& item,
          const ProjectItem* parent,
          std::vector<BlueprintEdgeView>& out)
{
    appendItemEdges(item, parent, out);
    for (const auto& childPtr : item.children)
    {
        if (!childPtr) continue;
        walk(*childPtr, &item, out);
    }
}

}

std::vector<BlueprintEdgeView> deriveAllEdges(const ProjectItem& root)
{
    std::vector<BlueprintEdgeView> out;
    for (const auto& childPtr : root.children)
    {
        if (!childPtr) continue;
        walk(*childPtr, &root, out);
    }
    return out;
}

std::vector<BlueprintEdgeView> deriveEdgesForItem(const ProjectItem& item,
                                                  const ProjectItem* parent)
{
    std::vector<BlueprintEdgeView> out;
    appendItemEdges(item, parent, out);
    return out;
}

namespace {

void appendBusSendEdges(const VMixNode& mix,
                        uint32_t busObjectID,
                        std::vector<BlueprintEdgeView>& out)
{
    const uint32_t fromId = busObjectID;
    if (fromId == 0) return;

    for (size_t i = 0; i < mix.sends.size(); ++i)
    {
        const auto& send = mix.sends[i];
        if (send.targetBusID == 0) continue;
        BlueprintEdgeView e;
        e.fromItemId = fromId;
        e.toItemId   = send.targetBusID;
        e.kind       = BlueprintEdgeKind::Send;
        e.sendIndex  = static_cast<uint32_t>(i);
        out.push_back(e);
    }
}

}

std::vector<BlueprintEdgeView> deriveAllEdges(const ProjectData& project)
{
    std::vector<BlueprintEdgeView> out;
    for (const auto* binding : project.allFileBindings())
    {
        if (!binding || !binding->rootItem) continue;
        walk(*binding->rootItem, nullptr, out);
    }
    if (!project.busses.empty() && project.busses[0].rootItem)
    {
        std::function<void(const ProjectItem*)> walkBus =
            [&](const ProjectItem* node) {
                if (node == nullptr) return;
                if (node->comp<VNX::VBus>())
                    if (const auto* mix = node->comp<VNX::VMixNode>())
                        appendBusSendEdges(*mix, node->objectID, out);
                for (auto const& child : node->children) walkBus(child.get());
            };
        walkBus(project.busses[0].rootItem.get());
    }
    return out;
}

}
