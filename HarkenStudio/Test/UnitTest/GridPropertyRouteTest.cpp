#include "ActionManager.h"
#include "Components/VSelection.h"
#include "Components/VVariable.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "VariableConsumerSemantics.h"
#include "ItemTypeCatalog.h"
#include "NameValidator.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <utility>

// the whole-grid ledger entry on set_item_property 鈥?GUI and IPC
// share this one write path (D4). fromJson clamps value violations, the route
// rejects structural ones, and every accepted change is one undo step.

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

QString guidString(const VNX::ProjectItem* item)
{
    return item->guid.toString(QUuid::WithoutBraces);
}

QUndoStack* undoStack()
{
    return VNX::UndoRedoManager::instance().undoStack();
}

} // namespace

class GridPropertyRouteTest : public ::testing::Test
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

        ASSERT_TRUE(tempDir_.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("GridRouteProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(pm.createProject(config));

        container_ = pm.createItem(nullptr, QStringLiteral("GR_Container"),
                                   VNX::ProjectItem::Type::Container);
        ASSERT_NE(container_, nullptr);
        childA_ = pm.createItem(container_, QStringLiteral("GR_ChildA"),
                                VNX::ProjectItem::Type::Sound);
        childB_ = pm.createItem(container_, QStringLiteral("GR_ChildB"),
                                VNX::ProjectItem::Type::Sound);
        ASSERT_NE(childA_, nullptr);
        ASSERT_NE(childB_, nullptr);
        varA_ = pm.createItem(nullptr, QStringLiteral("GR_VarA"),
                              VNX::ProjectItem::Type::Variable);
        varB_ = pm.createItem(nullptr, QStringLiteral("GR_VarB"),
                              VNX::ProjectItem::Type::Variable);
        ASSERT_NE(varA_, nullptr);
        ASSERT_NE(varB_, nullptr);
        container_->comp<VNX::VSelection>()->mode = VNX::SelectionMode::Match;

        VNX::UndoRedoManager::instance().clear();
        container_->dirtyBit = false;
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
    }

    VNX::ActionResult setSelections(const QJsonValue& value, const QString& undoText = QString())
    {
        return VNX::ActionManager::instance().execute(QStringLiteral("set_item_property"),
            {{QStringLiteral("authoring_handle"), static_cast<qint64>(container_->objectID)},
             {QStringLiteral("property"), QStringLiteral("selections")},
             {QStringLiteral("value"), value}, {QStringLiteral("undo_text"), undoText}}, VNX::ActionSource::GUI);
    }

    VNX::ActionResult setGrid(const QJsonValue& gridJson, const QString& undoText = QString())
    {
        auto candidate = selection()->selections;
        if (selection()->mode == VNX::SelectionMode::Match) {
            if (!gridJson.isObject()) return setSelections(gridJson, undoText);
            const auto grid = VNX::DiscreteGridDef::fromJson(gridJson.toObject());
            if (!candidate.shared.setMatchGrid(grid))
                return {false, {}, "INVALID_ARGS", "Invalid match view"};
        } else {
            if (!gridJson.isArray()) return setSelections(gridJson, undoText);
            const auto mixing = VNX::MixingDef::fromJson(gridJson.toArray());
            if (!mixing || !candidate.shared.setMixing(*mixing))
                return {false, {}, "INVALID_ARGS", "Invalid mixing view"};
        }
        return setSelections(candidate.toJson(), undoText);
    }

    QJsonObject discreteGridJson()
    {
        QJsonObject discrete;
        discrete[QStringLiteral("axes")] =
            QJsonArray{guidString(varA_), guidString(varB_)};
        discrete[QStringLiteral("rows")] = QJsonArray{
            QJsonObject{{QStringLiteral("target"), guidString(childA_)},
                        {QStringLiteral("cells"), QJsonArray{1, 2}}},
            QJsonObject{{QStringLiteral("target"), guidString(childB_)},
                        {QStringLiteral("cells"), QJsonArray{QJsonValue(), 2}}}};
        return discrete;
    }

    QJsonArray mixingJson()
    {
        return VNX::MixingDef{{{varB_->guid,
            {VNX::MixingChildDef::unity(childB_->guid, 0.0f, 1.0f)}}}}.toJson();
    }

    const VNX::VSelection* selection() const
    {
        return container_->comp<VNX::VSelection>();
    }

    QTemporaryDir tempDir_;
    VNX::ProjectItem* container_ = nullptr;
    VNX::ProjectItem* childA_ = nullptr;
    VNX::ProjectItem* childB_ = nullptr;
    VNX::ProjectItem* varA_ = nullptr;
    VNX::ProjectItem* varB_ = nullptr;
};

