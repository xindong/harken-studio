#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Support/FieldGroupTestUtils.h"
#include "Core/Components/VSelection.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "ActionManager.h"
#include "ComponentCatalog.h"
#include "ItemTypeCatalog.h"
#include "NameValidator.h"
#include "Components/VAudio.h"
#include "Components/VMixNode.h"
#include "Components/VSpatial.h"
#include "Components/VTrack.h"
#include "PlacementContext.h"
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectDocument.h"
#include "ProjectManager.h"
#include "ProjectSerializer.h"
#include "ProjectTypes.h"
#include "ItemPropertySchema.h"
#include "UndoCommands.h"
#include "Widgets/Inspector/ContentObjectSchemaRows.h"

#include <QApplication>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <unordered_map>

namespace {

using VNX::PlayablePropertyAuthoringV1;
using VNX::PlayablePropertyOccurrenceV1;
using VNX::PlayablePropertyOccurrenceValueV1;
using VNX::ProjectItem;
using VNX::VAudio;
using VNX::VMixNode;
using VNX::VSpatial;
using harkenplayableproperties::studio::kEnvelopeJsonKeyV1;
const auto kJsonFieldsV1 = PlayablePropertyAuthoringV1::fields();
using harkenplayableproperties::studio::kPresenceJsonKeyV1;
using harkenplayableproperties::PlayablePropertyIdV1;
using harkenplayableproperties::PlayablePropertyWireV1;

PlayablePropertyOccurrenceValueV1 exactOccurrenceValue(
    PlayablePropertyWireV1 wire,
    uint8_t id)
{
    if (wire == PlayablePropertyWireV1::F32)
        return static_cast<float>(id) + 0.25f;
    if (wire == PlayablePropertyWireV1::U8) return true;
    if (wire == PlayablePropertyWireV1::U32)
        return static_cast<uint32_t>(100 + id);
    if (wire == PlayablePropertyWireV1::ObjectId)
        return harkentest::StudioObjectId(1000 + id);
    return QJsonArray{QJsonObject{{QStringLiteral("id"), static_cast<int>(id)}}};
}

PlayablePropertyOccurrenceValueV1 wrongOccurrenceValue(
    PlayablePropertyWireV1 wire)
{
    return wire == PlayablePropertyWireV1::F32
        ? PlayablePropertyOccurrenceValueV1{false}
        : PlayablePropertyOccurrenceValueV1{0.5f};
}

QJsonValue exactJsonValue(PlayablePropertyWireV1 wire, uint8_t id)
{
    if (PlayablePropertyAuthoringV1::isComponentStored(id)) return 0.5;
    if (wire == PlayablePropertyWireV1::F32)
        return static_cast<double>(id) + 0.25;
    if (wire == PlayablePropertyWireV1::U8) return true;
    if (wire == PlayablePropertyWireV1::U32) return 100 + id;
    if (wire == PlayablePropertyWireV1::ObjectId)
        return VNX::authoringUuid(harkentest::StudioObjectId(1000 + id)).toString(QUuid::WithoutBraces);
    if (wire == PlayablePropertyWireV1::RowSpan)
        return QJsonArray{QJsonObject{
            {QStringLiteral("targetBusID"), 100 + id},
            {QStringLiteral("sendLevel"), 0.0}}};
    return QJsonArray{QJsonObject{{QStringLiteral("id"), static_cast<int>(id)}}};
}

ProjectItem makeDefinition(ProjectItem::Type type)
{
    ProjectItem item(QStringLiteral("definition"), type);
    item.guid = QUuid::createUuid();
    item.attachDefaultComponents();
    return item;
}

QJsonObject serializedDefinition(ProjectItem::Type type)
{
    VNX::ProjectSerializer serializer;
    ProjectItem item = makeDefinition(type);
    return serializer.projectItemToJson(item);
}

bool loadDefinition(const QJsonObject& json, ProjectItem* item)
{
    VNX::ProjectSerializer serializer;
    return item && serializer.projectItemFromJson(json, *item);
}

TEST(PlayablePropertyAuthoring, SpatializationMixKeepsComponentPresenceAndSpatialInheritance)
{
    ProjectItem parent = makeDefinition(ProjectItem::Type::Layer);
    ProjectItem child = makeDefinition(ProjectItem::Type::Sound);
    child.parent = &parent;
    parent.comp<VSpatial>()->spatializationMix = 0.5f;
    const auto groups = child.fieldGroups;
    const auto locator = *PlayablePropertyAuthoringV1::definitionLocator(child);
    const auto id = static_cast<uint8_t>(PlayablePropertyIdV1::SpatializationMix);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::clearOverride(child, locator, id, std::nullopt));
    auto source = PlayablePropertyAuthoringV1::resolveSpatializationMix(child);
    ASSERT_TRUE(source);
    EXPECT_EQ(source->owner, &parent);
    EXPECT_DOUBLE_EQ(source->state.value.toDouble(), 0.5);

    VNX::ProjectSerializer serializer;
    auto json = serializer.projectItemToJson(child);
    auto components = json.value("components").toObject();
    EXPECT_FALSE(components.value("spatial").toObject().contains("spatializationMix"));
    ProjectItem loaded;
    ASSERT_TRUE(serializer.projectItemFromJson(json, loaded));
    EXPECT_FALSE(loaded.comp<VSpatial>()->spatializationMixPresent);
    EXPECT_EQ(loaded.fieldGroups, groups);

    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(child, locator, id, 1.0, std::nullopt));
    json = serializer.projectItemToJson(child);
    EXPECT_DOUBLE_EQ(json.value("components").toObject().value("spatial").toObject()
                         .value("spatializationMix").toDouble(), 1.0);
    EXPECT_FALSE(json.value("playableProperties").toObject().contains("spatializationMix"));
    EXPECT_EQ(child.playablePropertyPresence.bits, 0u);
    ASSERT_TRUE(serializer.projectItemFromJson(json, loaded));
    EXPECT_TRUE(loaded.comp<VSpatial>()->spatializationMixPresent);
    loaded.parent = &parent;
    source = PlayablePropertyAuthoringV1::resolveSpatializationMix(loaded);
    ASSERT_TRUE(source);
    EXPECT_EQ(source->owner, &loaded);
    EXPECT_DOUBLE_EQ(source->state.value.toDouble(), 1.0);
    EXPECT_EQ(loaded.fieldGroups, groups);

    components = json.value("components").toObject();
    components.remove("spatial");
    json["components"] = components;
    ASSERT_TRUE(serializer.projectItemFromJson(json, loaded));
    EXPECT_FALSE(loaded.comp<VSpatial>()->spatializationMixPresent);
    EXPECT_EQ(loaded.fieldGroups, groups);
    auto malformed = PlayablePropertyAuthoringV1::definitionToJson(child);
    malformed["spatializationMix"] = 0.5;
    EXPECT_FALSE(PlayablePropertyAuthoringV1::definitionFromJson(malformed, child));
    malformed = PlayablePropertyAuthoringV1::definitionToJson(child);
    malformed["spatial.spatializationMix"] = 0.5;
    EXPECT_FALSE(PlayablePropertyAuthoringV1::definitionFromJson(malformed, child));
    EXPECT_FALSE(PlayablePropertyAuthoringV1::decodePresence(QStringLiteral("0000000000010000")));
}

TEST(PlayablePropertyAuthoring, SpatializationMixRejectsInvalidValuesBeforeMutation)
{
    ProjectItem item = makeDefinition(ProjectItem::Type::Sound);
    auto* spatial = item.comp<VSpatial>();
    ASSERT_TRUE(spatial->setField("spatializationMix", 0.5));
    const auto locator = *PlayablePropertyAuthoringV1::definitionLocator(item);
    const auto id = static_cast<uint8_t>(PlayablePropertyIdV1::SpatializationMix);
    for (const auto& value : {QJsonValue(-0.01), QJsonValue(1.01), QJsonValue(1.00000001),
                              QJsonValue("0.5"), QJsonValue(QJsonValue::Null)}) {
        EXPECT_FALSE(PlayablePropertyAuthoringV1::setOverride(item, locator, id, value, std::nullopt));
        EXPECT_FALSE(spatial->fromJson(QJsonObject{{"spatializationMix", value}}));
        EXPECT_FLOAT_EQ(spatial->spatializationMix, 0.5f);
        EXPECT_TRUE(spatial->spatializationMixPresent);
    }
}

TEST(PlayablePropertyAuthoring, PitchUsesCentsInSchemaAndSerializedDefinitions)
{
    const auto pitchId = static_cast<uint8_t>(PlayablePropertyIdV1::Pitch);
    for (const auto type : {ProjectItem::Type::Layer, ProjectItem::Type::Sound,
                           ProjectItem::Type::Container, ProjectItem::Type::Track,
                           ProjectItem::Type::Piece, ProjectItem::Type::Bus}) {
        ProjectItem item = makeDefinition(type);
        const auto descriptor = std::find_if(VMixNode::schema().begin(), VMixNode::schema().end(),
            [](const VNX::PropertyDescriptor& field) { return field.field == QStringLiteral("pitch"); });
        ASSERT_NE(descriptor, VMixNode::schema().end());
        EXPECT_EQ(descriptor->unit.trimmed(), QStringLiteral("cent"));
        EXPECT_DOUBLE_EQ(descriptor->min.toDouble(), -4800.0);
        EXPECT_DOUBLE_EQ(descriptor->max.toDouble(), 4800.0);
        const auto locator = PlayablePropertyAuthoringV1::definitionLocator(item);
        ASSERT_TRUE(locator.has_value());
        for (const double cents : {1200.0, -1200.0, 0.1, 12.0}) {
            ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
                item, *locator, pitchId, cents, std::nullopt));
            const QJsonObject json = VNX::ProjectSerializer().projectItemToJson(item);
            EXPECT_FLOAT_EQ(json.value(QLatin1String(kEnvelopeJsonKeyV1)).toObject()
                                .value(QStringLiteral("pitch")).toDouble(), cents);
            ProjectItem reloaded;
            ASSERT_TRUE(loadDefinition(json, &reloaded));
            ASSERT_NE(reloaded.comp<VMixNode>(), nullptr);
            EXPECT_FLOAT_EQ(reloaded.comp<VMixNode>()->pitch, cents);
        }
    }
}

