#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "Widgets/Hierarchy/Hierarchy.h"
#include "Widgets/Shared/InlineRenameEdit.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QColorDialog>
#include <QLineEdit>
#include <QKeyEvent>
#include <QPointer>
#include <QScrollBar>
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
    using ProjectTreePanel::rebuildTree;
    using ProjectTreePanel::setItemColorViaDialog;
    using ProjectTreePanel::treeItemForProjectItem;

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

TEST_F(ProjectTreePanelAnchorTest, RefreshPreservesEachPanelsSelectionExpansionAndScroll)
{
    auto& pm = ProjectManager::instance();
    QList<ProjectItem*> children;
    for (int index = 0; index < 40; ++index) {
        auto* child = pm.createItem(selectedLayer_, QStringLiteral("Child_%1").arg(index),
                                    ProjectItem::Type::Sound);
        ASSERT_NE(child, nullptr);
        children.append(child);
    }
    ASSERT_NE(pm.createItem(contextLayer_, QStringLiteral("ContextChild"),
                            ProjectItem::Type::Sound), nullptr);
    AnchorProbeHierarchy other;
    panel_->resize(320, 240);
    other.resize(320, 240);
    panel_->show();
    other.show();
    ASSERT_TRUE(panel_->selectProjectItem(children.front()));
    ASSERT_TRUE(other.selectProjectItem(children.back()));
    panel_->treeItemForProjectItem(contextLayer_)->setExpanded(false);
    other.treeItemForProjectItem(contextLayer_)->setExpanded(true);
    QApplication::processEvents();
    auto* firstScroll = panel_->treeWidget()->verticalScrollBar();
    auto* secondScroll = other.treeWidget()->verticalScrollBar();
    ASSERT_GT(firstScroll->maximum(), 0);
    firstScroll->setValue(firstScroll->maximum() / 2);
    secondScroll->setValue(0);
    const int firstPosition = firstScroll->value();
    int activations = 0;
    QObject::connect(panel_.get(), &ProjectTreePanel::itemSelected, &other,
            [&] { ++activations; });
    QObject::connect(&other, &ProjectTreePanel::itemSelected, &other,
            [&] { ++activations; });

    panel_->rebuildTree();
    other.rebuildTree();
    QApplication::processEvents();

    EXPECT_EQ(panel_->selectedItems(), QList<ProjectItem*>{children.front()});
    EXPECT_EQ(other.selectedItems(), QList<ProjectItem*>{children.back()});
    EXPECT_EQ(panel_->selectedItem(), children.front());
    EXPECT_EQ(other.selectedItem(), children.back());
    EXPECT_FALSE(panel_->treeItemForProjectItem(contextLayer_)->isExpanded());
    EXPECT_TRUE(other.treeItemForProjectItem(contextLayer_)->isExpanded());
    EXPECT_EQ(firstScroll->value(), firstPosition);
    EXPECT_EQ(secondScroll->value(), 0);
    EXPECT_EQ(activations, 0);

    auto* search = panel_->findChild<QLineEdit*>(QStringLiteral("projectSearchBox"));
    ASSERT_NE(search, nullptr);
    search->setText(QStringLiteral("Child"));
    QApplication::processEvents();
    panel_->treeItemForProjectItem(contextLayer_)->setExpanded(false);
    firstScroll->setValue(firstScroll->maximum() / 2);
    const int filteredPosition = firstScroll->value();
    ASSERT_NE(pm.createItem(contextLayer_, QStringLiteral("AnotherChild"),
                            ProjectItem::Type::Sound), nullptr);
    ASSERT_TRUE(pm.renameItem(selectedLayer_, QStringLiteral("Renamed_Layer")));
    QApplication::processEvents();
    EXPECT_FALSE(panel_->treeItemForProjectItem(contextLayer_)->isExpanded());
    EXPECT_EQ(panel_->selectedItem(), children.front());
    EXPECT_EQ(firstScroll->value(), filteredPosition);
    EXPECT_EQ(activations, 0);

    auto* bucket = panel_->treeItemForProjectItem(contextLayer_);
    while (bucket->parent()) bucket = bucket->parent();
    panel_->treeWidget()->setCurrentItem(bucket, 0, QItemSelectionModel::ClearAndSelect);
    panel_->rebuildTree();
    bucket = panel_->treeItemForProjectItem(contextLayer_);
    while (bucket->parent()) bucket = bucket->parent();
    EXPECT_TRUE(bucket->isSelected());
    EXPECT_EQ(panel_->treeWidget()->currentItem(), bucket);
    EXPECT_EQ(activations, 0);
}

TEST_F(ProjectTreePanelAnchorTest, KeyboardCurrentFolderAndCategoryClearObjectContextInMixedSelections)
{
    auto* folder = ProjectManager::instance().createFolder(
        nullptr, QStringLiteral("MixedFolder"), QStringLiteral("Layers"));
    ASSERT_NE(folder, nullptr);
    panel_->resize(320, 480);
    panel_->show();
    ASSERT_TRUE(panel_->selectProjectItem(selectedLayer_));
    auto* tree = panel_->treeWidget();
    tree->setFocus();
    QApplication::processEvents();
    QList<ProjectItem*> activations;
    QObject observer;
    QObject::connect(panel_.get(), &ProjectTreePanel::itemSelected, &observer,
                    [&](ProjectItem* item) { activations.append(item); });

    QKeyEvent next(QEvent::KeyPress, Qt::Key_Down, Qt::ShiftModifier);
    QApplication::sendEvent(tree, &next);
    ASSERT_EQ(panel_->selectedFolder(), folder);
    EXPECT_TRUE(panel_->selectedItems().contains(selectedLayer_));
    EXPECT_TRUE(panel_->selectedFolders().contains(folder));
    ASSERT_EQ(activations.size(), 1);
    EXPECT_EQ(activations.back(), nullptr);

    QKeyEvent first(QEvent::KeyPress, Qt::Key_Home, Qt::ShiftModifier);
    QApplication::sendEvent(tree, &first);
    ASSERT_NE(tree->currentItem(), nullptr);
    EXPECT_EQ(tree->currentItem()->parent(), nullptr);
    EXPECT_FALSE(panel_->selectedItems().isEmpty());
    ASSERT_EQ(activations.size(), 2);
    EXPECT_EQ(activations.back(), nullptr);
}
