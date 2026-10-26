#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "ShortcutManager.h"
#include "NameValidator.h"
#include "UndoCommands.h"
#include "Components/VAction.h"
#include "Components/VMixNode.h"
#include "Components/VSelection.h"
#include "Components/VSound.h"
#include "Components/VVariable.h"
#include "Components/VPiece.h"
#include "Widgets/Hierarchy/Hierarchy.h"
#include "Widgets/Shared/ProjectItemMenuController.h"
#include "Widgets/Shared/InlineRenameEdit.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QAction>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QColorDialog>
#include <QContextMenuEvent>
#include <QEventLoop>
#include <QDir>
#include <QFileInfo>
#include <QKeyEvent>
#include <QPointer>
#include <QMenu>
#include <QDialog>
#include <QMouseEvent>

#include <algorithm>
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

// Drives the panel's menu-context state the same way onContextMenu does
// (prepareItemContextMenu + updateActionVisibility) without exec'ing a menu.
class AnchorProbeHierarchy : public Hierarchy
{
public:
    using ProjectTreePanel::onDuplicate;
    using ProjectTreePanel::onCopy;
    using ProjectTreePanel::onPaste;
    using ProjectTreePanel::onDelete;
    using ProjectTreePanel::onNewAction;
    using ProjectTreePanel::onRename;
    using ProjectTreePanel::onPasteProperties;
    using ProjectTreePanel::setItemColorViaDialog;
    using ProjectTreePanel::treeItemForProjectItem;
    using ProjectTreePanel::prepareItemContextMenu;

    void armMenuContext(ProjectItem* item)
    {
        prepareItemContextMenu(treeItemForProjectItem(item), item);
    }

    void disarmMenuContext()
    {
        menuController_->clearMenuContext();
    }

    QAction* menuAction(const QString& label) const
    {
        for (QAction* action : contextMenu_->actions()) {
            if (action->text().section(QLatin1Char('\t'), 0, 0) == label)
                return action;
        }
        return nullptr;
    }

    QMenu* menu() const { return contextMenu_; }

    ProjectItem* createItem(ProjectItem::Type type)
    {
        menuController_->clearMenuContext();
        return menuController_->createByType(type);
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

    void deleteWithoutDialog()
    {
        int prompts = 0;
        QTimer dismiss;
        QObject::connect(&dismiss, &QTimer::timeout, [&] {
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                ++prompts;
                dialog->reject();
            }
        });
        dismiss.start(0);
        panel_->onDelete();
        dismiss.stop();
        EXPECT_EQ(prompts, 0);
    }

    QTemporaryDir tempDir_;
    std::unique_ptr<AnchorProbeHierarchy> panel_;
    ProjectItem* contextLayer_ = nullptr;
    ProjectItem* selectedLayer_ = nullptr;
};

}

TEST_F(ProjectTreePanelAnchorTest, DuplicateExecutesOnContextRowNotSelection)
{
    panel_->armMenuContext(contextLayer_);
    EXPECT_EQ(panel_->selectedItem(), contextLayer_);
    panel_->onDuplicate();

    ProjectItem* created = panel_->selectedItem();
    ASSERT_NE(created, nullptr);
    EXPECT_NE(created, contextLayer_);
    EXPECT_NE(created, selectedLayer_);
    EXPECT_TRUE(created->name.startsWith(QStringLiteral("Ctx_Layer")));
}

TEST_F(ProjectTreePanelAnchorTest, DuplicateIgnoresFolder)
{
    auto& pm = ProjectManager::instance();
    auto* folder = pm.createFolder(nullptr, QStringLiteral("Folder"), QStringLiteral("Actions"));
    ASSERT_NE(folder, nullptr);
    ASSERT_TRUE(panel_->selectFolder(folder));

    panel_->onDuplicate();

    EXPECT_EQ(panel_->selectedFolder(), folder);
    EXPECT_EQ(pm.currentProject()->folders.size(), 1);
}

TEST_F(ProjectTreePanelAnchorTest, TreeShortcutAndMenuHintsUseManagerBinding)
{
    ShortcutManager& manager = ShortcutManager::instance();
    const std::pair<QString, QString> commands[] = {
        {Shortcuts::Cut, QStringLiteral("Cut")},
        {Shortcuts::Copy, QStringLiteral("Copy")},
        {Shortcuts::Paste, QStringLiteral("Paste")},
        {Shortcuts::Delete, QStringLiteral("Delete")},
        {Shortcuts::Duplicate, QStringLiteral("Duplicate")},
    };

    QAction* duplicateTreeAction = nullptr;
    for (const auto& [id, label] : commands) {
        QAction* treeAction = nullptr;
        int panelBindings = 0;
        for (QAction* action : manager.info(id).actions) {
            if (!action)
                continue;
            for (QObject* owner = action->parent(); owner; owner = owner->parent()) {
                if (owner == panel_.get()) {
                    ++panelBindings;
                    if (action->parent() == panel_->treeWidget())
                        treeAction = action;
                    break;
                }
            }
        }

        ASSERT_NE(treeAction, nullptr);
        EXPECT_EQ(panelBindings, 1);
        EXPECT_EQ(treeAction->shortcutContext(), Qt::WidgetWithChildrenShortcut);
        EXPECT_EQ(treeAction->shortcut(), manager.shortcut(id));

        QAction* menuAction = panel_->menuAction(label);
        ASSERT_NE(menuAction, nullptr);
        EXPECT_TRUE(menuAction->shortcut().isEmpty());
        EXPECT_EQ(menuAction->text(),
                  label + QLatin1Char('\t')
                      + manager.shortcut(id).toString(QKeySequence::NativeText));

        if (id == Shortcuts::Duplicate)
            duplicateTreeAction = treeAction;
    }

    const QKeySequence original = manager.shortcut(Shortcuts::Duplicate);
    const QKeySequence replacement(QStringLiteral("Ctrl+Alt+Shift+D"));
    manager.setShortcut(Shortcuts::Duplicate, replacement);
    EXPECT_EQ(duplicateTreeAction->shortcut(), replacement);
    QAction* duplicateMenuAction = panel_->menuAction(QStringLiteral("Duplicate"));
    ASSERT_NE(duplicateMenuAction, nullptr);
    EXPECT_TRUE(duplicateMenuAction->shortcut().isEmpty());
    EXPECT_EQ(duplicateMenuAction->text(),
              QStringLiteral("Duplicate\t")
                  + replacement.toString(QKeySequence::NativeText));
    manager.setShortcut(Shortcuts::Duplicate, original);

    panel_->disarmMenuContext();
    duplicateTreeAction->trigger();
    ProjectItem* created = panel_->selectedItem();
    ASSERT_NE(created, nullptr);
    EXPECT_NE(created, selectedLayer_);
    EXPECT_TRUE(created->name.startsWith(QStringLiteral("Sel_Layer")));
}

TEST_F(ProjectTreePanelAnchorTest, CopyExecutesOnContextRowWhenOutsideSelection)
{
    panel_->armMenuContext(contextLayer_);
    EXPECT_EQ(panel_->selectedItems().size(), 1);
    EXPECT_EQ(panel_->selectedItem(), contextLayer_);
    panel_->onCopy();

    const auto sources =
        ProjectManager::instance().resolveClipboardSources();
    ASSERT_EQ(sources.size(), 1u);
    EXPECT_EQ(sources.front()->guid, contextLayer_->guid);
}

TEST_F(ProjectTreePanelAnchorTest, CopyKeepsWholeSelectionWhenContextRowInsideIt)
{
    QTreeWidgetItem* ctxRow = findRow(panel_->treeWidget(), QStringLiteral("Ctx_Layer"));
    ASSERT_NE(ctxRow, nullptr);
    ctxRow->setSelected(true);
    ASSERT_EQ(panel_->selectedItems().size(), 2);

    panel_->armMenuContext(contextLayer_);
    EXPECT_EQ(panel_->selectedItems().size(), 2);
    panel_->onCopy();

    const auto sources =
        ProjectManager::instance().resolveClipboardSources();
    ASSERT_EQ(sources.size(), 2u);
    EXPECT_EQ((QSet<QUuid>{sources[0]->guid, sources[1]->guid}),
              (QSet<QUuid>{contextLayer_->guid, selectedLayer_->guid}));
}

