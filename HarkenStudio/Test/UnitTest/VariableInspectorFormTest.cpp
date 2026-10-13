#include "Components/VVariable.h"
#include "PropertyDescriptor.h"
#include "VariableSemantics.h"
#include "Widgets/Inspector/VariableInspectorSchema.h"

#include <gtest/gtest.h>

#include <QJsonObject>
#include <QVariantList>

namespace {

const VNX::PropertyDescriptor& rowOf(const char* field)
{
    for (const auto& d : VNX::VVariable::schema())
        if (d.field == QLatin1String(field)) return d;
    ADD_FAILURE() << "missing schema row " << field;
    static const VNX::PropertyDescriptor kEmpty;
    return kEmpty;
}

VNX::FieldDomain domainOf(const char* field, const VNX::VVariable& variable)
{
    const auto& row = rowOf(field);
    EXPECT_TRUE(static_cast<bool>(row.domain)) << field;
    const auto domain = row.domain(&variable);
    EXPECT_TRUE(domain.has_value()) << field;
    return domain.value_or(VNX::FieldDomain{});
}

}

TEST(VariableInspectorForm, RangeRowReadsAndWritesBothBoundsAtOnce)
{
    const auto& range = rowOf("range");
    EXPECT_EQ(range.type, VNX::PropType::Range);
    EXPECT_FALSE(range.flags.serialize);

    VNX::VVariable variable;
    variable.rangeMin = 0.0f;
    variable.rangeMax = 100.0f;

    const QVariantList read = range.get(&variable).toList();
    ASSERT_EQ(read.size(), 2);
    EXPECT_FLOAT_EQ(read.at(0).toFloat(), 0.0f);
    EXPECT_FLOAT_EQ(read.at(1).toFloat(), 100.0f);

    EXPECT_FALSE(range.set(&variable, QVariant(QVariantList{-20.0, 5000.0})));
    EXPECT_FLOAT_EQ(variable.rangeMin, 0.0f);
    ASSERT_TRUE(range.set(&variable, QVariant(QVariantList{20.0, 5000.0})));
    EXPECT_FLOAT_EQ(variable.rangeMin, 20.0f);
    EXPECT_FLOAT_EQ(variable.rangeMax, 5000.0f);

    EXPECT_FALSE(range.set(&variable, QVariant(QVariantList{1.0})));
}

TEST(VariableInspectorForm, RangeRowOrdersInvertedBoundsAndReclampsTheDefault)
{
    const auto& range = rowOf("range");
    VNX::VVariable variable;
    variable.rangeMin = 0.0f;
    variable.rangeMax = 100.0f;
    variable.defaultValue = 80.0f;

    ASSERT_TRUE(range.set(&variable, QVariant(QVariantList{10.0, 3.0})));
    EXPECT_FLOAT_EQ(variable.rangeMin, 3.0f);
    EXPECT_FLOAT_EQ(variable.rangeMax, 10.0f);
    EXPECT_FLOAT_EQ(variable.defaultValue, 10.0f);
}

TEST(VariableInspectorForm, KindDrivesTheEditingFormOfEveryDeclaredNumber)
{
    VNX::VVariable smooth;
    smooth.kind = VNX::VariableKind::Smooth;
    for (const char* field : {"range", "defaultValue"})
    {
        const VNX::FieldDomain domain = domainOf(field, smooth);
        EXPECT_EQ(domain.decimals, 2) << field;
        EXPECT_DOUBLE_EQ(domain.quantum, 0.0) << field;
    }

    VNX::VVariable step;
    step.kind = VNX::VariableKind::Step;
    for (const char* field : {"range", "defaultValue"})
    {
        const VNX::FieldDomain domain = domainOf(field, step);
        EXPECT_EQ(domain.decimals, 0) << field;
        EXPECT_DOUBLE_EQ(domain.quantum, 1.0) << field;
    }
}

