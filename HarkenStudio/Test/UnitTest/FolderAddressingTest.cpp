#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/PathUtils.h"
#include "NameValidator.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "Support/AITestSupport.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

using namespace VNX;

namespace {

class FolderAddressingTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TestSupport::ensureApp();
        ASSERT_TRUE(tempDir_.isValid());

        ProjectConfig config;
        config.name = QStringLiteral("FolderAddressProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(ProjectManager::instance().createProject(config));
        ProjectData* project = ProjectManager::instance().currentProject();
        ASSERT_NE(project, nullptr);
        projectPath_ = project->projectPath;
        projectRoot_ = QFileInfo(projectPath_).absolutePath();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
    }

    PhysicalFolder* createFolder(const QString& name)
    {
        return ProjectManager::instance().createFolder(nullptr, name, QStringLiteral("Actions"));
    }

    QString actionFile(const ProjectItem* item) const
    {
        return QDir(projectRoot_).filePath(QStringLiteral("Actions/")
            + NameValidator::objectFileName(item->name, item->guid));
    }

    QTemporaryDir tempDir_;
    QString projectPath_;
    QString projectRoot_;
};

}

TEST_F(FolderAddressingTest, FailedSaveKeepsCommittedProjectFile)
{
    auto& pm = ProjectManager::instance();
    ASSERT_TRUE(pm.saveProject());
    auto* folder = createFolder(QStringLiteral("Uncommitted"));
    ASSERT_NE(folder, nullptr);

    QFile rootFile(projectPath_);
    ASSERT_TRUE(rootFile.open(QIODevice::ReadOnly));
    const QByteArray committedProject = rootFile.readAll();
    rootFile.close();

    ProjectItem* action = pm.createItem(
        nullptr, QStringLiteral("Play_Blocked"), ProjectItem::Type::Action,
        QStringLiteral("Uncommitted"));
    ASSERT_NE(action, nullptr);
    ASSERT_NE(pm.createFolder(folder, NameValidator::objectFileName(action->name, action->guid),
                               QStringLiteral("Actions")), nullptr);

    EXPECT_FALSE(pm.saveProject());
    ASSERT_TRUE(rootFile.open(QIODevice::ReadOnly));
    EXPECT_EQ(rootFile.readAll(), committedProject);
}

TEST_F(FolderAddressingTest, FailedSaveRestoresEarlierPackWrites)
{
    auto& pm = ProjectManager::instance();
    ProjectItem* first = pm.createItem(
        nullptr, QStringLiteral("Play_First"), ProjectItem::Type::Action);
    ASSERT_NE(first, nullptr);
    ProjectItem* blocked = pm.createItem(
        nullptr, QStringLiteral("Play_Blocked"), ProjectItem::Type::Action,
        QStringLiteral("Blocked"));
    ASSERT_NE(blocked, nullptr);
    ASSERT_TRUE(pm.saveProject());

    const QString firstPath = actionFile(first);
    QFile firstFile(firstPath);
    ASSERT_TRUE(firstFile.open(QIODevice::ReadOnly));
    const QByteArray committedPack = firstFile.readAll();
    firstFile.close();

    first->name = QStringLiteral("Play_First_Changed");
    first->markDirty();
    auto* folder = pm.currentProject()->findFolderByPath(QStringLiteral("Actions"), QStringLiteral("Blocked"));
    ASSERT_NE(folder, nullptr);
    ASSERT_NE(pm.createFolder(folder, NameValidator::objectFileName(blocked->name, blocked->guid),
                               QStringLiteral("Actions")), nullptr);

    EXPECT_FALSE(pm.saveProject());
    EXPECT_FALSE(QFileInfo::exists(actionFile(first)));
    ASSERT_TRUE(firstFile.open(QIODevice::ReadOnly));
    EXPECT_EQ(firstFile.readAll(), committedPack);
}

TEST_F(FolderAddressingTest, FailedSaveRestoresAssetRegistry)
{
    auto& pm = ProjectManager::instance();
    ASSERT_NE(createFolder(QStringLiteral("Blocked")), nullptr);
    ASSERT_TRUE(pm.saveProject());

    const QString registryPath = QDir(PathUtils::getAssetRegistryCacheDir(projectRoot_))
                                     .filePath(QStringLiteral("assets.json"));
    QFile registryFile(registryPath);
    ASSERT_TRUE(registryFile.open(QIODevice::ReadOnly));
    const QByteArray committedRegistry = registryFile.readAll();
    registryFile.close();

    AssetInfo asset;
    asset.sourceID = AssetRegistry::instance().allocateStableID();
    asset.relativePath = QStringLiteral("uncommitted.wav");
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(asset));
    ProjectItem* blocked = pm.createItem(
        nullptr, QStringLiteral("Play_Blocked"), ProjectItem::Type::Action,
        QStringLiteral("Blocked"));
    ASSERT_NE(blocked, nullptr);
    auto* folder = pm.currentProject()->findFolderByPath(QStringLiteral("Actions"), QStringLiteral("Blocked"));
    ASSERT_NE(folder, nullptr);
    ASSERT_NE(pm.createFolder(folder, NameValidator::objectFileName(blocked->name, blocked->guid),
                               QStringLiteral("Actions")), nullptr);

    EXPECT_FALSE(pm.saveProject());
    ASSERT_TRUE(registryFile.open(QIODevice::ReadOnly));
    EXPECT_EQ(registryFile.readAll(), committedRegistry);
}