TEST_F(ProjectTreePanelAnchorTest, ReplaceSourceEntryIsGoneFromObjectMenu)
{
    panel_->armMenuContext(contextLayer_);
    EXPECT_EQ(panel_->menuAction(QStringLiteral("Replace Source File...")), nullptr);
}

TEST_F(ProjectTreePanelAnchorTest, ReferencedVariableDeletesSilentlyAndUndoRestoresItsConsumers)
{
    auto& pm = ProjectManager::instance();
    ProjectItem* variable = pm.createItem(
        nullptr, QStringLiteral("DeleteDrive"), ProjectItem::Type::Variable);
    ASSERT_NE(variable, nullptr);
    variable->compOrCreate<VVariable>();
    ProjectItem* firstGrid = pm.createItem(
        nullptr, QStringLiteral("DeleteGridA"), ProjectItem::Type::Container);
    ProjectItem* secondGrid = pm.createItem(
        nullptr, QStringLiteral("DeleteGridB"), ProjectItem::Type::Container);
    ASSERT_NE(firstGrid, nullptr);
    ASSERT_NE(secondGrid, nullptr);
    firstGrid->comp<VSelection>()->mode = SelectionMode::Match;
    secondGrid->comp<VSelection>()->mode = SelectionMode::Blend;
    firstGrid->compOrCreate<VSelection>()->selections.shared.matchAxes.push_back(variable->guid);
    secondGrid->compOrCreate<VSelection>()->selections.shared.mixingRows.push_back({QUuid::createUuid(), variable->guid});
    const auto matchAxes = firstGrid->comp<VSelection>()->selections.shared.matchAxes;
    const auto blendAxes = secondGrid->comp<VSelection>()->selections.shared.mixingRows;
    const QUuid variableGuid = variable->guid;
    ASSERT_TRUE(pm.saveProject());
    panel_->armMenuContext(variable);
    deleteWithoutDialog();
    EXPECT_EQ(pm.findItemByGuid(variableGuid), nullptr);
    EXPECT_TRUE(firstGrid->comp<VSelection>()->selections.shared.matchAxes.empty());
    ASSERT_EQ(secondGrid->comp<VSelection>()->selections.shared.mixingRows.size(), 1u);
    EXPECT_TRUE(secondGrid->comp<VSelection>()->selections.shared.mixingRows.front().variable.isNull());
    for (auto* item : {firstGrid, secondGrid}) {
        const auto* row = panel_->treeItemForProjectItem(item);
        ASSERT_NE(row, nullptr);
        EXPECT_NE(row->text(0) + (row->font(0).italic() ? QStringLiteral("\nitalic") : QString()), item->name);
    }
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(pm.findItemByGuid(variableGuid), variable);
    EXPECT_EQ(firstGrid->comp<VSelection>()->selections.shared.matchAxes, matchAxes);
    EXPECT_EQ(secondGrid->comp<VSelection>()->selections.shared.mixingRows, blendAxes);
}


