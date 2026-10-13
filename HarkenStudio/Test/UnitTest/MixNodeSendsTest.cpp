#include "ProjectTypes.h"
#include "Components/VBus.h"
#include "Components/VMixNode.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonObject>

using VNX::ProjectItem;
using VNX::Send;
using VNX::VBus;
using VNX::VMixNode;

TEST(MixNodeSends, JsonRoundTripKeepsLegacyKeys)
{
    VMixNode node;
    node.sends.push_back(Send{7u, 0.5f});
    node.sends.push_back(Send{9u, 1.0f});

    const QJsonObject j = node.toJson();
    ASSERT_TRUE(j.contains("sends"));
    const QJsonArray arr = j.value("sends").toArray();
    ASSERT_EQ(arr.size(), 2);
    EXPECT_EQ(arr[0].toObject().value("targetBusID").toInt(), 7);
    EXPECT_NEAR(arr[0].toObject().value("sendLevel").toDouble(),
                VMixNode::linearToDb(0.5f), 1e-4);
    EXPECT_EQ(arr[1].toObject().value("targetBusID").toInt(), 9);
    EXPECT_NEAR(arr[1].toObject().value("sendLevel").toDouble(), 0.0, 1e-4);

    VMixNode restored;
    restored.fromJson(j);
    ASSERT_EQ(restored.sends.size(), 2u);
    EXPECT_EQ(restored.sends[0].targetBusID, 7u);
    EXPECT_NEAR(restored.sends[0].sendLevel, 0.5f, 1e-4);
    EXPECT_EQ(restored.sends[1].targetBusID, 9u);
    EXPECT_NEAR(restored.sends[1].sendLevel, 1.0f, 1e-4);
}

TEST(MixNodeSends, EmptySendsOmittedFromJson)
{
    VMixNode node;
    EXPECT_FALSE(node.toJson().contains("sends"));

    VMixNode restored;
    restored.fromJson(QJsonObject{});
    EXPECT_TRUE(restored.sends.empty());
}

TEST(MixNodeSends, SetFieldReplacesWholeListInDb)
{
    VMixNode node;
    node.sends.push_back(Send{3u, 1.0f});

    const QVariantList next{
        QVariantMap{{QStringLiteral("targetBusID"), 11u},
                    {QStringLiteral("sendLevel"), -12.0f}},
    };
    ASSERT_TRUE(node.setField(QStringLiteral("sends"), next));
    ASSERT_EQ(node.sends.size(), 1u);
    EXPECT_EQ(node.sends[0].targetBusID, 11u);
    EXPECT_NEAR(node.sends[0].sendLevel, VMixNode::dbToLinear(-12.0f), 1e-4);

    const QVariantList out = node.getField(QStringLiteral("sends")).toList();
    ASSERT_EQ(out.size(), 1);
    EXPECT_EQ(out[0].toMap().value(QStringLiteral("targetBusID")).toUInt(), 11u);
    EXPECT_NEAR(out[0].toMap().value(QStringLiteral("sendLevel")).toDouble(), -12.0, 1e-3);
}

TEST(MixNodeSends, LegacyBusSendsKeyIsIgnored)
{
    QJsonArray legacySends;
    legacySends.append(QJsonObject{{"targetBusID", 5}, {"sendLevel", 0.7}});
    QJsonObject legacyBusJson{{"pan", 0.0}, {"sends", legacySends}};

    VBus bus;
    bus.fromJson(legacyBusJson);
    EXPECT_FALSE(bus.toJson().contains("sends"));

    VMixNode mixFromBusJson;
    mixFromBusJson.fromJson(QJsonObject{});
    EXPECT_TRUE(mixFromBusJson.sends.empty());
}
