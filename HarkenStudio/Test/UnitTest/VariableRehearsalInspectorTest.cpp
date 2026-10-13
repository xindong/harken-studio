#include "Widgets/GridEditor/GridEditorPanel.h"
#include "Widgets/GridEditor/MixingEditorWidget.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/InspectorCustomizationRegistry.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/Inspector/Providers/SelectionSectionProvider.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterPresenter.h"

#include "ActionManager.h"
#include "Components/VSelection.h"
#include "Components/VVariable.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QAbstractItemView>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSlider>
#include <QTemporaryDir>
#include "Widgets/Selector/ObjectRefWidget.h"

#include <memory>

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
                                    nullptr, {});
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
        step_ = projectManager.createItem(nullptr, QStringLiteral("VR_Step"),
                                          VNX::ProjectItem::Type::Variable);
        ASSERT_NE(container_, nullptr);
        ASSERT_NE(child_, nullptr);
        ASSERT_NE(smooth_, nullptr);
        ASSERT_NE(step_, nullptr);

        auto* smoothVariable = smooth_->compOrCreate<VNX::VVariable>();
        smoothVariable->rangeMin = 0.0f;
        smoothVariable->rangeMax = 100.0f;
        smoothVariable->defaultValue = 0.0f;
        smoothVariable->labels = {{0, QStringLiteral("Calm")},
                                  {50, QStringLiteral("Storm")}};
        auto* stepVariable = step_->compOrCreate<VNX::VVariable>();
        stepVariable->kind = VNX::VariableKind::Step;
        stepVariable->rangeMin = 0.0f;
        stepVariable->rangeMax = 3.0f;
        stepVariable->defaultValue = 0.0f;

        auto* selection = container_->compOrCreate<VNX::VSelection>();
        selection->mode = VNX::SelectionMode::Match;
        auto grid = selection->matchGrid();
        ASSERT_TRUE(grid.addAxis(smooth_->guid));
        ASSERT_TRUE(grid.addAxis(step_->guid));
        ASSERT_TRUE(grid.addRow(child_->guid));
        ASSERT_TRUE(grid.setCell(0, 0, 0));
        ASSERT_TRUE(grid.setCell(0, 1, 2));
        ASSERT_TRUE(selection->selections.shared.setMatchGrid(grid));
        blend_ = projectManager.createItem(nullptr, QStringLiteral("Blend"),
            VNX::ProjectItem::Type::Container);
        ASSERT_NE(blend_, nullptr);
        auto* mixingChild = projectManager.createItem(blend_, QStringLiteral("BlendChild"),
            VNX::ProjectItem::Type::Sound);
        ASSERT_NE(mixingChild, nullptr);
        auto* blendSelection = blend_->comp<VNX::VSelection>();
        blendSelection->mode = VNX::SelectionMode::Blend;
        blendSelection->blendResponse = VNX::VariableResponse::Live;
        auto mixing = blendSelection->mixing();
        mixing.rows.push_back({step_->guid, {}});
        mixing.rows.back().children.push_back(VNX::MixingChildDef::unity(mixingChild->guid, 0.0f, 100.0f));
        auto& curve = mixing.rows.front().children[0];
        curve.points = {{0.0f, 0.0f}, {2.0f, 1.0f}, {3.0f, 1.0f}};
        curve.normalize();
        ASSERT_TRUE(blendSelection->selections.shared.setMixing(mixing));

        VNX::UndoRedoManager::instance().clear();
        VNX::registerBuiltinInspectorCustomizations();
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        auto& projectManager = VNX::ProjectManager::instance();
        if (projectManager.isProjectOpen())
            projectManager.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
    }

    std::unique_ptr<QWidget> makeSelectionSection()
    {
        auto* provider = VNX::InspectorCustomizationRegistry::instance().sectionProvider(
            VNX::ProjectItem::Type::Container, QStringLiteral("selection"));
        EXPECT_NE(provider, nullptr);
        std::unique_ptr<QWidget> section(provider->build(nullptr));
        provider->bind(section.get(), container_);
        return section;
    }

    VNX::ObjectRefWidget* axisChipFor(QWidget* section, const VNX::ProjectItem* variable,
                                      const QString& objectName)
    {
        for (auto* chip : section->findChildren<VNX::ObjectRefWidget*>(objectName)) {
            if (chip && chip->currentObjectID() == variable->objectID)
                return chip;
        }
        return nullptr;
    }

    QTemporaryDir tempDir_;
    VNX::ProjectItem* container_ = nullptr;
    VNX::ProjectItem* child_ = nullptr;
    VNX::ProjectItem* blend_ = nullptr;
    VNX::ProjectItem* smooth_ = nullptr;
    VNX::ProjectItem* step_ = nullptr;
};

