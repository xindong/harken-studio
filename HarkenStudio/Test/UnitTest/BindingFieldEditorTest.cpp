#include "UnitTest/StudioObjectIdTestSupport.h"
#include "ActionManager.h"
#include "BindingEditing.h"
#include "Components/VMixNode.h"
#include "Components/VVariable.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "QtControlSkin.h"
#include "UndoCommands.h"
#include "VariableSemantics.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/CurveEditor/CurveEditorWidget.h"
#include "Widgets/Inspector/BindingFieldDecorator.h"
#include "Widgets/Inspector/BindingFieldEditor.h"
#include "Widgets/ExternalEditor/ExternalEditorPanel.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/InspectorCustomizationRegistry.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Selector/SelectorWindow.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

VNX::ExternalEditorPanel* prepareExternalEditor(VNX::InspectorPanel& inspector)
{
    auto* editor = inspector.findChild<VNX::ExternalEditorPanel*>();
    if (!editor) {
        editor = new VNX::ExternalEditorPanel(&inspector);
        editor->setWindowFlag(Qt::Window);
        editor->resize(420, 600);
        QObject::connect(&inspector, &VNX::InspectorPanel::externalEditorRequested, editor,
            [editor](const VNX::ExternalEditorSelection& selection, bool addBinding) {
                editor->bindSelection(selection);
                editor->show();
                if (addBinding) editor->showBindingAddMenu();
            });
        QObject::connect(editor, &VNX::ExternalEditorPanel::navigateRequested, &inspector,
            [&inspector](const QUuid& guid) {
                inspector.showItemInspector(VNX::ProjectManager::instance().findItemByGuid(guid));
            }, Qt::QueuedConnection);
    }
    return editor;
}

VNX::BindingFieldEditor* openField(VNX::InspectorPanel& inspector, const QString& field)
{
    prepareExternalEditor(inspector);
    auto* slot = inspector.findChild<QToolButton*>(QStringLiteral("bindingPoint_") + field);
    EXPECT_NE(slot, nullptr);
    if (!slot) return nullptr;
    slot->click();
    return inspector.findChild<VNX::BindingFieldEditor*>(
        QStringLiteral("bindingFieldEditor_") + field);
}

void expectSelectedBinding(VNX::ExternalEditorPanel* editor, VNX::ProjectItem* owner,
                           const VNX::ParameterBindingDef& binding)
{
    ASSERT_NE(editor, nullptr);
    EXPECT_TRUE(editor->isVisible());
    EXPECT_EQ(editor->boundItem(), owner);
    EXPECT_EQ(editor->selection().kind, VNX::ExternalEditorSelection::Kind::Binding);
    EXPECT_EQ(editor->selection().bindingAddress, binding.address);
    EXPECT_EQ(editor->selection().curve.bindingStableId, binding.runtimeId());
    auto* curve = editor->findChild<VNX::CurveEditorWidget*>();
    ASSERT_NE(curve, nullptr);
    EXPECT_TRUE(curve->hasBinding());
    EXPECT_EQ(curve->bindingHostGuid(), owner->guid);
    EXPECT_EQ(curve->bindingAddress(), binding.address);
    EXPECT_EQ(curve->bindingStableId(), binding.runtimeId());
    EXPECT_TRUE(curve->canvas()->isVisible());
    EXPECT_TRUE(curve->canvas()->isEnabled());
    int selectedRows = 0;
    for (auto* row : editor->findChildren<QWidget*>("listRow")) {
        if (!row->property("selected").toBool()) continue;
        ++selectedRows;
        EXPECT_EQ(row->property("bindingStableId").value<harkenstudio::ObjectId>(), binding.runtimeId());
    }
    EXPECT_EQ(selectedRows, 1);
}

VNX::SelectorWindow* activeSelector()
{
    for (QWidget* widget : QApplication::topLevelWidgets())
        if (auto* selector = qobject_cast<VNX::SelectorWindow*>(widget); selector && selector->isVisible())
            return selector;
    return nullptr;
}

VNX::SelectorWindow* openSelector(VNX::ObjectRefWidget* reference)
{
    if (!reference) return nullptr;
    auto* pick = reference->findChild<QToolButton*>(QStringLiteral("refPick"));
    if (!pick) return nullptr;
    pick->click();
    return activeSelector();
}

VNX::SelectorWindow* openSelector(QToolButton* button)
{
    if (!button) return nullptr;
    button->click();
    auto* menu = button->findChild<QMenu*>(QStringLiteral("bindingAddMenu"));
    if (!menu) return nullptr;
    auto* action = menu->findChild<QAction*>(QStringLiteral("bindingSelectVariable"));
    if (!action) return nullptr;
    menu->setActiveAction(action);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(menu, &enter);
    QApplication::processEvents();
    return activeSelector();
}

VNX::SelectorWindow* openSelector(VNX::BindingFieldEditor* popup)
{
    return openSelector(popup->findChild<QToolButton*>(QStringLiteral("bindingAdd")));
}

bool createVariable(QToolButton* button, VNX::VariableKind kind)
{
    if (!button) return false;
    button->click();
    auto* menu = button->findChild<QMenu*>(QStringLiteral("bindingAddMenu"));
    if (!menu) return false;
    auto* submenu = menu->findChild<QMenu*>(QStringLiteral("bindingNewVariable"));
    if (!submenu) return false;
    menu->hide();
    submenu->actions().at(kind == VNX::VariableKind::Step ? 1 : 0)->trigger();
    QApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QApplication::processEvents();
    return true;
}

bool chooseVariable(VNX::SelectorWindow* selector, uint32_t objectId)
{
    if (!selector) return false;
    auto* tree = selector->findChild<QTreeWidget*>();
    if (!tree) return false;
    QTreeWidgetItemIterator it(tree);
    while (*it) {
        if ((*it)->data(0, VNX::kRoleObjectID).toUInt() == objectId
            && (*it)->data(0, VNX::kRoleSelectable).toBool()) {
            tree->setCurrentItem(*it);
            selector->accept();
            QApplication::processEvents();
            return true;
        }
        ++it;
    }
    return false;
}