TEST(PlayablePropertyAuthoring, GeneratedRegistryHasUniqueExactStudioStorage)
{
    constexpr std::array<uint8_t, 13> expectedIds{
        1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 14, 17};
    ASSERT_EQ(std::size(kJsonFieldsV1), expectedIds.size());
    ProjectItem item = makeDefinition(ProjectItem::Type::Sound);
    QSet<QString> jsonKeys;
    QSet<QString> storageKeys;
    for (size_t index = 0; index < std::size(kJsonFieldsV1); ++index) {
        const auto& field = kJsonFieldsV1[index];
        EXPECT_EQ(field.id, expectedIds[index]);
        EXPECT_EQ(field.presentBit, UINT64_C(1) << (field.id - 1));
        const QString jsonKey = QLatin1String(field.jsonKey);
        EXPECT_FALSE(jsonKeys.contains(jsonKey));
        jsonKeys.insert(jsonKey);
        const QString storage = QString::fromLatin1(field.component)
            + QLatin1Char('.') + QString::fromLatin1(field.storageField);
        EXPECT_FALSE(storageKeys.contains(storage));
        storageKeys.insert(storage);
        const auto componentType = VNX::ComponentCatalog::parseComponentName(
            QLatin1String(field.component));
        ASSERT_TRUE(componentType.has_value());
        const VNX::IComponent* component = nullptr;
        item.components.forEach([&](VNX::IComponent& candidate) {
            if (candidate.getType() == *componentType) component = &candidate;
        });
        ASSERT_NE(component, nullptr) << storage.toStdString();
        EXPECT_TRUE(component->getField(QLatin1String(field.storageField)).isValid())
            << storage.toStdString();
    }
}

TEST(PlayablePropertyAuthoring, InspectorRowsHaveNoPerFieldSourceIndicators)
{
    const VNX::PropertySchema rows =
        VNX::contentObjectInspectorRows(VNX::ProjectItem::Type::Sound);
    size_t rowCount = 0;
    for (const auto& field : kJsonFieldsV1) {
        if (!PlayablePropertyAuthoringV1::isInspectorVisible(field.id))
            continue;
        const auto row = std::find_if(rows.begin(), rows.end(), [&](const auto& descriptor) {
            return descriptor.field == QLatin1String(field.fieldName);
        });
        ASSERT_NE(row, rows.end());
        EXPECT_EQ(row->jsonKey, QLatin1String(field.jsonKey));
        EXPECT_FALSE(row->flags.sourceIndicator);
        ++rowCount;
    }
    ASSERT_EQ(rowCount + 1, rows.size());
    EXPECT_EQ(rows.back().field, QStringLiteral("notes"));
}

TEST(PlayablePropertyAuthoring, StreamingModeIsWithheldFromEveryEditorSurface)
{
    const uint8_t streamingMode = static_cast<uint8_t>(
        harkenplayableproperties::PlayablePropertyIdV1::StreamingMode);
    EXPECT_FALSE(PlayablePropertyAuthoringV1::isEditorExposed(streamingMode));
    EXPECT_FALSE(PlayablePropertyAuthoringV1::isInspectorVisible(streamingMode));

    // The slot itself is untouched: storage, table entry, and serialization
    // still carry it so existing projects keep whatever they authored.
    EXPECT_NE(PlayablePropertyAuthoringV1::fieldById(streamingMode), nullptr);
    EXPECT_NE(PlayablePropertyAuthoringV1::fieldByName(
                  QStringLiteral("streamingMode")), nullptr);

    // Row absence across every type is InspectorRowSetGold's job; what belongs
    // here is the command surface the AI and the action layer are handed.
    const QJsonArray editable = VNX::setItemPropertySchema()
        .value(QStringLiteral("properties")).toObject()
        .value(QStringLiteral("property")).toObject()
        .value(QStringLiteral("enum")).toArray();
    EXPECT_FALSE(editable.isEmpty());
    EXPECT_FALSE(editable.contains(QStringLiteral("streamingMode")));
}

TEST(PlayablePropertyAuthoring, ItemPropertySchemaCarriesExactOccurrenceEditShape)
{
    const QJsonObject schema = VNX::setItemPropertySchema();
    const QJsonArray required = schema.value(QStringLiteral("required")).toArray();
    EXPECT_TRUE(required.contains(QStringLiteral("property")));
    EXPECT_FALSE(required.contains(QStringLiteral("value")));
    const QJsonObject properties = schema.value(QStringLiteral("properties")).toObject();
    EXPECT_TRUE(properties.contains(QStringLiteral("override_operation")));
    EXPECT_TRUE(properties.contains(QStringLiteral("occurrence_kind")));
    EXPECT_TRUE(properties.contains(QStringLiteral("occurrence_guid")));
    EXPECT_TRUE(properties.contains(QStringLiteral("occurrence_target_guid")));
    EXPECT_TRUE(properties.contains(QStringLiteral("occurrence_ordinal")));
    const QJsonArray names = properties.value(QStringLiteral("property"))
        .toObject().value(QStringLiteral("enum")).toArray();
    for (const auto& field : kJsonFieldsV1)
        EXPECT_EQ(names.contains(QLatin1String(field.fieldName)),
                  PlayablePropertyAuthoringV1::isEditorExposed(field.id))
            << field.fieldName;
}

TEST(PlayablePropertyAuthoring, SignalGraphHasNoAuthoringStorageOrEditSurface)
{
    constexpr auto id = PlayablePropertyIdV1::SignalGraph;
    const auto slot = static_cast<uint8_t>(id);
    EXPECT_EQ(PlayablePropertyAuthoringV1::fieldById(slot), nullptr);
    EXPECT_EQ(PlayablePropertyAuthoringV1::fieldByName(QStringLiteral("signalGraph")), nullptr);
    EXPECT_EQ(PlayablePropertyAuthoringV1::fieldByJsonKey(QStringLiteral("signalGraph")), nullptr);
    EXPECT_FALSE(PlayablePropertyAuthoringV1::isEditorExposed(slot));
    EXPECT_FALSE(PlayablePropertyAuthoringV1::isInspectorVisible(slot));
    EXPECT_FALSE(PlayablePropertyAuthoringV1::decodePresence(
        QStringLiteral("0000000000008000")));
    const QJsonArray editable = VNX::setItemPropertySchema()
        .value(QStringLiteral("properties")).toObject()
        .value(QStringLiteral("property")).toObject()
        .value(QStringLiteral("enum")).toArray();
    EXPECT_FALSE(editable.contains(QStringLiteral("signalGraph")));

    for (const auto type : {ProjectItem::Type::Layer, ProjectItem::Type::Sound,
                            ProjectItem::Type::Container, ProjectItem::Type::Track,
                            ProjectItem::Type::Piece, ProjectItem::Type::Bus}) {
        ProjectItem item = makeDefinition(type);
        const auto locator = PlayablePropertyAuthoringV1::definitionLocator(item);
        ASSERT_TRUE(locator.has_value());
        EXPECT_FALSE(item.comp<VMixNode>()->getField(QStringLiteral("signalGraph")).isValid());
        EXPECT_FALSE(item.comp<VMixNode>()->setField(QStringLiteral("signalGraph"), QJsonArray{}));
        EXPECT_FALSE(PlayablePropertyAuthoringV1::isSlotApplicable(type, slot));
        EXPECT_FALSE(PlayablePropertyAuthoringV1::setOverride(
            item, *locator, slot, QJsonArray{}, std::nullopt));
        EXPECT_EQ(item.playablePropertyPresence.bits, 0u);
    }

    PlayablePropertyOccurrenceV1 occurrence;
    EXPECT_FALSE(occurrence.setValue(id, QJsonArray{}));
    EXPECT_EQ(occurrence.value(id), nullptr);
    occurrence.presence.Set(id);
    EXPECT_TRUE(PlayablePropertyAuthoringV1::occurrenceToJson(occurrence).isEmpty());
}

TEST(PlayablePropertyAuthoring, AllSixDefinitionKindsRoundTripMaskZeroEnvelope)
{
    constexpr ProjectItem::Type types[] = {
        ProjectItem::Type::Layer,
        ProjectItem::Type::Sound,
        ProjectItem::Type::Container,
        ProjectItem::Type::Track,
        ProjectItem::Type::Piece,
        ProjectItem::Type::Bus,
    };
    VNX::ProjectSerializer serializer;
    for (const ProjectItem::Type type : types) {
        ProjectItem source = makeDefinition(type);
        const QJsonObject json = serializer.projectItemToJson(source);
        ASSERT_TRUE(json.value(QLatin1String(kEnvelopeJsonKeyV1)).isObject());
        EXPECT_EQ(json.value(QLatin1String(kEnvelopeJsonKeyV1)).toObject()
                      .value(QLatin1String(kPresenceJsonKeyV1)).toString(),
                  QStringLiteral("0000000000000000"));
        ProjectItem restored;
        ASSERT_TRUE(serializer.projectItemFromJson(json, restored));
        EXPECT_EQ(restored.type, type);
        EXPECT_EQ(restored.playablePropertyPresence.bits, 0u);
    }
}