TEST(VariableInspectorForm, DefaultValueDomainFollowsTheDeclaredRange)
{
    VNX::VVariable variable;
    variable.rangeMin = -6.0f;
    variable.rangeMax = 3000.0f;

    const VNX::FieldDomain domain = domainOf("defaultValue", variable);
    EXPECT_DOUBLE_EQ(domain.min, -6.0);
    EXPECT_DOUBLE_EQ(domain.max, 3000.0);

    // The range row itself is a free numeric field, only fenced against
    // runaway input.
    const VNX::FieldDomain rangeDomain = domainOf("range", variable);
    EXPECT_DOUBLE_EQ(rangeDomain.min, 0.0);
    EXPECT_DOUBLE_EQ(rangeDomain.max, VNX::kVariableRangeAuthoringBound);
}

TEST(VariableInspectorForm, DefaultValueAnnotationWearsTheNearestAnchorLabel)
{
    VNX::VVariable variable;
    variable.kind = VNX::VariableKind::Step;
    variable.rangeMin = 0.0f;
    variable.rangeMax = 3.0f;
    variable.labels = {{0, QStringLiteral("Stealth")}, {1, QStringLiteral("High Intensity")}};

    const VNX::FieldDomain domain = domainOf("defaultValue", variable);
    ASSERT_TRUE(static_cast<bool>(domain.annotation));
    EXPECT_EQ(domain.annotation(0.0), QStringLiteral("Stealth"));
    EXPECT_EQ(domain.annotation(1.0), QStringLiteral("High Intensity"));
    EXPECT_EQ(domain.annotation(0.8), QStringLiteral("High Intensity"));
    EXPECT_TRUE(domain.annotation(3.0).isEmpty());

    // The resolver captures the anchors it was built from; a later edit needs a
    // re-resolve, which is exactly what the engine does after every commit.
    variable.labels.clear();
    const VNX::FieldDomain rebuilt = domainOf("defaultValue", variable);
    EXPECT_TRUE(rebuilt.annotation(0.0).isEmpty());
}

TEST(VariableInspectorForm, LabelsWindowFollowsTheRangeWithoutDeregisteringAnchors)
{
    VNX::VVariable variable;
    variable.kind = VNX::VariableKind::Step;
    variable.rangeMin = 0.0f;
    variable.rangeMax = 10.0f;
    variable.labels = {{0, QStringLiteral("Calm")}, {10, QStringLiteral("Boss")}};

    VNX::FieldDomain window = domainOf("labels", variable);
    EXPECT_DOUBLE_EQ(window.min, 0.0);
    EXPECT_DOUBLE_EQ(window.max, 10.0);
    EXPECT_EQ(window.decimals, 0);
    EXPECT_DOUBLE_EQ(window.quantum, 1.0);

    // Narrowing the range slides the window; the anchor it moves off keeps its
    // name and its place, and comes back when the range widens again.
    const auto& range = rowOf("range");
    ASSERT_TRUE(range.set(&variable, QVariant(QVariantList{0.0, 5.0})));
    window = domainOf("labels", variable);
    EXPECT_DOUBLE_EQ(window.max, 5.0);
    ASSERT_EQ(variable.labels.size(), 2u);
    EXPECT_EQ(variable.labels.back().anchor, 10);
    EXPECT_EQ(variable.labels.back().name, QStringLiteral("Boss"));

    ASSERT_TRUE(range.set(&variable, QVariant(QVariantList{0.0, 10.0})));
    EXPECT_EQ(VNX::VariableSemantics::labelTextForValue(10.0f, variable.labels)
                  .value_or(QString()),
              QStringLiteral("Boss"));
}

TEST(VariableInspectorForm, StrandedAnchorsSurviveAKindSwitchAndTheProjectFile)
{
    VNX::VVariable variable;
    variable.rangeMin = 0.0f;
    variable.rangeMax = 5.0f;
    variable.labels = {{10, QStringLiteral("Boss")}};

    ASSERT_TRUE(rowOf("kind").set(&variable,
                                  static_cast<int>(VNX::VariableKind::Step)));
    ASSERT_EQ(variable.labels.size(), 1u);
    EXPECT_EQ(variable.labels.front().anchor, 10);

    VNX::VVariable reloaded;
    reloaded.fromJson(variable.toJson());
    ASSERT_EQ(reloaded.labels.size(), 1u);
    EXPECT_EQ(reloaded.labels.front().anchor, 10);
    EXPECT_EQ(reloaded.labels.front().name, QStringLiteral("Boss"));
}