TEST_F(GridPropertyRouteTest, WritesDiscreteGridAndMarksDirtyAsOneUndoStep)
{
    const int base = undoStack()->count();
    const auto result = setGrid(discreteGridJson());
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();

    const auto* sel = selection();
    ASSERT_NE(sel, nullptr);
    ASSERT_EQ(sel->matchGrid().axes.size(), 2u);
    EXPECT_EQ(sel->matchGrid().axes[0], varA_->guid);
    EXPECT_EQ(sel->matchGrid().axes[1], varB_->guid);
    ASSERT_EQ(sel->matchGrid().rows.size(), 2u);
    EXPECT_EQ(sel->matchGrid().rows[0].target, childA_->guid);
    EXPECT_EQ(VNX::GridSemantics::discreteCellValue(*sel->matchGrid().cellAt(0, 0),
        varA_->comp<VNX::VVariable>()), std::optional<int>(1));
    EXPECT_EQ(sel->matchGrid().cellAt(1, 0), std::nullopt);
    EXPECT_EQ(VNX::GridSemantics::discreteCellValue(*sel->matchGrid().cellAt(1, 1),
        varB_->comp<VNX::VVariable>()), std::optional<int>(2));
    EXPECT_TRUE(container_->isDirtyObject());
    EXPECT_EQ(undoStack()->count(), base + 1);
}

TEST_F(GridPropertyRouteTest, UndoRedoRoundtripRestoresBothStates)
{
    ASSERT_TRUE(setGrid(discreteGridJson()).success);

    const auto identity = selection()->matchGrid().cellAt(0, 1)->labelId;
    EXPECT_FALSE(identity.isNull());
    undoStack()->undo();
    EXPECT_TRUE(selection()->matchGrid().empty());
    EXPECT_TRUE(varA_->comp<VNX::VVariable>()->labels.empty());
    EXPECT_TRUE(varB_->comp<VNX::VVariable>()->labels.empty());

    undoStack()->redo();
    ASSERT_EQ(selection()->matchGrid().rows.size(), 2u);
    EXPECT_EQ(selection()->matchGrid().cellAt(0, 1)->labelId, identity);
    EXPECT_EQ(VNX::GridSemantics::discreteCellValue(*selection()->matchGrid().cellAt(0, 1),
        varB_->comp<VNX::VVariable>()), std::optional<int>(2));
}

TEST_F(GridPropertyRouteTest, LabelReorderingKeepsGridReferencesAndRejectsReferencedDeletion)
{
    auto* variable = varA_->comp<VNX::VVariable>();
    variable->labels = {{1, QStringLiteral("AAA")}, {2, QStringLiteral("BBB")}};
    const auto aaa = variable->labels[0].id;
    ASSERT_TRUE(setGrid(discreteGridJson()).success);
    const auto savedGrid = selection()->matchGrid().toJson();
    container_->comp<VNX::VSelection>()->mode = VNX::SelectionMode::Random;
    auto labels = variable->getField(QStringLiteral("labels")).toList();
    auto first = labels[0].toMap();
    auto second = labels[1].toMap();
    first[QStringLiteral("anchor")] = 2;
    second[QStringLiteral("anchor")] = 1;
    labels = {second, first};
    const auto edit = [&](const QVariantList& value) {
        return VNX::ActionManager::instance().execute(QStringLiteral("set_item_property"),
            QJsonObject{{QStringLiteral("authoring_handle"), static_cast<qint64>(varA_->objectID)},
                         {QStringLiteral("property"), QStringLiteral("labels")},
                         {QStringLiteral("value"), QJsonArray::fromVariantList(value)}}, VNX::ActionSource::GUI);
    };
    ASSERT_TRUE(edit(labels).success);
    EXPECT_EQ(selection()->matchGrid().toJson(), savedGrid);
    EXPECT_EQ(selection()->matchGrid().cellAt(0, 0)->labelId, aaa);
    EXPECT_EQ(VNX::GridSemantics::discreteCellValue(*selection()->matchGrid().cellAt(0, 0), variable), 2);
    EXPECT_FLOAT_EQ(variable->defaultValue, 0.0f);
    undoStack()->undo();
    EXPECT_EQ(VNX::GridSemantics::discreteCellValue(*selection()->matchGrid().cellAt(0, 0), variable), 1);
    undoStack()->redo();
    EXPECT_EQ(VNX::GridSemantics::discreteCellValue(*selection()->matchGrid().cellAt(0, 0), variable), 2);
    const auto rejected = edit({second});
    EXPECT_FALSE(rejected.success);
    ASSERT_EQ(variable->labels.size(), 2u);
    EXPECT_EQ(variable->labels[1].id, aaa);
}

