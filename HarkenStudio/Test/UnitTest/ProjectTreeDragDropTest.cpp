#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "Widgets/Hierarchy/Hierarchy.h"
#include "Widgets/ProjectTree/ProjectTreeWidget.h"
#include "Widgets/Shared/TreeRowStyle.h"
#include "Widgets/Selector/SelectorWindow.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QTemporaryDir>
#include <QTreeWidget>

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

void sendDoubleClick(QWidget* target, const QPoint& pos)
{
    const QPoint global = target->mapToGlobal(pos);
    QMouseEvent firstPress(QEvent::MouseButtonPress, pos, global,
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &firstPress);
    QMouseEvent firstRelease(QEvent::MouseButtonRelease, pos, global,
                             Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(target, &firstRelease);
    QMouseEvent doublePress(QEvent::MouseButtonDblClick, pos, global,
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &doublePress);
    QMouseEvent secondRelease(QEvent::MouseButtonRelease, pos, global,
                              Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(target, &secondRelease);
}

QString rowLabel(const QTreeWidgetItem* row)
{
    QString text = row->text(0);
    if (text.endsWith(QStringLiteral(" *")))
        text.chop(2);
    return text;
}

QTreeWidgetItem* findRowIn(QTreeWidgetItem* row, const QString& label)
{
    if (rowLabel(row) == label) return row;
    for (int i = 0; i < row->childCount(); ++i) {
        if (QTreeWidgetItem* hit = findRowIn(row->child(i), label))
            return hit;
    }
    return nullptr;
}

QTreeWidgetItem* findRow(QTreeWidget* tree, const QString& label)
{
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        if (QTreeWidgetItem* hit = findRowIn(tree->topLevelItem(i), label))
            return hit;
    }
    return nullptr;
}

class ProjectTreeDragDropTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        ASSERT_TRUE(tempDir_.isValid());

        panel_ = std::make_unique<Hierarchy>();

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
        sound_ = pm.createItem(layerRoot_, QStringLiteral("Drag_Shot"), ProjectItem::Type::Sound);
        ASSERT_NE(sound_, nullptr);
        container_ = pm.createItem(layerRoot_, QStringLiteral("Drag_Box"), ProjectItem::Type::Container);
        ASSERT_NE(container_, nullptr);
        track_ = pm.createItem(layerRoot_, QStringLiteral("Drag_Seq"), ProjectItem::Type::Track);
        ASSERT_NE(track_, nullptr);
        piece_ = pm.createItem(layerRoot_, QStringLiteral("Drag_Intro"), ProjectItem::Type::Piece);
        ASSERT_NE(piece_, nullptr);
        soundInTrack_ = pm.createItem(track_, QStringLiteral("Drag_Shot2"), ProjectItem::Type::Sound);
        ASSERT_NE(soundInTrack_, nullptr);
        action_ = pm.createItem(nullptr, QStringLiteral("Drag_Play"), ProjectItem::Type::Action);
        ASSERT_NE(action_, nullptr);
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
    }

    QTemporaryDir tempDir_;
    std::unique_ptr<Hierarchy> panel_;

    ProjectItem* layerRoot_ = nullptr;
    ProjectItem* subLayer_ = nullptr;
    ProjectItem* subSubLayer_ = nullptr;
    ProjectItem* layerAlt_ = nullptr;
    ProjectItem* sound_ = nullptr;
    ProjectItem* container_ = nullptr;
    ProjectItem* track_ = nullptr;
    ProjectItem* piece_ = nullptr;
    ProjectItem* soundInTrack_ = nullptr;
    ProjectItem* action_ = nullptr;
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
    EXPECT_FALSE(ProjectTreePanel::wouldCreateCycle(nullptr, layerRoot_));
    EXPECT_FALSE(ProjectTreePanel::wouldCreateCycle(layerRoot_, nullptr));
}

TEST_F(ProjectTreeDragDropTest, DragOverOwnDescendantDoesNotLightAllowHighlight)
{
    ASSERT_TRUE(panel_->selectProjectItem(layerRoot_));
    ProjectTreeWidget* tree = panel_->treeWidget();

    EXPECT_FALSE(tree->canDropSelectionOn(layerRoot_));
    EXPECT_FALSE(tree->canDropSelectionOn(subLayer_));
    EXPECT_FALSE(tree->canDropSelectionOn(subSubLayer_));

    EXPECT_TRUE(tree->canDropSelectionOn(layerAlt_));
}

