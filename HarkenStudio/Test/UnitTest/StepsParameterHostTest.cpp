#include "ActionManager.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "Components/VAction.h"
#include "Components/VVariable.h"
#include "Core/ThemeManager.h"
#include "Core/ThemeAssetCatalog.h"
#include "Core/QtControlSkin.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterControlHost.h"
#include "Widgets/Common/ParameterPresenter.h"
#include "Widgets/Common/WheelGuard.h"
#include "Widgets/Inspector/Providers/StepsSectionProvider.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Shared/ProjectItemMime.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDropEvent>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QScopeGuard>
#include <QSlider>
#include <QTemporaryDir>
#include <QToolButton>
#include <QWheelEvent>

#include <memory>

using VNX::ProjectItem;

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

int undoDepth()
{
    return VNX::UndoRedoManager::instance().undoStack()->count();
}

bool sendDrop(VNX::ObjectRefWidget& chip, uint32_t objectID)
{
    QMimeData mime;
    VNX::ProjectItemMime::writeObjectIDs(&mime, {objectID});
    QDropEvent event(QPointF(5, 5), Qt::CopyAction | Qt::MoveAction, &mime,
                     Qt::LeftButton, Qt::NoModifier);
    // QApplication::notify only routes drop events to the live drag target, so
    // synthesized drops must enter through QWidget::event directly.
    static_cast<QObject&>(chip).event(&event);
    return event.isAccepted();
}

class StepsParameterHostTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerProjectActions(VNX::ActionManager::instance(), &pm, nullptr, {});
        ASSERT_TRUE(tempDir.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("StepsParameterHostProject");
        config.location = tempDir.path();
        ASSERT_TRUE(pm.createProject(config));
    }

    void TearDown() override
    {
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
    }

    void buildSetValueStep(VNX::VariableKind kind, float rangeMin, float rangeMax,
                           float initialValue,
                           std::vector<VNX::VariableLabel> labels = {})
    {
        auto& pm = VNX::ProjectManager::instance();
        variable_ = pm.createItem(nullptr, QStringLiteral("Axis"),
                                  ProjectItem::Type::Variable);
        ASSERT_NE(variable_, nullptr);
        auto* declaration = variable_->comp<VNX::VVariable>();
        ASSERT_NE(declaration, nullptr);
        declaration->kind = kind;
        declaration->rangeMin = rangeMin;
        declaration->rangeMax = rangeMax;
        declaration->labels = std::move(labels);

        action_ = pm.createItem(nullptr, QStringLiteral("SetValue_Action"),
                                ProjectItem::Type::Action);
        ASSERT_NE(action_, nullptr);
        steps_ = action_->comp<VNX::VAction>();
        ASSERT_NE(steps_, nullptr);
        VNX::VActionStep step;
        step.type = VNX::VActionStep::Type::SetVariable;
        step.targetObjectID = variable_->objectID;
        step.value = initialValue;
        steps_->steps = { step };

        section_.reset(provider_.build(nullptr));
        provider_.bind(section_.get(), action_);
        host_ = section_->findChild<VNX::ParameterControlHost*>(
            QStringLiteral("stepValueHost_0"));
        ASSERT_NE(host_, nullptr);
    }

    float storedValue() const { return steps_->steps.at(0).value; }

    void expandDetailRow()
    {
        section_->show();
        for (QWidget* ancestor = host_; ancestor; ancestor = ancestor->parentWidget())
            ancestor->show();
        section_->activateWindow();
        QApplication::processEvents();
    }

    QTemporaryDir tempDir;
    VNX::StepsSectionProvider provider_;
    std::unique_ptr<QWidget> section_;
    ProjectItem* variable_ = nullptr;
    ProjectItem* action_ = nullptr;
    VNX::VAction* steps_ = nullptr;
    VNX::ParameterControlHost* host_ = nullptr;
};

} // namespace