TEST_F(GridPropertyRouteTest, ClampsMixingCurveGainAndKeepsNumericCoordinates)
{
    container_->comp<VNX::VSelection>()->mode = VNX::SelectionMode::Blend;
    VNX::MixingChildDef child{childA_->guid, {{20.0f, -1.0f}, {80.0f, 2.0f}},
        {{20.0f, -1.0f, 80.0f, 2.0f}}};
    const VNX::MixingDef mixing{{{varA_->guid, {child}}}};
    ASSERT_TRUE(setGrid(mixing.toJson()).success);
    const auto stored = selection()->mixing().rows.front().children.front();
    EXPECT_FLOAT_EQ(stored.points.front().x, 20.0f);
    EXPECT_FLOAT_EQ(stored.points.back().x, 80.0f);
    EXPECT_FLOAT_EQ(stored.points.front().value, 0.0f);
    EXPECT_FLOAT_EQ(stored.points.back().value, 1.0f);
}

TEST_F(GridPropertyRouteTest, RejectsMalformedSelectionDataAtomically)
{
    ASSERT_TRUE(setGrid(discreteGridJson()).success);
    const auto before = selection()->toJson();
    const int base = undoStack()->count();
    const auto reject = [&](const QJsonObject& shared) {
        auto data = selection()->selections.toJson();
        data[QStringLiteral("shared")] = shared;
        EXPECT_FALSE(setSelections(data).success);
        EXPECT_EQ(selection()->toJson(), before);
        EXPECT_EQ(undoStack()->count(), base);
    };
    auto shared = selection()->selections.shared.toJson();
    shared[QStringLiteral("matchAxes")] = QJsonArray{guidString(varA_), guidString(varA_)};
    reject(shared);
    shared = selection()->selections.shared.toJson();
    shared[QStringLiteral("matchAxes")] = QJsonArray{guidString(childA_), guidString(varB_)};
    reject(shared);
    shared = selection()->selections.shared.toJson();
    auto entries = shared[QStringLiteral("entries")].toArray();
    auto entry = entries[0].toObject();
    entry[QStringLiteral("childGuid")] = guidString(varA_);
    entries[0] = entry;
    shared[QStringLiteral("entries")] = entries;
    reject(shared);
    shared = selection()->selections.shared.toJson();
    entries = shared[QStringLiteral("entries")].toArray();
    entry = entries[0].toObject();
    auto conditions = entry[QStringLiteral("conditions")].toArray();
    auto condition = conditions[0].toObject();
    condition[QStringLiteral("cells")] = QJsonArray{1.5, QJsonValue::Null};
    conditions[0] = condition;
    entry[QStringLiteral("conditions")] = conditions;
    entries[0] = entry;
    shared[QStringLiteral("entries")] = entries;
    reject(shared);
    EXPECT_FALSE(setSelections(42).success);
    EXPECT_EQ(selection()->toJson(), before);
    EXPECT_EQ(undoStack()->count(), base);
}

TEST_F(GridPropertyRouteTest, IdenticalWriteAddsNoUndoStep)
{
    ASSERT_TRUE(setGrid(discreteGridJson()).success);
    const int base = undoStack()->count();
    ASSERT_TRUE(setGrid(selection()->matchGrid().toJson()).success);
    EXPECT_EQ(undoStack()->count(), base);
}

TEST_F(GridPropertyRouteTest, ModeSwitchUndoRestoresTheAuthoredTable)
{
    ASSERT_TRUE(setGrid(discreteGridJson()).success);
    const auto original = selection()->toJson();
    const auto changed = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"), QJsonObject{
            {QStringLiteral("authoring_handle"), static_cast<qint64>(container_->objectID)},
            {QStringLiteral("property"), QStringLiteral("mode")},
            {QStringLiteral("value"), static_cast<int>(VNX::SelectionMode::Blend)}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(changed.success) << changed.errorMessage.toStdString();
    EXPECT_EQ(selection()->variableResponse(), VNX::VariableResponse::Live);
    EXPECT_FALSE(selection()->matchGrid().empty());
    const auto mixing = mixingJson();
    ASSERT_TRUE(setGrid(mixing).success);
    EXPECT_FALSE(selection()->mixing().empty());
    undoStack()->undo();
    undoStack()->undo();
    EXPECT_EQ(selection()->toJson(), original);
    undoStack()->redo();
    undoStack()->redo();
    EXPECT_EQ(selection()->mode, VNX::SelectionMode::Blend);
    EXPECT_FALSE(selection()->mixing().empty());
    EXPECT_FALSE(selection()->matchGrid().empty());
}

