#include "Widgets/Common/ParameterBinding.h"

#include <gtest/gtest.h>

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
                         binding.setMixedValue(true);
                     });
    binding.commitValue(20.0);
    EXPECT_EQ(commits, 1);
    EXPECT_TRUE(binding.mixedValue());
}

TEST(ParameterBinding, DomainChangesAllowExplicitCommitOfNormalizedValues)
{
    for (const bool quantized : {false, true}) {
        VNX::ParameterBinding binding;
        binding.setRange(0.0, 100.0);
        binding.setValue(75.5);
        int commits = 0;
        QObject::connect(&binding, &VNX::ParameterBinding::valueCommitted, &binding,
            [&](double) { ++commits; });
        if (quantized) binding.setQuantum(1.0);
        else binding.setRange(0.0, 10.0);
        EXPECT_EQ(commits, 0);
        binding.commitValue(binding.value());
        EXPECT_EQ(commits, 1);
        binding.commitValue(binding.value());
        EXPECT_EQ(commits, 1);
    }
}
