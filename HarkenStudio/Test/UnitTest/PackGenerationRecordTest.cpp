#include "Core/PackGenerationRecord.h"

#include <gtest/gtest.h>

TEST(PackGenerationRecord, ClearingSessionCancelsWorkAndInvalidatesItsEvents)
{
    auto& store = harken::studio::PackGenerationRecordStore::instance();
    store.clear();
    const auto cancel = std::make_shared<std::atomic_bool>(false);
    const auto oldRun = store.beginProgress(cancel);
    harken::studio::PackGenerationProgress progress{
        QStringLiteral("platform"), QStringLiteral("English"), QStringLiteral("a:123"),
        41, harken::studio::PackGenerationStage::Compiling};
    store.updateProgress(oldRun, progress);
    store.clear();
    EXPECT_TRUE(cancel->load());
    EXPECT_FALSE(store.isExporting());
    EXPECT_EQ(store.findProgress(progress.platform, progress.language, progress.unitKey), nullptr);

    const auto nextCancel = std::make_shared<std::atomic_bool>(false);
    const auto nextRun = store.beginProgress(nextCancel);
    ASSERT_NE(nextRun, oldRun);
    store.updateProgress(oldRun, progress);
    store.finishProgress(oldRun, true);
    EXPECT_TRUE(store.isExporting());
    EXPECT_EQ(store.findProgress(progress.platform, progress.language, progress.unitKey), nullptr);
    EXPECT_FALSE(nextCancel->load());
    store.clear();
}

TEST(PackGenerationRecord, ObservationsFollowActiveRunAndTrackedUnits)
{
    auto& store = harken::studio::PackGenerationRecordStore::instance();
    store.clear();
    const auto run = store.beginProgress(std::make_shared<std::atomic_bool>(false));
    harken::studio::PackGenerationProgress progress{
        QStringLiteral("platform"), QStringLiteral("English"), QStringLiteral("a:123"),
        0, harken::studio::PackGenerationStage::Queued};
    harken::studio::PackBuildObservation global{progress.platform, {}, {},
        harken::studio::PackBuildObservationStage::CompilingProject};
    harken::studio::PackBuildObservation unit{progress.platform, progress.language, progress.unitKey,
        harken::studio::PackBuildObservationStage::Prepared, QStringLiteral("Action")};
    store.updateObservation(run, unit);
    EXPECT_EQ(store.findObservation(unit.platform, unit.language, unit.unitKey), nullptr);
    store.updateProgress(run, progress);
    store.updateObservation(run, global);
    store.updateObservation(run, unit);
    ASSERT_NE(store.globalObservation(), nullptr);
    ASSERT_NE(store.findObservation(unit.platform, unit.language, unit.unitKey), nullptr);
    progress.stage = harken::studio::PackGenerationStage::Compiling;
    progress.percent = 40;
    store.updateProgress(run, progress);
    ASSERT_NE(store.findObservation(unit.platform, unit.language, unit.unitKey), nullptr);
    EXPECT_EQ(store.findProgress(unit.platform, unit.language, unit.unitKey)->stage, progress.stage);
    EXPECT_EQ(store.findProgress(unit.platform, unit.language, unit.unitKey)->percent, progress.percent);
    progress.stage = harken::studio::PackGenerationStage::Writing;
    store.updateProgress(run, progress);
    EXPECT_EQ(store.findObservation(unit.platform, unit.language, unit.unitKey), nullptr);
    progress.stage = harken::studio::PackGenerationStage::Complete;
    store.updateProgress(run, progress);
    store.updateObservation(run, unit);
    EXPECT_EQ(store.findObservation(unit.platform, unit.language, unit.unitKey), nullptr);
    store.finishProgress(run, false);
    store.updateObservation(run, global);
    EXPECT_EQ(store.globalObservation(), nullptr);
    const auto nextRun = store.beginProgress(std::make_shared<std::atomic_bool>(false));
    store.updateObservation(run, global);
    store.updateObservation(run, unit);
    store.updateObservation(nextRun, unit);
    EXPECT_EQ(store.globalObservation(), nullptr);
    EXPECT_EQ(store.findObservation(unit.platform, unit.language, unit.unitKey), nullptr);
    progress.stage = harken::studio::PackGenerationStage::Queued;
    store.updateProgress(nextRun, progress);
    store.updateObservation(nextRun, unit);
    ASSERT_NE(store.findObservation(unit.platform, unit.language, unit.unitKey), nullptr);
    store.clear();
}

TEST(PackGenerationRecord, EndingSessionsClearsGlobalAndUnitObservations)
{
    auto& store = harken::studio::PackGenerationRecordStore::instance();
    for (int ending = 0; ending < 3; ++ending) {
        SCOPED_TRACE(ending);
        store.clear();
        const auto cancel = std::make_shared<std::atomic_bool>(false);
        const auto run = store.beginProgress(cancel);
        harken::studio::PackGenerationProgress progress{
            QStringLiteral("platform"), {}, QStringLiteral("init"),
            5, harken::studio::PackGenerationStage::Preparing};
        harken::studio::PackBuildObservation global{progress.platform, {}, {},
            harken::studio::PackBuildObservationStage::PreparingMedia};
        harken::studio::PackBuildObservation unit{progress.platform, progress.language, progress.unitKey,
            harken::studio::PackBuildObservationStage::Prepared, QStringLiteral("Init")};
        store.updateProgress(run, progress);
        store.updateObservation(run, global);
        store.updateObservation(run, unit);
        ASSERT_NE(store.globalObservation(), nullptr);
        ASSERT_NE(store.findObservation(unit.platform, unit.language, unit.unitKey), nullptr);
        if (ending == 2) store.clear();
        else store.finishProgress(run, ending == 1);
        EXPECT_FALSE(store.isExporting());
        EXPECT_EQ(store.globalObservation(), nullptr);
        EXPECT_EQ(store.findObservation(unit.platform, unit.language, unit.unitKey), nullptr);
        store.updateObservation(run, global);
        store.updateObservation(run, unit);
        EXPECT_EQ(store.globalObservation(), nullptr);
        EXPECT_EQ(store.findObservation(unit.platform, unit.language, unit.unitKey), nullptr);
    }
    store.clear();
}