TEST_F(GridPropertyRouteTest, MatchGridPersistsAcrossSaveAndReload)
{
    const auto modeChanged = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"),
             static_cast<qint64>(container_->objectID)},
            {QStringLiteral("property"), QStringLiteral("mode")},
            {QStringLiteral("value"),
             static_cast<int>(VNX::SelectionMode::Match)}},
        VNX::ActionSource::GUI);
    ASSERT_TRUE(modeChanged.success)
        << modeChanged.errorMessage.toStdString();
    EXPECT_FALSE(setGrid(mixingJson()).success);
    ASSERT_TRUE(setGrid(discreteGridJson()).success);

    const auto serialized = selection()->toJson();
    EXPECT_FALSE(serialized.contains(QStringLiteral("grid")));
    EXPECT_FALSE(serialized.contains(QStringLiteral("matches")));
    EXPECT_FALSE(serialized.contains(QStringLiteral("blends")));
    EXPECT_EQ(serialized.value(QStringLiteral("selections")).toObject(), selection()->selections.toJson());
    const auto selectionsBefore = selection()->selections.toJson();

    const QUuid containerGuid = container_->guid;
    const QUuid discreteAxis = varA_->guid;
    const QUuid discreteTarget = childA_->guid;
    auto& projects = VNX::ProjectManager::instance();
    ASSERT_TRUE(projects.saveProject());
    const QString projectPath = projects.currentProject()->projectPath;
    ASSERT_TRUE(projects.loadProject(projectPath));

    const auto* reloaded = projects.findItemByGuid(containerGuid);
    ASSERT_NE(reloaded, nullptr);
    const auto* selection = reloaded->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    EXPECT_EQ(selection->mode, VNX::SelectionMode::Match);
    ASSERT_EQ(selection->matchGrid().axes.size(), 2u);
    EXPECT_EQ(selection->matchGrid().axes[0], discreteAxis);
    EXPECT_EQ(selection->matchGrid().rows[0].target, discreteTarget);
    EXPECT_EQ(VNX::GridSemantics::discreteCellValue(*selection->matchGrid().cellAt(0, 0),
        projects.findItemByGuid(discreteAxis)->comp<VNX::VVariable>()), std::optional<int>(1));
    EXPECT_TRUE(selection->mixing().empty());
    EXPECT_EQ(selection->selections.toJson(), selectionsBefore);
}

