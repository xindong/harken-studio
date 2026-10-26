#include "ActionManager.h"
#include "ActionStepUtils.h"
#include "ItemTypeCatalog.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "Components/VAction.h"
#include "Components/VVariable.h"
#include "Widgets/Inspector/ActionInspectorSchema.h"
#include "Widgets/Inspector/InspectorCustomization.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/InspectorCustomizationRegistry.h"
#include "Widgets/Inspector/PropertyEditorEngine.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterControlHost.h"
#include "Widgets/Inspector/Providers/StepsSectionProvider.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Selector/SelectorWindow.h"
#include "Widgets/Shared/ProjectItemMime.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDropEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

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

VNX::VActionStep postActionStep(const ProjectItem* target = nullptr)
{
    VNX::VActionStep step;
    step.type = VNX::VActionStep::Type::PostAction;
    if (target) {
        step.targetObjectID = target->objectID;
        step.targetName = target->name;
    }
    return step;
}

bool dropTarget(VNX::ObjectRefWidget& widget, uint32_t objectID)
{
    QMimeData mime;
    VNX::ProjectItemMime::writeObjectIDs(&mime, {objectID});
    QDropEvent event(QPointF(5, 5), Qt::CopyAction, &mime,
                     Qt::LeftButton, Qt::NoModifier);
    static_cast<QObject&>(widget).event(&event);
    return event.isAccepted();
}

VNX::SelectorWindow* visibleSelector()
{
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        auto* selector = qobject_cast<VNX::SelectorWindow*>(widget);
        if (selector && selector->isVisible())
            return selector;
    }
    return nullptr;
}

QTreeWidgetItem* selectorRow(VNX::SelectorWindow* selector, uint32_t objectID)
{
    auto* tree = selector ? selector->findChild<QTreeWidget*>() : nullptr;
    if (!tree) return nullptr;
    for (QTreeWidgetItemIterator row(tree); *row; ++row)
        if ((*row)->data(0, VNX::kRoleObjectID).toUInt() == objectID)
            return *row;
    return nullptr;
}
} // namespace

class StepsChipTargetDomainTest : public ::testing::Test
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
        config.name = QStringLiteral("StepsChipProject");
        config.location = tempDir.path();
        ASSERT_TRUE(pm.createProject(config));
    }

    void TearDown() override
    {
        VNX::SelectorWindow::dismiss();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
    }

    QTemporaryDir tempDir;
};

namespace {

struct ChipDomainFixtureItems
{
    ProjectItem* sound = nullptr;
    ProjectItem* container = nullptr;
    ProjectItem* track = nullptr;
    ProjectItem* piece = nullptr;
    ProjectItem* layer = nullptr;
    ProjectItem* bus = nullptr;
    ProjectItem* targetAction = nullptr;
    ProjectItem* variable = nullptr;
    ProjectItem* attenuation = nullptr;
    ProjectItem* action = nullptr;
};

ChipDomainFixtureItems buildChipDomainFixture(VNX::ProjectManager& pm)
{
    ChipDomainFixtureItems f;
    f.sound = pm.createItem(nullptr, QStringLiteral("Chip_Sound"), ProjectItem::Type::Sound);
    f.container = pm.createItem(nullptr, QStringLiteral("Chip_Container"),
                                ProjectItem::Type::Container);
    f.track = pm.createItem(nullptr, QStringLiteral("Chip_Track"), ProjectItem::Type::Track);
    f.piece = pm.createItem(nullptr, QStringLiteral("Chip_Piece"), ProjectItem::Type::Piece);
    f.layer = pm.createItem(nullptr, QStringLiteral("Chip_Layer"), ProjectItem::Type::Layer);
    ProjectItem* master = pm.getMasterBusItem();
    f.bus = master ? pm.createItem(master, QStringLiteral("Chip_Bus"), ProjectItem::Type::Bus)
                   : nullptr;
    f.targetAction = pm.createItem(nullptr, QStringLiteral("Chip_TargetAction"),
                                   ProjectItem::Type::Action);
    f.variable = pm.createItem(nullptr, QStringLiteral("Chip_Variable"),
                               ProjectItem::Type::Variable);
    f.attenuation = pm.createItem(nullptr, QStringLiteral("Chip_Attenuation"),
                                  ProjectItem::Type::Attenuation);
    f.action = pm.createItem(nullptr, QStringLiteral("Chip_EditedAction"),
                             ProjectItem::Type::Action);
    return f;
}

} // namespace

