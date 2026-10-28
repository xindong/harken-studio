#include "Common/ObjectIdTestSupport.h"
#include "harken/media/PackTable.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

#include "UnitTest/AllocationProbe.h"

namespace {

static_assert(std::is_nothrow_destructible_v<harken::PackTable::PreparedSetMutation>);

struct SetStorageMember {
    harken::PackTable::AlignedBuffer bytes;
    std::vector<harken::PackTocEntry> media;
    harken::PackRevisionKey key{};

    SetStorageMember(std::uint64_t packId, VnSoundId soundId, float value) {
        bytes.reset(static_cast<std::uint8_t*>(::operator new[](
            64, std::align_val_t{harken::kPackBlobAlign})));
        std::memset(bytes.get(), 0, 64);
        auto* samples = reinterpret_cast<float*>(bytes.get());
        std::fill(samples, samples + 8, value);
        harken::PackTocEntry row{};
        row.soundId = soundId;
        row.sampleRate = 48000;
        row.channelCount = 1;
        row.frameCount = 8;
        row.blobChecksum = harken::Fnv1a64(bytes.get(), 8 * sizeof(float));
        media.push_back(row);
        key.packStableId = packId;
        key.contentRevision = 1;
        key.partitionRevision = 1;
        key.resourceRevision = 1;
    }

    harken::PackTable::CanonicalSetMember Input(VnPackHandle handle = 0) const {
        return {handle, key, &bytes, &media};
    }
};

TEST(CanonicalPackSetStorage, DistinctHandlesAndTokensShareEqualMedia) {
    harken::PCMRegistry registry;
    harken::PackTable packs(2, 4);
    SetStorageMember a(21, harkentest::ObjectId(100), 0.25f), b(22, harkentest::ObjectId(100), 0.25f);
    harken::PackTable::PreparedSetMutation prepared;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input(), b.Input()}, {}, registry, &prepared));
    EXPECT_EQ(0u, packs.LoadedCount());
    EXPECT_EQ(0u, registry.CookieOf(harkentest::ObjectId(100)));
    EXPECT_NE(prepared.HandleAt(0), prepared.HandleAt(1));
    EXPECT_NE(prepared.ResourceTokenAt(0), prepared.ResourceTokenAt(1));
    packs.CommitPreparedSet(&prepared, registry);
    EXPECT_EQ(2u, packs.LoadedCount());
    const auto* first = packs.Entries(prepared.HandleAt(0));
    const auto* second = packs.Entries(prepared.HandleAt(1));
    ASSERT_NE(nullptr, first);
    ASSERT_NE(nullptr, second);
    ASSERT_EQ(1u, first->size());
    ASSERT_EQ(1u, second->size());
    EXPECT_NE(0u, first->front().registrationCookie);
    EXPECT_EQ(first->front().registrationCookie, second->front().registrationCookie);
    EXPECT_EQ(first->front().registration.View().channels[0],
              second->front().registration.View().channels[0]);
}

TEST(CanonicalPackSetStorage, KeptOwnerSurvivesSharedOwnerRemovalAndOldReaderReplacement) {
    harken::PCMRegistry registry;
    harken::PackTable packs(2, 4, 2);
    const auto sound = harkentest::ObjectId(100);
    SetStorageMember a(21, sound, 0.25f), b(22, sound, 0.25f);
    harken::PackTable::PreparedSetMutation first;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input(), b.Input()}, {}, registry, &first));
    packs.CommitPreparedSet(&first, registry);
    const auto handleA = first.HandleAt(0), handleB = first.HandleAt(1);
    const auto tokenA = first.ResourceTokenAt(0), tokenB = first.ResourceTokenAt(1);
    const auto cookie = registry.CookieOf(sound);
    harken::PackResourceClosureToken reader = 0;
    ASSERT_EQ(VN_OK, packs.AcquireResourceClosure(&tokenB, 1, &reader));

    harken::PackTable::CanonicalSetMember keep{handleA, a.key, nullptr, nullptr, true};
    harken::PackTable::PreparedSetMutation removal;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({keep}, {handleB}, registry, &removal));
    EXPECT_EQ(handleA, removal.HandleAt(0));
    EXPECT_EQ(tokenA, removal.ResourceTokenAt(0));
    packs.CommitPreparedSet(&removal, registry);
    EXPECT_EQ(1u, packs.LoadedCount());
    EXPECT_EQ(cookie, registry.CookieOf(sound));
    harken::PCMView old{};
    ASSERT_EQ(VN_OK, packs.LookupExactClosure(reader, sound, registry, &old));
    EXPECT_FLOAT_EQ(0.25f, old.channels[0][0]);

    SetStorageMember changed(21, sound, 0.75f);
    harken::PackTable::PreparedSetMutation replacement;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({changed.Input(handleA)}, {}, registry, &replacement));
    packs.CommitPreparedSet(&replacement, registry);
    EXPECT_NE(tokenA, replacement.ResourceTokenAt(0));
    EXPECT_FLOAT_EQ(0.75f, registry.Lookup(sound).channels[0][0]);
    packs.SweepRetiring(+[](void*, harken::PackResourceRevisionToken) { return false; });
    std::uint64_t checksum = 0;
    bool known = false;
    ASSERT_EQ(VN_OK, packs.LookupExactClosure(reader, sound, registry, &old, &checksum, &known));
    EXPECT_FLOAT_EQ(0.25f, old.channels[0][0]);
    EXPECT_TRUE(known);
    EXPECT_EQ(b.media.front().blobChecksum, checksum);
    harken::PCMView current{};
    ASSERT_EQ(VN_OK, packs.LookupExact(replacement.ResourceTokenAt(0), sound, registry, &current, &checksum, &known));
    EXPECT_TRUE(known);
    EXPECT_EQ(changed.media.front().blobChecksum, checksum);
    EXPECT_NE(b.media.front().blobChecksum, checksum);
    ASSERT_EQ(VN_OK, packs.ReleaseResourceClosure(reader));
    packs.SweepRetiring(+[](void*, harken::PackResourceRevisionToken) { return false; });
    EXPECT_EQ(0u, packs.RetiringCount());
}

