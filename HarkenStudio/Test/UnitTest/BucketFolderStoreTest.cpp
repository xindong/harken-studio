#include "Core/BucketFolderStore.h"
#include "Core/ProjectManager.h"
#include "Core/UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>

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

bool writeJson(const QString& filePath, const QJsonObject& object)
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    return file.write(bytes) == bytes.size();
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

TEST_P(BucketFolderPersistenceTest, SavingRestoresLoadedDirectoriesAndOverwritesIgnoredExternalChanges)
{
    const QString nestedPath = QDir(bucketRoot()).filePath(QStringLiteral("Combat/Boss"));
    ASSERT_TRUE(QDir().mkpath(nestedPath));

    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.loadProject(projectPath_));
    ProjectData* project = manager.currentProject();
    ASSERT_NE(project, nullptr);

    const auto roots = project->folderRootsForBucket(bucketName());
    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(roots.front()->name, QStringLiteral("Combat"));
    EXPECT_EQ(roots.front()->parent, nullptr);

    PhysicalFolder* boss = project->findFolderByPath(
        bucketName(), QStringLiteral("Combat/Boss"));
    ASSERT_NE(boss, nullptr);
    ASSERT_NE(boss->parent, nullptr);
    EXPECT_EQ(boss->parent->name, QStringLiteral("Combat"));
    EXPECT_EQ(project->folderRelPath(boss), QStringLiteral("Combat/Boss"));
    EXPECT_EQ(project->findFolderByPath(bucketName(), QString()), nullptr);

    ASSERT_TRUE(QDir(bucketRoot()).removeRecursively());
    ASSERT_FALSE(QFileInfo::exists(nestedPath));
    qApp->processEvents();
    ASSERT_TRUE(manager.saveProject());
    EXPECT_TRUE(QFileInfo(nestedPath).isDir());

    const QString untracked = QDir(bucketRoot()).filePath(QStringLiteral("Untracked/Empty"));
    ASSERT_TRUE(QDir().mkpath(untracked));
    ASSERT_TRUE(manager.saveProject());
    EXPECT_FALSE(QFileInfo::exists(untracked));
}

TEST_P(BucketFolderPersistenceTest, KeepingEditsOverwritesChangedAndDeletedAuthoringDocuments)
{
    auto& manager = ProjectManager::instance();
    auto* folder = manager.createFolder(nullptr, QStringLiteral("Authoring"), bucketName());
    ASSERT_NE(folder, nullptr);
    const auto type = bucketName() == QStringLiteral("Actions") ? ProjectItem::Type::Action
        : bucketName() == QStringLiteral("Variables") ? ProjectItem::Type::Variable
        : bucketName() == QStringLiteral("Attenuations") ? ProjectItem::Type::Attenuation
        : ProjectItem::Type::Sound;
    auto* changed = manager.createItem(nullptr, QStringLiteral("ChangedExternally"), type,
                                       QStringLiteral("Authoring"));
    auto* deleted = manager.createItem(nullptr, QStringLiteral("DeletedExternally"), type,
                                       QStringLiteral("Authoring"));
    ASSERT_NE(changed, nullptr);
    ASSERT_NE(deleted, nullptr);
    changed->extensions.insert(QStringLiteral("testContent"), QStringLiteral("saved"));
    deleted->extensions.insert(QStringLiteral("testContent"), QStringLiteral("retained"));
    const QUuid changedGuid = changed->guid;
    const QUuid deletedGuid = deleted->guid;
    ASSERT_TRUE(manager.saveProject());
    const QString changedPath = manager.currentProject()->objectFilePath(changed);
    const QString deletedPath = manager.currentProject()->objectFilePath(deleted);

    ASSERT_TRUE(writeJson(changedPath, {{QStringLiteral("externalChange"), true}}));
    ASSERT_TRUE(QFile::remove(deletedPath));
    ASSERT_TRUE(writeJson(projectPath_, {{QStringLiteral("externalProjectChange"), true}}));
    const QString addedFolder = QDir(bucketRoot()).filePath(QStringLiteral("External/Empty"));
    ASSERT_TRUE(QDir().mkpath(addedFolder));
    ASSERT_TRUE(writeJson(QDir(addedFolder).filePath(QStringLiteral("ordinary.json")),
                          {{QStringLiteral("external"), true}}));
    changed->extensions.insert(QStringLiteral("testContent"), QStringLiteral("unsaved"));
    ASSERT_TRUE(manager.saveProject());
    EXPECT_TRUE(QFileInfo::exists(changedPath));
    EXPECT_TRUE(QFileInfo::exists(deletedPath));
    EXPECT_FALSE(QFileInfo::exists(QDir(bucketRoot()).filePath(QStringLiteral("External"))));
    ASSERT_TRUE(manager.loadProject(projectPath_));
    changed = manager.findItemByGuid(changedGuid);
    deleted = manager.findItemByGuid(deletedGuid);
    ASSERT_NE(changed, nullptr);
    ASSERT_NE(deleted, nullptr);
    EXPECT_EQ(changed->extensions.value(QStringLiteral("testContent")).toString(),
              QStringLiteral("unsaved"));
    EXPECT_EQ(deleted->extensions.value(QStringLiteral("testContent")).toString(),
              QStringLiteral("retained"));

    ASSERT_TRUE(QFile::remove(projectPath_));
    ASSERT_TRUE(QFile::remove(changedPath));
    changed->extensions.insert(QStringLiteral("testContent"), QStringLiteral("afterProjectDeletion"));
    ASSERT_TRUE(manager.saveProject());
    EXPECT_TRUE(QFileInfo::exists(projectPath_));
    ASSERT_TRUE(manager.loadProject(projectPath_));
    changed = manager.findItemByGuid(changedGuid);
    ASSERT_NE(changed, nullptr);
    EXPECT_EQ(changed->extensions.value(QStringLiteral("testContent")).toString(),
              QStringLiteral("afterProjectDeletion"));
    EXPECT_NE(manager.findItemByGuid(deletedGuid), nullptr);
}