TEST_F(StepsChipTargetDomainTest, ChipsGateTargetsByVerbAcrossNineTypes)
{
    auto& pm = VNX::ProjectManager::instance();
    const ChipDomainFixtureItems f = buildChipDomainFixture(pm);
    ASSERT_NE(f.bus, nullptr);

    auto* comp = f.action->comp<VNX::VAction>();
    ASSERT_NE(comp, nullptr);

    VNX::VActionStep play;
    play.type = VNX::VActionStep::Type::Play;
    VNX::VActionStep stop;
    stop.type = VNX::VActionStep::Type::Stop;
    VNX::VActionStep setValue;
    setValue.type = VNX::VActionStep::Type::SetVariable;
    VNX::VActionStep post;
    post.type = VNX::VActionStep::Type::PostAction;
    VNX::VActionStep reset;
    reset.type = VNX::VActionStep::Type::Reset;
    comp->steps = { play, stop, setValue, post, reset };

    VNX::StepsSectionProvider provider;
    std::unique_ptr<QWidget> w(provider.build(nullptr));
    ASSERT_NE(w, nullptr);
    provider.bind(w.get(), f.action);

    auto* playChip = w->findChild<VNX::ObjectRefWidget*>(QStringLiteral("stepTargetChip_0"));
    auto* stopChip = w->findChild<VNX::ObjectRefWidget*>(QStringLiteral("stepTargetChip_1"));
    auto* setValueChip = w->findChild<VNX::ObjectRefWidget*>(QStringLiteral("stepTargetChip_2"));
    auto* postChip = w->findChild<VNX::ObjectRefWidget*>(QStringLiteral("stepTargetChip_3"));
    auto* resetChip = w->findChild<VNX::ObjectRefWidget*>(QStringLiteral("stepTargetChip_4"));
    ASSERT_NE(playChip, nullptr);
    ASSERT_NE(stopChip, nullptr);
    ASSERT_NE(setValueChip, nullptr);
    ASSERT_NE(postChip, nullptr);
    ASSERT_NE(resetChip, nullptr);

    const struct {
        ProjectItem* item;
        bool play;
        bool stop;
        bool setValue;
        bool post;
        bool reset;
    } rows[] = {
        { f.sound,        true,  true,  false, false, false },
        { f.container,    true,  true,  false, false, true  },
        { f.track,        true,  true,  false, false, false },
        { f.piece,        true,  true,  false, false, false },
        { f.layer,        false, false, false, false, false },
        { f.bus,          false, false, false, false, false },
        { f.targetAction, false, false, false, true,  false },
        { f.variable,     false, false, true,  false, false },
        { f.attenuation,  false, false, false, false, false },
    };

    for (const auto& row : rows)
    {
        ASSERT_NE(row.item, nullptr);
        ASSERT_NE(row.item->objectID, 0u);
        const std::string name =
            VNX::ItemTypeCatalog::canonicalName(row.item->type).toStdString();
        EXPECT_EQ(playChip->canAcceptObject(row.item->objectID), row.play)
            << "Play chip vs " << name;
        EXPECT_EQ(stopChip->canAcceptObject(row.item->objectID), row.stop)
            << "Stop chip vs " << name;
        EXPECT_EQ(setValueChip->canAcceptObject(row.item->objectID), row.setValue)
            << "Set Variable chip vs " << name;
        EXPECT_EQ(postChip->canAcceptObject(row.item->objectID), row.post)
            << "Post Action chip vs " << name;
        EXPECT_EQ(resetChip->canAcceptObject(row.item->objectID), row.reset)
            << "Reset chip vs " << name;
    }

    EXPECT_FALSE(postChip->canAcceptObject(f.action->objectID))
        << "Post Action chip must reject the edited action itself";
}