bool chooseVariable(VNX::ObjectRefWidget* reference, uint32_t objectId)
{
    return chooseVariable(openSelector(reference), objectId);
}

bool chooseVariable(VNX::BindingFieldEditor* popup, uint32_t objectId)
{
    return chooseVariable(openSelector(popup), objectId);
}

bool chooseVariable(QToolButton* button, uint32_t objectId)
{
    return chooseVariable(openSelector(button), objectId);
}

}

class BindingFieldEditorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerBindingActions(VNX::ActionManager::instance());
        VNX::registerBuiltinInspectorCustomizations();
        auto& manager = VNX::ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        ASSERT_TRUE(temp_.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("BindingFieldEditor");
        config.location = temp_.path();
        ASSERT_TRUE(manager.createProject(config));
        sound_ = manager.createItem(nullptr, QStringLiteral("BoundSound"), VNX::ProjectItem::Type::Sound);
        ASSERT_NE(sound_, nullptr);
        VNX::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        VNX::SelectorWindow::dismiss();
        VNX::UndoRedoManager::instance().clear();
        auto& manager = VNX::ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
        QApplication::processEvents();
    }

    VNX::ProjectItem* variable(const QString& name = QStringLiteral("Drive"))
    {
        return VNX::ProjectManager::instance().createItem(
            nullptr, name, VNX::ProjectItem::Type::Variable);
    }

    void add(VNX::ProjectItem* owner, VNX::ProjectItem* source, const VNX::BindingAddress& address)
    {
        ASSERT_TRUE(VNX::BindingEditing::upsert(
            owner, VNX::BindingSemantics::defaultBinding(address, source->guid)));
    }

    QTemporaryDir temp_;
    VNX::ProjectItem* sound_ = nullptr;
};

TEST_F(BindingFieldEditorTest, InitialAndCanceledSelectionStayEmptyWhileNewBindingsAreSelected)
{
    const auto address = VNX::BindingSemantics::volumeAddress();
    auto* source = variable();
    add(sound_, source, address);
    VNX::BindingFieldEditor editor;
    editor.bind(sound_, address);
    editor.show();
    auto* remove = editor.findChild<QToolButton*>("bindingRemoveSelected");
    auto* table = editor.findChild<VNX::TableWidget*>();
    ASSERT_NE(remove, nullptr);
    ASSERT_NE(table, nullptr);
    EXPECT_FALSE(remove->isEnabled());
    const auto first = sound_->comp<VNX::VMixNode>()->bindings.findFirst(address)->runtimeId();
    editor.selectBinding(sound_->guid, first);
    EXPECT_TRUE(remove->isEnabled());
    editor.bind(sound_, address);
    EXPECT_TRUE(remove->isEnabled());
    table->clearSelection();
    EXPECT_FALSE(remove->isEnabled());
    editor.bind(sound_, address);
    EXPECT_FALSE(remove->isEnabled());
    EXPECT_TRUE(chooseVariable(&editor, source->objectID));
    EXPECT_TRUE(remove->isEnabled());
    const auto bindings = sound_->comp<VNX::VMixNode>()->bindings.findAll(address);
    ASSERT_EQ(bindings.size(), 2u);
    bool selectedNew = false;
    for (auto* row : editor.findChildren<QWidget*>("listRow")) {
        if (!row->property("selected").toBool()) continue;
        selectedNew = true;
        EXPECT_EQ(row->property("bindingStableId").value<harkenstudio::ObjectId>(), bindings.back()->runtimeId());
    }
    EXPECT_TRUE(selectedNew);
    remove->click();
    QApplication::processEvents();
    EXPECT_TRUE(remove->isEnabled());
    EXPECT_EQ(sound_->comp<VNX::VMixNode>()->bindings.findAll(address).size(), 1u);
    const auto* remaining = editor.findChild<QWidget*>("listRow");
    ASSERT_NE(remaining, nullptr);
    EXPECT_TRUE(remaining->property("selected").toBool());
    EXPECT_EQ(remaining->property("bindingStableId").value<harkenstudio::ObjectId>(), first);
}

TEST_F(BindingFieldEditorTest, ExternalEditorContainsOnlyBindingsForItsField)
{
    auto* drive = variable();
    auto* extra = variable(QStringLiteral("Extra"));
    add(sound_, drive, VNX::BindingSemantics::volumeAddress());
    add(sound_, extra, VNX::BindingSemantics::volumeAddress());
    add(sound_, drive, VNX::BindingSemantics::pitchAddress());
    VNX::InspectorPanel inspector;
    inspector.showItemInspector(sound_);
    auto* popup = openField(inspector, QStringLiteral("volume"));
    ASSERT_NE(popup, nullptr);
    EXPECT_EQ(popup->findChildren<VNX::ObjectRefWidget*>(QStringLiteral("bindingVariableRef")).size(), 2);
    EXPECT_EQ(popup->property("bindingAddress").toString(),
              VNX::BindingSemantics::addressKey(VNX::BindingSemantics::volumeAddress()));
}