TEST_P(BucketFolderPersistenceTest, NestedFolderPayloadFollowsItsOwnLocationAndRestoresAfterSavedDeletion)
{
    const QString original = QDir(bucketRoot()).filePath(QStringLiteral("Original"));
    const QString nested = QDir(original).filePath(QStringLiteral("Nested"));
    const QString destination = QDir(bucketRoot()).filePath(QStringLiteral("Destination"));
    ASSERT_TRUE(QDir().mkpath(QDir(nested).filePath(QStringLiteral(".hidden-empty"))));
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
    EXPECT_TRUE(QFileInfo::exists(original));
    ASSERT_TRUE(manager.saveProject());
    EXPECT_FALSE(QFileInfo::exists(original));
    const QString movedNested = QDir(destination).filePath(QStringLiteral("Nested"));
    EXPECT_TRUE(QFileInfo(QDir(movedNested).filePath(QStringLiteral(".hidden-empty"))).isDir());
    {
        QFile file(QDir(movedNested).filePath(QStringLiteral(".hidden.bin")));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        EXPECT_EQ(file.readAll(), childBytes);
    }

    UndoRedoManager::instance().undo();
    EXPECT_EQ(project->findFolderByPath(bucketName(), QStringLiteral("Original")), parentFolder);
    EXPECT_FALSE(QFileInfo::exists(original));
    ASSERT_TRUE(manager.saveProject());
    {
        QFile file(QDir(original).filePath(QStringLiteral("ordinary.bin")));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        EXPECT_EQ(file.readAll(), parentBytes);
    }
    EXPECT_FALSE(QFileInfo::exists(nested));
    EXPECT_TRUE(QFileInfo(movedNested).isDir());

    UndoRedoManager::instance().undo();
    ASSERT_TRUE(manager.saveProject());
    EXPECT_TRUE(QFileInfo(nested).isDir());
    EXPECT_FALSE(QFileInfo::exists(movedNested));
    {
        QFile file(QDir(nested).filePath(QStringLiteral(".hidden.bin")));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        EXPECT_EQ(file.readAll(), childBytes);
    }
}

INSTANTIATE_TEST_SUITE_P(AllFolderBuckets,
                         BucketFolderPersistenceTest,
                         ::testing::Values("Actions", "Variables", "Attenuations", "Layers"));

