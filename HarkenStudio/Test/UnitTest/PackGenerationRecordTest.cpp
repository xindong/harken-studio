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
