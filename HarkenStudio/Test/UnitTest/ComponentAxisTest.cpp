#include "Component.h"
#include "ItemTypeCatalog.h"
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectTypes.h"
#include "Components/VAttenuation.h"
#include "Components/VAudio.h"
#include "Components/VBus.h"
#include "Components/VMixNode.h"
#include "Components/VSelection.h"
#include "Components/VSound.h"
#include "Components/VVariable.h"
#include "Components/VTrack.h"
#include "Components/VPiece.h"
#include "Widgets/Inspector/LayerInspectorSchema.h"
#include "harken_playable_property_studio.g.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonObject>

#include <initializer_list>
#include <vector>

using VNX::ComponentType;
using VNX::ProjectItem;

namespace {

ProjectItem makeAttached(ProjectItem::Type type)
{
    ProjectItem item(QStringLiteral("Item"), type);
    item.attachDefaultComponents();
    return item;
}

struct MatrixRow {
    ProjectItem::Type type;
    std::vector<ComponentType> components;
};

const std::vector<MatrixRow>& attachMatrix()
{
    static const std::vector<MatrixRow> rows = {
        { ProjectItem::Type::None,      {} },
        { ProjectItem::Type::Sound,       { ComponentType::MixNode, ComponentType::Audio,
                                            ComponentType::Spatial, ComponentType::Sound } },
        { ProjectItem::Type::Container,   { ComponentType::MixNode, ComponentType::Audio,
                                            ComponentType::Spatial, ComponentType::Selection } },
        { ProjectItem::Type::Track,       { ComponentType::MixNode, ComponentType::Audio,
                                            ComponentType::Spatial,
                                            ComponentType::Track } },
        { ProjectItem::Type::Piece,       { ComponentType::MixNode, ComponentType::Audio,
                                            ComponentType::Spatial,
                                            ComponentType::Piece } },
        { ProjectItem::Type::Layer,       { ComponentType::MixNode,
                                            ComponentType::Spatial } },
        { ProjectItem::Type::Bus,         { ComponentType::MixNode, ComponentType::Bus } },
        { ProjectItem::Type::Action,      { ComponentType::Action } },
        { ProjectItem::Type::Variable,    { ComponentType::Variable } },
        { ProjectItem::Type::Attenuation, { ComponentType::Attenuation } },
    };
    return rows;
}

const VNX::PropertyDescriptor* findField(const VNX::PropertySchema& s, QLatin1String field)
{
    for (const auto& d : s)
        if (d.field == field) return &d;
    return nullptr;
}

} // namespace

TEST(ComponentAxis, AttachMatrixMatchesBlueprint)
{
    for (const MatrixRow& row : attachMatrix())
    {
        const ProjectItem item = makeAttached(row.type);
        EXPECT_EQ(item.components.count(), row.components.size())
            << VNX::ItemTypeCatalog::canonicalName(row.type).toStdString();
        for (ComponentType c : row.components)
            EXPECT_TRUE(item.components.has(c))
                << VNX::ItemTypeCatalog::canonicalName(row.type).toStdString()
                << " missing component " << static_cast<int>(c);
    }
}

TEST(ComponentAxis, CatalogPredicatesMatchAttachedComponents)
{
    for (const MatrixRow& row : attachMatrix())
    {
        const ProjectItem item = makeAttached(row.type);
        EXPECT_EQ(VNX::ItemTypeCatalog::hasMixingCapability(row.type),
                  item.components.has(ComponentType::MixNode))
            << VNX::ItemTypeCatalog::canonicalName(row.type).toStdString();
        EXPECT_EQ(VNX::ItemTypeCatalog::isContentObject(row.type),
                  item.components.has(ComponentType::Audio))
            << VNX::ItemTypeCatalog::canonicalName(row.type).toStdString();
        EXPECT_EQ(VNX::ItemTypeCatalog::hasSpatialCapability(row.type),
                  item.components.has(ComponentType::Spatial))
            << VNX::ItemTypeCatalog::canonicalName(row.type).toStdString();
    }
}

