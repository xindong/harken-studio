#include "Common/ObjectIdTestSupport.h"
#include "harken/param/Spatializer.h"
#include "harken/core/LogicAllocCheckpoint.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <new>
#include <type_traits>
#include <utility>

namespace allocation_probe {
extern thread_local bool enabled;
extern thread_local std::size_t count;
}

namespace {

using Key = harken::Spatializer::RetainedProfileKey;
using Prepared = harken::Spatializer::PreparedProfileSet;

class AllocationProbeScope {
public:
    AllocationProbeScope() : previous_(allocation_probe::enabled) {
        allocation_probe::count = 0;
        allocation_probe::enabled = true;
    }
    ~AllocationProbeScope() { allocation_probe::enabled = previous_; }
private:
    bool previous_;
};

class AllocationFaultScope {
public:
    explicit AllocationFaultScope(std::size_t nth) {
        harken::detail::ArmLogicAllocFaultForTests(nth, harken::detail::LogicAllocSite::None);
    }
    ~AllocationFaultScope() { harken::detail::DisarmLogicAllocFaultForTests(); }
};

VnAttenuationDesc Profile(float maxDistance = 10.0f) {
    VnAttenuationDesc desc{};
    desc.structSize = sizeof(desc);
    desc.rolloff = VN_ROLLOFF_LINEAR;
    desc.minDistance = 1.0f;
    desc.maxDistance = maxDistance;
    desc.airAbsorptionNearHz = 1.0e9f;
    desc.airAbsorptionFarHz = 1.0e9f;
    return desc;
}

std::vector<Key> Generations(std::uint64_t last) {
    std::vector<Key> keys;
    for (std::uint64_t generation = 1; generation <= last; ++generation) {
        keys.push_back({harkentest::ObjectId(17), generation});
    }
    return keys;
}

VnResult Install(harken::Spatializer& spatial, const VnAttenuationDesc& desc,
                 std::uint64_t generation, const std::vector<Key>& keys = {}) {
    const harken::Spatializer::PackProfileInput input{harkentest::ObjectId(17), &desc};
    Prepared prepared;
    const auto result = spatial.PreparePackProfiles(&input, 1, generation, &prepared, keys);
    if (result == VN_OK) spatial.CommitPreparedSet(&prepared);
    return result;
}

static_assert(std::is_nothrow_destructible_v<Prepared>);
static_assert(std::is_nothrow_move_constructible_v<Prepared>);
static_assert(std::is_nothrow_move_assignable_v<Prepared>);
static_assert(noexcept(std::declval<harken::Spatializer&>().CommitPreparedSet(nullptr)));
static_assert(std::is_trivially_copy_assignable_v<harken::AttenuationProfile>);

TEST(CanonicalAttenuationGeneration, MirrorsAndDelayedBindingsKeepExactOldCurves) {
    harken::EngineConfig config{};
    ASSERT_TRUE(harken::VoiceCapacityModel::TryCreate(4, 4, &config.voiceCapacity));
    harken::Spatializer spatial(config);
    ASSERT_EQ(VN_OK, Install(spatial, Profile(), 1));
    const VnVoiceHandle first = (1ull << 32), delayed = first + 1;
    spatial.OnVoiceStarted(first, true, 5.0f, 0.0f, 0.0f);
    ASSERT_EQ(VN_OK, spatial.BindVoice(first, harkentest::ObjectId(17), 1));
    const auto oldDb = spatial.AttenuationVolumeDb(harkentest::ObjectId(17), 5.0f, 0.0f, 0.0f, 1.0f, 1);
    spatial.SetRetainCommitPageLimitForTests(0);
    EXPECT_EQ(VN_BACKPRESSURE, Install(spatial, Profile(20), 2, Generations(1)));
    EXPECT_EQ(nullptr, spatial.FindProfile(harkentest::ObjectId(17), 2));
    EXPECT_FLOAT_EQ(oldDb, spatial.AttenuationVolumeDb(harkentest::ObjectId(17), 5.0f, 0.0f, 0.0f, 1.0f, 1));
    spatial.SetRetainCommitPageLimitForTests(SIZE_MAX);
    ASSERT_EQ(VN_OK, Install(spatial, Profile(20), 2, Generations(1)));
    ASSERT_EQ(VN_OK, Install(spatial, Profile(30), 3, Generations(2)));
    spatial.SetRetainCommitPageLimitForTests(0);
    harken::Spatializer::SpatialVoiceState parked{};
    ASSERT_TRUE(spatial.CaptureVoice(first, &parked));
    EXPECT_EQ(harkentest::ObjectId(17u), parked.attenuationId);
    EXPECT_EQ(1u, parked.attenuationGeneration);
    EXPECT_FLOAT_EQ(oldDb, spatial.AttenuationVolumeDb(harkentest::ObjectId(17), 5.0f, 0.0f, 0.0f, 1.0f, 1));
    spatial.OnVoiceStarted(delayed, true, 5.0f, 0.0f, 0.0f);
    ASSERT_EQ(VN_OK, spatial.BindVoice(delayed, parked.attenuationId, parked.attenuationGeneration));
    ASSERT_EQ(VN_OK, spatial.BindVoice(first, harkentest::ObjectId(17), 3));
    ASSERT_EQ(VN_OK, spatial.SweepRetainedProfiles());
    EXPECT_NE(nullptr, spatial.FindProfile(harkentest::ObjectId(17), 1));
    EXPECT_EQ(nullptr, spatial.FindProfile(harkentest::ObjectId(17), 2));
    spatial.NoteVoiceEnded(delayed);
    ASSERT_EQ(VN_OK, spatial.SweepRetainedProfiles(Generations(1)));
    EXPECT_NE(nullptr, spatial.FindProfile(harkentest::ObjectId(17), 1));
    ASSERT_EQ(VN_OK, spatial.SweepRetainedProfiles());
    EXPECT_EQ(nullptr, spatial.FindProfile(harkentest::ObjectId(17), 1));
    EXPECT_EQ(VN_UNKNOWN_ID, spatial.BindVoice(first, harkentest::ObjectId(17), 1));
    ASSERT_TRUE(spatial.CaptureVoice(first, &parked));
    EXPECT_EQ(3u, parked.attenuationGeneration);
}

TEST(CanonicalAttenuationGeneration, UnchangedAndCancellationConsumeNoRetainSlots) {
    harken::Spatializer spatial(harken::EngineConfig{});
    ASSERT_EQ(VN_OK, Install(spatial, Profile(), 1));
    for (std::uint64_t generation = 2; generation < 300; ++generation) {
        ASSERT_EQ(VN_OK, Install(spatial, Profile(), generation, Generations(1)));
    }
    EXPECT_EQ(0u, spatial.RetainedProfileCount());
    auto desc = Profile(20);
    const harken::Spatializer::PackProfileInput input{harkentest::ObjectId(17), &desc};
    {
        Prepared prepared;
        ASSERT_EQ(VN_OK, spatial.PreparePackProfiles(&input, 1, 300, &prepared, Generations(1)));
        EXPECT_FLOAT_EQ(10, spatial.FindProfile(harkentest::ObjectId(17), 1)->maxDistance);
        EXPECT_EQ(nullptr, spatial.FindProfile(harkentest::ObjectId(17), 300));
    }
    ASSERT_EQ(VN_OK, Install(spatial, desc, 300, Generations(1)));
    EXPECT_EQ(1u, spatial.RetainedProfileCount());
}

TEST(CanonicalAttenuationGeneration, EndAndSlotReuseSweepWithoutNotifications) {
    harken::EngineConfig config{};
    ASSERT_TRUE(harken::VoiceCapacityModel::TryCreate(2, 2, &config.voiceCapacity));
    harken::Spatializer spatial(config);
    ASSERT_EQ(VN_OK, Install(spatial, Profile(), 1));
    const VnVoiceHandle first = (1ull << 32), reused = (2ull << 32), ended = first + 1;
    spatial.OnVoiceStarted(first, true, 5, 0, 0);
    spatial.OnVoiceStarted(ended, true, 5, 0, 0);
    ASSERT_EQ(VN_OK, spatial.BindVoice(first, harkentest::ObjectId(17), 1));
    ASSERT_EQ(VN_OK, spatial.BindVoice(ended, harkentest::ObjectId(17), 1));
    ASSERT_EQ(VN_OK, Install(spatial, Profile(20), 2));
    spatial.OnVoiceStarted(reused, true, 5, 0, 0);
    ASSERT_EQ(VN_OK, spatial.BindVoice(reused, harkentest::ObjectId(17), 2));
    ASSERT_EQ(VN_OK, spatial.SweepRetainedProfiles());
    EXPECT_EQ(1u, spatial.RetainedProfileCount());
    VnVoiceHandle live = reused;
    spatial.SweepEndedVoices(+[](void* context, VnVoiceHandle h) noexcept {
        return h == *static_cast<VnVoiceHandle*>(context);
    }, &live);
    ASSERT_EQ(VN_OK, spatial.SweepRetainedProfiles());
    EXPECT_EQ(0u, spatial.RetainedProfileCount());
}

TEST(CanonicalAttenuationGeneration, ConfiguredCapacityHasNo256HardCap) {
    for (const auto plans : {0u, 4u, 5u}) {
        harken::EngineConfig config{};
        config.topologyPlanPoolCapacity = plans;
        harken::Spatializer spatial(config);
        const auto capacity = (plans == 0 ? 4u : plans) * 64u;
        ASSERT_EQ(VN_OK, Install(spatial, Profile(), 1));
        for (std::uint64_t generation = 2; generation <= capacity + 1; ++generation) {
            ASSERT_EQ(VN_OK, Install(spatial, Profile(static_cast<float>(generation + 10)),
                                    generation, Generations(generation - 1)));
        }
        EXPECT_EQ(capacity, spatial.RetainedProfileCount());
        EXPECT_EQ(VN_PACK_RESIDENT_CAPACITY,
                  Install(spatial, Profile(1000), capacity + 2, Generations(capacity + 1)));
        EXPECT_NE(nullptr, spatial.FindProfile(harkentest::ObjectId(17), capacity + 1));
        EXPECT_EQ(nullptr, spatial.FindProfile(harkentest::ObjectId(17), capacity + 2));
        ASSERT_EQ(VN_OK, spatial.SweepRetainedProfiles());
        EXPECT_EQ(0u, spatial.RetainedProfileCount());
        EXPECT_EQ(VN_OK, Install(spatial, Profile(1000), capacity + 2));
    }
}

TEST(CanonicalAttenuationGeneration, ExhaustedEpochDoesNotWrapOrReuse) {
    harken::EngineConfig config{};
    config.topologyPlanPoolCapacity = 1;
    harken::Spatializer spatial(config);
    for (std::size_t slot = 0; slot < 64; ++slot) {
        ASSERT_EQ(VN_OK, spatial.SetRetainEpochForTests(slot, slot == 0 ? UINT32_MAX - 1 : UINT32_MAX));
    }
    ASSERT_EQ(VN_OK, Install(spatial, Profile(), 1));
    ASSERT_EQ(VN_OK, Install(spatial, Profile(20), 2, Generations(1)));
    ASSERT_EQ(VN_OK, spatial.SweepRetainedProfiles());
    EXPECT_EQ(VN_PACK_RESIDENT_CAPACITY, Install(spatial, Profile(30), 3, Generations(2)));
    EXPECT_NE(nullptr, spatial.FindProfile(harkentest::ObjectId(17), 2));
    EXPECT_EQ(nullptr, spatial.FindProfile(harkentest::ObjectId(17), 3));
}

TEST(CanonicalAttenuationGeneration, LiveHolesPreserveDirectProfilesAndOldParkBinding) {
    harken::Spatializer spatial(harken::EngineConfig{});
    auto desc = Profile();
    ASSERT_EQ(VN_OK, spatial.RegisterProfile(harkentest::ObjectId(0), &desc));
    ASSERT_EQ(VN_OK, Install(spatial, desc, 1));
    ASSERT_EQ(VN_OK, spatial.RegisterProfile(harkentest::ObjectId(19), &desc));
    const auto* direct = spatial.FindProfile(harkentest::ObjectId(19));
    Prepared removed;
    ASSERT_EQ(VN_OK, spatial.PreparePackProfiles(nullptr, 0, 0, &removed, Generations(1)));
    spatial.CommitPreparedSet(&removed);
    EXPECT_EQ(2u, spatial.ProfileCount());
    EXPECT_EQ(direct, spatial.FindProfile(harkentest::ObjectId(19)));
    EXPECT_NE(nullptr, spatial.FindProfile(harkentest::ObjectId(0)));
    EXPECT_EQ(nullptr, spatial.FindProfile(harkentest::ObjectId(17)));
    EXPECT_NE(nullptr, spatial.FindProfile(harkentest::ObjectId(17), 1));
    desc.maxDistance = 20;
    ASSERT_EQ(VN_OK, Install(spatial, desc, 2));
    EXPECT_FLOAT_EQ(10, spatial.FindProfile(harkentest::ObjectId(17), 1)->maxDistance);
    EXPECT_FLOAT_EQ(20, spatial.FindProfile(harkentest::ObjectId(17), 2)->maxDistance);
    EXPECT_EQ(VN_IN_USE, spatial.ReplaceProfile(harkentest::ObjectId(17), &desc));
    EXPECT_EQ(VN_OK, spatial.ReplaceProfile(harkentest::ObjectId(19), &desc));
}

TEST(CanonicalAttenuationGeneration, InvalidKeysRejectBeforeAnyReservationOrReclamation) {
    harken::Spatializer spatial(harken::EngineConfig{});
    ASSERT_EQ(VN_OK, Install(spatial, Profile(), 1));
    ASSERT_EQ(VN_OK, Install(spatial, Profile(20), 2, Generations(1)));
    const std::vector<std::vector<Key>> invalid{
        {{harkentest::ObjectId(17), 1}, {harkentest::ObjectId(17), 1}}, {{harkentest::ObjectId(17), 2}, {harkentest::ObjectId(17), 1}}, {{harkentest::ObjectId(0), 1}}, {{harkentest::ObjectId(17), 0}},
        {{harkentest::ObjectId(18), 1}, {harkentest::ObjectId(17), 1}}
    };
    for (const auto& keys : invalid) {
        EXPECT_EQ(VN_INVALID_ARG, spatial.SweepRetainedProfiles(keys));
        EXPECT_EQ(1u, spatial.RetainedProfileCount());
        EXPECT_NE(nullptr, spatial.FindProfile(harkentest::ObjectId(17), 1));
        EXPECT_EQ(VN_INVALID_ARG, Install(spatial, Profile(30), 3, keys));
        EXPECT_EQ(nullptr, spatial.FindProfile(harkentest::ObjectId(17), 3));
    }
    EXPECT_EQ(VN_PACK_RESIDENT_CAPACITY, spatial.SweepRetainedProfiles(Generations(321)));
    EXPECT_EQ(1u, spatial.RetainedProfileCount());
    EXPECT_EQ(VN_OK, Install(spatial, Profile(30), 3, Generations(2)));
    EXPECT_EQ(2u, spatial.RetainedProfileCount());
}

TEST(CanonicalAttenuationGeneration, LargeSortedKeysAndValueLifetimePreserveExactGenerations) {
    harken::EngineConfig config{};
    config.topologyPlanPoolCapacity = 5;
    harken::Spatializer spatial(config);
    ASSERT_EQ(VN_OK, Install(spatial, Profile(), 1));
    ASSERT_EQ(VN_OK, Install(spatial, Profile(20), 2, Generations(1)));
    std::vector<Key> keys{{harkentest::ObjectId(1), 1}, {harkentest::ObjectId(17), 1}, {harkentest::ObjectId(17), 2}};
    for (std::uint64_t id = 18; id < 350; ++id) keys.push_back({harkentest::ObjectId(id), UINT64_MAX});
    ASSERT_EQ(VN_OK, spatial.SweepRetainedProfiles(keys));
    auto desc = Profile(30);
    const harken::Spatializer::PackProfileInput input{harkentest::ObjectId(17), &desc};
    Prepared next;
    ASSERT_EQ(VN_OK, spatial.PreparePackProfiles(&input, 1, 3, &next, keys));
    keys.clear();
    keys.shrink_to_fit();
    spatial.CommitPreparedSet(&next);
    EXPECT_EQ(2u, spatial.RetainedProfileCount());
    ASSERT_EQ(VN_OK, spatial.SweepRetainedProfiles({{harkentest::ObjectId(17), 2}, {harkentest::ObjectId(19), 1}}));
    EXPECT_EQ(nullptr, spatial.FindProfile(harkentest::ObjectId(17), 1));
    EXPECT_NE(nullptr, spatial.FindProfile(harkentest::ObjectId(17), 2));
    ASSERT_EQ(VN_OK, spatial.SweepRetainedProfiles());
    EXPECT_EQ(0u, spatial.RetainedProfileCount());
}

TEST(CanonicalAttenuationGeneration, PendingGateMovesAndSweepsAreSideEffectFree) {
    harken::EngineConfig config{};
    harken::Spatializer spatial(config), other(config);
    ASSERT_EQ(VN_OK, Install(spatial, Profile(), 1));
    ASSERT_EQ(VN_OK, Install(spatial, Profile(20), 2, Generations(1)));
    const auto voice = UINT64_C(1) << 32;
    spatial.OnVoiceStarted(voice, true, 5, 0, 0);
    ASSERT_EQ(VN_OK, spatial.BindVoice(voice, harkentest::ObjectId(17), 2));
    auto desc = Profile(30);
    const harken::Spatializer::PackProfileInput input{harkentest::ObjectId(17), &desc};
    Prepared first, refused, target;
    ASSERT_EQ(VN_OK, spatial.PreparePackProfiles(&input, 1, 3, &first, Generations(2)));
    EXPECT_EQ(VN_IN_USE, spatial.PreparePackProfiles(&input, 1, 3, &refused));
    EXPECT_EQ(VN_IN_USE, spatial.PreparePackProfiles(&input, 1, 3, &first));
    EXPECT_EQ(VN_IN_USE, other.PreparePackProfiles(&input, 1, 3, &first));
    EXPECT_EQ(VN_IN_USE, spatial.SweepRetainedProfiles({{harkentest::ObjectId(0), 0}}));
    int callbacks = 0;
    spatial.SweepEndedVoices(+[](void* context, VnVoiceHandle) noexcept {
        ++*static_cast<int*>(context);
        return false;
    }, &callbacks);
    EXPECT_EQ(0, callbacks);
    EXPECT_EQ(1u, spatial.RetainedProfileCount());
    ASSERT_EQ(VN_OK, other.PreparePackProfiles(&input, 1, 1, &target));
    target = std::move(first);
    EXPECT_FALSE(first.IsPrepared());
    Prepared moved(std::move(target));
    EXPECT_EQ(VN_IN_USE, spatial.PreparePackProfiles(&input, 1, 3, &refused));
    ASSERT_EQ(VN_OK, other.PreparePackProfiles(&input, 1, 1, &refused));
    Prepared empty;
    refused = std::move(empty);
    ASSERT_EQ(VN_OK, other.PreparePackProfiles(&input, 1, 1, &refused));
    spatial.CommitPreparedSet(&moved);
    EXPECT_EQ(2u, spatial.RetainedProfileCount());
    EXPECT_EQ(VN_OK, spatial.PreparePackProfiles(nullptr, 0, 0, &first));
    spatial.CancelPreparedSet(&first);
    EXPECT_FALSE(first.IsPrepared());
    EXPECT_EQ(VN_OK, spatial.SweepRetainedProfiles());
}

TEST(CanonicalAttenuationGeneration, InvalidCommitKeepsPreparedOwnerRecoverable) {
    harken::Spatializer spatial(harken::EngineConfig{}), wrong(harken::EngineConfig{});
    auto desc = Profile();
    const harken::Spatializer::PackProfileInput input{harkentest::ObjectId(17), &desc};
    Prepared prepared, empty;
    ASSERT_EQ(VN_OK, spatial.PreparePackProfiles(&input, 1, 1, &prepared));
#if defined(NDEBUG)
    wrong.CommitPreparedSet(&prepared);
    spatial.CommitPreparedSet(nullptr);
    spatial.CommitPreparedSet(&empty);
#else
    EXPECT_DEATH(wrong.CommitPreparedSet(&prepared), "");
    EXPECT_DEATH(spatial.CommitPreparedSet(nullptr), "");
    EXPECT_DEATH(spatial.CommitPreparedSet(&empty), "");
#endif
    EXPECT_TRUE(prepared.IsPrepared());
    EXPECT_EQ(0u, spatial.ProfileCount());
    EXPECT_EQ(VN_IN_USE, spatial.PreparePackProfiles(&input, 1, 1, &empty));
    spatial.CommitPreparedSet(&prepared);
    EXPECT_EQ(1u, spatial.ProfileCount());
    EXPECT_EQ(0u, wrong.ProfileCount());
}

TEST(CanonicalAttenuationGeneration, EarlyAndPartialFailuresReleasePendingAndRetains) {
    harken::EngineConfig config{};
    config.topologyPlanPoolCapacity = 1;
    harken::Spatializer spatial(config);
    for (std::size_t slot = 1; slot < 64; ++slot) {
        ASSERT_EQ(VN_OK, spatial.SetRetainEpochForTests(slot, UINT32_MAX));
    }
    auto desc = Profile();
    harken::Spatializer::PackProfileInput inputs[]{{harkentest::ObjectId(17), &desc}, {harkentest::ObjectId(18), &desc}};
    Prepared initial;
    ASSERT_EQ(VN_OK, spatial.PreparePackProfiles(inputs, 2, 1, &initial));
    spatial.CommitPreparedSet(&initial);
    desc.maxDistance = 20;
    Prepared refused;
    EXPECT_EQ(VN_PACK_RESIDENT_CAPACITY,
              spatial.PreparePackProfiles(inputs, 2, 2, &refused, {{harkentest::ObjectId(17), 1}, {harkentest::ObjectId(18), 1}}));
    EXPECT_FALSE(refused.IsPrepared());
    EXPECT_EQ(0u, spatial.RetainedProfileCount());
    EXPECT_FLOAT_EQ(10, spatial.FindProfile(harkentest::ObjectId(17), 1)->maxDistance);
    EXPECT_EQ(VN_OK, spatial.SetRetainEpochForTests(0, 1));
    for (std::size_t nth : {1u, 2u}) {
        VnResult result;
        {
            AllocationFaultScope fault(nth);
            result = spatial.PreparePackProfiles(inputs, 1, 2, &refused, {{harkentest::ObjectId(17), 1}});
        }
        EXPECT_EQ(VN_BACKPRESSURE, result);
        EXPECT_FALSE(refused.IsPrepared());
        EXPECT_EQ(0u, spatial.RetainedProfileCount());
    }
    ASSERT_EQ(VN_OK, spatial.PreparePackProfiles(inputs, 1, 2, &refused, {{harkentest::ObjectId(17), 1}}));
    spatial.CommitPreparedSet(&refused);
    EXPECT_NE(nullptr, spatial.FindProfile(harkentest::ObjectId(17), 2));
}

TEST(CanonicalAttenuationGeneration, DuplicateAndDirectIdsAndOldRevisionAreRejected) {
    harken::Spatializer spatial(harken::EngineConfig{});
    auto desc = Profile();
    ASSERT_EQ(VN_OK, spatial.RegisterProfile(harkentest::ObjectId(19), &desc));
    ASSERT_EQ(VN_OK, Install(spatial, desc, 2));
    harken::Spatializer::PackProfileInput duplicate[]{{harkentest::ObjectId(17), &desc}, {harkentest::ObjectId(17), &desc}};
    Prepared out;
    EXPECT_EQ(VN_BAD_PACK, spatial.PreparePackProfiles(duplicate, 2, 3, &out));
    const harken::Spatializer::PackProfileInput direct{harkentest::ObjectId(19), &desc};
    EXPECT_EQ(VN_IN_USE, spatial.PreparePackProfiles(&direct, 1, 3, &out));
    EXPECT_EQ(VN_REVISION_CONFLICT, Install(spatial, Profile(20), 1));
    EXPECT_FALSE(out.IsPrepared());
    EXPECT_FLOAT_EQ(10, spatial.FindProfile(harkentest::ObjectId(17), 2)->maxDistance);
    EXPECT_EQ(VN_OK, Install(spatial, Profile(20), 3));
}

TEST(CanonicalAttenuationGeneration, SameRetainSlotNewEpochRejectsOldEndAndStaleMirror) {
    harken::EngineConfig config{};
    config.topologyPlanPoolCapacity = 1;
    ASSERT_TRUE(harken::VoiceCapacityModel::TryCreate(2, 2, &config.voiceCapacity));
    harken::Spatializer spatial(config);
    for (std::size_t slot = 1; slot < 64; ++slot) {
        ASSERT_EQ(VN_OK, spatial.SetRetainEpochForTests(slot, UINT32_MAX));
    }
    const VnVoiceHandle oldVoice = UINT64_C(1) << 32, nextVoice = UINT64_C(2) << 32;
    ASSERT_EQ(VN_OK, Install(spatial, Profile(), 1));
    spatial.OnVoiceStarted(oldVoice, true, 5, 0, 0);
    ASSERT_EQ(VN_OK, spatial.BindVoice(oldVoice, harkentest::ObjectId(17), 1));
    ASSERT_EQ(VN_OK, Install(spatial, Profile(20), 2));
    spatial.NoteVoiceEnded(oldVoice);
    ASSERT_EQ(VN_OK, spatial.SweepRetainedProfiles());
    EXPECT_EQ(0u, spatial.RetainedProfileCount());
    spatial.OnVoiceStarted(nextVoice, true, 5, 0, 0);
    ASSERT_EQ(VN_OK, spatial.BindVoice(nextVoice, harkentest::ObjectId(17), 2));
    ASSERT_EQ(VN_OK, Install(spatial, Profile(30), 3));
    spatial.NoteVoiceEnded(oldVoice);
    harken::Spatializer::SpatialVoiceState captured{};
    ASSERT_TRUE(spatial.CaptureVoice(nextVoice, &captured));
    EXPECT_EQ(2u, captured.attenuationGeneration);
    EXPECT_EQ(VN_OK, spatial.SetVoiceRetainEpochForTests(nextVoice, 1));
    ASSERT_TRUE(spatial.CaptureVoice(nextVoice, &captured));
    EXPECT_EQ(harkentest::ObjectId(0u), captured.attenuationId);
    spatial.NoteVoiceEnded(nextVoice);
    ASSERT_EQ(VN_OK, spatial.SweepRetainedProfiles());
    EXPECT_EQ(1u, spatial.RetainedProfileCount());
}

TEST(CanonicalAttenuationGeneration, CommitHasNoAllocationAndProbeIsLive) {
    std::size_t positive = 0;
    {
        AllocationProbeScope probe;
        void* p = ::operator new(8);
        ::operator delete(p);
        positive = allocation_probe::count;
    }
    EXPECT_EQ(1u, positive);
    harken::EngineConfig config{};
    harken::Spatializer spatial(config);
    auto desc = Profile();
    const harken::Spatializer::PackProfileInput input{harkentest::ObjectId(17), &desc};
    Prepared prepared;
    ASSERT_EQ(VN_OK, spatial.PreparePackProfiles(&input, 1, 1, &prepared));
    std::size_t allocations = 0;
    {
        AllocationProbeScope probe;
        spatial.CommitPreparedSet(&prepared);
        allocations = allocation_probe::count;
    }
    EXPECT_EQ(0u, allocations);
    desc.maxDistance = 20;
    ASSERT_EQ(VN_OK, spatial.PreparePackProfiles(&input, 1, 2, &prepared, {{harkentest::ObjectId(17), 1}}));
    {
        AllocationProbeScope probe;
        spatial.CommitPreparedSet(&prepared);
        allocations = allocation_probe::count;
    }
    EXPECT_EQ(0u, allocations);
    EXPECT_EQ(1u, spatial.RetainedProfileCount());
}

TEST(CanonicalAttenuationGeneration, ConstructorFailureAndEngineInitUnwindCleanly) {
    harken::EngineConfig config{};
    {
        AllocationFaultScope fault(1);
        EXPECT_THROW({ harken::Spatializer failed(config); }, std::bad_alloc);
    }
    harken::Spatializer recovered(config);
    EXPECT_EQ(VN_OK, Install(recovered, Profile(), 1));
    struct Cleanup { ~Cleanup() { Vn_Term(); } } cleanup;
    VnConfig publicConfig{};
    publicConfig.structSize = sizeof(publicConfig);
    publicConfig.deviceMode = VN_DEVICE_NULL;
    VnResult result;
    {
        AllocationFaultScope fault(1);
        result = Vn_Init(&publicConfig);
    }
    EXPECT_EQ(VN_INVALID_CONFIG, result);
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_Update());
    ASSERT_EQ(VN_OK, Vn_Init(&publicConfig));
    EXPECT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(VN_OK, Vn_Term());
}

