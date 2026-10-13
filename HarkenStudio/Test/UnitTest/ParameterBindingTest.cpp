#include "Widgets/Common/ParameterBinding.h"

#include <gtest/gtest.h>

#include <tuple>
#include <limits>
#include <vector>

namespace {

struct EditRecorder
{
    std::vector<double> values;
    std::vector<double> commits;
    std::vector<double> begins;
    std::vector<double> previews;
    std::vector<std::tuple<double, double, bool>> ends;

    explicit EditRecorder(VNX::ParameterBinding& binding)
    {
        QObject::connect(&binding, &VNX::ParameterBinding::valueUpdated,
            [&values = values](double value) { values.push_back(value); });
        QObject::connect(&binding, &VNX::ParameterBinding::valueCommitted,
            [&commits = commits](double value) { commits.push_back(value); });
        QObject::connect(&binding, &VNX::ParameterBinding::editBegan,
            [&begins = begins](double value) { begins.push_back(value); });
        QObject::connect(&binding, &VNX::ParameterBinding::editPreviewed,
            [&previews = previews](double value) { previews.push_back(value); });
        QObject::connect(&binding, &VNX::ParameterBinding::editEnded,
            [&ends = ends](double start, double end, bool committed) {
                ends.emplace_back(start, end, committed);
            });
    }
};

}

TEST(ParameterBinding, StoresMetadataAndMapsLinearPosition)
{
    VNX::ParameterBinding binding;
    binding.setRange(-96.0, 24.0);
    binding.setDefaultValue(-12.0);
    binding.setDecimals(1);
    binding.setUnit(QStringLiteral(" dB"));
    binding.setName(QStringLiteral("Volume"));

    EXPECT_DOUBLE_EQ(binding.minimum(), -96.0);
    EXPECT_DOUBLE_EQ(binding.maximum(), 24.0);
    EXPECT_DOUBLE_EQ(binding.defaultValue(), -12.0);
    EXPECT_EQ(binding.decimals(), 1);
    EXPECT_EQ(binding.unit(), QStringLiteral(" dB"));
    EXPECT_EQ(binding.name(), QStringLiteral("Volume"));
    EXPECT_DOUBLE_EQ(binding.valueFromPosition(0.5), -36.0);
    EXPECT_DOUBLE_EQ(binding.positionFromValue(-36.0), 0.5);
}

TEST(ParameterBinding, ProgrammaticSetIsSilentAndClamped)
{
    VNX::ParameterBinding binding;
    binding.setRange(0.0, 100.0);
    EditRecorder recorder(binding);

    binding.setValue(120.0);

    EXPECT_DOUBLE_EQ(binding.value(), 100.0);
    ASSERT_EQ(recorder.values.size(), 1u);
    EXPECT_DOUBLE_EQ(recorder.values[0], 100.0);
    EXPECT_TRUE(recorder.commits.empty());
    EXPECT_TRUE(recorder.begins.empty());
    EXPECT_TRUE(recorder.previews.empty());
    EXPECT_TRUE(recorder.ends.empty());
}

TEST(ParameterBinding, EditPreviewAndCommitHaveOneSessionBoundary)
{
    VNX::ParameterBinding binding;
    binding.setRange(0.0, 100.0);
    binding.setValue(20.0);
    EditRecorder recorder(binding);

    binding.beginEdit();
    binding.beginEdit();
    binding.previewEdit(50.0);
    binding.previewEdit(75.0);
    binding.commitEdit();
    binding.commitEdit();

    EXPECT_FALSE(binding.isEditing());
    EXPECT_DOUBLE_EQ(binding.value(), 75.0);
    ASSERT_EQ(recorder.begins.size(), 1u);
    EXPECT_DOUBLE_EQ(recorder.begins[0], 20.0);
    ASSERT_EQ(recorder.previews.size(), 2u);
    EXPECT_DOUBLE_EQ(recorder.previews[0], 50.0);
    EXPECT_DOUBLE_EQ(recorder.previews[1], 75.0);
    ASSERT_EQ(recorder.ends.size(), 1u);
    EXPECT_EQ(recorder.ends[0], std::make_tuple(20.0, 75.0, true));
    EXPECT_TRUE(recorder.commits.empty());
}

TEST(ParameterBinding, CancelRestoresStartValue)
{
    VNX::ParameterBinding binding;
    binding.setRange(0.0, 100.0);
    binding.setValue(20.0);
    EditRecorder recorder(binding);

    binding.beginEdit();
    binding.previewEdit(75.0);
    binding.cancelEdit();
    binding.cancelEdit();

    EXPECT_FALSE(binding.isEditing());
    EXPECT_DOUBLE_EQ(binding.value(), 20.0);
    ASSERT_EQ(recorder.ends.size(), 1u);
    EXPECT_EQ(recorder.ends[0], std::make_tuple(20.0, 75.0, false));
    ASSERT_EQ(recorder.values.size(), 2u);
    EXPECT_DOUBLE_EQ(recorder.values[0], 75.0);
    EXPECT_DOUBLE_EQ(recorder.values[1], 20.0);
}