TEST_F(StepsChipTargetDomainTest, PostActionRejectsCyclesInPickerAndDropWithoutClearingTarget)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* action = pm.createItem(nullptr, QStringLiteral("Cycle_A"), ProjectItem::Type::Action);
    auto* reciprocal = pm.createItem(nullptr, QStringLiteral("Cycle_B"), ProjectItem::Type::Action);
    auto* indirect = pm.createItem(nullptr, QStringLiteral("Cycle_C"), ProjectItem::Type::Action);
    auto* safe = pm.createItem(nullptr, QStringLiteral("Cycle_Safe"), ProjectItem::Type::Action);
    ASSERT_NE(action, nullptr);
    ASSERT_NE(reciprocal, nullptr);
    ASSERT_NE(indirect, nullptr);
    ASSERT_NE(safe, nullptr);
    action->comp<VNX::VAction>()->steps = {postActionStep(safe)};
    reciprocal->comp<VNX::VAction>()->steps = {postActionStep(action)};
    indirect->comp<VNX::VAction>()->steps = {postActionStep(reciprocal)};

    VNX::StepsSectionProvider provider;
    std::unique_ptr<QWidget> widget(provider.build(nullptr));
    provider.bind(widget.get(), action);
    auto* chip = widget->findChild<VNX::ObjectRefWidget*>(QStringLiteral("stepTargetChip_0"));
    ASSERT_NE(chip, nullptr);

    for (auto* target : {action, reciprocal, indirect}) {
        EXPECT_FALSE(chip->canAcceptObject(target->objectID));
        EXPECT_FALSE(dropTarget(*chip, target->objectID));
        EXPECT_EQ(chip->currentObjectID(), safe->objectID);
        ASSERT_EQ(action->comp<VNX::VAction>()->steps.size(), 1u);
        EXPECT_EQ(action->comp<VNX::VAction>()->steps[0].targetObjectID, safe->objectID);
    }

    chip->openSelector();
    auto* selector = visibleSelector();
    ASSERT_NE(selector, nullptr);
    for (auto* target : {action, reciprocal, indirect}) {
        auto* row = selectorRow(selector, target->objectID);
        ASSERT_NE(row, nullptr);
        EXPECT_FALSE(row->data(0, VNX::kRoleSelectable).toBool());
        EXPECT_TRUE(row->toolTip(0).contains(QStringLiteral("PostAction dependency cycle")));
        EXPECT_TRUE(row->toolTip(0).contains(action->name));
    }
    auto* safeRow = selectorRow(selector, safe->objectID);
    ASSERT_NE(safeRow, nullptr);
    EXPECT_TRUE(safeRow->data(0, VNX::kRoleSelectable).toBool());
}

