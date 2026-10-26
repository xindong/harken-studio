#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "Widgets/Hierarchy/Hierarchy.h"
#include "Widgets/Shared/InlineRenameEdit.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QColorDialog>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>

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

class AnchorProbeHierarchy : public Hierarchy
{
public:
    using ProjectTreePanel::onRename;
    using ProjectTreePanel::setItemColorViaDialog;

    void armMenuContext(ProjectItem* item)
    {
        prepareItemContextMenu(treeItemForProjectItem(item), item);
    }
};

class ProjectTreePanelAnchorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        ASSERT_TRUE(tempDir_.isValid());

        panel_ = std::make_unique<AnchorProbeHierarchy>();

        ProjectConfig config;
        config.name = QStringLiteral("PanelAnchorProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(ProjectManager::instance().createProject(config));

        auto& pm = ProjectManager::instance();
        contextLayer_ = pm.createItem(nullptr, QStringLiteral("Ctx_Layer"), ProjectItem::Type::Layer);
        ASSERT_NE(contextLayer_, nullptr);
        selectedLayer_ = pm.createItem(nullptr, QStringLiteral("Sel_Layer"), ProjectItem::Type::Layer);
        ASSERT_NE(selectedLayer_, nullptr);
        ASSERT_TRUE(panel_->selectProjectItem(selectedLayer_));
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
    }

    QTemporaryDir tempDir_;
    std::unique_ptr<AnchorProbeHierarchy> panel_;
    ProjectItem* contextLayer_ = nullptr;
    ProjectItem* selectedLayer_ = nullptr;
};

}

TEST_F(ProjectTreePanelAnchorTest, ColorDialogDoesNotUseDestroyedTargets)
{
    auto& pm = ProjectManager::instance();
    const auto originalColor = selectedLayer_->color;
    panel_->setSelectedProjectItems({contextLayer_, selectedLayer_});
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QColorDialog*>(QApplication::activeModalWidget());
        ASSERT_NE(dialog, nullptr);
        ASSERT_TRUE(pm.deleteItem(contextLayer_));
        UndoRedoManager::instance().clear();
        dialog->setCurrentColor(QColor(12, 34, 56));
        dialog->accept();
    });
    panel_->setItemColorViaDialog({contextLayer_, selectedLayer_});
    EXPECT_EQ(selectedLayer_->color, originalColor);
}

TEST_F(ProjectTreePanelAnchorTest, InlineRenameDoesNotTouchADeletedTarget)
{
    auto& pm = ProjectManager::instance();
    const QUuid guid = contextLayer_->guid;
    const QString survivingName = selectedLayer_->name;
    panel_->armMenuContext(contextLayer_);
    panel_->onRename();
    QPointer<InlineRenameEdit> editor = panel_->findChild<InlineRenameEdit*>();
    ASSERT_NE(editor, nullptr);
    editor->setText(QStringLiteral("RenamedAfterDeletion"));
    ASSERT_TRUE(pm.deleteItem(contextLayer_));
    UndoRedoManager::instance().clear();
    if (editor) editor->finish(true);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_EQ(pm.findItemByGuid(guid), nullptr);
    EXPECT_EQ(selectedLayer_->name, survivingName);
}