TEST_F(ProjectTreeDragDropTest, MixedSelectionStillAcceptsWhenAnyDraggedItemCanDrop)
{
    ASSERT_TRUE(panel_->selectProjectItem(layerRoot_));
    QTreeWidgetItem* altRow = findRow(panel_->treeWidget(), QStringLiteral("Drag_Alt"));
    ASSERT_NE(altRow, nullptr);
    altRow->setSelected(true);
    ASSERT_EQ(panel_->selectedItems().size(), 2);

    EXPECT_TRUE(panel_->treeWidget()->canDropSelectionOn(subLayer_));
}

TEST_F(ProjectTreeDragDropTest, MatrixRejectsIllegalInternalDropTargets)
{
    ProjectTreeWidget* tree = panel_->treeWidget();

    ASSERT_TRUE(panel_->selectProjectItem(sound_));
    EXPECT_FALSE(tree->canDropSelectionOn(piece_));
    EXPECT_FALSE(tree->canDropSelectionOn(soundInTrack_));
    EXPECT_TRUE(tree->canDropSelectionOn(container_));

    ASSERT_TRUE(panel_->selectProjectItem(action_));
    EXPECT_FALSE(tree->canDropSelectionOn(layerRoot_));
    EXPECT_FALSE(tree->canDropSelectionOn(layerAlt_));
}

TEST_F(ProjectTreeDragDropTest, PieceTargetAcceptsOnlyTrackSelections)
{
    ProjectTreeWidget* tree = panel_->treeWidget();

    ASSERT_TRUE(panel_->selectProjectItem(track_));
    EXPECT_TRUE(tree->canDropSelectionOn(piece_));

    ASSERT_TRUE(panel_->selectProjectItem(container_));
    EXPECT_FALSE(tree->canDropSelectionOn(piece_));

    ASSERT_TRUE(panel_->selectProjectItem(subLayer_));
    EXPECT_FALSE(tree->canDropSelectionOn(piece_));
}

TEST_F(ProjectTreeDragDropTest, BucketRowTargetFollowsTopLevelTables)
{
    ProjectTreeWidget* tree = panel_->treeWidget();

    ASSERT_TRUE(panel_->selectProjectItem(subLayer_));
    EXPECT_TRUE(tree->canDropSelectionAtBucketRoot(QStringLiteral("Layers")));
    EXPECT_FALSE(tree->canDropSelectionAtBucketRoot(QStringLiteral("Busses")));
    EXPECT_FALSE(tree->canDropSelectionAtBucketRoot(QStringLiteral("Actions")));

    ASSERT_TRUE(panel_->selectProjectItem(sound_));
    EXPECT_TRUE(tree->canDropSelectionAtBucketRoot(QStringLiteral("Layers")));
    EXPECT_FALSE(tree->canDropSelectionAtBucketRoot(QStringLiteral("Actions")));

    ASSERT_TRUE(panel_->selectProjectItem(container_));
    EXPECT_TRUE(tree->canDropSelectionAtBucketRoot(QStringLiteral("Layers")));
    ASSERT_TRUE(panel_->selectProjectItem(track_));
    EXPECT_TRUE(tree->canDropSelectionAtBucketRoot(QStringLiteral("Layers")));
    ASSERT_TRUE(panel_->selectProjectItem(piece_));
    EXPECT_TRUE(tree->canDropSelectionAtBucketRoot(QStringLiteral("Layers")));

    ASSERT_TRUE(panel_->selectProjectItem(subLayer_));
    QTreeWidgetItem* soundRow = findRow(panel_->treeWidget(), QStringLiteral("Drag_Shot"));
    ASSERT_NE(soundRow, nullptr);
    soundRow->setSelected(true);
    ASSERT_EQ(panel_->selectedItems().size(), 2);
    EXPECT_TRUE(tree->canDropSelectionAtBucketRoot(QStringLiteral("Layers")));

    ASSERT_TRUE(panel_->selectProjectItem(action_));
    EXPECT_FALSE(tree->canDropSelectionAtBucketRoot(QStringLiteral("Actions")));
    EXPECT_FALSE(tree->canDropSelectionAtBucketRoot(QStringLiteral("Layers")));
}

