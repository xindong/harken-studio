#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"
#include "UnitTest/AllocationProbe.h"
#include "harken/core/ChannelPeakAccumulator.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <thread>
#include <vector>

namespace {

TEST(ChannelPeakAccumulator, RetainsIndependentMaximaAndReplacesRetiredGenerations) {
    harken::ChannelPeakAccumulator peaks;
    float value = -1.0f;
    EXPECT_FALSE(peaks.Take(0, 1, 0, value));
    EXPECT_FLOAT_EQ(-1.0f, value);
    for (std::uint32_t block = 0; block < 4096; ++block) {
        for (std::uint32_t bus = 0; bus < VN_SNAPSHOT_MAX_BUSES; ++bus) {
            for (std::uint32_t channel = 0; channel < VN_MAX_CHANNELS; ++channel) {
                const float peak = block == bus + channel ? (channel + 1) * 0.1f : 0.01f;
                peaks.Publish(bus, 1, channel, peak);
            }
        }
    }
    for (std::uint32_t bus = 0; bus < VN_SNAPSHOT_MAX_BUSES; ++bus) {
        for (std::uint32_t channel = 0; channel < VN_MAX_CHANNELS; ++channel) {
            EXPECT_TRUE(peaks.Take(bus, 1, channel, value));
            EXPECT_FLOAT_EQ((channel + 1) * 0.1f, value);
            EXPECT_FALSE(peaks.Take(bus, 1, channel, value));
            EXPECT_FLOAT_EQ((channel + 1) * 0.1f, value);
        }
    }
    peaks.Publish(1, 1, 0, 0.9f);
    value = -1.0f;
    EXPECT_FALSE(peaks.Take(1, 2, 0, value));
    EXPECT_FLOAT_EQ(-1.0f, value);
    peaks.Publish(1, 1, 0, 0.9f);
    peaks.Publish(1, 2, 0, 0.1f);
    EXPECT_TRUE(peaks.Take(1, 2, 0, value));
    EXPECT_FLOAT_EQ(0.1f, value);
    peaks.Publish(1, 2, 0, 0.0f);
    EXPECT_TRUE(peaks.Take(1, 2, 0, value));
    EXPECT_FLOAT_EQ(0.0f, value);
    EXPECT_FALSE(peaks.Take(1, 2, 0, value));
    EXPECT_FLOAT_EQ(0.0f, value);
    peaks.Publish(1, 2, 0, std::numeric_limits<float>::infinity());
    peaks.Publish(1, 2, 0, std::numeric_limits<float>::quiet_NaN());
    EXPECT_TRUE(peaks.Take(1, 2, 0, value));
    EXPECT_FLOAT_EQ(0.0f, value);
    peaks.Publish(1, 2, 0, 1.5f);
    EXPECT_TRUE(peaks.Take(1, 2, 0, value));
    EXPECT_FLOAT_EQ(1.5f, value);
}

TEST(ChannelPeakAccumulator, ConcurrentTakeRetainsEveryRoundImpulse) {
    harken::ChannelPeakAccumulator peaks;
    std::atomic<std::uint32_t> published{0}, consumed{0};
    std::atomic<bool> abort{false};
    constexpr std::uint32_t rounds = 4000;
    std::thread producer([&] {
        for (std::uint32_t round = 1; round <= rounds; ++round) {
            while (consumed.load(std::memory_order_acquire) != round - 1) {
                if (abort.load(std::memory_order_relaxed)) return;
                std::this_thread::yield();
            }
            peaks.Publish(0, 7, 0, static_cast<float>(round));
            peaks.Publish(0, 7, 0, 0.0f);
            published.store(round, std::memory_order_release);
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::uint32_t lost = 0, completed = 0, emptyMutations = 0;
    for (std::uint32_t round = 1; round <= rounds; ++round) {
        float maximum = 0.0f;
        while (published.load(std::memory_order_acquire) < round) {
            float value = -1.0f;
            if (peaks.Take(0, 7, 0, value))
                maximum = std::max(maximum, value);
            else
                emptyMutations += value != -1.0f;
            if (std::chrono::steady_clock::now() >= deadline) {
                abort.store(true, std::memory_order_relaxed);
                break;
            }
            std::this_thread::yield();
        }
        if (abort.load(std::memory_order_relaxed)) break;
        float value = -1.0f;
        if (peaks.Take(0, 7, 0, value))
            maximum = std::max(maximum, value);
        else
            emptyMutations += value != -1.0f;
        lost += maximum != static_cast<float>(round);
        completed = round;
        consumed.store(round, std::memory_order_release);
    }
    producer.join();
    EXPECT_EQ(rounds, completed);
    EXPECT_EQ(0u, lost);
    EXPECT_EQ(0u, emptyMutations);
    float value = -1.0f;
    EXPECT_FALSE(peaks.Take(0, 7, 0, value));
    EXPECT_FLOAT_EQ(-1.0f, value);
}

TEST(ChannelPeakMeter, DelayedTakeRetainsRenderedTransientWithoutChangingSnapshot) {
    for (const std::uint32_t channels : {1u, 2u, 8u}) {
        const auto layout = channels == 1 ? VN_CHANNEL_LAYOUT_MONO
            : channels == 2 ? VN_CHANNEL_LAYOUT_STEREO : VN_CHANNEL_LAYOUT_7_1;
        harkentest::EngineSession session(
            harkentest::MakeNullConfigLayout(4, 128, 48000, layout, layout));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto* engine = session.Engine();
        harken::ChannelPeakBatch batch;
        EXPECT_FALSE(engine->mixer.TakeChannelPeaks(batch));
        engine->mixer.EnableChannelPeakMetering();
        const auto busId = harkentest::ObjectId(42);
        ASSERT_EQ(VN_OK, Vn_CreateBus(busId, VN_BUS_MASTER));
        std::vector<float> samples(128 * 80 * channels);
        for (std::size_t frame = 0; frame < samples.size() / channels; ++frame)
            for (std::uint32_t channel = 0; channel < channels; ++channel)
                samples[frame * channels + channel] =
                    (frame < 128 ? 0.1f : 0.01f) * (channel + 1);
        const auto pcm = harkentest::InterleavedDesc(samples, channels, 48000);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
        auto play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
        play.busId = busId;
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        const auto allocationsBefore = allocation_probe::count;
        allocation_probe::enabled = true;
        engine->nullBackend.Pump(64);
        allocation_probe::enabled = false;
        EXPECT_EQ(allocationsBefore, allocation_probe::count);
        VnSnapshot snapshot{};
        snapshot.structSize = sizeof(snapshot);
        ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
        EXPECT_EQ(63u, snapshot.blockIndex);
        engine->mixer.EnableChannelPeakMetering();
        ASSERT_TRUE(engine->mixer.TakeChannelPeaks(batch));
        const auto firstSerial = batch.consumeSerial;
        const auto bus = engine->buses.FindIndex(busId);
        ASSERT_EQ(busId, batch.busIds[bus]);
        EXPECT_EQ(channels, batch.mixChannelCount);
        for (const auto row : {0u, bus}) {
            for (std::uint32_t channel = 0; channel < channels; ++channel) {
                EXPECT_NEAR(0.1f * (channel + 1),
                    batch.channelPeaks[row * VN_MAX_CHANNELS + channel], 1e-5f);
                EXPECT_NEAR(0.01f * (channel + 1),
                    snapshot.busChannelPeaks[row * VN_MAX_CHANNELS + channel], 1e-5f);
            }
        }
        const auto retainedPeaks = batch.channelPeaks;
        engine->mixer.EnableChannelPeakMetering();
        ASSERT_TRUE(engine->mixer.TakeChannelPeaks(batch));
        EXPECT_GT(batch.consumeSerial, firstSerial);
        EXPECT_EQ(retainedPeaks, batch.channelPeaks);
        engine->nullBackend.Pump(1);
        ASSERT_TRUE(engine->mixer.TakeChannelPeaks(batch));
        EXPECT_NEAR(0.01f, batch.channelPeaks[0], 1e-5f);
        const auto sustainedPeaks = batch.channelPeaks;
        for (int read = 0; read < 4; ++read) {
            ASSERT_TRUE(engine->mixer.TakeChannelPeaks(batch));
            EXPECT_EQ(sustainedPeaks, batch.channelPeaks);
        }
        ASSERT_EQ(VN_OK, Vn_Stop(voice, 0));
        engine->nullBackend.Pump(1);
        ASSERT_TRUE(engine->mixer.TakeChannelPeaks(batch));
        for (const float peak : batch.channelPeaks) EXPECT_FLOAT_EQ(0.0f, peak);
        ASSERT_TRUE(engine->mixer.TakeChannelPeaks(batch));
        for (const float peak : batch.channelPeaks) EXPECT_FLOAT_EQ(0.0f, peak);
    }
}

TEST(ChannelPeakMeter, BusSlotReuseRejectsRetiredGenerationBeforeNewAudio) {
    for (const bool observeDeletion : {false, true}) {
        harkentest::EngineSession session(harkentest::MakeNullConfig(2, 128));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto* engine = session.Engine();
        engine->mixer.EnableChannelPeakMetering();
        const auto oldId = harkentest::ObjectId(42), newId = harkentest::ObjectId(43);
        ASSERT_EQ(VN_OK, Vn_CreateBus(oldId, VN_BUS_MASTER));
        const std::vector<float> samples(4096, 0.8f);
        const auto pcm = harkentest::MonoDesc(samples, 48000);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
        auto play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
        play.busId = oldId;
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        engine->nullBackend.Pump(2);
        const auto oldRef = engine->buses.FindRef(oldId);
        harken::ChannelPeakBatch batch;
        ASSERT_TRUE(engine->mixer.TakeChannelPeaks(batch));
        ASSERT_GT(batch.channelPeaks[oldRef.index * VN_MAX_CHANNELS], 0.1f);
        ASSERT_EQ(VN_OK, Vn_DestroyBus(oldId));
        if (observeDeletion) {
            ASSERT_TRUE(engine->mixer.TakeChannelPeaks(batch));
            EXPECT_EQ(VnBusId{}, batch.busIds[oldRef.index]);
            EXPECT_FLOAT_EQ(0.0f, batch.channelPeaks[oldRef.index * VN_MAX_CHANNELS]);
        }
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_EQ(VN_OK, Vn_CreateBus(newId, VN_BUS_MASTER));
        const auto newRef = engine->buses.FindRef(newId);
        ASSERT_EQ(oldRef.index, newRef.index);
        ASSERT_NE(oldRef.generation, newRef.generation);
        ASSERT_TRUE(engine->mixer.TakeChannelPeaks(batch));
        EXPECT_EQ(newId, batch.busIds[newRef.index]);
        EXPECT_FLOAT_EQ(0.0f, batch.channelPeaks[newRef.index * VN_MAX_CHANNELS]);
        engine->nullBackend.Pump(1);
        ASSERT_TRUE(engine->mixer.TakeChannelPeaks(batch));
        EXPECT_FLOAT_EQ(0.0f, batch.channelPeaks[newRef.index * VN_MAX_CHANNELS]);
    }
}

TEST(ChannelPeakMeter, DegradedBlockDoesNotRepublishPreviousOutput) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(2, 128));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    engine->mixer.EnableChannelPeakMetering();
    const std::vector<float> samples(4096, 0.8f);
    const auto pcm = harkentest::MonoDesc(samples, 48000);
    const auto soundId = harkentest::ObjectId(1);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(soundId, &pcm));
    auto play = harkentest::MakePlayDesc(soundId);
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    engine->nullBackend.Pump(2);
    harken::ChannelPeakBatch batch;
    ASSERT_TRUE(engine->mixer.TakeChannelPeaks(batch));
    ASSERT_GT(batch.channelPeaks[0], 0.1f);
    engine->mixer.ArenaForTests().SetCapacityForTests(64);
    engine->nullBackend.Pump(1);
    ASSERT_TRUE(engine->mixer.TakeChannelPeaks(batch));
    for (const float peak : batch.channelPeaks) EXPECT_FLOAT_EQ(0.0f, peak);
}

} // namespace