TEST(CanonicalPackSetStorage, MixedKeepMutationsPreserveDistinctTokensAndOldReaders) {
    harken::PCMRegistry registry;
    harken::PackTable packs(3, 8, 4);
    const auto soundA = harkentest::ObjectId(100), soundB = harkentest::ObjectId(101);
    SetStorageMember init(20, harkentest::ObjectId(99), 0.0f);
    SetStorageMember a(21, soundA, 0.25f), b(22, soundB, 0.5f);
    harken::PackTable::PreparedSetMutation first;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({init.Input(), a.Input()}, {}, registry, &first));
    packs.CommitPreparedSet(&first, registry);
    const auto handleA = first.HandleAt(1);
    const auto tokenA = first.ResourceTokenAt(1);
    harken::PackTable::CanonicalSetMember keepInit{
        first.HandleAt(0), init.key, nullptr, nullptr, true};
    harken::PackTable::CanonicalSetMember keepA{handleA, a.key, nullptr, nullptr, true};

    harken::PackTable::PreparedSetMutation addition;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({keepInit, keepA, b.Input()}, {}, registry, &addition));
    packs.CommitPreparedSet(&addition, registry);
    const auto tokenB = addition.ResourceTokenAt(2);
    EXPECT_EQ(first.ResourceTokenAt(0), addition.ResourceTokenAt(0));
    EXPECT_EQ(tokenA, addition.ResourceTokenAt(1));
    EXPECT_NE(tokenA, tokenB);
    const harken::PackResourceRevisionToken oldTokens[]{tokenA, tokenB};
    harken::PackResourceClosureToken reader = 0;
    ASSERT_EQ(VN_OK, packs.AcquireResourceClosure(oldTokens, 2, &reader));

    SetStorageMember changedA(21, soundA, 0.75f);
    changedA.key.contentRevision = 2;
    harken::PackTable::CanonicalSetMember keepB{
        addition.HandleAt(2), b.key, nullptr, nullptr, true};
    harken::PackTable::PreparedSetMutation replacement;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet(
        {keepInit, changedA.Input(handleA), keepB}, {}, registry, &replacement));
    EXPECT_NE(tokenA, replacement.ResourceTokenAt(1));
    EXPECT_NE(tokenB, replacement.ResourceTokenAt(1));
    EXPECT_EQ(tokenB, replacement.ResourceTokenAt(2));
    packs.CommitPreparedSet(&replacement, registry);
    EXPECT_FLOAT_EQ(0.75f, registry.Lookup(soundA).channels[0][0]);
    packs.SweepRetiring(+[](void*, harken::PackResourceRevisionToken) { return false; });
    harken::PCMView oldA{}, oldB{};
    ASSERT_EQ(VN_OK, packs.LookupExactClosure(reader, soundA, registry, &oldA));
    ASSERT_EQ(VN_OK, packs.LookupExactClosure(reader, soundB, registry, &oldB));
    EXPECT_FLOAT_EQ(0.25f, oldA.channels[0][0]);
    EXPECT_FLOAT_EQ(0.5f, oldB.channels[0][0]);
    ASSERT_EQ(VN_OK, packs.ReleaseResourceClosure(reader));
    packs.SweepRetiring(+[](void*, harken::PackResourceRevisionToken) { return false; });
    EXPECT_EQ(0u, packs.RetiringCount());
}

TEST(CanonicalPackSetStorage, CancellationPreservesHandleTokenAndCookieFloors) {
    harken::PCMRegistry registry;
    harken::PackTable packs(2, 4);
    SetStorageMember a(21, harkentest::ObjectId(100), 0.25f), b(22, harkentest::ObjectId(101), 0.5f);
    VnPackHandle handle = 0;
    harken::PackResourceRevisionToken token = 0;
    {
        harken::PackTable::PreparedSetMutation cancelled;
        ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input(), b.Input()}, {}, registry, &cancelled));
        handle = cancelled.HandleAt(0);
        token = cancelled.ResourceTokenAt(0);
    }
    EXPECT_EQ(0u, packs.LoadedCount());
    EXPECT_EQ(0u, registry.CookieOf(harkentest::ObjectId(100)));
    harken::PackTable::PreparedSetMutation next;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input(), b.Input()}, {}, registry, &next));
    EXPECT_EQ(handle, next.HandleAt(0));
    EXPECT_EQ(token, next.ResourceTokenAt(0));
    packs.CommitPreparedSet(&next, registry);
    EXPECT_EQ(1u, registry.CookieOf(harkentest::ObjectId(100)));
    EXPECT_EQ(2u, registry.CookieOf(harkentest::ObjectId(101)));
}

TEST(CanonicalPackSetStorage, LaterSharedMismatchPublishesNothing) {
    harken::PCMRegistry registry;
    harken::PackTable packs(2, 4);
    SetStorageMember a(21, harkentest::ObjectId(100), 0.25f), b(22, harkentest::ObjectId(100), 0.5f);
    harken::PackTable::PreparedSetMutation refused;
    EXPECT_EQ(VN_IN_USE, packs.PrepareCanonicalSet({a.Input(), b.Input()}, {}, registry, &refused));
    EXPECT_FALSE(refused.IsPrepared());
    EXPECT_EQ(0u, packs.LoadedCount());
    EXPECT_EQ(0u, registry.CookieOf(harkentest::ObjectId(100)));
    EXPECT_EQ(harkentest::ObjectId(100u), packs.ConflictingSoundId());
    harken::PackTable::PreparedSetMutation retry;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input()}, {}, registry, &retry));
    EXPECT_EQ(1u, retry.ResourceTokenAt(0));
}

TEST(CanonicalPackSetStorage, SharedRevisionRequiresAllOwnersAndKeepsOldClosure) {
    harken::PCMRegistry registry;
    harken::PackTable packs(2, 4, 4);
    SetStorageMember a(21, harkentest::ObjectId(100), 0.25f), b(22, harkentest::ObjectId(100), 0.25f);
    harken::PackTable::PreparedSetMutation first;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input(), b.Input()}, {}, registry, &first));
    packs.CommitPreparedSet(&first, registry);
    const auto handleA = first.HandleAt(0), handleB = first.HandleAt(1);
    const auto tokenA = first.ResourceTokenAt(0), tokenB = first.ResourceTokenAt(1);
    const harken::PackResourceRevisionToken oldTokens[]{tokenA, tokenB};
    harken::PackResourceClosureToken closure = 0;
    ASSERT_EQ(VN_OK, packs.AcquireResourceClosure(oldTokens, 2, &closure));
    SetStorageMember changedA(21, harkentest::ObjectId(100), 0.75f), changedB(22, harkentest::ObjectId(100), 0.75f);
    changedA.key.contentRevision = changedB.key.contentRevision = 2;
    harken::PackTable::PreparedSetMutation next;
    packs.SetStorageCommitPageLimitsForTests(0, 0, 0);
    EXPECT_EQ(VN_BACKPRESSURE, packs.PrepareCanonicalSet(
        {changedA.Input(handleA), changedB.Input(handleB)}, {}, registry, &next));
    EXPECT_FALSE(next.IsPrepared());
    EXPECT_FLOAT_EQ(0.25f, registry.Lookup(harkentest::ObjectId(100)).channels[0][0]);
    packs.SetStorageCommitPageLimitsForTests(0, 0, SIZE_MAX);
    harken::PackTable::PreparedSetMutation partial;
    EXPECT_EQ(VN_IN_USE, packs.PrepareCanonicalSet({changedA.Input(handleA)}, {}, registry, &partial));
    EXPECT_FLOAT_EQ(0.25f, registry.Lookup(harkentest::ObjectId(100)).channels[0][0]);
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet(
        {changedA.Input(handleA), changedB.Input(handleB)}, {}, registry, &next));
    packs.SetStorageCommitPageLimitsForTests(0, 0, 0);
    EXPECT_EQ(handleA, next.HandleAt(0));
    EXPECT_EQ(handleB, next.HandleAt(1));
    EXPECT_FLOAT_EQ(0.25f, registry.Lookup(harkentest::ObjectId(100)).channels[0][0]);
    packs.CommitPreparedSet(&next, registry);
    EXPECT_FLOAT_EQ(0.75f, registry.Lookup(harkentest::ObjectId(100)).channels[0][0]);
    harken::PCMView old{};
    ASSERT_EQ(VN_OK, packs.LookupExactClosure(closure, harkentest::ObjectId(100), registry, &old));
    EXPECT_FLOAT_EQ(0.25f, old.channels[0][0]);
    EXPECT_EQ(2u, packs.RetiringCount());
    ASSERT_EQ(VN_OK, packs.ReleaseResourceClosure(closure));
    packs.SweepRetiring(+[](void*, harken::PackResourceRevisionToken) { return false; });
    EXPECT_EQ(0u, packs.RetiringCount());
}