TEST(PlayablePropertyAuthoring, GeneratedLocatorSetsAndClearsEveryDefinitionField)
{
    constexpr ProjectItem::Type types[] = {
        ProjectItem::Type::Layer,
        ProjectItem::Type::Sound,
        ProjectItem::Type::Container,
        ProjectItem::Type::Track,
        ProjectItem::Type::Piece,
        ProjectItem::Type::Bus,
    };
    for (const ProjectItem::Type type : types) {
        ProjectItem item = makeDefinition(type);
        const auto locator = PlayablePropertyAuthoringV1::definitionLocator(item);
        ASSERT_TRUE(locator.has_value());
        for (const auto& field : kJsonFieldsV1) {
            if (!PlayablePropertyAuthoringV1::isSlotApplicable(type, field.id)) {
                EXPECT_FALSE(PlayablePropertyAuthoringV1::setOverride(
                    item, *locator, field.id, exactJsonValue(field.wire, field.id), std::nullopt))
                    << static_cast<int>(field.id);
                EXPECT_FALSE(PlayablePropertyAuthoringV1::readProperty(
                    item, *locator, field.id, std::nullopt).has_value())
                    << static_cast<int>(field.id);
                continue;
            }
            if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(type)
                && VNX::FieldGroupForProperty(static_cast<PlayablePropertyIdV1>(field.id))) {
                EXPECT_FALSE(PlayablePropertyAuthoringV1::setOverride(
                    item, *locator, field.id, exactJsonValue(field.wire, field.id), std::nullopt));
                EXPECT_FALSE(PlayablePropertyAuthoringV1::clearOverride(
                    item, *locator, field.id, std::nullopt));
                continue;
            }
            ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
                item, *locator, field.id, exactJsonValue(field.wire, field.id), std::nullopt));
            const auto state = PlayablePropertyAuthoringV1::readProperty(
                item, *locator, field.id, std::nullopt);
            ASSERT_TRUE(state.has_value());
            EXPECT_TRUE(state->present) << static_cast<int>(field.id);
            EXPECT_EQ(state->value, exactJsonValue(field.wire, field.id))
                << static_cast<int>(field.id);
            ASSERT_TRUE(PlayablePropertyAuthoringV1::clearOverride(
                item, *locator, field.id, std::nullopt));
            const auto cleared = PlayablePropertyAuthoringV1::readProperty(
                item, *locator, field.id, std::nullopt);
            ASSERT_TRUE(cleared.has_value());
            EXPECT_FALSE(cleared->present) << static_cast<int>(field.id);
        }
        EXPECT_EQ(item.playablePropertyPresence.bits, 0u);
    }
}

TEST(PlayablePropertyAuthoring, OccurrenceLocatorsRequireExactStableIdentity)
{
    ProjectItem track = makeDefinition(ProjectItem::Type::Track);
    auto* timeline = track.comp<VNX::VTrack>();
    ASSERT_NE(timeline, nullptr);
    VNX::Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.targetGuid = QUuid::createUuid();
    clip.stableOccurrenceOrdinal = 7;
    timeline->clips.items.push_back(clip);
    timeline->clips.occurrenceOrdinalHighWater = 8;

    const auto trackLocator = PlayablePropertyAuthoringV1::trackClipLocator(
        track, clip.clipGuid);
    ASSERT_TRUE(trackLocator.has_value());
    EXPECT_FALSE(PlayablePropertyAuthoringV1::setOverride(
        track, *trackLocator, 4,
        exactJsonValue(PlayablePropertyWireV1::U32, 4), std::nullopt));
    auto trackState = PlayablePropertyAuthoringV1::readProperty(
        track, *trackLocator, 4, std::nullopt);
    ASSERT_TRUE(trackState.has_value());
    EXPECT_FALSE(trackState->present);
    EXPECT_EQ(trackState->value.toInt(), 1);

    auto staleTrack = *trackLocator;
    ++staleTrack.stableOccurrenceOrdinal;
    EXPECT_FALSE(PlayablePropertyAuthoringV1::readProperty(
        track, staleTrack, 4, std::nullopt).has_value());

    ProjectItem container = makeDefinition(ProjectItem::Type::Container);
    const QUuid childGuid = QUuid::createUuid();
    container.comp<VNX::VSelection>()->selections.shared.occurrenceOrdinalHighWater = 3;
    container.comp<VNX::VSelection>()->selections.shared.setOccurrenceRows({{childGuid, 2, {}}});
    const auto childLocator = PlayablePropertyAuthoringV1::containerChildLocator(
        container, childGuid);
    ASSERT_TRUE(childLocator.has_value());
    EXPECT_FALSE(PlayablePropertyAuthoringV1::setOverride(
        container, *childLocator, 1, 0.0, std::nullopt));
    const auto childState = PlayablePropertyAuthoringV1::readProperty(
        container, *childLocator, 1, std::nullopt);
    ASSERT_TRUE(childState.has_value());
    EXPECT_FALSE(childState->present);
    EXPECT_EQ(childState->value.toDouble(), 0.0);

    auto wrongChild = *childLocator;
    wrongChild.occurrenceGuid = QUuid::createUuid();
    EXPECT_FALSE(PlayablePropertyAuthoringV1::clearOverride(
        container, wrongChild, 1, std::nullopt));
}

TEST(PlayablePropertyAuthoring, LegacyRegistryCombineRulesRemainDistinctFromBusLocalRules)
{
    const QSet<uint8_t> inheritedFields{5, 6, 7, 9, 11, 12, 14, 17};
    for (const auto& field : kJsonFieldsV1) {
        SCOPED_TRACE(field.fieldName);
        EXPECT_EQ(PlayablePropertyAuthoringV1::valueRule(
                      ProjectItem::Type::Sound, field.id)
                      == VNX::PlayablePropertyValueRuleV1::Inherit,
                  inheritedFields.contains(field.id));
        EXPECT_NE(PlayablePropertyAuthoringV1::valueRule(
                      ProjectItem::Type::Bus, field.id),
                  VNX::PlayablePropertyValueRuleV1::Inherit);
    }
}

TEST(PlayablePropertyAuthoring, FieldGroupFieldsRejectLegacyDefinitionWrites)
{
    for (const uint8_t id : {6, 7, 8, 9, 11, 12, 14}) {
        const auto* field = PlayablePropertyAuthoringV1::fieldById(id);
        ASSERT_NE(field, nullptr);
        SCOPED_TRACE(field->fieldName);
        ProjectItem parent = makeDefinition(ProjectItem::Type::Container);
        ProjectItem child = makeDefinition(ProjectItem::Type::Sound);
        child.parent = &parent;
        const auto parentLocator = PlayablePropertyAuthoringV1::definitionLocator(parent);
        const auto childLocator = PlayablePropertyAuthoringV1::definitionLocator(child);
        ASSERT_TRUE(parentLocator && childLocator);
        const auto parentValue = exactJsonValue(field->wire, id);
        const auto parentBefore = parent.fieldGroups;
        const auto childBefore = child.fieldGroups;
        for (const VNX::PlatformScope scope : {
                 VNX::PlatformScope{}, VNX::PlatformScope{HarkenTest::PlatformIds::Windows}}) {
            EXPECT_FALSE(PlayablePropertyAuthoringV1::setOverride(
                parent, *parentLocator, id, parentValue, scope));
            EXPECT_FALSE(PlayablePropertyAuthoringV1::setOverride(
                child, *childLocator, id, parentValue, scope));
            EXPECT_FALSE(PlayablePropertyAuthoringV1::clearOverride(
                child, *childLocator, id, scope));
        }
        EXPECT_EQ(parent.fieldGroups, parentBefore);
        EXPECT_EQ(child.fieldGroups, childBefore);
        EXPECT_EQ(parent.playablePropertyPresence.bits, 0u);
        EXPECT_EQ(child.playablePropertyPresence.bits, 0u);
    }
}

TEST(PlayablePropertyAuthoring, SignalChainContributionsAndLoopHaveNoInheritedSource)
{
    for (const uint8_t id : {1, 2, 3, 4}) {
        const auto* field = PlayablePropertyAuthoringV1::fieldById(id);
        ASSERT_NE(field, nullptr);
        SCOPED_TRACE(field->fieldName);
        ProjectItem parent = makeDefinition(ProjectItem::Type::Container);
        ProjectItem child = makeDefinition(ProjectItem::Type::Sound);
        child.parent = &parent;
        const auto parentLocator = PlayablePropertyAuthoringV1::definitionLocator(parent);
        const auto childLocator = PlayablePropertyAuthoringV1::definitionLocator(child);
        ASSERT_TRUE(parentLocator && childLocator);
        ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
            parent, *parentLocator, id, exactJsonValue(field->wire, id), std::nullopt));
        EXPECT_FALSE(PlayablePropertyAuthoringV1::resolveSource(
            child, *childLocator, id, nullptr, {}, std::nullopt));
        ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
            child, *childLocator, id, exactJsonValue(field->wire, id), std::nullopt));
        const auto own = PlayablePropertyAuthoringV1::resolveSource(
            child, *childLocator, id, nullptr, {}, std::nullopt);
        ASSERT_TRUE(own);
        EXPECT_EQ(own->owner, &child);
        ASSERT_TRUE(PlayablePropertyAuthoringV1::clearOverride(
            child, *childLocator, id, std::nullopt));
        EXPECT_FALSE(PlayablePropertyAuthoringV1::resolveSource(
            child, *childLocator, id, nullptr, {}, std::nullopt));
    }
}

