#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Core/PackGenerationRecord.h"
#include "Core/ProjectSerializer.h"
#include "Core/Components/VAction.h"
#include "Support/ProjectPlatformTestUtils.h"

#include <gtest/gtest.h>

TEST(PackGenerationRecord, SerializesReadOnlyFields)
{
    VNX::PackGenerationRecord record;
    record.packName = QStringLiteral("Combat");
    record.platform = HarkenTest::platformIdString(QStringLiteral("Windows"));
    record.platformName = QStringLiteral("Windows");
    record.language = QStringLiteral("Japanese");
    record.success = false;
    record.errors = QStringLiteral("capability unavailable");
    record.elapsedMs = 12;
    const auto json = record.toJson();
    EXPECT_EQ(json.value(QStringLiteral("packName")).toString(), QStringLiteral("Combat"));
    EXPECT_EQ(json.value(QStringLiteral("platform_id")).toString(), record.platform);
    EXPECT_EQ(json.value(QStringLiteral("platform_name")).toString(), record.platformName);
    EXPECT_FALSE(json.value(QStringLiteral("success")).toBool());
    EXPECT_EQ(json.value(QStringLiteral("language")).toString(),
              QStringLiteral("Japanese"));
    EXPECT_EQ(json.value(QStringLiteral("elapsedMs")).toInteger(), 12);
}

TEST(PackGenerationRecord, StoreKeepsInsertionOrderAndClears)
{
    auto& store = VNX::PackGenerationRecordStore::instance();
    store.clear();
    VNX::PackGenerationRecord first;
    first.packName = QStringLiteral("A");
    VNX::PackGenerationRecord second;
    second.packName = QStringLiteral("B");
    store.add(first);
    store.add(second);
    const auto records = store.records();
    ASSERT_EQ(records.size(), 2);
    EXPECT_EQ(records.at(0).packName, QStringLiteral("A"));
    EXPECT_EQ(records.at(1).packName, QStringLiteral("B"));
    store.clear();
    EXPECT_TRUE(store.records().isEmpty());
}

TEST(PackGenerationRecord, StoreReplacesThePreviousBuildSnapshot)
{
    auto& store = VNX::PackGenerationRecordStore::instance();
    store.clear();
    VNX::PackGenerationRecord stale;
    stale.packName = QStringLiteral("Previous");
    store.add(stale);
    VNX::PackGenerationRecord current;
    current.packName = QStringLiteral("Current");

    store.replace({current});

    const auto records = store.records();
    ASSERT_EQ(records.size(), 1);
    EXPECT_EQ(records.front().packName, QStringLiteral("Current"));
    store.clear();
}

TEST(ActionPackIdentity, FullUuidRoundTripsAndMissingIdentityIsRejected)
{
    VNX::ProjectSerializer serializer;
    VNX::ProjectItem source(QStringLiteral("Action"), VNX::ProjectItem::Type::Action);
    source.attachDefaultComponents();
    source.guid = QUuid::createUuid();
    source.objectID = 7;
    source.guid = VNX::authoringUuid(harkentest::StudioObjectId(42));

    const QJsonObject json = serializer.actionPackToJson(source);
    EXPECT_FALSE(json.contains(QStringLiteral("runtimeStableId")));
    EXPECT_EQ(json.value(QStringLiteral("guid")).toString(), VNX::objectIdString(source.runtimeId()));

    VNX::ProjectItem restored(QString(), VNX::ProjectItem::Type::Action);
    ASSERT_TRUE(serializer.actionPackFromJson(json, restored));
    EXPECT_EQ(restored.runtimeId(), harkentest::StudioObjectId(42u));

    QJsonObject missingIdentity = json;
    missingIdentity.remove(QStringLiteral("guid"));
    VNX::ProjectItem rejected(QString(), VNX::ProjectItem::Type::Action);
    VNX::ProjectSerializer reader;
    EXPECT_FALSE(reader.actionPackFromJson(missingIdentity, rejected));
}