TEST_F(GridPropertyRouteTest, LegacyLabelsLoadWithoutSchemaUpgradeAndPersistStableReferences)
{
    auto& projects = VNX::ProjectManager::instance();
    auto* variable = varA_->comp<VNX::VVariable>();
    variable->rangeMin = -1.0f;
    variable->rangeMax = 12.0f;
    variable->labels = {{-1, QStringLiteral("Legacy")}, {10, QStringLiteral("AAA")},
                        {12, QStringLiteral("BBB")}};
    auto* selection = container_->comp<VNX::VSelection>();
    selection->mode = VNX::SelectionMode::Match;
    auto grid = selection->matchGrid();
    ASSERT_TRUE(grid.addAxis(varA_->guid));
    ASSERT_TRUE(grid.addRow(childA_->guid));
    ASSERT_TRUE(grid.addRow(childB_->guid));
    ASSERT_TRUE(grid.setCell(0, 0, 10));
    ASSERT_TRUE(grid.setCell(1, 0, 11));
    ASSERT_TRUE(selection->selections.shared.setMatchGrid(grid));
    ASSERT_TRUE(projects.saveProject());
    const auto containerGuid = container_->guid;
    const auto variableGuid = varA_->guid;
    const auto projectPath = projects.currentProject()->projectPath;
    const auto variablePath = projects.currentProject()->objectFilePath(varA_);
    QFile file(variablePath);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    auto document = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    auto content = document[QStringLiteral("content")].toObject();
    auto components = content[QStringLiteral("components")].toObject();
    auto declaration = components[QStringLiteral("variable")].toObject();
    auto labels = declaration[QStringLiteral("labels")].toArray();
    for (int i = 0; i < labels.size(); ++i) {
        auto label = labels[i].toObject();
        label.remove(QStringLiteral("id"));
        labels[i] = label;
    }
    declaration[QStringLiteral("labels")] = labels;
    components[QStringLiteral("variable")] = declaration;
    content[QStringLiteral("components")] = components;
    document[QStringLiteral("content")] = content;
    const auto bytes = QJsonDocument(document).toJson();
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(file.write(bytes), bytes.size());
    file.close();

    ASSERT_TRUE(projects.loadProject(projectPath, false));
    variable = projects.findItemByGuid(variableGuid)->comp<VNX::VVariable>();
    selection = projects.findItemByGuid(containerGuid)->comp<VNX::VSelection>();
    EXPECT_FLOAT_EQ(variable->rangeMin, -1.0f);
    ASSERT_EQ(variable->labels.size(), 4u);
    EXPECT_EQ(variable->labels[0].anchor, -1);
    EXPECT_EQ(variable->labels[1].anchor, 10);
    EXPECT_EQ(variable->labels[2].anchor, 12);
    EXPECT_EQ(variable->labels[3].anchor, 11);
    const auto namedId = selection->matchGrid().cellAt(0, 0)->labelId;
    const auto unnamedId = selection->matchGrid().cellAt(1, 0)->labelId;
    EXPECT_EQ(namedId, variable->labels[1].id);
    EXPECT_EQ(unnamedId, variable->labels[3].id);
    EXPECT_FALSE(namedId.isNull());
    EXPECT_FALSE(unnamedId.isNull());
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), bytes);
    file.close();

    ASSERT_TRUE(projects.loadProject(projectPath, false));
    variable = projects.findItemByGuid(variableGuid)->comp<VNX::VVariable>();
    selection = projects.findItemByGuid(containerGuid)->comp<VNX::VSelection>();
    EXPECT_EQ(selection->matchGrid().cellAt(0, 0)->labelId, namedId);
    EXPECT_EQ(selection->matchGrid().cellAt(1, 0)->labelId, unnamedId);
    variable->labels[1].anchor = 12;
    variable->labels[2].anchor = 10;
    ASSERT_TRUE(projects.saveProject());
    ASSERT_TRUE(projects.loadProject(projectPath, false));
    variable = projects.findItemByGuid(variableGuid)->comp<VNX::VVariable>();
    selection = projects.findItemByGuid(containerGuid)->comp<VNX::VSelection>();
    EXPECT_EQ(selection->matchGrid().cellAt(0, 0)->labelId, namedId);
    EXPECT_EQ(VNX::GridSemantics::discreteCellValue(*selection->matchGrid().cellAt(0, 0), variable), 12);
    EXPECT_EQ(selection->matchGrid().cellAt(1, 0)->labelId, unnamedId);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(QJsonDocument::fromJson(file.readAll()).object()[QStringLiteral("harkenDocument")],
              document[QStringLiteral("harkenDocument")]);
}

TEST_F(GridPropertyRouteTest, ModeRoundtripPreservesEveryStrategyAndResponseThroughSerialization)
{
    ASSERT_TRUE(setGrid(discreteGridJson()).success);
    auto* value = container_->comp<VNX::VSelection>();
    ASSERT_TRUE(value->setField(QStringLiteral("variableResponse"), 1));
    ASSERT_TRUE(value->setField(QStringLiteral("mode"), static_cast<int>(VNX::SelectionMode::Blend)));
    ASSERT_TRUE(setGrid(mixingJson()).success);
    ASSERT_TRUE(value->setField(QStringLiteral("variableResponse"), 0));
    const auto authored = value->selections.toJson();
    for (const auto mode : {VNX::SelectionMode::Random, VNX::SelectionMode::Sequence,
                            VNX::SelectionMode::Match, VNX::SelectionMode::Blend}) {
        ASSERT_TRUE(value->setField(QStringLiteral("mode"), static_cast<int>(mode)));
        EXPECT_EQ(value->selections.toJson(), authored);
        VNX::VSelection loaded;
        ASSERT_TRUE(loaded.fromJson(value->toJson()));
        EXPECT_EQ(loaded.selections.toJson(), authored);
        EXPECT_EQ(loaded.matchResponse, VNX::VariableResponse::Live);
        EXPECT_EQ(loaded.blendResponse, VNX::VariableResponse::AtStart);
    }
    ASSERT_TRUE(value->setField(QStringLiteral("mode"), static_cast<int>(VNX::SelectionMode::Random)));
    ASSERT_TRUE(value->setField(QStringLiteral("playType"), 1));
    ASSERT_TRUE(value->setField(QStringLiteral("mode"), static_cast<int>(VNX::SelectionMode::Match)));
    EXPECT_FALSE(value->isQueue());
    EXPECT_EQ(VNX::VariableConsumerSemantics::consumersOf(
        VNX::ProjectManager::instance().currentProject(), varB_->guid).size(), 2u);
    ASSERT_TRUE(value->setField(QStringLiteral("mode"), static_cast<int>(VNX::SelectionMode::Random)));
    EXPECT_TRUE(value->isQueue());
    EXPECT_EQ(VNX::VariableConsumerSemantics::consumersOf(
        VNX::ProjectManager::instance().currentProject(), varB_->guid).size(), 2u);
}

