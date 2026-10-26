#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QTemporaryDir>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

class OutputSendExclusionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
        VNX::UndoRedoManager::instance().clear();
        ASSERT_TRUE(tempDir.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("OutputSendExclusionProject");
        config.location = tempDir.path();
        ASSERT_TRUE(pm.createProject(config));
    }

    void TearDown() override
    {
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
        VNX::UndoRedoManager::instance().clear();
    }

    QTemporaryDir tempDir;
};

}

TEST_F(OutputSendExclusionTest, MasterKeepsItsOwnUuidAcrossSaveAndOpen)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* master = pm.findBusItem(VNX::kMasterBusID);
    ASSERT_NE(master, nullptr);

    const auto masterId = master->runtimeId();
    ASSERT_TRUE(pm.saveProject());
    const auto projectPath = pm.currentProject()->projectPath;
    pm.closeProject();
    ASSERT_TRUE(pm.loadProject(projectPath));
    EXPECT_EQ(pm.getMasterBusItem()->runtimeId(), masterId);
    pm.closeProject();
    VNX::ProjectConfig config;
    config.name = QStringLiteral("IndependentMasterProject");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));
    EXPECT_NE(pm.getMasterBusItem()->runtimeId(), masterId);
}