TEST(PlayablePropertyAuthoring, MixSourceUsesOwnPresenceForExplicitDefault)
{
    ProjectItem layer = makeDefinition(ProjectItem::Type::Layer);
    ProjectItem sound = makeDefinition(ProjectItem::Type::Sound);
    sound.parent = &layer;
    const auto layerLocator = PlayablePropertyAuthoringV1::definitionLocator(layer);
    const auto soundLocator = PlayablePropertyAuthoringV1::definitionLocator(sound);
    ASSERT_TRUE(layerLocator.has_value());
    ASSERT_TRUE(soundLocator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        layer, *layerLocator, 1, 0.0, std::nullopt));

    auto source = PlayablePropertyAuthoringV1::resolveSource(
        sound, *soundLocator, 1, nullptr, {}, std::nullopt);
    EXPECT_FALSE(source.has_value());

    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        sound, *soundLocator, 1, 0.0, std::nullopt));
    source = PlayablePropertyAuthoringV1::resolveSource(
        sound, *soundLocator, 1, nullptr, {}, std::nullopt);
    ASSERT_TRUE(source.has_value());
    EXPECT_EQ(source->owner, &sound);
    EXPECT_EQ(source->kind, VNX::PlayablePropertySourceV1::Kind::Own);
    EXPECT_FLOAT_EQ(static_cast<float>(source->state.value.toDouble()), 0.0f);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::clearOverride(
        sound, *soundLocator, 1, std::nullopt));
    source = PlayablePropertyAuthoringV1::resolveSource(
        sound, *soundLocator, 1, nullptr, {}, std::nullopt);
    EXPECT_FALSE(source.has_value());
}

TEST(PlayablePropertyAuthoring, NestedPlacementsHaveNoAdditionalMixSource)
{
    ProjectItem outerLayer = makeDefinition(ProjectItem::Type::Layer);
    ProjectItem outerPiece = makeDefinition(ProjectItem::Type::Piece);
    ProjectItem outerTrack = makeDefinition(ProjectItem::Type::Track);
    ProjectItem nestedLayer = makeDefinition(ProjectItem::Type::Layer);
    ProjectItem nestedPiece = makeDefinition(ProjectItem::Type::Piece);
    ProjectItem innerTrack = makeDefinition(ProjectItem::Type::Track);
    ProjectItem sound = makeDefinition(ProjectItem::Type::Sound);
    outerPiece.parent = &outerLayer;
    outerTrack.parent = &outerPiece;
    nestedPiece.parent = &nestedLayer;
    innerTrack.parent = &nestedPiece;

    VNX::Clip outerClip;
    outerClip.clipGuid = QUuid::createUuid();
    outerClip.targetGuid = nestedPiece.guid;
    ASSERT_TRUE(outerTrack.comp<VNX::VTrack>()->clips
                    .mintOccurrenceOrdinal(&outerClip));
    outerTrack.comp<VNX::VTrack>()->clips.items.push_back(outerClip);
    VNX::Clip innerClip;
    innerClip.clipGuid = QUuid::createUuid();
    innerClip.targetGuid = sound.guid;
    ASSERT_TRUE(innerTrack.comp<VNX::VTrack>()->clips
                    .mintOccurrenceOrdinal(&innerClip));
    innerTrack.comp<VNX::VTrack>()->clips.items.push_back(innerClip);

    const auto setVolume = [](ProjectItem& item, double value) {
        const auto locator = PlayablePropertyAuthoringV1::definitionLocator(item);
        ASSERT_TRUE(locator.has_value());
        ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
            item, *locator, 1, value, std::nullopt));
    };
    setVolume(outerLayer, -0.01);
    setVolume(outerPiece, -0.02);
    setVolume(outerTrack, -0.03);
    setVolume(nestedLayer, -0.99);
    setVolume(nestedPiece, -0.05);
    setVolume(innerTrack, -0.06);
    setVolume(sound, -0.08);
    const auto outerLocator = PlayablePropertyAuthoringV1::trackClipLocator(
        outerTrack, outerClip.clipGuid);
    const auto innerLocator = PlayablePropertyAuthoringV1::trackClipLocator(
        innerTrack, innerClip.clipGuid);
    ASSERT_TRUE(outerLocator.has_value());
    ASSERT_TRUE(innerLocator.has_value());
    EXPECT_FALSE(PlayablePropertyAuthoringV1::setOverride(
        outerTrack, *outerLocator, 1, -0.04, std::nullopt));
    EXPECT_FALSE(PlayablePropertyAuthoringV1::setOverride(
        innerTrack, *innerLocator, 1, -0.07, std::nullopt));

    const std::vector<ProjectItem*> items{
        &outerLayer, &outerPiece, &outerTrack, &nestedLayer,
        &nestedPiece, &innerTrack, &sound};
    const auto resolve = [&items](const QUuid& guid) -> ProjectItem* {
        for (ProjectItem* item : items)
            if (item->guid == guid) return item;
        return nullptr;
    };
    const VNX::PlacementContext placement({
        {innerTrack.guid, innerClip.clipGuid, sound.guid},
        {outerTrack.guid, outerClip.clipGuid, nestedPiece.guid}});
    const auto edited = PlayablePropertyAuthoringV1::placementLocator(
        sound, placement, resolve);
    ASSERT_TRUE(edited.has_value());
    auto source = PlayablePropertyAuthoringV1::resolveSource(
        sound, *edited, 1, &placement, resolve, std::nullopt);
    EXPECT_FALSE(source.has_value());
    EXPECT_FALSE(PlayablePropertyAuthoringV1::clearOverride(
        innerTrack, *innerLocator, 1, std::nullopt));
    source = PlayablePropertyAuthoringV1::resolveSource(
        sound, *edited, 1, &placement, resolve, std::nullopt);
    EXPECT_FALSE(source.has_value());
}

TEST(PlayablePropertyAuthoring, ContainerChildHasNoAdditionalMixSource)
{
    ProjectItem layer = makeDefinition(ProjectItem::Type::Layer);
    ProjectItem container = makeDefinition(ProjectItem::Type::Container);
    ProjectItem child = makeDefinition(ProjectItem::Type::Sound);
    container.parent = &layer;
    child.parent = &container;
    container.comp<VNX::VSelection>()->selections.shared.occurrenceOrdinalHighWater = 1;
    container.comp<VNX::VSelection>()->selections.shared.setOccurrenceRows({{child.guid, 0}});
    const auto layerLocator = PlayablePropertyAuthoringV1::definitionLocator(layer);
    const auto containerLocator = PlayablePropertyAuthoringV1::definitionLocator(container);
    const auto childLocator = PlayablePropertyAuthoringV1::definitionLocator(child);
    const auto occurrence = PlayablePropertyAuthoringV1::containerChildLocator(
        container, child.guid);
    ASSERT_TRUE(layerLocator && containerLocator && childLocator && occurrence);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(layer, *layerLocator, 1, -0.01, std::nullopt));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(container, *containerLocator, 1, -0.02, std::nullopt));
    EXPECT_FALSE(PlayablePropertyAuthoringV1::setOverride(container, *occurrence, 1, -0.03, std::nullopt));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(child, *childLocator, 1, -0.04, std::nullopt));
    const auto resolve = [&](const QUuid& guid) -> ProjectItem* {
        if (guid == layer.guid) return &layer;
        if (guid == container.guid) return &container;
        if (guid == child.guid) return &child;
        return nullptr;
    };
    auto source = PlayablePropertyAuthoringV1::resolveSource(
        child, *occurrence, 1, nullptr, resolve, std::nullopt);
    EXPECT_FALSE(source.has_value());
    EXPECT_FALSE(PlayablePropertyAuthoringV1::clearOverride(container, *occurrence, 1, std::nullopt));
    source = PlayablePropertyAuthoringV1::resolveSource(
        child, *occurrence, 1, nullptr, resolve, std::nullopt);
    EXPECT_FALSE(source.has_value());
}

TEST(PlayablePropertyAuthoring, ExplicitDefaultsAndLocalEmptySendRemainAuthored)
{
    ProjectItem sound = makeDefinition(ProjectItem::Type::Sound);
    sound.playablePropertyPresence.bits = UINT64_C(0x0000000000000009);
    sound.comp<VMixNode>()->volume = 0.0f;
    sound.comp<VAudio>()->loopCount = 1;
    sound.fieldGroups.send.shared = {
        VNX::FieldGroupMode::Local, VNX::SendFieldGroup{}};

    VNX::ProjectSerializer serializer;
    const QJsonObject json = serializer.projectItemToJson(sound);
    const QJsonObject envelope = json.value(QLatin1String(kEnvelopeJsonKeyV1)).toObject();
    EXPECT_EQ(envelope.value(QLatin1String(kPresenceJsonKeyV1)).toString(),
              QStringLiteral("0000000000000009"));
    EXPECT_TRUE(envelope.contains(QStringLiteral("volume")));
    EXPECT_EQ(envelope.value(QStringLiteral("volume")).toDouble(), 0.0);
    EXPECT_TRUE(envelope.contains(QStringLiteral("loopCount")));
    EXPECT_EQ(envelope.value(QStringLiteral("loopCount")).toInt(), 1);
    EXPECT_FALSE(envelope.contains(QStringLiteral("sends")));
    const QJsonObject send = json.value(QStringLiteral("fieldGroups")).toObject()
        .value(QStringLiteral("shared")).toObject().value(QStringLiteral("send")).toObject();
    EXPECT_EQ(send.value(QStringLiteral("inherit")), QJsonValue(false));
    EXPECT_EQ(send.value(QStringLiteral("values")).toObject()
                  .value(QStringLiteral("sends")).toArray(), QJsonArray{});

    ProjectItem restored;
    ASSERT_TRUE(serializer.projectItemFromJson(json, restored));
    EXPECT_EQ(restored.fieldGroups, sound.fieldGroups);
    EXPECT_EQ(serializer.projectItemToJson(restored), json);
}