TEST_F(GridPropertyRouteTest, TargetReplacementPreservesOccurrenceIdentityAndMergesOnlyCompatibleCurves)
{
    auto table = selection()->selections.shared;
    auto occurrences = table.occurrenceRows();
    const auto firstOrdinal = occurrences.front().stableOccurrenceOrdinal;
    occurrences.push_back({childA_->guid, static_cast<uint32_t>(table.occurrenceOrdinalHighWater), 73});
    ASSERT_TRUE(table.setOccurrenceRows(occurrences));
    const auto memberOrdinal = table.member(childA_->guid)->stableMemberOrdinal;
    occurrences = table.occurrenceRows();
    occurrences.front().childGuid = childB_->guid;
    ASSERT_TRUE(table.setOccurrenceRows(occurrences));
    EXPECT_EQ(table.member(childA_->guid)->stableMemberOrdinal, memberOrdinal);
    EXPECT_EQ(table.memberForOccurrence(firstOrdinal)->childGuid, childB_->guid);
    EXPECT_EQ(table.memberForOrdinal(memberOrdinal)->childGuid, childA_->guid);
    ASSERT_TRUE(VNX::SelectionTable::fromJson(table.toJson()));

    const auto curveA = VNX::MixingChildDef::unity(childA_->guid, 0.0f, 1.0f);
    const auto curveB = VNX::MixingChildDef::unity(childB_->guid, 0.0f, 1.0f);
    ASSERT_TRUE(table.setMixing(VNX::MixingDef{{{varA_->guid, {curveA, curveB}}}}));
    const auto compatible = table;
    occurrences = table.occurrenceRows();
    for (auto& row : occurrences) row.childGuid = childB_->guid;
    ASSERT_TRUE(table.setOccurrenceRows(occurrences));
    EXPECT_EQ(table.entries.size(), 1u);
    EXPECT_EQ(table.entries.front().curves.size(), 1u);
    EXPECT_EQ(table.occurrenceRows(), occurrences);

    table = compatible;
    auto mixing = table.mixing();
    mixing.rows.front().children.front().points.front().value = 0.25f;
    ASSERT_TRUE(table.setMixing(mixing));
    const auto before = table.toJson();
    EXPECT_FALSE(table.setOccurrenceRows(occurrences));
    EXPECT_EQ(table.toJson(), before);
}