TEST(CanonicalPackSetStorage, FullTableReusesRemovedSlotWithFreshHandle) {
    harken::PCMRegistry registry;
    harken::PackTable packs(2, 4);
    SetStorageMember a(21, harkentest::ObjectId(100), 0.25f), b(22, harkentest::ObjectId(101), 0.5f), c(23, harkentest::ObjectId(102), 0.75f);
    harken::PackTable::PreparedSetMutation first;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input(), b.Input()}, {}, registry, &first));
    packs.CommitPreparedSet(&first, registry);
    const auto handleA = first.HandleAt(0), handleB = first.HandleAt(1);
    harken::PackTable::PreparedSetMutation next;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet(
        {a.Input(handleA), c.Input()}, {handleB}, registry, &next));
    EXPECT_EQ(handleA, next.HandleAt(0));
    EXPECT_NE(handleB, next.HandleAt(1));
    EXPECT_EQ(static_cast<std::uint32_t>(handleB), static_cast<std::uint32_t>(next.HandleAt(1)));
    EXPECT_NE(nullptr, packs.Entries(handleB));
    packs.CommitPreparedSet(&next, registry);
    EXPECT_EQ(2u, packs.LoadedCount());
    EXPECT_EQ(nullptr, packs.Entries(handleB));
    EXPECT_EQ(0u, registry.CookieOf(harkentest::ObjectId(101)));
    EXPECT_NE(0u, registry.CookieOf(harkentest::ObjectId(102)));
}

TEST(CanonicalPackSetStorage, LateRetirementCapacityFailureCancelsAllReservations) {
    harken::PCMRegistry registry;
    harken::PackTable packs(2, 1);
    SetStorageMember a(21, harkentest::ObjectId(100), 0.25f), b(22, harkentest::ObjectId(101), 0.5f);
    harken::PackTable::PreparedSetMutation first;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input(), b.Input()}, {}, registry, &first));
    packs.CommitPreparedSet(&first, registry);
    const auto handleA = first.HandleAt(0), handleB = first.HandleAt(1);
    const auto cookie = registry.CookieOf(harkentest::ObjectId(100));
    harken::PackTable::PreparedSetMutation refused;
    EXPECT_EQ(VN_BACKPRESSURE, packs.PrepareCanonicalSet(
        {a.Input(handleA), b.Input(handleB)}, {}, registry, &refused));
    EXPECT_FALSE(refused.IsPrepared());
    EXPECT_EQ(2u, packs.LoadedCount());
    EXPECT_EQ(cookie, registry.CookieOf(harkentest::ObjectId(100)));
    harken::PackTable::PreparedSetMutation retry;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input(handleA)}, {}, registry, &retry));
    EXPECT_EQ(3u, retry.ResourceTokenAt(0));
}

TEST(CanonicalPackSetStorage, PcmAllocationFailureRestoresAllReservations) {
    harken::PCMRegistry registry;
    harken::PackTable packs(2, 2, 4);
    SetStorageMember a(21, harkentest::ObjectId(100), 0.25f), b(22, harkentest::ObjectId(100), 0.25f);
    harken::PackTable::PreparedSetMutation first;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input(), b.Input()}, {}, registry, &first));
    packs.CommitPreparedSet(&first, registry);
    const auto handleA = first.HandleAt(0), handleB = first.HandleAt(1);
    const auto cookie = registry.CookieOf(harkentest::ObjectId(100));
    const harken::PackResourceRevisionToken oldTokens[]{first.ResourceTokenAt(0), first.ResourceTokenAt(1)};
    harken::PackResourceClosureToken closure = 0;
    ASSERT_EQ(VN_OK, packs.AcquireResourceClosure(oldTokens, 2, &closure));
    SetStorageMember changedA(21, harkentest::ObjectId(100), 0.75f), changedB(22, harkentest::ObjectId(100), 0.75f);
    harken::PackTable::PreparedSetMutation refused;
    harken::SetPcmRegisterFaultForTests(harken::PcmRegisterFault::MapCommit);
    const auto result = packs.PrepareCanonicalSet(
        {changedA.Input(handleA), changedB.Input(handleB)}, {}, registry, &refused);
    harken::SetPcmRegisterFaultForTests(harken::PcmRegisterFault::None);
    EXPECT_EQ(VN_BACKPRESSURE, result);
    EXPECT_FALSE(refused.IsPrepared());
    EXPECT_EQ(0u, packs.RetiringCount());
    EXPECT_EQ(2u, packs.LoadedCount());
    EXPECT_EQ(cookie, registry.CookieOf(harkentest::ObjectId(100)));
    EXPECT_FLOAT_EQ(0.25f, registry.Lookup(harkentest::ObjectId(100)).channels[0][0]);
    harken::PackTable::PreparedSetMutation retry;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet(
        {changedA.Input(handleA), changedB.Input(handleB)}, {}, registry, &retry));
    EXPECT_EQ(3u, retry.ResourceTokenAt(0));
    EXPECT_EQ(4u, retry.ResourceTokenAt(1));
    packs.CommitPreparedSet(&retry, registry);
    EXPECT_EQ(cookie + 1, registry.CookieOf(harkentest::ObjectId(100)));
    EXPECT_EQ(packs.Entries(handleA)->front().registrationCookie,
              packs.Entries(handleB)->front().registrationCookie);
    EXPECT_EQ(2u, packs.RetiringCount());
    harken::PCMView old{};
    ASSERT_EQ(VN_OK, packs.LookupExactClosure(closure, harkentest::ObjectId(100), registry, &old));
    EXPECT_FLOAT_EQ(0.25f, old.channels[0][0]);
    ASSERT_EQ(VN_OK, packs.ReleaseResourceClosure(closure));
    packs.SweepRetiring(+[](void*, harken::PackResourceRevisionToken) { return false; });
    EXPECT_EQ(0u, packs.RetiringCount());
}