TEST_F(StepsParameterHostTest, DetailExpanderUsesTheSharedChevronGeometry)
{
    buildSetValueStep(VNX::VariableKind::Smooth, 0.0f, 4.0f, 2.0f);
    section_->setStyleSheet(VNX::QtControlSkin::current().applicationStyleSheet());
    auto* expander = section_->findChild<QToolButton*>(QStringLiteral("stepExpander"));
    ASSERT_NE(expander, nullptr);
    expander->ensurePolished();
    expander->resize(expander->sizeHint());
    EXPECT_EQ(expander->size(), QSize(20, 20));
    EXPECT_EQ(expander->iconSize(), QSize(20, 20));
    EXPECT_EQ(expander->arrowType(), Qt::RightArrow);
    expander->click();
    EXPECT_EQ(expander->arrowType(), Qt::DownArrow);
}

TEST_F(StepsParameterHostTest, TypeIconsKeepTheTwentyPixelGridAndRefreshThemeForeground)
{
    buildSetValueStep(VNX::VariableKind::Smooth, 0.0f, 4.0f, 2.0f);
    const auto previousPalette = qApp->palette();
    const auto restore = qScopeGuard([&] {
        qApp->setPalette(previousPalette);
        emit VNX::ThemeManager::instance().themeChanged(VNX::ThemeManager::instance().snapshot());
    });
    const std::pair<VNX::VActionStep::Type, const char*> types[] = {
        {VNX::VActionStep::Type::Play, VNX::ThemeAssetIds::ActionStepPlay},
        {VNX::VActionStep::Type::Stop, VNX::ThemeAssetIds::ActionStepStop},
        {VNX::VActionStep::Type::SetVariable, VNX::ThemeAssetIds::ActionStepSetValue},
        {VNX::VActionStep::Type::PostAction, VNX::ThemeAssetIds::ActionStepPostAction},
        {VNX::VActionStep::Type::Reset, VNX::ThemeAssetIds::ActionStepReset}};
    steps_->steps.clear();
    for (const auto& [type, asset] : types) {
        VNX::VActionStep step;
        step.type = type;
        steps_->steps.push_back(step);
    }
    provider_.bind(section_.get(), action_);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    for (const auto& foreground : {QColor("#eeeeee"), QColor("#202020")}) {
        auto palette = previousPalette;
        palette.setColor(QPalette::ButtonText, foreground);
        qApp->setPalette(palette);
        emit VNX::ThemeManager::instance().themeChanged(VNX::ThemeManager::instance().snapshot());
        for (int index = 0; index < 5; ++index) {
            auto* label = section_->findChild<QLabel*>(QStringLiteral("stepTypeIcon_%1").arg(index));
            ASSERT_NE(label, nullptr);
            EXPECT_EQ(label->size(), QSize(20, 20));
            EXPECT_EQ(label->pixmap().deviceIndependentSize(), QSizeF(20, 20));
            const auto expected = VNX::ThemeManager::instance().resolveImage(
                QString::fromLatin1(types[index].second),
                VNX::ThemeAssetVariant{QSize(20, 20), label->devicePixelRatioF(), 0, foreground});
            ASSERT_TRUE(expected.ok());
            EXPECT_EQ(label->pixmap().toImage(), expected.resource->image);
        }
    }
}