TEST(ParameterBinding, ProgrammaticSetInterruptsEditBeforePublishingValue)
{
    VNX::ParameterBinding binding;
    binding.setRange(0.0, 100.0);
    binding.setValue(20.0);
    EditRecorder recorder(binding);

    binding.beginEdit();
    binding.previewEdit(75.0);
    binding.setValue(10.0);

    EXPECT_FALSE(binding.isEditing());
    EXPECT_DOUBLE_EQ(binding.value(), 10.0);
    ASSERT_EQ(recorder.ends.size(), 1u);
    EXPECT_EQ(recorder.ends[0], std::make_tuple(20.0, 75.0, false));
    EXPECT_TRUE(recorder.commits.empty());
}

TEST(ParameterBinding, DirectCommitEmitsOnceOnlyForAChange)
{
    VNX::ParameterBinding binding;
    binding.setRange(0.0, 100.0);
    binding.setValue(20.0);
    EditRecorder recorder(binding);

    binding.commitValue(20.0);
    binding.commitValue(30.0);
    binding.commitValue(30.0);

    ASSERT_EQ(recorder.commits.size(), 1u);
    EXPECT_DOUBLE_EQ(recorder.commits[0], 30.0);
    EXPECT_DOUBLE_EQ(binding.value(), 30.0);
}

TEST(ParameterBinding, MixedValueCommitsCachedValueOnceAndRejectsNonFiniteInput)
{
    VNX::ParameterBinding binding;
    binding.setRange(0.0, 100.0);
    binding.setValue(20.0);
    EditRecorder recorder(binding);
    binding.setMixedValue(true);
    binding.commitValue(std::numeric_limits<double>::quiet_NaN());
    EXPECT_TRUE(binding.mixedValue());
    EXPECT_TRUE(recorder.commits.empty());

    binding.commitValue(20.0);
    ASSERT_EQ(recorder.commits.size(), 1u);
    EXPECT_DOUBLE_EQ(recorder.commits.front(), 20.0);
    EXPECT_FALSE(binding.mixedValue());
    EXPECT_TRUE(recorder.values.empty());
    binding.commitValue(20.0);
    EXPECT_EQ(recorder.commits.size(), 1u);
}

TEST(ParameterBinding, CommitPreservesMixedStateSetByItsCallback)
{
    VNX::ParameterBinding binding;
    binding.setRange(0.0, 100.0);
    binding.setValue(20.0);
    binding.setMixedValue(true);
    int commits = 0;
    QObject::connect(&binding, &VNX::ParameterBinding::valueCommitted, &binding,
                     [&](double) {
                         ++commits;
                         EXPECT_FALSE(binding.mixedValue());
                         binding.setMixedValue(true);
                     });
    binding.commitValue(20.0);
    EXPECT_EQ(commits, 1);
    EXPECT_TRUE(binding.mixedValue());
}

TEST(ParameterBinding, QuantumSnapsEveryValueAndStaysOffByDefault)
{
    VNX::ParameterBinding binding;
    binding.setRange(0.0, 3.0);

    binding.setValue(2.4);
    EXPECT_DOUBLE_EQ(binding.value(), 2.4);

    binding.setQuantum(1.0);
    EXPECT_DOUBLE_EQ(binding.quantum(), 1.0);
    EXPECT_DOUBLE_EQ(binding.value(), 2.0);

    binding.setValue(0.6);
    EXPECT_DOUBLE_EQ(binding.value(), 1.0);
    binding.commitValue(2.5);
    EXPECT_DOUBLE_EQ(binding.value(), 3.0);

    binding.setQuantum(0.0);
    binding.setValue(1.25);
    EXPECT_DOUBLE_EQ(binding.value(), 1.25);
}

TEST(ParameterBinding, QuantizedPositionDomainSpansOnePositionPerStep)
{
    VNX::ParameterBinding binding;
    binding.setRange(0.0, 4.0);
    EXPECT_EQ(binding.positionSteps(1000), 1000);

    binding.setQuantum(1.0);
    EXPECT_EQ(binding.positionSteps(1000), 4);
    EXPECT_EQ(binding.positionOf(0.0, 4), 0);
    EXPECT_EQ(binding.positionOf(3.0, 4), 3);
    EXPECT_EQ(binding.positionOf(4.0, 4), 4);

    // Every position on that domain already is a value, so an editor tracking
    // the pointer over it never lands between steps and never needs to be
    // snapped back after the fact.
    for (int position = 0; position <= 4; ++position)
        EXPECT_DOUBLE_EQ(binding.valueFromPosition(position / 4.0),
                         static_cast<double>(position));

    // A step finer than the smooth resolution keeps the smooth domain.
    binding.setQuantum(0.001);
    EXPECT_EQ(binding.positionSteps(1000), 1000);
}

TEST(ParameterBinding, AnnotationIsOptionalAndReadsTheGivenValue)
{
    VNX::ParameterBinding binding;
    binding.setRange(0.0, 3.0);
    EXPECT_TRUE(binding.annotationFor(1.0).isEmpty());

    binding.setAnnotation([](double value) {
        return value >= 1.0 ? QStringLiteral("High") : QString();
    });
    EXPECT_EQ(binding.annotationFor(2.0), QStringLiteral("High"));
    EXPECT_TRUE(binding.annotationFor(0.0).isEmpty());
}
