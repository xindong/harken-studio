#include "Core/PackGenerationRecord.h"

#include <gtest/gtest.h>

TEST(PackGenerationRecord, ClearingSessionCancelsWorkAndInvalidatesItsEvents)
{
    auto& store = VNX::PackGenerationRecordStore::instance();
    store.clear();
    const auto cancel = std::make_shared<std::atomic_bool>(false);
    const auto oldRun = store.beginProgress(cancel);
    VNX::PackGenerationProgress progress{
        QStringLiteral("platform"), QStringLiteral("English"), QStringLiteral("a:123"),
        41, VNX::PackGenerationStage::Compiling};
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
    auto& store = VNX::PackGenerationRecordStore::instance();
    store.clear();
    const auto run = store.beginProgress(std::make_shared<std::atomic_bool>(false));
    VNX::PackGenerationProgress progress{
        QStringLiteral("platform"), QStringLiteral("English"), QStringLiteral("a:123"),
        0, VNX::PackGenerationStage::Queued};
    VNX::PackBuildObservation global{progress.platform, {}, {},
        VNX::PackBuildObservationStage::CompilingProject};
    VNX::PackBuildObservation unit{progress.platform, progress.language, progress.unitKey,
        VNX::PackBuildObservationStage::Prepared, QStringLiteral("Action")};
    store.updateObservation(run, unit);
    EXPECT_EQ(store.findObservation(unit.platform, unit.language, unit.unitKey), nullptr);
    store.updateProgress(run, progress);
    store.updateObservation(run, global);
    store.updateObservation(run, unit);
    ASSERT_NE(store.globalObservation(), nullptr);
    ASSERT_NE(store.findObservation(unit.platform, unit.language, unit.unitKey), nullptr);
    progress.stage = VNX::PackGenerationStage::Compiling;
    progress.percent = 40;
    store.updateProgress(run, progress);
    ASSERT_NE(store.findObservation(unit.platform, unit.language, unit.unitKey), nullptr);
    EXPECT_EQ(store.findProgress(unit.platform, unit.language, unit.unitKey)->stage, progress.stage);
    EXPECT_EQ(store.findProgress(unit.platform, unit.language, unit.unitKey)->percent, progress.percent);
    progress.stage = VNX::PackGenerationStage::Writing;
    store.updateProgress(run, progress);
    EXPECT_EQ(store.findObservation(unit.platform, unit.language, unit.unitKey), nullptr);
    unit.stage = VNX::PackBuildObservationStage::ReadyToPublish;
    store.updateObservation(run, unit);
    progress.stage = VNX::PackGenerationStage::Publishing;
    store.updateProgress(run, progress);
    ASSERT_NE(store.findObservation(unit.platform, unit.language, unit.unitKey), nullptr);
    progress.stage = VNX::PackGenerationStage::Complete;
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
    progress.stage = VNX::PackGenerationStage::Queued;
    store.updateProgress(nextRun, progress);
    store.updateObservation(nextRun, unit);
    ASSERT_NE(store.findObservation(unit.platform, unit.language, unit.unitKey), nullptr);
    store.clear();
}

TEST(PackGenerationRecord, EndingSessionsClearsGlobalAndUnitObservations)
{
    auto& store = VNX::PackGenerationRecordStore::instance();
    for (int ending = 0; ending < 3; ++ending) {
        SCOPED_TRACE(ending);
        store.clear();
        const auto cancel = std::make_shared<std::atomic_bool>(false);
        const auto run = store.beginProgress(cancel);
        VNX::PackGenerationProgress progress{
            QStringLiteral("platform"), {}, QStringLiteral("init"),
            5, VNX::PackGenerationStage::Preparing};
        VNX::PackBuildObservation global{progress.platform, {}, {},
            VNX::PackBuildObservationStage::PreparingMedia};
        VNX::PackBuildObservation unit{progress.platform, progress.language, progress.unitKey,
            VNX::PackBuildObservationStage::Prepared, QStringLiteral("Init")};
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
