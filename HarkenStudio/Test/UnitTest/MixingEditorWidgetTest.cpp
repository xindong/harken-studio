#include "Widgets/GridEditor/MixingEditorWidget.h"
#include "Widgets/CurveEditor/CurveCanvas.h"
#include "Widgets/GridEditor/MixingRowCanvas.h"
#include "Widgets/Common/ResizableTableView.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Common/ListFooter.h"
#include "ActionManager.h"
#include "Components/VSelection.h"
#include "Components/VVariable.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "VariableRehearsal.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Core/HarkenStyle.h"

#include <gtest/gtest.h>
#include <QApplication>
#include <QAction>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QHeaderView>
#include <QLabel>
#include <QToolButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyleFactory>
#include <QTemporaryDir>

namespace {
void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char name[] = "harken_editor_tests";
    static char* argv[] = {name, nullptr};
    static QApplication app(argc, argv);
}
void drainEvents()
{
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}
QUndoStack* undoStack() { return VNX::UndoRedoManager::instance().undoStack(); }

void applyStudioStyle(QWidget& root)
{
    auto* style = new VNX::HarkenStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    style->setParent(&root);
    root.setStyle(style);
    for (auto* widget : root.findChildren<QWidget*>()) widget->setStyle(style);
}
}

class MixingEditorWidgetTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        styleSheet_ = qApp->styleSheet();
        qApp->setStyleSheet(VNX::QtControlSkin::current().applicationStyleSheet());
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerProjectActions(VNX::ActionManager::instance(), &pm, nullptr, {});

        ASSERT_TRUE(tempDir_.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("ContinuousGridProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(pm.createProject(config));

        container_ = pm.createItem(nullptr, QStringLiteral("CG_Container"),
                                   VNX::ProjectItem::Type::Container);
        ASSERT_NE(container_, nullptr);
        container_->compOrCreate<VNX::VSelection>()->mode =
            VNX::SelectionMode::Blend;
        childA_ = pm.createItem(container_, QStringLiteral("CG_RainLight"),
                                VNX::ProjectItem::Type::Sound);
        childB_ = pm.createItem(container_, QStringLiteral("CG_RainHeavy"),
                                VNX::ProjectItem::Type::Sound);
        ASSERT_NE(childA_, nullptr);
        ASSERT_NE(childB_, nullptr);

        varWind_ = pm.createItem(nullptr, QStringLiteral("CG_WindForce"),
                                 VNX::ProjectItem::Type::Variable);
        ASSERT_NE(varWind_, nullptr);
        varWind_->compOrCreate<VNX::VVariable>();
        varSurface_ = pm.createItem(nullptr, QStringLiteral("CG_Surface"),
                                    VNX::ProjectItem::Type::Variable);
        ASSERT_NE(varSurface_, nullptr);
        auto* surface = varSurface_->compOrCreate<VNX::VVariable>();
        surface->kind = VNX::VariableKind::Step;

        VNX::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
        qApp->setStyleSheet(styleSheet_);
    }

    VNX::VSelection* selection() { return container_->comp<VNX::VSelection>(); }
    QString styleSheet_;
    QTemporaryDir tempDir_;
    VNX::ProjectItem* container_ = nullptr;
    VNX::ProjectItem* childA_ = nullptr;
    VNX::ProjectItem* childB_ = nullptr;
    VNX::ProjectItem* varWind_ = nullptr;
    VNX::ProjectItem* varSurface_ = nullptr;
};

TEST_F(MixingEditorWidgetTest, RowMembershipAndCurvesAreIndependentAndUndoable)
{
    VNX::MixingEditorWidget editor;
    editor.setBoundItem(container_);
    const auto before = selection()->toJson();
    ASSERT_TRUE(editor.commitAddRow(varWind_->guid));
    drainEvents();
    ASSERT_TRUE(editor.commitAddChild(0, childA_->guid));
    drainEvents();
    ASSERT_TRUE(editor.commitAddRow(varSurface_->guid));
    drainEvents();
    ASSERT_TRUE(editor.commitAddChild(1, childA_->guid));
    drainEvents();
    EXPECT_FALSE(editor.commitAddChild(1, childA_->guid));
    ASSERT_TRUE(editor.commitAddChild(0, childB_->guid));
    drainEvents();
    EXPECT_EQ(editor.childGains().size(), 2);
    EXPECT_FLOAT_EQ(editor.childGains().value(childA_->guid), 1.0f);
    EXPECT_FLOAT_EQ(editor.childGains().value(childB_->guid), 1.0f);
    ASSERT_TRUE(editor.commitRemoveChild(0, 0));
    drainEvents();
    EXPECT_EQ(editor.childGains().size(), 2);
    ASSERT_TRUE(editor.commitRemoveRow(1));
    drainEvents();
    EXPECT_EQ(editor.childGains().size(), 1);
    while (undoStack()->canUndo()) undoStack()->undo();
    drainEvents();
    EXPECT_EQ(selection()->toJson(), before);
    while (undoStack()->canRedo()) undoStack()->redo();
    drainEvents();
    EXPECT_EQ(editor.childGains().size(), 1);
    EXPECT_FLOAT_EQ(editor.childGains().value(childB_->guid), 1.0f);
}

