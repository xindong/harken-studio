#include "Support/FieldGroupTestUtils.h"
#include "Core/ActionManager.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/PlayablePropertyAuthoringV1.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Core/QtAdsThemeAdapter.h"
#include "Core/ThemeManager.h"
#include "Core/UndoCommands.h"
#include "Widgets/Blueprint/BlueprintCanvasWidget.h"
#include "Widgets/Blueprint/BlueprintPanel.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Common/ParameterValueEditor.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/Inspector/InspectorTabContainer.h"

#include <DockManager.h>
#include <DockWidget.h>

#include <QApplication>
#include <QDoubleSpinBox>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLayout>
#include <QLineEdit>
#include <QMainWindow>
#include <QPointer>
#include <QSlider>
#include <QTemporaryDir>
#include <QVBoxLayout>

#include <gtest/gtest.h>

#include <memory>
#include "Support/ProjectPlatformTestUtils.h"

namespace {

void drainEvents()
{
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
}

class InspectorFocus : public testing::Test
{
protected:
    void SetUp() override
    {
        VNX::registerBuiltinInspectorCustomizations();
        previousFlags_ = ads::CDockManager::configFlags();
        ads::CDockManager::setConfigFlags(ads::CDockManager::DefaultOpaqueConfig);
        ads::CDockManager::setConfigFlag(ads::CDockManager::FocusHighlighting, true);
        auto& theme = VNX::ThemeManager::instance();
        VNX::QtAdsThemeAdapter::applyIcons(theme.assetResolver(), theme.snapshot());
        window_ = std::make_unique<QMainWindow>();
        window_->resize(1000, 700);
        manager_ = new ads::CDockManager(window_.get());

        auto* blueprint = new VNX::BlueprintPanel();
        canvas_ = blueprint->canvas();
        auto* blueprintDock = new ads::CDockWidget(QStringLiteral("Blueprint"));
        blueprintDock->setWidget(blueprint);
        manager_->addDockWidget(ads::LeftDockWidgetArea, blueprintDock);

        inspector_ = new VNX::InspectorPanel();
        inspectorDock_ = new ads::CDockWidget(QStringLiteral("Inspector"));
        inspectorDock_->setWidget(inspector_);
        manager_->addDockWidget(ads::RightDockWidgetArea, inspectorDock_);
        window_->show();
        window_->activateWindow();
        drainEvents();

        QObject::connect(manager_, &ads::CDockManager::focusedDockWidgetChanged,
                         window_.get(), [this](ads::CDockWidget* previous,
                                              ads::CDockWidget* current) {
            if (previous == inspectorDock_ && current != inspectorDock_)
                focusEscaped_ = true;
        });
    }

    void TearDown() override
    {
        window_.reset();
        drainEvents();
        ads::CDockManager::setConfigFlags(previousFlags_);
        if (projectCreated_) {
            VNX::UndoRedoManager::instance().clear();
            VNX::ActionManager::instance().clearAllForTesting();
            VNX::ProjectManager::instance().closeProject();
            VNX::PlatformContextService::instance().setStorageForTesting(
                VNX::SettingsStorageFactory::create(
                    VNX::SettingsStorageFactory::StorageType::QSettings,
                    QStringLiteral("HarkenSound"), QStringLiteral("HarkenStudio")));
        }
    }

    QLineEdit* addEditor()
    {
        auto* editor = new QLineEdit(inspector_);
        inspector_->layout()->addWidget(editor);
        editor->show();
        QWidget::setTabOrder(editor, canvas_);
        return editor;
    }

    void focusEditor(QWidget* editor)
    {
        editor->setFocus(Qt::OtherFocusReason);
        drainEvents();
        ASSERT_TRUE(editor->hasFocus());
        ASSERT_EQ(manager_->focusedDockWidget(), inspectorDock_);
        focusEscaped_ = false;
    }