TEST_F(VariableRehearsalInspectorTest, DefaultValueMouseDragCommitsAndUndoesForBothKinds)
{
    VNX::InspectorPanel panel;
    auto* undo = VNX::UndoRedoManager::instance().undoStack();
    for (auto* item : {step_, smooth_}) {
        SCOPED_TRACE(item->name.toStdString());
        panel.showItemInspector(item);
        auto* control = panel.findChild<VNX::ParameterSlider*>(QStringLiteral("prop_defaultValue"));
        ASSERT_NE(control, nullptr);
        auto* slider = control->presenter()->slider();
        ASSERT_NE(slider, nullptr);
        slider->resize(400, 24);
        auto* variable = item->comp<VNX::VVariable>();
        const int before = undo->index();
        const auto mouse = [&](QEvent::Type type, double x) {
            const QPointF point(x, slider->height() / 2.0);
            QMouseEvent event(type, point, slider->mapToGlobal(point.toPoint()),
                type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                Qt::NoModifier);
            QApplication::sendEvent(slider, &event);
        };
        mouse(QEvent::MouseButtonPress, 120);
        ASSERT_TRUE(control->binding()->isEditing());
        mouse(QEvent::MouseMove, 280);
        EXPECT_GT(control->value(), 0);
        EXPECT_FLOAT_EQ(variable->defaultValue, static_cast<float>(control->value()));
        EXPECT_EQ(undo->index(), before);
        mouse(QEvent::MouseMove, 399);
        mouse(QEvent::MouseButtonRelease, 399);
        EXPECT_FALSE(control->binding()->isEditing());
        EXPECT_FLOAT_EQ(variable->defaultValue, variable->rangeMax);
        EXPECT_DOUBLE_EQ(control->value(), variable->rangeMax);
        EXPECT_EQ(undo->index(), before + 1);
        undo->undo();
        EXPECT_FLOAT_EQ(variable->defaultValue, 0);
        EXPECT_DOUBLE_EQ(control->value(), 0);
        undo->redo();
        EXPECT_FLOAT_EQ(variable->defaultValue, variable->rangeMax);
        EXPECT_DOUBLE_EQ(control->value(), variable->rangeMax);
        item->clearDirty();
        mouse(QEvent::MouseButtonPress, 120);
        EXPECT_LT(variable->defaultValue, variable->rangeMax);
        control->cancelPendingInput();
        QEvent ungrab(QEvent::UngrabMouse);
        QApplication::sendEvent(slider, &ungrab);
        mouse(QEvent::MouseButtonRelease, 120);
        EXPECT_FLOAT_EQ(variable->defaultValue, variable->rangeMax);
        EXPECT_DOUBLE_EQ(control->value(), variable->rangeMax);
        EXPECT_EQ(undo->index(), before + 1);
        EXPECT_FALSE(item->isDirtyObject());
        undo->clear();
    }
}

