#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "Components/VSelection.h"
#include "ThemeManager.h"
#include "UndoCommands.h"
#include "Widgets/Hierarchy/Hierarchy.h"
#include "Widgets/ProjectExplorer/ProjectExplorer.h"
#include "Widgets/Shared/InlineRenameEdit.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QColorDialog>
#include <QImage>
#include <QLineEdit>
#include <QKeyEvent>
#include <QPixmap>
#include <QPointer>
#include <QScopeGuard>
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

class PresentationProbeExplorer : public ProjectExplorer
{
public:
    using ProjectTreePanel::treeItemForProjectItem;
    using ProjectTreePanel::updateTreeItem;
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

TEST_F(ProjectTreePanelAnchorTest, RefreshedPresentationMatchesFreshRowsAcrossAuthoringAndThemeChanges)
{
    auto& pm = ProjectManager::instance();
    auto* container = pm.createItem(selectedLayer_, QStringLiteral("PresentationContainer"),
                                    ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    auto* selection = container->comp<VSelection>();
    ASSERT_NE(selection, nullptr);
    PresentationProbeExplorer explorer;
    auto& theme = ThemeManager::instance();
    const auto committedTheme = theme.currentThemeId();
    const auto restoreTheme = qScopeGuard([&] { theme.selectTheme(committedTheme); });

    const auto verify = [&](const char* stage) {
        SCOPED_TRACE(stage);
        explorer.onProjectSaved();
        explorer.onProjectSaved();
        for (auto* item : {selectedLayer_, container}) {
            SCOPED_TRACE(item->name.toStdString());
            auto* row = explorer.treeItemForProjectItem(item);
            ASSERT_NE(row, nullptr);
            QTreeWidgetItem fresh;
            explorer.updateTreeItem(&fresh, item);
            EXPECT_EQ(row->text(0), fresh.text(0));
            EXPECT_EQ(row->font(0).resolve(explorer.treeWidget()->font()),
                      fresh.font(0).resolve(explorer.treeWidget()->font()));
            EXPECT_EQ(row->toolTip(0), fresh.toolTip(0));
            EXPECT_EQ(row->data(0, Qt::ForegroundRole), fresh.data(0, Qt::ForegroundRole));
            const int extent = theme.metrics().objectIconExtent;
            for (const auto mode : {QIcon::Normal, QIcon::Disabled, QIcon::Selected}) {
                for (const auto state : {QIcon::Off, QIcon::On}) {
                    for (const qreal scale : {1.0, 2.0}) {
                        const auto actual = row->icon(0).pixmap(
                            QSize(extent, extent), scale, mode, state).toImage();
                        const auto expected = fresh.icon(0).pixmap(
                            QSize(extent, extent), scale, mode, state).toImage();
                        ASSERT_FALSE(expected.isNull());
                        EXPECT_EQ(actual, expected);
                    }
                }
            }
        }
    };

    verify("initial");
    ASSERT_TRUE(pm.setItemsColor({selectedLayer_, container}, QColor(12, 146, 213).rgba()));
    verify("color");
    selection->mode = SelectionMode::Sequence;
    explorer.onItemPropertyChanged(container, QStringLiteral("mode"));
    verify("sequence");
    selection->playType = SelectionPlayType::Queue;
    explorer.onItemPropertyChanged(container, QStringLiteral("playType"));
    verify("queue");

    ASSERT_TRUE(theme.selectTheme(builtinThemeId()).ok());
    ASSERT_TRUE(theme.reloadTheme().ok());
    verify("theme reloaded");
    ASSERT_TRUE(theme.selectTheme(committedTheme).ok());
    verify("theme restored");

    ASSERT_TRUE(pm.saveProject());
    verify("saved");
    EXPECT_FALSE(container->isDirtyObject());
    EXPECT_FALSE(explorer.treeItemForProjectItem(container)->font(0).italic());
    ASSERT_TRUE(pm.setItemsColor({selectedLayer_, container}, 0));
    selection->mode = SelectionMode::Random;
    selection->playType = SelectionPlayType::Single;
    explorer.onItemPropertyChanged(container, QStringLiteral("mode"));
    verify("restored authoring");
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