TEST_F(MixingEditorWidgetTest, MembersWithoutCurvesStayDormantAndLastCurveRemovalPreservesOccurrences)
{
    const auto occurrences = selection()->selections.shared.occurrenceRows();
    ASSERT_EQ(occurrences.size(), 2u);
    VNX::MixingEditorWidget editor;
    editor.setBoundItem(container_);
    EXPECT_TRUE(editor.childGains().isEmpty());
    EXPECT_TRUE(editor.mixingDef().empty());
    ASSERT_TRUE(editor.commitAddRow(varWind_->guid));
    drainEvents();
    EXPECT_TRUE(editor.childGains().isEmpty());
    ASSERT_TRUE(editor.commitAddChild(0, childA_->guid));
    drainEvents();
    EXPECT_FLOAT_EQ(editor.childGains().value(childA_->guid), 1.0f);
    EXPECT_FALSE(editor.childGains().contains(childB_->guid));
    EXPECT_EQ(selection()->selections.shared.occurrenceRows(), occurrences);
    ASSERT_TRUE(editor.commitRemoveChild(0, 0));
    drainEvents();
    EXPECT_TRUE(editor.childGains().isEmpty());
    EXPECT_EQ(selection()->selections.shared.occurrenceRows(), occurrences);
    ASSERT_NE(selection()->selections.shared.member(childA_->guid), nullptr);
    EXPECT_TRUE(selection()->selections.shared.member(childA_->guid)->curves.empty());
    undoStack()->undo();
    drainEvents();
    EXPECT_FLOAT_EQ(editor.childGains().value(childA_->guid), 1.0f);
    EXPECT_FALSE(editor.childGains().contains(childB_->guid));
    EXPECT_EQ(selection()->selections.shared.occurrenceRows(), occurrences);
}

TEST_F(MixingEditorWidgetTest, EmptyVariableRowMixesOnlyItsTargetsAndKeepsTheirEditorsEnabled)
{
    VNX::MixingEditorWidget editor;
    editor.setBoundItem(container_);
    const auto occurrences = selection()->selections.shared.occurrenceRows();
    auto* add = editor.findChild<QAction*>(QStringLiteral("mixingAddEmptyRow"));
    ASSERT_NE(add, nullptr);
    add->trigger();
    drainEvents();
    ASSERT_EQ(editor.mixingDef().rows.size(), 1u);
    EXPECT_TRUE(editor.mixingDef().rows.front().variable.isNull());
    ASSERT_NE(editor.canvas(0), nullptr);
    EXPECT_FALSE(editor.canvas(0)->isEnabled());
    EXPECT_TRUE(editor.childGains().isEmpty());
    auto* variable = editor.findChild<VNX::ObjectRefWidget*>(QStringLiteral("mixingRowVariable"));
    ASSERT_NE(variable, nullptr);
    EXPECT_TRUE(variable->isEnabled());
    EXPECT_EQ(variable->currentObjectID(), 0u);
    ASSERT_TRUE(editor.commitAddChild(0, childA_->guid));
    drainEvents();
    EXPECT_FLOAT_EQ(editor.childGains().value(childA_->guid), 1.0f);
    EXPECT_FALSE(editor.childGains().contains(childB_->guid));
    ASSERT_EQ(selection()->selections.shared.member(childA_->guid)->curves.size(), 1u);
    EXPECT_TRUE(selection()->selections.shared.member(childA_->guid)->curves.front().points.isEmpty());
    EXPECT_TRUE(editor.canvas(0)->renderData().points.isEmpty());
    auto* target = editor.findChild<VNX::ObjectRefWidget*>(QStringLiteral("mixingChildRef"));
    ASSERT_NE(target, nullptr);
    EXPECT_TRUE(target->isEnabled());
    EXPECT_FALSE(editor.commitCurve(0, 0, {{0.0f, 0.0f}, {100.0f, 100.0f}},
        {{0.0f, 0.0f, 100.0f, 100.0f}}, QStringLiteral("Curve")));
    EXPECT_EQ(selection()->selections.shared.occurrenceRows(), occurrences);
    const auto authored = selection()->selections.toJson();
    for (const auto mode : {VNX::SelectionMode::Random, VNX::SelectionMode::Match,
                           VNX::SelectionMode::Sequence, VNX::SelectionMode::Blend}) {
        selection()->mode = mode;
        editor.reload();
        EXPECT_EQ(selection()->selections.toJson(), authored);
    }
    EXPECT_FLOAT_EQ(editor.childGains().value(childA_->guid), 1.0f);
    ASSERT_TRUE(editor.commitRemoveChild(0, 0));
    drainEvents();
    EXPECT_TRUE(editor.childGains().isEmpty());
    EXPECT_EQ(selection()->selections.shared.occurrenceRows(), occurrences);
    undoStack()->undo();
    drainEvents();
    EXPECT_EQ(selection()->selections.toJson(), authored);
    EXPECT_FLOAT_EQ(editor.childGains().value(childA_->guid), 1.0f);
    ASSERT_TRUE(editor.commitRemoveRow(0));
    drainEvents();
    EXPECT_TRUE(editor.mixingDef().empty());
    EXPECT_EQ(selection()->selections.shared.occurrenceRows(), occurrences);
}