TEST_F(BindingFieldEditorTest, CancelledOrEmptyVariableSelectionDoesNotCreateBinding)
{
    variable();
    auto* undo = VNX::UndoRedoManager::instance().undoStack();
    const int undoIndex = undo->index();
    const int undoCount = undo->count();
    VNX::InspectorPanel inspector;
    int editorRequests = 0;
    QObject::connect(&inspector, &VNX::InspectorPanel::externalEditorRequested, &inspector,
        [&editorRequests] { ++editorRequests; });
    inspector.showItemInspector(sound_);
    auto* editor = prepareExternalEditor(inspector);
    auto* slot = inspector.findChild<QToolButton*>(QStringLiteral("bindingPoint_volume"));
    ASSERT_NE(slot, nullptr);
    auto* selector = openSelector(slot);
    ASSERT_NE(selector, nullptr);
    EXPECT_EQ(editorRequests, 0);
    EXPECT_FALSE(editor->isVisible());
    selector->reject();
    QApplication::processEvents();
    EXPECT_TRUE(sound_->comp<VNX::VMixNode>()->bindings.isEmpty());
    EXPECT_EQ(undo->index(), undoIndex);
    EXPECT_EQ(undo->count(), undoCount);
    selector = openSelector(slot);
    ASSERT_NE(selector, nullptr);
    selector->findChild<QTreeWidget*>()->setCurrentItem(nullptr);
    selector->accept();
    QApplication::processEvents();
    EXPECT_TRUE(sound_->comp<VNX::VMixNode>()->bindings.isEmpty());
    EXPECT_EQ(undo->index(), undoIndex);
    EXPECT_EQ(undo->count(), undoCount);
    EXPECT_EQ(editorRequests, 0);
    EXPECT_FALSE(editor->isVisible());
    EXPECT_TRUE(editor->selection().isEmpty());
}

TEST_F(BindingFieldEditorTest, ExplicitVariableSelectionCreatesFieldBindingWithUndoRedo)
{
    variable(QStringLiteral("A_First"));
    auto* selected = variable(QStringLiteral("Z_Selected"));
    VNX::InspectorPanel inspector;
    int editorRequests = 0;
    QObject::connect(&inspector, &VNX::InspectorPanel::externalEditorRequested, &inspector,
        [&editorRequests] { ++editorRequests; });
    inspector.showItemInspector(sound_);
    auto* editor = prepareExternalEditor(inspector);
    auto* slot = inspector.findChild<QToolButton*>(QStringLiteral("bindingPoint_pitch"));
    ASSERT_NE(slot, nullptr);
    ASSERT_TRUE(chooseVariable(slot, selected->objectID));
    ASSERT_EQ(sound_->comp<VNX::VMixNode>()->bindings.size(), 1u);
    const auto binding = sound_->comp<VNX::VMixNode>()->bindings.entries().front();
    EXPECT_EQ(binding.address, VNX::BindingSemantics::pitchAddress());
    EXPECT_EQ(binding.variable, selected->guid);
    EXPECT_EQ(editorRequests, 1);
    expectSelectedBinding(editor, sound_, binding);
    EXPECT_EQ(slot->property("bindingState").toString(), QStringLiteral("own"));
    VNX::UndoRedoManager::instance().undoStack()->undo();
    QApplication::processEvents();
    EXPECT_TRUE(sound_->comp<VNX::VMixNode>()->bindings.isEmpty());
    EXPECT_EQ(slot->property("bindingState").toString(), QStringLiteral("none"));
    VNX::UndoRedoManager::instance().undoStack()->redo();
    QApplication::processEvents();
    EXPECT_EQ(sound_->comp<VNX::VMixNode>()->bindings.entries().front().runtimeId(),
              binding.runtimeId());
    EXPECT_EQ(editorRequests, 1);
    auto* popup = openField(inspector, QStringLiteral("pitch"));
    ASSERT_NE(popup, nullptr);
    EXPECT_EQ(editorRequests, 2);
    EXPECT_TRUE(popup->isVisible());
    EXPECT_EQ(popup->findChildren<VNX::ObjectRefWidget*>(QStringLiteral("bindingVariableRef")).size(), 1);
}

TEST_F(BindingFieldEditorTest, NewVariableMenuOpensStepBindingForEditingWithoutConfirmation)
{
    VNX::InspectorPanel inspector;
    int editorRequests = 0;
    QObject::connect(&inspector, &VNX::InspectorPanel::externalEditorRequested, &inspector,
        [&editorRequests] { ++editorRequests; });
    inspector.showItemInspector(sound_);
    auto* editor = prepareExternalEditor(inspector);
    auto* slot = inspector.findChild<QToolButton*>(QStringLiteral("bindingPoint_volume"));
    ASSERT_NE(slot, nullptr);
    ASSERT_TRUE(createVariable(slot, VNX::VariableKind::Step));
    EXPECT_EQ(activeSelector(), nullptr);
    const auto variables = VNX::VariableSemantics::fileBoundVariables(
        VNX::ProjectManager::instance().currentProject());
    ASSERT_EQ(variables.size(), 1u);
    const auto* declaration = variables.front()->comp<VNX::VVariable>();
    ASSERT_NE(declaration, nullptr);
    EXPECT_EQ(declaration->kind, VNX::VariableKind::Step);
    ASSERT_EQ(sound_->comp<VNX::VMixNode>()->bindings.size(), 1u);
    EXPECT_EQ(sound_->comp<VNX::VMixNode>()->bindings.entries().front().variable,
              variables.front()->guid);
    EXPECT_EQ(slot->property("bindingState").toString(), QStringLiteral("own"));
    EXPECT_EQ(editorRequests, 1);
    expectSelectedBinding(editor, sound_, sound_->comp<VNX::VMixNode>()->bindings.entries().front());
}