TEST_F(StepsChipTargetDomainTest, PostActionPickerRechecksChangedGraphAndRefreshesAvailability)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* action = pm.createItem(nullptr, QStringLiteral("Live_A"), ProjectItem::Type::Action);
    auto* target = pm.createItem(nullptr, QStringLiteral("Live_B"), ProjectItem::Type::Action);
    auto* safe = pm.createItem(nullptr, QStringLiteral("Live_Safe"), ProjectItem::Type::Action);
    ASSERT_NE(action, nullptr);
    ASSERT_NE(target, nullptr);
    ASSERT_NE(safe, nullptr);
    action->comp<VNX::VAction>()->steps = {postActionStep(safe)};

    VNX::StepsSectionProvider provider;
    std::unique_ptr<QWidget> widget(provider.build(nullptr));
    provider.bind(widget.get(), action);
    auto* chip = widget->findChild<VNX::ObjectRefWidget*>(QStringLiteral("stepTargetChip_0"));
    ASSERT_NE(chip, nullptr);
    chip->openSelector();
    auto* selector = visibleSelector();
    ASSERT_NE(selector, nullptr);
    auto* row = selectorRow(selector, target->objectID);
    ASSERT_NE(row, nullptr);
    ASSERT_TRUE(row->data(0, VNX::kRoleSelectable).toBool());
    row->treeWidget()->setCurrentItem(row);
    target->comp<VNX::VAction>()->steps = {postActionStep(action)};
    selector->accept();
    EXPECT_EQ(chip->currentObjectID(), safe->objectID);
    EXPECT_EQ(action->comp<VNX::VAction>()->steps[0].targetObjectID, safe->objectID);

    chip->openSelector();
    selector = visibleSelector();
    ASSERT_NE(selector, nullptr);
    row = selectorRow(selector, target->objectID);
    ASSERT_NE(row, nullptr);
    EXPECT_FALSE(row->data(0, VNX::kRoleSelectable).toBool());
    target->comp<VNX::VAction>()->steps = {postActionStep(safe)};
    emit pm.itemPropertyChanged(target, QStringLiteral("steps"), nullptr);
    QCoreApplication::processEvents();
    row = selectorRow(selector, target->objectID);
    ASSERT_NE(row, nullptr);
    EXPECT_TRUE(row->data(0, VNX::kRoleSelectable).toBool());
    row->treeWidget()->setCurrentItem(row);
    selector->accept();
    EXPECT_EQ(chip->currentObjectID(), target->objectID);
    EXPECT_EQ(action->comp<VNX::VAction>()->steps[0].targetObjectID, target->objectID);
}

TEST_F(StepsChipTargetDomainTest, RebindingActionInvalidatesPendingPostActionSelection)
{
    auto& pm = VNX::ProjectManager::instance();
    auto* first = pm.createItem(nullptr, QStringLiteral("Rebind_A"), ProjectItem::Type::Action);
    auto* second = pm.createItem(nullptr, QStringLiteral("Rebind_B"), ProjectItem::Type::Action);
    auto* target = pm.createItem(nullptr, QStringLiteral("Rebind_Target"), ProjectItem::Type::Action);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(target, nullptr);
    first->comp<VNX::VAction>()->steps = {postActionStep()};
    second->comp<VNX::VAction>()->steps = {postActionStep()};

    VNX::StepsSectionProvider provider;
    std::unique_ptr<QWidget> widget(provider.build(nullptr));
    provider.bind(widget.get(), first);
    auto* oldChip = widget->findChild<VNX::ObjectRefWidget*>(QStringLiteral("stepTargetChip_0"));
    ASSERT_NE(oldChip, nullptr);
    oldChip->openSelector();
    auto* selector = visibleSelector();
    ASSERT_NE(selector, nullptr);
    auto* row = selectorRow(selector, target->objectID);
    ASSERT_NE(row, nullptr);
    row->treeWidget()->setCurrentItem(row);

    provider.bind(widget.get(), second);
    EXPECT_FALSE(selector->isVisible());
    EXPECT_FALSE(oldChip->canAcceptObject(target->objectID));
    EXPECT_FALSE(dropTarget(*oldChip, target->objectID));
    selector->accept();
    EXPECT_EQ(first->comp<VNX::VAction>()->steps[0].targetObjectID, 0u);
    EXPECT_EQ(second->comp<VNX::VAction>()->steps[0].targetObjectID, 0u);
}

TEST_F(StepsChipTargetDomainTest, TypeMenuIsCatalogDriven)
{
    auto& pm = VNX::ProjectManager::instance();
    ProjectItem* action = pm.createItem(nullptr, QStringLiteral("Catalog_Action"),
                                        ProjectItem::Type::Action);
    ASSERT_NE(action, nullptr);
    auto* comp = action->comp<VNX::VAction>();
    ASSERT_NE(comp, nullptr);
    VNX::VActionStep play;
    play.type = VNX::VActionStep::Type::Play;
    comp->steps = { play };

    VNX::StepsSectionProvider provider;
    std::unique_ptr<QWidget> w(provider.build(nullptr));
    provider.bind(w.get(), action);

    auto* combo = w->findChild<QComboBox*>(QStringLiteral("stepTypeCombo_0"));
    ASSERT_NE(combo, nullptr);

    const auto& catalog = VNX::EditorUtils::actionStepCatalog();
    ASSERT_EQ(combo->count(), static_cast<int>(catalog.size()));
    for (int i = 0; i < combo->count(); ++i) {
        EXPECT_EQ(combo->itemText(i), catalog[i].name);
        EXPECT_EQ(combo->itemData(i).toInt(), static_cast<int>(catalog[i].type));
    }
}

