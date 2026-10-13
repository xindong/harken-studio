#include "Projects/ProjectStore.h"

#include <gtest/gtest.h>

#include <QTemporaryDir>

using VNH::ProjectRecord;
using VNH::ProjectStore;

namespace {

class ProjectStoreLaunchFieldsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        ASSERT_TRUE(store.open(dir.filePath(QStringLiteral("projects.db"))));
        ProjectRecord record;
        record.canonicalPath = QStringLiteral("C:/projects/Demo.hsp");
        record.displayName = QStringLiteral("Demo");
        record.detectedSchema = 5;
        ASSERT_TRUE(store.addProject(record));
    }

    QTemporaryDir dir;
    ProjectStore store;
};

TEST_F(ProjectStoreLaunchFieldsTest, SetLastKnownGoodUpdatesBothFields)
{
    ASSERT_TRUE(store.setLastKnownGood(
        QStringLiteral("C:/projects/Demo.hsp"),
        QStringLiteral("run1.aaaa1111"),
        QStringLiteral("2026-07-24T12:00:00.000Z")));

    const auto records = store.listProjects();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records.front().lastKnownGoodStudioBuildId,
              QStringLiteral("run1.aaaa1111"));
    EXPECT_EQ(records.front().lastOpenedAt,
              QStringLiteral("2026-07-24T12:00:00.000Z"));
}

TEST(ProjectRecordLaunchFieldsTest, ResolvesLinkedBuildFromPreferredOnly)
{
    ProjectRecord record;
    record.lastKnownGoodStudioBuildId = QStringLiteral("run1.last-known");
    EXPECT_TRUE(record.linkedStudioBuildId().isEmpty());

    record.preferredStudioBuildId = QStringLiteral("run2.preferred");
    EXPECT_EQ(record.linkedStudioBuildId(),
              QStringLiteral("run2.preferred"));
}

TEST_F(ProjectStoreLaunchFieldsTest, SetLastPreflightUpdatesTimestamp)
{
    ASSERT_TRUE(store.setLastPreflight(
        QStringLiteral("C:/projects/Demo.hsp"),
        QStringLiteral("2026-07-24T13:00:00.000Z")));
    const auto records = store.listProjects();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records.front().lastPreflightAt,
              QStringLiteral("2026-07-24T13:00:00.000Z"));
}

TEST_F(ProjectStoreLaunchFieldsTest, SetLastOpenedUpdatesTimestampOnly)
{
    ASSERT_TRUE(store.setLastKnownGood(
        QStringLiteral("C:/projects/Demo.hsp"),
        QStringLiteral("run1.aaaa1111"),
        QStringLiteral("2026-07-24T12:00:00.000Z")));
    ASSERT_TRUE(store.setLastOpened(
        QStringLiteral("C:/projects/Demo.hsp"),
        QStringLiteral("2026-07-25T09:00:00.000Z")));

    const auto records = store.listProjects();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records.front().lastKnownGoodStudioBuildId,
              QStringLiteral("run1.aaaa1111"));
    EXPECT_EQ(records.front().lastOpenedAt,
              QStringLiteral("2026-07-25T09:00:00.000Z"));
}

TEST_F(ProjectStoreLaunchFieldsTest, SuccessfulOpenRefreshesProjectIdentityAndSchema)
{
    ASSERT_TRUE(store.recordSuccessfulOpen(
        QStringLiteral("C:/projects/Demo.hsp"),
        QStringLiteral("project-id"), QStringLiteral("Renamed"), 6,
        QStringLiteral("run2.bbbb2222"),
        QStringLiteral("2026-07-25T10:00:00.000Z")));

    const auto records = store.listProjects();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records.front().projectId, QStringLiteral("project-id"));
    EXPECT_EQ(records.front().displayName, QStringLiteral("Renamed"));
    EXPECT_EQ(records.front().detectedSchema, 6);
    EXPECT_EQ(records.front().lastKnownGoodStudioBuildId,
              QStringLiteral("run2.bbbb2222"));
}

TEST_F(ProjectStoreLaunchFieldsTest, UpdatesRejectUnknownProject)
{
    EXPECT_FALSE(store.setLastKnownGood(QStringLiteral("C:/missing.hsp"),
                                        QStringLiteral("run1.aaaa1111"),
                                        QStringLiteral("t")));
    EXPECT_FALSE(store.setLastOpened(QStringLiteral("C:/missing.hsp"),
                                     QStringLiteral("t")));
    EXPECT_FALSE(store.setLastPreflight(QStringLiteral("C:/missing.hsp"),
                                        QStringLiteral("t")));
    EXPECT_FALSE(store.recordSuccessfulOpen(
        QStringLiteral("C:/missing.hsp"), QStringLiteral("id"),
        QStringLiteral("Missing"), 1, QStringLiteral("run1"),
        QStringLiteral("t")));
}

}