TEST_F(BindingFieldEditorTest, NewVariableMenuCreatesAndSelectsSmoothBindingsWithExistingVariables)
{
    auto* existing = variable();
    VNX::InspectorPanel inspector;
    int editorRequests = 0;
    QObject::connect(&inspector, &VNX::InspectorPanel::externalEditorRequested, &inspector,
        [&editorRequests] { ++editorRequests; });
    inspector.showItemInspector(sound_);
    auto* editor = prepareExternalEditor(inspector);
    auto* slot = inspector.findChild<QToolButton*>(QStringLiteral("bindingPoint_volume"));
    ASSERT_NE(slot, nullptr);
    ASSERT_TRUE(createVariable(slot, VNX::VariableKind::Smooth));
    EXPECT_EQ(activeSelector(), nullptr);
    EXPECT_EQ(VNX::VariableSemantics::fileBoundVariables(
        VNX::ProjectManager::instance().currentProject()).size(), 2u);
    const auto* node = sound_->comp<VNX::VMixNode>();
    ASSERT_EQ(node->bindings.size(), 1u);
    const auto firstVariable = node->bindings.entries().front().variable;
    EXPECT_NE(firstVariable, existing->guid);
    const auto* created = VNX::ProjectManager::instance().findItemByGuid(firstVariable);
    ASSERT_NE(created, nullptr);
    ASSERT_NE(created->comp<VNX::VVariable>(), nullptr);
    EXPECT_EQ(created->comp<VNX::VVariable>()->kind, VNX::VariableKind::Smooth);
    EXPECT_EQ(slot->property("bindingState").toString(), QStringLiteral("own"));
    EXPECT_EQ(editorRequests, 1);
    expectSelectedBinding(editor, sound_, node->bindings.entries().front());
    auto* popup = editor->findChild<VNX::BindingFieldEditor*>();
    ASSERT_NE(popup, nullptr);
    ASSERT_TRUE(createVariable(popup->findChild<QToolButton*>(QStringLiteral("bindingAdd")),
                               VNX::VariableKind::Smooth));
    EXPECT_EQ(activeSelector(), nullptr);
    EXPECT_EQ(editorRequests, 1);
    ASSERT_EQ(node->bindings.size(), 2u);
    const auto& added = node->bindings.entries().back();
    EXPECT_NE(added.variable, firstVariable);
    EXPECT_NE(added.variable, existing->guid);
    EXPECT_TRUE(popup->findChild<QToolButton*>(QStringLiteral("bindingRemoveSelected"))->isEnabled());
    expectSelectedBinding(editor, sound_, added);
}

TEST_F(BindingFieldEditorTest, ChangingVariableKeepsBindingIdentityAndCurve)
{
    auto* drive = variable();
    auto* extra = variable(QStringLiteral("Extra"));
    auto binding = VNX::BindingSemantics::defaultBinding(VNX::BindingSemantics::volumeAddress(), drive->guid);
    binding.curve = {{0.0f, -12.0f}, {0.25f, -3.0f}, {1.0f, 0.0f}};
    ASSERT_TRUE(VNX::BindingEditing::upsert(sound_, binding));
    const auto before = sound_->comp<VNX::VMixNode>()->bindings.entries().front();
    VNX::InspectorPanel inspector;
    inspector.showItemInspector(sound_);
    auto* popup = openField(inspector, QStringLiteral("volume"));
    ASSERT_NE(popup, nullptr);
    auto* reference = popup->findChild<VNX::ObjectRefWidget*>(QStringLiteral("bindingVariableRef"));
    auto* thumbnail = popup->findChild<QWidget*>(QStringLiteral("bindingCurveThumb"));
    auto* table = popup->findChild<VNX::TableWidget*>();
    ASSERT_NE(reference, nullptr);
    ASSERT_NE(thumbnail, nullptr);
    ASSERT_NE(table, nullptr);
    auto* external = prepareExternalEditor(inspector);
    external->setStyleSheet(VNX::QtControlSkin::current().applicationStyleSheet());
    external->resize(420, 600);
    QApplication::processEvents();
    table->refreshLayout();
    QApplication::processEvents();
    EXPECT_LT(reference->mapToGlobal(QPoint(reference->width(), 0)).x(),
              thumbnail->mapToGlobal(QPoint()).x());
    ASSERT_TRUE(chooseVariable(reference, extra->objectID));
    const auto after = sound_->comp<VNX::VMixNode>()->bindings.entries().front();
    EXPECT_EQ(after.runtimeId(), before.runtimeId());
    EXPECT_EQ(after.variable, extra->guid);
    ASSERT_EQ(after.curve.size(), before.curve.size());
    EXPECT_EQ(after.curve[1].x, before.curve[1].x);
    EXPECT_EQ(after.curve[1].value, before.curve[1].value);
    VNX::UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(sound_->comp<VNX::VMixNode>()->bindings.entries().front().variable, drive->guid);
}

TEST_F(BindingFieldEditorTest, RemoveTargetsOnlySelectedBindingAndUndoRestoresIt)
{
    auto* drive = variable();
    auto* extra = variable(QStringLiteral("Extra"));
    auto* last = variable(QStringLiteral("Last"));
    add(sound_, drive, VNX::BindingSemantics::volumeAddress());
    add(sound_, extra, VNX::BindingSemantics::volumeAddress());
    add(sound_, last, VNX::BindingSemantics::volumeAddress());
    add(sound_, drive, VNX::BindingSemantics::pitchAddress());
    const auto bindings = sound_->comp<VNX::VMixNode>()->bindings.findAll(VNX::BindingSemantics::volumeAddress());
    const auto firstBinding = *bindings.front();
    const auto lastBinding = *bindings.back();
    VNX::InspectorPanel inspector;
    inspector.showItemInspector(sound_);
    auto* popup = openField(inspector, QStringLiteral("volume"));
    ASSERT_NE(popup, nullptr);
    popup->selectRow(1);
    auto* remove = popup->findChild<QToolButton*>(QStringLiteral("bindingRemoveSelected"));
    ASSERT_NE(remove, nullptr);
    remove->click();
    QApplication::processEvents();
    EXPECT_EQ(sound_->comp<VNX::VMixNode>()->bindings.size(), 3u);
    auto* external = inspector.findChild<VNX::ExternalEditorPanel*>();
    expectSelectedBinding(external, sound_, lastBinding);
    EXPECT_TRUE(remove->isEnabled());
    remove->click();
    QApplication::processEvents();
    EXPECT_EQ(sound_->comp<VNX::VMixNode>()->bindings.size(), 2u);
    EXPECT_EQ(sound_->comp<VNX::VMixNode>()->bindings.findAll(VNX::BindingSemantics::volumeAddress()).size(), 1u);
    EXPECT_EQ(sound_->comp<VNX::VMixNode>()->bindings.findAll(VNX::BindingSemantics::pitchAddress()).size(), 1u);
    const auto numbers = popup->findChildren<QLabel*>(QStringLiteral("bindingDragHandle"));
    ASSERT_EQ(numbers.size(), 1);
    EXPECT_EQ(numbers.front()->text(), QStringLiteral("1"));
    expectSelectedBinding(external, sound_, firstBinding);
    EXPECT_TRUE(remove->isEnabled());
    remove->click();
    QApplication::processEvents();
    EXPECT_TRUE(sound_->comp<VNX::VMixNode>()->bindings.findAll(VNX::BindingSemantics::volumeAddress()).empty());
    EXPECT_EQ(sound_->comp<VNX::VMixNode>()->bindings.findAll(VNX::BindingSemantics::pitchAddress()).size(), 1u);
    EXPECT_FALSE(remove->isEnabled());
    EXPECT_FALSE(external->findChild<VNX::CurveEditorWidget*>()->hasBinding());
    VNX::UndoRedoManager::instance().undoStack()->undo();
    QApplication::processEvents();
    EXPECT_EQ(popup->findChildren<VNX::ObjectRefWidget*>(QStringLiteral("bindingVariableRef")).size(), 1);
    EXPECT_EQ(sound_->comp<VNX::VMixNode>()->bindings.findFirst(VNX::BindingSemantics::volumeAddress())->runtimeId(),
              firstBinding.runtimeId());
}