TEST(PlayablePropertyAuthoring, AbsentValueNormalizesAndIsOmittedOnSave)
{
    QJsonObject json = serializedDefinition(ProjectItem::Type::Sound);
    QJsonObject envelope = json.value(QLatin1String(kEnvelopeJsonKeyV1)).toObject();
    envelope[QStringLiteral("volume")] = 18.0;
    json[QLatin1String(kEnvelopeJsonKeyV1)] = envelope;

    VNX::ProjectSerializer serializer;
    ProjectItem restored;
    ASSERT_TRUE(serializer.projectItemFromJson(json, restored));
    EXPECT_EQ(restored.comp<VMixNode>()->volume, 0.0f);
    const QJsonObject saved = serializer.projectItemToJson(restored)
        .value(QLatin1String(kEnvelopeJsonKeyV1)).toObject();
    EXPECT_FALSE(saved.contains(QStringLiteral("volume")));
    EXPECT_EQ(saved.value(QLatin1String(kPresenceJsonKeyV1)).toString(),
              QStringLiteral("0000000000000000"));
}

TEST(PlayablePropertyAuthoring, InvalidPresenceSpellingsAndUnknownBitsAreRejected)
{
    const QString invalidMasks[] = {
        QStringLiteral("000000000000000"),
        QStringLiteral("00000000000000AF"),
        QStringLiteral("0x0000000000000000"),
        QStringLiteral("0000000000010000"),
        QStringLiteral("0000000000000200"),
    };
    for (const QString& mask : invalidMasks) {
        QJsonObject json = serializedDefinition(ProjectItem::Type::Container);
        QJsonObject envelope = json.value(QLatin1String(kEnvelopeJsonKeyV1)).toObject();
        envelope[QLatin1String(kPresenceJsonKeyV1)] = mask;
        json[QLatin1String(kEnvelopeJsonKeyV1)] = envelope;
        ProjectItem restored;
        EXPECT_FALSE(loadDefinition(json, &restored)) << mask.toStdString();
    }
}

TEST(PlayablePropertyAuthoring, PresentFieldWithoutValueIsRejected)
{
    QJsonObject json = serializedDefinition(ProjectItem::Type::Piece);
    QJsonObject envelope = json.value(QLatin1String(kEnvelopeJsonKeyV1)).toObject();
    envelope[QLatin1String(kPresenceJsonKeyV1)] = QStringLiteral("0000000000000001");
    json[QLatin1String(kEnvelopeJsonKeyV1)] = envelope;
    ProjectItem restored;
    EXPECT_FALSE(loadDefinition(json, &restored));
}

TEST(PlayablePropertyAuthoring, LoopSlotFollowsTheDeclaredLoopPolicy)
{
    for (const ProjectItem::Type type : VNX::ItemTypeCatalog::allTypes()) {
        EXPECT_EQ(PlayablePropertyAuthoringV1::isSlotApplicable(
                      type, static_cast<uint8_t>(PlayablePropertyIdV1::LoopCount)),
                  VNX::ItemTypeCatalog::allowsLoop(type))
            << VNX::ItemTypeCatalog::canonicalName(type).toStdString();
    }
}

TEST(PlayablePropertyAuthoring, PresenceOnSlotTheTypeDoesNotCarryIsRejected)
{
    QJsonObject json = serializedDefinition(ProjectItem::Type::Layer);
    QJsonObject envelope = json.value(QLatin1String(kEnvelopeJsonKeyV1)).toObject();
    envelope[QLatin1String(kPresenceJsonKeyV1)] = QStringLiteral("0000000000000008");
    envelope[QStringLiteral("loopCount")] = 3;
    json[QLatin1String(kEnvelopeJsonKeyV1)] = envelope;
    ProjectItem restored;
    EXPECT_FALSE(loadDefinition(json, &restored));
}

TEST(PlayablePropertyAuthoring, ComponentJsonHasNoRegistryOwnedSecondAuthority)
{
    ProjectItem sound = makeDefinition(ProjectItem::Type::Sound);
    ASSERT_NE(sound.comp<VMixNode>(), nullptr);
    ASSERT_NE(sound.comp<VAudio>(), nullptr);
    ASSERT_NE(sound.comp<VSpatial>(), nullptr);
    sound.playablePropertyPresence.bits = UINT64_C(0x1f);
    VNX::ProjectSerializer serializer;
    const QJsonObject json = serializer.projectItemToJson(sound);
    ASSERT_FALSE(json.isEmpty());
    const QJsonObject components = json.value(QStringLiteral("components")).toObject();
    for (const auto& field : kJsonFieldsV1) {
        const QJsonObject component = components.value(
            QLatin1String(field.component)).toObject();
        if (PlayablePropertyAuthoringV1::isComponentStored(field.id)) {
            EXPECT_TRUE(component.contains(QLatin1String(field.storageField)));
            continue;
        }
        EXPECT_FALSE(component.contains(QLatin1String(field.storageField)));
        EXPECT_FALSE(component.contains(QLatin1String(field.jsonKey)));
    }
}

TEST(PlayablePropertyAuthoring, BusMixFieldsRoundTripInsideDefinitionEnvelope)
{
    auto target = std::make_shared<ProjectItem>(
        QStringLiteral("Target"), ProjectItem::Type::Bus);
    target->guid = QUuid::createUuid();
    target->objectID = 42;
    target->attachDefaultComponents();
    auto child = std::make_shared<ProjectItem>(
        QStringLiteral("definition"), ProjectItem::Type::Bus);
    child->guid = QUuid::createUuid();
    child->objectID = 43;
    child->parent = target.get();
    child->attachDefaultComponents();
    target->children.push_back(child);
    ProjectItem& bus = *child;
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(bus);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        bus, *locator, static_cast<uint8_t>(PlayablePropertyIdV1::Volume), -6.0, std::nullopt));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        bus, *locator, static_cast<uint8_t>(PlayablePropertyIdV1::Pitch), 2.0, std::nullopt));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        bus, *locator, static_cast<uint8_t>(PlayablePropertyIdV1::OutputBusVolume), -3.0, std::nullopt));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        bus, *locator, static_cast<uint8_t>(PlayablePropertyIdV1::Sends),
        QJsonArray{QJsonObject{
            {QStringLiteral("targetBusID"), 42},
            {QStringLiteral("sendLevel"),
             static_cast<double>(VMixNode::linearToDb(0.5f))}}}, std::nullopt));
    bus.comp<VMixNode>()->effects = QJsonArray{QJsonObject{{"type", "gain"}}};

    VNX::ProjectSerializer serializer;
    VNX::ProjectData project;
    project.busses.push_back(
        {target, QStringLiteral("Busses")});
    ASSERT_TRUE(serializer.prepareReferenceWrite(project))
        << serializer.lastError().toStdString();
    const QJsonObject json = serializer.projectItemToJson(bus);
    ASSERT_TRUE(json.value(QLatin1String(kEnvelopeJsonKeyV1)).isObject());
    const QJsonObject envelope = json.value(QLatin1String(kEnvelopeJsonKeyV1)).toObject();
    EXPECT_TRUE(envelope.contains(QStringLiteral("volume")));
    EXPECT_TRUE(envelope.contains(QStringLiteral("pitch")));
    EXPECT_TRUE(envelope.contains(QStringLiteral("outputBusVolume")));
    EXPECT_TRUE(envelope.contains(QStringLiteral("sends")));
    EXPECT_FALSE(envelope.contains(QStringLiteral("mainOutputBusRef")));
    const QJsonObject mixJson = json.value(QStringLiteral("components"))
        .toObject().value(QStringLiteral("mixnode")).toObject();
    EXPECT_FALSE(mixJson.contains(QStringLiteral("volume")));
    EXPECT_FALSE(mixJson.contains(QStringLiteral("pitch")));
    EXPECT_FALSE(mixJson.contains(QStringLiteral("outputBusVolume")));
    EXPECT_FALSE(mixJson.contains(QStringLiteral("sends")));
    EXPECT_TRUE(mixJson.contains(QStringLiteral("effects")));

    ProjectItem restored;
    ASSERT_TRUE(serializer.projectItemFromJson(json, restored));
    ASSERT_NE(restored.comp<VMixNode>(), nullptr);
    EXPECT_FLOAT_EQ(restored.comp<VMixNode>()->volume, -6.0f);
    EXPECT_FLOAT_EQ(restored.comp<VMixNode>()->pitch, 2.0f);
    EXPECT_FLOAT_EQ(restored.comp<VMixNode>()->outputBusVolume, -3.0f);
    ASSERT_EQ(restored.comp<VMixNode>()->sends.size(), 1u);
    EXPECT_EQ(restored.comp<VMixNode>()->sends[0].targetBusID, 42u);
    EXPECT_NEAR(restored.comp<VMixNode>()->sends[0].sendLevel, 0.5f, 1.0e-6f);
    EXPECT_TRUE(restored.playablePropertyPresence.Has(PlayablePropertyIdV1::Volume));
    EXPECT_TRUE(restored.playablePropertyPresence.Has(PlayablePropertyIdV1::Sends));
    EXPECT_FALSE(restored.playablePropertyPresence.Has(
        PlayablePropertyIdV1::MainOutputBusRef));
}

TEST(PlayablePropertyAuthoring, DefinitionComponentDuplicateAuthorityIsRejected)
{
    VNX::ProjectSerializer serializer;
    const QJsonObject canonical = serializedDefinition(ProjectItem::Type::Sound);
    for (const auto& field : kJsonFieldsV1) {
        if (PlayablePropertyAuthoringV1::isComponentStored(field.id)) continue;
        const QString keys[] = {
            QLatin1String(field.storageField),
            QLatin1String(field.jsonKey),
        };
        for (const QString& key : keys) {
            QJsonObject json = canonical;
            QJsonObject components = json.value(QStringLiteral("components")).toObject();
            QJsonObject component = components.value(
                QLatin1String(field.component)).toObject();
            component[key] = QJsonValue::Null;
            components[QLatin1String(field.component)] = component;
            json[QStringLiteral("components")] = components;
            ProjectItem restored;
            EXPECT_FALSE(serializer.projectItemFromJson(json, restored))
                << field.fieldName << ": " << key.toStdString();
        }
    }
}

