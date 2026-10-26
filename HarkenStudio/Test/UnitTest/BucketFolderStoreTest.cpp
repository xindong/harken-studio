#include "Core/ProjectManager.h"
#include "Core/UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

namespace VNX {
namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = { arg0, nullptr };
    static QApplication app(argc, argv);
}

class BucketFolderPersistenceTest : public ::testing::TestWithParam<const char*>
{
protected:
    void SetUp() override
    {
        ensureApp();
        ASSERT_TRUE(tempDir_.isValid());

        ProjectConfig config;
        config.name = QStringLiteral("BucketFolderProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(ProjectManager::instance().createProject(config));
        ProjectData* project = ProjectManager::instance().currentProject();
        ASSERT_NE(project, nullptr);
        projectPath_ = project->projectPath;
        projectRoot_ = QFileInfo(projectPath_).absolutePath();
    }

    void TearDown() override
    {
        ProjectManager::instance().closeProject();
    }

    QString bucketName() const
    {
        return QString::fromLatin1(GetParam());
    }

    QString bucketRoot() const
    {
        return QDir(projectRoot_).filePath(bucketName());
    }

    QTemporaryDir tempDir_;
    QString projectPath_;
    QString projectRoot_;
};

TEST_P(BucketFolderPersistenceTest, NestedFolderPayloadFollowsItsOwnLocationAndRestoresAfterSavedDeletion)
{
    const QString original = QDir(bucketRoot()).filePath(QStringLiteral("Original"));
    const QString nested = QDir(original).filePath(QStringLiteral("Nested"));
    const QString destination = QDir(bucketRoot()).filePath(QStringLiteral("Destination"));
    ASSERT_TRUE(QDir().mkpath(nested));
    ASSERT_TRUE(QDir().mkpath(destination));
    const QByteArray parentBytes("parent\0payload", 14);
    const QByteArray childBytes("child\0payload", 13);
    {
        QFile file(QDir(original).filePath(QStringLiteral("ordinary.bin")));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(parentBytes), parentBytes.size());
    }
    {
        QFile file(QDir(nested).filePath(QStringLiteral(".hidden.bin")));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(childBytes), childBytes.size());
    }

    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.loadProject(projectPath_));
    auto* project = manager.currentProject();
    auto* parentFolder = project->findFolderByPath(bucketName(), QStringLiteral("Original"));
    auto* nestedFolder = project->findFolderByPath(bucketName(), QStringLiteral("Original/Nested"));
    auto* destinationFolder = project->findFolderByPath(bucketName(), QStringLiteral("Destination"));
    ASSERT_NE(parentFolder, nullptr);
    ASSERT_NE(nestedFolder, nullptr);
    ASSERT_NE(destinationFolder, nullptr);
    ASSERT_TRUE(manager.moveFolder(nestedFolder, destinationFolder, bucketName()));
    ASSERT_TRUE(manager.deleteEntries({}, {parentFolder}));
    ASSERT_TRUE(manager.saveProject());
    const QString movedNested = QDir(destination).filePath(QStringLiteral("Nested"));
    {
        QFile file(QDir(movedNested).filePath(QStringLiteral(".hidden.bin")));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        EXPECT_EQ(file.readAll(), childBytes);
    }

    UndoRedoManager::instance().undo();
    ASSERT_TRUE(manager.saveProject());
    {
        QFile file(QDir(original).filePath(QStringLiteral("ordinary.bin")));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        EXPECT_EQ(file.readAll(), parentBytes);
    }

    UndoRedoManager::instance().undo();
    ASSERT_TRUE(manager.saveProject());
    {
        QFile file(QDir(nested).filePath(QStringLiteral(".hidden.bin")));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        EXPECT_EQ(file.readAll(), childBytes);
    }
}

INSTANTIATE_TEST_SUITE_P(AllFolderBuckets,
                         BucketFolderPersistenceTest,
                         ::testing::Values("Actions", "Variables", "Attenuations", "Layers"));

}
}