TEST_F(MixingEditorWidgetTest, ClearingVariablePreservesCurvesAndReassignmentInitializesOnlyNewTargets)
{
    VNX::MixingEditorWidget editor;
    editor.setBoundItem(container_);
    ASSERT_TRUE(editor.commitAddRow(varWind_->guid));
    drainEvents();
    ASSERT_TRUE(editor.commitAddChild(0, childA_->guid));
    drainEvents();
    ASSERT_TRUE(editor.canvas(0)->setPointCoordinates(0, 10.0f, 25.0f));
    drainEvents();
    ASSERT_TRUE(editor.canvas(0)->setPointCoordinates(1, 70.0f, 75.0f));
    drainEvents();
    const auto authored = selection()->mixing().toJson().first().toObject()
        .value(QStringLiteral("children")).toArray().first().toObject();
    auto* variable = editor.findChild<VNX::ObjectRefWidget*>(QStringLiteral("mixingRowVariable"));
    ASSERT_NE(variable, nullptr);
    emit variable->objectChanged(0);
    drainEvents();
    ASSERT_TRUE(editor.mixingDef().rows.front().variable.isNull());
    EXPECT_FALSE(editor.canvas(0)->isEnabled());
    EXPECT_FLOAT_EQ(editor.canvas(0)->curve().front().x, 10.0f);
    EXPECT_FLOAT_EQ(editor.canvas(0)->curve().front().value, 25.0f);
    EXPECT_FLOAT_EQ(editor.childGains().value(childA_->guid), 1.0f);
    EXPECT_EQ(selection()->mixing().toJson().first().toObject()
        .value(QStringLiteral("children")).toArray().first().toObject(), authored);
    ASSERT_TRUE(editor.commitAddChild(0, childB_->guid));
    drainEvents();
    const auto cleared = selection()->selections.toJson();
    EXPECT_TRUE(editor.mixingDef().rows.front().children[1].points.isEmpty());
    variable = editor.findChild<VNX::ObjectRefWidget*>(QStringLiteral("mixingRowVariable"));
    ASSERT_NE(variable, nullptr);
    emit variable->objectChanged(varWind_->objectID);
    drainEvents();
    EXPECT_EQ(editor.mixingDef().rows.front().variable, varWind_->guid);
    EXPECT_TRUE(editor.canvas(0)->isEnabled());
    EXPECT_EQ(selection()->mixing().toJson().first().toObject()
        .value(QStringLiteral("children")).toArray().first().toObject(), authored);
    const auto initialized = editor.mixingDef().rows.front().children[1];
    ASSERT_EQ(initialized.points.size(), 2);
    EXPECT_FLOAT_EQ(initialized.points.front().x, varWind_->comp<VNX::VVariable>()->rangeMin);
    EXPECT_FLOAT_EQ(initialized.points.back().x, varWind_->comp<VNX::VVariable>()->rangeMax);
    EXPECT_FLOAT_EQ(initialized.points.front().value, 1.0f);
    EXPECT_FLOAT_EQ(initialized.points.back().value, 1.0f);
    undoStack()->undo();
    drainEvents();
    EXPECT_EQ(selection()->selections.toJson(), cleared);
    EXPECT_FALSE(editor.canvas(0)->isEnabled());
    EXPECT_FLOAT_EQ(editor.childGains().value(childB_->guid), 1.0f);
    undoStack()->redo();
    drainEvents();
    EXPECT_TRUE(editor.canvas(0)->isEnabled());
    EXPECT_EQ(selection()->mixing().toJson().first().toObject()
        .value(QStringLiteral("children")).toArray().first().toObject(), authored);
}

TEST_F(MixingEditorWidgetTest, UnboundRowsKeepTheirChildrenAndCurvesAcrossReorderingAndUndo)
{
    VNX::MixingEditorWidget editor;
    editor.setBoundItem(container_);
    auto* add = editor.findChild<QAction*>(QStringLiteral("mixingAddEmptyRow"));
    ASSERT_NE(add, nullptr);
    for (int i = 0; i < 2; ++i) {
        add->trigger();
        drainEvents();
    }
    ASSERT_EQ(editor.mixingDef().rows.size(), 2u);
    const auto first = editor.mixingDef().rows[0].guid;
    const auto second = editor.mixingDef().rows[1].guid;
    EXPECT_NE(first, second);
    ASSERT_TRUE(editor.commitAddChild(0, childA_->guid));
    drainEvents();
    ASSERT_TRUE(editor.commitAddChild(1, childB_->guid));
    drainEvents();
    ASSERT_TRUE(editor.commitReplaceVariable(0, varWind_->guid));
    drainEvents();
    ASSERT_TRUE(editor.commitReplaceVariable(1, varSurface_->guid));
    drainEvents();
    ASSERT_TRUE(editor.commitCurve(0, 0, {{0.0f, 25.0f}, {3.0f, 25.0f}},
        {{1.0f, 25.0f, 2.0f, 25.0f}}, QStringLiteral("Set Mixing Curve")));
    ASSERT_TRUE(editor.commitCurve(1, 0, {{0.0f, 75.0f}, {3.0f, 75.0f}},
        {{1.0f, 75.0f, 2.0f, 75.0f}}, QStringLiteral("Set Mixing Curve")));
    ASSERT_TRUE(editor.commitReplaceVariable(0, {}));
    drainEvents();
    ASSERT_TRUE(editor.commitReplaceVariable(1, {}));
    drainEvents();
    const auto unbound = selection()->selections.toJson();
    ASSERT_TRUE(editor.commitMoveRow(0, 1));
    drainEvents();
    EXPECT_EQ(editor.mixingDef().rows[0].guid, second);
    EXPECT_EQ(editor.mixingDef().rows[1].guid, first);
    ASSERT_TRUE(editor.commitReplaceVariable(0, varSurface_->guid));
    drainEvents();
    ASSERT_TRUE(editor.commitReplaceVariable(1, varWind_->guid));
    drainEvents();
    ASSERT_NE(editor.canvas(0), nullptr);
    ASSERT_NE(editor.canvas(1), nullptr);
    EXPECT_FLOAT_EQ(editor.canvas(0)->curve().front().value, 75.0f);
    EXPECT_FLOAT_EQ(editor.canvas(1)->curve().front().value, 25.0f);
    EXPECT_EQ(editor.mixingDef().rows[0].children.front().target, childB_->guid);
    EXPECT_EQ(editor.mixingDef().rows[1].children.front().target, childA_->guid);
    const auto rebound = selection()->selections.toJson();
    for (int i = 0; i < 3; ++i) {
        undoStack()->undo();
        drainEvents();
    }
    EXPECT_EQ(selection()->selections.toJson(), unbound);
    for (int i = 0; i < 3; ++i) {
        undoStack()->redo();
        drainEvents();
    }
    EXPECT_EQ(selection()->selections.toJson(), rebound);
}

