#include "ActionManager.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "Components/VSelection.h"
#include "Components/VVariable.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

#include <utility>
#include <vector>

namespace VNX {
bool claimsVariableItemProperty(ProjectItem*, const QString&);
ActionResult handleVariableItemProperty(ProjectManager*, ProjectItem*, const QString&,
                                        const QJsonObject&, const QObject*);
bool claimsSelectionItemProperty(ProjectItem*, const QString&);
ActionResult handleSelectionItemProperty(ProjectManager*, ProjectItem*, const QString&,
                                         const QJsonObject&, const QObject*);
}

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

QJsonObject valueArgs(const QJsonValue& value)
{
    return QJsonObject{{QStringLiteral("value"), value}};
}

QJsonArray labelsJson(std::initializer_list<std::pair<int, const char*>> labels)
{
    QJsonArray out;
    for (const auto& l : labels)
        out.append(QJsonObject{{QStringLiteral("anchor"), l.first},
                               {QStringLiteral("name"), QString::fromLatin1(l.second)}});
    return out;
}

void expectLabels(const std::vector<VNX::VariableLabel>& actual,
                  std::initializer_list<std::pair<int, const char*>> expected)
{
    ASSERT_EQ(actual.size(), expected.size());
    size_t i = 0;
    for (const auto& e : expected)
    {
        EXPECT_EQ(actual[i].anchor, e.first) << "label " << i;
        EXPECT_EQ(actual[i].name, QString::fromLatin1(e.second)) << "label " << i;
        ++i;
    }
}

int undoStackCount()
{
    return VNX::UndoRedoManager::instance().undoStack()->count();
}

} // namespace

class VariableModeCommandTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
        ASSERT_TRUE(tempDir.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("VariableModeProject");
        config.location = tempDir.path();
        ASSERT_TRUE(pm.createProject(config));
        VNX::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
    }

    VNX::ProjectItem* makeVariable(const QString& name)
    {
        auto* item = VNX::ProjectManager::instance().createItem(
            nullptr, name, VNX::ProjectItem::Type::Variable);
        EXPECT_NE(item, nullptr);
        return item;
    }

    VNX::ProjectItem* makeContainer(const QString& name)
    {
        auto* item = VNX::ProjectManager::instance().createItem(
            nullptr, name, VNX::ProjectItem::Type::Container);
        EXPECT_NE(item, nullptr);
        return item;
    }

    QTemporaryDir tempDir;
};

TEST_F(VariableModeCommandTest, VariableClaimCoversDeclarationNamesOnVariableItemsOnly)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* variable = makeVariable(QStringLiteral("VM_ClaimVar"));
    VNX::ProjectItem* sound = pm.createItem(nullptr, QStringLiteral("VM_ClaimSound"),
                                            VNX::ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);

    for (const auto name : { QStringLiteral("kind"), QStringLiteral("rangeMin"),
                             QStringLiteral("rangeMax"), QStringLiteral("range"),
                             QStringLiteral("defaultValue"),
                             QStringLiteral("labels") })
    {
        EXPECT_TRUE(VNX::claimsVariableItemProperty(variable, name)) << name.toStdString();
        EXPECT_FALSE(VNX::claimsVariableItemProperty(sound, name)) << name.toStdString();
    }
    EXPECT_FALSE(VNX::claimsVariableItemProperty(variable, QStringLiteral("volume")));
    EXPECT_FALSE(VNX::claimsVariableItemProperty(variable, QStringLiteral("notes")));
    EXPECT_FALSE(VNX::claimsVariableItemProperty(variable, QStringLiteral("Kind")));
    EXPECT_FALSE(VNX::claimsVariableItemProperty(variable, QStringLiteral("scope")));
}

