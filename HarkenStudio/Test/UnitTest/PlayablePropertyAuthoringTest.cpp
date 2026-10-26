#include "UnitTest/StudioObjectIdTestSupport.h"
#include "FieldGroups.h"
#include "PlayablePropertyAuthoringV1.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

namespace {

using VNX::PlayablePropertyAuthoringV1;
using VNX::PlayablePropertyOccurrenceV1;
using VNX::PlayablePropertyOccurrenceValueV1;
const auto kJsonFieldsV1 = PlayablePropertyAuthoringV1::fields();
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

TEST(PlayablePropertyAuthoring, GeneratedRegistryHasUniqueExactStudioStorage)
{
    QSet<QString> jsonKeys;
    QSet<QString> storageKeys;
    for (const auto& field : kJsonFieldsV1) {
        const QString jsonKey = QLatin1String(field.jsonKey);
        EXPECT_FALSE(jsonKeys.contains(jsonKey));
        jsonKeys.insert(jsonKey);
        const QString storage = QString::fromLatin1(field.component)
            + QLatin1Char('.') + QString::fromLatin1(field.storageField);
        EXPECT_FALSE(storageKeys.contains(storage));
        storageKeys.insert(storage);

    }
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
    }
}

}
