#include "Engine.h"
#include "Widgets/Meter/MeterBallistics.h"
#include "Widgets/Meter/MeterSource.h"
#include "Widgets/Common/AmplitudeAxis.h"
#include "MonitorCaptureHubTestHooks.h"
#include "MonitorCaptureHub.h"
#include "UnitTest/StudioObjectIdTestSupport.h"

#include <gtest/gtest.h>

#include <QScopeGuard>

#include <algorithm>
#include <array>
#include <cmath>

TEST(MeterSource, CaptureChangesAndTapRetirementStayIsolatedToTheirConsumers)
{
    VnConfig config{};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    ASSERT_EQ(Vn_Init(&config), VN_OK);
    const auto terminate = qScopeGuard([] { Vn_Term(); });
    const auto bus = harkentest::StudioObjectId(8301);
    ASSERT_EQ(Vn_CreateBus(harkentest::ToRuntime(bus), VN_BUS_MASTER), VN_OK);
    ASSERT_EQ(Vn_Update(), VN_OK);
    harken::detail::ActiveEngine()->nullBackend.Pump(1);
    harken::monitor::StopDrainForTests();
    const auto restartDrain = qScopeGuard([] { harken::monitor::StartDrainForTests(); });
    VNX::MeterSource first;
    VNX::MeterSource second;
    VNX::MeterSource otherBus;
    const auto spectrum = HarkenEditor::Bridge::MeterMeasurement::Spectrum;
    ASSERT_TRUE(first.start(harkenstudio::kMasterBusObjectId, spectrum)) << first.result();
    ASSERT_TRUE(second.start(harkenstudio::kMasterBusObjectId, spectrum)) << second.result();
    first.poll();
    second.poll();
    harken::monitor::AdvanceExecuteFrameForTests(1);
    uint64_t sourceFrame = 0;
    const auto publish = [&](bool includeOtherBus) {
        auto* runtime = harken::detail::ActiveEngine();
        for (bool master : {true, false}) {
            if (!master && !includeOtherBus)
                continue;
            std::array<float, 2048> samples;
            const float amplitude = master ? 0.5f : 0.0625f;
            for (size_t index = 0; index < samples.size(); ++index)
                samples[index] = amplitude * std::sin(2.0 * 3.141592653589793 * 64 * index / samples.size());
            float* planes[] = {samples.data(), samples.data()};
            const uint32_t busIndex = runtime->buses.FindIndex(master ? VN_BUS_MASTER : harkentest::ToRuntime(bus));
            harken::monitor::PublishPlanarAudioChunk(master ? VN_MONITOR_TAP_MASTER_OUTPUT : VN_MONITOR_TAP_BUS_POST_EFFECTS,
                busIndex + 1u, runtime->buses.GenerationAt(busIndex), 0, planes, samples.size(), 2, 0,
                VN_CHANNEL_LAYOUT_STEREO, 48000, sourceFrame);
        }
        sourceFrame += 2048;
        harken::monitor::DrainOnceForTests();
    };
    const auto receive = [](VNX::MeterSource& source, float expected) {
        const auto update = source.poll();
        EXPECT_FALSE(update.interrupted);
        ASSERT_FALSE(update.frames.empty());
        for (const auto& frame : update.frames) {
            ASSERT_FALSE(frame.spectrumDb.empty());
            EXPECT_NEAR(*std::max_element(frame.spectrumDb.begin(), frame.spectrumDb.end()), expected, 0.1f);
        }
    };
    publish(false);
    receive(first, -6.0206f);
    ASSERT_TRUE(otherBus.start(bus, spectrum)) << otherBus.result();
    receive(second, -6.0206f);
    otherBus.poll();
    harken::monitor::AdvanceExecuteFrameForTests(1);
    publish(true);
    receive(first, -6.0206f);
    receive(second, -6.0206f);
    receive(otherBus, -24.0824f);
    first.stop();
    otherBus.stop();
    EXPECT_FALSE(second.poll().interrupted);
    harken::monitor::AdvanceExecuteFrameForTests(1);
    publish(false);
    receive(second, -6.0206f);
    ASSERT_TRUE(otherBus.start(bus, spectrum));
    otherBus.stop();
    harken::monitor::AdvanceExecuteFrameForTests(1);
    EXPECT_FALSE(second.poll().interrupted);
    harken::monitor::AdvanceExecuteFrameForTests(1);
    publish(false);
    receive(second, -6.0206f);

    ASSERT_TRUE(otherBus.start(bus, spectrum));
    otherBus.poll();
    harken::monitor::AdvanceExecuteFrameForTests(1);
    publish(true);
    receive(otherBus, -24.0824f);
    const auto* runtime = harken::detail::ActiveEngine();
    const auto busIndex = runtime->buses.FindIndex(harkentest::ToRuntime(bus));
    harken::monitor::NoteExactInstanceRetired(VN_MONITOR_TAP_BUS_POST_EFFECTS,
        busIndex + 1u, runtime->buses.GenerationAt(busIndex));
    harken::monitor::DrainOnceForTests();
    EXPECT_FALSE(second.poll().interrupted);
    EXPECT_FALSE(otherBus.active());
    EXPECT_TRUE(otherBus.poll().interrupted);
    EXPECT_TRUE(second.active());
    EXPECT_FALSE(second.poll().interrupted);
    harken::monitor::AdvanceExecuteFrameForTests(1);
    publish(false);
    receive(second, -6.0206f);
}

