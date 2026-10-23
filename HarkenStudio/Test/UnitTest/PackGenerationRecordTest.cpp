#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Core/PackGenerationRecord.h"
#include "Core/ProjectSerializer.h"
#include "Core/Components/VAction.h"
#include "Support/ProjectPlatformTestUtils.h"

#include <gtest/gtest.h>
#include <QObject>

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

TEST(PackGenerationRecord, StableIdentityUpdatesOneSessionRecordAndNotifiesReaders)
{
    auto& store = VNX::PackGenerationRecordStore::instance();
    store.clear();
    QObject receiver;
    int changes = 0;
    QObject::connect(&store, &VNX::PackGenerationRecordStore::changed,
                     &receiver, [&] { ++changes; });
    VNX::PackGenerationRecord record;
    record.unitKey = QStringLiteral("a:12345678901234567890");
    record.platform = QStringLiteral("platform");
    record.language = QStringLiteral("English");
    record.packName = QStringLiteral("Before");
    record.elapsedMs = 12;
    store.add(record);
    record.packName = QStringLiteral("After");
    record.elapsedMs = 24;
    store.add(record);

    ASSERT_EQ(store.records().size(), 1);
    const auto* current = store.find(record.platform, record.language, record.unitKey);
    ASSERT_NE(current, nullptr);
    EXPECT_EQ(current->packName, QStringLiteral("After"));
    EXPECT_EQ(current->elapsedMs, 24);
    EXPECT_EQ(changes, 2);

    record.language = QStringLiteral("Japanese");
    store.add(record);
    EXPECT_EQ(store.records().size(), 2);
    store.clear();
}

TEST(PackGenerationRecord, ProgressTracksPlatformLanguageIdentityAndPreservesFailurePosition)
{
    auto& store = VNX::PackGenerationRecordStore::instance();
    store.clear();
    const auto cancel = std::make_shared<std::atomic_bool>(false);
    const auto run = store.beginProgress(cancel);
    ASSERT_NE(run, 0);
    EXPECT_EQ(store.beginProgress(std::make_shared<std::atomic_bool>(false)), 0);

    VNX::PackGenerationProgress first{
        QStringLiteral("platform-a"), QStringLiteral("English"), QStringLiteral("a:123"),
        32, VNX::PackGenerationStage::Preparing};
    auto otherLanguage = first;
    otherLanguage.language = QStringLiteral("Japanese");
    otherLanguage.percent = 100;
    otherLanguage.stage = VNX::PackGenerationStage::Complete;
    auto otherPlatform = first;
    otherPlatform.platform = QStringLiteral("platform-b");
    otherPlatform.percent = 61;
    otherPlatform.stage = VNX::PackGenerationStage::Failed;
    store.updateProgress(run, first);
    store.updateProgress(run, otherLanguage);
    store.updateProgress(run, otherPlatform);
    store.finishProgress(run, false);

    const auto* failed = store.findProgress(first.platform, first.language, first.unitKey);
    ASSERT_NE(failed, nullptr);
    EXPECT_EQ(failed->percent, first.percent);
    EXPECT_EQ(failed->stage, VNX::PackGenerationStage::Failed);
    const auto* completed = store.findProgress(otherLanguage.platform, otherLanguage.language,
                                               otherLanguage.unitKey);
    ASSERT_NE(completed, nullptr);
    EXPECT_EQ(completed->stage, VNX::PackGenerationStage::Complete);
    const auto* earlierFailure = store.findProgress(otherPlatform.platform, otherPlatform.language,
                                                    otherPlatform.unitKey);
    ASSERT_NE(earlierFailure, nullptr);
    EXPECT_EQ(earlierFailure->percent, otherPlatform.percent);
    EXPECT_EQ(earlierFailure->stage, VNX::PackGenerationStage::Failed);
    EXPECT_FALSE(store.isExporting());
    store.clear();
}