TEST_F(MixingEditorWidgetTest, FreeCurvesUsePercentagesAndMultiplyAcrossRows)
{
    VNX::MixingEditorWidget editor;
    editor.setBoundItem(container_);
    for (const auto* variable : {varWind_, varSurface_}) {
        ASSERT_TRUE(editor.commitAddRow(variable->guid));
        drainEvents();
        ASSERT_TRUE(editor.commitAddChild(static_cast<int>(editor.mixingDef().rows.size()) - 1, childA_->guid));
        drainEvents();
    }
    const auto saved = selection()->mixing().toJson();
    for (int i = 0; i < 2; ++i) {
        auto* canvas = editor.canvas(i);
        ASSERT_NE(canvas, nullptr);
        EXPECT_FLOAT_EQ(canvas->profile().yMax, 100.0f);
        ASSERT_TRUE(canvas->setPointCoordinates(0, canvas->xMin(), 50.0f));
        drainEvents();
        ASSERT_TRUE(editor.canvas(i)->setPointCoordinates(1, editor.canvas(i)->xMax(), 50.0f));
        drainEvents();
    }
    EXPECT_FLOAT_EQ(editor.childGains().value(childA_->guid), 0.25f);
    EXPECT_EQ(selection()->mixing().rows[0].children[0].points.size(), 2);
    const auto oldName = childA_->name;
    childA_->name = QStringLiteral("RenamedMixingTarget");
    emit VNX::ProjectManager::instance().itemRenamed(childA_, oldName);
    auto& theme = VNX::ThemeManager::instance();
    emit theme.themeChanged(theme.snapshot());
    drainEvents();
    const auto references = editor.findChildren<VNX::ObjectRefWidget*>(QStringLiteral("mixingChildRef"));
    ASSERT_EQ(references.size(), 2);
    for (const auto* reference : references) {
        EXPECT_TRUE(reference->toolTip().contains(childA_->name));
        EXPECT_FALSE(reference->toolTip().contains(oldName));
        const auto* label = reference->findChild<QLabel*>(QStringLiteral("refName"));
        ASSERT_NE(label, nullptr);
        EXPECT_EQ(label->toolTip(), reference->toolTip());
    }
    for (int i = 0; i < 4; ++i) undoStack()->undo();
    drainEvents();
    EXPECT_EQ(selection()->mixing().toJson(), saved);
}

TEST_F(MixingEditorWidgetTest, ChangingVariableRangeDoesNotRewriteAuthoredCoordinates)
{
    VNX::MixingEditorWidget editor;
    editor.setBoundItem(container_);
    ASSERT_TRUE(editor.commitAddRow(varWind_->guid));
    drainEvents();
    ASSERT_TRUE(editor.commitAddChild(0, childA_->guid));
    drainEvents();
    auto mixing = selection()->mixing();
    auto& authored = mixing.rows[0].children[0];
    authored.points = {{0.0f, 1.0f}, {60.0f, 0.37123456f}, {100.0f, 1.0f}};
    authored.handles.clear();
    authored.normalize();
    ASSERT_TRUE(selection()->selections.shared.setMixing(mixing));
    editor.reload();
    const auto original = selection()->mixing().toJson();
    ASSERT_TRUE(editor.canvas(0)->setPointCoordinates(1, 59.0f, editor.canvas(0)->curve()[1].value));
    EXPECT_EQ(selection()->mixing().rows[0].children[0].points[1].value, 0.37123456f);
    undoStack()->undo();
    drainEvents();
    EXPECT_EQ(selection()->mixing().toJson(), original);
    varWind_->comp<VNX::VVariable>()->rangeMax = 40.0f;
    emit VNX::ProjectManager::instance().itemPropertyChanged(varWind_, QStringLiteral("rangeMax"));
    drainEvents();
    EXPECT_FLOAT_EQ(editor.canvas(0)->xMax(), 40.0f);
    EXPECT_EQ(selection()->mixing().toJson(), original);
    EXPECT_FLOAT_EQ(editor.canvas(0)->curve().back().x, 100.0f);
    ASSERT_TRUE(editor.canvas(0)->setPointCoordinates(0, 0.0f, 25.0f));
    EXPECT_FLOAT_EQ(selection()->mixing().rows[0].children[0].points.back().x, 100.0f);
    EXPECT_EQ(selection()->mixing().rows[0].children[0].points[1].value, 0.37123456f);
    ASSERT_TRUE(editor.canvas(0)->setPointCoordinates(1, 60.0f, 25.0f));
    EXPECT_FLOAT_EQ(selection()->mixing().rows[0].children[0].points[1].x, 60.0f);

    editor.setAttribute(Qt::WA_DontShowOnScreen);
    editor.resize(900, 420);
    editor.show();
    drainEvents();
    auto* canvas = editor.canvas(0);
    canvas->setSelectedPointIndices({0, 1, 2}, 0);
    const QPoint start = canvas->renderData().points.front().position.toPoint();
    const QPoint end = start + QPoint(0, 15);
    QMouseEvent press(QEvent::MouseButtonPress, start, canvas->mapToGlobal(start),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &press);
    QMouseEvent move(QEvent::MouseMove, end, canvas->mapToGlobal(end),
        Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, end, canvas->mapToGlobal(end),
        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &release);
    const auto edited = selection()->mixing().rows[0].children[0].points;
    EXPECT_FLOAT_EQ(edited[0].x, 0.0f);
    EXPECT_FLOAT_EQ(edited[1].x, 60.0f);
    EXPECT_FLOAT_EQ(edited[2].x, 100.0f);
    EXPECT_LT(edited[0].value, 0.25f);
}

