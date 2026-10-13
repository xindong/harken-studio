#include "VariableSemantics.h"
#include "VariableRehearsal.h"
#include "Components/VVariable.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>
#include <QVariant>

#include <limits>

using namespace VNX;
namespace VS = VNX::VariableSemantics;

TEST(VariableSemantics, CreationAndKindChangesPreservePreparedLabels)
{
    VVariable variable;
    VS::initializeNewVariable(variable, VariableKind::Step);
    EXPECT_EQ(variable.kind, VariableKind::Step);
    EXPECT_FLOAT_EQ(variable.rangeMin, 0.0f);
    EXPECT_FLOAT_EQ(variable.rangeMax, 3.0f);
    EXPECT_FLOAT_EQ(variable.defaultValue, 0.0f);
    ASSERT_EQ(variable.labels.size(), 4u);
    EXPECT_TRUE(VS::validateLabels(variable.labels));
    EXPECT_EQ(variable.labels.front().anchor, 0);
    EXPECT_EQ(variable.labels.front().name, QStringLiteral("None"));
    for (size_t i = 1; i < variable.labels.size(); ++i) {
        EXPECT_EQ(variable.labels[i].anchor, static_cast<int>(i));
        EXPECT_TRUE(variable.labels[i].name.isEmpty());
    }
    variable.labels.front().name = QStringLiteral("Rest");
    const auto labels = variable.labels;
    ASSERT_TRUE(variable.setField(QStringLiteral("kind"), static_cast<int>(VariableKind::Smooth)));
    ASSERT_TRUE(variable.setField(QStringLiteral("rangeMin"), 0.4f));
    ASSERT_TRUE(variable.setField(QStringLiteral("rangeMax"), 9.7f));
    ASSERT_TRUE(variable.setField(QStringLiteral("defaultValue"), 3.6f));
    ASSERT_TRUE(variable.setField(QStringLiteral("kind"), static_cast<int>(VariableKind::Step)));
    EXPECT_FLOAT_EQ(variable.rangeMin, 0.0f);
    EXPECT_FLOAT_EQ(variable.rangeMax, 10.0f);
    EXPECT_FLOAT_EQ(variable.defaultValue, 4.0f);
    ASSERT_TRUE(variable.setField(QStringLiteral("kind"), static_cast<int>(VariableKind::Smooth)));
    ASSERT_EQ(variable.labels.size(), labels.size());
    for (size_t i = 0; i < labels.size(); ++i) {
        EXPECT_EQ(variable.labels[i].id, labels[i].id);
        EXPECT_EQ(variable.labels[i].anchor, labels[i].anchor);
        EXPECT_EQ(variable.labels[i].name, labels[i].name);
    }
    ASSERT_TRUE(variable.setField(QStringLiteral("rangeMax"), 2.0f));
    EXPECT_FLOAT_EQ(variable.defaultValue, 2.0f);
    VS::initializeNewVariable(variable, VariableKind::Smooth);
    EXPECT_TRUE(variable.labels.empty());
    EXPECT_FLOAT_EQ(variable.rangeMax, 100.0f);
}

TEST(VariableSemantics, LabelValidationAndExactRangeProjectionPreserveHiddenData)
{
    VVariable variable;
    variable.rangeMin = 0.4f;
    variable.rangeMax = 10.6f;
    variable.labels = {{0, QStringLiteral("None")}, {1, {}}, {10, QStringLiteral("High")},
                       {11, QStringLiteral("Hidden")}};
    ASSERT_TRUE(VS::validateLabels(variable.labels));
    const auto effective = VS::effectiveLabels(variable);
    ASSERT_EQ(effective.size(), 2u);
    EXPECT_EQ(effective[0].anchor, 1);
    EXPECT_EQ(effective[1].anchor, 10);
    EXPECT_EQ(variable.labels.size(), 4u);
    EXPECT_FALSE(VS::labelTextForValue(1.0f, variable.labels));
    variable.labels.back().name = QStringLiteral("None");
    EXPECT_FALSE(VS::validateLabels(variable.labels));
    variable.labels.back().name = QStringLiteral("none");
    EXPECT_TRUE(VS::validateLabels(variable.labels));
    EXPECT_FALSE(variable.setField(QStringLiteral("rangeMin"), -1.0f));
    EXPECT_FLOAT_EQ(variable.rangeMin, 0.4f);
    const auto json = variable.toJson();
    VVariable reopened;
    reopened.fromJson(json);
    ASSERT_EQ(reopened.labels.size(), variable.labels.size());
    for (size_t i = 0; i < variable.labels.size(); ++i)
        EXPECT_EQ(reopened.labels[i].id, variable.labels[i].id);
}