TEST(PlayablePropertyAuthoring, LegacyOutputBusRoutingCannotAuthorSoundGroup)
{
    if (!qApp) {
        static int argc = 1;
        static char arg0[] = "harken_editor_tests";
        static char* argv[] = {arg0, nullptr};
        static QApplication app(argc, argv);
        Q_UNUSED(app);
    }
    auto& actions = VNX::ActionManager::instance();
    actions.clearAllForTesting();
    VNX::registerMixerActions(actions, nullptr, &VNX::ProjectManager::instance(), nullptr);
    auto& manager = VNX::ProjectManager::instance();
    if (manager.isProjectOpen()) manager.closeProject();
    VNX::UndoRedoManager::instance().clear();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("WriteBusPropertySilentSound");
    config.location = tempDir.path();
    ASSERT_TRUE(manager.createProject(config));
    ProjectItem* layer = manager.createItem(
        nullptr, QStringLiteral("Layer"), ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    ProjectItem* sound = manager.createItem(
        layer, QStringLiteral("Sound"), ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(sound->comp<VMixNode>(), nullptr);
    EXPECT_FALSE(sound->playablePropertyPresence.Has(
        PlayablePropertyIdV1::OutputBusVolume));
    const auto result = actions.execute(
        QStringLiteral("set_output_bus_routing"),
        QJsonObject{
            {QStringLiteral("path"), QStringLiteral("Layers/Layer/Sound")},
            {QStringLiteral("volume_db"), -6.0}});
    EXPECT_FALSE(result.success);
    EXPECT_FLOAT_EQ(sound->comp<VMixNode>()->outputBusVolume, 0.0f);
    EXPECT_FALSE(sound->playablePropertyPresence.Has(
        PlayablePropertyIdV1::OutputBusVolume));
    EXPECT_EQ(sound->fieldGroups, VNX::DefinitionFieldGroups{});
    VNX::UndoRedoManager::instance().clear();
    manager.closeProject();
    actions.clearAllForTesting();
}

TEST(PlayablePropertyAuthoring, LegacySendActionsCannotAuthorSoundGroup)
{
    if (!qApp) {
        static int argc = 1;
        static char arg0[] = "harken_editor_tests";
        static char* argv[] = {arg0, nullptr};
        static QApplication app(argc, argv);
        Q_UNUSED(app);
    }
    auto& actions = VNX::ActionManager::instance();
    actions.clearAllForTesting();
    VNX::registerMixerActions(actions, nullptr, &VNX::ProjectManager::instance(), nullptr);
    auto& manager = VNX::ProjectManager::instance();
    if (manager.isProjectOpen()) manager.closeProject();
    VNX::UndoRedoManager::instance().clear();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("SoundSendWriteback");
    config.location = tempDir.path();
    ASSERT_TRUE(manager.createProject(config));
    ProjectItem* master = manager.findBusItem(VNX::kMasterBusID);
    ASSERT_NE(master, nullptr);
    ProjectItem* target = manager.createItem(
        master, QStringLiteral("SendTarget"), ProjectItem::Type::Bus);
    ASSERT_NE(target, nullptr);
    ProjectItem* layer = manager.createItem(
        nullptr, QStringLiteral("Layer"), ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    ProjectItem* sound = manager.createItem(
        layer, QStringLiteral("Sound"), ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    auto* mix = sound->compOrCreate<VMixNode>();
    ASSERT_NE(mix, nullptr);
    EXPECT_FALSE(sound->playablePropertyPresence.Has(PlayablePropertyIdV1::Sends));
    const auto added = actions.execute(
        QStringLiteral("set_send"),
        QJsonObject{
            {QStringLiteral("src_bus"), QStringLiteral("Layers/Layer/Sound")},
            {QStringLiteral("target_bus_id"),
             static_cast<qint64>(target->objectID)},
            {QStringLiteral("level"), 0.5}});
    EXPECT_FALSE(added.success);
    EXPECT_TRUE(mix->sends.empty());
    EXPECT_FALSE(sound->playablePropertyPresence.Has(PlayablePropertyIdV1::Sends));
    const auto removed = actions.execute(
        QStringLiteral("remove_send"),
        QJsonObject{
            {QStringLiteral("src_bus"), QStringLiteral("Layers/Layer/Sound")},
            {QStringLiteral("target_bus_id"),
             static_cast<qint64>(target->objectID)}});
    EXPECT_FALSE(removed.success);
    EXPECT_TRUE(mix->sends.empty());
    EXPECT_FALSE(sound->playablePropertyPresence.Has(PlayablePropertyIdV1::Sends));
    EXPECT_EQ(sound->fieldGroups, VNX::DefinitionFieldGroups{});
    VNX::UndoRedoManager::instance().clear();
    manager.closeProject();
    actions.clearAllForTesting();
}

TEST(PlayablePropertyAuthoring, BusSlotSevenIsInapplicableAndRejected)
{
    ProjectItem bus = makeDefinition(ProjectItem::Type::Bus);
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(bus);
    ASSERT_TRUE(locator.has_value());
    const auto id = static_cast<uint8_t>(PlayablePropertyIdV1::MainOutputBusRef);
    EXPECT_FALSE(PlayablePropertyAuthoringV1::isSlotApplicable(bus.type, id));
    EXPECT_FALSE(PlayablePropertyAuthoringV1::setOverride(
        bus, *locator, id, QStringLiteral("1"), std::nullopt));
    EXPECT_FALSE(PlayablePropertyAuthoringV1::readProperty(bus, *locator, id, std::nullopt).has_value());

    QJsonObject json = serializedDefinition(ProjectItem::Type::Bus);
    QJsonObject envelope = json.value(QLatin1String(kEnvelopeJsonKeyV1)).toObject();
    envelope[QLatin1String(kPresenceJsonKeyV1)] = QStringLiteral("0000000000000040");
    envelope[QStringLiteral("mainOutputBusRef")] = QStringLiteral("1");
    json[QLatin1String(kEnvelopeJsonKeyV1)] = envelope;
    ProjectItem restored;
    EXPECT_FALSE(loadDefinition(json, &restored));
}

TEST(PlayablePropertyAuthoring, BusResolveSourceNeverInheritsFromParentBus)
{
    ProjectItem parent = makeDefinition(ProjectItem::Type::Bus);
    ProjectItem child = makeDefinition(ProjectItem::Type::Bus);
    child.parent = &parent;
    const auto parentLocator = PlayablePropertyAuthoringV1::definitionLocator(parent);
    const auto childLocator = PlayablePropertyAuthoringV1::definitionLocator(child);
    ASSERT_TRUE(parentLocator.has_value());
    ASSERT_TRUE(childLocator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        parent, *parentLocator, static_cast<uint8_t>(PlayablePropertyIdV1::Volume), -12.0, std::nullopt));

    auto source = PlayablePropertyAuthoringV1::resolveSource(
        child, *childLocator, static_cast<uint8_t>(PlayablePropertyIdV1::Volume),
        nullptr, {}, std::nullopt);
    EXPECT_FALSE(source.has_value());

    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        child, *childLocator, static_cast<uint8_t>(PlayablePropertyIdV1::Volume), -3.0, std::nullopt));
    source = PlayablePropertyAuthoringV1::resolveSource(
        child, *childLocator, static_cast<uint8_t>(PlayablePropertyIdV1::Volume),
        nullptr, {}, std::nullopt);
    ASSERT_TRUE(source.has_value());
    EXPECT_EQ(source->owner, &child);
    EXPECT_EQ(source->kind, VNX::PlayablePropertySourceV1::Kind::Own);
    EXPECT_TRUE(source->state.present);
    EXPECT_FLOAT_EQ(static_cast<float>(source->state.value.toDouble()), -3.0f);
}

TEST(PlayablePropertyAuthoring, BusSidecarRoundTripStripsMixKeysAndKeepsLedgers)
{
    auto parent = std::make_shared<ProjectItem>(
        QStringLiteral("Master"), ProjectItem::Type::Bus);
    parent->guid = QUuid::createUuid();
    parent->objectID = 1;
    parent->attachDefaultComponents();
    auto child = std::make_shared<ProjectItem>(
        QStringLiteral("Mix"), ProjectItem::Type::Bus);
    child->guid = QUuid::createUuid();
    child->objectID = 2;
    child->parent = parent.get();
    child->attachDefaultComponents();
    parent->children.push_back(child);
    ProjectItem& bus = *child;
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(bus);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        bus, *locator, static_cast<uint8_t>(PlayablePropertyIdV1::Volume), -6.0, std::nullopt));
    bus.comp<VMixNode>()->effects = QJsonArray{QJsonObject{{"type", "gain"}}};
    bus.comp<VMixNode>()->bindings = VNX::BindingLedger::fromJson(
        QJsonArray{QJsonObject{{QStringLiteral("id"), 1}}});

    VNX::ProjectSerializer serializer;
    VNX::ProjectData project;
    project.busses.push_back(
        {parent, QStringLiteral("Busses")});
    ASSERT_TRUE(serializer.prepareReferenceWrite(project))
        << serializer.lastError().toStdString();
    const QJsonObject json = serializer.busPackToJson(bus);
    ASSERT_TRUE(json.value(QLatin1String(kEnvelopeJsonKeyV1)).isObject());
    const QJsonObject mixJson = json.value(QStringLiteral("components"))
        .toObject().value(QStringLiteral("mixnode")).toObject();
    EXPECT_FALSE(mixJson.contains(QStringLiteral("volume")));
    EXPECT_TRUE(mixJson.contains(QStringLiteral("effects")));
    EXPECT_TRUE(mixJson.contains(QStringLiteral("bindings")));

    ProjectItem bucket(QStringLiteral("Busses"), ProjectItem::Type::None);
    std::unordered_map<uint32_t, uint32_t> edges;
    ProjectItem* restored = serializer.busPackFromJson(json, bucket, edges);
    ASSERT_NE(restored, nullptr);
    EXPECT_FLOAT_EQ(restored->comp<VMixNode>()->volume, -6.0f);
    EXPECT_TRUE(restored->playablePropertyPresence.Has(PlayablePropertyIdV1::Volume));
}