TEST_F(ProjectTreePanelAnchorTest, DuplicateClonesSubtreeComponentsAndFieldsWithSingleUndoStep)
{
    auto& pm = ProjectManager::instance();
    ProjectItem* container = pm.createItem(contextLayer_, QStringLiteral("Dup_Container"),
                                           ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    VSelection* mode = container->comp<VSelection>();
    ASSERT_NE(mode, nullptr);
    mode->mode = SelectionMode::Sequence;
    mode->playType = SelectionPlayType::Queue;

    ProjectItem* sound = pm.createItem(container, QStringLiteral("Dup_Sound"),
                                       ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    sound->color = 0xFFAA5511u;
    auto* source = sound->comp<VSound>();
    ASSERT_NE(source, nullptr);
    source->sources.plain.entries.push_back({QUuid::createUuid(), 77});
    source->sources.plain.active = source->sources.plain.entries.front().entryId;
    VMixNode* mix = sound->comp<VMixNode>();
    ASSERT_NE(mix, nullptr);
    mix->volume = -7.5f;
    mix->pitch = 3.0f;
    mix->makeUpGain = 2.25f;
    sound->playablePropertyPresence.Set(
        harkenplayableproperties::PlayablePropertyIdV1::Volume);
    sound->playablePropertyPresence.Set(
        harkenplayableproperties::PlayablePropertyIdV1::Pitch);
    sound->playablePropertyPresence.Set(
        harkenplayableproperties::PlayablePropertyIdV1::MakeUpGain);

    QUndoStack* stack = UndoRedoManager::instance().undoStack();
    const int baseIndex = stack->index();
    ASSERT_EQ(contextLayer_->children.size(), 1u);

    panel_->armMenuContext(container);
    panel_->onDuplicate();

    ProjectItem* created = panel_->selectedItem();
    ASSERT_NE(created, nullptr);
    EXPECT_NE(created, container);
    EXPECT_EQ(created->type, ProjectItem::Type::Container);
    EXPECT_EQ(created->parent, contextLayer_);
    EXPECT_TRUE(created->name.startsWith(QStringLiteral("Dup_Container")));
    EXPECT_NE(created->name, container->name);
    EXPECT_NE(created->guid, container->guid);
    EXPECT_NE(created->objectID, container->objectID);
    EXPECT_EQ(stack->index(), baseIndex + 1);

    const VSelection* createdMode = created->comp<VSelection>();
    ASSERT_NE(createdMode, nullptr);
    EXPECT_EQ(createdMode->mode, SelectionMode::Sequence);
    EXPECT_EQ(createdMode->playType, SelectionPlayType::Queue);

    auto expectChildFidelity = [&](const ProjectItem* clone) {
        ASSERT_EQ(clone->children.size(), 1u);
        const ProjectItem* child = clone->children.front().get();
        EXPECT_EQ(child->type, ProjectItem::Type::Sound);
        EXPECT_TRUE(child->name.startsWith(QStringLiteral("Dup_Sound")));
        EXPECT_EQ(child->name, sound->name);
        EXPECT_NE(child->guid, sound->guid);
        EXPECT_EQ(child->color, 0xFFAA5511u);
        const auto* childSource = child->comp<VSound>();
        ASSERT_NE(childSource, nullptr);
        ASSERT_EQ(childSource->sources.plain.entries.size(), 1u);
        const auto& entry = childSource->sources.plain.entries.front();
        EXPECT_EQ(entry.sourceId, 77u);
        EXPECT_FALSE(entry.entryId.isNull());
        EXPECT_NE(entry.entryId, source->sources.plain.entries.front().entryId);
        EXPECT_EQ(childSource->sources.plain.active, entry.entryId);
        const VMixNode* childMix = child->comp<VMixNode>();
        ASSERT_NE(childMix, nullptr);
        EXPECT_FLOAT_EQ(childMix->volume, -7.5f);
        EXPECT_FLOAT_EQ(childMix->pitch, 3.0f);
        EXPECT_FLOAT_EQ(childMix->makeUpGain, 2.25f);
    };
    expectChildFidelity(created);

    stack->undo();
    ASSERT_EQ(contextLayer_->children.size(), 1u);
    EXPECT_EQ(contextLayer_->children.front().get(), container);

    stack->redo();
    ASSERT_EQ(contextLayer_->children.size(), 2u);
    EXPECT_EQ(contextLayer_->children.back().get(), created);
    ASSERT_NE(created->comp<VSelection>(), nullptr);
    EXPECT_EQ(created->comp<VSelection>()->mode, SelectionMode::Sequence);
    EXPECT_EQ(created->comp<VSelection>()->playType, SelectionPlayType::Queue);
    expectChildFidelity(created);
}

TEST_F(ProjectTreePanelAnchorTest, HandlersFallBackToSelectionWithoutMenuContext)
{
    panel_->armMenuContext(contextLayer_);
    panel_->disarmMenuContext();
    ASSERT_TRUE(panel_->selectProjectItem(selectedLayer_));
    panel_->onCopy();

    const auto sources =
        ProjectManager::instance().resolveClipboardSources();
    ASSERT_EQ(sources.size(), 1u);
    EXPECT_EQ(sources.front()->guid, selectedLayer_->guid);

    panel_->onDuplicate();
    ProjectItem* created = panel_->selectedItem();
    ASSERT_NE(created, nullptr);
    EXPECT_NE(created, selectedLayer_);
    EXPECT_TRUE(created->name.startsWith(QStringLiteral("Sel_Layer")));
}

TEST_F(ProjectTreePanelAnchorTest, HittingEitherSelectedRowKeepsTheSameCopyTargets)
{
    QTreeWidgetItem* ctxRow = findRow(panel_->treeWidget(), QStringLiteral("Ctx_Layer"));
    ASSERT_NE(ctxRow, nullptr);
    ctxRow->setSelected(true);
    ASSERT_EQ(panel_->selectedItems().size(), 2);

    panel_->armMenuContext(contextLayer_);
    panel_->onCopy();
    const auto fromContext =
        ProjectManager::instance().resolveClipboardSources();
    ASSERT_EQ(fromContext.size(), 2u);

    panel_->armMenuContext(selectedLayer_);
    panel_->onCopy();
    const auto fromSelected =
        ProjectManager::instance().resolveClipboardSources();
    ASSERT_EQ(fromSelected.size(), 2u);
    EXPECT_EQ((QSet<QUuid>{fromSelected[0]->guid, fromSelected[1]->guid}),
              (QSet<QUuid>{contextLayer_->guid, selectedLayer_->guid}));
}

TEST_F(ProjectTreePanelAnchorTest, ParentAndChildCopyStoresOnlyTheAncestor)
{
    auto& pm = ProjectManager::instance();
    ProjectItem* sound = pm.createItem(contextLayer_, QStringLiteral("Covered_Sound"),
                                       ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    ASSERT_TRUE(panel_->selectProjectItem(contextLayer_));
    QTreeWidgetItem* soundRow = findRow(panel_->treeWidget(), QStringLiteral("Covered_Sound"));
    ASSERT_NE(soundRow, nullptr);
    soundRow->setSelected(true);
    ASSERT_EQ(panel_->selectedItems().size(), 2);

    panel_->armMenuContext(sound);
    panel_->onCopy();

    const auto sources = pm.resolveClipboardSources();
    ASSERT_EQ(sources.size(), 1u);
    EXPECT_EQ(sources.front()->guid, contextLayer_->guid);
}

TEST_F(ProjectTreePanelAnchorTest, DuplicateSelectsEveryCloneRootAndUndoesOnce)
{
    auto& pm = ProjectManager::instance();
    ProjectItem* first = pm.createItem(nullptr, QStringLiteral("DupA_Layer"),
                                       ProjectItem::Type::Layer);
    ProjectItem* second = pm.createItem(nullptr, QStringLiteral("DupB_Layer"),
                                        ProjectItem::Type::Layer);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    panel_->setSelectedProjectItems({first, second});
    ASSERT_EQ(panel_->selectedItems().size(), 2);

    QUndoStack* stack = UndoRedoManager::instance().undoStack();
    const int baseIndex = stack->index();
    panel_->armMenuContext(first);
    panel_->onDuplicate();

    const QList<ProjectItem*> selected = panel_->selectedItems();
    ASSERT_EQ(selected.size(), 2);
    EXPECT_EQ(stack->index(), baseIndex + 1);
    QSet<QUuid> cloneGuids;
    for (ProjectItem* item : selected) {
        ASSERT_NE(item, first);
        ASSERT_NE(item, second);
        EXPECT_TRUE(item->name.startsWith(QStringLiteral("DupA_Layer"))
                    || item->name.startsWith(QStringLiteral("DupB_Layer")));
        cloneGuids.insert(item->guid);
    }
    EXPECT_EQ(cloneGuids.size(), 2);

    stack->undo();
    for (const QUuid& guid : cloneGuids)
        EXPECT_EQ(pm.findItemByGuid(guid), nullptr);

    stack->redo();
    for (const QUuid& guid : cloneGuids)
        EXPECT_NE(pm.findItemByGuid(guid), nullptr);
}

TEST_F(ProjectTreePanelAnchorTest, ParentAndChildDuplicateKeepsOneCloneRoot)
{
    auto& pm = ProjectManager::instance();
    ProjectItem* sound = pm.createItem(contextLayer_, QStringLiteral("DupCovered_Sound"),
                                       ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    ASSERT_TRUE(panel_->selectProjectItem(contextLayer_));
    QTreeWidgetItem* soundRow = findRow(panel_->treeWidget(), QStringLiteral("DupCovered_Sound"));
    ASSERT_NE(soundRow, nullptr);
    soundRow->setSelected(true);
    ASSERT_EQ(panel_->selectedItems().size(), 2);

    QUndoStack* stack = UndoRedoManager::instance().undoStack();
    const int baseIndex = stack->index();
    panel_->armMenuContext(sound);
    panel_->onDuplicate();

    ASSERT_EQ(panel_->selectedItems().size(), 1);
    ProjectItem* clone = panel_->selectedItem();
    ASSERT_NE(clone, nullptr);
    EXPECT_EQ(clone->type, ProjectItem::Type::Layer);
    EXPECT_TRUE(clone->name.startsWith(QStringLiteral("Ctx_Layer")));
    ASSERT_EQ(clone->children.size(), 1u);
    EXPECT_EQ(stack->index(), baseIndex + 1);
}

TEST_F(ProjectTreePanelAnchorTest, NewActionCreatesOneActionPerSelectedObject)
{
    auto& pm = ProjectManager::instance();
    ProjectItem* first = pm.createItem(contextLayer_, QStringLiteral("NA_SoundA"),
                                       ProjectItem::Type::Sound);
    ProjectItem* second = pm.createItem(contextLayer_, QStringLiteral("NA_SoundB"),
                                        ProjectItem::Type::Sound);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    panel_->setSelectedProjectItems({first, second});
    panel_->armMenuContext(first);

    QUndoStack* stack = UndoRedoManager::instance().undoStack();
    const int baseIndex = stack->index();
    panel_->onNewAction(VActionStep::Type::Play);

    const QList<ProjectItem*> selected = panel_->selectedItems();
    ASSERT_EQ(selected.size(), 2);
    EXPECT_EQ(stack->index(), baseIndex + 1);
    QSet<uint32_t> targets;
    for (ProjectItem* action : selected) {
        ASSERT_EQ(action->type, ProjectItem::Type::Action);
        const auto* component = action->comp<VAction>();
        ASSERT_NE(component, nullptr);
        ASSERT_EQ(component->steps.size(), 1u);
        EXPECT_EQ(component->steps.front().type, VActionStep::Type::Play);
        targets.insert(component->steps.front().targetObjectID);
    }
    EXPECT_TRUE(targets.contains(first->objectID));
    EXPECT_TRUE(targets.contains(second->objectID));

    stack->undo();
    EXPECT_TRUE(panel_->selectedItems().isEmpty()
                || (pm.findItemByGuid(selected.front()->guid) == nullptr
                    && pm.findItemByGuid(selected.back()->guid) == nullptr));
    EXPECT_EQ(pm.findItemByGuid(selected.front()->guid), nullptr);
    EXPECT_EQ(pm.findItemByGuid(selected.back()->guid), nullptr);
}

TEST_F(ProjectTreePanelAnchorTest, ParentAndChildNewActionKeepsBothTargets)
{
    auto& pm = ProjectManager::instance();
    ProjectItem* container = pm.createItem(contextLayer_, QStringLiteral("NA_Container"),
                                           ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    ProjectItem* sound = pm.createItem(container, QStringLiteral("NA_Covered"),
                                       ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    ASSERT_TRUE(panel_->selectProjectItem(container));
    QTreeWidgetItem* soundRow = findRow(panel_->treeWidget(), QStringLiteral("NA_Covered"));
    ASSERT_NE(soundRow, nullptr);
    soundRow->setSelected(true);
    panel_->armMenuContext(sound);
    panel_->onNewAction(VActionStep::Type::Play);

    const QList<ProjectItem*> selected = panel_->selectedItems();
    ASSERT_EQ(selected.size(), 2);
    QSet<uint32_t> targets;
    for (ProjectItem* action : selected) {
        ASSERT_EQ(action->type, ProjectItem::Type::Action);
        const auto* component = action->comp<VAction>();
        ASSERT_NE(component, nullptr);
        ASSERT_EQ(component->steps.size(), 1u);
        targets.insert(component->steps.front().targetObjectID);
    }
    EXPECT_TRUE(targets.contains(container->objectID));
    EXPECT_TRUE(targets.contains(sound->objectID));
}

TEST_F(ProjectTreePanelAnchorTest, NewParentMovesFolderAndHierarchyInOneUndoCommand)
{
    auto& manager = ProjectManager::instance();
    auto* first = manager.createItem(contextLayer_, QStringLiteral("First"), ProjectItem::Type::Sound);
    auto* second = manager.createItem(contextLayer_, QStringLiteral("Second"), ProjectItem::Type::Sound);
    auto* folder = manager.createFolder(nullptr, QStringLiteral("Other"), QStringLiteral("Layers"));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(folder, nullptr);
    auto* project = manager.currentProject();
    project->setObjectFileLocation(first, folder, {});
    project->setObjectFileLocation(second, folder, {});
    ASSERT_TRUE(manager.saveProject());
    contextLayer_->extensions.insert(QStringLiteral("test"), QStringLiteral("Unsaved"));
    contextLayer_->markDirty();
    panel_->setSelectedProjectItems({first, second});
    panel_->armMenuContext(first);
    auto* newParent = panel_->menuAction(QStringLiteral("New Parent"));
    ASSERT_NE(newParent, nullptr);
    ASSERT_NE(newParent->menu(), nullptr);
    QAction* createContainer = nullptr;
    for (auto* action : newParent->menu()->actions()) {
        if (action->text() == ProjectItemMenuController::typeDisplayName(ProjectItem::Type::Container))
            createContainer = action;
    }
    ASSERT_NE(createContainer, nullptr);
    ASSERT_TRUE(createContainer->isEnabled());
    auto& undo = UndoRedoManager::instance();
    const int history = undo.undoStack()->count();
    const QString firstPath = project->objectFilePath(first);
    createContainer->trigger();
    ASSERT_EQ(contextLayer_->children.size(), 1u);
    auto* container = contextLayer_->children.front().get();
    EXPECT_EQ(container->type, ProjectItem::Type::Container);
    EXPECT_EQ(first->parent, container);
    EXPECT_EQ(second->parent, container);
    EXPECT_EQ(project->objectFolder(first), project->objectFolder(contextLayer_));
    EXPECT_EQ(project->objectFolder(second), project->objectFolder(contextLayer_));
    EXPECT_EQ(undo.undoStack()->count(), history + 1);
    EXPECT_TRUE(contextLayer_->isDirtyObject());
    EXPECT_TRUE(QFileInfo::exists(firstPath));
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(first->parent, contextLayer_);
    EXPECT_EQ(second->parent, contextLayer_);
    EXPECT_EQ(project->objectFolder(first), folder);
    EXPECT_EQ(project->objectFolder(second), folder);
}

TEST_F(ProjectTreePanelAnchorTest, CutPasteRestoresResolvedParentAndFolderTogether)
{
    auto& manager = ProjectManager::instance();
    auto* source = manager.createItem(nullptr, QStringLiteral("Source"), ProjectItem::Type::Sound);
    auto* target = manager.createItem(contextLayer_, QStringLiteral("Target"), ProjectItem::Type::Sound);
    auto* folder = manager.createFolder(nullptr, QStringLiteral("Other"), QStringLiteral("Layers"));
    ASSERT_NE(source, nullptr);
    ASSERT_NE(target, nullptr);
    ASSERT_NE(folder, nullptr);
    auto* project = manager.currentProject();
    project->setObjectFileLocation(source, folder, {});
    project->setObjectFileLocation(target, folder, {});
    ASSERT_TRUE(manager.saveProject());
    source->extensions.insert(QStringLiteral("test"), QStringLiteral("Unsaved"));
    source->markDirty();
    ASSERT_EQ(manager.setClipboard({source}, ProjectManager::ClipboardOp::Cut), 1);
    ASSERT_TRUE(panel_->selectProjectItem(target));
    panel_->armMenuContext(target);
    auto& undo = UndoRedoManager::instance();
    const int history = undo.undoStack()->count();
    const QString sourcePath = project->objectFilePath(source);
    panel_->onPaste();
    EXPECT_EQ(source->parent, contextLayer_);
    EXPECT_EQ(project->objectFolder(source), project->objectFolder(contextLayer_));
    EXPECT_EQ(project->objectFolder(target), folder);
    EXPECT_TRUE(manager.clipboardEmpty());
    EXPECT_TRUE(source->isDirtyObject());
    EXPECT_TRUE(QFileInfo::exists(sourcePath));
    EXPECT_EQ(undo.undoStack()->count(), history + 1);
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(source->parent, nullptr);
    EXPECT_EQ(project->objectFolder(source), folder);
}

TEST_F(ProjectTreePanelAnchorTest, PasteSelectsEveryCreatedRoot)
{
    auto& pm = ProjectManager::instance();
    ProjectItem* first = pm.createItem(nullptr, QStringLiteral("PasteA_Layer"),
                                       ProjectItem::Type::Layer);
    ProjectItem* second = pm.createItem(nullptr, QStringLiteral("PasteB_Layer"),
                                        ProjectItem::Type::Layer);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_EQ(pm.setClipboard({first, second}, ProjectManager::ClipboardOp::Copy), 2);

    QUndoStack* stack = UndoRedoManager::instance().undoStack();
    const int baseIndex = stack->index();
    panel_->armMenuContext(contextLayer_);
    panel_->onPaste();

    const QList<ProjectItem*> selected = panel_->selectedItems();
    ASSERT_EQ(selected.size(), 2);
    EXPECT_EQ(stack->index(), baseIndex + 1);
    for (ProjectItem* item : selected) {
        EXPECT_EQ(item->parent, contextLayer_);
        EXPECT_TRUE(item->name.startsWith(QStringLiteral("PasteA_Layer"))
                    || item->name.startsWith(QStringLiteral("PasteB_Layer")));
    }

    stack->undo();
    EXPECT_EQ(pm.findItemByGuid(selected.front()->guid), nullptr);
    EXPECT_EQ(pm.findItemByGuid(selected.back()->guid), nullptr);
}

TEST_F(ProjectTreePanelAnchorTest, MissingMenuTargetRejectsTheWholeBatch)
{
    auto& pm = ProjectManager::instance();
    ASSERT_EQ(pm.setClipboard({selectedLayer_}, ProjectManager::ClipboardOp::Cut), 1);
    panel_->setSelectedProjectItems({contextLayer_, selectedLayer_});
    panel_->armMenuContext(contextLayer_);
    ASSERT_TRUE(pm.deleteItem(contextLayer_));
    UndoRedoManager::instance().clear();

    panel_->onCopy();
    EXPECT_EQ(pm.clipboardOp(), ProjectManager::ClipboardOp::Cut);
    EXPECT_EQ(pm.resolveClipboardSources(), (std::vector<const ProjectItem*>{selectedLayer_}));
    panel_->onDuplicate();
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(ProjectTreePanelAnchorTest, BlankContextCannotExpandThePreviousCurrentRow)
{
    auto& pm = ProjectManager::instance();
    ASSERT_NE(pm.createItem(selectedLayer_, QStringLiteral("BlankChild"), ProjectItem::Type::Sound), nullptr);
    auto* selectedRow = panel_->treeItemForProjectItem(selectedLayer_);
    ASSERT_NE(selectedRow, nullptr);
    selectedRow->setExpanded(false);
    panel_->prepareItemContextMenu(nullptr, nullptr);
    auto* operation = panel_->menuAction(QStringLiteral("Operation"));
    ASSERT_NE(operation, nullptr);
    EXPECT_TRUE(operation->isVisible());
    EXPECT_FALSE(operation->isEnabled());
    ASSERT_NE(operation->menu(), nullptr);
    for (auto* action : operation->menu()->actions()) {
        EXPECT_FALSE(action->isEnabled());
        action->trigger();
    }
    EXPECT_FALSE(selectedRow->isExpanded());
    EXPECT_TRUE(panel_->selectedItems().isEmpty());
}

TEST_F(ProjectTreePanelAnchorTest, BucketContextRemainsThePasteDestinationAfterClosingMenu)
{
    auto& pm = ProjectManager::instance();
    ASSERT_EQ(pm.setClipboard({selectedLayer_}, ProjectManager::ClipboardOp::Copy), 1);
    auto* bucket = panel_->treeItemForProjectItem(selectedLayer_)->parent();
    ASSERT_NE(bucket, nullptr);
    panel_->prepareItemContextMenu(bucket, nullptr);
    panel_->disarmMenuContext();
    panel_->onPaste();
    auto* result = panel_->selectedItem();
    ASSERT_NE(result, nullptr);
    EXPECT_NE(result, selectedLayer_);
    EXPECT_EQ(result->parent, nullptr);
}

TEST_F(ProjectTreePanelAnchorTest, PastePropertiesRejectsUnsupportedSourcesAndDoesNotRecordNoOps)
{
    auto& pm = ProjectManager::instance();
    auto* action = pm.createItem(nullptr, QStringLiteral("NoPasteProperties"), ProjectItem::Type::Action);
    ASSERT_NE(action, nullptr);
    ASSERT_EQ(pm.setClipboard({action}, ProjectManager::ClipboardOp::Copy), 1);
    panel_->armMenuContext(action);
    EXPECT_FALSE(panel_->menuAction(QStringLiteral("Paste Properties"))->isVisible());
    EXPECT_FALSE(panel_->menuAction(QStringLiteral("Paste Properties"))->isEnabled());

    ASSERT_EQ(pm.setClipboard({contextLayer_}, ProjectManager::ClipboardOp::Copy), 1);
    panel_->armMenuContext(selectedLayer_);
    UndoRedoManager::instance().clear();
    panel_->onPasteProperties();
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(ProjectTreePanelAnchorTest, CreationMenusRejectReadOnlyTimelineBeforePrompting)
{
    auto& pm = ProjectManager::instance();
    auto* piece = pm.createItem(contextLayer_, QStringLiteral("RestrictedPiece"), ProjectItem::Type::Piece);
    ASSERT_NE(piece, nullptr);
    auto* track = pm.createItem(piece, QStringLiteral("RestrictedTrack"), ProjectItem::Type::Track);
    ASSERT_NE(track, nullptr);
    auto* sound = pm.createItem(track, QStringLiteral("RestrictedSound"), ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    piece->comp<VPiece>()->pieceTime = PieceTimeSpec::musical();
    UndoRedoManager::instance().clear();
    panel_->armMenuContext(piece);
    EXPECT_TRUE(panel_->menuAction(QStringLiteral("New Child"))->isVisible());
    EXPECT_FALSE(panel_->menuAction(QStringLiteral("New Child"))->isEnabled());
    panel_->armMenuContext(sound);
    EXPECT_TRUE(panel_->menuAction(QStringLiteral("New Parent"))->isVisible());
    EXPECT_FALSE(panel_->menuAction(QStringLiteral("New Parent"))->isEnabled());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(ProjectTreePanelAnchorTest, KeyboardMenuUsesCurrentSelectionInsteadOfEventPosition)
{
    panel_->resize(700, 500);
    panel_->show();
    panel_->setSelectedProjectItems({contextLayer_, selectedLayer_});
    QTimer::singleShot(0, [&] {
        EXPECT_EQ(panel_->selectedItems().size(), 2);
        EXPECT_TRUE(panel_->menuAction(QStringLiteral("Copy"))->isEnabled());
        panel_->menu()->close();
    });
    QContextMenuEvent event(QContextMenuEvent::Keyboard, QPoint(-1, -1), QPoint(-1, -1));
    QApplication::sendEvent(panel_->treeWidget(), &event);
    EXPECT_EQ(panel_->selectedItems().size(), 2);
}

TEST_F(ProjectTreePanelAnchorTest, ColorDialogRejectsTheWholeBatchWhenATargetDisappears)
{
    auto& pm = ProjectManager::instance();
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
    EXPECT_EQ(selectedLayer_->color, 0u);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(ProjectTreePanelAnchorTest, DeleteBatchIsSilentAndOneUndoStepPreservesUnselectedItems)
{
    auto& pm = ProjectManager::instance();
    auto* untouched = pm.createItem(nullptr, QStringLiteral("Untouched"), ProjectItem::Type::Layer);
    ASSERT_NE(untouched, nullptr);
    const QUuid firstGuid = contextLayer_->guid;
    const QUuid secondGuid = selectedLayer_->guid;
    auto& undo = UndoRedoManager::instance();
    const int history = undo.undoStack()->count();
    panel_->setSelectedProjectItems({contextLayer_, selectedLayer_});
    panel_->armMenuContext(contextLayer_);
    deleteWithoutDialog();
    EXPECT_EQ(pm.findItemByGuid(firstGuid), nullptr);
    EXPECT_EQ(pm.findItemByGuid(secondGuid), nullptr);
    EXPECT_EQ(pm.findItemByGuid(untouched->guid), untouched);
    EXPECT_EQ(undo.undoStack()->count(), history + 1);
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(pm.findItemByGuid(firstGuid), contextLayer_);
    EXPECT_EQ(pm.findItemByGuid(secondGuid), selectedLayer_);
    EXPECT_EQ(pm.findItemByGuid(untouched->guid), untouched);
}


TEST_F(ProjectTreePanelAnchorTest, CreationOffersEditableNamesAcrossObjectTypesAndPreservesIdentity)
{
    panel_->resize(700, 500);
    panel_->show();
    panel_->activateWindow();
    QApplication::processEvents();
    auto& manager = ProjectManager::instance();
    for (const auto& spec : ProjectItemMenuController::specs()) {
        if (spec.comingSoon) continue;
        SCOPED_TRACE(spec.defaultNameKey);
        if (spec.type == ProjectItem::Type::Bus)
            panel_->selectProjectItem(manager.getMasterBusItem());
        else
            panel_->selectProjectItem(contextLayer_);
        auto* created = panel_->createItem(spec.type);
        ASSERT_NE(created, nullptr);
        const QUuid guid = created->guid;
        const QString initialName = created->name;
        QApplication::processEvents();
        auto* editor = panel_->findChild<InlineRenameEdit*>();
        ASSERT_NE(editor, nullptr);
        EXPECT_TRUE(editor->hasFocus());
        EXPECT_EQ(editor->selectedText(), initialName);
        editor->setText(QStringLiteral("Authored_") + initialName);
        ASSERT_TRUE(editor->finish(true));
        EXPECT_EQ(manager.findItemByGuid(guid), created);
        EXPECT_EQ(created->name, QStringLiteral("Authored_") + initialName);
        UndoRedoManager::instance().undo();
        EXPECT_EQ(created->name, initialName);
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

TEST_F(ProjectTreePanelAnchorTest, InlineRenameDoesNotTouchADeletedTarget)
{
    auto& pm = ProjectManager::instance();
    const QUuid guid = contextLayer_->guid;
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
    EXPECT_EQ(selectedLayer_->name, QStringLiteral("Sel_Layer"));
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(ProjectTreePanelAnchorTest, InlineRenameAllowsExplicitDuplicateNames)
{
    const QString originalName = contextLayer_->name;
    const QString requestedName = selectedLayer_->name;
    const QUuid guid = contextLayer_->guid;
    panel_->armMenuContext(contextLayer_);
    panel_->onRename();
    auto* editor = panel_->findChild<InlineRenameEdit*>();
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(editor->text(), originalName);
    editor->setText(requestedName);
    EXPECT_TRUE(editor->finish(true));
    EXPECT_EQ(QApplication::activeModalWidget(), nullptr);
    EXPECT_EQ(contextLayer_->guid, guid);
    EXPECT_EQ(contextLayer_->name, requestedName);
    EXPECT_EQ(selectedLayer_->name, requestedName);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(contextLayer_->name, originalName);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(contextLayer_->name, requestedName);
}

TEST_F(ProjectTreePanelAnchorTest, InlineRenameKeepsInvalidNamesEditableWithoutWarnings)
{
    panel_->resize(420, 500);
    panel_->show();
    panel_->armMenuContext(contextLayer_);
    panel_->onRename();
    auto* editor = panel_->findChild<InlineRenameEdit*>();
    ASSERT_NE(editor, nullptr);
    const QString original = contextLayer_->name;
    UndoRedoManager::instance().clear();
    QObject observer;
    int warnings = 0;
    QObject::connect(&ProjectManager::instance(), &ProjectManager::errorOccurred,
                     &observer, [&](const QString&) { ++warnings; });
    for (const QString& value : {QString(), QStringLiteral(" "), QStringLiteral("Bad/Name")}) {
        editor->setText(value);
        EXPECT_TRUE(editor->property("inputInvalid").toBool());
        EXPECT_FALSE(editor->toolTip().isEmpty());
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(editor, &enter);
        EXPECT_TRUE(editor->isVisible());
        EXPECT_EQ(editor->text(), value);
        EXPECT_EQ(contextLayer_->name, original);
        EXPECT_EQ(QApplication::activeModalWidget(), nullptr);
    }
    EXPECT_EQ(warnings, 0);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    editor->setText(QStringLiteral("Corrected"));
    EXPECT_FALSE(editor->property("inputInvalid").toBool());
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(editor, &enter);
    EXPECT_EQ(contextLayer_->name, QStringLiteral("Corrected"));
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    panel_->onRename();
    editor = panel_->findChild<InlineRenameEdit*>();
    ASSERT_NE(editor, nullptr);
    editor->clear();
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(editor, &escape);
    EXPECT_EQ(contextLayer_->name, QStringLiteral("Corrected"));
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
}

TEST_F(ProjectTreePanelAnchorTest, SelectedNameSlowClickRenamesAndFastDoubleClickDoesNot)
{
    panel_->resize(420, 500);
    panel_->show();
    auto* tree = panel_->treeWidget();
    tree->scrollToItem(panel_->treeItemForProjectItem(contextLayer_));
    QApplication::processEvents();
    QApplication::setActiveWindow(panel_.get());
    tree->setFocus(Qt::MouseFocusReason);
    const QRect rowRect = tree->visualItemRect(panel_->treeItemForProjectItem(contextLayer_));
    QPoint point(rowRect.left() + tree->iconSize().width() + 10, rowRect.center().y());
    const auto mouse = [&](QEvent::Type type, Qt::MouseButtons buttons) {
        QMouseEvent event(type, QPointF(point), QPointF(tree->viewport()->mapToGlobal(point)),
                          Qt::LeftButton, buttons, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &event);
    };
    const auto wait = [] {
        QEventLoop loop;
        QTimer::singleShot(QApplication::doubleClickInterval() + 100, &loop, &QEventLoop::quit);
        loop.exec();
    };
    mouse(QEvent::MouseButtonPress, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, Qt::NoButton);
    wait();
    EXPECT_EQ(tree->findChild<InlineRenameEdit*>(), nullptr);
    ASSERT_TRUE(tree->hasFocus());
    ASSERT_TRUE(tree->isActiveWindow());
    ASSERT_EQ(tree->currentItem(), panel_->treeItemForProjectItem(contextLayer_));
    mouse(QEvent::MouseButtonPress, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, Qt::NoButton);
    wait();
    auto* editor = tree->findChild<InlineRenameEdit*>();
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(editor->text(), contextLayer_->name);
    editor->finish(false);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    const QPoint namePoint = point;
    for (const auto& position : {QPoint(rowRect.left() + 5, point.y()),
                                 QPoint(rowRect.right() - 5, point.y())}) {
        point = position;
        mouse(QEvent::MouseButtonPress, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, Qt::NoButton);
        wait();
        EXPECT_EQ(tree->findChild<InlineRenameEdit*>(), nullptr);
    }
    point = namePoint;
    panel_->setSelectedProjectItems({contextLayer_, selectedLayer_});
    mouse(QEvent::MouseButtonPress, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, Qt::NoButton);
    wait();
    EXPECT_EQ(tree->findChild<InlineRenameEdit*>(), nullptr);
    QLineEdit otherPanel(panel_.get());
    QLineEdit otherWindow;
    for (auto* other : {&otherPanel, &otherWindow}) {
        other->show();
        QApplication::setActiveWindow(other->window());
        other->setFocus(Qt::MouseFocusReason);
        QApplication::processEvents();
        ASSERT_TRUE(other->hasFocus());
        QApplication::setActiveWindow(panel_.get());
        tree->setFocus(Qt::MouseFocusReason);
        mouse(QEvent::MouseButtonPress, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, Qt::NoButton);
        wait();
        EXPECT_EQ(tree->findChild<InlineRenameEdit*>(), nullptr);
        mouse(QEvent::MouseButtonPress, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, Qt::NoButton);
        QApplication::setActiveWindow(other->window());
        other->setFocus(Qt::MouseFocusReason);
        wait();
        EXPECT_EQ(tree->findChild<InlineRenameEdit*>(), nullptr);
        other->hide();
    }
    QApplication::setActiveWindow(panel_.get());
    tree->setFocus(Qt::MouseFocusReason);
    mouse(QEvent::MouseButtonPress, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, Qt::NoButton);
    wait();
    EXPECT_EQ(tree->findChild<InlineRenameEdit*>(), nullptr);
    mouse(QEvent::MouseButtonPress, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, Qt::NoButton);
    wait();
    editor = tree->findChild<InlineRenameEdit*>();
    ASSERT_NE(editor, nullptr);
    editor->finish(false);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    mouse(QEvent::MouseButtonPress, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, Qt::NoButton);
    mouse(QEvent::MouseButtonDblClick, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, Qt::NoButton);
    wait();
    EXPECT_EQ(tree->findChild<InlineRenameEdit*>(), nullptr);
}

TEST_F(ProjectTreePanelAnchorTest, DuplicateSearchLabelsSurvivePropertyUpdatesAndSave)
{
    auto& pm = ProjectManager::instance();
    auto* first = pm.createItem(contextLayer_, QStringLiteral("SharedSound"), ProjectItem::Type::Sound);
    auto* second = pm.createItem(selectedLayer_, QStringLiteral("SharedSound"), ProjectItem::Type::Sound);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    auto* search = panel_->findChild<SearchField*>(QStringLiteral("projectSearchBox"));
    ASSERT_NE(search, nullptr);
    search->setText(QStringLiteral("SharedSound"));
    auto* firstRow = panel_->treeItemForProjectItem(first);
    auto* secondRow = panel_->treeItemForProjectItem(second);
    ASSERT_NE(firstRow, nullptr);
    ASSERT_NE(secondRow, nullptr);
    const QString firstLabel = rowLabel(firstRow);
    const QString secondLabel = rowLabel(secondRow);
    EXPECT_TRUE(firstLabel.contains(QStringLiteral("Ctx_Layer/SharedSound")));
    EXPECT_TRUE(secondLabel.contains(QStringLiteral("Sel_Layer/SharedSound")));
    EXPECT_NE(firstLabel, secondLabel);

    pm.setItemsColor({first, second}, QColor(12, 34, 56).rgba());
    EXPECT_EQ(rowLabel(firstRow), firstLabel);
    EXPECT_EQ(rowLabel(secondRow), secondLabel);
    ASSERT_TRUE(pm.saveProject());
    EXPECT_EQ(firstRow->text(0), firstLabel);
    EXPECT_EQ(secondRow->text(0), secondLabel);

    search->clear();
    EXPECT_EQ(rowLabel(firstRow), QStringLiteral("SharedSound"));
    EXPECT_EQ(rowLabel(secondRow), QStringLiteral("SharedSound"));
}

TEST_F(ProjectTreePanelAnchorTest, DuplicateSearchLabelsFollowHierarchyChanges)
{
    auto& pm = ProjectManager::instance();
    auto* first = pm.createItem(contextLayer_, QStringLiteral("SharedSound"), ProjectItem::Type::Sound);
    ASSERT_NE(first, nullptr);
    auto* search = panel_->findChild<SearchField*>(QStringLiteral("projectSearchBox"));
    ASSERT_NE(search, nullptr);
    search->setText(QStringLiteral("SharedSound"));
    auto* firstRow = panel_->treeItemForProjectItem(first);
    ASSERT_NE(firstRow, nullptr);
    EXPECT_EQ(rowLabel(firstRow), QStringLiteral("SharedSound"));

    auto* second = pm.createItem(selectedLayer_, QStringLiteral("SharedSound"), ProjectItem::Type::Sound);
    ASSERT_NE(second, nullptr);
    QApplication::processEvents();
    auto* secondRow = panel_->treeItemForProjectItem(second);
    ASSERT_NE(secondRow, nullptr);
    EXPECT_TRUE(rowLabel(firstRow).contains(QStringLiteral("Ctx_Layer/SharedSound")));
    EXPECT_TRUE(rowLabel(secondRow).contains(QStringLiteral("Sel_Layer/SharedSound")));

    ASSERT_TRUE(pm.renameItem(contextLayer_, QStringLiteral("RenamedLayer")));
    EXPECT_TRUE(rowLabel(firstRow).contains(QStringLiteral("RenamedLayer/SharedSound")));
    EXPECT_TRUE(firstRow->toolTip(0).contains(QStringLiteral("RenamedLayer/SharedSound")));
    EXPECT_FALSE(firstRow->toolTip(0).contains(QStringLiteral("Ctx_Layer")));

    ASSERT_TRUE(pm.moveItem(second, contextLayer_));
    QApplication::processEvents();
    EXPECT_TRUE(rowLabel(firstRow).contains(first->guid.toString(QUuid::WithoutBraces)));
    EXPECT_TRUE(rowLabel(secondRow).contains(second->guid.toString(QUuid::WithoutBraces)));
    EXPECT_TRUE(secondRow->toolTip(0).contains(QStringLiteral("RenamedLayer/SharedSound")));
    EXPECT_FALSE(secondRow->toolTip(0).contains(QStringLiteral("Sel_Layer")));

    ASSERT_TRUE(pm.deleteItem(second));
    QApplication::processEvents();
    EXPECT_EQ(rowLabel(firstRow), QStringLiteral("SharedSound"));
    search->clear();
    ASSERT_TRUE(pm.renameItem(contextLayer_, QStringLiteral("FinalLayer")));
    EXPECT_EQ(rowLabel(firstRow), QStringLiteral("SharedSound"));
    EXPECT_TRUE(firstRow->toolTip(0).contains(QStringLiteral("FinalLayer/SharedSound")));
}

TEST_F(ProjectTreePanelAnchorTest, RightMouseMenuPreservesSelectedRowsAndReplacesOutsideSelection)
{
    panel_->resize(700, 500);
    panel_->show();
    panel_->treeWidget()->expandAll();
    auto* tree = panel_->treeWidget();
    panel_->setSelectedProjectItems({contextLayer_, selectedLayer_});
    auto rightClick = [&](ProjectItem* item, Qt::KeyboardModifiers modifiers, int expectedCount) {
        const QPoint position = tree->visualItemRect(panel_->treeItemForProjectItem(item)).center();
        const QPoint global = tree->viewport()->mapToGlobal(position);
        QMouseEvent press(QEvent::MouseButtonPress, position, global, Qt::RightButton, Qt::RightButton, modifiers);
        QApplication::sendEvent(tree->viewport(), &press);
        QTimer::singleShot(0, [&, expectedCount] {
            EXPECT_EQ(panel_->selectedItems().size(), expectedCount);
            EXPECT_TRUE(panel_->selectedItems().contains(item));
            panel_->menu()->close();
        });
        QContextMenuEvent context(QContextMenuEvent::Mouse, position, global, modifiers);
        QApplication::sendEvent(tree->viewport(), &context);
        QMouseEvent release(QEvent::MouseButtonRelease, position, global, Qt::RightButton, Qt::NoButton, modifiers);
        QApplication::sendEvent(tree->viewport(), &release);
        EXPECT_EQ(panel_->selectedItems().size(), expectedCount);
    };
    rightClick(contextLayer_, Qt::NoModifier, 2);
    rightClick(selectedLayer_, Qt::ControlModifier, 2);
    auto* other = ProjectManager::instance().createItem(nullptr, QStringLiteral("OutsideSelection"), ProjectItem::Type::Layer);
    ASSERT_NE(other, nullptr);
    rightClick(other, Qt::ControlModifier, 1);
}

TEST_F(ProjectTreePanelAnchorTest, BusDepthLimitDisablesCreationAndParentWrapping)
{
    auto& pm = ProjectManager::instance();
    ProjectItem* parent = pm.getMasterBusItem();
    for (int i = 1; i < 8; ++i) {
        parent = pm.createItem(parent, QStringLiteral("DeepBus%1").arg(i), ProjectItem::Type::Bus);
        ASSERT_NE(parent, nullptr);
    }
    panel_->armMenuContext(parent);
    EXPECT_TRUE(panel_->menuAction(QStringLiteral("New Child"))->isVisible());
    EXPECT_FALSE(panel_->menuAction(QStringLiteral("New Child"))->isEnabled());
    EXPECT_TRUE(panel_->menuAction(QStringLiteral("New Parent"))->isVisible());
    EXPECT_FALSE(panel_->menuAction(QStringLiteral("New Parent"))->isEnabled());
}

TEST_F(ProjectTreePanelAnchorTest, PastePropertiesPersistsBothExplicitParentAndChildTargets)
{
    using Property = harkenplayableproperties::PlayablePropertyIdV1;
    auto& pm = ProjectManager::instance();
    auto* source = pm.createItem(contextLayer_, QStringLiteral("PropertySource"), ProjectItem::Type::Container);
    auto* parent = pm.createItem(contextLayer_, QStringLiteral("PropertyParent"), ProjectItem::Type::Container);
    ASSERT_NE(source, nullptr);
    ASSERT_NE(parent, nullptr);
    auto* child = pm.createItem(parent, QStringLiteral("PropertyChild"), ProjectItem::Type::Container);
    ASSERT_NE(child, nullptr);
    source->comp<VMixNode>()->volume = -7.5f;
    source->comp<VMixNode>()->pitch = 3.0f;
    source->playablePropertyPresence.Set(Property::Volume);
    source->playablePropertyPresence.Set(Property::Pitch);
    ASSERT_EQ(pm.setClipboard({source}, ProjectManager::ClipboardOp::Copy), 1);
    panel_->setSelectedProjectItems({parent, child});
    panel_->armMenuContext(parent);
    UndoRedoManager::instance().clear();
    panel_->onPasteProperties();
    auto* stack = UndoRedoManager::instance().undoStack();
    EXPECT_EQ(stack->count(), 1);
    EXPECT_EQ(panel_->selectedItems().size(), 2);
    for (auto* item : {parent, child}) {
        EXPECT_FLOAT_EQ(item->comp<VMixNode>()->volume, -7.5f);
        EXPECT_TRUE(item->playablePropertyPresence.Has(Property::Volume));
        EXPECT_TRUE(item->playablePropertyPresence.Has(Property::Pitch));
    }
    stack->undo();
    for (auto* item : {parent, child}) {
        EXPECT_FLOAT_EQ(item->comp<VMixNode>()->volume, 0.0f);
        EXPECT_FALSE(item->playablePropertyPresence.Has(Property::Volume));
    }
    stack->redo();
    panel_->onPasteProperties();
    EXPECT_EQ(stack->count(), 1);
    const QList<QUuid> guids{parent->guid, child->guid};
    const QString path = pm.currentProject()->projectPath;
    ASSERT_TRUE(pm.saveProject());
    ASSERT_TRUE(pm.closeProject());
    QString error;
    ASSERT_TRUE(pm.loadProject(path, false, &error)) << error.toStdString();
    for (const auto& guid : guids) {
        auto* item = pm.findItemByGuid(guid);
        ASSERT_NE(item, nullptr);
        EXPECT_FLOAT_EQ(item->comp<VMixNode>()->volume, -7.5f);
        EXPECT_FLOAT_EQ(item->comp<VMixNode>()->pitch, 3.0f);
    }
}

TEST_F(ProjectTreePanelAnchorTest, LongTargetNamesStillCreateDistinctActionsInOneBatch)
{
    auto& pm = ProjectManager::instance();
    const QString stem(NameValidator::MAX_NAME_LENGTH - 1, QLatin1Char('A'));
    auto* first = pm.createItem(contextLayer_, stem + QLatin1Char('1'), ProjectItem::Type::Sound);
    auto* second = pm.createItem(contextLayer_, stem + QLatin1Char('2'), ProjectItem::Type::Sound);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    panel_->setSelectedProjectItems({first, second});
    panel_->armMenuContext(first);
    UndoRedoManager::instance().clear();
    panel_->onNewAction(VActionStep::Type::Play);
    const auto actions = panel_->selectedItems();
    ASSERT_EQ(actions.size(), 2);
    EXPECT_NE(actions[0]->name, actions[1]->name);
    for (auto* action : actions) {
        EXPECT_EQ(action->type, ProjectItem::Type::Action);
        EXPECT_TRUE(NameValidator::validate(action->name));
        ASSERT_EQ(action->comp<VAction>()->steps.size(), 1u);
    }
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
}

TEST_F(ProjectTreePanelAnchorTest, MissingSourceDisablesOpenContainingFolder)
{
    auto& pm = ProjectManager::instance();
    auto* sound = pm.createItem(contextLayer_, QStringLiteral("MissingSource"), ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    auto* sources = &sound->comp<VSound>()->sources.plain;
    sources->entries.push_back({QUuid::createUuid(), 123456});
    sources->active = sources->entries.front().entryId;
    for (ProjectItem* item : {sound, contextLayer_, pm.getMasterBusItem(), static_cast<ProjectItem*>(nullptr)}) {
        panel_->armMenuContext(item);
        EXPECT_FALSE(panel_->menuAction(QStringLiteral("Open Containing Folder"))->isEnabled());
    }
}

TEST_F(ProjectTreePanelAnchorTest, SelectionAndClipboardStateDisableRelevantCommandsWithoutHiding)
{
    auto& pm = ProjectManager::instance();
    panel_->armMenuContext(contextLayer_);
    auto* paste = panel_->menuAction(QStringLiteral("Paste"));
    auto* properties = panel_->menuAction(QStringLiteral("Paste Properties"));
    ASSERT_TRUE(paste->isVisible());
    EXPECT_FALSE(paste->isEnabled());
    ASSERT_TRUE(properties->isVisible());
    EXPECT_FALSE(properties->isEnabled());

    ASSERT_EQ(pm.setClipboard({selectedLayer_}, ProjectManager::ClipboardOp::Copy), 1);
    panel_->armMenuContext(contextLayer_);
    EXPECT_TRUE(paste->isVisible());
    EXPECT_TRUE(paste->isEnabled());
    EXPECT_TRUE(properties->isVisible());
    EXPECT_TRUE(properties->isEnabled());

    panel_->setSelectedProjectItems({contextLayer_, selectedLayer_});
    for (ProjectItem* item : {contextLayer_, selectedLayer_}) {
        panel_->armMenuContext(item);
        for (const auto& label : {QStringLiteral("Rename"), QStringLiteral("New Child"),
                                  QStringLiteral("Paste"), QStringLiteral("Open in New Window")}) {
            auto* action = panel_->menuAction(label);
            ASSERT_NE(action, nullptr);
            EXPECT_TRUE(action->isVisible()) << label.toStdString();
            EXPECT_FALSE(action->isEnabled()) << label.toStdString();
        }
    }
}

TEST_F(ProjectTreePanelAnchorTest, ExpandCommandsFollowBranchScopeAndExpansionState)
{
    auto& pm = ProjectManager::instance();
    auto* container = pm.createItem(contextLayer_, QStringLiteral("ExpansionBranch"), ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    auto* sound = pm.createItem(container, QStringLiteral("ExpansionLeaf"), ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(pm.createItem(selectedLayer_, QStringLiteral("OtherLeaf"), ProjectItem::Type::Sound), nullptr);
    auto* otherAction = pm.createItem(nullptr, QStringLiteral("OtherAction"), ProjectItem::Type::Action);
    ASSERT_NE(otherAction, nullptr);
    auto* contextRow = panel_->treeItemForProjectItem(contextLayer_);
    auto* containerRow = panel_->treeItemForProjectItem(container);
    auto* selectedRow = panel_->treeItemForProjectItem(selectedLayer_);
    auto* actionRow = panel_->treeItemForProjectItem(otherAction);
    ASSERT_NE(contextRow, nullptr);
    ASSERT_NE(containerRow, nullptr);
    ASSERT_NE(selectedRow, nullptr);
    ASSERT_NE(actionRow, nullptr);
    auto* bucket = contextRow->parent();
    auto* otherBucket = actionRow->parent();
    ASSERT_NE(bucket, nullptr);
    ASSERT_NE(otherBucket, nullptr);
    ASSERT_NE(bucket, otherBucket);
    const int undoIndex = UndoRedoManager::instance().undoStack()->index();

    for (auto* target : {contextRow, bucket}) {
        ProjectItem* item = target == contextRow ? contextLayer_ : nullptr;
        panel_->prepareItemContextMenu(target, item);
        panel_->treeWidget()->collapseAll();
        otherBucket->setExpanded(true);
        panel_->prepareItemContextMenu(target, item);
        ASSERT_FALSE(bucket->isExpanded());
        const auto selection = panel_->selectedItems();
        auto* operation = panel_->menuAction(QStringLiteral("Operation"));
        ASSERT_NE(operation, nullptr);
        ASSERT_NE(operation->menu(), nullptr);
        for (auto* action : operation->menu()->actions()) {
            if (action->text() == QStringLiteral("Collapse All"))
                EXPECT_FALSE(action->isEnabled());
            if (action->text() == QStringLiteral("Expand All"))
                action->trigger();
        }
        EXPECT_TRUE(contextRow->isExpanded());
        EXPECT_TRUE(containerRow->isExpanded());
        EXPECT_EQ(selectedRow->isExpanded(), target == bucket);
        EXPECT_EQ(bucket->isExpanded(), target == bucket);
        EXPECT_TRUE(otherBucket->isExpanded());
        EXPECT_EQ(panel_->selectedItems(), selection);

        panel_->prepareItemContextMenu(target, item);
        selectedRow->setExpanded(true);
        for (auto* action : operation->menu()->actions()) {
            if (action->text() == QStringLiteral("Expand All"))
                EXPECT_FALSE(action->isEnabled());
            if (action->text() == QStringLiteral("Collapse All"))
                action->trigger();
        }
        EXPECT_FALSE(contextRow->isExpanded());
        EXPECT_FALSE(containerRow->isExpanded());
        EXPECT_EQ(selectedRow->isExpanded(), target == contextRow);
        EXPECT_FALSE(bucket->isExpanded());
        EXPECT_TRUE(otherBucket->isExpanded());
        EXPECT_EQ(panel_->selectedItems(), selection);
    }

    panel_->armMenuContext(sound);
    auto* operation = panel_->menuAction(QStringLiteral("Operation"));
    ASSERT_NE(operation, nullptr);
    EXPECT_TRUE(operation->isVisible());
    EXPECT_FALSE(operation->isEnabled());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->index(), undoIndex);
}

TEST_F(ProjectTreePanelAnchorTest, InvalidParentNameDoesNotLeaveAnEmptyUndoEntry)
{
    auto& pm = ProjectManager::instance();
    auto* sound = pm.createItem(contextLayer_, QStringLiteral("GroupTarget"), ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    UndoRedoManager::instance().clear();
    EXPECT_EQ(pm.createParentContainer({sound}, ProjectItem::Type::Container, QStringLiteral("Invalid/Name")), nullptr);
    EXPECT_EQ(sound->parent, contextLayer_);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}