TEST(VariableSemantics, InvalidLabelNamesCannotChangeComponentData)
{
    VVariable variable;
    VS::initializeNewVariable(variable, VariableKind::Step);
    const auto before = variable.toJson();
    auto rows = variable.getField(QStringLiteral("labels")).toList();
    auto row = rows[1].toMap();
    for (const QString& invalid : {QStringLiteral("None"), QStringLiteral("1"),
             QStringLiteral("2"), QStringLiteral("02"), QStringLiteral("2.0"),
             QStringLiteral("+2"), QStringLiteral(".2"), QStringLiteral("2e3"),
             QStringLiteral(" Ready"), QStringLiteral("Ready "), QStringLiteral(" \t ")}) {
        row[QStringLiteral("name")] = invalid;
        rows[1] = row;
        EXPECT_FALSE(variable.setField(QStringLiteral("labels"), rows)) << invalid.toStdString();
        EXPECT_EQ(variable.toJson(), before);
    }
    row[QStringLiteral("name")] = QStringLiteral("Stage 2");
    rows[1] = row;
    EXPECT_TRUE(variable.setField(QStringLiteral("labels"), rows));
}

TEST(VariableSemantics, DiscreteReadCoversHalfOpenAnchorWindows)
{
    EXPECT_EQ(VS::discreteRead(0.0f), 0);
    EXPECT_EQ(VS::discreteRead(0.49f), 0);
    EXPECT_EQ(VS::discreteRead(0.5f), 1);
    EXPECT_EQ(VS::discreteRead(1.49f), 1);
    EXPECT_EQ(VS::discreteRead(-0.5f), 0);
    EXPECT_EQ(VS::discreteRead(-0.51f), -1);
    EXPECT_EQ(VS::discreteRead(-1.5f), -1);
    EXPECT_EQ(VS::discreteRead(-1.51f), -2);
}

TEST(VariableSemantics, DiscreteReadClampsOutOfIntRange)
{
    EXPECT_EQ(VS::discreteRead(std::numeric_limits<float>::infinity()), 0);
    EXPECT_EQ(VS::discreteRead(-std::numeric_limits<float>::infinity()), 0);
    EXPECT_EQ(VS::discreteRead(std::numeric_limits<float>::quiet_NaN()), 0);
    EXPECT_EQ(VS::discreteRead(3.0e9f), std::numeric_limits<int>::max());
    EXPECT_EQ(VS::discreteRead(-3.0e9f), std::numeric_limits<int>::min());
    EXPECT_FALSE(VS::stepComboFits(-3.0e9f, 3.0e9f));
}

TEST(VariableSemantics, DiscreteCellReachabilityUsesTheConsumerRoundingWindow)
{
    EXPECT_TRUE(VS::discreteCellReachable(0, 0.0f, 1.0f));
    EXPECT_TRUE(VS::discreteCellReachable(1, 0.0f, 1.0f));
    EXPECT_FALSE(VS::discreteCellReachable(2, 0.0f, 1.0f));
    EXPECT_TRUE(VS::discreteCellReachable(0, 0.4f, 0.6f));
    EXPECT_TRUE(VS::discreteCellReachable(1, 0.4f, 0.6f));
    EXPECT_TRUE(VS::discreteCellReachable(-1, -1.4f, -0.6f));
    EXPECT_FALSE(VS::discreteCellReachable(
        0, std::numeric_limits<float>::quiet_NaN(), 1.0f));
}

TEST(VariableSemantics, LabelTextHitsExactAnchorAndSparseGapsReturnNothing)
{
    const std::vector<VariableLabel> labels = {
        { 0, QStringLiteral("Calm") },
        { 3, QStringLiteral("Storm") },
    };

    EXPECT_EQ(VS::labelTextForValue(0.2f, labels), QStringLiteral("Calm"));
    EXPECT_EQ(VS::labelTextForValue(2.6f, labels), QStringLiteral("Storm"));
    EXPECT_EQ(VS::labelTextForValue(1.0f, labels), std::nullopt);
    EXPECT_EQ(VS::labelTextForValue(0.2f, {}), std::nullopt);
}

TEST(VariableSemantics, ClampAndDefaultResolutionPinToNearestEndpoint)
{
    EXPECT_FLOAT_EQ(VS::clampToRange(50.0f, 0.0f, 100.0f), 50.0f);
    EXPECT_FLOAT_EQ(VS::resolveDefaultValue(0.0f, 10.0f, 100.0f), 10.0f);
    EXPECT_FLOAT_EQ(VS::resolveDefaultValue(0.0f, -100.0f, -10.0f), -10.0f);
    EXPECT_FLOAT_EQ(VS::resolveDefaultValue(0.0f, 0.0f, 100.0f), 0.0f);
}