static_assert(noexcept(std::declval<harken::PackTable&>().CommitPreparedSet(
    static_cast<harken::PackTable::PreparedSetMutation*>(nullptr), std::declval<harken::PCMRegistry&>())));
static_assert(std::is_nothrow_move_constructible_v<harken::PackTable::PreparedSetMutation>);
static_assert(std::is_nothrow_move_assignable_v<harken::PackTable::PreparedSetMutation>);

TEST(CanonicalPackSetStorage, AllocationProbeDetectsNormalArrayAlignedAndNothrow) {
    allocation_probe::count = 0;
    allocation_probe::enabled = true;
    void* a = ::operator new(0);
    void* b = ::operator new[](8);
    void* c = ::operator new(64, std::align_val_t{64});
    void* d = ::operator new[](64, std::align_val_t{64}, std::nothrow);
    void* e = ::operator new(8, std::nothrow);
    allocation_probe::enabled = false;
    const auto count = allocation_probe::count;
    ::operator delete(a);
    ::operator delete[](b);
    ::operator delete(c, std::align_val_t{64});
    ::operator delete[](d, std::align_val_t{64}, std::nothrow);
    ::operator delete(e, std::nothrow);
    EXPECT_EQ(5u, count);
}

TEST(CanonicalPackSetStorage, CommitHasNoAllocationsWithReplacementAndSharedFanout) {
    harken::PCMRegistry registry;
    harken::PackTable packs(3, 6, 4);
    SetStorageMember a(21, harkentest::ObjectId(100), 0.25f), b(22, harkentest::ObjectId(100), 0.25f), c(23, harkentest::ObjectId(101), 0.5f);
    harken::PackTable::PreparedSetMutation first;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input(), b.Input(), c.Input()}, {}, registry, &first));
    allocation_probe::count = 0;
    allocation_probe::enabled = true;
    packs.CommitPreparedSet(&first, registry);
    allocation_probe::enabled = false;
    EXPECT_EQ(0u, allocation_probe::count);
    const auto handleA = first.HandleAt(0), handleB = first.HandleAt(1), handleC = first.HandleAt(2);
    harken::PackResourceClosureToken closure = 0;
    const auto oldToken = first.ResourceTokenAt(0);
    ASSERT_EQ(VN_OK, packs.AcquireResourceClosure(&oldToken, 1, &closure));
    SetStorageMember changedA(21, harkentest::ObjectId(100), 0.75f), changedB(22, harkentest::ObjectId(100), 0.75f);
    harken::PackTable::PreparedSetMutation next;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({changedA.Input(handleA), changedB.Input(handleB)}, {handleC}, registry, &next));
    allocation_probe::count = 0;
    allocation_probe::enabled = true;
    packs.CommitPreparedSet(&next, registry);
    allocation_probe::enabled = false;
    EXPECT_EQ(0u, allocation_probe::count);
    EXPECT_FALSE(next.IsPrepared());
    EXPECT_EQ(0u, registry.CookieOf(harkentest::ObjectId(101)));
    EXPECT_EQ(packs.Entries(handleA)->front().registrationCookie, packs.Entries(handleB)->front().registrationCookie);
    harken::PCMView old{};
    ASSERT_EQ(VN_OK, packs.LookupExactClosure(closure, harkentest::ObjectId(100), registry, &old));
    EXPECT_FLOAT_EQ(0.25f, old.channels[0][0]);
    EXPECT_EQ(VN_OK, packs.ReleaseResourceClosure(closure));
}

TEST(CanonicalPackSetStorage, LoopViewAndContentIdentityMatchDirectRegistration) {
    for (const auto end : {0u, 6u}) {
        harken::PCMRegistry registry, direct;
        harken::PackTable packs(2, 4);
        SetStorageMember a(21, harkentest::ObjectId(100), 0.25f), b(22, harkentest::ObjectId(100), 0.25f);
        a.media[0].flags = b.media[0].flags = harken::kPackFlagLoopable;
        a.media[0].loopStartFrame = b.media[0].loopStartFrame = end == 0 ? 0 : 2;
        a.media[0].loopEndFrame = b.media[0].loopEndFrame = end;
        harken::PackTable::PreparedSetMutation first;
        ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input(), b.Input()}, {}, registry, &first));
        packs.CommitPreparedSet(&first, registry);
        const auto view = registry.Lookup(harkentest::ObjectId(100));
        EXPECT_EQ(a.media[0].loopStartFrame, view.loop.startFrame);
        EXPECT_EQ(end, view.loop.endFrame);
        EXPECT_EQ(packs.Entries(first.HandleAt(0))->front().registrationCookie,
                  packs.Entries(first.HandleAt(1))->front().registrationCookie);
        const float* plane = reinterpret_cast<const float*>(a.bytes.get());
        VnPCMDesc desc{};
        desc.structSize = sizeof(desc);
        desc.sampleRate = 48000;
        desc.channels = 1;
        desc.layout = VN_PCM_LAYOUT_PLANAR;
        desc.policy = VN_PCM_POLICY_COPY;
        desc.frames = 8;
        desc.samplesPlanar = &plane;
        ASSERT_EQ(VN_OK, direct.RegisterPCM(harkentest::ObjectId(100), &desc, nullptr, view.loop));
        EXPECT_EQ(direct.Lookup(harkentest::ObjectId(100)).contentRevision, view.contentRevision);
        ASSERT_EQ(VN_OK, direct.RegisterPCM(harkentest::ObjectId(101), &desc, nullptr, harken::PcmLoopRegion{1, 7}));
        EXPECT_NE(direct.Lookup(harkentest::ObjectId(101)).contentRevision, view.contentRevision);
    }
}