class TreeExpansionConsistencyTest : public ::testing::TestWithParam<int>
{
protected:
    void SetUp() override
    {
        ensureApp();
        if (GetParam() == 0) {
            panel_ = std::make_unique<Hierarchy>();
            tree_ = panel_->treeWidget();
        } else {
            SelectorRequest request;
            if (GetParam() == 2)
                request.onToggled = [](uint32_t, bool) {};
            auto* selector = SelectorWindow::open(request);
            tree_ = selector->findChild<QTreeWidget*>(QStringLiteral("SelectorTreeWidget"));
        }
        ASSERT_NE(tree_, nullptr);
        tree_->clear();
    }

    void TearDown() override
    {
        SelectorWindow::dismiss();
    }

    QObject connectionContext_;
    std::unique_ptr<Hierarchy> panel_;
    QTreeWidget* tree_ = nullptr;
};

INSTANTIATE_TEST_SUITE_P(ProjectAndSelectors, TreeExpansionConsistencyTest,
                        ::testing::Values(0, 1, 2));

TEST_P(TreeExpansionConsistencyTest, ChevronDoubleClickPerformsTwoToggles)
{
    ensureApp();

    QTreeWidget& tree = *tree_;
    tree.setExpandsOnDoubleClick(false);
    tree.setRootIsDecorated(false);
    tree.setIndentation(20);
    tree.resize(320, 200);

    auto* bucket = new QTreeWidgetItem();
    bucket->setData(0, TreeRowStyle::kBucketRole, true);
    bucket->setText(0, QStringLiteral("Layers"));
    tree.addTopLevelItem(bucket);

    auto* object = new QTreeWidgetItem(bucket);
    object->setText(0, QStringLiteral("Layer"));
    new QTreeWidgetItem(object);

    tree.show();
    QApplication::processEvents();
    bucket->setExpanded(true);

    int expandedCount = 0;
    int collapsedCount = 0;
    int doubleClickCount = 0;
    QObject::connect(&tree, &QTreeWidget::itemExpanded, &connectionContext_,
                     [object, &expandedCount](QTreeWidgetItem* item) {
                         if (item == object) ++expandedCount;
                     });
    QObject::connect(&tree, &QTreeWidget::itemCollapsed, &connectionContext_,
                     [object, &collapsedCount](QTreeWidgetItem* item) {
                         if (item == object) ++collapsedCount;
                     });
    QObject::connect(&tree, &QTreeWidget::itemDoubleClicked, &connectionContext_,
                     [object, &doubleClickCount](QTreeWidgetItem* item, int) {
                         if (item == object) ++doubleClickCount;
                     });

    const QRect row = tree.visualItemRect(object);
    const QPoint chevron(row.left() - tree.indentation() / 2, row.center().y());
    sendDoubleClick(tree.viewport(), chevron);

    EXPECT_FALSE(object->isExpanded());
    EXPECT_EQ(expandedCount, 1);
    EXPECT_EQ(collapsedCount, 1);
    EXPECT_EQ(doubleClickCount, 0);
}

TEST_P(TreeExpansionConsistencyTest, PendingChevronClickDoesNotSurviveRowRemoval)
{
    tree_->setIndentation(20);
    tree_->resize(320, 200);
    tree_->show();
    QApplication::processEvents();

    for (bool resetModel : {false, true}) {
        SCOPED_TRACE(resetModel ? "Model reset" : "Row removal");
        tree_->clear();
        auto* bucket = new QTreeWidgetItem(tree_);
        bucket->setData(0, TreeRowStyle::kBucketRole, true);
        auto* object = new QTreeWidgetItem(bucket);
        new QTreeWidgetItem(object);
        bucket->setExpanded(true);

        const QRect row = tree_->visualItemRect(object);
        const QPoint chevron(row.left() - tree_->indentation() / 2, row.center().y());
        const auto sendMouse = [&](QEvent::Type type, Qt::MouseButtons buttons) {
            QMouseEvent event(type, chevron, tree_->viewport()->mapToGlobal(chevron),
                              Qt::LeftButton, buttons, Qt::NoModifier);
            QApplication::sendEvent(tree_->viewport(), &event);
        };
        sendMouse(QEvent::MouseButtonPress, Qt::LeftButton);
        EXPECT_FALSE(object->isExpanded());

        {
            const QSignalBlocker blocker(tree_);
            ASSERT_EQ(bucket->takeChild(0), object);
            if (resetModel) {
                tree_->clear();
                bucket = new QTreeWidgetItem(tree_);
                bucket->setData(0, TreeRowStyle::kBucketRole, true);
            }
            bucket->addChild(object);
            bucket->setExpanded(true);
        }
        ASSERT_EQ(tree_->itemAt(chevron), object);
        sendMouse(QEvent::MouseButtonRelease, Qt::NoButton);
        EXPECT_FALSE(object->isExpanded());

        sendMouse(QEvent::MouseButtonPress, Qt::LeftButton);
        sendMouse(QEvent::MouseButtonRelease, Qt::NoButton);
        EXPECT_TRUE(object->isExpanded());
    }
}

