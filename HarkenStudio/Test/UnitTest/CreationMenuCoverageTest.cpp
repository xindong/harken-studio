#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "Components/VSelection.h"
#include "UndoCommands.h"
#include "Widgets/Hierarchy/Hierarchy.h"
#include "Widgets/Shared/ProjectItemMenuController.h"

#include "Support/AITestSupport.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QMenu>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>

using namespace VNX;

namespace {

class NewParentProbeHierarchy : public Hierarchy
{
public:
    using ProjectTreePanel::prepareItemContextMenu;

    void armMenuContext(ProjectItem* item)
    {
        prepareItemContextMenu(treeItemForProjectItem(item), item);
    }

    QMenu* itemContextMenu() const { return contextMenu_; }
};

class NewActionEntryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TestSupport::ensureApp();
        ASSERT_TRUE(tempDir_.isValid());
        panel_ = std::make_unique<NewParentProbeHierarchy>();
        ProjectConfig config;
        config.name = QStringLiteral("NewActionEntryProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(ProjectManager::instance().createProject(config));
        auto& pm = ProjectManager::instance();
        layer_ = pm.createItem(nullptr, QStringLiteral("NA_Layer"), ProjectItem::Type::Layer);
        ASSERT_NE(layer_, nullptr);
        sound_ = pm.createItem(layer_, QStringLiteral("NA_Sound"), ProjectItem::Type::Sound);
        ASSERT_NE(sound_, nullptr);
    }

    void TearDown() override
    {
        panel_.reset();
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
    }

    QAction* newActionAction(ProjectItem* item)
    {
        panel_->armMenuContext(item);
        for (QAction* action : panel_->itemContextMenu()->actions())
            if (action->text() == QStringLiteral("New Action"))
                return action;
        return nullptr;
    }

    QTemporaryDir tempDir_;
    std::unique_ptr<NewParentProbeHierarchy> panel_;
    ProjectItem* layer_ = nullptr;
    ProjectItem* sound_ = nullptr;
};

class BusMenuSpecTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TestSupport::ensureApp();
        ASSERT_TRUE(tempDir_.isValid());
        panel_ = std::make_unique<NewParentProbeHierarchy>();
        ProjectConfig config;
        config.name = QStringLiteral("BusMenuProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(ProjectManager::instance().createProject(config));
        auto& pm = ProjectManager::instance();
        master_ = pm.getMasterBusItem();
        ASSERT_NE(master_, nullptr);
        childBus_ = pm.createItem(master_, QStringLiteral("Mix_Sub"), ProjectItem::Type::Bus);
        ASSERT_NE(childBus_, nullptr);
    }

    void TearDown() override
    {
        panel_.reset();
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
    }

    QAction* findMenuAction(const QString& label)
    {
        for (QAction* action : panel_->itemContextMenu()->actions())
            if (action->text().section(QLatin1Char('\t'), 0, 0) == label)
                return action;
        return nullptr;
    }

    QTemporaryDir tempDir_;
    std::unique_ptr<NewParentProbeHierarchy> panel_;
    ProjectItem* master_ = nullptr;
    ProjectItem* childBus_ = nullptr;
};

class CreationCommandTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TestSupport::ensureApp();
        ASSERT_TRUE(tempDir_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("CreationCommandProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(ProjectManager::instance().createProject(config));
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
    }

    QTemporaryDir tempDir_;
};

}

TEST_F(CreationCommandTest, BusCreationCommandUsesMasterAndUndoRedo)
{
    auto& pm = ProjectManager::instance();
    auto* master = pm.getMasterBusItem();
    ASSERT_NE(master, nullptr);

    ProjectItemMenuController::Callbacks callbacks;
    callbacks.getSelected = []() -> ProjectItem* { return nullptr; };
    ProjectItemMenuController controller(std::move(callbacks));
    QMenu menu;
    controller.buildNewChildMenu(&menu);
    controller.updateActionVisibility(nullptr, QStringLiteral("Busses"));

    QAction* busAction = nullptr;
    for (QAction* action : controller.newChildMenu()->actions())
        if (action->text() == QStringLiteral("Bus"))
            busAction = action;
    ASSERT_NE(busAction, nullptr);
    ASSERT_TRUE(busAction->isEnabled());

    busAction->trigger();
    ASSERT_EQ(master->children.size(), 1u);
    ProjectItem* created = master->children.front().get();
    EXPECT_EQ(created->type, ProjectItem::Type::Bus);
    EXPECT_EQ(created->parent, master);

    auto* stack = UndoRedoManager::instance().undoStack();
    stack->undo();
    EXPECT_TRUE(master->children.empty());
    stack->redo();
    ASSERT_EQ(master->children.size(), 1u);
    EXPECT_EQ(master->children.front().get(), created);
}