TEST(VariableSemantics, CurrentValueAppliesOneClampAndStepPipeline)
{
    EXPECT_FLOAT_EQ(VS::currentValue(std::nullopt, VariableKind::Step,
                                     0.0f, 10.0f, 2.6f), 3.0f);
    EXPECT_FLOAT_EQ(VS::currentValue(12.0f, VariableKind::Smooth,
                                     0.0f, 10.0f, 1.0f), 10.0f);
    EXPECT_FLOAT_EQ(VS::currentValue(-2.6f, VariableKind::Step,
                                     -10.0f, 10.0f, 1.0f), -3.0f);
}

TEST(VariableSemantics, StepComboFitsStopsBeforeEnumeratingWideRanges)
{
    EXPECT_TRUE(VS::stepComboFits(0.0f, 3.0f));
    EXPECT_TRUE(VS::stepComboFits(0.0f, static_cast<float>(
        VS::kStepComboMaxInclusiveCount - 1)));
    EXPECT_FALSE(VS::stepComboFits(0.0f, static_cast<float>(
        VS::kStepComboMaxInclusiveCount)));
    EXPECT_FALSE(VS::stepComboFits(-1000000.0f, 1000000.0f));
    EXPECT_FALSE(VS::stepComboFits(16777200.0f, 16777263.0f));
}

TEST(VariableSemantics, ClampStepExactIntStopsAtFloatIntegerPrecision)
{
    EXPECT_EQ(VS::clampStepExactInt(0), 0);
    EXPECT_EQ(VS::clampStepExactInt(VS::kStepExactIntBound),
              VS::kStepExactIntBound);
    EXPECT_EQ(VS::clampStepExactInt(VS::kStepExactIntBound + 1),
              VS::kStepExactIntBound);
    EXPECT_EQ(VS::clampStepExactInt(-VS::kStepExactIntBound - 1),
              -VS::kStepExactIntBound);
    EXPECT_EQ(VS::discreteRead(static_cast<float>(VS::kStepExactIntBound)),
              VS::kStepExactIntBound);
}

TEST(VariableSemantics, StepExactSpinRangeEmptyWhenOutsideFloatIntegers)
{
    const auto inside = VS::stepExactSpinRange(0.0f, 10.0f);
    EXPECT_TRUE(inside.valid);
    EXPECT_EQ(inside.begin, 0);
    EXPECT_EQ(inside.end, 10);

    const auto clipped = VS::stepExactSpinRange(-3.0e9f, 3.0e9f);
    EXPECT_TRUE(clipped.valid);
    EXPECT_EQ(clipped.begin, -VS::kStepExactIntBound);
    EXPECT_EQ(clipped.end, VS::kStepExactIntBound);

    const auto emptyHigh = VS::stepExactSpinRange(20000000.0f, 30000000.0f);
    EXPECT_FALSE(emptyHigh.valid);
    const auto emptyLow = VS::stepExactSpinRange(-30000000.0f, -20000000.0f);
    EXPECT_FALSE(emptyLow.valid);
}

TEST(VariableRehearsal, RepeatedWritesAreSignaledAndSessionScopeDefaultsToEmitter)
{
    VariableRehearsal rehearsal;
    int changed = 0;
    QObject::connect(&rehearsal, &VariableRehearsal::rehearsalChanged,
                     [&changed](const QUuid&) { ++changed; });
    const QUuid first = QUuid::createUuid();
    const QUuid second = QUuid::createUuid();

    EXPECT_FALSE(rehearsal.setOverride({}, 1.0f));
    EXPECT_FALSE(rehearsal.setOverride(first, std::numeric_limits<float>::infinity()));
    EXPECT_TRUE(rehearsal.setOverride(first, 2.0f));
    EXPECT_TRUE(rehearsal.setOverride(first, 2.0f));
    EXPECT_EQ(changed, 2);
    EXPECT_EQ(rehearsal.scopeFor(first), VariableScope::Emitter);
    rehearsal.setScope(first, VariableScope::Global);
    EXPECT_EQ(rehearsal.scopeFor(first), VariableScope::Global);
    EXPECT_EQ(changed, 3);
    rehearsal.setScope(first, VariableScope::Global);
    EXPECT_EQ(changed, 3);
    EXPECT_EQ(rehearsal.scopeFor(second), VariableScope::Emitter);
    rehearsal.setScope(second, VariableScope::Global);
    EXPECT_EQ(changed, 3);
    EXPECT_TRUE(rehearsal.setOverride(second, 3.0f));
    rehearsal.clearAll();
    EXPECT_EQ(changed, 6);
    EXPECT_EQ(rehearsal.scopeFor(first), VariableScope::Emitter);
    EXPECT_EQ(rehearsal.overrideFor(first), std::nullopt);
    EXPECT_EQ(rehearsal.overrideFor(second), std::nullopt);
}

