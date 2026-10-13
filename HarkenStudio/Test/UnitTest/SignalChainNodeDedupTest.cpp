#include "ProjectTypes.h"
#include "Widgets/Blueprint/SignalChainSource.h"

#include <gtest/gtest.h>

#include <QString>

namespace {

constexpr std::array<bool, 3> kAllLayers{true, true, true};

VNX::BusChainNode makeBus(uint32_t id, const char* name)
{
    VNX::BusChainNode b;
    b.busID = id;
    b.name  = QString::fromLatin1(name);
    return b;
}

int countBusNodes(const std::vector<VNX::BlueprintNodeDesc>& nodes, uint32_t id)
{
    int n = 0;
    for (auto const& d : nodes) {
        if (d.kind == VNX::BlueprintNodeKind::Bus && d.id == id) ++n;
    }
    return n;
}

int countSendEdges(const std::vector<VNX::BlueprintEdgeDesc>& edges,
                   uint32_t fromId, uint32_t toId)
{
    int n = 0;
    for (auto const& e : edges) {
        if (e.fromKind == VNX::BlueprintNodeKind::Bus && e.fromId == fromId &&
            e.toKind == VNX::BlueprintNodeKind::Bus && e.toId == toId &&
            e.label.startsWith(QStringLiteral("Send"))) ++n;
    }
    return n;
}

}

TEST(SignalChainNodeDedupTest, SendsToSameTargetShareOneCard)
{
    VNX::SignalChainView view;
    view.busChain.push_back(makeBus(10, "BusA"));
    view.busChain.push_back(makeBus(11, "BusB"));
    view.busChain[0].sends.push_back(VNX::SendViewInfo{90, QStringLiteral("Reverb"), -6.0f});
    view.busChain[1].sends.push_back(VNX::SendViewInfo{90, QStringLiteral("Reverb"), -3.0f});

    const auto nodes = VNX::SignalChainSource::nodesFromView(view, kAllLayers);
    EXPECT_EQ(countBusNodes(nodes, 90), 1);

    const auto edges = VNX::SignalChainSource::edgesFromView(view, kAllLayers);
    EXPECT_EQ(countSendEdges(edges, 10, 90), 1);
    EXPECT_EQ(countSendEdges(edges, 11, 90), 1);
}

TEST(SignalChainNodeDedupTest, SendTargetInsideChainReusesChainCard)
{
    VNX::SignalChainView view;
    view.busChain.push_back(makeBus(10, "BusA"));
    view.busChain.push_back(makeBus(11, "Master"));
    view.busChain[0].sends.push_back(VNX::SendViewInfo{11, QStringLiteral("Master"), 0.0f});

    const auto nodes = VNX::SignalChainSource::nodesFromView(view, kAllLayers);
    EXPECT_EQ(countBusNodes(nodes, 11), 1);

    const auto edges = VNX::SignalChainSource::edgesFromView(view, kAllLayers);
    EXPECT_EQ(countSendEdges(edges, 10, 11), 1);
}

TEST(SignalChainNodeDedupTest, IncomingSenderAlreadyShownAsSendTargetSharesCard)
{
    VNX::SignalChainView view;
    view.busChain.push_back(makeBus(10, "BusA"));
    view.busChain[0].sends.push_back(VNX::SendViewInfo{90, QStringLiteral("Aux"), 0.0f});
    view.incomingSends.push_back(VNX::IncomingSendInfo{90, QStringLiteral("Aux"), 0.5f});

    const auto nodes = VNX::SignalChainSource::nodesFromView(view, kAllLayers);
    EXPECT_EQ(countBusNodes(nodes, 90), 1);

    const auto edges = VNX::SignalChainSource::edgesFromView(view, kAllLayers);
    EXPECT_EQ(countSendEdges(edges, 10, 90), 1);
    EXPECT_EQ(countSendEdges(edges, 90, 10), 1);
}

TEST(SignalChainNodeDedupTest, SendLevelsLiveOnEdgesNotOnSharedCard)
{
    VNX::SignalChainView view;
    view.busChain.push_back(makeBus(10, "BusA"));
    view.busChain.push_back(makeBus(11, "BusB"));
    view.busChain[0].sends.push_back(VNX::SendViewInfo{90, QStringLiteral("Reverb"), -6.0f});
    view.busChain[1].sends.push_back(VNX::SendViewInfo{90, QStringLiteral("Reverb"), -3.0f});

    const auto nodes = VNX::SignalChainSource::nodesFromView(view, kAllLayers);
    for (auto const& d : nodes) {
        if (d.kind == VNX::BlueprintNodeKind::Bus && d.id == 90) {
            EXPECT_EQ(d.title, QStringLiteral("Reverb"));
        }
    }

    const auto edges = VNX::SignalChainSource::edgesFromView(view, kAllLayers);
    QString labelFromA;
    QString labelFromB;
    for (auto const& e : edges) {
        if (e.toId != 90) continue;
        if (e.fromId == 10) labelFromA = e.label;
        if (e.fromId == 11) labelFromB = e.label;
    }
    EXPECT_EQ(labelFromA, QStringLiteral("Send (-6.0 dB)"));
    EXPECT_EQ(labelFromB, QStringLiteral("Send (-3.0 dB)"));
}

TEST(SignalChainNodeDedupTest, DuplicateIncomingSendersCollapseToOneCard)
{
    VNX::SignalChainView view;
    view.busChain.push_back(makeBus(10, "Head"));
    view.incomingSends.push_back(VNX::IncomingSendInfo{80, QStringLiteral("SideA"), 0.5f});
    view.incomingSends.push_back(VNX::IncomingSendInfo{80, QStringLiteral("SideA"), 0.25f});

    const auto nodes = VNX::SignalChainSource::nodesFromView(view, kAllLayers);
    EXPECT_EQ(countBusNodes(nodes, 80), 1);
}