TEST_F(MixingEditorWidgetTest, VariableKindRefreshKeepsCurvesAndMatchesRehearsal)
{
    auto* variable = varWind_->comp<VNX::VVariable>();
    variable->rangeMax = 3.0f;
    variable->labels = {{0, QStringLiteral("Calm")}, {1, QStringLiteral("Breeze")},
                        {2, QStringLiteral("Gale")}, {3, QStringLiteral("Storm")}};
    VNX::MixingDef mixing;
    mixing.rows = {{varWind_->guid, {
        {childA_->guid, {{0.25f, 0.4f}, {2.25f, 0.8f}}, {}},
        {childB_->guid, {{0.0f, 1.0f}, {3.0f, 0.0f}}, {}}}}};
    for (auto& child : mixing.rows[0].children) child.normalize();
    ASSERT_TRUE(selection()->selections.shared.setMixing(mixing));
    const auto authored = selection()->mixing().toJson();
    const int undoCount = undoStack()->count();
    auto& manager = VNX::ProjectManager::instance();
    manager.variableRehearsal().setOverride(varWind_->guid, 1.7f);
    VNX::MixingEditorWidget editor;
    editor.setBoundItem(container_);
    editor.selectChild(0, 0);
    applyStudioStyle(editor);
    editor.setAttribute(Qt::WA_DontShowOnScreen);
    editor.resize(760, 420);
    editor.show();
    drainEvents();
    EXPECT_FALSE(editor.canvas(0)->renderData().discretePath.has_value());
    EXPECT_NEAR(editor.childGains().value(childA_->guid), 0.69f, 0.0001f);

    variable->kind = VNX::VariableKind::Step;
    emit manager.itemPropertyChanged(varWind_, QStringLiteral("kind"));
    drainEvents();
    auto* canvas = editor.canvas(0);
    const auto data = canvas->renderData();
    ASSERT_TRUE(data.discretePath.has_value());
    ASSERT_EQ(data.overlays.size(), 1);
    ASSERT_TRUE(data.overlays[0].discretePath.has_value());
    EXPECT_EQ(canvas->playhead(), std::optional<float>(2.0f));
    EXPECT_NEAR(editor.childGains().value(childA_->guid), 0.75f, 0.0001f);
    EXPECT_NEAR(editor.childGains().value(childB_->guid), 1.0f / 3.0f, 0.0001f);
    QStringList labels;
    for (const auto& tick : data.textTicks)
        if (tick.rect.top() > data.graphRect.bottom()) labels.append(tick.text);
    for (const auto& label : variable->labels) EXPECT_TRUE(labels.contains(label.name));
    const auto pick = QPointF(data.graphRect.left() + data.graphRect.width() * 2.4 / 3.0,
                              data.graphRect.bottom() - data.graphRect.height() * (100.0 / 3.0) / 100.0);
    QMouseEvent press(QEvent::MouseButtonPress, pick, canvas->mapToGlobal(pick.toPoint()),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, pick, canvas->mapToGlobal(pick.toPoint()),
        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &release);
    EXPECT_FLOAT_EQ(editor.canvas(0)->curve().front().value, 100.0f);

    variable->labels[2].name = QStringLiteral("Strong");
    emit manager.itemPropertyChanged(varWind_, QStringLiteral("labels"));
    drainEvents();
    labels.clear();
    for (const auto& tick : editor.canvas(0)->renderData().textTicks) labels.append(tick.text);
    EXPECT_TRUE(labels.contains(QStringLiteral("Strong")));
    EXPECT_FALSE(labels.contains(QStringLiteral("Gale")));
    variable->kind = VNX::VariableKind::Smooth;
    emit manager.itemPropertyChanged(varWind_, QStringLiteral("kind"));
    drainEvents();
    const auto smooth = editor.canvas(0)->renderData();
    EXPECT_FALSE(smooth.discretePath.has_value());
    EXPECT_FALSE(smooth.overlays[0].discretePath.has_value());
    for (const auto& tick : smooth.textTicks)
        if (tick.rect.top() > smooth.graphRect.bottom()) {
            bool numeric = false;
            tick.text.toDouble(&numeric);
            EXPECT_TRUE(numeric);
        }
    EXPECT_EQ(editor.canvas(0)->playhead(), std::optional<float>(1.7f));
    EXPECT_NEAR(editor.childGains().value(childA_->guid), 0.69f, 0.0001f);
    EXPECT_EQ(selection()->mixing().toJson(), authored);
    EXPECT_EQ(undoStack()->count(), undoCount);
}