TEST(CanonicalPackSetStorage, DefaultAndNonLoopableInputsHaveEmptyLoop) {
    harken::PCMRegistry registry;
    harken::PackTable packs(1, 2);
    SetStorageMember a(21, harkentest::ObjectId(100), 0.25f);
    a.media[0].loopStartFrame = 2;
    a.media[0].loopEndFrame = 6;
    harken::PackTable::PreparedSetMutation first;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input()}, {}, registry, &first));
    packs.CommitPreparedSet(&first, registry);
    EXPECT_EQ(0u, registry.Lookup(harkentest::ObjectId(100)).loop.startFrame);
    EXPECT_EQ(0u, registry.Lookup(harkentest::ObjectId(100)).loop.endFrame);
    harken::PCMRegistry::PackReferenceRegistration defaulted{};
    EXPECT_EQ(0u, defaulted.loop.startFrame);
    EXPECT_EQ(0u, defaulted.loop.endFrame);
}

TEST(CanonicalPackSetStorage, MetadataMismatchPublishesNothing) {
    for (int field = 0; field < 6; ++field) {
        harken::PCMRegistry registry;
        harken::PackTable packs(2, 4);
        SetStorageMember a(21, harkentest::ObjectId(100), 0.25f), b(22, harkentest::ObjectId(100), 0.25f);
        switch (field) {
        case 0: b.media[0].sampleRate = 44100; break;
        case 1: b.media[0].frameCount = 4; break;
        case 2: b.media[0].channelCount = 2; b.media[0].frameCount = 4; break;
        case 3: b.media[0].flags = harken::kPackFlagLoopable; break;
        case 4: a.media[0].flags = b.media[0].flags = harken::kPackFlagLoopable;
                b.media[0].loopStartFrame = 2; b.media[0].loopEndFrame = a.media[0].loopEndFrame = 6; break;
        case 5: a.media[0].flags = b.media[0].flags = harken::kPackFlagLoopable;
                a.media[0].loopEndFrame = 6; b.media[0].loopEndFrame = 7; break;
        }
        harken::PackTable::PreparedSetMutation refused;
        EXPECT_EQ(VN_IN_USE, packs.PrepareCanonicalSet({a.Input(), b.Input()}, {}, registry, &refused));
        EXPECT_FALSE(refused.IsPrepared());
        EXPECT_EQ(0u, packs.LoadedCount());
        EXPECT_EQ(0u, registry.CookieOf(harkentest::ObjectId(100)));
    }
}

TEST(CanonicalPackSetStorage, LoopOnlyReplacementChangesIdentityAndKeepsOldClosure) {
    harken::PCMRegistry registry;
    harken::PackTable packs(1, 2, 2);
    SetStorageMember a(21, harkentest::ObjectId(100), 0.25f);
    a.media[0].flags = harken::kPackFlagLoopable;
    a.media[0].loopEndFrame = 6;
    harken::PackTable::PreparedSetMutation first;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input()}, {}, registry, &first));
    packs.CommitPreparedSet(&first, registry);
    const auto old = registry.Lookup(harkentest::ObjectId(100));
    const auto token = first.ResourceTokenAt(0);
    harken::PackResourceClosureToken closure = 0;
    ASSERT_EQ(VN_OK, packs.AcquireResourceClosure(&token, 1, &closure));
    a.media[0].loopEndFrame = 7;
    harken::PackTable::PreparedSetMutation next;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input(first.HandleAt(0))}, {}, registry, &next));
    packs.CommitPreparedSet(&next, registry);
    EXPECT_EQ(7u, registry.Lookup(harkentest::ObjectId(100)).loop.endFrame);
    EXPECT_NE(old.contentRevision, registry.Lookup(harkentest::ObjectId(100)).contentRevision);
    harken::PCMView pinned{};
    ASSERT_EQ(VN_OK, packs.LookupExactClosure(closure, harkentest::ObjectId(100), registry, &pinned));
    EXPECT_EQ(6u, pinned.loop.endFrame);
    EXPECT_EQ(old.contentRevision, pinned.contentRevision);
    EXPECT_EQ(VN_OK, packs.ReleaseResourceClosure(closure));
}

TEST(CanonicalPackSetStorage, MoveAndExplicitCancelReleaseReservations) {
    harken::PCMRegistry registry, otherRegistry;
    harken::PackTable packs(1, 1), otherPacks(1, 1);
    SetStorageMember a(21, harkentest::ObjectId(100), 0.25f);
    harken::PackTable::PreparedSetMutation first;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input()}, {}, registry, &first));
    packs.CommitPreparedSet(&first, registry);
    harken::PackTable::PreparedSetMutation source, target;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input(first.HandleAt(0))}, {}, registry, &source));
    ASSERT_EQ(VN_OK, otherPacks.PrepareCanonicalSet({a.Input()}, {}, otherRegistry, &target));
    const auto token = source.ResourceTokenAt(0);
    target = std::move(source);
    EXPECT_FALSE(source.IsPrepared());
    harken::PackTable::PreparedSetMutation moved(std::move(target));
    EXPECT_FALSE(target.IsPrepared());
    packs.CancelPreparedSet(&moved);
    EXPECT_FALSE(moved.IsPrepared());
    packs.CancelPreparedSet(&moved);
    harken::PackTable::PreparedSetMutation retry;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input(first.HandleAt(0))}, {}, registry, &retry));
    EXPECT_EQ(token, retry.ResourceTokenAt(0));
    packs.CommitPreparedSet(&retry, registry);
    harken::PackTable::PreparedSetMutation otherRetry;
    ASSERT_EQ(VN_OK, otherPacks.PrepareCanonicalSet({a.Input()}, {}, otherRegistry, &otherRetry));
    EXPECT_EQ(1u, otherRetry.ResourceTokenAt(0));
}

TEST(CanonicalPackSetStorage, RemovalOnlyPreservesSharedMediaUntilLastOwner) {
    harken::PCMRegistry registry;
    harken::PackTable packs(2, 4, 2);
    SetStorageMember a(21, harkentest::ObjectId(100), 0.25f), b(22, harkentest::ObjectId(100), 0.25f);
    harken::PackTable::PreparedSetMutation first;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input(), b.Input()}, {}, registry, &first));
    packs.CommitPreparedSet(&first, registry);
    const auto cookie = registry.CookieOf(harkentest::ObjectId(100));
    const auto token = first.ResourceTokenAt(0);
    harken::PackResourceClosureToken closure = 0;
    ASSERT_EQ(VN_OK, packs.AcquireResourceClosure(&token, 1, &closure));
    harken::PackTable::PreparedSetMutation removeA;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({}, {first.HandleAt(0)}, registry, &removeA));
    packs.CommitPreparedSet(&removeA, registry);
    EXPECT_EQ(cookie, registry.CookieOf(harkentest::ObjectId(100)));
    harken::PackTable::PreparedSetMutation removeB;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({}, {first.HandleAt(1)}, registry, &removeB));
    packs.CommitPreparedSet(&removeB, registry);
    EXPECT_EQ(0u, registry.CookieOf(harkentest::ObjectId(100)));
    EXPECT_EQ(0u, packs.LoadedCount());
    harken::PCMView old{};
    ASSERT_EQ(VN_OK, packs.LookupExactClosure(closure, harkentest::ObjectId(100), registry, &old));
    EXPECT_FLOAT_EQ(0.25f, old.channels[0][0]);
    EXPECT_EQ(VN_OK, packs.ReleaseResourceClosure(closure));
    packs.SweepRetiring(+[](void*, harken::PackResourceRevisionToken) { return false; });
    EXPECT_EQ(0u, packs.RetiringCount());
}