TEST_F(VariableModeCommandTest, SelectionClaimCoversModeOnSelectionItemsOnly)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* container = makeContainer(QStringLiteral("VM_ClaimCont"));
    VNX::ProjectItem* sound = pm.createItem(nullptr, QStringLiteral("VM_ClaimSound2"),
                                            VNX::ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);

    EXPECT_TRUE(VNX::claimsSelectionItemProperty(container, QStringLiteral("mode")));
    EXPECT_TRUE(VNX::claimsSelectionItemProperty(container, QStringLiteral("playType")));
    EXPECT_FALSE(VNX::claimsSelectionItemProperty(sound, QStringLiteral("playType")));
    EXPECT_FALSE(VNX::claimsSelectionItemProperty(sound, QStringLiteral("mode")));
    EXPECT_FALSE(VNX::claimsSelectionItemProperty(container, QStringLiteral("kind")));
    EXPECT_FALSE(VNX::claimsSelectionItemProperty(container, QStringLiteral("Mode")));
}

TEST_F(VariableModeCommandTest, KindSwitchNormalizesGroupAndUndoRestoresWholeDeclaration)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* item = makeVariable(QStringLiteral("VM_KindVar"));
    auto* var = item->comp<VNX::VVariable>();
    ASSERT_NE(var, nullptr);
    var->rangeMin = 0.4f;
    var->rangeMax = 99.6f;
    var->defaultValue = 2.5f;
    var->labels = { {0, QStringLiteral("Off")}, {3, QStringLiteral("High")} };

    const VNX::ActionResult r = VNX::handleVariableItemProperty(
        &pm, item, QStringLiteral("kind"), valueArgs(0), nullptr);

    EXPECT_TRUE(r.success);
    EXPECT_EQ(var->kind, VNX::VariableKind::Step);
    EXPECT_FLOAT_EQ(var->rangeMin, 0.0f);
    EXPECT_FLOAT_EQ(var->rangeMax, 100.0f);
    EXPECT_FLOAT_EQ(var->defaultValue, 3.0f);
    ASSERT_EQ(var->labels.size(), 2u);
    EXPECT_TRUE(item->isDirtyObject());
    ASSERT_TRUE(static_cast<bool>(r.undoFn));
    ASSERT_TRUE(static_cast<bool>(r.redoFn));

    r.undoFn();
    EXPECT_EQ(var->kind, VNX::VariableKind::Smooth);
    EXPECT_FLOAT_EQ(var->rangeMin, 0.4f);
    EXPECT_FLOAT_EQ(var->rangeMax, 99.6f);
    EXPECT_FLOAT_EQ(var->defaultValue, 2.5f);
    ASSERT_EQ(var->labels.size(), 2u);
    EXPECT_EQ(var->labels[1].anchor, 3);
    EXPECT_EQ(var->labels[1].name, QStringLiteral("High"));

    r.redoFn();
    EXPECT_EQ(var->kind, VNX::VariableKind::Step);
    EXPECT_FLOAT_EQ(var->rangeMin, 0.0f);
    EXPECT_FLOAT_EQ(var->rangeMax, 100.0f);
    EXPECT_FLOAT_EQ(var->defaultValue, 3.0f);
    ASSERT_EQ(var->labels.size(), 2u);
}

TEST_F(VariableModeCommandTest, KindSwitchEmitsEachChangedFieldAndNamedFieldCarriesOriginator)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* item = makeVariable(QStringLiteral("VM_EmitVar"));
    auto* var = item->comp<VNX::VVariable>();
    var->rangeMin = 0.4f;
    var->rangeMax = 99.6f;
    var->defaultValue = 2.5f;

    QObject sentinel;
    struct Emission { QString field; const QObject* origin; };
    std::vector<Emission> emissions;
    const QMetaObject::Connection conn = QObject::connect(
        &pm, &VNX::ProjectManager::itemPropertyChanged,
        [&emissions](VNX::ProjectItem*, const QString& prop, const QObject* origin) {
            emissions.push_back({prop, origin});
        });

    const VNX::ActionResult r = VNX::handleVariableItemProperty(
        &pm, item, QStringLiteral("kind"), valueArgs(0), &sentinel);
    ASSERT_TRUE(r.success);

    ASSERT_EQ(emissions.size(), 4u);
    EXPECT_EQ(emissions[0].field, QStringLiteral("kind"));
    EXPECT_EQ(emissions[0].origin, &sentinel);
    EXPECT_EQ(emissions[1].field, QStringLiteral("rangeMin"));
    EXPECT_EQ(emissions[1].origin, nullptr);
    EXPECT_EQ(emissions[2].field, QStringLiteral("rangeMax"));
    EXPECT_EQ(emissions[2].origin, nullptr);
    EXPECT_EQ(emissions[3].field, QStringLiteral("defaultValue"));
    EXPECT_EQ(emissions[3].origin, nullptr);

    emissions.clear();
    r.undoFn();
    ASSERT_EQ(emissions.size(), 4u);
    for (const Emission& e : emissions)
        EXPECT_EQ(e.origin, nullptr) << e.field.toStdString();

    QObject::disconnect(conn);
}