TEST(PlayablePropertyAuthoring, BusSidecarDuplicateAuthorityIsRejected)
{
    ProjectItem bus = makeDefinition(ProjectItem::Type::Bus);
    VNX::ProjectSerializer serializer;
    QJsonObject json = serializer.busPackToJson(bus);
    QJsonObject components = json.value(QStringLiteral("components")).toObject();
    QJsonObject mixnode = components.value(QStringLiteral("mixnode")).toObject();
    mixnode[QStringLiteral("volume")] = -6.0;
    components[QStringLiteral("mixnode")] = mixnode;
    json[QStringLiteral("components")] = components;

    ProjectItem bucket(QStringLiteral("Busses"), ProjectItem::Type::None);
    std::unordered_map<uint32_t, uint32_t> edges;
    EXPECT_EQ(serializer.busPackFromJson(json, bucket, edges), nullptr);
}

TEST(PlayablePropertyAuthoring, BusSidecarMissingEnvelopeIsRejected)
{
    ProjectItem bus = makeDefinition(ProjectItem::Type::Bus);
    VNX::ProjectSerializer serializer;
    QJsonObject json = serializer.busPackToJson(bus);
    json.remove(QLatin1String(kEnvelopeJsonKeyV1));
    ProjectItem bucket(QStringLiteral("Busses"), ProjectItem::Type::None);
    std::unordered_map<uint32_t, uint32_t> edges;
    EXPECT_EQ(serializer.busPackFromJson(json, bucket, edges), nullptr);
}

TEST(PlayablePropertyAuthoring, SavedBusPackOnDiskHasEnvelopeWithoutMixKeys)
{
    if (!qApp) {
        static int argc = 1;
        static char arg0[] = "harken_editor_tests";
        static char* argv[] = {arg0, nullptr};
        static QApplication app(argc, argv);
        Q_UNUSED(app);
    }
    auto& manager = VNX::ProjectManager::instance();
    if (manager.isProjectOpen()) manager.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("BusSidecarDisk");
    config.location = tempDir.path();
    ASSERT_TRUE(manager.createProject(config));
    ProjectItem* master = manager.getMasterBusItem();
    ASSERT_NE(master, nullptr);
    ProjectItem* bus = manager.createItem(
        master, QStringLiteral("Mix_World"), ProjectItem::Type::Bus);
    ASSERT_NE(bus, nullptr);
    auto& actions = VNX::ActionManager::instance();
    actions.clearAllForTesting();
    VNX::registerMixerActions(actions, nullptr, &manager, nullptr);
    const auto written = actions.execute(
        QStringLiteral("set_bus_properties"),
        QJsonObject{
            {QStringLiteral("bus_id"), static_cast<qint64>(bus->objectID)},
            {QStringLiteral("volume"), -6.0}});
    ASSERT_TRUE(written.success) << written.errorMessage.toStdString();
    const QString projectPath = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.saveProject());
    const QString packPath = QDir(QFileInfo(projectPath).absolutePath())
        .filePath(QStringLiteral("Busses/") + VNX::NameValidator::objectFileName(bus->name, bus->guid));
    QFile file(packPath);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly)) << packPath.toStdString();
    VNX::ProjectDocumentData document;
    QString documentError;
    ASSERT_TRUE(VNX::ProjectDocument::parse(
        QJsonDocument::fromJson(file.readAll()).object(),
        QStringLiteral("project.bus"), &document, &documentError))
        << documentError.toStdString();
    const QJsonObject json = document.content;
    ASSERT_TRUE(json.value(QLatin1String(kEnvelopeJsonKeyV1)).isObject());
    const QJsonObject mixJson = json.value(QStringLiteral("components"))
        .toObject().value(QStringLiteral("mixnode")).toObject();
    EXPECT_FALSE(mixJson.contains(QStringLiteral("volume")));
    EXPECT_TRUE(json.value(QLatin1String(kEnvelopeJsonKeyV1)).toObject()
                    .contains(QStringLiteral("volume")));
    VNX::UndoRedoManager::instance().clear();
    manager.closeProject();
    actions.clearAllForTesting();
}

TEST(PlayablePropertyAuthoring, SetBusPropertiesSetsPresenceAndUndoRestoresIt)
{
    if (!qApp) {
        static int argc = 1;
        static char arg0[] = "harken_editor_tests";
        static char* argv[] = {arg0, nullptr};
        static QApplication app(argc, argv);
        Q_UNUSED(app);
    }
    auto& actions = VNX::ActionManager::instance();
    actions.clearAllForTesting();
    VNX::registerMixerActions(actions, nullptr, &VNX::ProjectManager::instance(), nullptr);
    auto& manager = VNX::ProjectManager::instance();
    if (manager.isProjectOpen()) manager.closeProject();
    VNX::UndoRedoManager::instance().clear();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("BusWritePresence");
    config.location = tempDir.path();
    ASSERT_TRUE(manager.createProject(config));
    ProjectItem* master = manager.getMasterBusItem();
    ASSERT_NE(master, nullptr);
    ProjectItem* bus = manager.createItem(
        master, QStringLiteral("GainBus"), ProjectItem::Type::Bus);
    ASSERT_NE(bus, nullptr);
    auto* stack = VNX::UndoRedoManager::instance().undoStack();
    const int countBefore = stack->count();
    const auto result = actions.execute(
        QStringLiteral("set_bus_properties"),
        QJsonObject{
            {QStringLiteral("bus_id"), static_cast<qint64>(bus->objectID)},
            {QStringLiteral("volume"), -6.0}});
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_FLOAT_EQ(bus->comp<VMixNode>()->volume, -6.0f);
    EXPECT_TRUE(bus->playablePropertyPresence.Has(PlayablePropertyIdV1::Volume));
    EXPECT_EQ(stack->count(), countBefore + 1);
    ASSERT_TRUE(stack->canUndo());
    stack->undo();
    EXPECT_FLOAT_EQ(bus->comp<VMixNode>()->volume, 0.0f);
    EXPECT_FALSE(bus->playablePropertyPresence.Has(PlayablePropertyIdV1::Volume));
    stack->redo();
    EXPECT_FLOAT_EQ(bus->comp<VMixNode>()->volume, -6.0f);
    EXPECT_TRUE(bus->playablePropertyPresence.Has(PlayablePropertyIdV1::Volume));
    VNX::UndoRedoManager::instance().clear();
    manager.closeProject();
    actions.clearAllForTesting();
}

TEST(PlayablePropertyAuthoring, UndoableFalseBusVolumeActionsStillSetPresence)
{
    if (!qApp) {
        static int argc = 1;
        static char arg0[] = "harken_editor_tests";
        static char* argv[] = {arg0, nullptr};
        static QApplication app(argc, argv);
        Q_UNUSED(app);
    }
    auto& actions = VNX::ActionManager::instance();
    actions.clearAllForTesting();
    VNX::registerMixerActions(actions, nullptr, &VNX::ProjectManager::instance(), nullptr);
    auto& manager = VNX::ProjectManager::instance();
    if (manager.isProjectOpen()) manager.closeProject();
    VNX::UndoRedoManager::instance().clear();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("BusSilentPresence");
    config.location = tempDir.path();
    ASSERT_TRUE(manager.createProject(config));
    ProjectItem* master = manager.getMasterBusItem();
    ASSERT_NE(master, nullptr);
    const auto volumeDb = actions.execute(
        QStringLiteral("set_bus_volume_db"),
        QJsonObject{
            {QStringLiteral("bus_name"), master->name},
            {QStringLiteral("db"), -4.0}});
    ASSERT_TRUE(volumeDb.success) << volumeDb.errorMessage.toStdString();
    EXPECT_TRUE(master->playablePropertyPresence.Has(PlayablePropertyIdV1::Volume));
    const auto mixerVolume = actions.execute(
        QStringLiteral("set_mixer_volume"),
        QJsonObject{
            {QStringLiteral("bus"), master->name},
            {QStringLiteral("volume_db"), -8.0}});
    ASSERT_TRUE(mixerVolume.success) << mixerVolume.errorMessage.toStdString();
    EXPECT_FLOAT_EQ(master->comp<VMixNode>()->volume, -8.0f);
    EXPECT_TRUE(master->playablePropertyPresence.Has(PlayablePropertyIdV1::Volume));
    VNX::UndoRedoManager::instance().clear();
    manager.closeProject();
    actions.clearAllForTesting();
}