TEST_F(BindingFieldEditorTest, DragReordersOnlyTheCurrentFieldAndSupportsUndo)
{
    auto* drive = variable();
    auto* extra = variable(QStringLiteral("Extra"));
    add(sound_, drive, VNX::BindingSemantics::volumeAddress());
    add(sound_, drive, VNX::BindingSemantics::pitchAddress());
    add(sound_, extra, VNX::BindingSemantics::volumeAddress());
    const auto before = sound_->comp<VNX::VMixNode>()->bindings.findAll(VNX::BindingSemantics::volumeAddress());
    const harkenstudio::ObjectId firstId = before[0]->runtimeId();
    const harkenstudio::ObjectId secondId = before[1]->runtimeId();
    VNX::InspectorPanel inspector;
    inspector.showItemInspector(sound_);
    auto* popup = openField(inspector, QStringLiteral("volume"));
    ASSERT_NE(popup, nullptr);
    QApplication::processEvents();
    const auto handles = popup->findChildren<QLabel*>(QStringLiteral("bindingDragHandle"));
    ASSERT_EQ(handles.size(), 2);
    const QPoint start = handles[1]->mapToGlobal(QPoint(2, 2));
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(2, 2), QPointF(start),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(handles[1], &press);
    const QPoint finish = handles[0]->mapToGlobal(QPoint(2, 1));
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(popup->mapFromGlobal(finish)), QPointF(finish),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(popup, &release);
    QApplication::processEvents();
    auto after = sound_->comp<VNX::VMixNode>()->bindings.findAll(VNX::BindingSemantics::volumeAddress());
    ASSERT_EQ(after.size(), 2u);
    EXPECT_EQ(after[0]->runtimeId(), secondId);
    EXPECT_EQ(after[1]->runtimeId(), firstId);
    EXPECT_EQ(sound_->comp<VNX::VMixNode>()->bindings.findAll(VNX::BindingSemantics::pitchAddress()).size(), 1u);
    VNX::UndoRedoManager::instance().undoStack()->undo();
    after = sound_->comp<VNX::VMixNode>()->bindings.findAll(VNX::BindingSemantics::volumeAddress());
    EXPECT_EQ(after[0]->runtimeId(), firstId);
}

TEST_F(BindingFieldEditorTest, ReloadDuringDragDoesNotReorderAChangedRow)
{
    auto* drive = variable();
    auto* extra = variable(QStringLiteral("Extra"));
    const auto address = VNX::BindingSemantics::volumeAddress();
    add(sound_, drive, address);
    add(sound_, extra, address);
    const auto before = sound_->comp<VNX::VMixNode>()->bindings.findAll(address);
    const harkenstudio::ObjectId firstId = before[0]->runtimeId();
    const harkenstudio::ObjectId secondId = before[1]->runtimeId();
    VNX::InspectorPanel inspector;
    inspector.showItemInspector(sound_);
    auto* popup = openField(inspector, QStringLiteral("volume"));
    ASSERT_NE(popup, nullptr);
    QApplication::processEvents();
    auto handles = popup->findChildren<QLabel*>(QStringLiteral("bindingDragHandle"));
    ASSERT_EQ(handles.size(), 2);
    const QPoint start = handles[1]->mapToGlobal(QPoint(2, 2));
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(2, 2), QPointF(start),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(handles[1], &press);

    ASSERT_TRUE(VNX::BindingEditing::reorder(sound_, address, 0, 1));
    auto* undo = VNX::UndoRedoManager::instance().undoStack();
    const int undoIndex = undo->index();
    handles = popup->findChildren<QLabel*>(QStringLiteral("bindingDragHandle"));
    ASSERT_EQ(handles.size(), 2);
    const QPoint finish = handles[0]->mapToGlobal(QPoint(2, 1));
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(popup->mapFromGlobal(finish)), QPointF(finish),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(popup, &release);
    QApplication::processEvents();
    const auto after = sound_->comp<VNX::VMixNode>()->bindings.findAll(address);
    ASSERT_EQ(after.size(), 2u);
    EXPECT_EQ(after[0]->runtimeId(), secondId);
    EXPECT_EQ(after[1]->runtimeId(), firstId);
    EXPECT_EQ(undo->index(), undoIndex);
}

