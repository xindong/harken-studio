#include "ItemTypeCatalog.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "Widgets/Shared/ProjectItemMenuController.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QMenu>
#include <QTemporaryDir>

#include <memory>

using namespace VNX;

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

QAction* newChildAction(ProjectItemMenuController* controller, ProjectItem::Type type)
{
    const QString label = ItemTypeCatalog::displayName(type);
    const QList<QAction*> actions = controller->newChildMenu()->actions();
    for (QAction* act : actions) {
        if (!act->isSeparator() && act->text() == label) return act;
    }
    return nullptr;
}

class ProjectItemMenuControllerAnchorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        ASSERT_TRUE(tempDir_.isValid());

        ProjectConfig config;
        config.name = QStringLiteral("MenuAnchorProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(ProjectManager::instance().createProject(config));

        auto& pm = ProjectManager::instance();
        contextLayer_ = pm.createItem(nullptr, QStringLiteral("Ctx_Layer"), ProjectItem::Type::Layer);
        ASSERT_NE(contextLayer_, nullptr);
        selectedLayer_ = pm.createItem(nullptr, QStringLiteral("Sel_Layer"), ProjectItem::Type::Layer);
        ASSERT_NE(selectedLayer_, nullptr);

        ProjectItemMenuController::Callbacks cb;
        cb.getSelected = [this]() { return selectedLayer_; };
        cb.getSelection = [this]() { return QList<ProjectItem*>{selectedLayer_}; };
        controller_ = std::make_unique<ProjectItemMenuController>(std::move(cb));
    }

    void TearDown() override
    {
        controller_.reset();
        ProjectManager::instance().closeProject();
    }

    QTemporaryDir tempDir_;
    std::unique_ptr<ProjectItemMenuController> controller_;
    ProjectItem* contextLayer_ = nullptr;
    ProjectItem* selectedLayer_ = nullptr;
};

}

TEST_F(ProjectItemMenuControllerAnchorTest, CreateLandsOnContextRowWhenItDiffersFromSelection)
{
    controller_->updateActionVisibility(contextLayer_, QString());

    ProjectItem* created = controller_->createByType(ProjectItem::Type::Sound);
    ASSERT_NE(created, nullptr);
    EXPECT_EQ(created->parent, contextLayer_);
    EXPECT_NE(created->parent, selectedLayer_);
}

TEST_F(ProjectItemMenuControllerAnchorTest, BucketRowContextCreatesAtBucketRootNotUnderSelection)
{
    QMenu menu;
    controller_->buildNewChildMenu(&menu);
    ProjectData* project = ProjectManager::instance().currentProject();
    ASSERT_NE(project, nullptr);

    for (const char* bucket : {ItemTypeCatalog::kActionsFolderName,
                               ItemTypeCatalog::kVariablesFolderName,
                               ItemTypeCatalog::kAttenuationsFolderName,
                               ItemTypeCatalog::kLayersFolderName}) {
        SCOPED_TRACE(bucket);
        controller_->updateActionVisibility(nullptr, QLatin1String(bucket));

        QAction* folderAction = nullptr;
        for (QAction* action : controller_->newChildMenu()->actions())
            if (action->text() == QStringLiteral("Folder")) folderAction = action;
        ASSERT_NE(folderAction, nullptr);
        EXPECT_TRUE(folderAction->isVisible());
        EXPECT_TRUE(folderAction->isEnabled());

        auto* created = controller_->createFolder();
        ASSERT_NE(created, nullptr);
        EXPECT_EQ(created->parent, nullptr);
        EXPECT_EQ(project->folderBucket(created), QLatin1String(bucket));
    }
}

TEST_F(ProjectItemMenuControllerAnchorTest, FolderRowContextCreatesActionAtContextAddress)
{
    controller_->updateActionVisibility(nullptr, ItemTypeCatalog::kActionsFolderName,
                                        QStringLiteral("Combat"));

    ProjectItem* created = controller_->createByType(ProjectItem::Type::Action);
    ASSERT_NE(created, nullptr);
    EXPECT_EQ(created->parent, nullptr);
    EXPECT_EQ(ProjectManager::instance().currentProject()->itemFolderPath(created), QStringLiteral("Combat"));
}

TEST_F(ProjectItemMenuControllerAnchorTest, FolderObjectContextCreatesPackAtContextAddress)
{
    auto& pm = ProjectManager::instance();
    QMenu menu;
    controller_->buildNewChildMenu(&menu);
    for (const auto type : {ProjectItem::Type::Action, ProjectItem::Type::Variable,
                           ProjectItem::Type::Attenuation, ProjectItem::Type::Layer,
                           ProjectItem::Type::Sound, ProjectItem::Type::Container,
                           ProjectItem::Type::Track, ProjectItem::Type::Piece}) {
        SCOPED_TRACE(static_cast<int>(type));
        const QString bucket = ItemTypeCatalog::folderNameOf(ItemTypeCatalog::bucketOf(type));
        auto* folder = pm.createFolder(nullptr, QStringLiteral("Combat"), bucket);
        ASSERT_NE(folder, nullptr);
        controller_->updateActionVisibility(nullptr, bucket, folder->name);

        QAction* action = newChildAction(controller_.get(), type);
        ASSERT_NE(action, nullptr);
        EXPECT_TRUE(action->isVisible());
        EXPECT_TRUE(action->isEnabled());
        ProjectItem* created = controller_->createByType(type);
        ASSERT_NE(created, nullptr);
        EXPECT_EQ(created->parent, nullptr);
        EXPECT_EQ(ProjectManager::instance().currentProject()->itemFolderPath(created), folder->name);
        EXPECT_TRUE(folder->children.empty());
    }
}

TEST_F(ProjectItemMenuControllerAnchorTest, PathsWithoutMenuContextFallBackToSelection)
{
    ProjectItem* created = controller_->createByType(ProjectItem::Type::Sound);
    ASSERT_NE(created, nullptr);
    EXPECT_EQ(created->parent, selectedLayer_);
}

TEST_F(ProjectItemMenuControllerAnchorTest, ClearMenuContextRestoresSelectionFallback)
{
    controller_->updateActionVisibility(contextLayer_, QString());
    controller_->clearMenuContext();

    ProjectItem* created = controller_->createByType(ProjectItem::Type::Sound);
    ASSERT_NE(created, nullptr);
    EXPECT_EQ(created->parent, selectedLayer_);
}

TEST_F(ProjectItemMenuControllerAnchorTest, NewChildEnableAnchorsToContextRowNotSelection)
{
    ProjectItem* master = ProjectManager::instance().getMasterBusItem();
    ASSERT_NE(master, nullptr);

    ProjectItemMenuController::Callbacks cb;
    cb.getSelected = []() -> ProjectItem* { return nullptr; };
    ProjectItemMenuController noSelectionController(std::move(cb));

    QMenu menu;
    noSelectionController.buildNewChildMenu(&menu);
    noSelectionController.updateActionVisibility(master, QString());

    QAction* busAction = newChildAction(&noSelectionController, ProjectItem::Type::Bus);
    ASSERT_NE(busAction, nullptr);
    EXPECT_TRUE(busAction->isVisible());
    EXPECT_TRUE(busAction->isEnabled());
}
