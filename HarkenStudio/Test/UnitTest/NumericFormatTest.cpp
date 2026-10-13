#include "Core/NumericFormat.h"

#include <gtest/gtest.h>

TEST(NumericFormat, ReadoutsKeepMorePrecisionThanAxisLabels)
{
    const double value = 1.35435165;
    for (double step : {1.0, 0.1, 0.01}) {
        const double axis = VNX::formatAxisValue(value, VNX::axisDecimalsForStep(step)).toDouble();
        const double readout = VNX::formatAxisValue(value, VNX::axisReadoutDecimalsForStep(step)).toDouble();
        EXPECT_LT(std::abs(readout - value), std::abs(axis - value));
        EXPECT_LE(std::abs(readout - value), step / 20.0);
    }
    EXPECT_EQ(VNX::axisReadoutDecimalsForStep(1e-9, 6), 6);
}

TEST(NumericFormat, IntegerAxesOmitAllFractionalZeros)
{
    EXPECT_EQ(VNX::formatAxisValues({0, 20, 40, 60, 80, 100}, 3),
        (QStringList{"0", "20", "40", "60", "80", "100"}));
    EXPECT_EQ(VNX::formatAxisValues({-0.00001, 0.25, 0.5, 1.0}, 3),
        (QStringList{"0.00", "0.25", "0.50", "1.00"}));
}

TEST(NumericFormat, FractionalAxesUseOneSharedPrecision)
{
    EXPECT_EQ(VNX::formatAxisValues({-96, -12.04, 0, 12.04, 24}, 1),
        (QStringList{"-96", "-12", "0", "12", "24"}));
    EXPECT_EQ(VNX::formatAxisValues({0, 0.2, 0.4, 0.6}, 3),
        (QStringList{"0.0", "0.2", "0.4", "0.6"}));
    EXPECT_EQ(VNX::formatAxisValue(-0.00001, 3), QStringLiteral("0"));
    EXPECT_EQ(VNX::compactAxisLabels({"1.0e+06", "2.0e+06"}),
        (QStringList{"1e+06", "2e+06"}));
}

TEST(NumericFormat, CompactionPreservesUnitsAndNamedLabels)
{
    EXPECT_EQ(VNX::compactAxisLabels({"20.00", "100.00", "1.00k", "20.00k"}),
        (QStringList{"20", "100", "1k", "20k"}));
    EXPECT_EQ(VNX::compactAxisLabels({"-12.0 dB", "0.0 dB", "+6.0 dB", "Off"}),
        (QStringList{"-12 dB", "0 dB", "+6 dB", "Off"}));
    EXPECT_EQ(VNX::formatDecimal(20.0, 3), QStringLiteral("20.0"));
}
