#include "Projects/ProjectStore.h"

#include <gtest/gtest.h>

#include <QTemporaryDir>

using VNH::ProjectRecord;
using VNH::ProjectStore;

namespace {

ProjectRecord makeRecord(const QString& path, const QString& name)
{
    ProjectRecord record;
    record.canonicalPath = path;
    record.displayName = name;
    record.detectedSchema = 5;
    return record;
}

}

TEST(ProjectStore, OpenCreatesDatabase)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    ProjectStore store;
    EXPECT_TRUE(store.open(dir.filePath(QStringLiteral("data/projects.db"))));
    EXPECT_TRUE(store.isOpen());
    EXPECT_TRUE(store.listProjects().empty());
}

TEST(ProjectStore, AddListRemove)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    ProjectStore store;
    ASSERT_TRUE(store.open(dir.filePath(QStringLiteral("projects.db"))));

    EXPECT_TRUE(store.addProject(makeRecord(QStringLiteral("C:/a/A.hsp"),
                                            QStringLiteral("Alpha"))));
    EXPECT_TRUE(store.addProject(makeRecord(QStringLiteral("C:/b/B.hsp"),
                                            QStringLiteral("Beta"))));

    const auto records = store.listProjects();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].displayName, QStringLiteral("Alpha"));
    EXPECT_EQ(records[1].displayName, QStringLiteral("Beta"));
    EXPECT_EQ(records[0].detectedSchema, 5);

    EXPECT_TRUE(store.contains(QStringLiteral("C:/a/A.hsp")));
    EXPECT_TRUE(store.removeProject(QStringLiteral("C:/a/A.hsp")));
    EXPECT_FALSE(store.contains(QStringLiteral("C:/a/A.hsp")));
    EXPECT_EQ(store.listProjects().size(), 1u);
    EXPECT_FALSE(store.removeProject(QStringLiteral("C:/a/A.hsp")));
}

TEST(ProjectStore, DuplicateCanonicalPathIsRejected)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    ProjectStore store;
    ASSERT_TRUE(store.open(dir.filePath(QStringLiteral("projects.db"))));

    ASSERT_TRUE(store.addProject(makeRecord(QStringLiteral("C:/a/A.hsp"),
                                            QStringLiteral("Alpha"))));
    EXPECT_FALSE(store.addProject(makeRecord(QStringLiteral("C:/a/A.hsp"),
                                             QStringLiteral("AlphaAgain"))));
    EXPECT_EQ(store.listProjects().size(), 1u);
}

TEST(ProjectStore, SetPreferredBuildPersists)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("projects.db"));

    {
        ProjectStore store;
        ASSERT_TRUE(store.open(databasePath));
        ASSERT_TRUE(store.addProject(makeRecord(QStringLiteral("C:/a/A.hsp"),
                                                QStringLiteral("Alpha"))));
        EXPECT_TRUE(store.setPreferredBuild(QStringLiteral("C:/a/A.hsp"),
                                            QStringLiteral("run12.abcd1234")));
        EXPECT_FALSE(store.setPreferredBuild(QStringLiteral("C:/missing.hsp"),
                                             QStringLiteral("run12.abcd1234")));
    }

    ProjectStore reopened;
    ASSERT_TRUE(reopened.open(databasePath));
    const auto records = reopened.listProjects();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].preferredStudioBuildId, QStringLiteral("run12.abcd1234"));
}