TEST_F(StepsChipTargetDomainTest, BindingPreservesHiddenPlayQuantizeReference)
{
    auto& pm = VNX::ProjectManager::instance();
    const ChipDomainFixtureItems f = buildChipDomainFixture(pm);
    auto* comp = f.action->comp<VNX::VAction>();
    ASSERT_NE(comp, nullptr);
    VNX::VActionStep play;
    play.type = VNX::VActionStep::Type::Play;
    play.targetObjectID = f.sound->objectID;
    play.quantize.timing = VNX::QuantizeTiming::NextMarker;
    play.quantize.referenceObjectID = f.sound->objectID;
    play.quantize.referenceName = f.sound->name;
    play.quantize.ifSilent = VNX::QuantizeIfSilent::Drop;
    comp->steps = { play };

    VNX::StepsSectionProvider provider;
    std::unique_ptr<QWidget> w(provider.build(nullptr));
    provider.bind(w.get(), f.action);

    EXPECT_EQ(w->findChild<VNX::ObjectRefWidget*>(
                  QStringLiteral("stepQuantizeReference_0")), nullptr);
    ASSERT_EQ(comp->steps.size(), 1u);
    EXPECT_EQ(comp->steps[0].quantize.timing, VNX::QuantizeTiming::NextMarker);
    EXPECT_EQ(comp->steps[0].quantize.referenceObjectID, f.sound->objectID);
    EXPECT_EQ(comp->steps[0].quantize.referenceName, f.sound->name);
    EXPECT_EQ(comp->steps[0].quantize.ifSilent, VNX::QuantizeIfSilent::Drop);
}

TEST_F(StepsChipTargetDomainTest, SetValueDetailRangesFromVariableDeclaration)
{
    auto& pm = VNX::ProjectManager::instance();
    ProjectItem* variable = pm.createItem(nullptr, QStringLiteral("Ranged_Variable"),
                                          ProjectItem::Type::Variable);
    ASSERT_NE(variable, nullptr);
    auto* var = variable->comp<VNX::VVariable>();
    ASSERT_NE(var, nullptr);
    var->kind = VNX::VariableKind::Step;
    var->rangeMin = -5.0f;
    var->rangeMax = 5.0f;

    ProjectItem* action = pm.createItem(nullptr, QStringLiteral("SetValue_Action"),
                                        ProjectItem::Type::Action);
    ASSERT_NE(action, nullptr);
    auto* comp = action->comp<VNX::VAction>();
    ASSERT_NE(comp, nullptr);
    VNX::VActionStep setValue;
    setValue.type = VNX::VActionStep::Type::SetVariable;
    setValue.targetObjectID = variable->objectID;
    comp->steps = { setValue };

    VNX::StepsSectionProvider provider;
    std::unique_ptr<QWidget> w(provider.build(nullptr));
    provider.bind(w.get(), action);

    auto* host = w->findChild<VNX::ParameterControlHost*>(
        QStringLiteral("stepValueHost_0"));
    ASSERT_NE(host, nullptr);
    EXPECT_FLOAT_EQ(static_cast<float>(host->binding()->minimum()), -5.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(host->binding()->maximum()), 5.0f);
    EXPECT_EQ(host->binding()->decimals(), 0);
    EXPECT_DOUBLE_EQ(host->binding()->quantum(), 1.0);
}