TEST_F(StepsParameterHostTest, ExpandedSelectionCoversTheNumberColumnForTheWholeLogicalRow)
{
    buildSetValueStep(VNX::VariableKind::Smooth, 0.0f, 4.0f, 2.0f);
    VNX::VActionStep play;
    play.type = VNX::VActionStep::Type::Play;
    steps_->steps.insert(steps_->steps.begin(), play);
    provider_.bind(section_.get(), action_);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    section_->setStyleSheet(VNX::QtControlSkin::current().applicationStyleSheet());
    section_->resize(500, 200);
    section_->show();
    const auto rows = section_->findChildren<QWidget*>(QStringLiteral("listRow"));
    ASSERT_EQ(rows.size(), 2);
    auto* row = rows[1];
    auto* expander = row->findChild<QToolButton*>(QStringLiteral("stepExpander"));
    auto* details = section_->findChild<QWidget*>(QStringLiteral("stepDetails_1"));
    ASSERT_NE(expander, nullptr);
    ASSERT_NE(row, nullptr);
    ASSERT_NE(details, nullptr);
    auto* number = row->findChild<QLabel*>(QStringLiteral("tableRowNumber"));
    ASSERT_NE(number, nullptr);
    expander->click();
    QApplication::processEvents();
    const auto color = [](QWidget* widget, int x = 4) {
        QImage image(widget->size(), QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        widget->render(&image);
        return image.pixelColor(x, widget->height() / 2);
    };
    const auto& colors = VNX::ThemeManager::instance().colors();
    const auto expectContinuousSelection = [&] {
        auto* body = row->parentWidget();
        QImage image(body->size(), QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        body->render(&image);
        const int left = number->mapTo(body, QPoint()).x();
        for (int y = row->y(); y <= details->geometry().bottom(); ++y) {
            EXPECT_EQ(image.pixelColor(left, y), colors.accent) << y;
            EXPECT_EQ(image.pixelColor(left + 3, y), colors.bgSelected) << y;
        }
        EXPECT_EQ(color(details, number->width() + 1), colors.bgPrimary);
        EXPECT_EQ(color(details, details->width() - 2), colors.bgPrimary);
    };
    EXPECT_EQ(color(row, number->x()), colors.bgPrimary);
    EXPECT_EQ(color(row, number->x() + 4), colors.bgPrimary);
    EXPECT_EQ(color(details), colors.bgPrimary);
    const QPointF position(4, 4);
    QMouseEvent click(QEvent::MouseButtonPress, position,
                      details->mapToGlobal(position.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(details, &click);
    EXPECT_TRUE(row->property("selected").toBool());
    EXPECT_EQ(color(row, number->x()), colors.accent);
    EXPECT_EQ(color(row, number->x() + 4), colors.bgSelected);
    EXPECT_EQ(color(row, row->width() - 2), colors.bgPrimary);
    expectContinuousSelection();
    expander->click();
    QApplication::processEvents();
    EXPECT_TRUE(details->isHidden());
    expander->click();
    number->setText(QStringLiteral("1000"));
    section_->resize(700, 240);
    QApplication::processEvents();
    expectContinuousSelection();
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(row, &escape);
    EXPECT_FALSE(row->property("selected").toBool());
    EXPECT_EQ(color(row, number->x()), colors.bgPrimary);
    EXPECT_EQ(color(row, number->x() + 4), colors.bgPrimary);
    EXPECT_EQ(color(details), colors.bgPrimary);
}

TEST_F(StepsParameterHostTest, SetVariableDetailBindsDomainLabelsAndIndependentScope)
{
    buildSetValueStep(VNX::VariableKind::Step, 0.0f, 4.0f, 2.0f,
                      {{1, QStringLiteral("Calm")}, {3, QStringLiteral("Combat")}});

    auto* value = host_->binding();
    EXPECT_DOUBLE_EQ(value->minimum(), 0.0);
    EXPECT_DOUBLE_EQ(value->maximum(), 4.0);
    EXPECT_DOUBLE_EQ(value->quantum(), 1.0);
    EXPECT_EQ(value->decimals(), 0);
    EXPECT_DOUBLE_EQ(value->value(), 2.0);
    EXPECT_EQ(value->annotationFor(3.0), QStringLiteral("Combat"));
    EXPECT_TRUE(value->annotationFor(2.0).isEmpty());

    ASSERT_NE(host_->presenter(), nullptr);
    ASSERT_NE(host_->presenter()->slider(), nullptr);
    ASSERT_NE(host_->presenter()->choiceEditor(), nullptr);
    EXPECT_EQ(host_->presenter()->valueEditor(), nullptr);
    EXPECT_EQ(host_->presenter()->choiceEditor()->count(), 5);
    EXPECT_EQ(host_->presenter()->choiceEditor()->itemText(2), QStringLiteral("2"));
    EXPECT_EQ(host_->presenter()->choiceEditor()->itemText(3), QStringLiteral("Combat"));
    EXPECT_EQ(host_->presenter()->slider()->minimum(), 0);
    EXPECT_EQ(host_->presenter()->slider()->maximum(), 4);

    auto* scope = section_->findChild<QComboBox*>(QStringLiteral("stepVariableScopeCombo_0"));
    ASSERT_NE(scope, nullptr);
    EXPECT_EQ(scope->currentData().toInt(), static_cast<int>(VNX::VariableScope::Emitter));
    const int before = undoDepth();
    scope->setCurrentIndex(scope->findData(static_cast<int>(VNX::VariableScope::Global)));
    EXPECT_EQ(steps_->steps[0].scope, VNX::VariableScope::Global);
    EXPECT_FLOAT_EQ(storedValue(), 2);
    EXPECT_EQ(undoDepth(), before + 1);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    EXPECT_EQ(steps_->steps[0].scope, VNX::VariableScope::Emitter);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().redo());
    EXPECT_EQ(steps_->steps[0].scope, VNX::VariableScope::Global);
    auto* slider = host_->presenter()->slider();
    slider->setSliderDown(true);
    slider->setValue(3);
    EXPECT_DOUBLE_EQ(value->value(), 3.0);
    slider->setSliderDown(false);
    EXPECT_FLOAT_EQ(storedValue(), 3.0f);
    EXPECT_EQ(slider->value(), 3);
    EXPECT_EQ(steps_->steps[0].scope, VNX::VariableScope::Global);
    EXPECT_EQ(undoDepth(), before + 2);
}

TEST_F(StepsParameterHostTest, ChoicesCommitOneUndoAndRefreshWhenTheVariableChanges)
{
    buildSetValueStep(VNX::VariableKind::Step, 0, 100, 0, {{2, QStringLiteral("Ready")}});
    auto* combo = host_->presenter()->choiceEditor();
    ASSERT_NE(combo, nullptr);
    const int before = undoDepth();
    combo->setCurrentIndex(combo->findData(2));
    EXPECT_FLOAT_EQ(storedValue(), 2);
    EXPECT_EQ(undoDepth(), before + 1);
    auto* variable = variable_->comp<VNX::VVariable>();
    variable->labels.front().name = QStringLiteral("Waiting");
    emit VNX::ProjectManager::instance().itemPropertyChanged(variable_, QStringLiteral("labels"), nullptr);
    EXPECT_EQ(combo->currentText(), QStringLiteral("Waiting"));
    variable->rangeMax = 4;
    emit VNX::ProjectManager::instance().itemPropertyChanged(variable_, QStringLiteral("rangeMax"), nullptr);
    EXPECT_EQ(combo->count(), 5);
    EXPECT_EQ(combo->currentData().toInt(), 2);
    variable->kind = VNX::VariableKind::Smooth;
    emit VNX::ProjectManager::instance().itemPropertyChanged(variable_, QStringLiteral("kind"), nullptr);
    EXPECT_EQ(host_->presenter()->choiceEditor(), nullptr);
    ASSERT_NE(host_->presenter()->valueEditor(), nullptr);
    EXPECT_EQ(undoDepth(), before + 1);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    EXPECT_FLOAT_EQ(storedValue(), 0);
}

TEST_F(StepsParameterHostTest, VariableMetadataRefreshesCurrentRowsAndPreservesValueGestures)
{
    buildSetValueStep(VNX::VariableKind::Step, 0, 3, 0, {{1, QStringLiteral("Ready")}});
    auto* retired = host_;
    provider_.bind(section_.get(), action_);
    host_ = section_->findChildren<VNX::ParameterControlHost*>(QStringLiteral("stepValueHost_0")).back();
    ASSERT_NE(host_, retired);
    auto* value = host_->binding();
    auto* combo = host_->presenter()->choiceEditor();
    ASSERT_NE(combo, nullptr);
    auto* variable = variable_->comp<VNX::VVariable>();
    variable->labels.front().name = QStringLiteral("Waiting");
    emit VNX::ProjectManager::instance().itemPropertyChanged(variable_, QStringLiteral("labels"), nullptr);
    EXPECT_EQ(combo->itemText(1), QStringLiteral("Waiting"));
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    const int before = undoDepth();
    value->beginEdit();
    value->previewEdit(1);
    variable->defaultValue = 2;
    emit VNX::ProjectManager::instance().itemPropertyChanged(variable_, QStringLiteral("defaultValue"), nullptr);
    EXPECT_TRUE(value->isEditing());
    variable->labels.front().name = QStringLiteral("Armed");
    emit VNX::ProjectManager::instance().itemPropertyChanged(variable_, QStringLiteral("labels"), nullptr);
    EXPECT_TRUE(value->isEditing());
    EXPECT_DOUBLE_EQ(value->value(), 1);
    EXPECT_EQ(combo->currentText(), QStringLiteral("Armed"));
    EXPECT_FLOAT_EQ(storedValue(), 0);
    EXPECT_EQ(undoDepth(), before);
    value->commitEdit();
    EXPECT_FLOAT_EQ(storedValue(), 1);
    EXPECT_EQ(undoDepth(), before + 1);
}

TEST_F(StepsParameterHostTest, DetailRowsDoNotReserveTableSlotsAgain)
{
    buildSetValueStep(VNX::VariableKind::Smooth, 0.0f, 1.0f, 0.25f);
    auto* list = section_->findChild<QWidget*>(QStringLiteral("stepsListWidget"));
    auto* table = section_->findChild<QFrame*>(QStringLiteral("listBox"));
    auto* gutter = section_->findChild<QWidget*>(
        QStringLiteral("selectionFormGutter_stepValueHost_0"));
    ASSERT_NE(list, nullptr);
    ASSERT_NE(table, nullptr);
    ASSERT_NE(gutter, nullptr);
    section_->resize(600, 720);
    expandDetailRow();
    const auto& inspector = VNX::ThemeManager::instance().metrics().inspector;
    EXPECT_EQ(list->width() - table->geometry().right() - 1,
              inspector.slotWidthFor(2) + inspector.columnPadding);
    EXPECT_EQ(gutter->width(), 0);
}

TEST_F(StepsParameterHostTest, GesturePreviewsWithoutUndoThenCommitsOnce)
{
    buildSetValueStep(VNX::VariableKind::Smooth, 0.0f, 1.0f, 0.25f);
    auto* value = host_->binding();
    const int before = undoDepth();

    value->beginEdit();
    value->previewEdit(0.4);
    value->previewEdit(0.8);
    EXPECT_FLOAT_EQ(storedValue(), 0.25f);
    EXPECT_EQ(undoDepth(), before);

    value->commitEdit();
    EXPECT_FALSE(value->isEditing());
    EXPECT_FLOAT_EQ(storedValue(), 0.8f);
    EXPECT_EQ(undoDepth(), before + 1);

    VNX::UndoRedoManager::instance().undoStack()->undo();
    EXPECT_FLOAT_EQ(storedValue(), 0.25f);
}

TEST_F(StepsParameterHostTest, CanceledAndEmptyGesturesDoNotCreateUndoEntries)
{
    buildSetValueStep(VNX::VariableKind::Smooth, 0.0f, 1.0f, 0.25f);
    auto* value = host_->binding();
    const int before = undoDepth();

    value->beginEdit();
    value->previewEdit(0.9);
    value->cancelEdit();

    EXPECT_FALSE(value->isEditing());
    EXPECT_DOUBLE_EQ(value->value(), 0.25);
    EXPECT_FLOAT_EQ(storedValue(), 0.25f);
    EXPECT_EQ(undoDepth(), before);

    auto* slider = host_->presenter()->slider();
    ASSERT_NE(slider, nullptr);
    slider->setSliderDown(true);
    slider->setSliderDown(false);

    EXPECT_EQ(undoDepth(), before);
    EXPECT_FLOAT_EQ(storedValue(), 0.25f);
}

TEST_F(StepsParameterHostTest, TypedValueProducesExactlyOneUndoEntry)
{
    buildSetValueStep(VNX::VariableKind::Smooth, 0.0f, 1.0f, 0.25f);
    auto* editor = host_->presenter()->valueEditor();
    ASSERT_NE(editor, nullptr);
    ASSERT_FALSE(editor->keyboardTracking());
    expandDetailRow();
    editor->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    editor->selectAll();

    const int before = undoDepth();
    for (const int key : {Qt::Key_0, Qt::Key_Period, Qt::Key_7, Qt::Key_5})
    {
        const QString text = key == Qt::Key_Period
            ? QStringLiteral(".")
            : QString(QChar('0' + (key - Qt::Key_0)));
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, text);
        QApplication::sendEvent(editor, &press);
    }

    EXPECT_EQ(undoDepth(), before);
    EXPECT_FLOAT_EQ(storedValue(), 0.25f);

    QKeyEvent accept(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier,
                     QStringLiteral("\r"));
    QApplication::sendEvent(editor, &accept);

    EXPECT_EQ(undoDepth(), before + 1);
    EXPECT_FLOAT_EQ(storedValue(), 0.75f);
}

TEST_F(StepsParameterHostTest, FocusKeyboardAndAccessibilityReachTheEditors)
{
    buildSetValueStep(VNX::VariableKind::Step, 0.0f, 4.0f, 2.0f);
    auto* value = host_->binding();
    auto* slider = host_->presenter()->slider();
    auto* editor = host_->presenter()->choiceEditor();
    ASSERT_NE(slider, nullptr);
    ASSERT_NE(editor, nullptr);

    expandDetailRow();
    slider->setFocus(Qt::TabFocusReason);
    QApplication::processEvents();
    EXPECT_TRUE(slider->hasFocus());
    EXPECT_NE(slider->focusPolicy(), Qt::NoFocus);
    EXPECT_NE(editor->focusPolicy(), Qt::NoFocus);

    const int before = undoDepth();
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Up, Qt::NoModifier);
    QApplication::sendEvent(slider, &press);
    QApplication::sendEvent(slider, &release);

    EXPECT_DOUBLE_EQ(value->value(), 3.0);
    EXPECT_FLOAT_EQ(storedValue(), 3.0f);
    EXPECT_EQ(undoDepth(), before + 1);
}

TEST_F(StepsParameterHostTest, WheelGuardCoversThePresenterEditors)
{
    buildSetValueStep(VNX::VariableKind::Step, 0.0f, 4.0f, 2.0f);
    auto* value = host_->binding();
    auto* editor = host_->presenter()->choiceEditor();
    ASSERT_NE(editor, nullptr);
    expandDetailRow();
    ASSERT_FALSE(editor->hasFocus());

    const int before = undoDepth();
    const double unfocused = value->value();
    qApp->installEventFilter(VNX::WheelGuard::instance());
    QWheelEvent wheel(QPointF(1.0, 1.0), editor->mapToGlobal(QPoint(1, 1)),
                      QPoint(0, 0), QPoint(0, 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(editor, &wheel);
    qApp->removeEventFilter(VNX::WheelGuard::instance());

    EXPECT_DOUBLE_EQ(value->value(), unfocused);
    EXPECT_FLOAT_EQ(storedValue(), 2.0f);
    EXPECT_EQ(undoDepth(), before);
}

TEST_F(StepsParameterHostTest, ThemeSwitchDuringAGestureCommitsExactlyOnce)
{
    buildSetValueStep(VNX::VariableKind::Smooth, 0.0f, 1.0f, 0.25f);
    auto* value = host_->binding();
    const int before = undoDepth();

    auto* slider = host_->presenter()->slider();
    ASSERT_NE(slider, nullptr);
    QPointer<VNX::ParameterPresenter> replaced = host_->presenter();
    slider->setSliderDown(true);
    slider->setValue(700);
    ASSERT_TRUE(value->isEditing());
    EXPECT_EQ(undoDepth(), before);

    auto& theme = VNX::ThemeManager::instance();
    emit theme.themeChanged(theme.snapshot());

    EXPECT_TRUE(replaced.isNull());
    EXPECT_FALSE(value->isEditing());
    EXPECT_DOUBLE_EQ(value->value(), 0.7);
    EXPECT_FLOAT_EQ(storedValue(), 0.7f);
    EXPECT_EQ(undoDepth(), before + 1);
    EXPECT_EQ(host_->binding(), value);
}

TEST_F(StepsParameterHostTest, PresenterRebuildAddsNoCommitAndKeepsFullPrecision)
{
    buildSetValueStep(VNX::VariableKind::Smooth, 0.0f, 1.0f, 0.25f);
    auto* value = host_->binding();
    value->setValue(0.123456789);
    const double exact = value->value();
    const float stored = storedValue();
    const int before = undoDepth();

    for (const auto id : {VNX::ParameterPresenterId::HorizontalSlider,
                          VNX::ParameterPresenterId::VerticalFader,
                          VNX::ParameterPresenterId::RotaryKnob,
                          VNX::ParameterPresenterId::SliderWithValue})
    {
        QPointer<VNX::ParameterPresenter> previous = host_->presenter();
        host_->setPresenterOverride(id);
        host_->rebuildPresenter();
        EXPECT_TRUE(previous.isNull());
        EXPECT_EQ(host_->presenterId(), id);
        EXPECT_EQ(host_->binding(), value);
        EXPECT_EQ(host_->presenter()->primaryEditor()->accessibleName(), QStringLiteral("Value"));
        EXPECT_DOUBLE_EQ(value->value(), exact);
        EXPECT_FLOAT_EQ(storedValue(), stored);
        EXPECT_EQ(undoDepth(), before);
    }

    EXPECT_DOUBLE_EQ(host_->presenter()->valueEditor()->value(), 0.12);
    EXPECT_DOUBLE_EQ(value->value(), exact);
}

TEST_F(StepsParameterHostTest, RetargetingRebindsAndClearsThePreviousDomainWithoutRebuildingTheRow)
{
    buildSetValueStep(VNX::VariableKind::Smooth, 0.0f, 100.0f, 80.0f);
    auto* value = host_->binding();
    QPointer<VNX::ParameterControlHost> host(host_);

    auto& pm = VNX::ProjectManager::instance();
    ProjectItem* stage = pm.createItem(nullptr, QStringLiteral("Stage"),
                                       ProjectItem::Type::Variable);
    ASSERT_NE(stage, nullptr);
    auto* declaration = stage->comp<VNX::VVariable>();
    ASSERT_NE(declaration, nullptr);
    declaration->kind = VNX::VariableKind::Step;
    declaration->rangeMin = 0.0f;
    declaration->rangeMax = 3.0f;
    declaration->labels = {{2, QStringLiteral("Boss")}};

    auto* chip = section_->findChild<VNX::ObjectRefWidget*>(
        QStringLiteral("stepTargetChip_0"));
    ASSERT_NE(chip, nullptr);
    ASSERT_TRUE(sendDrop(*chip, stage->objectID));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    ASSERT_FALSE(host.isNull());
    EXPECT_EQ(host_->binding(), value);
    EXPECT_DOUBLE_EQ(value->minimum(), 0.0);
    EXPECT_DOUBLE_EQ(value->maximum(), 3.0);
    EXPECT_DOUBLE_EQ(value->quantum(), 1.0);
    EXPECT_EQ(value->decimals(), 0);
    EXPECT_EQ(value->annotationFor(2.0), QStringLiteral("Boss"));
    EXPECT_DOUBLE_EQ(value->value(), 3.0);
    EXPECT_FLOAT_EQ(storedValue(), 3.0f);
    EXPECT_EQ(steps_->steps.at(0).targetObjectID, stage->objectID);

    variable_->comp<VNX::VVariable>()->rangeMax = 1.0f;
    ASSERT_TRUE(sendDrop(*chip, variable_->objectID));
    EXPECT_EQ(host_->binding(), value);
    EXPECT_DOUBLE_EQ(value->quantum(), 0.0);
    EXPECT_EQ(value->decimals(), 2);
    EXPECT_TRUE(value->annotationFor(2.0).isEmpty());
    EXPECT_DOUBLE_EQ(value->maximum(), 1.0);
    EXPECT_FLOAT_EQ(storedValue(), 1.0f);
    EXPECT_EQ(steps_->steps.at(0).targetObjectID, variable_->objectID);
}
