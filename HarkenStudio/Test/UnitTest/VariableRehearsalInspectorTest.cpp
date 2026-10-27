#include "ActionManager.h"
#include "Components/VSelection.h"
#include "Components/VVariable.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
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

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

// Every save stamps metadata.modifiedDate with a second-resolution clock
// reading, so two saves that straddle a second boundary differ regardless
// of what the editor did in between. Drop it before comparing content.
QByteArray readProjectContent(const QString& path)
{
    QJsonObject root = QJsonDocument::fromJson(readFile(path)).object();
    QJsonObject content = root.value(QStringLiteral("content")).toObject();
    QJsonObject metadata = content.value(QStringLiteral("metadata")).toObject();
    metadata.remove(QStringLiteral("modifiedDate"));
    content[QStringLiteral("metadata")] = metadata;
    root[QStringLiteral("content")] = content;
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

}

class VariableRehearsalInspectorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& projectManager = VNX::ProjectManager::instance();
        if (projectManager.isProjectOpen())
            projectManager.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerProjectActions(VNX::ActionManager::instance(), &projectManager,
                                    nullptr);
        ASSERT_TRUE(tempDir_.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("VariableRehearsalProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(projectManager.createProject(config));

        container_ = projectManager.createItem(nullptr, QStringLiteral("VR_Container"),
                                               VNX::ProjectItem::Type::Container);
        child_ = projectManager.createItem(container_, QStringLiteral("VR_Child"),
                                           VNX::ProjectItem::Type::Sound);
        smooth_ = projectManager.createItem(nullptr, QStringLiteral("VR_Smooth"),
                                            VNX::ProjectItem::Type::Variable);
        ASSERT_NE(container_, nullptr);
        ASSERT_NE(child_, nullptr);
        ASSERT_NE(smooth_, nullptr);

        auto* smoothVariable = smooth_->compOrCreate<VNX::VVariable>();
        smoothVariable->rangeMin = 0.0f;
        smoothVariable->rangeMax = 100.0f;
        smoothVariable->defaultValue = 0.0f;

        auto* selection = container_->compOrCreate<VNX::VSelection>();
        selection->mode = VNX::SelectionMode::Match;
        auto grid = selection->matchGrid();
        ASSERT_TRUE(grid.addAxis(smooth_->guid));
        ASSERT_TRUE(grid.addRow(child_->guid));
        ASSERT_TRUE(grid.setCell(0, 0, 0));
        ASSERT_TRUE(selection->selections.shared.setMatchGrid(grid));

        VNX::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        auto& projectManager = VNX::ProjectManager::instance();
        if (projectManager.isProjectOpen())
            projectManager.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
    }

    QTemporaryDir tempDir_;
    VNX::ProjectItem* container_ = nullptr;
    VNX::ProjectItem* child_ = nullptr;
    VNX::ProjectItem* smooth_ = nullptr;
};

TEST_F(VariableRehearsalInspectorTest, RehearsalDoesNotTouchDirtyUndoOrProjectBytes)
{
    auto& projectManager = VNX::ProjectManager::instance();
    ASSERT_TRUE(projectManager.saveProject());
    const QString projectPath = projectManager.currentProject()->projectPath;
    const QByteArray before = readProjectContent(projectPath);
    ASSERT_FALSE(before.isEmpty());
    auto* undo = VNX::UndoRedoManager::instance().undoStack();
    undo->setClean();
    const int count = undo->count();
    const int index = undo->index();
    const bool containerDirty = container_->isDirtyObject();
    const bool variableDirty = smooth_->isDirtyObject();

    ASSERT_TRUE(projectManager.variableRehearsal().setOverride(smooth_->guid, 62.5f));

    EXPECT_EQ(undo->count(), count);
    EXPECT_EQ(undo->index(), index);
    EXPECT_TRUE(undo->isClean());
    EXPECT_EQ(container_->isDirtyObject(), containerDirty);
    EXPECT_EQ(smooth_->isDirtyObject(), variableDirty);
    ASSERT_TRUE(projectManager.saveProject());
    EXPECT_EQ(readProjectContent(projectPath), before);
}