TEST(ComponentAxis, TimelineComponentsAttachDetachAndReattachByType)
{
    ProjectItem track = makeAttached(ProjectItem::Type::Track);
    ProjectItem piece = makeAttached(ProjectItem::Type::Piece);

    EXPECT_NE(track.comp<VNX::VTrack>(), nullptr);
    EXPECT_EQ(track.comp<VNX::VPiece>(), nullptr);
    EXPECT_NE(piece.comp<VNX::VPiece>(), nullptr);
    EXPECT_EQ(piece.comp<VNX::VTrack>(), nullptr);

    track.components.remove<VNX::VTrack>();
    piece.components.remove<VNX::VPiece>();
    EXPECT_EQ(track.comp<VNX::VTrack>(), nullptr);
    EXPECT_EQ(piece.comp<VNX::VPiece>(), nullptr);

    track.attachDefaultComponents();
    piece.attachDefaultComponents();
    EXPECT_NE(track.comp<VNX::VTrack>(), nullptr);
    EXPECT_NE(piece.comp<VNX::VPiece>(), nullptr);
}

TEST(ComponentAxis, MixNodeSchemaCarriesChassisFields)
{
    const auto& s = VNX::VMixNode::schema();
    for (const auto field : { QLatin1String("volume"), QLatin1String("pitch"),
                              QLatin1String("outputBusId"),
                              QLatin1String("sends") })
        EXPECT_NE(findField(s, field), nullptr) << field.latin1();
    EXPECT_EQ(findField(s, QLatin1String("color")), nullptr);
    EXPECT_EQ(findField(s, QLatin1String("attenuationName")), nullptr);
}

TEST(ComponentAxis, MixNodeCarriesNoMuteSolo)
{
    const auto& s = VNX::VMixNode::schema();
    EXPECT_EQ(findField(s, QLatin1String("mute")), nullptr);
    EXPECT_EQ(findField(s, QLatin1String("solo")), nullptr);

    const VNX::VMixNode mix;
    const QJsonObject saved = mix.toJson();
    EXPECT_FALSE(saved.contains(QStringLiteral("mute")));
    EXPECT_FALSE(saved.contains(QStringLiteral("solo")));
}

TEST(ComponentAxis, MixNodeBindingsAndEffectsRoundtrip)
{
    VNX::VMixNode mix;
    mix.bindings = VNX::BindingLedger::fromJson(
        QJsonArray{ QJsonObject{{ QStringLiteral("id"), 5 }} });
    mix.effects  = QJsonArray{ QJsonObject{{ QStringLiteral("slot"), 1 }} };

    const QJsonObject saved = mix.toJson();
    EXPECT_EQ(saved[QStringLiteral("bindings")].toArray(), mix.bindings.toJson());
    EXPECT_EQ(saved[QStringLiteral("effects")].toArray(), mix.effects.toJson());

    VNX::VMixNode reloaded;
    reloaded.fromJson(saved);
    EXPECT_EQ(reloaded.bindings.toJson(), mix.bindings.toJson());
    EXPECT_EQ(reloaded.effects.toJson(), mix.effects.toJson());
}

TEST(ComponentAxis, LayerInspectorSchemaCarriesChassisAndSpatialRows)
{
    const auto& s = VNX::layerInspectorSchema();
    size_t visibleRows = 0;
    for (const auto& field : VNX::PlayablePropertyAuthoringV1::fields()) {
        const bool expected = VNX::PlayablePropertyAuthoringV1::isInspectorVisible(field.id)
            && VNX::PlayablePropertyAuthoringV1::isSlotApplicable(
                ProjectItem::Type::Layer, field.id);
        EXPECT_EQ(findField(s, QLatin1String(field.fieldName)) != nullptr, expected)
            << field.fieldName;
        if (expected) ++visibleRows;
    }
    ASSERT_EQ(s.size(), visibleRows + 1);
    EXPECT_EQ(findField(s, QLatin1String("attenuationName")), nullptr);
    EXPECT_NE(findField(s, QLatin1String("notes")), nullptr);
}