TEST(PackGenerationRecord, RebuildingResetsOnlyItsIdentityAndRejectsPreviousRunEvents)
{
    auto& store = VNX::PackGenerationRecordStore::instance();
    store.clear();
    const auto firstRun = store.beginProgress(std::make_shared<std::atomic_bool>(false));
    VNX::PackGenerationProgress progress{
        QStringLiteral("platform"), QStringLiteral("English"), QStringLiteral("a:123"),
        100, VNX::PackGenerationStage::Complete};
    auto retained = progress;
    retained.language = QStringLiteral("Japanese");
    store.updateProgress(firstRun, progress);
    store.updateProgress(firstRun, retained);
    store.finishProgress(firstRun, false);
    VNX::PackGenerationRecord record;
    record.platform = progress.platform;
    record.language = progress.language;
    record.unitKey = progress.unitKey;
    record.elapsedMs = 12;
    store.add(record);
    record.language = retained.language;
    store.add(record);
    QObject receiver;
    int changes = 0;
    int progressChanges = 0;
    QObject::connect(&store, &VNX::PackGenerationRecordStore::changed,
                     &receiver, [&] { ++changes; });
    QObject::connect(&store, &VNX::PackGenerationRecordStore::progressChanged,
                     &receiver, [&] { ++progressChanges; });

    const auto secondRun = store.beginProgress(std::make_shared<std::atomic_bool>(false));
    ASSERT_GT(secondRun, firstRun);
    progress.percent = 0;
    progress.stage = VNX::PackGenerationStage::Queued;
    store.updateProgress(secondRun, progress);
    EXPECT_EQ(store.find(progress.platform, progress.language, progress.unitKey), nullptr);
    EXPECT_NE(store.find(retained.platform, retained.language, retained.unitKey), nullptr);
    EXPECT_EQ(changes, 0);
    EXPECT_EQ(progressChanges, 1);

    auto late = progress;
    late.percent = 100;
    late.stage = VNX::PackGenerationStage::Complete;
    store.updateProgress(firstRun, late);
    store.finishProgress(firstRun, false);
    ASSERT_TRUE(store.isExporting());
    const auto* restarted = store.findProgress(progress.platform, progress.language, progress.unitKey);
    ASSERT_NE(restarted, nullptr);
    EXPECT_EQ(restarted->percent, 0);
    EXPECT_EQ(restarted->stage, VNX::PackGenerationStage::Queued);
    EXPECT_EQ(progressChanges, 1);

    progress.percent = 47;
    progress.stage = VNX::PackGenerationStage::Writing;
    store.updateProgress(secondRun, progress);
    auto duplicateQueue = progress;
    duplicateQueue.percent = 0;
    duplicateQueue.stage = VNX::PackGenerationStage::Queued;
    store.updateProgress(secondRun, duplicateQueue);
    restarted = store.findProgress(progress.platform, progress.language, progress.unitKey);
    ASSERT_NE(restarted, nullptr);
    EXPECT_EQ(restarted->percent, progress.percent);
    EXPECT_EQ(restarted->stage, VNX::PackGenerationStage::Writing);
    store.finishProgress(secondRun, true);
    restarted = store.findProgress(progress.platform, progress.language, progress.unitKey);
    ASSERT_NE(restarted, nullptr);
    EXPECT_EQ(restarted->percent, progress.percent);
    EXPECT_EQ(restarted->stage, VNX::PackGenerationStage::Canceled);
    const auto* unchanged = store.findProgress(retained.platform, retained.language, retained.unitKey);
    ASSERT_NE(unchanged, nullptr);
    EXPECT_EQ(unchanged->stage, VNX::PackGenerationStage::Complete);
    store.clear();
}

TEST(PackGenerationRecord, ClearingSessionCancelsWorkAndInvalidatesItsEvents)
{
    auto& store = VNX::PackGenerationRecordStore::instance();
    store.clear();
    QObject receiver;
    QVector<bool> activity;
    QObject::connect(&store, &VNX::PackGenerationRecordStore::exportActivityChanged,
                     &receiver, [&](bool exporting) { activity.push_back(exporting); });
    const auto cancel = std::make_shared<std::atomic_bool>(false);
    const auto oldRun = store.beginProgress(cancel);
    EXPECT_TRUE(store.isPreflighting());
    VNX::PackGenerationProgress progress{
        QStringLiteral("platform"), QStringLiteral("English"), QStringLiteral("a:123"),
        41, VNX::PackGenerationStage::Compiling};
    store.updateProgress(oldRun, progress);
    store.clear();
    EXPECT_TRUE(cancel->load());
    EXPECT_FALSE(store.isExporting());
    EXPECT_FALSE(store.isPreflighting());
    EXPECT_EQ(store.findProgress(progress.platform, progress.language, progress.unitKey), nullptr);
    EXPECT_EQ(activity, QVector<bool>({true, false}));

    const auto nextCancel = std::make_shared<std::atomic_bool>(false);
    const auto nextRun = store.beginProgress(nextCancel);
    store.finishPreflight(oldRun);
    EXPECT_TRUE(store.isPreflighting());
    ASSERT_GT(nextRun, oldRun);
    store.updateProgress(oldRun, progress);
    store.finishProgress(oldRun, true);
    EXPECT_TRUE(store.isExporting());
    EXPECT_EQ(store.findProgress(progress.platform, progress.language, progress.unitKey), nullptr);
    EXPECT_FALSE(nextCancel->load());
    store.cancelExport();
    EXPECT_TRUE(nextCancel->load());
    EXPECT_TRUE(store.isExporting());
    EXPECT_TRUE(store.isPreflighting());
    store.finishProgress(nextRun, true);
    EXPECT_FALSE(store.isExporting());
    EXPECT_FALSE(store.isPreflighting());
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
