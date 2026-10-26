#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "Widgets/ProjectTree/ProjectTreePanel.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QTemporaryDir>

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

class ProjectTreeDragDropTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        ASSERT_TRUE(tempDir_.isValid());

        ProjectConfig config;
        config.name = QStringLiteral("DragDropProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(ProjectManager::instance().createProject(config));

        auto& pm = ProjectManager::instance();
        layerRoot_ = pm.createItem(nullptr, QStringLiteral("Drag_Root"), ProjectItem::Type::Layer);
        ASSERT_NE(layerRoot_, nullptr);
        subLayer_ = pm.createItem(layerRoot_, QStringLiteral("Drag_Sub"), ProjectItem::Type::Layer);
        ASSERT_NE(subLayer_, nullptr);
        subSubLayer_ = pm.createItem(subLayer_, QStringLiteral("Drag_SubSub"), ProjectItem::Type::Layer);
        ASSERT_NE(subSubLayer_, nullptr);
        layerAlt_ = pm.createItem(nullptr, QStringLiteral("Drag_Alt"), ProjectItem::Type::Layer);
        ASSERT_NE(layerAlt_, nullptr);
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
    }

    QTemporaryDir tempDir_;

    ProjectItem* layerRoot_ = nullptr;
    ProjectItem* subLayer_ = nullptr;
    ProjectItem* subSubLayer_ = nullptr;
    ProjectItem* layerAlt_ = nullptr;
};

}

TEST_F(ProjectTreeDragDropTest, WouldCreateCycleWalksTargetAncestorChain)
{
    EXPECT_TRUE(ProjectTreePanel::wouldCreateCycle(layerRoot_, layerRoot_));
    EXPECT_TRUE(ProjectTreePanel::wouldCreateCycle(layerRoot_, subLayer_));
    EXPECT_TRUE(ProjectTreePanel::wouldCreateCycle(layerRoot_, subSubLayer_));
    EXPECT_TRUE(ProjectTreePanel::wouldCreateCycle(subLayer_, subSubLayer_));

    EXPECT_FALSE(ProjectTreePanel::wouldCreateCycle(subLayer_, layerRoot_));
    EXPECT_FALSE(ProjectTreePanel::wouldCreateCycle(layerRoot_, layerAlt_));
}