TEST(VariableInspectorForm, StorageRowsKeepTheirByNameSetters)
{
    VNX::VVariable variable;
    EXPECT_TRUE(variable.setField(QStringLiteral("rangeMin"), 2.0f));
    EXPECT_TRUE(variable.setField(QStringLiteral("rangeMax"), 9.0f));
    EXPECT_FLOAT_EQ(variable.rangeMin, 2.0f);
    EXPECT_FLOAT_EQ(variable.rangeMax, 9.0f);

    EXPECT_TRUE(variable.setField(QStringLiteral("range"), QVariant(QVariantList{0.0, 4.0})));
    EXPECT_FLOAT_EQ(variable.rangeMin, 0.0f);
    EXPECT_FLOAT_EQ(variable.rangeMax, 4.0f);
}

TEST(VariableInspectorForm, RangeRowIsNotSerialized)
{
    VNX::VVariable variable;
    variable.rangeMin = 1.0f;
    variable.rangeMax = 7.0f;

    const QJsonObject json = variable.toJson();
    EXPECT_FALSE(json.contains(QStringLiteral("range")));
    ASSERT_TRUE(json.contains(QStringLiteral("rangeMin")));
    ASSERT_TRUE(json.contains(QStringLiteral("rangeMax")));
    EXPECT_DOUBLE_EQ(json.value(QStringLiteral("rangeMin")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(json.value(QStringLiteral("rangeMax")).toDouble(), 7.0);
}

TEST(VariableInspectorForm, AdvancedContractFieldsStayOffInspectorPage)
{
    const auto inspectorHas = [](const char* field) {
        for (const auto& d : VNX::variableInspectorSchema())
            if (d.field == QLatin1String(field)) return true;
        return false;
    };
    const auto componentRow = [](const char* field) -> const VNX::PropertyDescriptor* {
        for (const auto& d : VNX::VVariable::schema())
            if (d.field == QLatin1String(field)) return &d;
        return nullptr;
    };
    for (const char* field : {"writers", "unit", "precision"}) {
        EXPECT_FALSE(inspectorHas(field)) << field;
        const auto* row = componentRow(field);
        ASSERT_NE(row, nullptr) << field;
        EXPECT_TRUE(row->flags.advanced) << field;
    }
}

TEST(VariableInspectorForm, MissingWritersNormalizeToGame)
{
    VNX::VVariable variable;
    EXPECT_EQ(1u, variable.writers);
    ASSERT_TRUE(variable.fromJson(QJsonObject{}));
    variable.normalizeDeclaration();
    EXPECT_EQ(1u, variable.writers);
    EXPECT_FALSE(variable.toJson().contains(QStringLiteral("writers")));
}

TEST(VariableInspectorForm, WritersZeroIsRefused)
{
    VNX::VVariable variable;
    variable.writers = 3;
    EXPECT_FALSE(variable.setField(QStringLiteral("writers"), 0));
    EXPECT_EQ(3u, variable.writers);
    variable.writers = 0;
    variable.normalizeDeclaration();
    EXPECT_EQ(1u, variable.writers);
}

TEST(VariableInspectorForm, WritersThreeIsAccepted)
{
    VNX::VVariable variable;
    ASSERT_TRUE(variable.setField(QStringLiteral("writers"), 3));
    EXPECT_EQ(3u, variable.writers);
    EXPECT_EQ(3, variable.toJson().value(QStringLiteral("writers")).toInt());
    VNX::VVariable loaded;
    ASSERT_TRUE(loaded.fromJson(variable.toJson()));
    EXPECT_EQ(3u, loaded.writers);
}