TEST(MeterBallistics, ReleaseDependsOnElapsedTimeInsteadOfUpdateFrequency)
{
    for (float totalSeconds : {0.05f, 0.75f, 0.95f, 1.0f, 1.25f, 2.0f}) {
        for (float inputDb : {-96.0f, -24.0f}) {
            VNX::MeterBallistics single;
            single.advance(0.0f, 0.0f);
            single.advance(inputDb, totalSeconds);
            for (int subdivisions : {3, 11, 60}) {
                VNX::MeterBallistics divided;
                divided.advance(0.0f, 0.0f);
                for (int step = 0; step < subdivisions; ++step) {
                    divided.advance(inputDb, totalSeconds / subdivisions);
                    EXPECT_GE(divided.peakDb, divided.levelDb);
                }
                EXPECT_NEAR(divided.levelDb, single.levelDb, 0.001f);
                EXPECT_NEAR(divided.peakDb, single.peakDb, 0.001f);
                auto singleAfterSilence = single;
                singleAfterSilence.advance(-96.0f, 0.1f);
                divided.advance(-96.0f, 0.1f);
                EXPECT_NEAR(divided.levelDb, singleAfterSilence.levelDb, 0.001f);
                EXPECT_NEAR(divided.peakDb, singleAfterSilence.peakDb, 0.001f);
            }
        }
    }
}

TEST(AmplitudeScale, PreservesRangeAndInverseAcrossCurveSettings)
{
    for (double maximumDb : {0.0, 6.0, 24.0}) {
        for (double exponent : {0.0, 1e-10, 0.01, 0.3, 1.0}) {
            const VNX::AmplitudeScale axis{-96.0, maximumDb, exponent};
            SCOPED_TRACE(::testing::Message() << "maximum=" << maximumDb << " exponent=" << exponent);
            EXPECT_DOUBLE_EQ(axis.dbToPosition(-96.0), 0.0);
            EXPECT_DOUBLE_EQ(axis.dbToPosition(maximumDb), 1.0);
            EXPECT_NEAR(axis.positionToDb(0.0), -96.0, 1e-12);
            EXPECT_NEAR(axis.positionToDb(1.0), maximumDb, 1e-12);
            double previous = 0.0;
            for (double db : {-95.9, -72.0, -48.0, -24.0, -6.0, 0.0}) {
                const double position = axis.dbToPosition(db);
                EXPECT_GT(position, previous);
                EXPECT_NEAR(axis.positionToDb(position), db, 1e-7);
                const double amplitude = std::pow(10.0, db / 20.0);
                EXPECT_NEAR(axis.amplitudeToPosition(amplitude), position, 1e-10);
                EXPECT_NEAR(axis.positionToAmplitude(position), amplitude, 1e-10);
                previous = position;
            }
        }
    }
}