TEST(CanonicalPackSetStorage, SampleBackingFailurePreservesPublishedGeneration) {
    harken::PCMRegistry registry;
    harken::PackTable packs(1, 1);
    SetStorageMember a(21, harkentest::ObjectId(100), 0.25f), b(21, harkentest::ObjectId(100), 0.75f);
    harken::PackTable::PreparedSetMutation first;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input()}, {}, registry, &first));
    packs.CommitPreparedSet(&first, registry);
    const auto cookie = registry.CookieOf(harkentest::ObjectId(100));
    harken::PackTable::PreparedSetMutation refused;
    harken::SetPcmRegisterFaultForTests(harken::PcmRegisterFault::SampleBacking);
    const auto result = packs.PrepareCanonicalSet({b.Input(first.HandleAt(0))}, {}, registry, &refused);
    harken::SetPcmRegisterFaultForTests(harken::PcmRegisterFault::None);
    EXPECT_EQ(VN_BACKPRESSURE, result);
    EXPECT_FALSE(refused.IsPrepared());
    EXPECT_EQ(cookie, registry.CookieOf(harkentest::ObjectId(100)));
    EXPECT_FLOAT_EQ(0.25f, registry.Lookup(harkentest::ObjectId(100)).channels[0][0]);
    harken::PackTable::PreparedSetMutation retry;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({b.Input(first.HandleAt(0))}, {}, registry, &retry));
    EXPECT_EQ(2u, retry.ResourceTokenAt(0));
    packs.CommitPreparedSet(&retry, registry);
    EXPECT_FLOAT_EQ(0.75f, registry.Lookup(harkentest::ObjectId(100)).channels[0][0]);
}

TEST(CanonicalPackSetStorage, DuplicateIdentityAndInvalidHandlesDoNotPublish) {
    harken::PCMRegistry registry;
    harken::PackTable packs(2, 4);
    SetStorageMember a(21, harkentest::ObjectId(100), 0.25f), duplicate(21, harkentest::ObjectId(101), 0.5f), other(22, harkentest::ObjectId(101), 0.5f);
    harken::PackTable::PreparedSetMutation refused;
    EXPECT_EQ(VN_INVALID_ARG, packs.PrepareCanonicalSet({a.Input(), duplicate.Input()}, {}, registry, &refused));
    harken::PackTable::PreparedSetMutation first;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input()}, {}, registry, &first));
    EXPECT_EQ(1u, first.ResourceTokenAt(0));
    packs.CommitPreparedSet(&first, registry);
    const auto handle = first.HandleAt(0);
    EXPECT_EQ(VN_INVALID_ARG, packs.PrepareCanonicalSet({duplicate.Input()}, {}, registry, &refused));
    EXPECT_EQ(VN_INVALID_ARG, packs.PrepareCanonicalSet({a.Input(handle), a.Input(handle)}, {}, registry, &refused));
    EXPECT_EQ(VN_INVALID_ARG, packs.PrepareCanonicalSet({other.Input(handle)}, {}, registry, &refused));
    EXPECT_EQ(VN_INVALID_ARG, packs.PrepareCanonicalSet({}, {handle, handle}, registry, &refused));
    EXPECT_EQ(VN_INVALID_ARG, packs.PrepareCanonicalSet({a.Input(handle)}, {handle}, registry, &refused));
    EXPECT_EQ(VN_STALE_HANDLE, packs.PrepareCanonicalSet({}, {handle + (UINT64_C(1) << 32)}, registry, &refused));
    EXPECT_EQ(1u, packs.LoadedCount());
    EXPECT_EQ(1u, registry.CookieOf(harkentest::ObjectId(100)));
    harken::PackTable::PreparedSetMutation replaceIdentity;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({duplicate.Input()}, {handle}, registry, &replaceIdentity));
    EXPECT_EQ(2u, replaceIdentity.ResourceTokenAt(0));
    EXPECT_NE(handle, replaceIdentity.HandleAt(0));
    packs.CommitPreparedSet(&replaceIdentity, registry);
    EXPECT_EQ(nullptr, packs.Entries(handle));
    EXPECT_EQ(0u, registry.CookieOf(harkentest::ObjectId(100)));
    EXPECT_NE(0u, registry.CookieOf(harkentest::ObjectId(101)));
}

TEST(CanonicalPackSetStorage, RetainedTokenListIsValueOnly) {
    harken::PCMRegistry registry;
    harken::PackTable packs(1, 2);
    SetStorageMember a(21, harkentest::ObjectId(100), 0.25f);
    harken::PackTable::PreparedSetMutation first;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input()}, {}, registry, &first));
    packs.CommitPreparedSet(&first, registry);
    harken::PackTable::PreparedSetMutation next;
    std::vector<harken::PackResourceRevisionToken> retained{0, first.ResourceTokenAt(0), first.ResourceTokenAt(0), 999};
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input(first.HandleAt(0))}, {}, registry, &next, retained));
    retained.clear();
    retained.shrink_to_fit();
    packs.CommitPreparedSet(&next, registry);
    EXPECT_EQ(1u, packs.RetiringCount());
    packs.SweepRetiring(+[](void*, harken::PackResourceRevisionToken) { return false; });
    EXPECT_EQ(0u, packs.RetiringCount());
    harken::PackTable::PreparedSetMutation last;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({}, {next.HandleAt(0)}, registry, &last, {0, 999}));
    packs.CommitPreparedSet(&last, registry);
    EXPECT_EQ(0u, packs.RetiringCount());
}

TEST(CanonicalPackSetStorage, InvalidCommitCannotMutateOwnersAndRemainsCancelable) {
    harken::PCMRegistry registry, wrongRegistry;
    harken::PackTable packs(1, 1), wrongOwner(1, 1);
    SetStorageMember a(21, harkentest::ObjectId(100), 0.25f);
    harken::PackTable::PreparedSetMutation first;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input()}, {}, registry, &first));
    packs.CommitPreparedSet(&first, registry);
    harken::PackTable::PreparedSetMutation next, empty;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input(first.HandleAt(0))}, {}, registry, &next));