TEST_F(VariableModeCommandTest, RangeMaxEditClampsDefaultValueAndUndoRestoresBoth)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* item = makeVariable(QStringLiteral("VM_RangeVar"));
    auto* var = item->comp<VNX::VVariable>();
    var->defaultValue = 80.0f;

    const VNX::ActionResult r = VNX::handleVariableItemProperty(
        &pm, item, QStringLiteral("rangeMax"), valueArgs(50.0), nullptr);

    EXPECT_TRUE(r.success);
    EXPECT_FLOAT_EQ(var->rangeMax, 50.0f);
    EXPECT_FLOAT_EQ(var->defaultValue, 50.0f);
    ASSERT_TRUE(static_cast<bool>(r.undoFn));

    r.undoFn();
    EXPECT_FLOAT_EQ(var->rangeMax, 100.0f);
    EXPECT_FLOAT_EQ(var->defaultValue, 80.0f);

    r.redoFn();
    EXPECT_FLOAT_EQ(var->rangeMax, 50.0f);
    EXPECT_FLOAT_EQ(var->defaultValue, 50.0f);
}

TEST_F(VariableModeCommandTest, RangeEditWritesBothBoundsInOneUndoStep)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* item = makeVariable(QStringLiteral("VM_RangePairVar"));
    auto* var = item->comp<VNX::VVariable>();
    var->defaultValue = 80.0f;

    QStringList changed;
    QObject scope;
    QObject::connect(&pm, &VNX::ProjectManager::itemPropertyChanged, &scope,
        [&changed, item](VNX::ProjectItem* changedItem, const QString& field, const QObject*) {
            if (changedItem == item) changed << field;
        });

    const VNX::ActionResult r = VNX::handleVariableItemProperty(
        &pm, item, QStringLiteral("range"),
        valueArgs(QJsonArray{20.0, 5.0}), nullptr);

    EXPECT_TRUE(r.success);
    EXPECT_FLOAT_EQ(var->rangeMin, 5.0f);
    EXPECT_FLOAT_EQ(var->rangeMax, 20.0f);
    EXPECT_FLOAT_EQ(var->defaultValue, 20.0f);
    EXPECT_EQ(changed, (QStringList{QStringLiteral("rangeMin"), QStringLiteral("rangeMax"),
                                    QStringLiteral("defaultValue")}));

    ASSERT_TRUE(static_cast<bool>(r.undoFn));
    r.undoFn();
    EXPECT_FLOAT_EQ(var->rangeMin, 0.0f);
    EXPECT_FLOAT_EQ(var->rangeMax, 100.0f);
    EXPECT_FLOAT_EQ(var->defaultValue, 80.0f);

    r.redoFn();
    EXPECT_FLOAT_EQ(var->rangeMin, 5.0f);
    EXPECT_FLOAT_EQ(var->rangeMax, 20.0f);
    EXPECT_FLOAT_EQ(var->defaultValue, 20.0f);
}

TEST_F(VariableModeCommandTest, DefaultValueOutOfRangeClampsThroughNormalize)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* item = makeVariable(QStringLiteral("VM_DefaultVar"));
    auto* var = item->comp<VNX::VVariable>();

    const VNX::ActionResult r = VNX::handleVariableItemProperty(
        &pm, item, QStringLiteral("defaultValue"), valueArgs(250.0), nullptr);

    EXPECT_TRUE(r.success);
    EXPECT_FLOAT_EQ(var->defaultValue, 100.0f);
    ASSERT_TRUE(static_cast<bool>(r.undoFn));

    r.undoFn();
    EXPECT_FLOAT_EQ(var->defaultValue, 0.0f);

    r.redoFn();
    EXPECT_FLOAT_EQ(var->defaultValue, 100.0f);
}