TEST_P(TreeExpansionConsistencyTest, ObjectLabelDoubleClickStillEmitsDoubleClick)
{
    ensureApp();

    QTreeWidget& tree = *tree_;
    tree.setExpandsOnDoubleClick(false);
    tree.setRootIsDecorated(false);
    tree.setIndentation(20);
    tree.resize(320, 200);

    auto* bucket = new QTreeWidgetItem();
    bucket->setData(0, TreeRowStyle::kBucketRole, true);
    bucket->setText(0, QStringLiteral("Layers"));
    tree.addTopLevelItem(bucket);
    auto* object = new QTreeWidgetItem(bucket);
    object->setText(0, QStringLiteral("Layer"));
    new QTreeWidgetItem(object);

    tree.show();
    QApplication::processEvents();
    bucket->setExpanded(true);

    int doubleClickCount = 0;
    int expandedCount = 0;
    int collapsedCount = 0;
    QObject::connect(&tree, &QTreeWidget::itemDoubleClicked, &connectionContext_,
                     [object, &doubleClickCount](QTreeWidgetItem* item, int) {
                         if (item == object) ++doubleClickCount;
                     });
    QObject::connect(&tree, &QTreeWidget::itemExpanded, &connectionContext_,
                     [object, &expandedCount](QTreeWidgetItem* item) {
                         if (item == object) ++expandedCount;
                     });
    QObject::connect(&tree, &QTreeWidget::itemCollapsed, &connectionContext_,
                     [object, &collapsedCount](QTreeWidgetItem* item) {
                         if (item == object) ++collapsedCount;
                     });

    const QRect row = tree.visualItemRect(object);
    const QPoint label(row.left() + tree.indentation() + 40, row.center().y());
    sendDoubleClick(tree.viewport(), label);

    EXPECT_EQ(doubleClickCount, 1);
    EXPECT_EQ(expandedCount, 0);
    EXPECT_EQ(collapsedCount, 0);
}

TEST_P(TreeExpansionConsistencyTest, BucketLabelDoubleClickPerformsTwoToggles)
{
    ensureApp();

    auto* tree = tree_;
    tree->resize(320, 200);

    auto* bucket = new QTreeWidgetItem();
    bucket->setData(0, TreeRowStyle::kBucketRole, true);
    bucket->setText(0, QStringLiteral("Layers"));
    if (GetParam() != 0)
        bucket->setFlags(bucket->flags() & ~Qt::ItemIsSelectable);
    tree->addTopLevelItem(bucket);
    new QTreeWidgetItem(bucket);

    tree->show();
    QApplication::processEvents();
    bucket->setExpanded(false);

    int expandedCount = 0;
    int collapsedCount = 0;
    QObject::connect(tree, &QTreeWidget::itemExpanded, &connectionContext_,
                     [bucket, &expandedCount](QTreeWidgetItem* item) {
                         if (item == bucket) ++expandedCount;
                     });
    QObject::connect(tree, &QTreeWidget::itemCollapsed, &connectionContext_,
                     [bucket, &collapsedCount](QTreeWidgetItem* item) {
                         if (item == bucket) ++collapsedCount;
                     });

    const QRect row = tree->visualItemRect(bucket);
    const QPoint label(row.left() + 40, row.center().y());
    sendDoubleClick(tree->viewport(), label);

    EXPECT_FALSE(bucket->isExpanded());
    EXPECT_EQ(expandedCount, 1);
    EXPECT_EQ(collapsedCount, 1);

    const QPoint chevron(tree->viewport()->width() - 8, row.center().y());
    sendDoubleClick(tree->viewport(), chevron);

    EXPECT_FALSE(bucket->isExpanded());
    EXPECT_EQ(expandedCount, 2);
    EXPECT_EQ(collapsedCount, 2);
}

