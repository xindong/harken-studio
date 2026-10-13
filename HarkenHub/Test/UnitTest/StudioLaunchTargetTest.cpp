#include "Versions/StudioLaunchTarget.h"

#include <gtest/gtest.h>

namespace {

VNH::InstalledVersion version(const QString& buildId, const QString& semantic)
{
    VNH::InstalledVersion result;
    result.buildId = buildId;
    result.semanticVersion = semantic;
    return result;
}

}

TEST(StudioLaunchTargetTest, ReturnsNullForEmptyList)
{
    EXPECT_EQ(VNH::StudioLaunchTarget::pick({}, QString(), {}), nullptr);
    EXPECT_EQ(VNH::StudioLaunchTarget::pick({}, QStringLiteral("b1"), {}),
              nullptr);
}

TEST(StudioLaunchTargetTest, PicksHighestSemanticVersionNumerically)
{
    const std::vector<VNH::InstalledVersion> versions = {
        version(QStringLiteral("b1"), QStringLiteral("0.1.0-dev")),
        version(QStringLiteral("b2"), QStringLiteral("0.1.10-dev")),
        version(QStringLiteral("b3"), QStringLiteral("0.1.2-dev"))};

    const auto* target = VNH::StudioLaunchTarget::pick(versions, QString(), {});
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->buildId, QStringLiteral("b2"));
}

TEST(StudioLaunchTargetTest, ComparesAcrossSegmentCounts)
{
    const std::vector<VNH::InstalledVersion> versions = {
        version(QStringLiteral("b1"), QStringLiteral("0.9.9")),
        version(QStringLiteral("b2"), QStringLiteral("1.0"))};

    const auto* target = VNH::StudioLaunchTarget::pick(versions, QString(), {});
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->buildId, QStringLiteral("b2"));
}

TEST(StudioLaunchTargetTest, BreaksVersionTiesByBuildId)
{
    const std::vector<VNH::InstalledVersion> versions = {
        version(QStringLiteral("b2"), QStringLiteral("0.1.2-dev")),
        version(QStringLiteral("b1"), QStringLiteral("0.1.2-dev"))};

    const auto* target = VNH::StudioLaunchTarget::pick(versions, QString(), {});
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->buildId, QStringLiteral("b2"));
}

TEST(StudioLaunchTargetTest, PinnedBuildOverridesLatest)
{
    const std::vector<VNH::InstalledVersion> versions = {
        version(QStringLiteral("b1"), QStringLiteral("0.1.0-dev")),
        version(QStringLiteral("b2"), QStringLiteral("0.1.2-dev"))};

    const auto* target =
        VNH::StudioLaunchTarget::pick(versions, QStringLiteral("b1"), {});
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->buildId, QStringLiteral("b1"));
}

TEST(StudioLaunchTargetTest, FallsBackToLatestWhenPinnedMissing)
{
    const std::vector<VNH::InstalledVersion> versions = {
        version(QStringLiteral("b1"), QStringLiteral("0.1.0-dev")),
        version(QStringLiteral("b2"), QStringLiteral("0.1.2-dev"))};

    const auto* target =
        VNH::StudioLaunchTarget::pick(versions, QStringLiteral("gone"), {});
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->buildId, QStringLiteral("b2"));
}

TEST(StudioLaunchTargetTest, SkipsExcludedLatestBuild)
{
    const std::vector<VNH::InstalledVersion> versions = {
        version(QStringLiteral("b1"), QStringLiteral("0.1.0-dev")),
        version(QStringLiteral("b2"), QStringLiteral("0.1.2-dev"))};

    const auto* target = VNH::StudioLaunchTarget::pick(
        versions, QString(), {QStringLiteral("b2")});
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->buildId, QStringLiteral("b1"));
}

TEST(StudioLaunchTargetTest, FallsBackWhenPinnedBuildExcluded)
{
    const std::vector<VNH::InstalledVersion> versions = {
        version(QStringLiteral("b1"), QStringLiteral("0.1.0-dev")),
        version(QStringLiteral("b2"), QStringLiteral("0.1.2-dev"))};

    const auto* target = VNH::StudioLaunchTarget::pick(
        versions, QStringLiteral("b1"), {QStringLiteral("b1")});
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->buildId, QStringLiteral("b2"));
}

TEST(StudioLaunchTargetTest, ReturnsNullWhenAllBuildsExcluded)
{
    const std::vector<VNH::InstalledVersion> versions = {
        version(QStringLiteral("b1"), QStringLiteral("0.1.0-dev"))};

    EXPECT_EQ(VNH::StudioLaunchTarget::pick(versions, QString(),
                                            {QStringLiteral("b1")}),
              nullptr);
    EXPECT_EQ(VNH::StudioLaunchTarget::pick(versions, QStringLiteral("b1"),
                                            {QStringLiteral("b1")}),
              nullptr);
}
