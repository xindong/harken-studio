#include "Widgets/Common/AmplitudeAxis.h"
#include "Widgets/Meter/MeterBallistics.h"

#include <gtest/gtest.h>

#include <cmath>

TEST(MeterBallistics, ReleaseDependsOnElapsedTimeInsteadOfUpdateFrequency)
{
    for (float totalSeconds : {0.05f, 0.75f, 1.0f, 2.0f}) {
        for (float inputDb : {-96.0f, -24.0f}) {
            harken::studio::MeterBallistics single;
            single.advance(0.0f, 0.0f);
            single.advance(inputDb, totalSeconds);
            for (int subdivisions : {3, 60}) {
                harken::studio::MeterBallistics divided;
                divided.advance(0.0f, 0.0f);
                for (int step = 0; step < subdivisions; ++step) {
                    divided.advance(inputDb, totalSeconds / subdivisions);
                    EXPECT_GE(divided.peakDb, divided.levelDb);
                }
                EXPECT_NEAR(divided.levelDb, single.levelDb, 0.001f);
                EXPECT_NEAR(divided.peakDb, single.peakDb, 0.001f);
            }
        }
    }
}

TEST(AmplitudeScale, PreservesRangeAndInverseAcrossCurveSettings)
{
    for (double maximumDb : {0.0, 24.0}) {
        for (double exponent : {0.0, 0.3, 1.0}) {
            const harken::studio::AmplitudeScale axis{-96.0, maximumDb, exponent};
            SCOPED_TRACE(::testing::Message() << "maximum=" << maximumDb << " exponent=" << exponent);
            EXPECT_DOUBLE_EQ(axis.dbToPosition(-96.0), 0.0);
            EXPECT_DOUBLE_EQ(axis.dbToPosition(maximumDb), 1.0);
            double previous = 0.0;
            for (double db : {-72.0, -24.0, -6.0, 0.0}) {
                const double position = axis.dbToPosition(db);
                EXPECT_GT(position, previous);
                EXPECT_NEAR(axis.positionToDb(position), db, 1e-7);
                EXPECT_NEAR(axis.amplitudeToPosition(std::pow(10.0, db / 20.0)), position, 1e-10);
                previous = position;
            }
        }
    }
}