TEST_F(VariableModeCommandTest, DeclarationScopeIsNotEditable)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* item = makeVariable(QStringLiteral("VM_ScopeVar"));
    auto* var = item->comp<VNX::VVariable>();
    EXPECT_FALSE(var->toJson().contains(QStringLiteral("scope")));

    const VNX::ActionResult r = VNX::handleVariableItemProperty(
        &pm, item, QStringLiteral("scope"), valueArgs(1), nullptr);
    EXPECT_FALSE(r.success);
    EXPECT_FALSE(static_cast<bool>(r.undoFn));
    EXPECT_FALSE(static_cast<bool>(r.redoFn));
}

TEST_F(VariableModeCommandTest, LabelsAddEditRemoveRoundtrip)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* item = makeVariable(QStringLiteral("VM_LabelsVar"));
    auto* var = item->comp<VNX::VVariable>();
    ASSERT_TRUE(var->labels.empty());

    const VNX::ActionResult added = VNX::handleVariableItemProperty(
        &pm, item, QStringLiteral("labels"),
        valueArgs(labelsJson({{0, "Off"}, {1, "On"}})), nullptr);
    EXPECT_TRUE(added.success);
    expectLabels(var->labels, {{0, "Off"}, {1, "On"}});
    ASSERT_TRUE(static_cast<bool>(added.undoFn));

    added.undoFn();
    EXPECT_TRUE(var->labels.empty());
    added.redoFn();
    expectLabels(var->labels, {{0, "Off"}, {1, "On"}});

    const VNX::ActionResult edited = VNX::handleVariableItemProperty(
        &pm, item, QStringLiteral("labels"),
        valueArgs(labelsJson({{0, "Off"}, {2, "Loud"}})), nullptr);
    EXPECT_TRUE(edited.success);
    expectLabels(var->labels, {{0, "Off"}, {2, "Loud"}});

    edited.undoFn();
    expectLabels(var->labels, {{0, "Off"}, {1, "On"}});
    edited.redoFn();
    expectLabels(var->labels, {{0, "Off"}, {2, "Loud"}});

    const VNX::ActionResult removed = VNX::handleVariableItemProperty(
        &pm, item, QStringLiteral("labels"), valueArgs(QJsonArray()), nullptr);
    EXPECT_TRUE(removed.success);
    EXPECT_TRUE(var->labels.empty());

    removed.undoFn();
    expectLabels(var->labels, {{0, "Off"}, {2, "Loud"}});

    const VNX::ActionResult same = VNX::handleVariableItemProperty(
        &pm, item, QStringLiteral("labels"),
        valueArgs(labelsJson({{0, "Off"}, {2, "Loud"}})), nullptr);
    EXPECT_TRUE(same.success);
    EXPECT_FALSE(static_cast<bool>(same.undoFn));
}

TEST_F(VariableModeCommandTest, KindOutOfRangeFailsWithoutTouchingDeclaration)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* item = makeVariable(QStringLiteral("VM_BadKindVar"));
    auto* var = item->comp<VNX::VVariable>();

    const VNX::ActionResult r = VNX::handleVariableItemProperty(
        &pm, item, QStringLiteral("kind"), valueArgs(7), nullptr);

    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.errorCode, QStringLiteral("SET_PROPERTY_FAILED"));
    EXPECT_EQ(var->kind, VNX::VariableKind::Smooth);
    EXPECT_FLOAT_EQ(var->rangeMin, 0.0f);
    EXPECT_FLOAT_EQ(var->rangeMax, 100.0f);
    EXPECT_FALSE(static_cast<bool>(r.undoFn));
}