TEST(PlayablePropertyAuthoring, SetSendOnBusKeepsLinearLevelAndRejectsCycle)
{
    if (!qApp) {
        static int argc = 1;
        static char arg0[] = "harken_editor_tests";
        static char* argv[] = {arg0, nullptr};
        static QApplication app(argc, argv);
        Q_UNUSED(app);
    }
    auto& actions = VNX::ActionManager::instance();
    actions.clearAllForTesting();
    VNX::registerMixerActions(actions, nullptr, &VNX::ProjectManager::instance(), nullptr);
    auto& manager = VNX::ProjectManager::instance();
    if (manager.isProjectOpen()) manager.closeProject();
    VNX::UndoRedoManager::instance().clear();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("BusSendWriteback");
    config.location = tempDir.path();
    ASSERT_TRUE(manager.createProject(config));
    ProjectItem* master = manager.getMasterBusItem();
    ASSERT_NE(master, nullptr);
    ProjectItem* child = manager.createItem(
        master, QStringLiteral("Child"), ProjectItem::Type::Bus);
    ASSERT_NE(child, nullptr);
    ProjectItem* sibling = manager.createItem(
        master, QStringLiteral("Sibling"), ProjectItem::Type::Bus);
    ASSERT_NE(sibling, nullptr);

    const auto toParent = actions.execute(
        QStringLiteral("set_send"),
        QJsonObject{
            {QStringLiteral("src_bus"), child->name},
            {QStringLiteral("target_bus_id"), static_cast<qint64>(master->objectID)},
            {QStringLiteral("level"), 0.5}});
    EXPECT_FALSE(toParent.success);

    const auto added = actions.execute(
        QStringLiteral("set_send"),
        QJsonObject{
            {QStringLiteral("src_bus"), child->name},
            {QStringLiteral("target_bus_id"), static_cast<qint64>(sibling->objectID)},
            {QStringLiteral("level"), 0.5}});
    ASSERT_TRUE(added.success) << added.errorMessage.toStdString();
    ASSERT_EQ(child->comp<VMixNode>()->sends.size(), 1u);
    EXPECT_NEAR(child->comp<VMixNode>()->sends[0].sendLevel, 0.5f, 1.0e-4f);
    EXPECT_TRUE(child->playablePropertyPresence.Has(PlayablePropertyIdV1::Sends));

    const auto back = actions.execute(
        QStringLiteral("set_send"),
        QJsonObject{
            {QStringLiteral("src_bus"), sibling->name},
            {QStringLiteral("target_bus_id"), static_cast<qint64>(child->objectID)},
            {QStringLiteral("level"), 0.5}});
    EXPECT_FALSE(back.success);

    VNX::UndoRedoManager::instance().clear();
    manager.closeProject();
    actions.clearAllForTesting();
}

TEST(PlayablePropertyAuthoring, ExistingNonRegistryComponentJsonRoundTrips)
{
    ProjectItem sound = makeDefinition(ProjectItem::Type::Sound);
    sound.comp<VAudio>()->priority = 77;
    sound.comp<VMixNode>()->effects = QJsonArray{QJsonObject{{"type", "gain"}}};

    VNX::ProjectSerializer serializer;
    const QJsonObject json = serializer.projectItemToJson(sound);
    ProjectItem restored;
    ASSERT_TRUE(serializer.projectItemFromJson(json, restored));
    EXPECT_EQ(restored.comp<VAudio>()->priority, 77u);
    const QJsonArray expectedEffects{QJsonObject{{"type", "gain"}}};
    EXPECT_EQ(restored.comp<VMixNode>()->effects, expectedEffects);
}

TEST(PlayablePropertyAuthoring, OccurrenceCodecPreservesExplicitMixAndLoopDefaults)
{
    PlayablePropertyOccurrenceV1 occurrence;
    occurrence.presence.Set(PlayablePropertyIdV1::Volume);
    occurrence.presence.Set(PlayablePropertyIdV1::LoopCount);
    ASSERT_TRUE(occurrence.setValue(PlayablePropertyIdV1::Volume, 0.0f));
    ASSERT_TRUE(occurrence.setValue(PlayablePropertyIdV1::LoopCount, 1u));

    const QJsonObject encoded = PlayablePropertyAuthoringV1::occurrenceToJson(
        occurrence);
    EXPECT_EQ(encoded.value(QLatin1String(kPresenceJsonKeyV1)).toString(),
              QStringLiteral("0000000000000009"));
    EXPECT_TRUE(encoded.contains(QStringLiteral("volume")));
    EXPECT_TRUE(encoded.contains(QStringLiteral("loopCount")));
    EXPECT_FALSE(encoded.contains(QStringLiteral("sends")));

    const auto restored = PlayablePropertyAuthoringV1::occurrenceFromJson(encoded);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(*restored, occurrence);
    EXPECT_EQ(PlayablePropertyAuthoringV1::occurrenceToJson(*restored), encoded);
}

TEST(PlayablePropertyAuthoring,
     OccurrenceCodecUsesGeneratedExactWireTypesForRemainingFields)
{
    PlayablePropertyOccurrenceV1 occurrence;
    for (const auto& field : kJsonFieldsV1) {
        if (PlayablePropertyAuthoringV1::isComponentStored(field.id)) continue;
        const auto id = static_cast<PlayablePropertyIdV1>(field.id);
        if (VNX::FieldGroupForProperty(id)) continue;
        occurrence.presence.Set(id);
        ASSERT_TRUE(occurrence.setValue(
            id, exactOccurrenceValue(field.wire, field.id)));
        const auto* stored = occurrence.value(id);
        ASSERT_NE(stored, nullptr);
        if (field.wire == PlayablePropertyWireV1::F32)
            EXPECT_TRUE(std::holds_alternative<float>(*stored));
        else if (field.wire == PlayablePropertyWireV1::U8)
            EXPECT_TRUE(std::holds_alternative<bool>(*stored));
        else if (field.wire == PlayablePropertyWireV1::U32)
            EXPECT_TRUE(std::holds_alternative<uint32_t>(*stored));
        else if (field.wire == PlayablePropertyWireV1::ObjectId)
            EXPECT_TRUE(std::holds_alternative<harkenstudio::ObjectId>(*stored));
        else
            EXPECT_TRUE(std::holds_alternative<QJsonArray>(*stored));
    }
    ASSERT_TRUE(occurrence.hasExactWireTypes());

    const QJsonObject encoded = PlayablePropertyAuthoringV1::occurrenceToJson(
        occurrence);
    EXPECT_EQ(encoded.value(QLatin1String(kPresenceJsonKeyV1)).toString(),
              QStringLiteral("000000000000001f"));
    for (const auto& field : kJsonFieldsV1) {
        if (PlayablePropertyAuthoringV1::isComponentStored(field.id)) {
            EXPECT_FALSE(encoded.contains(QLatin1String(field.jsonKey)));
            continue;
        }
        if (VNX::FieldGroupForProperty(static_cast<PlayablePropertyIdV1>(field.id))) {
            EXPECT_FALSE(encoded.contains(QLatin1String(field.jsonKey)));
            continue;
        }
        const QJsonValue value = encoded.value(QLatin1String(field.jsonKey));
        if (field.wire == PlayablePropertyWireV1::F32
            || field.wire == PlayablePropertyWireV1::U32)
            EXPECT_TRUE(value.isDouble());
        else if (field.wire == PlayablePropertyWireV1::U8)
            EXPECT_TRUE(value.isBool());
        else if (field.wire == PlayablePropertyWireV1::ObjectId)
            EXPECT_TRUE(value.isString());
        else
            EXPECT_TRUE(value.isArray());
    }
    const auto restored = PlayablePropertyAuthoringV1::occurrenceFromJson(encoded);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(*restored, occurrence);
}

TEST(PlayablePropertyAuthoring,
     OccurrenceSetterRejectsEveryWrongWireAlternativeWithoutMutation)
{
    for (const auto& field : kJsonFieldsV1) {
        PlayablePropertyOccurrenceV1 occurrence;
        const auto id = static_cast<PlayablePropertyIdV1>(field.id);
        const auto before = *occurrence.value(id);
        EXPECT_FALSE(occurrence.setValue(
            id, wrongOccurrenceValue(field.wire)));
        ASSERT_NE(occurrence.value(id), nullptr);
        EXPECT_EQ(*occurrence.value(id), before);
        EXPECT_TRUE(occurrence.hasExactWireTypes());
        EXPECT_FALSE(PlayablePropertyAuthoringV1::occurrenceToJson(
            occurrence).isEmpty());
    }
    PlayablePropertyOccurrenceV1 occurrence;
    EXPECT_FALSE(occurrence.setValue(
        static_cast<PlayablePropertyIdV1>(0), 0.0f));
}

TEST(PlayablePropertyAuthoring,
     OccurrenceSerializationRejectsCorruptedWireAlternative)
{
    PlayablePropertyOccurrenceV1 occurrence;
    occurrence.presence.Set(PlayablePropertyIdV1::Volume);
    const auto* readOnly = occurrence.value(PlayablePropertyIdV1::Volume);
    ASSERT_NE(readOnly, nullptr);
    *const_cast<PlayablePropertyOccurrenceValueV1*>(readOnly) = false;
    EXPECT_FALSE(occurrence.hasExactWireTypes());
    EXPECT_TRUE(PlayablePropertyAuthoringV1::occurrenceToJson(
        occurrence).isEmpty());
}

TEST(PlayablePropertyAuthoring, OccurrenceCodecNormalizesAbsentValuesAndRejectsMalformedInput)
{
    QJsonObject absent{{QLatin1String(kPresenceJsonKeyV1),
                        QStringLiteral("0000000000000000")},
                       {QStringLiteral("volume"), 12.0}};
    const auto normalized = PlayablePropertyAuthoringV1::occurrenceFromJson(absent);
    ASSERT_TRUE(normalized.has_value());
    const QJsonObject resaved = PlayablePropertyAuthoringV1::occurrenceToJson(
        *normalized);
    EXPECT_FALSE(resaved.contains(QStringLiteral("volume")));

    QJsonObject missingValue{{QLatin1String(kPresenceJsonKeyV1),
                              QStringLiteral("0000000000000001")}};
    EXPECT_FALSE(PlayablePropertyAuthoringV1::occurrenceFromJson(
        missingValue).has_value());
    for (const QString& mask : {QStringLiteral("000000000000000"),
                                QStringLiteral("00000000000000AF"),
                                QStringLiteral("0x0000000000000000"),
                                QStringLiteral("0000000000010000")}) {
        QJsonObject invalid{{QLatin1String(kPresenceJsonKeyV1), mask}};
        EXPECT_FALSE(PlayablePropertyAuthoringV1::occurrenceFromJson(
            invalid).has_value());
    }
}

}
