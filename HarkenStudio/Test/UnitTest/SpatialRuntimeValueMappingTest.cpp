#include "ProjectManager.h"

#include <gtest/gtest.h>

using VNX::ProjectManager;

TEST(SpatialRuntimeValueMapping, SpatMixPassesModelValueThroughInRange)
{
    EXPECT_FLOAT_EQ(ProjectManager::runtimeSpatMixForValue(0.0f), 0.0f);
    EXPECT_FLOAT_EQ(ProjectManager::runtimeSpatMixForValue(0.37f), 0.37f);
    EXPECT_FLOAT_EQ(ProjectManager::runtimeSpatMixForValue(1.0f), 1.0f);
}

TEST(SpatialRuntimeValueMapping, SpatMixClampsToEngineRange)
{
    EXPECT_FLOAT_EQ(ProjectManager::runtimeSpatMixForValue(-0.5f), 0.0f);
    EXPECT_FLOAT_EQ(ProjectManager::runtimeSpatMixForValue(1.5f), 1.0f);
}

TEST(SpatialRuntimeValueMapping, DistanceScalePassesModelValueThroughInRange)
{
    EXPECT_FLOAT_EQ(ProjectManager::runtimeDistanceScaleForValue(0.01f), 0.01f);
    EXPECT_FLOAT_EQ(ProjectManager::runtimeDistanceScaleForValue(1.0f), 1.0f);
    EXPECT_FLOAT_EQ(ProjectManager::runtimeDistanceScaleForValue(42.5f), 42.5f);
    EXPECT_FLOAT_EQ(ProjectManager::runtimeDistanceScaleForValue(100.0f), 100.0f);
}

TEST(SpatialRuntimeValueMapping, DistanceScaleClampsToEngineRange)
{
    EXPECT_FLOAT_EQ(ProjectManager::runtimeDistanceScaleForValue(0.0f), 0.01f);
    EXPECT_FLOAT_EQ(ProjectManager::runtimeDistanceScaleForValue(-3.0f), 0.01f);
    EXPECT_FLOAT_EQ(ProjectManager::runtimeDistanceScaleForValue(1000.0f), 100.0f);
}