TEST_F(BindingFieldEditorTest, AncestorBindingDoesNotFillTheChildSlotOrEditor)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* layer = manager.createItem(nullptr, QStringLiteral("Owner"), VNX::ProjectItem::Type::Layer);
    auto* child = manager.createItem(layer, QStringLiteral("Child"), VNX::ProjectItem::Type::Sound);
    auto* drive = variable();
    auto* extra = variable(QStringLiteral("Extra"));
    add(layer, drive, VNX::BindingSemantics::pitchAddress());
    VNX::InspectorPanel inspector;
    int editorRequests = 0;
    QObject::connect(&inspector, &VNX::InspectorPanel::externalEditorRequested, &inspector,
        [&editorRequests] { ++editorRequests; });
    inspector.showItemInspector(child);
    auto* editor = prepareExternalEditor(inspector);
    auto* slot = inspector.findChild<VNX::BindingFieldDecorator*>(QStringLiteral("bindingPoint_pitch"));
    ASSERT_NE(slot, nullptr);
    EXPECT_FALSE(slot->isBound());
    EXPECT_FALSE(slot->toolTip().contains(drive->name));
    ASSERT_TRUE(chooseVariable(slot, extra->objectID));
    EXPECT_EQ(editorRequests, 1);
    EXPECT_TRUE(slot->isBound());
    ASSERT_EQ(child->comp<VNX::VMixNode>()->bindings.size(), 1u);
    expectSelectedBinding(editor, child, child->comp<VNX::VMixNode>()->bindings.entries().front());
    auto* popup = editor->findChild<VNX::BindingFieldEditor*>();
    ASSERT_NE(popup, nullptr);
    const auto refs = popup->findChildren<VNX::ObjectRefWidget*>(QStringLiteral("bindingVariableRef"));
    ASSERT_EQ(refs.size(), 1);
    EXPECT_EQ(refs.front()->currentObjectID(), extra->objectID);
    EXPECT_FALSE(refs.front()->isReadOnly());
    EXPECT_FALSE(refs.front()->isInherited());
    EXPECT_TRUE(popup->findChild<QToolButton*>(QStringLiteral("bindingRemoveSelected"))->isEnabled());
    EXPECT_TRUE(popup->findChild<QToolButton*>(QStringLiteral("bindingAdd"))->isEnabled());
    EXPECT_EQ(layer->comp<VNX::VMixNode>()->bindings.size(), 1u);
    EXPECT_EQ(child->comp<VNX::VMixNode>()->bindings.size(), 1u);
}

TEST_F(BindingFieldEditorTest, RemovingLastLocalBindingEmptiesTheSlotDespiteAncestorBinding)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* layer = manager.createItem(nullptr, QStringLiteral("Owner"), VNX::ProjectItem::Type::Layer);
    auto* child = manager.createItem(layer, QStringLiteral("Child"), VNX::ProjectItem::Type::Sound);
    auto* drive = variable();
    const auto address = VNX::BindingSemantics::volumeAddress();
    add(layer, drive, address);
    add(child, drive, address);
    VNX::InspectorPanel inspector;
    inspector.showItemInspector(child);
    auto* popup = openField(inspector, QStringLiteral("volume"));
    ASSERT_NE(popup, nullptr);
    popup->selectRow(0);
    auto* remove = popup->findChild<QToolButton*>(QStringLiteral("bindingRemoveSelected"));
    ASSERT_NE(remove, nullptr);
    remove->click();
    QApplication::processEvents();
    EXPECT_TRUE(child->comp<VNX::VMixNode>()->bindings.isEmpty());
    EXPECT_EQ(layer->comp<VNX::VMixNode>()->bindings.size(), 1u);
    EXPECT_TRUE(popup->findChildren<VNX::ObjectRefWidget*>(QStringLiteral("bindingVariableRef")).isEmpty());
    auto* slot = inspector.findChild<VNX::BindingFieldDecorator*>(QStringLiteral("bindingPoint_volume"));
    ASSERT_NE(slot, nullptr);
    EXPECT_FALSE(slot->isBound());
    auto* selector = openSelector(slot);
    ASSERT_NE(selector, nullptr);
    selector->reject();
    VNX::UndoRedoManager::instance().undoStack()->undo();
    QApplication::processEvents();
    EXPECT_TRUE(slot->isBound());
    EXPECT_EQ(layer->comp<VNX::VMixNode>()->bindings.size(), 1u);
    EXPECT_EQ(child->comp<VNX::VMixNode>()->bindings.size(), 1u);
    EXPECT_EQ(popup->findChildren<VNX::ObjectRefWidget*>(QStringLiteral("bindingVariableRef")).size(), 1);
}

TEST_F(BindingFieldEditorTest, RehearsalLeavesInspectorUnchangedAndRenameRefreshesReference)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* drive = variable();
    drive->comp<VNX::VVariable>()->rangeMin = 0.0f;
    drive->comp<VNX::VVariable>()->rangeMax = 1.0f;
    add(sound_, drive, VNX::BindingSemantics::volumeAddress());
    QApplication::processEvents();
    VNX::InspectorPanel inspector;
    inspector.showItemInspector(sound_);
    auto* popup = openField(inspector, QStringLiteral("volume"));
    ASSERT_NE(popup, nullptr);
    QPointer<VNX::ObjectRefWidget> reference = popup->findChild<VNX::ObjectRefWidget*>(QStringLiteral("bindingVariableRef"));
    ASSERT_NE(reference, nullptr);
    auto* slot = inspector.findChild<QToolButton*>(QStringLiteral("bindingPoint_volume"));
    ASSERT_NE(slot, nullptr);
    const QString before = slot->toolTip();
    manager.variableRehearsal().setOverride(drive->guid, 1.0f);
    QApplication::processEvents();
    EXPECT_NE(reference, nullptr);
    EXPECT_EQ(slot->toolTip(), before);
    ASSERT_TRUE(manager.renameItem(drive, QStringLiteral("RenamedDrive")));
    QApplication::processEvents();
    ASSERT_NE(reference, nullptr);
    EXPECT_EQ(reference->findChild<QLabel*>(QStringLiteral("refName"))->text(), QStringLiteral("RenamedDrive"));
}