TEST_F(VariableRehearsalInspectorTest, DefaultValuePopupAndTypedEntryPersistAcrossReload)
{
    auto& pm = VNX::ProjectManager::instance();
    const QUuid stepId = step_->guid;
    const QUuid smoothId = smooth_->guid;
    auto* variable = step_->comp<VNX::VVariable>();
    variable->labels = {{1, QStringLiteral("Ready")}};
    VNX::InspectorPanel panel;
    panel.setAttribute(Qt::WA_DontShowOnScreen);
    panel.resize(700, 700);
    panel.show();
    panel.showItemInspector(step_);
    QApplication::processEvents();
    auto* control = panel.findChild<VNX::ParameterSlider*>(QStringLiteral("prop_defaultValue"));
    ASSERT_NE(control, nullptr);
    auto* combo = control->presenter()->choiceEditor();
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(combo->itemText(1), QStringLiteral("Ready"));
    EXPECT_EQ(combo->itemText(2), QStringLiteral("2"));
    combo->showPopup();
    QApplication::processEvents();
    auto* view = combo->view();
    const QPoint point = view->visualRect(view->model()->index(2, 0)).center();
    ASSERT_TRUE(view->viewport()->rect().contains(point));
    QMouseEvent press(QEvent::MouseButtonPress, point, view->viewport()->mapToGlobal(point),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &press);
    QMouseEvent release(QEvent::MouseButtonRelease, point, view->viewport()->mapToGlobal(point),
        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &release);
    EXPECT_FLOAT_EQ(step_->comp<VNX::VVariable>()->defaultValue, 2);
    EXPECT_EQ(combo->currentData().toInt(), 2);
    combo->hidePopup();
    variable->labels.front().name = QStringLiteral("Waiting");
    emit pm.itemPropertyChanged(step_, QStringLiteral("labels"), nullptr);
    EXPECT_EQ(combo->itemText(1), QStringLiteral("Waiting"));
    variable->kind = VNX::VariableKind::Smooth;
    emit pm.itemPropertyChanged(step_, QStringLiteral("kind"), nullptr);
    ASSERT_NE(control->presenter()->valueEditor(), nullptr);
    EXPECT_TRUE(combo->isHidden());
    EXPECT_EQ(control->presenter()->valueEditor()->decimals(), 2);
    variable->kind = VNX::VariableKind::Step;
    emit pm.itemPropertyChanged(step_, QStringLiteral("kind"), nullptr);
    ASSERT_NE(control->presenter()->choiceEditor(), nullptr);
    EXPECT_EQ(control->presenter()->choiceEditor()->currentData().toInt(), 2);

    panel.showItemInspector(smooth_);
    QApplication::processEvents();
    auto* editor = control->presenter()->valueEditor();
    ASSERT_NE(editor, nullptr);
    editor->setLocale(QLocale::c());
    editor->setFocus();
    editor->selectAll();
    for (const QChar character : QStringLiteral("42.5")) {
        QKeyEvent key(QEvent::KeyPress, character.unicode(), Qt::NoModifier, QString(character));
        QApplication::sendEvent(editor, &key);
    }
    EXPECT_FLOAT_EQ(smooth_->comp<VNX::VVariable>()->defaultValue, 0);
    QKeyEvent accept(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(editor, &accept);
    EXPECT_FLOAT_EQ(smooth_->comp<VNX::VVariable>()->defaultValue, 42.5f);
    auto* undo = VNX::UndoRedoManager::instance().undoStack();
    EXPECT_EQ(undo->index(), 2);
    undo->undo();
    EXPECT_DOUBLE_EQ(editor->value(), 0);
    undo->redo();
    EXPECT_DOUBLE_EQ(editor->value(), 42.5);
    ASSERT_TRUE(pm.saveProject());
    const QString path = pm.currentProject()->projectPath;
    pm.closeProject();
    ASSERT_TRUE(pm.loadProject(path));
    auto* step = pm.findItemByGuid(stepId);
    auto* smooth = pm.findItemByGuid(smoothId);
    ASSERT_NE(step, nullptr);
    ASSERT_NE(smooth, nullptr);
    EXPECT_FLOAT_EQ(step->comp<VNX::VVariable>()->defaultValue, 2);
    EXPECT_FLOAT_EQ(smooth->comp<VNX::VVariable>()->defaultValue, 42.5f);
}


TEST_F(VariableRehearsalInspectorTest, RestoredVariableResolvesGridHeaderRefsForBothKinds)
{
    VNX::GridEditorPanel panel;
    panel.bindItem(container_);
    VNX::GridEditorPanel blendPanel;
    blendPanel.bindItem(blend_);
    EXPECT_NE(axisChipFor(&panel, smooth_, QStringLiteral("gridAxisHeaderRef")), nullptr);
    EXPECT_NE(axisChipFor(&panel, step_, QStringLiteral("gridAxisHeaderRef")), nullptr);
    EXPECT_NE(axisChipFor(&blendPanel, step_, QStringLiteral("mixingRowVariable")), nullptr);
    auto& pm = VNX::ProjectManager::instance();
    const auto guid = step_->guid;
    ASSERT_TRUE(pm.deleteItem(step_));
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    auto* missing = blendPanel.findChild<VNX::ObjectRefWidget*>(QStringLiteral("mixingRowVariable"));
    ASSERT_NE(missing, nullptr);
    EXPECT_EQ(missing->currentObjectID(), 0u);
    VNX::UndoRedoManager::instance().undoStack()->undo();
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    auto* restored = pm.findItemByGuid(guid);
    ASSERT_NE(restored, nullptr);
    EXPECT_NE(axisChipFor(&panel, restored, QStringLiteral("gridAxisHeaderRef")), nullptr);
    EXPECT_NE(axisChipFor(&blendPanel, restored, QStringLiteral("mixingRowVariable")), nullptr);
}

TEST_F(VariableRehearsalInspectorTest, RehearsalUpdatesGridInTheSameFrame)
{
    VNX::GridEditorPanel grid;
    grid.bindItem(blend_);
    ASSERT_EQ(grid.mixingEditor()->childGains().size(), 1u);
    EXPECT_FLOAT_EQ(grid.mixingEditor()->childGains().constBegin().value(), 0.0f);

    ASSERT_TRUE(VNX::ProjectManager::instance().variableRehearsal().setOverride(step_->guid, 2.0f));
    EXPECT_EQ(VNX::ProjectManager::instance().variableRehearsal().overrideFor(step_->guid),
              std::optional<float>(2.0f));
    EXPECT_FLOAT_EQ(grid.mixingEditor()->childGains().constBegin().value(), 1.0f);
}

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

    const QUuid id = smooth_->guid;
    projectManager.closeProject();
    EXPECT_EQ(projectManager.variableRehearsal().overrideFor(id), std::nullopt);
}