TEST_F(GridPropertyRouteTest, RejectsObsoleteSelectionsWithoutChangingObjectsOrDocuments)
{
    ASSERT_TRUE(setGrid(discreteGridJson()).success);
    auto& projects = VNX::ProjectManager::instance();
    ASSERT_TRUE(projects.saveProject());
    const auto* currentProject = projects.currentProject();
    const auto projectPath = currentProject->projectPath;
    const auto containerGuid = container_->guid;
    const auto original = selection()->toJson();
    const auto path = QDir(QFileInfo(projectPath).absolutePath()).filePath(
        VNX::ItemTypeCatalog::folderNameOf(VNX::ItemTypeCatalog::bucketOf(container_->type))
        + QLatin1Char('/') + VNX::NameValidator::objectFileName(container_->name, containerGuid));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const auto savedDocument = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    ASSERT_NE(VNX::ProjectManager::prepareProject(projectPath, false).project, nullptr);
    const auto first = selection()->selections.shared.occurrenceRows().front();
    const QJsonObject obsoleteTable{
        {QStringLiteral("occurrenceOrdinalHighWater"),
            static_cast<qint64>(selection()->selections.shared.occurrenceOrdinalHighWater)},
        {QStringLiteral("rows"), QJsonArray{QJsonObject{
            {QStringLiteral("childGuid"), first.childGuid.toString(QUuid::WithoutBraces)},
            {QStringLiteral("stableOccurrenceOrdinal"), static_cast<qint64>(first.stableOccurrenceOrdinal)},
            {QStringLiteral("weight"), 17}}}}};
    const QJsonObject obsoleteFields{
        {QStringLiteral("selections"), QJsonObject{
            {QStringLiteral("shared"), obsoleteTable}, {QStringLiteral("platforms"), QJsonObject{}}}},
        {QStringLiteral("matches"), selection()->matchGrid().toJson()},
        {QStringLiteral("blends"), mixingJson()},
        {QStringLiteral("variableResponse"), 1}};
    QJsonObject rejectedComponents;
    for (auto it = obsoleteFields.begin(); it != obsoleteFields.end(); ++it) {
        auto obsolete = original;
        obsolete[it.key()] = it.value();
        rejectedComponents[it.key()] = obsolete;
    }
    auto legacy = original;
    legacy.remove(QStringLiteral("matchResponse"));
    legacy.remove(QStringLiteral("blendResponse"));
    for (const auto& field : {QStringLiteral("selections"), QStringLiteral("matches"), QStringLiteral("variableResponse")})
        legacy[field] = obsoleteFields.value(field);
    rejectedComponents[QStringLiteral("legacyMatch")] = legacy;
    legacy[QStringLiteral("mode")] = static_cast<int>(VNX::SelectionMode::Blend);
    legacy.remove(QStringLiteral("matches"));
    legacy[QStringLiteral("blends")] = obsoleteFields.value(QStringLiteral("blends"));
    rejectedComponents[QStringLiteral("legacyBlend")] = legacy;
    for (auto it = rejectedComponents.begin(); it != rejectedComponents.end(); ++it) {
        SCOPED_TRACE(it.key().toStdString());
        const auto obsolete = it.value().toObject();
        EXPECT_FALSE(container_->comp<VNX::VSelection>()->fromJson(obsolete));
        EXPECT_EQ(selection()->toJson(), original);
        const auto replace = [&](QJsonObject node, const auto& self) -> QJsonObject {
            if (QUuid(node.value(QStringLiteral("guid")).toString()) == containerGuid) {
                auto components = node.value(QStringLiteral("components")).toObject();
                components[QStringLiteral("selection")] = obsolete;
                node[QStringLiteral("components")] = components;
            }
            if (node.contains(QStringLiteral("children"))) {
                auto children = node.value(QStringLiteral("children")).toArray();
                for (qsizetype index = 0; index < children.size(); ++index)
                    children[index] = self(children[index].toObject(), self);
                node[QStringLiteral("children")] = children;
            }
            return node;
        };
        auto document = savedDocument;
        document[QStringLiteral("content")] = replace(document.value(QStringLiteral("content")).toObject(), replace);
        const auto bytes = QJsonDocument(document).toJson();
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(file.write(bytes), bytes.size());
        file.close();
        const auto prepared = VNX::ProjectManager::prepareProject(projectPath, false);
        EXPECT_EQ(prepared.project, nullptr);
        EXPECT_FALSE(prepared.error.isEmpty());
        EXPECT_EQ(projects.currentProject(), currentProject);
        EXPECT_EQ(selection()->toJson(), original);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        EXPECT_EQ(file.readAll(), bytes);
        file.close();
    }
}