TEST_F(CreationCommandTest, ContainerCreationModesRoundTripThroughUndoRedo)
{
    auto& pm = ProjectManager::instance();
    auto* layer = pm.createItem(nullptr, QStringLiteral("Mode_Layer"), ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);

    ProjectItemMenuController::Callbacks callbacks;
    callbacks.getSelected = [layer]() { return layer; };
    ProjectItemMenuController controller(std::move(callbacks));
    auto* stack = UndoRedoManager::instance().undoStack();
    const SelectionMode modes[] = {
        SelectionMode::Random, SelectionMode::Sequence, SelectionMode::Match, SelectionMode::Blend,
    };

    for (SelectionMode mode : modes)
    {
        const int baseIndex = stack->index();
        ASSERT_TRUE(layer->children.empty());

        ProjectItem* created = controller.createContainerWithMode(mode);
        ASSERT_NE(created, nullptr);
        ASSERT_EQ(created->type, ProjectItem::Type::Container);
        ASSERT_EQ(created->parent, layer);
        ASSERT_EQ(created->comp<VSelection>()->mode, mode);
        ASSERT_EQ(stack->index(), baseIndex + 1);

        stack->undo();
        EXPECT_TRUE(layer->children.empty());
        stack->redo();
        ASSERT_EQ(layer->children.size(), 1u);
        EXPECT_EQ(layer->children.front().get(), created);
        EXPECT_EQ(created->comp<VSelection>()->mode, mode);
        stack->undo();
    }
}

TEST_F(BusMenuSpecTest, MasterBusDeleteDisabledByPredicateNormalBusEnabled)
{
    panel_->armMenuContext(master_);
    QAction* del = findMenuAction(QStringLiteral("Delete"));
    ASSERT_NE(del, nullptr);
    EXPECT_FALSE(del->isEnabled());

    panel_->armMenuContext(childBus_);
    EXPECT_TRUE(del->isEnabled());
}

TEST_F(BusMenuSpecTest, MixedSoundAndBusSelectionDisablesClipboardAndDelete)
{
    auto* sound = ProjectManager::instance().createItem(
        nullptr, QStringLiteral("BM_Sound"), ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    panel_->setSelectedProjectItems({sound, childBus_});
    panel_->armMenuContext(sound);

    QAction* copy = findMenuAction(QStringLiteral("Copy"));
    QAction* cut = findMenuAction(QStringLiteral("Cut"));
    QAction* duplicate = findMenuAction(QStringLiteral("Duplicate"));
    ASSERT_NE(copy, nullptr);
    ASSERT_NE(cut, nullptr);
    ASSERT_NE(duplicate, nullptr);
    EXPECT_FALSE(copy->isEnabled());
    EXPECT_FALSE(cut->isEnabled());
    EXPECT_FALSE(duplicate->isEnabled());

    panel_->setSelectedProjectItems({sound, master_});
    panel_->armMenuContext(sound);
    QAction* del = findMenuAction(QStringLiteral("Delete"));
    ASSERT_NE(del, nullptr);
    EXPECT_FALSE(del->isEnabled());
}

TEST_F(NewActionEntryTest, MixedSoundAndVariableDisablesEveryNewActionStep)
{
    panel_->setSelectedProjectItems({sound_});
    panel_->armMenuContext(sound_);
    QAction* action = newActionAction(sound_);
    ASSERT_NE(action, nullptr);
    EXPECT_TRUE(action->isEnabled());

    auto* variable = ProjectManager::instance().createItem(
        nullptr, QStringLiteral("NA_Var"), ProjectItem::Type::Variable);
    ASSERT_NE(variable, nullptr);
    panel_->setSelectedProjectItems({sound_, variable});
    panel_->armMenuContext(sound_);
    action = newActionAction(sound_);
    ASSERT_NE(action, nullptr);
    EXPECT_FALSE(action->isEnabled());
    EXPECT_FALSE(newActionAction(variable)->isEnabled());
}