static_assert(noexcept(std::declval<const harken::Spatializer&>().FreeProfileSlotAtOrdinal(0)));

TEST(CanonicalAttenuationGeneration, FreeSlotOrdinalsPrepareDistinctTokensAcrossHoles) {
    harken::Spatializer spatial(harken::EngineConfig{});
    auto desc = Profile();
    ASSERT_EQ(VN_OK, spatial.RegisterProfile(harkentest::ObjectId(7000), &desc));
    ASSERT_EQ(VN_OK, Install(spatial, desc, 1));
    ASSERT_EQ(VN_OK, spatial.RegisterProfile(harkentest::ObjectId(7002), &desc));
    Prepared removed;
    ASSERT_EQ(VN_OK, spatial.PreparePackProfiles(nullptr, 0, 0, &removed));
    spatial.CommitPreparedSet(&removed);
    const auto* direct0 = spatial.FindProfile(harkentest::ObjectId(7000));
    const auto* direct2 = spatial.FindProfile(harkentest::ObjectId(7002));
    ASSERT_NE(nullptr, direct0);
    ASSERT_NE(nullptr, direct2);
    EXPECT_EQ(2, direct2 - direct0);
    EXPECT_EQ(2u, spatial.ProfileCount());
    EXPECT_EQ(1u, spatial.FreeProfileSlotAtOrdinal(0));
    EXPECT_EQ(3u, spatial.FreeProfileSlotAtOrdinal(1));
    EXPECT_EQ(4u, spatial.FreeProfileSlotAtOrdinal(2));
    EXPECT_EQ(harken::kMaxAttenuationProfiles, spatial.FreeProfileSlotAtOrdinal(UINT32_MAX));
    harken::Spatializer::PreparedProfileToken first, second;
    ASSERT_EQ(VN_OK, spatial.PrepareRegisterProfile(harkentest::ObjectId(8000), &desc,
        spatial.FreeProfileSlotAtOrdinal(0), &first));
    ASSERT_EQ(VN_OK, spatial.PrepareRegisterProfile(harkentest::ObjectId(8001), &desc,
        spatial.FreeProfileSlotAtOrdinal(1), &second));
    EXPECT_TRUE(first.IsPrepared());
    EXPECT_TRUE(second.IsPrepared());
    EXPECT_EQ(1u, first.SlotIndex());
    EXPECT_EQ(3u, second.SlotIndex());
    EXPECT_EQ(nullptr, spatial.FindProfile(harkentest::ObjectId(8000)));
    EXPECT_EQ(nullptr, spatial.FindProfile(harkentest::ObjectId(8001)));
    spatial.CommitPreparedProfile(&first);
    spatial.CommitPreparedProfile(&second);
    EXPECT_EQ(4u, spatial.ProfileCount());
    ASSERT_NE(nullptr, spatial.FindProfile(harkentest::ObjectId(8000)));
    ASSERT_NE(nullptr, spatial.FindProfile(harkentest::ObjectId(8001)));
    EXPECT_EQ(1, spatial.FindProfile(harkentest::ObjectId(8000)) - direct0);
    EXPECT_EQ(3, spatial.FindProfile(harkentest::ObjectId(8001)) - direct0);
    EXPECT_EQ(direct0, spatial.FindProfile(harkentest::ObjectId(7000)));
    EXPECT_EQ(direct2, spatial.FindProfile(harkentest::ObjectId(7002)));
    EXPECT_EQ(4u, spatial.FreeProfileSlotAtOrdinal(0));
}