TEST(VariableSemantics, StepNormalizationRoundsEndpointsAndNeverInverts)
{
    const auto stepped = VS::normalizeDeclarationForKind(
        VariableKind::Step, 0.4f, 9.7f, 3.6f);
    EXPECT_FLOAT_EQ(stepped.rangeMin, 0.0f);
    EXPECT_FLOAT_EQ(stepped.rangeMax, 10.0f);
    EXPECT_FLOAT_EQ(stepped.defaultValue, 4.0f);

    const auto tight = VS::normalizeDeclarationForKind(
        VariableKind::Step, 0.4f, 0.6f, 0.5f);
    EXPECT_LE(tight.rangeMin, tight.rangeMax);

    const auto smooth = VS::normalizeDeclarationForKind(
        VariableKind::Smooth, 0.4f, 9.7f, 3.6f);
    EXPECT_FLOAT_EQ(smooth.rangeMin, 0.4f);
    EXPECT_FLOAT_EQ(smooth.rangeMax, 9.7f);
    EXPECT_FLOAT_EQ(smooth.defaultValue, 3.6f);
}

TEST(VariableSemantics, FileBoundVariablesIncludesAVariableOutsideTheVariablesBucket)
{
    if (!qApp) {
        static int argc = 1;
        static char arg0[] = "harken_editor_tests";
        static char* argv[] = {arg0, nullptr};
        static QApplication app(argc, argv);
        (void)app;
    }

    auto& manager = ProjectManager::instance();
    if (manager.isProjectOpen())
        manager.closeProject();

    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    ProjectConfig config;
    config.name = QStringLiteral("FileBoundScanRoot");
    config.location = temp.path();
    ASSERT_TRUE(manager.createProject(config));
    ProjectData* project = manager.currentProject();
    ASSERT_NE(project, nullptr);

    ProjectItem* bucketVar = manager.createItem(
        nullptr, QStringLiteral("InVariablesBucket"), ProjectItem::Type::Variable);
    ASSERT_NE(bucketVar, nullptr);

    auto hosted = std::make_shared<ProjectItem>(
        QStringLiteral("HostedInLayers"), ProjectItem::Type::Variable);
    hosted->attachDefaultComponents();
    hosted->objectID = project->generateObjectID();
    hosted->guid = QUuid::createUuid();
    ASSERT_NE(hosted->comp<VVariable>(), nullptr);

    ProjectFileBinding binding;
    binding.rootItem = hosted;
    binding.parentFolder = QStringLiteral("Layers");
    project->layers.push_back(std::move(binding));
    ProjectItem* hostedVar = project->layers.back().rootItem.get();

    const auto variables = VS::fileBoundVariables(project);
    QSet<QUuid> guids;
    for (ProjectItem* item : variables)
        if (item)
            guids.insert(item->guid);

    EXPECT_TRUE(guids.contains(bucketVar->guid));
    EXPECT_TRUE(guids.contains(hostedVar->guid));
    manager.closeProject();
}

TEST(VariableSemantics, VariableComponentRoundtripsWithoutScope)
{
    VVariable variable;
    variable.kind = VariableKind::Step;
    variable.rangeMin = -4.0f;
    variable.rangeMax = 12.0f;
    variable.defaultValue = 3.0f;
    variable.labels = { { -4, QStringLiteral("Off") }, { 12, QStringLiteral("Max") } };

    const QJsonObject saved = variable.toJson();
    EXPECT_FALSE(saved.contains(QStringLiteral("scope")));
    VVariable reloaded;
    reloaded.fromJson(saved);
    EXPECT_EQ(reloaded.kind, VariableKind::Step);
    EXPECT_FLOAT_EQ(reloaded.rangeMin, -4.0f);
    EXPECT_FLOAT_EQ(reloaded.rangeMax, 12.0f);
    EXPECT_FALSE(reloaded.toJson().contains(QStringLiteral("scope")));
    EXPECT_FLOAT_EQ(reloaded.defaultValue, 3.0f);
    ASSERT_EQ(reloaded.labels.size(), 2u);
    EXPECT_EQ(reloaded.labels[0].anchor, -4);
    EXPECT_EQ(reloaded.labels[0].name, QStringLiteral("Off"));
    EXPECT_EQ(reloaded.labels[1].anchor, 12);
    EXPECT_EQ(reloaded.labels[1].name, QStringLiteral("Max"));
}