TEST_F(BindingFieldEditorTest, OnlyLocalBindingNotificationsChangeTheEditor)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* layer = manager.createItem(nullptr, QStringLiteral("Owner"), VNX::ProjectItem::Type::Layer);
    auto* child = manager.createItem(layer, QStringLiteral("Child"), VNX::ProjectItem::Type::Sound);
    auto* drive = variable();
    auto* extra = variable(QStringLiteral("Extra"));
    add(layer, drive, VNX::BindingSemantics::volumeAddress());
    add(child, extra, VNX::BindingSemantics::volumeAddress());
    VNX::InspectorPanel inspector;
    inspector.showItemInspector(child);
    auto* popup = openField(inspector, QStringLiteral("volume"));
    ASSERT_NE(popup, nullptr);
    add(layer, extra, VNX::BindingSemantics::volumeAddress());
    QApplication::processEvents();
    EXPECT_EQ(popup->findChildren<VNX::ObjectRefWidget*>(QStringLiteral("bindingVariableRef")).size(), 1);
    auto binding = VNX::BindingSemantics::defaultBinding(VNX::BindingSemantics::volumeAddress(), extra->guid);
    binding.guid = QUuid::createUuid();
    ASSERT_TRUE(child->comp<VNX::VMixNode>()->bindings.upsert(std::move(binding)));
    emit manager.itemPropertyChanged(child, QStringLiteral("bindings"));
    QApplication::processEvents();
    EXPECT_EQ(popup->findChildren<VNX::ObjectRefWidget*>(QStringLiteral("bindingVariableRef")).size(), 2);
}

TEST_F(BindingFieldEditorTest, BusVolumeUsesBusAddress)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* bus = manager.createItem(manager.getMasterBusItem(), QStringLiteral("BoundBus"), VNX::ProjectItem::Type::Bus);
    add(bus, variable(), VNX::BindingSemantics::volumeAddress(VNX::ProjectItemType::Bus));
    VNX::InspectorPanel inspector;
    inspector.showItemInspector(bus);
    auto* popup = openField(inspector, QStringLiteral("volume"));
    ASSERT_NE(popup, nullptr);
    EXPECT_EQ(popup->property("bindingAddress").toString(),
              VNX::BindingSemantics::addressKey(VNX::BindingSemantics::volumeAddress(VNX::ProjectItemType::Bus)));
}

TEST_F(BindingFieldEditorTest, SaveCloseOpenRestoresBindingsAtTheFieldSlot)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* drive = variable();
    add(sound_, drive, VNX::BindingSemantics::volumeAddress());
    const QUuid soundGuid = sound_->guid;
    const QUuid variableGuid = drive->guid;
    const QString path = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(path));
    auto* restored = manager.findItemByGuid(soundGuid);
    ASSERT_NE(restored, nullptr);
    VNX::InspectorPanel inspector;
    inspector.showItemInspector(restored);
    auto* popup = openField(inspector, QStringLiteral("volume"));
    ASSERT_NE(popup, nullptr);
    auto* reference = popup->findChild<VNX::ObjectRefWidget*>(QStringLiteral("bindingVariableRef"));
    ASSERT_NE(reference, nullptr);
    EXPECT_EQ(reference->currentObjectID(), manager.findItemByGuid(variableGuid)->objectID);
}

TEST_F(BindingFieldEditorTest, DisablingTheSlotDoesNotOwnTheExternalEditor)
{
    auto* drive = variable();
    ASSERT_NE(drive, nullptr);
    add(sound_, drive, VNX::BindingSemantics::volumeAddress());
    const int undoCount = VNX::UndoRedoManager::instance().undoStack()->count();
    VNX::InspectorPanel inspector;
    inspector.showItemInspector(sound_);
    auto* popup = openField(inspector, QStringLiteral("volume"));
    ASSERT_NE(popup, nullptr);
    QPointer<VNX::SelectorWindow> selector = openSelector(popup);
    ASSERT_NE(selector, nullptr);
    auto* slot = inspector.findChild<QToolButton*>(QStringLiteral("bindingPoint_volume"));
    ASSERT_NE(slot, nullptr);
    slot->setEnabled(false);
    QApplication::processEvents();
    EXPECT_TRUE(popup->isVisible());
    EXPECT_NE(activeSelector(), nullptr);
    if (selector) chooseVariable(selector, drive->objectID);
    EXPECT_EQ(sound_->comp<VNX::VMixNode>()->bindings.size(), 2u);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount + 1);
}

TEST_F(BindingFieldEditorTest, SwitchingObjectInvalidatesAnOpenVariableSelection)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* drive = variable();
    add(sound_, drive, VNX::BindingSemantics::volumeAddress());
    auto* other = manager.createItem(nullptr, QStringLiteral("OtherSound"), VNX::ProjectItem::Type::Sound);
    VNX::InspectorPanel inspector;
    inspector.showItemInspector(sound_);
    QPointer<VNX::BindingFieldEditor> popup = openField(inspector, QStringLiteral("volume"));
    ASSERT_NE(popup, nullptr);
    auto* selector = openSelector(popup.data());
    ASSERT_NE(selector, nullptr);
    inspector.findChild<VNX::ExternalEditorPanel*>()->bindSelection(
        VNX::ExternalEditorSelection::binding(other->guid, VNX::BindingSemantics::volumeAddress()));
    ASSERT_NE(popup, nullptr);
    EXPECT_TRUE(selector->isHidden());
    auto* tree = selector->findChild<QTreeWidget*>();
    ASSERT_NE(tree, nullptr);
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->data(0, VNX::kRoleObjectID).toUInt() == drive->objectID)
            tree->setCurrentItem(*it);
    selector->accept();
    QApplication::processEvents();
    EXPECT_EQ(sound_->comp<VNX::VMixNode>()->bindings.size(), 1u);
    EXPECT_TRUE(other->comp<VNX::VMixNode>()->bindings.isEmpty());
}