TEST_F(ProjectTreeDragDropTest, ObjectChevronDoubleClickInHierarchyPerformsTwoToggles)
{
    ProjectTreeWidget* tree = panel_->treeWidget();
    QTreeWidgetItem* object = findRow(tree, QStringLiteral("Drag_Sub"));
    ASSERT_NE(object, nullptr);

    for (QTreeWidgetItem* parent = object->parent(); parent; parent = parent->parent())
        parent->setExpanded(true);
    object->setExpanded(false);
    tree->show();
    QApplication::processEvents();

    int expandedCount = 0;
    int collapsedCount = 0;
    int doubleClickCount = 0;
    QObject::connect(tree, &QTreeWidget::itemExpanded,
                     [object, &expandedCount](QTreeWidgetItem* item) {
                         if (item == object) ++expandedCount;
                     });
    QObject::connect(tree, &QTreeWidget::itemCollapsed,
                     [object, &collapsedCount](QTreeWidgetItem* item) {
                         if (item == object) ++collapsedCount;
                     });
    QObject::connect(panel_.get(), &ProjectTreePanel::itemDoubleClicked,
                     [subLayer = subLayer_, &doubleClickCount](ProjectItem* item) {
                         if (item == subLayer) ++doubleClickCount;
                     });

    const QRect row = tree->visualItemRect(object);
    const QPoint chevron(row.left() - tree->indentation() / 2, row.center().y());
    sendDoubleClick(tree->viewport(), chevron);

    EXPECT_FALSE(object->isExpanded());
    EXPECT_EQ(expandedCount, 1);
    EXPECT_EQ(collapsedCount, 1);
    EXPECT_EQ(doubleClickCount, 0);
}

TEST_F(ProjectTreeDragDropTest, ObjectLabelDoubleClickKeepsProjectAction)
{
    ProjectTreeWidget* tree = panel_->treeWidget();
    QTreeWidgetItem* object = findRow(tree, QStringLiteral("Drag_Sub"));
    ASSERT_NE(object, nullptr);

    for (QTreeWidgetItem* parent = object->parent(); parent; parent = parent->parent())
        parent->setExpanded(true);
    tree->show();
    QApplication::processEvents();

    int doubleClickCount = 0;
    QObject::connect(panel_.get(), &ProjectTreePanel::itemDoubleClicked,
                     [subLayer = subLayer_, &doubleClickCount](ProjectItem* item) {
                         if (item == subLayer) ++doubleClickCount;
                     });

    const QRect row = tree->visualItemRect(object);
    const QPoint label(row.left() + 40, row.center().y());
    sendDoubleClick(tree->viewport(), label);

    EXPECT_EQ(doubleClickCount, 1);
}