    void expectInspectorFocus()
    {
        QWidget* focused = QApplication::focusWidget();
        EXPECT_TRUE(focused == inspector_ || inspector_->isAncestorOf(focused));
        EXPECT_EQ(manager_->focusedDockWidget(), inspectorDock_);
        EXPECT_FALSE(focusEscaped_);
    }

    ads::CDockManager::ConfigFlags previousFlags_;
    std::unique_ptr<QMainWindow> window_;
    ads::CDockManager* manager_ = nullptr;
    ads::CDockWidget* inspectorDock_ = nullptr;
    VNX::InspectorPanel* inspector_ = nullptr;
    VNX::BlueprintCanvasWidget* canvas_ = nullptr;
    QTemporaryDir projectDir_;
    bool projectCreated_ = false;
    bool focusEscaped_ = false;
};

TEST_F(InspectorFocus, HidingEditedRowKeepsFocusInsidePanel)
{
    auto* row = new QWidget(inspector_);
    auto* layout = new QVBoxLayout(row);
    auto* editor = new QLineEdit(row);
    layout->addWidget(editor);
    inspector_->layout()->addWidget(row);
    row->show();
    QWidget::setTabOrder(editor, canvas_);
    focusEditor(editor);

    row->hide();
    row->deleteLater();
    expectInspectorFocus();
    drainEvents();
    expectInspectorFocus();
}

TEST_F(InspectorFocus, DeletingFocusedEditorKeepsFocusInsidePanel)
{
    auto* editor = addEditor();
    focusEditor(editor);

    delete editor;
    expectInspectorFocus();
    drainEvents();
    expectInspectorFocus();
}

TEST_F(InspectorFocus, DisablingFocusedEditorKeepsFocusInsidePanel)
{
    auto* editor = addEditor();
    focusEditor(editor);

    editor->setEnabled(false);
    expectInspectorFocus();
}

TEST_F(InspectorFocus, DisablingEditedSectionKeepsFocusInsidePanel)
{
    auto* section = new QWidget(inspector_);
    auto* layout = new QVBoxLayout(section);
    auto* editor = new QLineEdit(section);
    layout->addWidget(editor);
    inspector_->layout()->addWidget(section);
    section->show();
    focusEditor(editor);

    section->setEnabled(false);
    expectInspectorFocus();
}

TEST_F(InspectorFocus, UserCanMoveFocusToBlueprint)
{
    auto* editor = addEditor();
    focusEditor(editor);

    canvas_->setFocus(Qt::MouseFocusReason);
    editor->hide();
    drainEvents();
    EXPECT_TRUE(canvas_->hasFocus());
    EXPECT_NE(manager_->focusedDockWidget(), inspectorDock_);
}

TEST_F(InspectorFocus, TabNavigationCanLeavePanel)
{
    auto* editor = addEditor();
    focusEditor(editor);

    QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
    QApplication::sendEvent(editor, &tab);
    drainEvents();
    EXPECT_TRUE(canvas_->hasFocus());
}

TEST_F(InspectorFocus, ClosingInspectorAllowsFocusToLeave)
{
    focusEditor(addEditor());

    inspectorDock_->toggleView(false);
    drainEvents();
    EXPECT_TRUE(inspectorDock_->isClosed());
    EXPECT_NE(manager_->focusedDockWidget(), inspectorDock_);
}

TEST_F(InspectorFocus, ActivatingAnotherWindowDoesNotRestoreInspectorFocus)
{
    focusEditor(addEditor());

    QWidget otherWindow;
    otherWindow.show();
    otherWindow.activateWindow();
    drainEvents();
    EXPECT_EQ(QApplication::activeWindow(), &otherWindow);
    EXPECT_FALSE(inspector_->hasFocus());
}

TEST_F(InspectorFocus, SendLevelCommitAndUndoKeepEditorFocusAndRowSelection)
{
    using VNX::PlayablePropertyAuthoringV1;
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    ASSERT_TRUE(projectDir_.isValid());
    VNX::PlatformContextService::instance().setStorageForTesting(
        std::make_unique<VNX::MemorySettingsStorage>());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("InspectorFocus");
    config.location = projectDir_.path();
    config.platforms = HarkenTest::platforms({QStringLiteral("Windows")});
    ASSERT_TRUE(pm.createProject(config));
    projectCreated_ = true;
    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    auto* sound = pm.createItem(nullptr, QStringLiteral("Sound"), VNX::ProjectItem::Type::Sound);
    auto* bus = pm.createItem(pm.getMasterBusItem(), QStringLiteral("SendBus"),
                              VNX::ProjectItem::Type::Bus);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(bus, nullptr);
    sound->fieldGroups.send.shared = {VNX::FieldGroupMode::Local,
        VNX::SendFieldGroup{{{VNX::toFieldGroupGuid(bus->guid), 0.0f}}}};
    auto& actions = VNX::ActionManager::instance();
    actions.clearAllForTesting();
    VNX::registerProjectActions(actions, &pm, nullptr, {});
    auto* undo = VNX::UndoRedoManager::instance().undoStack();
    undo->clear();
    ASSERT_TRUE(canvas_->focusItem(sound));
    inspector_->showItemInspector(sound);
    auto* tabs = inspector_->findChild<VNX::InspectorTabContainer*>();
    ASSERT_NE(tabs, nullptr);
    tabs->setCurrentTab(1);
    drainEvents();
    auto* sends = inspector_->findChild<QWidget*>(QStringLiteral("prop_sends"));
    ASSERT_NE(sends, nullptr);
    auto* row = sends->findChild<QWidget*>(QStringLiteral("listRow"));
    ASSERT_NE(row, nullptr);
    QPointer<VNX::ParameterSlider> slider = row->findChild<VNX::ParameterSlider*>();
    ASSERT_FALSE(slider.isNull());
    auto* valueEditor = row->findChild<VNX::ParameterValueEditor*>();
    ASSERT_NE(valueEditor, nullptr);
    QPointer<QDoubleSpinBox> spin = valueEditor->valueEditor();
    ASSERT_FALSE(spin.isNull());
    focusEditor(spin);

    spin->setValue(-12.0);
    drainEvents();
    ASSERT_FALSE(spin.isNull());
    EXPECT_TRUE(spin->hasFocus());
    EXPECT_EQ(undo->count(), 1);
    expectInspectorFocus();
    EXPECT_TRUE(row->property("selected").toBool());
    const auto authored = VNX::buildFieldGroupEffectiveSnapshot(
        *pm.currentProject(), HarkenTest::FieldGroupPlatformIds::Windows);
    ASSERT_TRUE(authored.ok());
    const auto& sendsPayload = authored.definitions.at(VNX::toFieldGroupGuid(sound->guid)).send;
    ASSERT_EQ(sendsPayload.sends.size(), 1u);
    EXPECT_NEAR(sendsPayload.sends.front().sendLevel, -12.0, 0.001);

    undo->undo();
    drainEvents();
    ASSERT_FALSE(spin.isNull());
    EXPECT_TRUE(spin->hasFocus());
    EXPECT_NEAR(spin->value(), 0.0, 0.001);
    undo->redo();
    drainEvents();
    ASSERT_FALSE(spin.isNull());
    EXPECT_TRUE(spin->hasFocus());
    EXPECT_NEAR(spin->value(), -12.0, 0.001);
    expectInspectorFocus();

    auto* rail = slider->findChild<QSlider*>();
    ASSERT_NE(rail, nullptr);
    focusEditor(rail);
    slider->binding()->beginEdit();
    slider->binding()->previewEdit(-34.4);
    slider->binding()->commitEdit();
    drainEvents();
    ASSERT_FALSE(slider.isNull());
    EXPECT_TRUE(rail->hasFocus());
    EXPECT_NEAR(slider->value(), -34.4, 0.001);
    EXPECT_TRUE(row->property("selected").toBool());
    expectInspectorFocus();
}

}