#if defined(NDEBUG)
    packs.CommitPreparedSet(&next, wrongRegistry);
    wrongOwner.CommitPreparedSet(&next, registry);
    packs.CommitPreparedSet(&empty, registry);
    packs.CommitPreparedSet(nullptr, registry);
#else
    EXPECT_DEATH(packs.CommitPreparedSet(&next, wrongRegistry), "");
    EXPECT_DEATH(wrongOwner.CommitPreparedSet(&next, registry), "");
    EXPECT_DEATH(packs.CommitPreparedSet(&empty, registry), "");
    EXPECT_DEATH(packs.CommitPreparedSet(nullptr, registry), "");
#endif
    EXPECT_TRUE(next.IsPrepared());
    EXPECT_EQ(1u, packs.LoadedCount());
    EXPECT_EQ(0u, wrongOwner.LoadedCount());
    EXPECT_EQ(0u, wrongRegistry.CookieOf(harkentest::ObjectId(100)));
    EXPECT_EQ(1u, registry.CookieOf(harkentest::ObjectId(100)));
    packs.CancelPreparedSet(&next);
    harken::PackTable::PreparedSetMutation retry;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input(first.HandleAt(0))}, {}, registry, &retry));
    EXPECT_EQ(2u, retry.ResourceTokenAt(0));
    packs.CommitPreparedSet(&retry, registry);
}

TEST(CanonicalPackSetStorage, LegacyEmptyLoopRegistrationIsNotReusedForAuthoredLoop) {
    harken::PCMRegistry registry;
    harken::PackTable packs(2, 4);
    SetStorageMember legacy(21, harkentest::ObjectId(100), 0.25f), current(21, harkentest::ObjectId(100), 0.25f), shared(22, harkentest::ObjectId(100), 0.25f);
    for (auto* member : {&legacy, &current, &shared}) {
        member->media[0].flags = harken::kPackFlagLoopable;
        member->media[0].loopStartFrame = 2;
        member->media[0].loopEndFrame = 6;
    }
    std::vector<harken::PackEntry> entries(1);
    entries[0].soundId = harkentest::ObjectId(100);
    entries[0].flags = harken::kPackFlagLoopable;
    entries[0].frameCount = 8;
    entries[0].loopStartFrame = 2;
    entries[0].loopEndFrame = 6;
    harken::PackTable::PreparedLoadToken load;
    ASSERT_EQ(VN_OK, packs.PrepareLoad(&load));
    VnPackHandle handle = 0;
    ASSERT_EQ(VN_OK, packs.LoadPrepared(&load, legacy.bytes, std::move(entries), legacy.media,
                                      registry, &handle, nullptr));
    const auto oldCookie = registry.CookieOf(harkentest::ObjectId(100));
    const auto oldRevision = registry.Lookup(harkentest::ObjectId(100)).contentRevision;
    EXPECT_EQ(0u, registry.Lookup(harkentest::ObjectId(100)).loop.startFrame);
    EXPECT_EQ(0u, registry.Lookup(harkentest::ObjectId(100)).loop.endFrame);
    harken::PackTable::PreparedSetMutation refused;
    EXPECT_EQ(VN_IN_USE, packs.PrepareCanonicalSet({shared.Input()}, {}, registry, &refused));
    EXPECT_EQ(oldCookie, registry.CookieOf(harkentest::ObjectId(100)));
    ASSERT_EQ(VN_OK, packs.Unload(handle, registry));
    harken::PackTable::PreparedSetMutation next;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet(
        {current.Input(), shared.Input()}, {}, registry, &next));
    packs.CommitPreparedSet(&next, registry);
    EXPECT_NE(oldCookie, registry.CookieOf(harkentest::ObjectId(100)));
    EXPECT_NE(oldRevision, registry.Lookup(harkentest::ObjectId(100)).contentRevision);
    EXPECT_EQ(2u, registry.Lookup(harkentest::ObjectId(100)).loop.startFrame);
    EXPECT_EQ(6u, registry.Lookup(harkentest::ObjectId(100)).loop.endFrame);
}

TEST(CanonicalPackSetStorage, FullTableAndInvalidArgumentsKeepCurrentGeneration) {
    harken::PCMRegistry registry;
    harken::PackTable packs(1, 1);
    SetStorageMember a(21, harkentest::ObjectId(100), 0.25f), b(22, harkentest::ObjectId(101), 0.5f);
    harken::PackTable::PreparedSetMutation first;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input()}, {}, registry, &first));
    packs.CommitPreparedSet(&first, registry);
    harken::PackTable::PreparedSetMutation refused;
    EXPECT_EQ(VN_NO_PACK, packs.PrepareCanonicalSet({b.Input()}, {}, registry, &refused));
    EXPECT_EQ(VN_INVALID_ARG, packs.PrepareCanonicalSet({b.Input()}, {}, registry, nullptr));
    auto bad = b.Input();
    bad.media = nullptr;
    EXPECT_EQ(VN_INVALID_ARG, packs.PrepareCanonicalSet({bad}, {}, registry, &refused));
    bad = b.Input();
    bad.revisionKey.packStableId = 0;
    EXPECT_EQ(VN_INVALID_ARG, packs.PrepareCanonicalSet({bad}, {}, registry, &refused));
    EXPECT_FALSE(refused.IsPrepared());
    EXPECT_EQ(1u, packs.LoadedCount());
    EXPECT_EQ(1u, registry.CookieOf(harkentest::ObjectId(100)));
    EXPECT_EQ(0u, registry.CookieOf(harkentest::ObjectId(101)));
}

TEST(CanonicalPackSetStorage, OverlappingPairPreparePreservesFirstTransaction) {
    harken::PCMRegistry registry;
    harken::PackTable packs(2, 4);
    SetStorageMember a(21, harkentest::ObjectId(100), 0.25f), b(22, harkentest::ObjectId(101), 0.5f);
    harken::PackTable::PreparedSetMutation first, refused;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input()}, {}, registry, &first));
    const auto handle = first.HandleAt(0);
    const auto token = first.ResourceTokenAt(0);
    ASSERT_EQ(VN_IN_USE, packs.PrepareCanonicalSet({b.Input()}, {}, registry, &refused));
    EXPECT_FALSE(refused.IsPrepared());
    EXPECT_TRUE(first.IsPrepared());
    EXPECT_EQ(handle, first.HandleAt(0));
    EXPECT_EQ(token, first.ResourceTokenAt(0));
    EXPECT_EQ(0u, registry.CookieOf(harkentest::ObjectId(100)));
    ASSERT_EQ(VN_IN_USE, packs.PrepareCanonicalSet({b.Input()}, {}, registry, &first));
    packs.CommitPreparedSet(&first, registry);
    EXPECT_NE(nullptr, packs.Entries(handle));
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({b.Input()}, {}, registry, &refused));
    EXPECT_NE(handle, refused.HandleAt(0));
    EXPECT_EQ(token + 1, refused.ResourceTokenAt(0));
    packs.CommitPreparedSet(&refused, registry);
    EXPECT_EQ(2u, packs.LoadedCount());
    EXPECT_EQ(1u, registry.CookieOf(harkentest::ObjectId(100)));
    EXPECT_EQ(2u, registry.CookieOf(harkentest::ObjectId(101)));
}