TEST_F(ProjectTreeDragDropTest, MoveItemToBucketRootRoundTripsThroughUndo)
{
    auto& pm = ProjectManager::instance();
    ProjectData* project = pm.currentProject();
    ASSERT_NE(project, nullptr);

    auto layerBindingFor = [project](const ProjectItem* item) -> const ProjectFileBinding* {
        for (const auto& binding : project->layers) {
            if (binding.rootItem.get() == item) return &binding;
        }
        return nullptr;
    };

    ASSERT_TRUE(pm.moveItemToBucketRoot(sound_, QStringLiteral("Layers")));
    EXPECT_EQ(sound_->parent, nullptr);
    EXPECT_EQ(sound_->parentRef.kind, ParentRef::Kind::Root);
    ASSERT_NE(layerBindingFor(sound_), nullptr);
    EXPECT_EQ(layerBindingFor(sound_)->parentFolder, QStringLiteral("Layers"));

    QUndoStack* soundStack = UndoRedoManager::instance().undoStack();
    soundStack->undo();
    EXPECT_EQ(sound_->parent, layerRoot_);
    EXPECT_EQ(layerBindingFor(sound_), nullptr);

    ASSERT_EQ(layerRoot_->children.front().get(), subLayer_);
    ASSERT_TRUE(pm.moveItemToBucketRoot(subLayer_, QStringLiteral("Layers")));
    EXPECT_EQ(subLayer_->parent, nullptr);
    EXPECT_EQ(subLayer_->parentRef.kind, ParentRef::Kind::Root);
    ASSERT_NE(layerBindingFor(subLayer_), nullptr);
    EXPECT_EQ(layerBindingFor(subLayer_)->parentFolder, QStringLiteral("Layers"));
    EXPECT_EQ(subSubLayer_->parent, subLayer_);

    QUndoStack* stack = UndoRedoManager::instance().undoStack();
    stack->undo();
    EXPECT_EQ(subLayer_->parent, layerRoot_);
    ASSERT_FALSE(layerRoot_->children.empty());
    EXPECT_EQ(layerRoot_->children.front().get(), subLayer_);
    EXPECT_EQ(layerBindingFor(subLayer_), nullptr);

    stack->redo();
    EXPECT_EQ(subLayer_->parent, nullptr);
    EXPECT_EQ(subLayer_->parentRef.kind, ParentRef::Kind::Root);
    ASSERT_NE(layerBindingFor(subLayer_), nullptr);

    ASSERT_NE(pm.createFolder(nullptr, QStringLiteral("Origin"), QStringLiteral("Layers")), nullptr);
    ASSERT_TRUE(pm.moveEntriesToFolder({layerAlt_}, {}, QStringLiteral("Layers"), QStringLiteral("Origin")));
    EXPECT_EQ(project->itemFolderPath(layerAlt_), QStringLiteral("Origin"));
    ASSERT_NE(layerBindingFor(layerAlt_), nullptr);
    const int beforeParentMove = stack->index();
    ASSERT_TRUE(pm.moveItem(layerAlt_, layerRoot_));
    EXPECT_EQ(layerAlt_->parent, layerRoot_);
    EXPECT_TRUE(project->itemFolderPath(layerAlt_).isEmpty());
    EXPECT_EQ(layerBindingFor(layerAlt_), nullptr);
    EXPECT_EQ(layerAlt_->parentRef.kind, ParentRef::Kind::Item);
    EXPECT_EQ(layerAlt_->parentRef.guid, layerRoot_->guid);
    EXPECT_EQ(stack->index(), beforeParentMove + 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(layerAlt_->parent, nullptr);
    EXPECT_EQ(project->itemFolderPath(layerAlt_), QStringLiteral("Origin"));
    EXPECT_EQ(layerAlt_->parentRef.kind, ParentRef::Kind::Root);
    ASSERT_NE(layerBindingFor(layerAlt_), nullptr);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(layerAlt_->parent, layerRoot_);
    EXPECT_TRUE(project->itemFolderPath(layerAlt_).isEmpty());
    EXPECT_EQ(layerAlt_->parentRef.kind, ParentRef::Kind::Item);
    EXPECT_EQ(layerBindingFor(layerAlt_), nullptr);

    ASSERT_TRUE(pm.moveItemToBucketRoot(layerAlt_, QStringLiteral("Layers")));
    EXPECT_EQ(layerAlt_->parent, nullptr);
    EXPECT_TRUE(project->itemFolderPath(layerAlt_).isEmpty());
    EXPECT_EQ(layerAlt_->parentRef.kind, ParentRef::Kind::Root);
    ASSERT_NE(layerBindingFor(layerAlt_), nullptr);

    stack->undo();
    EXPECT_EQ(layerAlt_->parent, layerRoot_);
    EXPECT_TRUE(project->itemFolderPath(layerAlt_).isEmpty());
    EXPECT_EQ(layerAlt_->parentRef.kind, ParentRef::Kind::Item);
    EXPECT_EQ(layerBindingFor(layerAlt_), nullptr);

    stack->redo();
    EXPECT_EQ(layerAlt_->parent, nullptr);
    EXPECT_TRUE(project->itemFolderPath(layerAlt_).isEmpty());
    EXPECT_EQ(layerAlt_->parentRef.kind, ParentRef::Kind::Root);
    ASSERT_NE(layerBindingFor(layerAlt_), nullptr);
}