TEST(CanonicalAttenuationGeneration, ExhaustedOrdinalCancelsEarlierPreparedTokenAndCanRetry) {
    harken::Spatializer spatial(harken::EngineConfig{});
    auto desc = Profile();
    for (std::uint64_t idValue = 1; idValue < harken::kMaxAttenuationProfiles; ++idValue) {
        const auto id = harkentest::ObjectId(idValue);
        ASSERT_EQ(VN_OK, spatial.RegisterProfile(id, &desc));
    }
    EXPECT_EQ(63u, spatial.FreeProfileSlotAtOrdinal(0));
    EXPECT_EQ(harken::kMaxAttenuationProfiles, spatial.FreeProfileSlotAtOrdinal(1));
    harken::Spatializer::PreparedProfileToken first, refused;
    ASSERT_EQ(VN_OK, spatial.PrepareRegisterProfile(harkentest::ObjectId(8000), &desc,
        spatial.FreeProfileSlotAtOrdinal(0), &first));
    EXPECT_EQ(VN_NO_ATTENUATION, spatial.PrepareRegisterProfile(harkentest::ObjectId(8001), &desc,
        spatial.FreeProfileSlotAtOrdinal(1), &refused));
    spatial.CancelPreparedProfile(&first);
    spatial.CancelPreparedProfile(&refused);
    EXPECT_FALSE(first.IsPrepared());
    EXPECT_FALSE(refused.IsPrepared());
    EXPECT_EQ(63u, spatial.ProfileCount());
    EXPECT_EQ(nullptr, spatial.FindProfile(harkentest::ObjectId(8000)));
    EXPECT_EQ(nullptr, spatial.FindProfile(harkentest::ObjectId(8001)));
    EXPECT_EQ(63u, spatial.FreeProfileSlotAtOrdinal(0));
    ASSERT_EQ(VN_OK, spatial.PrepareRegisterProfile(harkentest::ObjectId(8000), &desc,
        spatial.FreeProfileSlotAtOrdinal(0), &first));
    spatial.CommitPreparedProfile(&first);
    EXPECT_EQ(64u, spatial.ProfileCount());
    EXPECT_EQ(harken::kMaxAttenuationProfiles, spatial.FreeProfileSlotAtOrdinal(0));
    EXPECT_EQ(harken::kMaxAttenuationProfiles, spatial.FreeProfileSlotAtOrdinal(UINT32_MAX));
}

}