TEST(CanonicalPackSetStorage, SameTableBlocksOtherRegistryUntilCancellation) {
    harken::PCMRegistry registry, otherRegistry;
    harken::PackTable packs(2, 4);
    SetStorageMember a(21, harkentest::ObjectId(100), 0.25f);
    harken::PackTable::PreparedSetMutation first, refused;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input()}, {}, registry, &first));
    ASSERT_EQ(VN_IN_USE, packs.PrepareCanonicalSet({a.Input()}, {}, otherRegistry, &refused));
    EXPECT_TRUE(first.IsPrepared());
    EXPECT_EQ(0u, registry.Count());
    EXPECT_EQ(0u, otherRegistry.Count());
    packs.CancelPreparedSet(&first);
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input()}, {}, otherRegistry, &refused));
    packs.CommitPreparedSet(&refused, otherRegistry);
    EXPECT_EQ(1u, otherRegistry.CookieOf(harkentest::ObjectId(100)));
    EXPECT_EQ(0u, registry.CookieOf(harkentest::ObjectId(100)));
}

TEST(CanonicalPackSetStorage, SameRegistryBlocksOtherTableUntilCommit) {
    harken::PCMRegistry registry;
    harken::PackTable packs(1, 2), otherPacks(1, 2);
    SetStorageMember a(21, harkentest::ObjectId(100), 0.25f), b(22, harkentest::ObjectId(101), 0.5f);
    harken::PackTable::PreparedSetMutation first, refused;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input()}, {}, registry, &first));
    ASSERT_EQ(VN_IN_USE, otherPacks.PrepareCanonicalSet({b.Input()}, {}, registry, &refused));
    EXPECT_TRUE(first.IsPrepared());
    EXPECT_EQ(0u, otherPacks.LoadedCount());
    packs.CommitPreparedSet(&first, registry);
    ASSERT_EQ(VN_OK, otherPacks.PrepareCanonicalSet({b.Input()}, {}, registry, &refused));
    otherPacks.CommitPreparedSet(&refused, registry);
    EXPECT_EQ(1u, registry.CookieOf(harkentest::ObjectId(100)));
    EXPECT_EQ(2u, registry.CookieOf(harkentest::ObjectId(101)));
    EXPECT_EQ(1u, packs.LoadedCount());
    EXPECT_EQ(1u, otherPacks.LoadedCount());
}

TEST(CanonicalPackSetStorage, ForeignLiveOutputDoesNotReserveOrCancelEitherPair) {
    harken::PCMRegistry registry, otherRegistry;
    harken::PackTable packs(1, 2), otherPacks(1, 2);
    SetStorageMember a(21, harkentest::ObjectId(100), 0.25f);
    harken::PackTable::PreparedSetMutation first, independent;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input()}, {}, registry, &first));
    ASSERT_EQ(VN_IN_USE, otherPacks.PrepareCanonicalSet({a.Input()}, {}, otherRegistry, &first));
    EXPECT_TRUE(first.IsPrepared());
    ASSERT_EQ(VN_OK, otherPacks.PrepareCanonicalSet({a.Input()}, {}, otherRegistry, &independent));
    packs.CommitPreparedSet(&first, registry);
    otherPacks.CommitPreparedSet(&independent, otherRegistry);
    EXPECT_EQ(1u, registry.CookieOf(harkentest::ObjectId(100)));
    EXPECT_EQ(1u, otherRegistry.CookieOf(harkentest::ObjectId(100)));
}

TEST(CanonicalPackSetStorage, MovesKeepSourceReservationAndReleaseOldTargetPair) {
    harken::PCMRegistry registry, otherRegistry;
    harken::PackTable packs(1, 2), otherPacks(1, 2);
    SetStorageMember a(21, harkentest::ObjectId(100), 0.25f);
    {
        harken::PackTable::PreparedSetMutation source, target;
        ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input()}, {}, registry, &source));
        ASSERT_EQ(VN_OK, otherPacks.PrepareCanonicalSet({a.Input()}, {}, otherRegistry, &target));
        target = std::move(source);
        harken::PackTable::PreparedSetMutation refused, retry;
        ASSERT_EQ(VN_IN_USE, packs.PrepareCanonicalSet({a.Input()}, {}, registry, &refused));
        ASSERT_EQ(VN_OK, otherPacks.PrepareCanonicalSet({a.Input()}, {}, otherRegistry, &retry));
        harken::PackTable::PreparedSetMutation moved(std::move(target));
        ASSERT_EQ(VN_IN_USE, packs.PrepareCanonicalSet({a.Input()}, {}, registry, &refused));
        harken::PackTable::PreparedSetMutation empty;
        retry = std::move(empty);
        ASSERT_EQ(VN_OK, otherPacks.PrepareCanonicalSet({a.Input()}, {}, otherRegistry, &retry));
    }
    harken::PackTable::PreparedSetMutation retry;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input()}, {}, registry, &retry));
    EXPECT_EQ(1u, retry.ResourceTokenAt(0));
    packs.CommitPreparedSet(&retry, registry);
}

TEST(CanonicalPackSetStorage, InvalidCommitKeepsPendingReservationUntilCorrectCommit) {
    harken::PCMRegistry registry, wrongRegistry;
    harken::PackTable packs(1, 2);
    SetStorageMember a(21, harkentest::ObjectId(100), 0.25f);
    harken::PackTable::PreparedSetMutation first, refused;
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({a.Input()}, {}, registry, &first));
#if defined(NDEBUG)
    packs.CommitPreparedSet(&first, wrongRegistry);
#else
    EXPECT_DEATH(packs.CommitPreparedSet(&first, wrongRegistry), "");
#endif
    ASSERT_EQ(VN_IN_USE, packs.PrepareCanonicalSet({a.Input()}, {}, registry, &refused));
    EXPECT_TRUE(first.IsPrepared());
    packs.CommitPreparedSet(&first, registry);
    EXPECT_EQ(1u, packs.LoadedCount());
    ASSERT_EQ(VN_OK, packs.PrepareCanonicalSet({}, {first.HandleAt(0)}, registry, &refused));
    packs.CommitPreparedSet(&refused, registry);
    EXPECT_EQ(0u, packs.LoadedCount());
}

}