TEST_F(VariableModeCommandTest, ModeSameValueYieldsNoUndoAndOutOfRangeFails)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* item = makeContainer(QStringLiteral("VM_ModeSameCont"));
    auto* sel = item->comp<VNX::VSelection>();

    const VNX::ActionResult same = VNX::handleSelectionItemProperty(
        &pm, item, QStringLiteral("mode"), valueArgs(0), nullptr);
    EXPECT_TRUE(same.success);
    EXPECT_FALSE(static_cast<bool>(same.undoFn));
    EXPECT_FALSE(static_cast<bool>(same.redoFn));

    const VNX::ActionResult bad = VNX::handleSelectionItemProperty(
        &pm, item, QStringLiteral("mode"), valueArgs(9), nullptr);
    EXPECT_FALSE(bad.success);
    EXPECT_EQ(bad.errorCode, QStringLiteral("SET_PROPERTY_FAILED"));
    EXPECT_EQ(sel->mode, VNX::SelectionMode::Random);
}

TEST_F(VariableModeCommandTest, AudioRouteStillRejectsDeclarationAndModeNames)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* variable = makeVariable(QStringLiteral("VM_AudioRejVar"));
    VNX::ProjectItem* container = makeContainer(QStringLiteral("VM_AudioRejCont"));

    const VNX::ActionResult kindResult = handleAudioItemProperty(
        &pm, variable, QStringLiteral("kind"), valueArgs(0), nullptr);
    EXPECT_FALSE(kindResult.success);
    EXPECT_EQ(kindResult.errorCode, QStringLiteral("UNKNOWN_PROPERTY"));

    const VNX::ActionResult modeResult = handleAudioItemProperty(
        &pm, container, QStringLiteral("mode"), valueArgs(2), nullptr);
    EXPECT_FALSE(modeResult.success);
    EXPECT_EQ(modeResult.errorCode, QStringLiteral("UNKNOWN_PROPERTY"));
}

class VariableModeRouteTest : public VariableModeCommandTest
{
protected:
    void SetUp() override
    {
        VariableModeCommandTest::SetUp();
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerProjectActions(VNX::ActionManager::instance(),
                                    &VNX::ProjectManager::instance(), nullptr, {});
    }

    void TearDown() override
    {
        VNX::ActionManager::instance().clearAllForTesting();
        VariableModeCommandTest::TearDown();
    }
};

TEST_F(VariableModeRouteTest, SetItemPropertyRoutesDeclarationAndModeBeforeAudioFallback)
{
    VNX::ProjectItem* variable = makeVariable(QStringLiteral("VM_RouteVar"));
    VNX::ProjectItem* container = makeContainer(QStringLiteral("VM_RouteCont"));
    auto* var = variable->comp<VNX::VVariable>();
    auto* sel = container->comp<VNX::VSelection>();

    const int before = undoStackCount();
    const VNX::ActionResult kindResult = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{{QStringLiteral("authoring_handle"), static_cast<qint64>(variable->objectID)},
                    {QStringLiteral("property"), QStringLiteral("kind")},
                    {QStringLiteral("value"), 0}});
    EXPECT_TRUE(kindResult.success);
    EXPECT_EQ(var->kind, VNX::VariableKind::Step);
    EXPECT_EQ(undoStackCount(), before + 1);

    VNX::UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(var->kind, VNX::VariableKind::Smooth);
    VNX::UndoRedoManager::instance().undoStack()->redo();
    EXPECT_EQ(var->kind, VNX::VariableKind::Step);

    const VNX::ActionResult modeResult = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{{QStringLiteral("authoring_handle"), static_cast<qint64>(container->objectID)},
                    {QStringLiteral("property"), QStringLiteral("mode")},
                    {QStringLiteral("value"), static_cast<int>(VNX::SelectionMode::Sequence)}});
    EXPECT_TRUE(modeResult.success);
    EXPECT_EQ(sel->mode, VNX::SelectionMode::Sequence);
    EXPECT_EQ(undoStackCount(), before + 2);

    VNX::UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(sel->mode, VNX::SelectionMode::Random);

    const VNX::ActionResult unknown = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{{QStringLiteral("authoring_handle"), static_cast<qint64>(variable->objectID)},
                    {QStringLiteral("property"), QStringLiteral("bogus")},
                    {QStringLiteral("value"), 1}});
    EXPECT_FALSE(unknown.success);
    EXPECT_EQ(unknown.errorCode, QStringLiteral("UNKNOWN_PROPERTY"));
}