TEST_F(MixingEditorWidgetTest, RowsFitPanelAndUseEditableObjectReferences)
{
    VNX::MixingEditorWidget editor;
    editor.setBoundItem(container_);
    ASSERT_TRUE(editor.commitAddRow(varWind_->guid));
    drainEvents();
    ASSERT_TRUE(editor.commitAddChild(0, childA_->guid));
    drainEvents();
    ASSERT_TRUE(editor.commitAddChild(0, childB_->guid));
    drainEvents();
    applyStudioStyle(editor);
    editor.setAttribute(Qt::WA_DontShowOnScreen);
    editor.resize(510, 554);
    editor.show();
    drainEvents();
    auto* view = editor.findChild<VNX::ResizableTableView*>();
    ASSERT_NE(view, nullptr);
    auto* ref = editor.findChild<VNX::ObjectRefWidget*>(QStringLiteral("mixingRowVariable"));
    ASSERT_NE(ref, nullptr);
    EXPECT_FALSE(ref->isReadOnly());
    EXPECT_EQ(ref->currentObjectID(), varWind_->objectID);
    for (int width : {680, 400, 1000}) {
        editor.resize(width, 360);
        drainEvents();
        for (auto* scroll : editor.findChildren<QAbstractScrollArea*>()) {
            EXPECT_FALSE(scroll->horizontalScrollBar()->isVisible());
            EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
        }
        EXPECT_GT(editor.canvas(0)->renderData().graphRect.width(), 0.0);
        for (int column = 1; column < 3; ++column) {
            const auto index = view->model()->index(0, column);
            EXPECT_EQ(view->indexWidget(index)->geometry(), view->visualRect(index));
        }
        auto* page = view->indexWidget(view->model()->index(0, 1));
        auto* children = page->findChild<VNX::TableWidget*>();
        ASSERT_NE(children, nullptr);
        EXPECT_TRUE(page->rect().contains(ref->geometry()));
        EXPECT_TRUE(page->rect().contains(children->geometry()));
    }
    view->setRowHeight(0, 260);
    drainEvents();
    const auto handles = editor.findChildren<QWidget*>(QStringLiteral("tableRowHandle"));
    ASSERT_EQ(handles.size(), 2);
    const auto center = handles[0]->rect().center();
    QMouseEvent select(QEvent::MouseButtonPress, center, handles[0]->mapToGlobal(center),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(handles[0], &select);
    QMouseEvent releaseSelection(QEvent::MouseButtonRelease, center, handles[0]->mapToGlobal(center),
        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(handles[0], &releaseSelection);
    auto* page = view->indexWidget(view->model()->index(0, 1));
    auto* children = page->findChild<VNX::TableWidget*>();
    ASSERT_NE(children, nullptr);
    EXPECT_TRUE(children->rowForWidget(handles[0])->property("selected").toBool());
    EXPECT_FALSE(children->rowForWidget(handles[1])->property("selected").toBool());
    ASSERT_EQ(editor.canvas(0)->overlays().size(), 1);
    EXPECT_TRUE(editor.canvas(0)->overlays()[0].selectable);
    const auto beforeMove = selection()->mixing().toJson();
    auto* targetRow = children->rowForWidget(handles[1]);
    const QPoint destination = targetRow->mapToGlobal(QPoint(targetRow->width() / 2, targetRow->height() - 2));
    QMouseEvent dragPress(QEvent::MouseButtonPress, center, handles[0]->mapToGlobal(center),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(handles[0], &dragPress);
    QMouseEvent dragMove(QEvent::MouseMove, handles[0]->mapFromGlobal(destination), destination,
        Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(handles[0], &dragMove);
    QMouseEvent dragRelease(QEvent::MouseButtonRelease, handles[0]->mapFromGlobal(destination), destination,
        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(handles[0], &dragRelease);
    drainEvents();
    ASSERT_EQ(selection()->mixing().rows[0].children.size(), 2);
    EXPECT_EQ(selection()->mixing().rows[0].children[0].target, childB_->guid);
    EXPECT_EQ(selection()->mixing().rows[0].children[1].target, childA_->guid);
    page = view->indexWidget(view->model()->index(0, 1));
    children = page->findChild<VNX::TableWidget*>();
    const auto movedHandles = children->findChildren<QWidget*>(QStringLiteral("tableRowHandle"));
    ASSERT_EQ(movedHandles.size(), 2);
    EXPECT_TRUE(children->rowForWidget(movedHandles[1])->property("selected").toBool());
    undoStack()->undo();
    drainEvents();
    EXPECT_EQ(selection()->mixing().toJson(), beforeMove);
    editor.selectChild(0, -1);
    editor.reload();
    drainEvents();
    EXPECT_EQ(editor.canvas(0)->overlays().size(), 2);
    EXPECT_TRUE(editor.canvas(0)->renderData().points.isEmpty());
    auto* canvas = editor.canvas(0);
    const auto graph = canvas->renderData().graphRect;
    const QPoint pick(qRound(graph.center().x()), qRound(graph.top()));
    QMouseEvent press(QEvent::MouseButtonPress, pick, canvas->mapToGlobal(pick),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &press);
    EXPECT_EQ(editor.canvas(0)->overlays().size(), 1);
    EXPECT_FALSE(editor.canvas(0)->renderData().points.isEmpty());
}

TEST_F(MixingEditorWidgetTest, CompositeRowsKeepTheirLayoutAfterResizingAndThemeReload)
{
    VNX::MixingDef mixing;
    mixing.rows = {
        {varWind_->guid, {
            {childA_->guid, {{10.0f, 0.0f}, {30.0f, 0.85f}, {55.0f, 0.0f}}, {}},
            {childB_->guid, {{40.0f, 0.0f}, {70.0f, 1.0f}, {95.0f, 0.35f}}, {}}}},
        {varSurface_->guid, {
            {childA_->guid, {{0.0f, 0.3f}, {50.0f, 1.0f}, {100.0f, 0.3f}}, {}}}}};
    for (auto& row : mixing.rows)
        for (auto& child : row.children) child.normalize();
    ASSERT_TRUE(selection()->selections.shared.setMixing(mixing));
    VNX::MixingEditorWidget editor;
    editor.setBoundItem(container_);
    applyStudioStyle(editor);
    editor.setAttribute(Qt::WA_DontShowOnScreen);
    editor.resize(760, 640);
    editor.show();
    drainEvents();
    auto* view = editor.findChild<VNX::ResizableTableView*>();
    ASSERT_NE(view, nullptr);
    const auto expectLayout = [&] {
        EXPECT_EQ(view->columnWidth(0) + view->columnWidth(1) + view->columnWidth(2), view->viewport()->width());
        EXPECT_EQ(view->horizontalScrollBar()->maximum(), 0);
        for (int row = 0; row < 2; ++row) {
            for (int column = 1; column < 3; ++column) {
                const auto index = view->model()->index(row, column);
                EXPECT_EQ(view->indexWidget(index)->geometry(), view->visualRect(index));
            }
            auto* page = view->indexWidget(view->model()->index(row, 1));
            auto* variable = page->findChild<VNX::ObjectRefWidget*>(QStringLiteral("mixingRowVariable"));
            auto* children = page->findChild<VNX::TableWidget*>();
            ASSERT_NE(variable, nullptr);
            ASSERT_NE(children, nullptr);
            EXPECT_TRUE(page->rect().contains(variable->geometry()));
            EXPECT_TRUE(page->rect().contains(children->geometry()));
        }
    };
    editor.selectChild(0, 0);
    expectLayout();
    editor.selectChild(0, -1);
    editor.resize(510, 554);
    drainEvents();
    expectLayout();
    QWidget* rowHandle = nullptr;
    for (auto* widget : view->viewport()->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly))
        if (widget->property("tableResizeSection").isValid()
            && widget->property("tableResizeSection").toInt() == 0
            && !widget->property("tableResizeColumn").toBool()) rowHandle = widget;
    ASSERT_NE(rowHandle, nullptr);
    const int before = view->rowHeight(0);
    const auto start = rowHandle->rect().center();
    const auto global = rowHandle->mapToGlobal(start);
    const QPoint delta(0, 72);
    QMouseEvent press(QEvent::MouseButtonPress, start, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(rowHandle, &press);
    QMouseEvent move(QEvent::MouseMove, start + delta, global + delta, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(rowHandle, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, rowHandle->mapFromGlobal(global + delta), global + delta,
        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(rowHandle, &release);
    drainEvents();
    EXPECT_EQ(view->rowHeight(0), before + delta.y());
    ASSERT_TRUE(VNX::ThemeManager::instance().reloadTheme().ok());
    drainEvents();
    EXPECT_EQ(view->rowHeight(0), before + delta.y());
    expectLayout();
    editor.resize(400, 420);
    drainEvents();
    view->setColumnWidth(1, 150);
    editor.selectChild(1, 0);
    drainEvents();
    view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
    drainEvents();
    expectLayout();
    const auto canvasPosition = editor.canvas(1)->mapTo(view->viewport(), QPoint());
    EXPECT_TRUE(view->viewport()->rect().contains(QRect(canvasPosition, editor.canvas(1)->size())));
}

TEST_F(MixingEditorWidgetTest, RowNumberSelectionAndReorderingKeepTheContentUnhighlighted)
{
    VNX::MixingEditorWidget editor;
    editor.setBoundItem(container_);
    ASSERT_TRUE(editor.commitAddRow(varWind_->guid));
    drainEvents();
    ASSERT_TRUE(editor.commitAddChild(0, childA_->guid));
    drainEvents();
    ASSERT_TRUE(editor.commitAddRow(varSurface_->guid));
    drainEvents();
    ASSERT_TRUE(editor.commitAddChild(1, childB_->guid));
    drainEvents();
    applyStudioStyle(editor);
    editor.setAttribute(Qt::WA_DontShowOnScreen);
    editor.resize(760, 640);
    editor.show();
    drainEvents();
    auto* view = editor.findChild<VNX::ResizableTableView*>();
    ASSERT_NE(view, nullptr);
    ASSERT_TRUE(view->horizontalHeader()->isHidden());
    const auto clickNumber = [&](int row, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        const auto position = view->visualRect(view->model()->index(row, 0)).center();
        const auto global = view->viewport()->mapToGlobal(position);
        QMouseEvent press(QEvent::MouseButtonPress, position, global, Qt::LeftButton, Qt::LeftButton, modifiers);
        QApplication::sendEvent(view->viewport(), &press);
        QMouseEvent release(QEvent::MouseButtonRelease, position, global, Qt::LeftButton, Qt::NoButton, modifiers);
        QApplication::sendEvent(view->viewport(), &release);
        drainEvents();
    };
    clickNumber(0);
    ASSERT_TRUE(view->selectionModel()->isRowSelected(0));
    clickNumber(0, Qt::ControlModifier);
    EXPECT_FALSE(view->selectionModel()->hasSelection());
    clickNumber(0);
    const auto before = selection()->mixing().toJson();
    const int undoCount = undoStack()->count();
    const auto start = view->visualRect(view->model()->index(0, 0)).center();
    const auto target = view->visualRect(view->model()->index(1, 0));
    const QPoint end(target.center().x(), target.bottom() - 10);
    QMouseEvent press(QEvent::MouseButtonPress, start, view->viewport()->mapToGlobal(start),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &press);
    QMouseEvent move(QEvent::MouseMove, end, view->viewport()->mapToGlobal(end),
        Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &move);
    auto* indicator = view->viewport()->findChild<QFrame*>(QStringLiteral("tableRowInsertion"));
    ASSERT_NE(indicator, nullptr);
    EXPECT_TRUE(indicator->isVisible());
    QMouseEvent release(QEvent::MouseButtonRelease, end, view->viewport()->mapToGlobal(end),
        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &release);
    drainEvents();
    EXPECT_EQ(undoStack()->count(), undoCount + 1);
    EXPECT_EQ(selection()->mixing().rows[1].variable, varWind_->guid);
    EXPECT_EQ(selection()->mixing().rows[1].children[0].target, childA_->guid);
    EXPECT_EQ(view->currentIndex().row(), 1);
    undoStack()->undo();
    drainEvents();
    EXPECT_EQ(selection()->mixing().toJson(), before);
}

TEST_F(MixingEditorWidgetTest, CurveIntervalsMoveAsOneUndoStepAndCancelWithoutWriting)
{
    VNX::MixingEditorWidget editor;
    editor.setBoundItem(container_);
    ASSERT_TRUE(editor.commitAddRow(varWind_->guid));
    drainEvents();
    ASSERT_TRUE(editor.commitAddChild(0, childA_->guid));
    drainEvents();
    auto* canvas = qobject_cast<VNX::MixingRowCanvas*>(editor.canvas(0));
    ASSERT_NE(canvas, nullptr);
    ASSERT_TRUE(canvas->setPointCoordinates(0, 20.0f, 40.0f));
    ASSERT_TRUE(canvas->setPointCoordinates(1, 70.0f, 80.0f));
    const auto before = selection()->mixing().toJson();
    const auto original = selection()->mixing().rows[0].children[0];
    EXPECT_FLOAT_EQ(original.gainAt(19.0f), 0.0f);
    EXPECT_FLOAT_EQ(original.gainAt(71.0f), 0.0f);
    EXPECT_FLOAT_EQ(original.gainAt(20.0f), 0.4f);
    EXPECT_FLOAT_EQ(original.gainAt(70.0f), 0.8f);
    editor.setAttribute(Qt::WA_DontShowOnScreen);
    editor.resize(900, 420);
    editor.show();
    drainEvents();
    const auto send = [&](QEvent::Type type, const QPointF& position) {
        QMouseEvent event(type, position, canvas->mapToGlobal(position.toPoint()),
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &event);
    };
    const QPointF start = canvas->clipTitleRect(0).center();
    QMouseEvent hover(QEvent::MouseMove, start, canvas->mapToGlobal(start.toPoint()),
        Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &hover);
    EXPECT_EQ(canvas->cursor().shape(), Qt::SizeHorCursor);
    const QPointF end = start + QPointF(canvas->renderData().graphRect.width() / 10.0, 25.0);
    const int undoCount = undoStack()->count();
    send(QEvent::MouseButtonPress, start);
    send(QEvent::MouseMove, end);
    EXPECT_EQ(selection()->mixing().toJson(), before);
    send(QEvent::MouseButtonRelease, end);
    EXPECT_EQ(undoStack()->count(), undoCount + 1);
    const auto moved = selection()->mixing().rows[0].children[0];
    EXPECT_NEAR(moved.points.front().x, 30.0f, 0.001f);
    EXPECT_NEAR(moved.points.back().x, 80.0f, 0.001f);
    for (int i = 0; i < original.points.size(); ++i) EXPECT_EQ(moved.points[i].value, original.points[i].value);
    for (int i = 0; i < original.handles.size(); ++i) {
        EXPECT_NEAR(moved.handles[i].h1x - original.handles[i].h1x, 10.0f, 0.001f);
        EXPECT_NEAR(moved.handles[i].h2x - original.handles[i].h2x, 10.0f, 0.001f);
        EXPECT_EQ(moved.handles[i].h1y, original.handles[i].h1y);
        EXPECT_EQ(moved.handles[i].h2y, original.handles[i].h2y);
    }
    const auto committed = selection()->mixing().toJson();
    const auto movedTitle = canvas->clipTitleRect(0).center();
    send(QEvent::MouseButtonPress, movedTitle);
    send(QEvent::MouseMove, movedTitle + QPointF(30, 0));
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(canvas, &escape);
    send(QEvent::MouseButtonRelease, movedTitle + QPointF(30, 0));
    EXPECT_EQ(selection()->mixing().toJson(), committed);
    EXPECT_EQ(undoStack()->count(), undoCount + 1);
    undoStack()->undo();
    drainEvents();
    EXPECT_EQ(selection()->mixing().toJson(), before);
}

TEST_F(MixingEditorWidgetTest, InFlightCurveEditIsCanceledWhenRebinding)
{
    VNX::MixingEditorWidget editor;
    editor.setBoundItem(container_);
    ASSERT_TRUE(editor.commitAddRow(varWind_->guid));
    drainEvents();
    ASSERT_TRUE(editor.commitAddChild(0, childA_->guid));
    drainEvents();
    editor.resize(900, 420);
    editor.setAttribute(Qt::WA_DontShowOnScreen);
    editor.show();
    drainEvents();
    const auto before = selection()->mixing().toJson();
    const int undoCount = undoStack()->count();
    auto* canvas = editor.canvas(0);
    const QPoint start = canvas->renderData().points.front().position.toPoint();
    const QPoint end = start + QPoint(0, 50);
    QMouseEvent press(QEvent::MouseButtonPress, start, canvas->mapToGlobal(start),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &press);
    QMouseEvent move(QEvent::MouseMove, end, canvas->mapToGlobal(end),
        Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &move);
    editor.setBoundItem(nullptr);
    QMouseEvent release(QEvent::MouseButtonRelease, end, canvas->mapToGlobal(end),
        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &release);
    drainEvents();
    EXPECT_EQ(selection()->mixing().toJson(), before);
    EXPECT_EQ(undoStack()->count(), undoCount);
}

TEST_F(MixingEditorWidgetTest, NestedSelectionKeepsChildAndVariableRemovalSeparate)
{
    VNX::MixingEditorWidget editor;
    editor.setBoundItem(container_);
    for (const auto* variable : {varWind_, varSurface_}) {
        ASSERT_TRUE(editor.commitAddRow(variable->guid));
        drainEvents();
        ASSERT_TRUE(editor.commitAddChild(int(editor.mixingDef().rows.size()) - 1, childA_->guid));
        drainEvents();
    }
    editor.setAttribute(Qt::WA_DontShowOnScreen);
    editor.resize(900, 700);
    editor.show();
    drainEvents();
    auto* view = editor.findChild<VNX::ResizableTableView*>();
    ASSERT_NE(view, nullptr);
    auto* first = view->indexWidget(view->model()->index(0, 1));
    auto* handle = first->findChild<QWidget*>(QStringLiteral("tableRowHandle"));
    ASSERT_NE(handle, nullptr);
    const auto center = handle->rect().center();
    QMouseEvent press(QEvent::MouseButtonPress, center, handle->mapToGlobal(center),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(handle, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, center, handle->mapToGlobal(center),
        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(handle, &release);
    EXPECT_EQ(view->currentIndex().row(), 0);
    first->findChild<VNX::TableWidget*>()->footer()->removeButton()->click();
    drainEvents();
    ASSERT_EQ(selection()->mixing().rows.size(), 2);
    EXPECT_TRUE(selection()->mixing().rows[0].children.empty());
    EXPECT_EQ(selection()->mixing().rows[1].children.size(), 1);
    auto* table = qobject_cast<VNX::TableWidget*>(view->parentWidget());
    ASSERT_NE(table, nullptr);
    ASSERT_TRUE(table->footer()->removeButton()->isEnabled());
    table->footer()->removeButton()->click();
    drainEvents();
    ASSERT_EQ(selection()->mixing().rows.size(), 1);
    EXPECT_EQ(selection()->mixing().rows[0].variable, varSurface_->guid);
    EXPECT_EQ(selection()->mixing().rows[0].children.size(), 1);
    undoStack()->undo();
    undoStack()->undo();
    drainEvents();
    EXPECT_EQ(selection()->mixing().rows.size(), 2);
    EXPECT_EQ(selection()->mixing().rows[0].children.size(), 1);
}