TEST(BucketFolderStoreTest, SavingReplacesIgnoredExternalDocumentsWithLoadedAuthoringState)
{
    ensureApp();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    ProjectConfig config;
    config.name = QStringLiteral("ActionPruneProject");
    config.location = tempDir.path();
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.createProject(config));
    ASSERT_NE(manager.currentProject(), nullptr);

    const QString projectRoot = QFileInfo(manager.currentProject()->projectPath).absolutePath();
    const QDir actionsDir(QDir(projectRoot).filePath(QStringLiteral("Actions")));
    const QString foreignPath = actionsDir.filePath(QStringLiteral("Foreign.vp"));
    const QString stalePath = actionsDir.filePath(QStringLiteral("Stale.vp"));
    ASSERT_TRUE(writeJson(
        foreignPath,
        QJsonObject{
            { QStringLiteral("type"), QStringLiteral("Foreign") },
            { QStringLiteral("nested"),
              QJsonObject{ { QStringLiteral("type"), QStringLiteral("Action") } } }
        }));
    ASSERT_TRUE(writeJson(
        stalePath,
        QJsonObject{ { QStringLiteral("type"), QStringLiteral("Action") } }));

    EXPECT_TRUE(manager.saveProject());
    EXPECT_FALSE(QFileInfo::exists(foreignPath));
    EXPECT_FALSE(QFileInfo::exists(stalePath));
    manager.closeProject();
}

TEST(BucketFolderStoreTest, KeepingEditsOverwritesBusBucketChangesWithoutCreatingFolderObjects)
{
    ensureApp();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    ProjectConfig config;
    config.name = QStringLiteral("BusBucketOverwrite");
    config.location = tempDir.path();
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.createProject(config));
    auto* bus = manager.createItem(manager.getMasterBusItem(), QStringLiteral("LoadedBus"),
                                    ProjectItem::Type::Bus);
    ASSERT_NE(bus, nullptr);
    const QUuid busGuid = bus->guid;
    ASSERT_TRUE(manager.saveProject());
    const QString projectPath = manager.currentProject()->projectPath;
    const QDir bucket(QDir(QFileInfo(projectPath).absolutePath()).filePath(QStringLiteral("Busses")));
    const QString keptPath = bucket.filePath(QStringLiteral("kept.json"));
    const QJsonObject keptContent{{QStringLiteral("value"), QStringLiteral("loaded")}};
    ASSERT_TRUE(writeJson(keptPath, keptContent));
    const QString emptyPath = bucket.filePath(QStringLiteral(".Archive/Empty"));
    ASSERT_TRUE(QDir().mkpath(emptyPath));
    ASSERT_TRUE(writeJson(bucket.filePath(QStringLiteral(".Archive/.hidden.json")), keptContent));
    ASSERT_TRUE(manager.loadProject(projectPath));
    EXPECT_FALSE(ProjectManager::bucketAllowsFolder(QStringLiteral("Busses")));
    EXPECT_TRUE(manager.currentProject()->folderRootsForBucket(QStringLiteral("Busses")).empty());
    bus = manager.findItemByGuid(busGuid);
    ASSERT_NE(bus, nullptr);
    const QString busPath = manager.currentProject()->objectFilePath(bus);
    ASSERT_TRUE(QFile::remove(busPath));
    const QString damagedPath = bucket.filePath(QStringLiteral("Damaged.vp"));
    ASSERT_TRUE(writeJson(damagedPath, {{QStringLiteral("external"), true}}));
    const QString addedPath = bucket.filePath(QStringLiteral("External"));
    ASSERT_TRUE(QDir().mkpath(addedPath));
    ASSERT_TRUE(writeJson(QDir(addedPath).filePath(QStringLiteral("ordinary.json")),
                          {{QStringLiteral("external"), true}}));
    ASSERT_TRUE(writeJson(keptPath, {{QStringLiteral("value"), QStringLiteral("external")}}));
    ASSERT_TRUE(QDir(emptyPath).removeRecursively());
    bus->extensions.insert(QStringLiteral("testContent"), QStringLiteral("unsaved"));
    ASSERT_TRUE(manager.saveProject());
    EXPECT_FALSE(QFileInfo::exists(damagedPath));
    EXPECT_FALSE(QFileInfo::exists(addedPath));
    EXPECT_TRUE(QFileInfo::exists(busPath));
    EXPECT_TRUE(QFileInfo(emptyPath).isDir());
    for (const QString& path : {keptPath, bucket.filePath(QStringLiteral(".Archive/.hidden.json"))}) {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        EXPECT_EQ(QJsonDocument::fromJson(file.readAll()).object(), keptContent);
    }
    ASSERT_TRUE(manager.loadProject(projectPath));
    bus = manager.findItemByGuid(busGuid);
    ASSERT_NE(bus, nullptr);
    EXPECT_EQ(bus->extensions.value(QStringLiteral("testContent")).toString(),
              QStringLiteral("unsaved"));
    EXPECT_TRUE(manager.currentProject()->folderRootsForBucket(QStringLiteral("Busses")).empty());
    manager.closeProject();
}

}
}