TEST_F(StepsChipTargetDomainTest, ResetDetailExposesScopeToggle)
{
    auto& pm = VNX::ProjectManager::instance();
    ProjectItem* action = pm.createItem(nullptr, QStringLiteral("Reset_Action"),
                                        ProjectItem::Type::Action);
    ASSERT_NE(action, nullptr);
    auto* comp = action->comp<VNX::VAction>();
    ASSERT_NE(comp, nullptr);
    VNX::VActionStep reset;
    reset.type = VNX::VActionStep::Type::Reset;
    reset.resetScope = VNX::ResetScope::Global;
    comp->steps = { reset };

    VNX::StepsSectionProvider provider;
    std::unique_ptr<QWidget> w(provider.build(nullptr));
    provider.bind(w.get(), action);

    auto* combo = w->findChild<QComboBox*>(QStringLiteral("stepResetScopeCombo_0"));
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(combo->currentData().toInt(), static_cast<int>(VNX::ResetScope::Global));

    combo->setCurrentIndex(0);
    ASSERT_EQ(comp->steps.size(), 1u);
    EXPECT_EQ(comp->steps[0].resetScope, VNX::ResetScope::Emitter);
}

TEST_F(StepsChipTargetDomainTest, VisibleEditsPreserveHiddenMarkerTimingAndAbsentBehavior)
{
    auto& pm = VNX::ProjectManager::instance();
    const ChipDomainFixtureItems f = buildChipDomainFixture(pm);
    auto* comp = f.action->comp<VNX::VAction>();
    ASSERT_NE(comp, nullptr);
    VNX::VActionStep play;
    play.type = VNX::VActionStep::Type::Play;
    play.targetObjectID = f.sound->objectID;
    play.targetName = f.sound->name;
    play.quantize.timing = VNX::QuantizeTiming::NextMarker;
    play.quantize.referenceObjectID = f.sound->objectID;
    play.quantize.referenceName = f.sound->name;
    play.quantize.ifSilent = VNX::QuantizeIfSilent::Drop;
    VNX::VActionStep stop = play;
    stop.type = VNX::VActionStep::Type::Stop;
    stop.quantize.timing = VNX::QuantizeTiming::ExitMarker;
    comp->steps = { play, stop };

    VNX::StepsSectionProvider provider;
    std::unique_ptr<QWidget> w(provider.build(nullptr));
    provider.bind(w.get(), f.action);

    auto* delay = w->findChild<QDoubleSpinBox*>(QStringLiteral("stepDelaySpin_0"));
    auto* fade = w->findChild<QDoubleSpinBox*>(QStringLiteral("stepFadeSpin_1"));
    auto* target = w->findChild<VNX::ObjectRefWidget*>(QStringLiteral("stepTargetChip_0"));
    ASSERT_NE(delay, nullptr);
    ASSERT_NE(fade, nullptr);
    ASSERT_NE(target, nullptr);
    delay->setValue(0.25);
    fade->setValue(120.0);
    emit target->objectChanged(f.container->objectID);

    ASSERT_EQ(comp->steps.size(), 2u);
    EXPECT_FLOAT_EQ(comp->steps[0].delay, 250.0f);
    EXPECT_FLOAT_EQ(comp->steps[1].fadeTime, 120.0f);
    EXPECT_EQ(comp->steps[0].targetObjectID, f.container->objectID);
    EXPECT_EQ(comp->steps[0].quantize.timing, VNX::QuantizeTiming::NextMarker);
    EXPECT_EQ(comp->steps[1].quantize.timing, VNX::QuantizeTiming::ExitMarker);
    for (const auto& step : comp->steps) {
        EXPECT_EQ(step.quantize.referenceObjectID, f.sound->objectID);
        EXPECT_EQ(step.quantize.referenceName, f.sound->name);
        EXPECT_EQ(step.quantize.ifSilent, VNX::QuantizeIfSilent::Drop);
    }
}