TEST_F(GridPropertyRouteTest, EmptyVariableRowsPreserveIndependentMembershipAndDormantCurves)
{
    auto* value = container_->comp<VNX::VSelection>();
    value->mode = VNX::SelectionMode::Blend;
    const auto child = VNX::MixingChildDef::unity(childA_->guid, 2.0f, 7.0f);
    auto mixing = VNX::MixingDef{{{varA_->guid, {child}}}};
    ASSERT_TRUE(setGrid(mixing.toJson()).success);
    mixing.rows.front().variable = QUuid{};
    ASSERT_TRUE(setGrid(mixing.toJson()).success);
    ASSERT_EQ(value->activeRows().size(), 1u);
    EXPECT_EQ(value->activeRows().front().childGuid, childA_->guid);
    EXPECT_TRUE(value->selections.shared.toJson().value(QStringLiteral("blendAxes")).toArray().at(0)
        .toObject().value(QStringLiteral("variable")).isNull());
    VNX::VSelection restored;
    ASSERT_TRUE(restored.fromJson(value->toJson()));
    EXPECT_EQ(restored.mixing().toJson(), mixing.toJson());
    auto legacy = value->selections.shared.toJson();
    legacy[QStringLiteral("blendAxes")] = QJsonArray{QJsonValue::Null};
    auto entries = legacy.value(QStringLiteral("entries")).toArray();
    for (qsizetype index = 0; index < entries.size(); ++index) {
        auto entry = entries[index].toObject();
        auto curves = entry.value(QStringLiteral("curves")).toArray();
        for (qsizetype curveIndex = 0; curveIndex < curves.size(); ++curveIndex) {
            auto curve = curves[curveIndex].toObject();
            curve.remove(QStringLiteral("blendGuid"));
            curve[QStringLiteral("variable")] = QJsonValue::Null;
            curves[curveIndex] = curve;
        }
        entry[QStringLiteral("curves")] = curves;
        entries[index] = entry;
    }
    legacy[QStringLiteral("entries")] = entries;
    const auto migrated = VNX::SelectionTable::fromJson(legacy);
    const auto migratedAgain = VNX::SelectionTable::fromJson(legacy);
    ASSERT_TRUE(migrated);
    ASSERT_TRUE(migratedAgain);
    EXPECT_EQ(migrated->toJson(), migratedAgain->toJson());
    const auto roundTrip = VNX::SelectionTable::fromJson(migrated->toJson());
    ASSERT_TRUE(roundTrip);
    EXPECT_EQ(roundTrip->toJson(), migrated->toJson());
    auto expectedMigrated = mixing;
    expectedMigrated.rows.front().guid = migrated->mixing().rows.front().guid;
    EXPECT_EQ(migrated->mixing().toJson(), expectedMigrated.toJson());
    mixing.rows.push_back({{}, {{childB_->guid, {}, {}}}});
    mixing.rows.push_back({{}, {}});
    ASSERT_TRUE(setGrid(mixing.toJson()).success);
    ASSERT_EQ(value->activeRows().size(), 2u);
    ASSERT_TRUE(restored.fromJson(value->toJson()));
    EXPECT_EQ(restored.mixing().toJson(), mixing.toJson());
    auto invalid = mixing;
    invalid.rows.back().guid = invalid.rows.front().guid;
    const auto beforeRejected = value->toJson();
    EXPECT_FALSE(setGrid(invalid.toJson()).success);
    EXPECT_EQ(value->toJson(), beforeRejected);
    mixing.rows.front().variable = varA_->guid;
    ASSERT_TRUE(setGrid(mixing.toJson()).success);
    EXPECT_EQ(value->mixing().toJson(), mixing.toJson());
    invalid = mixing;
    invalid.rows.back().variable = varA_->guid;
    EXPECT_FALSE(setGrid(invalid.toJson()).success);
    EXPECT_EQ(value->mixing().toJson(), mixing.toJson());
    mixing.rows.front().variable = QUuid{};
    const auto beforeReorder = value->toJson();
    std::swap(mixing.rows.front(), mixing.rows.back());
    ASSERT_TRUE(setGrid(mixing.toJson()).success);
    const auto afterReorder = value->toJson();
    undoStack()->undo();
    EXPECT_EQ(value->toJson(), beforeReorder);
    undoStack()->redo();
    EXPECT_EQ(value->toJson(), afterReorder);
    ASSERT_TRUE(restored.fromJson(value->toJson()));
    EXPECT_EQ(restored.mixing().toJson(), value->mixing().toJson());
    const auto containerGuid = container_->guid;
    const auto expected = value->toJson();
    auto& manager = VNX::ProjectManager::instance();
    ASSERT_TRUE(manager.saveProject());
    const auto path = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.loadProject(path, false));
    const auto* loaded = manager.findItemByGuid(containerGuid);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->comp<VNX::VSelection>()->toJson(), expected);
}

TEST_F(GridPropertyRouteTest, PlatformOnlyMatchActivationRejectsUnsupportedTransitionsAtomically)
{
    auto* value = container_->comp<VNX::VSelection>();
    const auto* project = VNX::ProjectManager::instance().currentProject();
    ASSERT_FALSE(project->platforms.isEmpty());
    auto& platform = value->selections.editTable(project->platforms.front().id);
    auto grid = platform.matchGrid();
    ASSERT_TRUE(grid.addAxis(varA_->guid));
    ASSERT_TRUE(grid.addRow(childA_->guid));
    ASSERT_TRUE(platform.setMatchGrid(grid));
    value->transitions.rules.front().exitTiming = VNX::TransitionExitTiming::NextBar;
    const auto reject = [&](const QString& property, int replacement) {
        const auto before = value->toJson();
        const int count = undoStack()->count();
        const auto result = VNX::ActionManager::instance().execute(QStringLiteral("set_item_property"),
            {{QStringLiteral("authoring_handle"), static_cast<qint64>(container_->objectID)},
             {QStringLiteral("property"), property}, {QStringLiteral("value"), replacement}}, VNX::ActionSource::GUI);
        EXPECT_FALSE(result.success);
        EXPECT_EQ(value->toJson(), before);
        EXPECT_EQ(undoStack()->count(), count);
    };
    reject(QStringLiteral("variableResponse"), static_cast<int>(VNX::VariableResponse::Live));
    value->mode = VNX::SelectionMode::Random;
    value->matchResponse = VNX::VariableResponse::Live;
    reject(QStringLiteral("mode"), static_cast<int>(VNX::SelectionMode::Match));
}
