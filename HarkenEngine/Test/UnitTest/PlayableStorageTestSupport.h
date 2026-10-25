#ifndef HARKEN_TESTS_PLAYABLE_STORAGE_TEST_SUPPORT_H
#define HARKEN_TESTS_PLAYABLE_STORAGE_TEST_SUPPORT_H

#include "Common/TestUtil.h"

#include <array>
#include <utility>

namespace harkentest {

inline harken::Plan03GroupSliceIdentity QueueContinuationSlice(
    const harken::Engine& engine, harken::DecisionToken decision,
    std::uint16_t slice) {
    for (const auto& bank : engine.queueProgramBanks) {
        const auto& lane = bank.continuationLane;
        if (lane.Active() && lane.Decision().slot == decision.slot &&
            lane.Decision().generation == decision.generation)
            return lane.GroupIdentity(slice);
    }
    return {};
}

class PlayableCapacityScope {
public:
    PlayableCapacityScope(std::uint32_t playable, std::uint32_t modes) {
        harken::detail::SetPlayableCapacitiesForTests(playable, modes);
    }
    ~PlayableCapacityScope() { harken::detail::SetPlayableCapacitiesForTests(0, 0); }
    PlayableCapacityScope(const PlayableCapacityScope&) = delete;
    PlayableCapacityScope& operator=(const PlayableCapacityScope&) = delete;
};

template <typename T>
auto StorageIdentity(const std::vector<T>& rows) {
    return std::pair<const void*, std::size_t>{rows.data(), rows.capacity()};
}

template <typename T>
auto StorageIdentity(const harken::StableSlotStorage<T>& rows) {
    return std::pair<const void*, std::size_t>{rows.BackingAddress(), rows.size()};
}

template <typename T, typename Index>
auto StorageIdentity(const harken::StableRowStorage<T, Index>& rows) {
    return std::pair<const void*, std::size_t>{rows.data(), rows.Capacity()};
}

inline auto CapturePlayableStorage(const harken::Engine& engine) {
    return std::array{
        StorageIdentity(engine.playableStarts),
        StorageIdentity(engine.initialFrontierEvents),
        StorageIdentity(engine.initialFrontierRequests),
        StorageIdentity(engine.playableServices),
        StorageIdentity(engine.memberRelations),
        StorageIdentity(engine.playableMemberRecords),
        StorageIdentity(engine.memberProgramRecords),
        StorageIdentity(engine.variableControlRetargetPrograms),
        StorageIdentity(engine.queueProgramBanks)};
}

class PlayableStorageObservation {
public:
    explicit PlayableStorageObservation(harken::Engine& engine, bool enabled = true)
        : engine_(engine), enabled_(enabled), backing_(CapturePlayableStorage(engine)) {}

    template <typename Operation>
    VnResult Measure(Operation&& operation) {
        if (!enabled_) return operation();
        harken::memguard::AllocationObservation observed{};
        VnResult result;
        {
            harken::memguard::AllocationObservationScope scope(observed);
            result = operation();
        }
        available_ = available_ && observed.available;
        Add(logic_, observed.logic);
        Add(audio_, observed.audio);
        ++operations_;
        CheckStorage();
        return result;
    }

    void Pump(std::uint32_t blocks) {
        (void)Measure([&] { engine_.nullBackend.Pump(blocks); return VN_OK; });
    }

    void CheckStorage() {
        if (!enabled_) return;
        EXPECT_EQ(backing_, CapturePlayableStorage(engine_));
        EXPECT_LE(engine_.playablePartitions.OccupiedCount(), engine_.config.maxPlayableInstances);
        peakPartitions_ = std::max(peakPartitions_, engine_.playablePartitions.OccupiedCount());
        std::uint32_t services = 0;
        for (const auto& row : engine_.playableServices) {
            if (!row.partition.Valid()) continue;
            ++services;
            EXPECT_TRUE(engine_.playablePartitions.Owns(row.partition, row.identity));
            EXPECT_NE(nullptr, row.invocation);
            EXPECT_EQ(row.invocationGeneration, row.Invocation().generation);
        }
        EXPECT_EQ(services, engine_.playablePartitions.OccupiedCount());
    }

    void Finish() {
        if (!enabled_) return;
        CheckStorage();
#if defined(_DEBUG) && defined(_MSC_VER)
        EXPECT_TRUE(available_) << "Debug CRT hook unavailable during service observation";
#endif
        EXPECT_GT(operations_, 0u);
        EXPECT_GT(peakPartitions_, 0u);
        testing::Test::RecordProperty("storage_observed_operations", std::to_string(operations_));
        testing::Test::RecordProperty("storage_peak_partitions", std::to_string(peakPartitions_));
        testing::Test::RecordProperty("storage_backing_vectors", std::to_string(backing_.size()));
        const bool allocationAvailable = available_ && operations_ > 0;
        testing::Test::RecordProperty("allocation_observation_available", allocationAvailable ? "true" : "false");
        if (!allocationAvailable) return;
        // Control-side preparation may grow actual columns before publication.
        EXPECT_EQ(0u, audio_.allocations);
        EXPECT_EQ(0u, audio_.reallocations);
        EXPECT_EQ(0u, audio_.allocationBytes);
        EXPECT_EQ(0u, audio_.reallocationBytes);
        testing::Test::RecordProperty("logic_alloc_events", std::to_string(logic_.allocations));
        testing::Test::RecordProperty("logic_realloc_events", std::to_string(logic_.reallocations));
        testing::Test::RecordProperty("logic_alloc_requested_bytes", std::to_string(logic_.allocationBytes));
        testing::Test::RecordProperty("logic_realloc_requested_bytes", std::to_string(logic_.reallocationBytes));
        testing::Test::RecordProperty("audio_alloc_events", std::to_string(audio_.allocations));
        testing::Test::RecordProperty("audio_realloc_events", std::to_string(audio_.reallocations));
        testing::Test::RecordProperty("audio_alloc_requested_bytes", std::to_string(audio_.allocationBytes));
        testing::Test::RecordProperty("audio_realloc_requested_bytes", std::to_string(audio_.reallocationBytes));
    }

private:
    static void Add(harken::memguard::AllocationEvents& total,
                    const harken::memguard::AllocationEvents& value) {
        total.allocations += value.allocations;
        total.reallocations += value.reallocations;
        total.allocationBytes += value.allocationBytes;
        total.reallocationBytes += value.reallocationBytes;
    }
    harken::Engine& engine_;
    bool enabled_;
    decltype(CapturePlayableStorage(std::declval<const harken::Engine&>())) backing_;
    bool available_ = true;
    std::uint32_t operations_ = 0;
    std::uint32_t peakPartitions_ = 0;
    harken::memguard::AllocationEvents logic_{};
    harken::memguard::AllocationEvents audio_{};
};

} // namespace harkentest

#endif