TEST_F(StepsChipTargetDomainTest, TypeSwitchPreservesQuantizeAndClampsExclusiveFields)
{
    auto& pm = VNX::ProjectManager::instance();
    ProjectItem* action = pm.createItem(nullptr, QStringLiteral("Switch_Action"),
                                        ProjectItem::Type::Action);
    ProjectItem* piece = pm.createItem(nullptr, QStringLiteral("Switch_Piece"),
                                       ProjectItem::Type::Piece);
    ASSERT_NE(action, nullptr);
    ASSERT_NE(piece, nullptr);
    auto* comp = action->comp<VNX::VAction>();
    ASSERT_NE(comp, nullptr);
    VNX::VActionStep play;
    play.type = VNX::VActionStep::Type::Play;
    play.quantize.timing = VNX::QuantizeTiming::NextMarker;
    play.quantize.referenceObjectID = piece->objectID;
    play.quantize.referenceName = piece->name;
    comp->steps = { play };

    VNX::StepsSectionProvider provider;
    std::unique_ptr<QWidget> w(provider.build(nullptr));
    provider.bind(w.get(), action);

    auto* combo = w->findChild<QComboBox*>(QStringLiteral("stepTypeCombo_0"));
    ASSERT_NE(combo, nullptr);

    int stopIdx = combo->findData(static_cast<int>(VNX::VActionStep::Type::Stop));
    ASSERT_GE(stopIdx, 0);
    combo->setCurrentIndex(stopIdx);
    ASSERT_EQ(comp->steps.size(), 1u);
    EXPECT_EQ(comp->steps[0].type, VNX::VActionStep::Type::Stop);
    EXPECT_EQ(comp->steps[0].quantize.timing, VNX::QuantizeTiming::NextMarker);
    EXPECT_EQ(comp->steps[0].quantize.referenceObjectID, piece->objectID);
    EXPECT_EQ(comp->steps[0].quantize.referenceName, piece->name);

    auto* fadeSpin = w->findChild<QDoubleSpinBox*>(QStringLiteral("stepFadeSpin_0"));
    ASSERT_NE(fadeSpin, nullptr);
    fadeSpin->setValue(250.0);
    ASSERT_EQ(comp->steps.size(), 1u);
    EXPECT_FLOAT_EQ(comp->steps[0].fadeTime, 250.0f);

    combo = w->findChild<QComboBox*>(QStringLiteral("stepTypeCombo_0"));
    ASSERT_NE(combo, nullptr);
    int setValueIdx = combo->findData(static_cast<int>(VNX::VActionStep::Type::SetVariable));
    ASSERT_GE(setValueIdx, 0);
    combo->setCurrentIndex(setValueIdx);
    ASSERT_EQ(comp->steps.size(), 1u);
    EXPECT_EQ(comp->steps[0].type, VNX::VActionStep::Type::SetVariable);
    EXPECT_TRUE(VNX::EditorUtils::isCanonicalActionQuantize(comp->steps[0].quantize));
    EXPECT_FLOAT_EQ(comp->steps[0].fadeTime, 0.0f);
}

TEST_F(StepsChipTargetDomainTest, DelayColumnIsAddressableByNameAndCommitsInMilliseconds)
{
    auto& pm = VNX::ProjectManager::instance();
    ProjectItem* action = pm.createItem(nullptr, QStringLiteral("Delay_Action"),
                                        ProjectItem::Type::Action);
    ASSERT_NE(action, nullptr);
    auto* comp = action->comp<VNX::VAction>();
    ASSERT_NE(comp, nullptr);
    VNX::VActionStep first;
    first.type = VNX::VActionStep::Type::Play;
    VNX::VActionStep second = first;
    second.delay = 250.0f;
    comp->steps = { first, second };

    VNX::StepsSectionProvider provider;
    std::unique_ptr<QWidget> w(provider.build(nullptr));
    provider.bind(w.get(), action);

    auto* firstDelay = w->findChild<QDoubleSpinBox*>(QStringLiteral("stepDelaySpin_0"));
    auto* secondDelay = w->findChild<QDoubleSpinBox*>(QStringLiteral("stepDelaySpin_1"));
    ASSERT_NE(firstDelay, nullptr);
    ASSERT_NE(secondDelay, nullptr);
    EXPECT_DOUBLE_EQ(secondDelay->value(), 0.25);

    firstDelay->setValue(1.5);
    ASSERT_EQ(comp->steps.size(), 2u);
    EXPECT_FLOAT_EQ(comp->steps[0].delay, 1500.0f);
    EXPECT_FLOAT_EQ(comp->steps[1].delay, 250.0f);
}