TEST_F(BindingFieldEditorTest, SwitchingInspectorObjectCancelsEmptySlotSelection)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* drive = variable();
    auto* other = manager.createItem(nullptr, QStringLiteral("OtherSound"), VNX::ProjectItem::Type::Sound);
    VNX::InspectorPanel inspector;
    int editorRequests = 0;
    QObject::connect(&inspector, &VNX::InspectorPanel::externalEditorRequested, &inspector,
        [&editorRequests] { ++editorRequests; });
    auto* editor = prepareExternalEditor(inspector);
    for (const bool create : {false, true}) {
        SCOPED_TRACE(create ? "New Variable" : "Select Variable");
        inspector.showItemInspector(sound_);
        auto* slot = inspector.findChild<QToolButton*>(QStringLiteral("bindingPoint_volume"));
        ASSERT_NE(slot, nullptr);
        slot->click();
        auto* menu = slot->findChild<QMenu*>(QStringLiteral("bindingAddMenu"));
        ASSERT_NE(menu, nullptr);
        auto* action = menu->findChild<QAction*>(QStringLiteral("bindingSelectVariable"));
        if (create) {
            auto* submenu = menu->findChild<QMenu*>(QStringLiteral("bindingNewVariable"));
            ASSERT_NE(submenu, nullptr);
            ASSERT_FALSE(submenu->actions().isEmpty());
            action = submenu->actions().front();
        }
        ASSERT_NE(action, nullptr);
        menu->hide();
        action->trigger();
        inspector.showItemInspector(other);
        QApplication::processEvents();
        EXPECT_EQ(activeSelector(), nullptr);
        EXPECT_EQ(VNX::VariableSemantics::fileBoundVariables(manager.currentProject()).size(), 1u);
        EXPECT_TRUE(sound_->comp<VNX::VMixNode>()->bindings.isEmpty());
        EXPECT_TRUE(other->comp<VNX::VMixNode>()->bindings.isEmpty());
        EXPECT_EQ(editorRequests, 0);
        EXPECT_FALSE(editor->isVisible());
    }
    inspector.showItemInspector(sound_);
    auto* slot = inspector.findChild<QToolButton*>(QStringLiteral("bindingPoint_volume"));
    ASSERT_NE(slot, nullptr);
    auto* selector = openSelector(slot);
    ASSERT_NE(selector, nullptr);
    inspector.showItemInspector(other);
    EXPECT_TRUE(selector->isHidden());
    chooseVariable(selector, drive->objectID);
    EXPECT_TRUE(sound_->comp<VNX::VMixNode>()->bindings.isEmpty());
    EXPECT_TRUE(other->comp<VNX::VMixNode>()->bindings.isEmpty());
    EXPECT_EQ(editorRequests, 0);
    EXPECT_FALSE(editor->isVisible());
    slot = inspector.findChild<QToolButton*>(QStringLiteral("bindingPoint_volume"));
    ASSERT_NE(slot, nullptr);
    ASSERT_TRUE(chooseVariable(slot, drive->objectID));
    EXPECT_TRUE(sound_->comp<VNX::VMixNode>()->bindings.isEmpty());
    ASSERT_EQ(other->comp<VNX::VMixNode>()->bindings.size(), 1u);
    EXPECT_EQ(other->comp<VNX::VMixNode>()->bindings.entries().front().variable, drive->guid);
    EXPECT_EQ(editorRequests, 1);
    expectSelectedBinding(editor, other, other->comp<VNX::VMixNode>()->bindings.entries().front());
}

TEST_F(BindingFieldEditorTest, DeletedVariableClearsBindingAndUndoRestoresItsEditor)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* drive = variable();
    add(sound_, drive, VNX::BindingSemantics::volumeAddress());
    const QUuid guid = drive->guid;
    VNX::InspectorPanel inspector;
    inspector.showItemInspector(sound_);
    auto* popup = openField(inspector, QStringLiteral("volume"));
    ASSERT_NE(popup, nullptr);
    emit manager.itemPropertyChanged(drive, QStringLiteral("defaultValue"));
    ASSERT_TRUE(manager.deleteItem(drive));
    QApplication::processEvents();
    EXPECT_EQ(popup->findChild<VNX::ObjectRefWidget*>(QStringLiteral("bindingVariableRef")), nullptr);
    EXPECT_TRUE(sound_->comp<VNX::VMixNode>()->bindings.isEmpty());
    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    QApplication::processEvents();
    auto* reference = popup->findChild<VNX::ObjectRefWidget*>(QStringLiteral("bindingVariableRef"));
    ASSERT_NE(reference, nullptr);
    ASSERT_EQ(sound_->comp<VNX::VMixNode>()->bindings.size(), 1u);
    EXPECT_EQ(sound_->comp<VNX::VMixNode>()->bindings.entries().front().variable, guid);
}

TEST_F(BindingFieldEditorTest, ClosingProjectCancelsAnOpenVariableSelection)
{
    add(sound_, variable(), VNX::BindingSemantics::volumeAddress());
    VNX::InspectorPanel inspector;
    inspector.showItemInspector(sound_);
    QPointer<VNX::BindingFieldEditor> popup = openField(inspector, QStringLiteral("volume"));
    ASSERT_NE(popup, nullptr);
    auto* selector = openSelector(popup.data());
    ASSERT_NE(selector, nullptr);
    ASSERT_TRUE(VNX::ProjectManager::instance().closeProject());
    EXPECT_TRUE(selector->isHidden());
    selector->accept();
    QApplication::processEvents();
    EXPECT_TRUE(popup.isNull() || !popup->isVisible());
    EXPECT_EQ(inspector.currentContextItem(), nullptr);
}
