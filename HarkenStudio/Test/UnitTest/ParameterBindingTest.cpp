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
